/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2021-2022 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "binder_log.h"
#include "binder_data.h"
#include "binder_radio_caps.h"
#include "binder_radio.h"
#include "binder_sim_card.h"
#include "binder_sim_settings.h"
#include "binder_util.h"

#include <ofono/watch.h>
#include <ofono/radio-settings.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_idlepool.h>
#include <gutil_misc.h>

#define SET_CAPS_TIMEOUT_MS (30*1000)
#define GET_CAPS_TIMEOUT_MS (5*1000)
#define DATA_OFF_TIMEOUT_MS (10*1000)
#define DEACTIVATE_TIMEOUT_MS (10*1000)
#define CHECK_LATER_TIMEOUT_SEC (5)

#define GET_CAPS_RETRIES 60

/*
 * This code is doing something similar to what
 * com.android.internal.telephony.ProxyController
 * is doing.
 */

enum binder_radio_caps_watch_events {
    WATCH_EVENT_IMSI,
    WATCH_EVENT_MODEM,
    WATCH_EVENT_COUNT
};

enum binder_radio_caps_sim_events {
    SIM_EVENT_STATE_CHANGED,
    SIM_EVENT_IO_ACTIVE_CHANGED,
    SIM_EVENT_COUNT
};

enum binder_radio_caps_settings_events {
    SETTINGS_EVENT_PREF_MODE,
    SETTINGS_EVENT_COUNT
};

enum binder_radio_caps_client_events {
    CLIENT_EVENT_IND_RADIO_CAPABILITY,
    CLIENT_EVENT_OWNER,
    CLIENT_EVENT_COUNT
};

enum binder_radio_events {
    RADIO_EVENT_STATE,
    RADIO_EVENT_ONLINE,
    RADIO_EVENT_COUNT
};

typedef struct binder_radio_caps_object {
    GObject object;
    struct binder_radio_caps pub;
    enum ofono_radio_access_mode requested_modes;
    guint slot;
    char* log_prefix;
    RadioClient* client;
    RadioRequestGroup* g;
    GUtilIdlePool* idle_pool;
    gulong watch_event_id[WATCH_EVENT_COUNT];
    gulong settings_event_id[SETTINGS_EVENT_COUNT];
    gulong simcard_event_id[SIM_EVENT_COUNT];
    gulong client_event_id[CLIENT_EVENT_COUNT];
    gulong radio_event_id[RADIO_EVENT_COUNT];
    int tx_id;
    int tx_pending;
    struct ofono_watch* watch;
    BinderData* data;
    BinderRadio* radio;
    BinderSimSettings* settings;
    BinderSimCard* simcard;
    RadioCapability* cap;
    RadioCapability* old_cap;
    RadioCapability* new_cap;
} BinderRadioCapsObject;

typedef struct binder_radio_caps_manager {
    GObject object;
    GUtilIdlePool* idle_pool;
    GPtrArray* caps_list;
    GPtrArray* order_list;
    GPtrArray* requests;
    guint check_id;
    int tx_id;
    int tx_phase_index;
    gboolean tx_failed;
    BinderDataManager* data_manager;
} BinderRadioCapsManager;

typedef struct binder_radio_caps_closure {
    GCClosure cclosure;
    BinderRadioCapsFunc cb;
    void* user_data;
} BinderRadioCapsClosure;

#define binder_radio_caps_closure_new() ((BinderRadioCapsClosure*) \
    g_closure_new_simple(sizeof(BinderRadioCapsClosure), NULL))

struct binder_radio_caps_request {
    BinderRadioCapsObject* caps;
    enum ofono_radio_access_mode modes;
    enum ofono_slot_data_role role;
};

typedef struct binder_radio_caps_check_data {
    BinderRadioCapsCheckFunc cb;
    void* cb_data;
} BinderRadioCapsCheckData;

typedef struct binder_radio_caps_request_tx_phase {
    const char* name;
    RADIO_CAPABILITY_PHASE phase;
    RADIO_CAPABILITY_STATUS status;
    gboolean send_new_cap;
} BinderRadioCapsRequestTxPhase;

typedef
void
(*BinderRadioCapsEnumFunc)(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps);

typedef GObjectClass BinderRadioCapsObjectClass;
GType binder_radio_caps_object_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderRadioCapsObject, binder_radio_caps_object, G_TYPE_OBJECT)
#define RADIO_CAPS_TYPE binder_radio_caps_object_get_type()
#define RADIO_CAPS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, \
        RADIO_CAPS_TYPE, BinderRadioCapsObject)

enum binder_radio_caps_signal {
    CAPS_SIGNAL_RAF_CHANGED,
    CAPS_SIGNAL_COUNT
};

#define CAPS_SIGNAL_RAF_CHANGED_NAME    "binder-radio-raf-changed"
static guint binder_radio_caps_signals[CAPS_SIGNAL_COUNT] = { 0 };

typedef GObjectClass BinderRadioCapsManagerClass;
GType binder_radio_caps_manager_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderRadioCapsManager, binder_radio_caps_manager, G_TYPE_OBJECT)
#define RADIO_CAPS_MANAGER_TYPE binder_radio_caps_manager_get_type()
#define RADIO_CAPS_MANAGER(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, \
        RADIO_CAPS_MANAGER_TYPE, BinderRadioCapsManager)

enum binder_radio_caps_manager_signal {
    CAPS_MANAGER_SIGNAL_ABORTED,
    CAPS_MANAGER_SIGNAL_TX_DONE,
    CAPS_MANAGER_SIGNAL_COUNT
};

#define CAPS_MANAGER_SIGNAL_ABORTED_NAME  "binder-radio-capsmgr-aborted"
#define CAPS_MANAGER_SIGNAL_TX_DONE_NAME  "binder-radio-capsmgr-tx-done"
static guint binder_radio_caps_manager_signals[CAPS_MANAGER_SIGNAL_COUNT] = {0};

static const struct binder_access_mode_raf {
    enum ofono_radio_access_mode mode;
    RADIO_ACCESS_FAMILY raf;
} binder_access_mode_raf_map[] = {
    { OFONO_RADIO_ACCESS_MODE_GSM,  RAF_EDGE | RAF_GPRS | RAF_GSM },
    { OFONO_RADIO_ACCESS_MODE_UMTS, RAF_UMTS },
    { OFONO_RADIO_ACCESS_MODE_LTE,  RAF_LTE | RAF_LTE_CA },
    { OFONO_RADIO_ACCESS_MODE_NR,   RAF_NR }
};

static const BinderRadioCapsRequestTxPhase binder_radio_caps_tx_phase[] = {
    {
        "START",
        RADIO_CAPABILITY_PHASE_START,
        RADIO_CAPABILITY_STATUS_NONE,
        FALSE
    },{
        "APPLY",
        RADIO_CAPABILITY_PHASE_APPLY,
        RADIO_CAPABILITY_STATUS_NONE,
        TRUE
    },{
        "FINISH",
        RADIO_CAPABILITY_PHASE_FINISH,
        RADIO_CAPABILITY_STATUS_SUCCESS,
        TRUE
    }
};

static const BinderRadioCapsRequestTxPhase binder_radio_caps_fail_phase = {
    "ABORT",
    RADIO_CAPABILITY_PHASE_FINISH,
    RADIO_CAPABILITY_STATUS_FAIL,
    FALSE
};

static GUtilIdlePool* binder_radio_caps_shared_pool = NULL;

#define DBG_(obj, fmt, args...) DBG("%s" fmt, (obj)->log_prefix, ##args)

static
void
binder_radio_caps_manager_next_phase(
    BinderRadioCapsManager* mgr);

static
void
binder_radio_caps_manager_consider_requests(
    BinderRadioCapsManager* mgr);

static
void
binder_radio_caps_manager_schedule_check(
    BinderRadioCapsManager* mgr);

static
void
binder_radio_caps_manager_recheck_later(
    BinderRadioCapsManager* mgr);

static
void
binder_radio_caps_manager_add(
    BinderRadioCapsManager* mgr,
    BinderRadioCapsObject* caps);

static
void
binder_radio_caps_manager_remove(
    BinderRadioCapsManager* mgr,
    BinderRadioCapsObject* caps);

static
void
binder_radio_caps_permutate(
    GPtrArray* list,
    const guint* sample,
    guint off,
    guint n)
{
    if (off < n) {
        guint i;

        binder_radio_caps_permutate(list, sample, off + 1, n);
        for (i = off + 1; i < n; i++) {
            guint* resample = gutil_memdup(sample, sizeof(guint) * n);

            resample[off] = sample[i];
            resample[i] = sample[off];
            g_ptr_array_add(list, resample);
            binder_radio_caps_permutate(list, resample, off + 1, n);
        }
    }
}

static
void
binder_radio_caps_generate_permutations(
    GPtrArray* list,
    guint n)
{
    g_ptr_array_set_size(list, 0);

    if (n > 0) {
        guint i;
        guint* order = g_new(guint, n);

        /*
         * In a general case this gives n! of permutations (1, 2,
         * 6, 24, ...) but typically no more than 2
         */
        for (i = 0; i < n; i++) order[i] = i;
        g_ptr_array_set_free_func(list, g_free);
        g_ptr_array_add(list, order);
        binder_radio_caps_permutate(list, order, 0, n);
    }
}

static
RadioCapability*
binder_radio_caps_dup(
    const RadioCapability* in)
{
    if (in) {
        if (in->logicalModemUuid.data.str) {
            /* Allocate the whole thing from a single memory block */
            RadioCapability* out = g_malloc(G_ALIGN8(sizeof(*in)) +
                in->logicalModemUuid.len + 1);
            char* uuid = ((char*)out) + G_ALIGN8(sizeof(*out));

            memcpy(out, in, sizeof(*in));
            memcpy(uuid, in->logicalModemUuid.data.str,
                in->logicalModemUuid.len + 1);
            out->logicalModemUuid.data.str = uuid;
            return out;
        } else {
            return gutil_memdup(in, sizeof(*in));;
        }
    }
    return NULL;
}

static
gboolean
binder_radio_caps_equal(
    const RadioCapability* rc1,
    const RadioCapability* rc2)
{
    if (rc1 == rc2) {
        return TRUE;
    } else if (rc1 && rc2) {
        return rc1->session == rc2->session &&
            rc1->phase == rc2->phase &&
            rc1->raf == rc2->raf &&
            rc1->status == rc2->status &&
            rc1->logicalModemUuid.len == rc2->logicalModemUuid.len &&
            (!rc1->logicalModemUuid.len ||
             !memcmp(rc1->logicalModemUuid.data.str,
                     rc2->logicalModemUuid.data.str,
                     rc1->logicalModemUuid.len));
    } else {
        /* One of the pointers is NULL (but not both) */
        return FALSE;
    }
}

static
void
binder_radio_caps_check_done(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    BinderRadioCapsCheckData* check = user_data;
    const RadioCapability* result = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_RADIO_CAPABILITY) {
            if (error == RADIO_ERROR_NONE) {
                result = binder_read_hidl_struct(args, RadioCapability);
                if (result) {
                    DBG("tx=%d,phase=%d,raf=0x%x,uuid=%s,status=%d",
                        result->session, result->phase, result->raf,
                        result->logicalModemUuid.data.str, result->status);
                }
            } else {
                DBG("getRadioCapability error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getRadioCapability response %d", resp);
        }
    }

    check->cb(result, check->cb_data);
}

static
gboolean
binder_radio_caps_check_retry(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    if (status == RADIO_TX_STATUS_OK) {
        switch (error) {
        case RADIO_ERROR_NONE:
        case RADIO_ERROR_REQUEST_NOT_SUPPORTED:
        case RADIO_ERROR_OPERATION_NOT_ALLOWED:
            return FALSE;
        default:
            return TRUE;
        }
    }
    return TRUE;
}

RadioRequest*
binder_radio_caps_check(
    RadioClient* client,
    BinderRadioCapsCheckFunc cb,
    void* cb_data)
{
    BinderRadioCapsCheckData* check = g_new0(BinderRadioCapsCheckData, 1);

    /* getRadioCapability(int32 serial) */
    RadioRequest* req = radio_request_new(client,
        RADIO_REQ_GET_RADIO_CAPABILITY, NULL,
        binder_radio_caps_check_done, g_free, check);

    check->cb = cb;
    check->cb_data = cb_data;

    /*
     * Make is blocking because this is typically happening at startup
     * when there are lots of things happening at the same time which
     * makes some modems unhappy. Slow things down a bit by not letting
     * to submit any other requests while this one is pending.
     */
    radio_request_set_blocking(req, TRUE);
    radio_request_set_retry(req, GET_CAPS_TIMEOUT_MS, GET_CAPS_RETRIES);
    radio_request_set_retry_func(req, binder_radio_caps_check_retry);
    if (radio_request_submit(req)) {
        return req;
    } else {
        radio_request_unref(req);
        return NULL;
    }
}

/*==========================================================================*
 * binder_radio_caps
 *==========================================================================*/

static inline BinderRadioCapsObject*
binder_radio_caps_cast(BinderRadioCaps* caps)
    { return caps ? RADIO_CAPS(G_CAST(caps,BinderRadioCapsObject,pub)) : NULL; }
static inline void binder_radio_caps_object_ref(BinderRadioCapsObject* self)
    { g_object_ref(self); }
static inline void binder_radio_caps_object_unref(BinderRadioCapsObject* self)
    { g_object_unref(self); }

static
enum ofono_radio_access_mode
binder_radio_caps_access_mode(
    const RadioCapability* cap)
{
    if (cap) {
        const RADIO_ACCESS_FAMILY raf = cap->raf;
        int i;

        /* Return the highest matched mode */
        for (i = G_N_ELEMENTS(binder_access_mode_raf_map); i >= 0; i--) {
            if (raf & binder_access_mode_raf_map[i].raf) {
                return binder_access_mode_raf_map[i].mode;
            }
        }
    }
    return OFONO_RADIO_ACCESS_MODE_ANY;
}

static
enum ofono_radio_access_mode
binder_radio_caps_modes(
    const RadioCapability* cap)
{
    enum ofono_radio_access_mode modes = OFONO_RADIO_ACCESS_MODE_NONE;

    if (cap) {
        const RADIO_ACCESS_FAMILY raf = cap->raf;
        int i;

        /* Bitwise-OR all matched modes */
        for (i = 0; i < G_N_ELEMENTS(binder_access_mode_raf_map); i++) {
            if (raf & binder_access_mode_raf_map[i].raf) {
                modes |= binder_access_mode_raf_map[i].mode;
            }
        }
    }
    return modes;
}

static
void
binder_radio_caps_update_raf(
    BinderRadioCapsObject* self)
{
    BinderRadioCaps* caps = &self->pub;
    const RadioCapability* cap = self->cap;
    RADIO_ACCESS_FAMILY raf = cap ? cap->raf : RAF_NONE;

    if (caps->raf != raf) {
        caps->raf = raf;
        binder_radio_caps_manager_schedule_check(caps->mgr);
        g_signal_emit(self, binder_radio_caps_signals
            [CAPS_SIGNAL_RAF_CHANGED], 0);
    }
}

static
int
binder_radio_caps_score(
    const BinderRadioCapsObject* self,
    const RadioCapability* cap)
{
    if (!self->radio->online || !self->simcard->status ||
        self->simcard->status->card_state != RADIO_CARD_STATE_PRESENT) {
        /* Unusable slot */
        return -(int)binder_radio_caps_modes(cap);
    } else if (self->requested_modes) {
        if (binder_radio_caps_modes(cap) >= self->requested_modes) {
            /* Happy slot (upgrade not required) */
            return self->requested_modes;
        } else {
            /* Unhappy slot (wants upgrade) */
            return -(int)self->requested_modes;
        }
    } else {
        /* Whatever */
        return 0;
    }
}

static
gint
binder_radio_caps_slot_compare(
    gconstpointer a,
    gconstpointer b)
{
    const BinderRadioCapsObject* c1 = *(void**)a;
    const BinderRadioCapsObject* c2 = *(void**)b;

    return c1->slot < c2->slot ? (-1) : c1->slot > c2->slot ? 1 : 0;
}

static
void
binder_radio_caps_radio_event(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    void* user_data)
{
    BinderRadioCapsObject* self = RADIO_CAPS(user_data);

    DBG_(self, "");
    binder_radio_caps_manager_schedule_check(self->pub.mgr);
}

static
void
binder_radio_caps_simcard_event(
    BinderSimCard* sim,
    void* user_data)
{
    BinderRadioCapsObject* self = RADIO_CAPS(user_data);

    DBG_(self, "");
    binder_radio_caps_manager_schedule_check(self->pub.mgr);
}

static
void
binder_radio_caps_watch_event(
    struct ofono_watch* watch,
    void* user_data)
{
    BinderRadioCapsObject* self = RADIO_CAPS(user_data);

    DBG_(self, "");
    binder_radio_caps_manager_schedule_check(self->pub.mgr);
}

static
void
binder_radio_caps_settings_event(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data)
{
    BinderRadioCapsObject* self = RADIO_CAPS(user_data);
    BinderRadioCapsManager* mgr = self->pub.mgr;

    DBG_(self, "");
    binder_radio_caps_manager_consider_requests(mgr);
    binder_radio_caps_manager_schedule_check(mgr);
}

static
void
binder_radio_caps_changed_cb(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    /* radioCapabilityIndication(RadioIndicationType, RadioCapability rc) */
    BinderRadioCapsObject* self = RADIO_CAPS(user_data);
    const RadioCapability* cap = binder_read_hidl_struct(args, RadioCapability);

    DBG_(self, "");
    if (cap) {
        g_free(self->cap);
        self->cap = binder_radio_caps_dup(cap);
        binder_radio_caps_update_raf(self);
        binder_radio_caps_manager_schedule_check(self->pub.mgr);
    } else {
        ofono_error("Failed to parse RadioCapability payload");
    }
}

static
void
binder_radio_caps_finish_init(
    BinderRadioCapsObject* self)
{
    /* Register for update notifications */
    self->client_event_id[CLIENT_EVENT_IND_RADIO_CAPABILITY] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_RADIO_CAPABILITY_INDICATION,
            binder_radio_caps_changed_cb, self);

    /* Schedule capability check */
    binder_radio_caps_manager_schedule_check(self->pub.mgr);
}

static
void
binder_radio_caps_initial_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderRadioCapsObject* self = RADIO_CAPS(user_data);
    const RadioCapability* cap = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_RADIO_CAPABILITY) {
            /* getRadioCapabilityResponse(RadioResponseInfo, RadioCapability) */
            if (error == RADIO_ERROR_NONE) {
                cap = binder_read_hidl_struct(args, RadioCapability);
            } else {
                DBG_(self, "Failed get radio caps, error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getRadioCapability response %d", resp);
        }
    }

    if (cap) {
        binder_radio_caps_update_raf(self);
        binder_radio_caps_finish_init(self);
    }
}

BinderRadioCaps*
binder_radio_caps_new(
    BinderRadioCapsManager* mgr,
    const char* log_prefix,
    RadioClient* client,
    struct ofono_watch* watch,
    BinderData* data,
    BinderRadio* radio,
    BinderSimCard* sim,
    BinderSimSettings* settings,
    const BinderSlotConfig* config,
    const RadioCapability* cap)
{
    GASSERT(mgr);
    if (G_LIKELY(mgr)) {
        BinderRadioCapsObject* self = g_object_new(RADIO_CAPS_TYPE, 0);
        BinderRadioCaps* caps = &self->pub;

        self->slot = config->slot;
        self->log_prefix = binder_dup_prefix(log_prefix);

        self->g = radio_request_group_new(client);
        self->radio = binder_radio_ref(radio);
        self->data = binder_data_ref(data);
        caps->mgr = binder_radio_caps_manager_ref(mgr);

        self->radio = binder_radio_ref(radio);
        self->radio_event_id[RADIO_EVENT_STATE] =
            binder_radio_add_property_handler(radio,
                BINDER_RADIO_PROPERTY_STATE,
                binder_radio_caps_radio_event, self);
        self->radio_event_id[RADIO_EVENT_ONLINE] =
            binder_radio_add_property_handler(radio,
                BINDER_RADIO_PROPERTY_ONLINE,
                binder_radio_caps_radio_event, self);

        self->simcard = binder_sim_card_ref(sim);
        self->simcard_event_id[SIM_EVENT_STATE_CHANGED] =
            binder_sim_card_add_state_changed_handler(sim,
                binder_radio_caps_simcard_event, self);
        self->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
            binder_sim_card_add_sim_io_active_changed_handler(sim,
                binder_radio_caps_simcard_event, self);

        self->watch = ofono_watch_ref(watch);
        self->watch_event_id[WATCH_EVENT_IMSI] =
            ofono_watch_add_imsi_changed_handler(watch,
                binder_radio_caps_watch_event, self);
        self->watch_event_id[WATCH_EVENT_MODEM] =
            ofono_watch_add_modem_changed_handler(watch,
                binder_radio_caps_watch_event, self);

        self->settings = binder_sim_settings_ref(settings);
        self->settings_event_id[SETTINGS_EVENT_PREF_MODE] =
            binder_sim_settings_add_property_handler(settings,
                BINDER_SIM_SETTINGS_PROPERTY_PREF,
                binder_radio_caps_settings_event, self);

        binder_radio_caps_manager_add(mgr, self);
        if (cap) {
            /* Current capabilities are provided by the caller */
            self->cap = binder_radio_caps_dup(cap);
            caps->raf = cap->raf;
            binder_radio_caps_finish_init(self);
        } else {
            /* Need to query current capabilities */
            RadioRequest* req = radio_request_new2(self->g,
                RADIO_REQ_GET_RADIO_CAPABILITY, NULL,
                binder_radio_caps_initial_query_cb, NULL, self);

            radio_request_set_retry(req, GET_CAPS_TIMEOUT_MS, GET_CAPS_RETRIES);
            radio_request_submit(req);
            radio_request_unref(req);
        }
        return caps;
    }
    return NULL;
}

BinderRadioCaps*
binder_radio_caps_ref(
    BinderRadioCaps* caps)
{
    BinderRadioCapsObject* self = binder_radio_caps_cast(caps);

    if (G_LIKELY(self)) {
        binder_radio_caps_object_ref(self);
    }
    return caps;
}

void
binder_radio_caps_unref(
    BinderRadioCaps* caps)
{
    BinderRadioCapsObject* self = binder_radio_caps_cast(caps);

    if (G_LIKELY(self)) {
        binder_radio_caps_object_unref(self);
    }
}

void
binder_radio_caps_drop(
    BinderRadioCaps* caps)
{
    BinderRadioCapsObject* self = binder_radio_caps_cast(caps);

    if (G_LIKELY(self)) {
        binder_radio_caps_manager_remove(self->pub.mgr, self);
        g_object_unref(self);
    }
}

static
void
binder_radio_caps_signal_cb(
    BinderRadioCapsObject* caps,
    BinderRadioCapsClosure* closure)
{
    closure->cb(&caps->pub, closure->user_data);
}

gulong
binder_radio_caps_add_raf_handler(
    BinderRadioCaps* caps,
    BinderRadioCapsFunc cb,
    void* arg)
{
    BinderRadioCapsObject* self = binder_radio_caps_cast(caps);

    if (G_LIKELY(self) && G_LIKELY(cb)) {
        BinderRadioCapsClosure* closure = binder_radio_caps_closure_new();
        GCClosure* cc = &closure->cclosure;

        cc->closure.data = closure;
        cc->callback = G_CALLBACK(binder_radio_caps_signal_cb);
        closure->cb = cb;
        closure->user_data = arg;

        return g_signal_connect_closure_by_id(self,
            binder_radio_caps_signals[CAPS_SIGNAL_RAF_CHANGED],
            0, &cc->closure, FALSE);
    }
    return 0;
}

void
binder_radio_caps_remove_handler(
    BinderRadioCaps* caps,
    gulong id)
{
    if (G_LIKELY(id)) {
        BinderRadioCapsObject* self = binder_radio_caps_cast(caps);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

static
void
binder_radio_caps_object_finalize(
    GObject* object)
{
    BinderRadioCapsObject* self = RADIO_CAPS(object);
    BinderRadioCapsManager* mgr = self->pub.mgr;

    binder_radio_remove_all_handlers(self->radio, self->radio_event_id);
    binder_sim_settings_remove_all_handlers(self->settings,
        self->settings_event_id);
    binder_sim_card_remove_all_handlers(self->simcard, self->simcard_event_id);

    ofono_watch_remove_all_handlers(self->watch, self->watch_event_id);
    ofono_watch_unref(self->watch);

    binder_radio_caps_manager_remove(mgr, self);
    binder_radio_caps_manager_unref(mgr);

    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    radio_client_remove_all_handlers(self->client, self->client_event_id);
    radio_client_unref(self->client);

    binder_data_unref(self->data);
    binder_radio_unref(self->radio);
    binder_sim_card_unref(self->simcard);
    binder_sim_settings_unref(self->settings);
    gutil_idle_pool_unref(self->idle_pool);
    g_free(self->log_prefix);
    g_free(self->cap);
    g_free(self->old_cap);
    g_free(self->new_cap);
    G_OBJECT_CLASS(binder_radio_caps_object_parent_class)->finalize(object);
}

static
void
binder_radio_caps_object_init(
    BinderRadioCapsObject* self)
{
    self->idle_pool = gutil_idle_pool_ref
        (gutil_idle_pool_get(&binder_radio_caps_shared_pool));
}

static
void
binder_radio_caps_object_class_init(BinderRadioCapsObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_radio_caps_object_finalize;
    binder_radio_caps_signals[CAPS_SIGNAL_RAF_CHANGED] =
        g_signal_new(CAPS_SIGNAL_RAF_CHANGED_NAME,
            G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST,
            0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * binder_radio_caps_manager
 *==========================================================================*/

static
const char*
binder_radio_caps_manager_order_str(
    BinderRadioCapsManager* self,
    const guint* order)
{
    const guint n = self->caps_list->len;

    if (n > 0) {
        guint i;
        char* str;
        GString* buf = g_string_sized_new(2*n + 2 /* roughly */);

        g_string_append_printf(buf, "(%u", order[0]);
        for (i = 1; i < n; i++) {
            g_string_append_printf(buf, ",%u", order[i]);
        }
        g_string_append_c(buf, ')');
        str = g_string_free(buf, FALSE);
        gutil_idle_pool_add(self->idle_pool, str, g_free);
        return str;
    } else {
        return "()";
    }
}

static
const char*
binder_radio_caps_manager_role_str(
    BinderRadioCapsManager* self,
    enum ofono_slot_data_role role)
{
    char* str;

    switch (role) {
    case OFONO_SLOT_DATA_NONE:
        return "none";
    case OFONO_SLOT_DATA_MMS:
        return "mms";
    case OFONO_SLOT_DATA_INTERNET:
        return "internet";
    }

    str = g_strdup_printf("%d", (int)role);
    gutil_idle_pool_add(self->idle_pool, str, g_free);
    return str;
}

static
void
binder_radio_caps_manager_foreach(
    BinderRadioCapsManager* self,
    BinderRadioCapsEnumFunc cb)
{
    guint i;
    const GPtrArray* list = self->caps_list;

    for (i = 0; i < list->len; i++) {
        cb(self, (BinderRadioCapsObject*)(list->pdata[i]));
    }
}

static
void
binder_radio_caps_manager_foreach_tx(
    BinderRadioCapsManager* self,
    BinderRadioCapsEnumFunc cb)
{
    guint i;
    const GPtrArray* list = self->caps_list;

    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* caps = list->pdata[i];

        /* Ignore the modems not associated with this transaction */
        if (caps->tx_id == self->tx_id) {
            cb(self, caps);
        }
    }
}

static
gboolean
binder_radio_caps_manager_tx_pending(
    BinderRadioCapsManager* self)
{
    guint i;
    const GPtrArray* list = self->caps_list;

    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* caps = list->pdata[i];

        /* Ignore the modems not associated with this transaction */
        if (caps->tx_id == self->tx_id && caps->tx_pending > 0) {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * Checks that all radio caps have been initialized (i.e. all the initial
 * GET_RADIO_CAPABILITY requests have completed) and there's no transaction
 * in progress.
 */
static
gboolean
binder_radio_caps_manager_can_check(
    BinderRadioCapsManager* self)
{
    if (self->caps_list && !binder_radio_caps_manager_tx_pending(self)) {
        const GPtrArray* list = self->caps_list;
        const BinderRadioCapsObject* prev_caps = NULL;
        gboolean all_modes_equal = TRUE;
        guint i;

        for (i = 0; i < list->len; i++) {
            const BinderRadioCapsObject* caps = list->pdata[i];
            const BinderRadio* radio = caps->radio;
            const BinderSimCardStatus* status = caps->simcard->status;
            const gboolean slot_enabled = (caps->watch->modem != NULL);
            const gboolean sim_present = status &&
                (status->card_state == RADIO_CARD_STATE_PRESENT);

            if (slot_enabled && ((radio->online &&
                (radio->state != RADIO_STATE_ON || !caps->cap)) ||
                (sim_present && !caps->settings->imsi))) {
                DBG_(caps, "not ready");
                return FALSE;
            }

            if (!prev_caps) {
                prev_caps = caps;
            } else if (binder_radio_caps_access_mode(prev_caps->cap) !=
                binder_radio_caps_access_mode(caps->cap)) {
                all_modes_equal = FALSE;
            }

            DBG_(caps, "enabled=%s,online=%s,sim=%s,imsi=%s,"
                "raf=0x%x(%s),uuid=%s,req=%s,score=%d",
                slot_enabled ? "yes" : "no", radio->online ? "yes" : "no",
                status ? (status->card_state == RADIO_CARD_STATE_PRESENT) ?
                "yes" : "no" : "?",
                caps->settings->imsi ? caps->settings->imsi : "",
                 caps->cap ? caps->cap->raf : 0,
                ofono_radio_access_mode_to_string
                (binder_radio_caps_access_mode(caps->cap)),
                caps->cap ? caps->cap->logicalModemUuid.data.str : "(null)",
                ofono_radio_access_mode_to_string(caps->requested_modes),
                 binder_radio_caps_score(caps, caps->cap));
        }
        return !all_modes_equal;
    }
    return FALSE;
}

static
void
binder_radio_caps_manager_issue_requests(
    BinderRadioCapsManager* self,
    const BinderRadioCapsRequestTxPhase* phase,
    RadioRequestCompleteFunc handler)
{
    guint i;
    const GPtrArray* list = self->caps_list;

    DBG("%s transaction %d", phase->name, self->tx_id);
    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* caps = list->pdata[i];

        /* Ignore the modems not associated with this transaction */
        if (caps->tx_id == self->tx_id) {
            GBinderWriter args;
            RadioRequest* req = radio_request_new2(caps->g,
                RADIO_REQ_SET_RADIO_CAPABILITY, &args,
                handler, NULL, caps);
            const RadioCapability* cap = phase->send_new_cap ?
                caps->new_cap : caps->old_cap;
            RadioCapability* rc = gbinder_writer_new0(&args, RadioCapability);
            guint index;

            /* setRadioCapability(int32 serial, RadioCapability rc) */
            rc->session = self->tx_id;
            rc->phase = phase->phase;
            rc->raf = cap->raf;
            rc->status = phase->status;
            rc->logicalModemUuid = cap->logicalModemUuid;
            if (rc->logicalModemUuid.len) {
                rc->logicalModemUuid.data.str = gbinder_writer_memdup(&args,
                    cap->logicalModemUuid.data.str,
                    cap->logicalModemUuid.len + 1);
            }

            /* Write transaction arguments */
            index = gbinder_writer_append_buffer_object(&args, rc, sizeof(*rc));
            binder_append_hidl_string_data(&args, rc, logicalModemUuid, index);

            /* Submit the request */
            radio_request_set_timeout(req, SET_CAPS_TIMEOUT_MS);
            if (radio_request_submit(req)) {
                /* Count it */
                caps->tx_pending++;
                DBG_(caps, "tx_pending=%d", caps->tx_pending);
            } else {
                ofono_error("failed to set radio caps");
            }
            radio_request_unref(req);
        }
    }
}

static
void
binder_radio_caps_manager_next_transaction_cb(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    radio_request_group_cancel(caps->g);
    radio_client_remove_handlers(caps->client, caps->client_event_id +
        CLIENT_EVENT_OWNER, 1);
    binder_sim_card_remove_handlers(caps->simcard, caps->simcard_event_id +
        SIM_EVENT_IO_ACTIVE_CHANGED, 1);
}

static
void
binder_radio_caps_manager_next_transaction(
    BinderRadioCapsManager* self)
{
    binder_radio_caps_manager_foreach(self,
        binder_radio_caps_manager_next_transaction_cb);
    self->tx_failed = FALSE;
    self->tx_phase_index = -1;
    self->tx_id++;
    if (self->tx_id <= 0) self->tx_id = 1;
}

static
void binder_radio_caps_manager_cancel_cb(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    GASSERT(!caps->client_event_id[CLIENT_EVENT_OWNER]);
    radio_request_group_unblock(caps->g);
}

static
void
binder_radio_caps_manager_transaction_done(
    BinderRadioCapsManager* self)
{
    binder_radio_caps_manager_schedule_check(self);
    binder_data_manager_assert_data_on(self->data_manager);
    binder_radio_caps_manager_foreach(self,
        binder_radio_caps_manager_cancel_cb);
}

static
void
binder_radio_caps_manager_abort_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderRadioCapsObject* caps = RADIO_CAPS(user_data);
    BinderRadioCapsManager* self = caps->pub.mgr;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SET_RADIO_CAPABILITY) {
            if (error != RADIO_ERROR_NONE) {
                DBG_(caps, "Failed to abort radio caps switch, error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected setRadioCapability response %d", resp);
        }
    }

    GASSERT(caps->tx_pending > 0);
    caps->tx_pending--;
    DBG_(caps, "tx_pending=%d", caps->tx_pending);

    if (!binder_radio_caps_manager_tx_pending(self)) {
        DBG("transaction aborted");
        binder_radio_caps_manager_transaction_done(self);
    }
}

static
void
binder_radio_caps_manager_abort_transaction(
    BinderRadioCapsManager* self)
{
    guint i;
    const GPtrArray* list = self->caps_list;
    const int prev_tx_id = self->tx_id;

    /* Generate new transaction id */
    DBG("aborting transaction %d", prev_tx_id);
    binder_radio_caps_manager_next_transaction(self);

    /* Re-associate the modems with the new transaction */
    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* caps = list->pdata[i];

        if (caps->tx_id == prev_tx_id) {
            caps->tx_id = self->tx_id;
        }
    }

    /*
     * Issue a FINISH with RADIO_CAPABILITY_STATUS_FAIL. That's what
     * com.android.internal.telephony.ProxyController does (or at least
     * did last time when I checked) when something goes wrong.
     */
    binder_radio_caps_manager_issue_requests(self,
        &binder_radio_caps_fail_phase, binder_radio_caps_manager_abort_cb);

    /* Notify the listeners */
    g_signal_emit(self, binder_radio_caps_manager_signals
        [CAPS_MANAGER_SIGNAL_ABORTED], 0);
}

static
void binder_radio_caps_manager_next_phase_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderRadioCapsObject* caps = RADIO_CAPS(user_data);
    BinderRadioCapsManager* self = caps->pub.mgr;
    gboolean ok = FALSE;

    GASSERT(caps->tx_pending > 0);
    if (status == RADIO_TX_STATUS_OK) {
        /* getRadioCapabilityResponse(RadioResponseInfo, RadioCapability rc) */
        if (resp == RADIO_RESP_SET_RADIO_CAPABILITY) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                const RadioCapability* rc;

                gbinder_reader_copy(&reader, args);
                rc = gbinder_reader_read_hidl_struct(&reader, RadioCapability);
                if (rc && rc->status != RADIO_CAPABILITY_STATUS_FAIL) {
                    ok = TRUE;
                }
            } else {
                DBG_(caps, "Failed to set radio caps, error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected setRadioCapability response %d", resp);
        }
    }

    if (!ok) {
        if (!self->tx_failed) {
            self->tx_failed = TRUE;
            DBG("transaction %d failed", self->tx_id);
        }
    }

    caps->tx_pending--;
    DBG_(caps, "tx_pending=%d", caps->tx_pending);
    if (!binder_radio_caps_manager_tx_pending(self)) {
        if (self->tx_failed) {
            binder_radio_caps_manager_abort_transaction(self);
        } else {
            binder_radio_caps_manager_next_phase(self);
        }
    }
}

static
void
binder_radio_caps_manager_next_phase(
    BinderRadioCapsManager* self)
{
    /* Note: -1 > 2 if 2 is unsigned (which turns -1 into 4294967295) */
    const int max_index = G_N_ELEMENTS(binder_radio_caps_tx_phase) - 1;

    GASSERT(!binder_radio_caps_manager_tx_pending(self));
    if (self->tx_phase_index >= max_index) {
        const GPtrArray* list = self->caps_list;
        GSList* updated_caps = NULL;
        GSList* l;
        guint i;

        DBG("transaction %d is done", self->tx_id);

        /* Update all caps before emitting signals */
        for (i = 0; i < list->len; i++) {
            BinderRadioCapsObject* caps = list->pdata[i];

            if (caps->tx_id == self->tx_id) {
                caps->cap = caps->new_cap;
                /* Better bump refs to make sure BinderRadioCapsObject
                 * don't get freed by a signal handler */
                binder_radio_caps_object_ref(caps);
                updated_caps = g_slist_append(updated_caps, caps);
            }
        }
        /* binder_radio_caps_update_raf will emit signals if needed */
        for (l = updated_caps; l; l = l->next) {
            binder_radio_caps_update_raf((BinderRadioCapsObject*)l->data);
        }

        binder_radio_caps_manager_transaction_done(self);
        /* Free temporary BinderRadioCapsObject references */
        g_slist_free_full(updated_caps, g_object_unref);
        g_signal_emit(self, binder_radio_caps_manager_signals
            [CAPS_MANAGER_SIGNAL_TX_DONE], 0);
    } else {
        const BinderRadioCapsRequestTxPhase* phase =
            binder_radio_caps_tx_phase + (++self->tx_phase_index);

        binder_radio_caps_manager_issue_requests(self, phase,
            binder_radio_caps_manager_next_phase_cb);
    }
}

static
void
binder_radio_caps_manager_data_off_done(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    if (!binder_radio_caps_manager_tx_pending(self)) {
        if (self->tx_failed) {
            DBG("failed to start the transaction");
            binder_data_manager_assert_data_on(self->data_manager);
            binder_radio_caps_manager_recheck_later(self);
            binder_radio_caps_manager_foreach(self,
                binder_radio_caps_manager_cancel_cb);
            g_signal_emit(self, binder_radio_caps_manager_signals
                [CAPS_MANAGER_SIGNAL_ABORTED], 0);
        } else {
            DBG("starting transaction");
            binder_radio_caps_manager_next_phase(self);
        }
    }
}

static
void
binder_radio_caps_manager_data_disallowed(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderRadioCapsObject* caps = RADIO_CAPS(user_data);
    BinderRadioCapsManager* self = caps->pub.mgr;

    GASSERT(caps->tx_pending > 0);
    caps->tx_pending--;
    DBG_(caps, "tx_pending=%d", caps->tx_pending);

    if (status != RADIO_TX_STATUS_OK || error != RADIO_ERROR_NONE) {
        self->tx_failed = TRUE;
    }

    binder_radio_caps_manager_data_off_done(self, caps);
}

static
void
binder_radio_caps_manager_data_off(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    if (binder_data_manager_need_set_data_allowed(self->data_manager)) {
        RadioRequest* req = binder_data_set_data_allowed_request_new(caps->g,
            FALSE, binder_radio_caps_manager_data_disallowed, NULL, caps);

        caps->tx_pending++;
        DBG_(caps, "tx_pending=%d", caps->tx_pending);
        radio_request_set_timeout(req, DATA_OFF_TIMEOUT_MS);
        radio_request_submit(req);
        radio_request_unref(req);
    } else {
        binder_radio_caps_manager_data_off_done(self, caps);
    }
}

static
void
binder_radio_caps_manager_deactivate_data_call_done(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderRadioCapsObject* caps = RADIO_CAPS(user_data);
    BinderRadioCapsManager* self = caps->pub.mgr;

    GASSERT(caps->tx_pending > 0);
    caps->tx_pending--;
    DBG_(caps, "tx_pending=%d", caps->tx_pending);

    if (status != RADIO_TX_STATUS_OK || error != RADIO_ERROR_NONE) {
        self->tx_failed = TRUE;
        /*
         * Something seems to be slightly broken, try requesting the
         * current state (later, after we release the block).
         */
        binder_data_poll_call_state(caps->data);
    }

    if (!binder_radio_caps_manager_tx_pending(self)) {
        if (self->tx_failed) {
            DBG("failed to start the transaction");
            binder_radio_caps_manager_recheck_later(self);
            binder_radio_caps_manager_foreach(self,
                binder_radio_caps_manager_cancel_cb);
        } else {
            binder_radio_caps_manager_foreach_tx(self,
                binder_radio_caps_manager_data_off);
        }
    }
}

static
void
binder_radio_caps_deactivate_data_call(
    BinderRadioCapsObject* caps,
    int cid)
{
    RadioRequest* req = binder_data_deactivate_data_call_request_new(caps->g,
        cid, binder_radio_caps_manager_deactivate_data_call_done,
        NULL, caps);

    caps->tx_pending++;
    DBG_(caps, "cid=%u, tx_pending=%d", cid, caps->tx_pending);
    radio_request_set_blocking(req, TRUE);
    radio_request_set_timeout(req, DEACTIVATE_TIMEOUT_MS);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_radio_caps_deactivate_data_call_cb(
    gpointer list_data,
    gpointer user_data)
{
    BinderDataCall* call = list_data;

    if (call->status == RADIO_DATA_CALL_FAIL_NONE) {
        binder_radio_caps_deactivate_data_call(RADIO_CAPS(user_data),
            call->cid);
    }
}

static
void
binder_radio_caps_manager_deactivate_all_cb(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    BinderData* data = caps->data;

    if (data) {
        g_slist_foreach(data->calls, binder_radio_caps_deactivate_data_call_cb,
            caps);
    }
}

static
void
binder_radio_caps_manager_deactivate_all(
    BinderRadioCapsManager* self)
{
    binder_radio_caps_manager_foreach_tx(self,
        binder_radio_caps_manager_deactivate_all_cb);
    if (!binder_radio_caps_manager_tx_pending(self)) {
        /* No data calls, submit setDataAllowed requests right away */
        binder_radio_caps_manager_foreach_tx(self,
            binder_radio_caps_manager_data_off);
        GASSERT(binder_radio_caps_manager_tx_pending(self));
    }
}

static
void
binder_radio_caps_tx_wait_cb(
    RadioClient* client,
    void* user_data)
{
    BinderRadioCapsObject* caps = RADIO_CAPS(user_data);
    BinderRadioCapsManager* self = caps->pub.mgr;
    const GPtrArray* list = self->caps_list;
    gboolean can_start = TRUE;
    guint i;

    if (radio_request_group_block_status(caps->g) == RADIO_BLOCK_ACQUIRED) {
        /* We no longer need owner notifications from this channel */
        radio_client_remove_handlers(caps->client, caps->client_event_id +
            CLIENT_EVENT_OWNER, 1);
    }

    /* Check if all channels are ours */
    for (i = 0; i < list->len && can_start; i++) {
        const BinderRadioCapsObject* caps = list->pdata[i];

        if (caps->tx_id == self->tx_id &&
            radio_request_group_block_status(caps->g) != RADIO_BLOCK_ACQUIRED) {
            /* Still waiting for this one */
            DBG_(caps, "still waiting");
            can_start = FALSE;
        }
    }

    if (can_start) {
        /* All modems are ready */
        binder_radio_caps_manager_deactivate_all(self);
    }
}

static
void
binder_radio_caps_manager_lock_io_for_transaction(
    BinderRadioCapsManager* self)
{
    const GPtrArray* list = self->caps_list;
    gboolean can_start = TRUE;
    guint i;

    /*
     * We want to actually start the transaction when all the involved
     * modems stop doing other things. Otherwise some modems get confused
     * and break. We have already checked that SIM I/O has stopped. The
     * next synchronization point is the completion of all deactivateDataCall
     * and setDataAllowed requests. Then we can really start the capability
     * switch transaction.
     */
    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* caps = list->pdata[i];

        /*
         * Reset the block to make sure that we get to the end of
         * the block queue (to avoid deadlocks since we are going
         * to wait for all request groups to become the owners
         * before actually starting the transaction)
         */
        radio_request_group_unblock(caps->g);

        /*
         * Check if we need to wait for all requests to complete for
         * this client before we can actually  start the transaction.
         */
        if (radio_request_group_block(caps->g) == RADIO_BLOCK_QUEUED) {
            GASSERT(!caps->client_event_id[CLIENT_EVENT_OWNER]);
            caps->client_event_id[CLIENT_EVENT_OWNER] =
                radio_client_add_owner_changed_handler(caps->client,
                    binder_radio_caps_tx_wait_cb, caps);
            can_start = FALSE;
        }
    }

    if (can_start) {
        /* All modems are ready */
        binder_radio_caps_manager_deactivate_all(self);
    }
}

static
void
binder_radio_caps_manager_stop_sim_io_watch(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    /* binder_sim_card_remove_handlers zeros the id */
    binder_sim_card_remove_handlers(caps->simcard, caps->simcard_event_id +
        SIM_EVENT_IO_ACTIVE_CHANGED, 1);
}

static
void
binder_radio_caps_tx_wait_sim_io_cb(
    BinderSimCard* simcard,
    void* user_data)
{
    BinderRadioCapsObject* src = RADIO_CAPS(user_data);
    BinderRadioCapsManager* self = src->pub.mgr;
    const GPtrArray* list = self->caps_list;
    guint i;

    for (i = 0; i < list->len; i++) {
        const BinderRadioCapsObject* caps = list->pdata[i];

        if (caps->simcard->sim_io_active) {
            DBG_(caps, "still waiting for SIM I/O to calm down");
            return;
        }
    }

    /* We no longer need to be notified about SIM I/O activity */
    DBG("SIM I/O has calmed down");
    binder_radio_caps_manager_foreach(self,
        binder_radio_caps_manager_stop_sim_io_watch);

    /* Now this looks like a good moment to start the transaction */
    binder_radio_caps_manager_lock_io_for_transaction(self);
}

static
void
binder_radio_caps_manager_start_sim_io_watch(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    caps->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
        binder_sim_card_add_sim_io_active_changed_handler(caps->simcard,
            binder_radio_caps_tx_wait_sim_io_cb, caps);
}

static
void
binder_radio_caps_manager_start_transaction(
    BinderRadioCapsManager *self)
{
    const GPtrArray* list = self->caps_list;
    gboolean sim_io_active = FALSE;
    guint i, count = 0;

    /* Start the new request transaction */
    binder_radio_caps_manager_next_transaction(self);
    DBG("transaction %d", self->tx_id);

    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* caps = list->pdata[i];

        if (!binder_radio_caps_equal(caps->new_cap, caps->old_cap)) {
            /* Mark it as taking part in this transaction */
            caps->tx_id = self->tx_id;
            count++;
            if (caps->simcard->sim_io_active) {
                sim_io_active = TRUE;
            }
        }
    }

    GASSERT(count);
    if (!count) {
        /* This is not supposed to happen */
        DBG("nothing to do!");
    } else if (sim_io_active) {
        DBG("waiting for SIM I/O to calm down");
        binder_radio_caps_manager_foreach_tx(self,
            binder_radio_caps_manager_start_sim_io_watch);
    } else {
        /* Make sure we don't get notified about SIM I/O activity */
        binder_radio_caps_manager_foreach(self,
            binder_radio_caps_manager_stop_sim_io_watch);

        /* And continue with locking BINDER I/O for the transaction */
        binder_radio_caps_manager_lock_io_for_transaction(self);
    }
}

static
void
binder_radio_caps_manager_set_order(
    BinderRadioCapsManager* self,
    const guint* order)
{
    const GPtrArray* list = self->caps_list;
    guint i;

    DBG("%s => %s",
        binder_radio_caps_manager_order_str(self, self->order_list->pdata[0]),
        binder_radio_caps_manager_order_str(self, order));

    for (i = 0; i < list->len; i++) {
        BinderRadioCapsObject* dest = list->pdata[i];
        const BinderRadioCapsObject* src = list->pdata[order[i]];

        g_free(dest->old_cap);
        g_free(dest->new_cap);
        dest->old_cap = binder_radio_caps_dup(dest->cap);
        dest->new_cap = binder_radio_caps_dup(src->cap);
    }
    binder_radio_caps_manager_start_transaction(self);
}

static
void
binder_radio_caps_manager_check(
    BinderRadioCapsManager *self)
{
    if (binder_radio_caps_manager_can_check(self)) {
        guint i;
        const GPtrArray* list = self->caps_list;
        const GPtrArray* permutations = self->order_list;
        int highest_score = -INT_MAX, best_index = -1;

        for (i = 0; i < permutations->len; i++) {
            const guint* order = permutations->pdata[i];
            int score = 0;
            guint k;

            for (k = 0; k < list->len; k++) {
                const BinderRadioCapsObject *c1 = list->pdata[k];
                const BinderRadioCapsObject *c2 = list->pdata[order[k]];

                score += binder_radio_caps_score(c1, c2->cap);
            }

            DBG("%s %d", binder_radio_caps_manager_order_str(self, order),
                score);
            if (score > highest_score) {
                highest_score = score;
                best_index = i;
            }
        }

        if (best_index > 0) {
            binder_radio_caps_manager_set_order(self,
                permutations->pdata[best_index]);
        }
    }
}

static
gboolean
binder_radio_caps_manager_check_cb(
    gpointer user_data)
{
    BinderRadioCapsManager* self = RADIO_CAPS_MANAGER(user_data);

    GASSERT(self->check_id);
    self->check_id = 0;
    binder_radio_caps_manager_check(self);
    return G_SOURCE_REMOVE;
}

static
void
binder_radio_caps_manager_recheck_later(
    BinderRadioCapsManager* self)
{
    if (!binder_radio_caps_manager_tx_pending(self)) {
        if (self->check_id) {
            g_source_remove(self->check_id);
            self->check_id = 0;
        }
        self->check_id = g_timeout_add_seconds(CHECK_LATER_TIMEOUT_SEC,
            binder_radio_caps_manager_check_cb, self);
    }
}

static
void
binder_radio_caps_manager_schedule_check(
    BinderRadioCapsManager* self)
{
    if (!self->check_id && !binder_radio_caps_manager_tx_pending(self)) {
        self->check_id = g_idle_add(binder_radio_caps_manager_check_cb, self);
    }
}

static
gint
binder_caps_manager_request_sort(
    gconstpointer a,
    gconstpointer b)
{
    const BinderRadioCapsRequest* r1 = *(void**)a;
    const BinderRadioCapsRequest* r2 = *(void**)b;

    /* MMS requests have higher priority */
    if (r1->role == OFONO_SLOT_DATA_MMS) {
        if (r2->role != OFONO_SLOT_DATA_MMS) {
            return -1;
        }
    } else if (r2->role == OFONO_SLOT_DATA_MMS) {
        return 1;
    }
    return (int)r2->role - (int)r1->role;
}

static
void
binder_radio_caps_manager_consider_requests(
    BinderRadioCapsManager* self)
{
    guint i;
    gboolean changed = FALSE;
    const GPtrArray* list = self->caps_list;
    GPtrArray* requests = self->requests;

    if (requests->len) {
        const BinderRadioCapsRequest* req;

        g_ptr_array_sort(requests, binder_caps_manager_request_sort);
        req = self->requests->pdata[0];

        for (i = 0; i < list->len; i++) {
            BinderRadioCapsObject* caps = list->pdata[i];
            BinderSimSettings* settings = caps->settings;
            const enum ofono_radio_access_mode modes = (req->caps == caps) ?
                (req->modes & settings->pref) : OFONO_RADIO_ACCESS_MODE_NONE;

            if (caps->requested_modes != modes) {
                caps->requested_modes = modes;
                changed = TRUE;
            }
        }
    } else {
        for (i = 0; i < list->len; i++) {
            BinderRadioCapsObject* caps = list->pdata[i];

            if (caps->requested_modes) {
                caps->requested_modes = OFONO_RADIO_ACCESS_MODE_NONE;
                changed = TRUE;
            }
        }
    }
    if (changed) {
        binder_radio_caps_manager_schedule_check(self);
    }
}

static
void
binder_radio_caps_manager_list_changed(
    BinderRadioCapsManager* self)
{
    /* Order list elements according to slot numbers */
    g_ptr_array_sort(self->caps_list, binder_radio_caps_slot_compare);

    /* Generate full list of available permutations */
    binder_radio_caps_generate_permutations(self->order_list,
        self->caps_list->len);
}

static
void
binder_radio_caps_manager_add(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    g_ptr_array_add(self->caps_list, caps);
    binder_radio_caps_manager_list_changed(self);
}

static
void binder_radio_caps_manager_remove(
    BinderRadioCapsManager* self,
    BinderRadioCapsObject* caps)
{
    if (g_ptr_array_remove(self->caps_list, caps)) {
        binder_radio_caps_manager_list_changed(self);
    }
}

gulong
binder_radio_caps_manager_add_tx_aborted_handler(
    BinderRadioCapsManager* self,
    BinderRadioCapsManagerFunc cb,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
        CAPS_MANAGER_SIGNAL_ABORTED_NAME, G_CALLBACK(cb), user_data) : 0;
}

gulong
binder_radio_caps_manager_add_tx_done_handler(
    BinderRadioCapsManager* self,
    BinderRadioCapsManagerFunc cb,
    void* user_data)
{
    return (G_LIKELY(self) && G_LIKELY(cb)) ? g_signal_connect(self,
        CAPS_MANAGER_SIGNAL_TX_DONE_NAME, G_CALLBACK(cb), user_data) : 0;
}

void
binder_radio_caps_manager_remove_handler(
    BinderRadioCapsManager* self,
    gulong id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_signal_handler_disconnect(self, id);
    }
}

void
binder_radio_caps_manager_remove_handlers(
    BinderRadioCapsManager* self,
    gulong* ids,
    int count)
{
    gutil_disconnect_handlers(self, ids, count);
}

BinderRadioCapsManager*
binder_radio_caps_manager_ref(
    BinderRadioCapsManager *self)
{
    if (G_LIKELY(self)) {
        g_object_ref(RADIO_CAPS_MANAGER(self));
    }
    return self;
}

void
binder_radio_caps_manager_unref(
    BinderRadioCapsManager* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(RADIO_CAPS_MANAGER(self));
    }
}

BinderRadioCapsManager*
binder_radio_caps_manager_new(
    BinderDataManager* dm)
{
    BinderRadioCapsManager* self = g_object_new(RADIO_CAPS_MANAGER_TYPE, 0);

    self->data_manager = binder_data_manager_ref(dm);
    return self;
}

static
void
binder_radio_caps_manager_init(
    BinderRadioCapsManager* self)
{
    self->caps_list = g_ptr_array_new();
    self->order_list = g_ptr_array_new();
    self->requests = g_ptr_array_new();
    self->tx_phase_index = -1;
    self->idle_pool = gutil_idle_pool_ref
        (gutil_idle_pool_get(&binder_radio_caps_shared_pool));
}

static
void
binder_radio_caps_manager_finalize(
    GObject* object)
{
    BinderRadioCapsManager* self = RADIO_CAPS_MANAGER(object);

    GASSERT(!self->caps_list->len);
    GASSERT(!self->order_list->len);
    GASSERT(!self->requests->len);
    g_ptr_array_free(self->caps_list, TRUE);
    g_ptr_array_free(self->order_list, TRUE);
    g_ptr_array_free(self->requests, TRUE);
    if (self->check_id) {
        g_source_remove(self->check_id);
    }
    binder_data_manager_unref(self->data_manager);
    gutil_idle_pool_unref(self->idle_pool);
    G_OBJECT_CLASS(binder_radio_caps_manager_parent_class)->finalize(object);
}

static
void
binder_radio_caps_manager_class_init(
    BinderRadioCapsManagerClass* klass)
{
    GType type = G_OBJECT_CLASS_TYPE(klass);

    G_OBJECT_CLASS(klass)->finalize = binder_radio_caps_manager_finalize;
    binder_radio_caps_manager_signals[CAPS_MANAGER_SIGNAL_ABORTED] =
        g_signal_new(CAPS_MANAGER_SIGNAL_ABORTED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    binder_radio_caps_manager_signals[CAPS_MANAGER_SIGNAL_TX_DONE] =
        g_signal_new(CAPS_MANAGER_SIGNAL_TX_DONE_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * binder_radio_caps_request
 *==========================================================================*/

BinderRadioCapsRequest*
binder_radio_caps_request_new(
    BinderRadioCaps* pub,
    enum ofono_radio_access_mode modes,
    enum ofono_slot_data_role role)
{
    BinderRadioCapsRequest* req = NULL;
    BinderRadioCapsObject* caps = binder_radio_caps_cast(pub);

    if (caps) {
        BinderRadioCapsManager *mgr = pub->mgr;

        DBG_(caps, "%s %s (0x%02x)",
            binder_radio_caps_manager_role_str(pub->mgr, role),
            ofono_radio_access_mode_to_string(modes), modes);
        req = g_slice_new(BinderRadioCapsRequest);
        binder_radio_caps_object_ref(req->caps = caps);
        req->modes = modes;
        req->role = role;
        g_ptr_array_add(mgr->requests, req);
        binder_radio_caps_manager_consider_requests(mgr);
    }
    return req;
}

void
binder_radio_caps_request_free(
    BinderRadioCapsRequest* req)
{
    if (req) {
        /* In case if g_object_unref frees the caps */
        BinderRadioCapsManager* mgr =
            binder_radio_caps_manager_ref(req->caps->pub.mgr);

        DBG_(req->caps, "%s (%s)",
            binder_radio_caps_manager_role_str(mgr, req->role),
            ofono_radio_access_mode_to_string(req->modes));
        g_ptr_array_remove(mgr->requests, req);
        binder_radio_caps_object_unref(req->caps);
        gutil_slice_free(req);
        binder_radio_caps_manager_consider_requests(mgr);
        binder_radio_caps_manager_unref(mgr);
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
