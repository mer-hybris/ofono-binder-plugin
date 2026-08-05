/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *  Copyright (C) 2021-2022 Jolla Ltd.
 *  Copyright (C) 2025 Slava Monich <slava@monich.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "binder_base.h"
#include "binder_data.h"
#include "binder_radio.h"
#include "binder_network.h"
#include "binder_sim_settings.h"
#include "binder_util.h"
#include "binder_log.h"

#include <ofono/log.h>

#include <radio_client.h>
#include <radio_config.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <radio_data_types.h>
#include <radio_network_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_ints.h>
#include <gutil_intarray.h>
#include <gutil_strv.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

/* Yes, it does sometimes take minutes in roaming */
#define SETUP_DATA_CALL_TIMEOUT (300*1000) /* ms */

typedef enum binder_data_flags {
    BINDER_DATA_FLAG_NONE = 0x00,
    BINDER_DATA_FLAG_ALLOWED = 0x01,
    BINDER_DATA_FLAG_MAX_SPEED = 0x02,
    BINDER_DATA_FLAG_ON = 0x04
} BINDER_DATA_FLAGS;

/*
 * How it works:
 *
 * This code implements "one data SIM at a time" model. It will have
 * to be updated to support multiple data SIMs active simultanously.
 *
 * There's one binder_data per slot.
 *
 * BINDER_DATA_FLAG_ALLOWED is set for the last SIM for which
 * binder_data_allow() was called with non-zero role. No more
 * than one SIM at a time has this flag set.
 *
 * BINDER_DATA_FLAG_MAX_SPEED is set for the last SIM for which
 * binder_data_allow() was called with OFONO_SLOT_DATA_INTERNET.
 * No more than one SIM at a time has this flag set.
 *
 * BINDER_DATA_FLAG_ON is set for the active SIM after setDataAllowed
 * has successfully completed.
 *
 * Each binder_data object has a request queue which serializes
 * setDataAllowed, setupDataCall and deactivateDataCall requests
 * for the associated SIM.
 *
 * setDataAllowed isn't sent to the selected data SIM until all
 * requests are finished for the other SIM.
 *
 * Power on is requested with binder_radio_power_on while data is allowed or
 * any requests are pending for the SIM. Once data is disallowed and all
 * requests are finished, power is released with binder_radio_power_off.
 */

enum binder_data_io_event_id {
    IO_EVENT_RESTRICTED_STATE_CHANGED,
    IO_EVENT_DATA_CALL_LIST_CHANGED,
    IO_EVENT_DATA_CALL_LIST_CHANGED_1,
    IO_EVENT_DATA_CALL_LIST_CHANGED_2, /* See data_call_list_changed_ind */
    IO_EVENT_DEATH,
    IO_EVENT_COUNT
};

enum binder_data_settings_event_id {
    SETTINGS_EVENT_IMSI_CHANGED,
    SETTINGS_EVENT_PREF_MODE,
    SETTINGS_EVENT_COUNT
};

struct binder_data_manager {
    gint refcount;
    GSList* data_list;
    enum binder_data_manager_flags flags;
    RadioConfig* rc;
    RadioRequest* phone_cap_req;
    GUtilInts* modem_ids;
    enum ofono_radio_access_mode non_data_mode;
};

typedef struct binder_data_api BinderDataApi;
typedef struct binder_data_object {
    BinderBase base;
    BinderData pub;
    RadioRequestGroup* g;
    const BinderDataApi* api;
    BinderRadio* radio;
    BinderNetwork* network;
    BinderDataManager* dm;

    BINDER_DATA_FLAGS flags;
    RADIO_RESTRICTED_STATE restricted_state;

    BinderDataRequest* req_queue;
    BinderDataRequest* pending_req;

    BinderDataOptions options;
    BinderDataProfileConfig profile_config;
    guint slot;
    char* log_prefix;
    RadioClient* network_client;
    RadioRequest* query_req;
    gulong io_event_id[IO_EVENT_COUNT];
    gulong settings_event_id[SETTINGS_EVENT_COUNT];
    gulong network_client_restricted_state_change_id;
    GHashTable* grab;
    gboolean downgraded_tech; /* Status 55 workaround */
} BinderDataObject;

typedef BinderBaseClass BinderDataObjectClass;
GType binder_data_object_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderDataObject, binder_data_object, BINDER_TYPE_BASE)
#define PARENT_CLASS binder_data_object_parent_class
#define THIS_TYPE binder_data_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, BinderDataObject)
BINDER_BASE_ASSERT_COUNT(BINDER_DATA_PROPERTY_COUNT);

typedef enum binder_data_request_flags {
    DATA_REQUEST_NO_FLAGS = 0,
    DATA_REQUEST_FLAG_COMPLETED = 0x1,
    DATA_REQUEST_FLAG_SUBMISSION_FAILURE = 0x2,
    DATA_REQUEST_FLAG_CANCEL_WHEN_ALLOWED = 0x4,
    DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED = 0x8
} BINDER_DATA_REQUEST_FLAGS;

struct binder_data_request {
    BinderDataRequest* next;
    BinderDataObject* data;
    union binder_data_request_cb {
        BinderDataCallSetupFunc setup;
        BinderDataCallDeactivateFunc deact;
        void (*ptr)();
    } cb;
    void* arg;
    gboolean (*submit)(BinderDataRequest* dr);
    void (*cancel)(BinderDataRequest* dr);
    void (*free)(BinderDataRequest* dr);
    RadioRequest* radio_req;
    BINDER_DATA_REQUEST_FLAGS flags;
    const char* name;
};

typedef struct binder_data_request_setup {
    BinderDataRequest req;
    guint profile_id;
    char* apn;
    char* username;
    char* password;
    enum ofono_gprs_proto proto;
    enum ofono_gprs_auth_method auth_method;
    guint retry_count;
    guint retry_delay_id;
} BinderDataRequestSetup;

typedef struct binder_data_request_deact {
    BinderDataRequest req;
    int cid;
} BinderDataRequestDeact;

typedef struct binder_data_request_allow_data {
    BinderDataRequest req;
    gboolean allow;
} BinderDataRequestAllowData;

struct binder_data_api {
    const char* name;
    RADIO_REQ set_data_allowed_req;
    RADIO_REQ get_data_call_list_req;
    GSList* (*read_get_data_call_list_resp)(
        GBinderReader* reader,
        RADIO_RESP resp);
    RADIO_REQ setup_data_call_req;
    void (*write_setup_data_call_args)(
        GBinderWriter* args,
        const BinderDataRequestSetup* setup,
        const BinderDataProfileConfig* config,
        RADIO_TECH tech);
    BinderDataCall* (*read_setup_data_call_resp)(
        GBinderReader* reader,
        RADIO_RESP resp);
    RADIO_REQ deactivate_data_call_req;
    void (*write_deactivate_data_call_args)(
        GBinderWriter* args,
        int cid);
    RADIO_IND network_restricted_state_changed_ind;
    RADIO_IND restricted_state_changed_ind;
    RADIO_IND data_call_list_changed_ind[3];
    GSList* (*read_data_call_list_changed_ind)(
        GBinderReader* reader,
        RADIO_IND ind);
};

/* Data type descriptors */

/*
 * typedef struct radio_data_profile {
 *     RADIO_DATA_PROFILE_ID profileId;
 *     GBinderHidlString apn;
 *     GBinderHidlString protocol;
 *     GBinderHidlString roamingProtocol;
 *     RADIO_APN_AUTH_TYPE authType;
 *     GBinderHidlString user;
 *     GBinderHidlString password;
 *     RADIO_DATA_PROFILE_TYPE type;
 *     gint32 maxConnsTime;
 *     gint32 maxConns;
 *     gint32 waitTime;
 *     guint8 enabled;
 *     RADIO_APN_TYPES supportedApnTypesBitmap;
 *     RADIO_ACCESS_FAMILY bearerBitmap;
 *     gint32 mtu;
 *     gint32 mvnoType;
 *     GBinderHidlString mvnoMatchData;
 * } RadioDataProfile;
 */
static const GBinderWriterField binder_data_profile_f[] = {
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile,apn),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile,protocol),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile,roamingProtocol),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile,user),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile,password),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile,mvnoMatchData),
    GBINDER_WRITER_FIELD_END()
};
const GBinderWriterType binder_data_profile_type = {
    GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioDataProfile),
    binder_data_profile_f
};

/*
 * typedef struct radio_data_profile_1_4 {
 *     RADIO_DATA_PROFILE_ID profileId;
 *     GBinderHidlString apn;
 *     RADIO_PDP_PROTOCOL_TYPE protocol;
 *     RADIO_PDP_PROTOCOL_TYPE roamingProtocol;
 *     RADIO_APN_AUTH_TYPE authType;
 *     GBinderHidlString user;
 *     GBinderHidlString password;
 *     RADIO_DATA_PROFILE_TYPE type;
 *     gint32 maxConnsTime;
 *     gint32 maxConns;
 *     gint32 waitTime;
 *     guint8 enabled;
 *     RADIO_APN_TYPES supportedApnTypesBitmap;
 *     RADIO_ACCESS_FAMILY bearerBitmap;
 *     gint32 mtu;
 *     guint8 preferred;
 *     guint8 persistent;
 * } RadioDataProfile_1_4;
 */
static const GBinderWriterField binder_data_profile_1_4_f[] = {
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile_1_4,apn),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile_1_4,user),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile_1_4,password),
    GBINDER_WRITER_FIELD_END()
};
const GBinderWriterType binder_data_profile_1_4_type = {
    GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioDataProfile_1_4),
    binder_data_profile_1_4_f
};

/*
 * typedef struct radio_data_profile_1_5 {
 *     RADIO_DATA_PROFILE_ID profileId;
 *     GBinderHidlString apn;
 *     RADIO_PDP_PROTOCOL_TYPE protocol;
 *     RADIO_PDP_PROTOCOL_TYPE roamingProtocol;
 *     RADIO_APN_AUTH_TYPE authType;
 *     GBinderHidlString user;
 *     GBinderHidlString password;
 *     RADIO_DATA_PROFILE_TYPE type;
 *     gint32 maxConnsTime;
 *     gint32 maxConns;
 *     gint32 waitTime;
 *     guint8 enabled;
 *     RADIO_APN_TYPES supportedApnTypesBitmap;
 *     RADIO_ACCESS_FAMILY bearerBitmap;
 *     gint32 mtuV4;
 *     gint32 mtuV6;
 *     guint8 preferred;
 *     guint8 persistent;
 * } RadioDataProfile_1_5;
 */
static const GBinderWriterField binder_data_profile_1_5_f[] = {
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile_1_5,apn),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile_1_5,user),
    GBINDER_WRITER_FIELD_HIDL_STRING(RadioDataProfile_1_5,password),
    GBINDER_WRITER_FIELD_END()
};
const GBinderWriterType binder_data_profile_1_5_type = {
    GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioDataProfile_1_5),
    binder_data_profile_1_5_f
};

static struct ofono_debug_desc binder_data_debug_desc OFONO_DEBUG_ATTR = {
    .file = __FILE__,
    .flags = OFONO_DEBUG_FLAG_DEFAULT,
};

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderDataObject* binder_data_cast(BinderData* data)
    { return data ? THIS(G_CAST(data, BinderDataObject, pub)) : NULL; }

static void binder_data_manager_check_network_mode(BinderDataManager* dm);
static void binder_data_call_deact_cid(BinderDataObject* data, int cid);
static void binder_data_cancel_all_requests(BinderDataObject* data);
static void binder_data_power_update(BinderDataObject* data);

static
guint8
binder_data_modem_id(
    BinderDataObject* data)
{
    if (data) {
        guint count = 0;
        BinderDataManager* dm = data->dm;
        const int* ids = gutil_ints_get_data(dm->modem_ids, &count);

        if (data->slot < count) {
            return (guint8)ids[data->slot];
        }
        return (guint8)data->slot;
    }
    return 0;
}

static
RadioRequest*
binder_data_object_deactivate_data_call_request_new(
    BinderDataObject* self,
    int cid,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    const BinderDataApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        api->deactivate_data_call_req, &args, complete, destroy, user_data);

    api->write_deactivate_data_call_args(&args, cid);
    return req;
}

static
RadioRequest*
binder_data_object_set_data_allowed_request_new(
    BinderDataObject* self,
    gboolean allow,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        self->api->set_data_allowed_req, &args, complete, destroy, user_data);

    /*
     * IRadio.hal:
     * oneway setDataAllowed(int32_t serial, bool allow);
     *
     * IRadioData.aidl:
     * void setDataAllowed(in int serial, in boolean allow);
     */
    gbinder_writer_append_bool(&args, allow);
    return req;
}

/*==========================================================================*
 * BinderDataCall
 *==========================================================================*/

static
BinderDataCall*
binder_data_call_new()
{
    return g_new0(struct binder_data_call, 1);
}

/* extern */
BinderDataCall*
binder_data_call_dup(
    const BinderDataCall* call)
{
    if (call) {
        BinderDataCall* dc = binder_data_call_new();

        dc->cid = call->cid;
        dc->status = call->status;
        dc->active = call->active;
        dc->prot = call->prot;
        dc->retry_time = call->retry_time;
        dc->mtu = call->mtu;
        dc->ifname = g_strdup(call->ifname);
        dc->dnses = g_strdupv(call->dnses);
        dc->gateways = g_strdupv(call->gateways);
        dc->addresses = g_strdupv(call->addresses);
        dc->pcscf = g_strdupv(call->pcscf);
        return dc;
    }
    return NULL;
}

static
void
binder_data_call_destroy(
    BinderDataCall* call)
{
    g_free(call->ifname);
    g_strfreev(call->dnses);
    g_strfreev(call->gateways);
    g_strfreev(call->addresses);
    g_strfreev(call->pcscf);
}

/* extern */
void
binder_data_call_free(
    BinderDataCall* call)
{
    if (call) {
        binder_data_call_destroy(call);
        g_free(call);
    }
}

static
void
binder_data_call_list_free(
    GSList* calls)
{
    g_slist_free_full(calls, (GDestroyNotify) binder_data_call_free);
}

static
gint
binder_data_call_compare(
    gconstpointer a,
    gconstpointer b)
{
    const BinderDataCall* ca = a;
    const BinderDataCall* cb = b;

    return ca->cid - cb->cid;
}

static
gboolean
binder_data_call_equal(
    const BinderDataCall* c1,
    const BinderDataCall* c2)
{
    if (c1 == c2) {
        return TRUE;
    } else if (c1 && c2) {
        return c1->cid == c2->cid &&
            c1->status == c2->status &&
            c1->active == c2->active &&
            c1->prot == c2->prot &&
            c1->retry_time == c2->retry_time &&
            c1->mtu == c2->mtu &&
            !g_strcmp0(c1->ifname, c2->ifname) &&
            gutil_strv_equal(c1->dnses, c2->dnses) &&
            gutil_strv_equal(c1->gateways, c2->gateways) &&
            gutil_strv_equal(c1->addresses, c2->addresses) &&
            gutil_strv_equal(c1->pcscf, c2->pcscf);
    } else {
        return FALSE;
    }
}

static
gboolean
binder_data_call_list_equal(
    GSList* l1,
    GSList* l2)
{
    while (l1 && l2) {
        if (!binder_data_call_equal(l1->data, l2->data)) {
            return FALSE;
        }
        l1 = l1->next;
        l2 = l2->next;
    }
    return (!l1 && !l2);
}

static
gboolean
binder_data_call_list_contains(
    GSList* l,
    const BinderDataCall* call)
{
    while (l) {
        if (binder_data_call_equal(l->data, call)) {
            return TRUE;
        }
        l = l->next;
    }
    return FALSE;
}

/* extern */
BinderDataCall*
binder_data_call_find(
    GSList* l,
    int cid)
{
    while (l) {
        BinderDataCall* call = l->data;

        if (call->cid == cid) {
            return call;
        }
        l = l->next;
    }
    return NULL;
}

static
void
binder_data_set_calls(
    BinderDataObject* self,
    GSList* list)
{
    BinderBase* base = &self->base;
    BinderData* data = &self->pub;
    GHashTableIter it;
    gpointer key;

    if (binder_data_call_list_equal(data->calls, list)) {
        binder_data_call_list_free(list);
    } else {
        DBG("data calls changed");
        binder_data_call_list_free(data->calls);
        data->calls = list;
        binder_base_queue_property_change(base, BINDER_DATA_PROPERTY_CALLS);
    }

    /* Clean up the grab table */
    g_hash_table_iter_init(&it, self->grab);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        const int cid = GPOINTER_TO_INT(key);

        if (!binder_data_call_find(data->calls, cid)) {
            g_hash_table_iter_remove(&it);
        }
    }

    if (data->calls) {
        GSList* l;

        /* Disconnect stray calls (one at a time) */
        for (l = data->calls; l; l = l->next) {
            const BinderDataCall* dc = l->data;

            key = GINT_TO_POINTER(dc->cid);
            if (!g_hash_table_contains(self->grab, key)) {
                DBG_(self, "stray call %u", dc->cid);
                if (dc->cid > 0) {
                    binder_data_call_deact_cid(self, dc->cid);
                }
                break;
            }
        }
    }

    binder_base_emit_queued_signals(base);
}

static
gboolean
binder_data_is_allowed(
    BinderDataObject* data)
{
    return G_LIKELY(data) &&
        (data->restricted_state & RADIO_RESTRICTED_STATE_PS_ALL) == 0 &&
        (data->flags & (BINDER_DATA_FLAG_ALLOWED | BINDER_DATA_FLAG_ON)) ==
                       (BINDER_DATA_FLAG_ALLOWED | BINDER_DATA_FLAG_ON);
}

static
void
binder_data_check_allowed(
    BinderDataObject* data,
    gboolean was_allowed)
{
    if (binder_data_is_allowed(data) != was_allowed) {
        binder_base_queue_property_change(&data->base,
        BINDER_DATA_PROPERTY_ALLOWED);
    }
}

static
gboolean
binder_data_manager_set_preferred_data_modem_allowed(
    BinderDataManager* dm)
{
    return dm &&
        (radio_config_interface_type(dm->rc) == RADIO_INTERFACE_TYPE_AIDL ||
         (radio_config_interface_type(dm->rc) == RADIO_INTERFACE_TYPE_HIDL &&
          radio_config_interface(dm->rc) >= RADIO_CONFIG_INTERFACE_1_1));
}

static
void
binder_data_restricted_state_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataObject* data = THIS(user_data);
    GBinderReader reader;
    gint32 state;

    /*
     * IRadioIndication.hal:
     * oneway restrictedStateChanged(RadioIndicationType type,
     *     PhoneRestrictedState state);
     *
     * IRadioNetworkIndication.aidl:
     * oneway void restrictedStateChanged(in RadioIndicationType type,
     *     in PhoneRestrictedState state);
     */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &state)) {
        if (data->restricted_state != state) {
            const gboolean was_allowed = binder_data_is_allowed(data);

            DBG_(data, "restricted state 0x%02x", state);
            data->restricted_state = state;
            binder_data_check_allowed(data, was_allowed);
        }
    }
}

static
void
binder_data_call_list_apply(
    BinderDataObject* data,
    GSList* list)
{
    if (data->query_req) {
        /* We have received change event before query has completed */
        DBG_(data, "cancelling query");
        radio_request_drop(data->query_req);
        data->query_req = NULL;
    }

    binder_data_set_calls(data, list);
}

static
void
binder_data_call_list_changed(
    RadioClient* client,
    RADIO_IND ind,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataObject* data = THIS(user_data);
    GBinderReader reader;

    gbinder_reader_copy(&reader, args);
    binder_data_call_list_apply(data,
        data->api->read_data_call_list_changed_ind(&reader, ind));
}

static
void
binder_data_query_data_calls_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataObject* data = THIS(user_data);
    GSList* list = NULL;

    GASSERT(data->query_req == req);
    radio_request_unref(data->query_req);
    data->query_req = NULL;

    /*
     * Only RADIO_ERROR_NONE and RADIO_ERROR_RADIO_NOT_AVAILABLE are expected,
     * all other errors are filtered out by binder_data_poll_call_state_retry()
     */
    if (status != RADIO_TX_STATUS_OK) {
        DBG_(data, "getDataCallList tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG_(data, "getDataCallList error %s",
            binder_radio_error_string(error));
    } else {
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        list = data->api->read_get_data_call_list_resp(&reader, resp);
    }
    binder_data_set_calls(data, list);
}

/*==========================================================================*
 * BinderDataRequest
 *==========================================================================*/

static
void
binder_data_request_free(
    BinderDataRequest* dr)
{
    if (dr->free) {
        dr->free(dr);
    } else {
        g_free(dr);
    }
}

/* extern */
void
binder_data_request_detach(
    BinderDataRequest* dr)
{
    if (dr) {
        dr->cb.ptr = NULL;
        dr->arg = NULL;
    }
}

static
gboolean
binder_data_request_call(
    BinderDataRequest* dr,
    RadioRequest* req)
{
    GASSERT(!dr->radio_req);
    radio_request_drop(dr->radio_req);
    if (radio_request_submit(req)) {
        dr->radio_req = req; /* Keep the ref */
        return TRUE;
    } else {
        radio_request_drop(req);
        dr->radio_req = NULL;
        dr->flags |= DATA_REQUEST_FLAG_SUBMISSION_FAILURE;
        return FALSE;
    }
}

static
void
binder_data_request_cancel_io(
    BinderDataRequest* dr)
{
    if (dr->radio_req) {
        radio_request_drop(dr->radio_req);
        dr->radio_req = NULL;
    }
}

static
void
binder_data_request_submit_next(
    BinderDataObject* data)
{
    if (!data->pending_req) {
        int submission_failure = 0;

        binder_data_power_update(data);
        while (data->req_queue) {
            BinderDataRequest* dr = data->req_queue;

            GASSERT(dr->data == data);
            data->req_queue = dr->next;
            dr->next = NULL;

            data->pending_req = dr;
            if (dr->submit(dr)) {
                DBG_(data, "submitted %s request %p", dr->name, dr);
                break;
            } else {
                DBG_(data, "%s request %p done (or failed)", dr->name, dr);
                data->pending_req = NULL;
                if (dr->flags & DATA_REQUEST_FLAG_SUBMISSION_FAILURE) {
                    submission_failure++;
                }
                binder_data_request_free(dr);
            }
        }

        if (!data->pending_req && !submission_failure) {
            binder_data_manager_check_data(data->dm);
        }
    }
    binder_data_power_update(data);
}

static
gboolean
binder_data_request_do_cancel(
    BinderDataRequest* dr)
{
    if (dr && !(dr->flags & DATA_REQUEST_FLAG_COMPLETED)) {
        BinderDataObject* data = dr->data;

        DBG_(data, "canceling %s request %p", dr->name, dr);
        if (dr->cancel) {
            dr->cancel(dr);
        }
        if (data->pending_req == dr) {
            /* Request has been submitted already */
            data->pending_req = NULL;
        } else if (data->req_queue == dr) {
            /* It's the first one in the queue */
            data->req_queue = dr->next;
        } else {
            /* It's somewhere in the queue */
            BinderDataRequest* prev = data->req_queue;

            while (prev->next && prev->next != dr) {
                prev = prev->next;
            }

            /* Assert that it's there */
            GASSERT(prev);
            if (prev) {
                prev->next = dr->next;
            }
        }

        binder_data_request_free(dr);
        return TRUE;
    } else {
        return FALSE;
    }
}

/* extern */
void
binder_data_request_cancel(
    BinderDataRequest* dr)
{
    if (dr) {
        BinderDataObject* data = dr->data;

        if (binder_data_request_do_cancel(dr)) {
            binder_data_request_submit_next(data);
        }
    }
}

static
void
binder_data_request_completed(
    BinderDataRequest* dr)
{
    GASSERT(!(dr->flags & DATA_REQUEST_FLAG_COMPLETED));
    dr->flags |= DATA_REQUEST_FLAG_COMPLETED;
}

static
void
binder_data_request_finish(
    BinderDataRequest* dr)
{
    BinderDataObject* data = dr->data;

    GASSERT(dr == data->pending_req);
    GASSERT(!dr->next);
    data->pending_req = NULL;

    binder_data_request_free(dr);
    binder_data_request_submit_next(data);
}

static
void
binder_data_request_queue(
    BinderDataRequest* dr)
{
    BinderDataObject* data = dr->data;

    dr->next = NULL;

    if (!data->req_queue) {
        data->req_queue = dr;
    } else {
        BinderDataRequest* last = data->req_queue;

        while (last->next) {
            last = last->next;
        }
        last->next = dr;
    }

    DBG_(data, "queued %s request %p", dr->name, dr);
    binder_data_request_submit_next(data);
}

/*==========================================================================*
 * BinderDataRequestSetup
 *==========================================================================*/

static
RADIO_APN_AUTH_TYPE
binder_data_call_setup_auth(
    const BinderDataRequestSetup* setup)
{
    return (setup->username && setup->username[0]) ?
        binder_radio_auth_from_ofono_method(setup->auth_method) :
        RADIO_APN_AUTH_NONE;
}

static
void
binder_data_call_setup_cancel(
    BinderDataRequest* dr)
{
    BinderDataRequestSetup* setup = G_CAST(dr, BinderDataRequestSetup, req);

    binder_data_request_cancel_io(dr);
    gutil_source_clear(&setup->retry_delay_id);
    if (dr->cb.setup) {
        BinderDataCallSetupFunc cb = dr->cb.setup;

        dr->cb.setup = NULL;
        cb(&dr->data->pub, RADIO_ERROR_CANCELLED, NULL, dr->arg);
    }
}

static
gboolean
binder_data_call_setup_retry(
    gpointer user_data)
{
    BinderDataRequestSetup* setup = user_data;
    BinderDataRequest* dr = &setup->req;

    GASSERT(setup->retry_delay_id);
    setup->retry_delay_id = 0;
    setup->retry_count++;
    DBG("silent retry %u out of %u", setup->retry_count,
        dr->data->options.data_call_retry_limit);
    dr->submit(dr);
    return G_SOURCE_REMOVE;
}

static
gboolean
binder_data_call_retry(
    BinderDataRequestSetup* setup)
{
    BinderDataRequest* dr = &setup->req;
    const BinderDataOptions* options = &dr->data->options;

    if (setup->retry_count < options->data_call_retry_limit) {
        binder_data_request_cancel_io(dr);
        GASSERT(!setup->retry_delay_id);
        if (!setup->retry_count) {
            /* No delay first time */
            setup->retry_count++;
            DBG("silent retry %u out of %u", setup->retry_count,
                options->data_call_retry_limit);
            dr->submit(dr);
        } else {
            const guint ms = options->data_call_retry_delay_ms;

            DBG("silent retry scheduled in %u ms", ms);
            setup->retry_delay_id = g_timeout_add(ms,
                binder_data_call_setup_retry, setup);
        }
        return TRUE;
    }
    return FALSE;
}

static
void
binder_data_call_setup_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataRequestSetup* setup = user_data;
    BinderDataRequest* dr = &setup->req;
    BinderDataObject* self = dr->data;
    BinderDataCall* call = NULL;
    BinderData* data = &self->pub;
    BinderBase* base = &self->base;
    BinderDataCall* free_call = NULL;

    GASSERT(dr->radio_req == req);
    radio_request_unref(dr->radio_req);
    dr->radio_req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "setupDataCall tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG_(self, "setupDataCall error %s",
            binder_radio_error_string(error));
    } else {
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        call = self->api->read_setup_data_call_resp(&reader, resp);
    }

    if (call) {
        BinderNetwork* network = self->network;

        switch (call->status) {
        case RADIO_DATA_CALL_FAIL_UNSPECIFIED:
            /*
             * First time we retry immediately and if that doesn't work,
             * then after certain delay.
             */
            if (binder_data_call_retry(setup)) {
                binder_data_call_free(call);
                return;
            }
            break;
        case RADIO_DATA_CALL_FAIL_MULTI_CONN_TO_SAME_PDN_NOT_ALLOWED:
            /*
             * With some networks we sometimes start getting error 55
             * (Multiple PDN connections for a given APN not allowed)
             * when trying to setup an LTE data call and this error
             * doesn't go away until we successfully establish a data
             * call over 3G. Then we can switch back to LTE.
             */
            if (network->data.access_tech == OFONO_ACCESS_TECHNOLOGY_EUTRAN &&
                !self->downgraded_tech) {
                DBG("downgrading preferred technology");
                self->downgraded_tech = TRUE;
                binder_data_manager_check_network_mode(self->dm);
                /* And let this call fail */
            }
            break;
        default:
            break;
        }
    }

    binder_data_request_completed(dr);
    free_call = call;

    if (call && call->status == RADIO_DATA_CALL_FAIL_NONE) {
        if (self->downgraded_tech) {
            DBG("done with status 55 workaround");
            self->downgraded_tech = FALSE;
            binder_data_manager_check_network_mode(self->dm);
        }

        if (!binder_data_call_list_contains(data->calls, call)) {
            data->calls = g_slist_insert_sorted(data->calls, call,
                binder_data_call_compare);
            DBG_(self, "new data call");
            binder_base_queue_property_change(base, BINDER_DATA_PROPERTY_CALLS);
            free_call = NULL;
        }
    }

    if (dr->cb.setup) {
        dr->cb.setup(data, error, call, dr->arg);
    }

    binder_data_request_finish(dr);
    binder_data_call_free(free_call);
    binder_base_emit_queued_signals(base);
}

static
gboolean
binder_data_call_setup_submit(
    BinderDataRequest* dr)
{
    BinderDataRequestSetup* setup = G_CAST(dr, BinderDataRequestSetup, req);
    BinderDataObject* data = dr->data;
    const BinderDataApi* api = data->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(data->g, api->setup_data_call_req,
        &args, binder_data_call_setup_cb, NULL, setup);

    api->write_setup_data_call_args(&args, setup, &data->profile_config,
        (setup->profile_id == RADIO_DATA_PROFILE_IMS) ? RADIO_TECH_LTE :
        data->network->data.radio_tech);
    return binder_data_request_call(dr, req);
}

static
void
binder_data_call_setup_free(
    BinderDataRequest* dr)
{
    BinderDataRequestSetup* setup = G_CAST(dr, BinderDataRequestSetup, req);

    g_free(setup->apn);
    g_free(setup->username);
    g_free(setup->password);
    g_free(setup);
}

static
BinderDataRequest*
binder_data_call_setup_new(
    BinderDataObject* data,
    const struct ofono_gprs_primary_context* ctx,
    enum ofono_gprs_context_type context_type,
    BinderDataCallSetupFunc cb,
    void* arg)
{
    const BinderDataProfileConfig* dpc = &data->profile_config;
    BinderDataRequestSetup* setup = g_new0(BinderDataRequestSetup, 1);
    BinderDataRequest* dr = &setup->req;

    if (dpc->use_data_profiles) {
        setup->profile_id = dpc->default_profile_id;
        switch (context_type) {
        case OFONO_GPRS_CONTEXT_TYPE_MMS:
            setup->profile_id = dpc->mms_profile_id;
            break;
        case OFONO_GPRS_CONTEXT_TYPE_IMS:
            setup->profile_id = RADIO_DATA_PROFILE_IMS;
            break;
        case OFONO_GPRS_CONTEXT_TYPE_ANY:
        case OFONO_GPRS_CONTEXT_TYPE_INTERNET:
        case OFONO_GPRS_CONTEXT_TYPE_WAP:
            /* Leave the default value untouched */
            break;
        }
    } else {
        setup->profile_id = RADIO_DATA_PROFILE_INVALID;
    }

    setup->apn = g_strdup(ctx->apn);
    setup->username = g_strdup(ctx->username);
    setup->password = g_strdup(ctx->password);
    setup->proto = ctx->proto;
    setup->auth_method = ctx->auth_method;

    dr->name = "CALL_SETUP";
    dr->cb.setup = cb;
    dr->arg = arg;
    dr->data = data;
    dr->submit = binder_data_call_setup_submit;
    dr->cancel = binder_data_call_setup_cancel;
    dr->free = binder_data_call_setup_free;
    dr->flags = DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED;
    return dr;
}

/*==========================================================================*
 * BinderDataRequestDeact
 *==========================================================================*/

static
void
binder_data_call_deact_cancel(
    BinderDataRequest* dr)
{
    binder_data_request_cancel_io(dr);
    if (dr->cb.deact) {
        BinderDataCallDeactivateFunc cb = dr->cb.deact;

        dr->cb.deact = NULL;
        cb(&dr->data->pub, RADIO_ERROR_CANCELLED, dr->arg);
    }
}

static
void
binder_data_call_deact_cb(
    RadioRequest* ioreq,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataRequestDeact* deact = user_data;
    BinderDataRequest* dr = &deact->req;
    BinderDataObject* self = dr->data;
    BinderData* data = &self->pub;
    BinderBase* base = &self->base;
    BinderDataCall* call = NULL;

    GASSERT(dr->radio_req == ioreq);
    radio_request_unref(dr->radio_req);
    dr->radio_req = NULL;

    binder_data_request_completed(dr);

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "deactivateDataCall tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG_(self, "deactivateDataCall(%d) error %s", deact->cid,
            binder_radio_error_string(error));
    } else {
        /*
         * If RADIO_REQ_DEACTIVATE_DATA_CALL succeeds, some adaptations
         * don't send dataCallListChanged even though the list of calls
         * has changed. Update the list of calls to account for that.
         */
        call = binder_data_call_find(data->calls, deact->cid);
        if (call) {
            DBG_(self, "removing call %d", deact->cid);
            data->calls = g_slist_remove(data->calls, call);
        }
    }

    if (call) {
        binder_data_call_free(call);
        binder_base_emit_property_change(base, BINDER_DATA_PROPERTY_CALLS);
    } else {
        /* Something seems to be slightly broken, request the current state */
        binder_data_poll_call_state(data);
    }

    if (dr->cb.deact) {
        dr->cb.deact(data, error, dr->arg);
    }

    binder_data_request_finish(dr);
}

static
gboolean
binder_data_call_deact_submit(
    BinderDataRequest* dr)
{
    BinderDataRequestDeact* deact = G_CAST(dr, BinderDataRequestDeact, req);

    return binder_data_request_call(dr,
        binder_data_object_deactivate_data_call_request_new(dr->data,
            deact->cid, binder_data_call_deact_cb, NULL, deact));
}

static
BinderDataRequest*
binder_data_call_deact_new(
    BinderDataObject* data,
    int cid,
    BinderDataCallDeactivateFunc cb,
    void* arg)
{
    BinderDataRequestDeact* deact = g_new0(BinderDataRequestDeact, 1);
    BinderDataRequest* dr = &deact->req;

    deact->cid = cid;
    dr->cb.deact = cb;
    dr->arg = arg;
    dr->data = data;
    dr->submit = binder_data_call_deact_submit;
    dr->cancel = binder_data_call_deact_cancel;
    dr->name = "DEACTIVATE";
    return dr;
}

static
void
binder_data_call_deact_cid(
    BinderDataObject* data,
    int cid)
{
    binder_data_request_queue(binder_data_call_deact_new(data, cid,
        NULL, NULL));
}

/*==========================================================================*
 * setPreferredDataModem (IRadioConfig >= 1.1)
 *==========================================================================*/

static
void
binder_data_set_preferred_data_modem_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_CONFIG_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataRequest* dr = user_data;
    BinderDataObject* data = dr->data;

    GASSERT(dr->radio_req == req);
    radio_request_unref(dr->radio_req);
    dr->radio_req = NULL;

    binder_data_request_completed(dr);

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            guint32 code =
                radio_config_interface_type(data->dm->rc) == RADIO_INTERFACE_TYPE_AIDL ?
                    RADIO_CONFIG_AIDL_RESP_SET_PREFERRED_DATA_MODEM :
                    RADIO_CONFIG_RESP_SET_PREFERRED_DATA_MODEM;
            if (resp == code) {
                const gboolean was_allowed = binder_data_is_allowed(data);

                data->flags |= BINDER_DATA_FLAG_ON;
                DBG_(data, "data on");
                binder_data_check_allowed(data, was_allowed);
            } else {
                ofono_error("Unexpected setPreferredDataModem response %d",
                    resp);
            }
        } else {
            DBG("setPreferredDataModem error %s",
                binder_radio_error_string(error));
        }
    }

    binder_data_request_finish(dr);
}

static
gboolean
binder_data_set_preferred_data_modem_submit(
    BinderDataRequest* dr)
{
    GBinderWriter args;
    BinderDataObject* data = dr->data;
    BinderDataManager* dm = data->dm;
    guint32 code =
        radio_config_interface_type(dm->rc) == RADIO_INTERFACE_TYPE_AIDL ?
            RADIO_CONFIG_AIDL_REQ_SET_PREFERRED_DATA_MODEM :
            RADIO_CONFIG_REQ_SET_PREFERRED_DATA_MODEM;
    RadioRequest* req = radio_config_request_new(dm->rc,
            code, &args,
            binder_data_set_preferred_data_modem_cb, NULL, dr);

    /* setPreferredDataModem(serial, uint8 modemId) */
    if (req) {
        const guint8 modem_id = binder_data_modem_id(data);

        DBG("setPreferredDataModem(%u)", modem_id);
        gbinder_writer_append_int8(&args, modem_id);
        radio_request_set_retry(req, BINDER_RETRY_SECS*1000, -1);
        return binder_data_request_call(dr, req);
    }
    return FALSE;
}

static
BinderDataRequest*
binder_data_set_preferred_data_modem_new(
    BinderDataObject* data)
{
    BinderDataRequest* dr = g_new0(BinderDataRequest, 1);

    dr->name = "SET_PREFERRED_DATA_MODEM";
    dr->data = data;
    dr->submit = binder_data_set_preferred_data_modem_submit;
    dr->cancel = binder_data_request_cancel_io;
    dr->flags = DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED;
    return dr;
}

/*==========================================================================*
 * BinderDataRequestAllowData
 *==========================================================================*/

static
void
binder_data_allow_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataRequestAllowData* ad = user_data;
    BinderDataRequest* dr = &ad->req;
    BinderDataObject* data = dr->data;

    GASSERT(dr->radio_req == req);
    radio_request_unref(dr->radio_req);
    dr->radio_req = NULL;

    binder_data_request_completed(dr);

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(data, "setDataAllowed tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG_(data, "setDataAllowed error %s",
            binder_radio_error_string(error));
    } else {
        const gboolean was_allowed = binder_data_is_allowed(data);

        if (ad->allow) {
            data->flags |= BINDER_DATA_FLAG_ON;
            DBG_(data, "data on");
        } else {
            data->flags &= ~BINDER_DATA_FLAG_ON;
            DBG_(data, "data off");
        }

        binder_data_check_allowed(data, was_allowed);
    }

    binder_data_request_finish(dr);
}

static
gboolean
binder_data_allow_submit(
    BinderDataRequest* dr)
{
    BinderDataRequestAllowData* ad =
        G_CAST(dr, BinderDataRequestAllowData, req);
    RadioRequest* req =
        binder_data_object_set_data_allowed_request_new(dr->data,
            ad->allow, binder_data_allow_cb, NULL, ad);

    radio_request_set_retry(req, BINDER_RETRY_SECS*1000, -1);
    radio_request_set_blocking(req, TRUE);
    return binder_data_request_call(dr, req);
}

static
BinderDataRequest*
binder_data_allow_new(
    BinderDataObject* data,
    gboolean allow)
{
    BinderDataRequestAllowData* ad = g_new0(BinderDataRequestAllowData, 1);
    BinderDataRequest* dr = &ad->req;

    dr->name = "ALLOW_DATA";
    dr->data = data;
    dr->submit = binder_data_allow_submit;
    dr->cancel = binder_data_request_cancel_io;
    dr->flags = DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED;
    ad->allow = allow;
    return dr;
}

static
gboolean
binder_data_allow_can_submit(
    BinderDataObject* data)
{
    if (data) {
        switch (data->options.allow_data) {
        case BINDER_ALLOW_DATA_ENABLED:
            return TRUE;
        case BINDER_ALLOW_DATA_DISABLED:
            break;
        }
    }
    return FALSE;
}

static
gboolean
binder_data_allow_submit_request(
    BinderDataObject* data,
    gboolean allow)
{
    if (binder_data_manager_need_set_data_allowed(data->dm)) {
        if (binder_data_allow_can_submit(data)) {
            binder_data_request_queue(binder_data_allow_new(data, allow));
            return TRUE;
        }
    } else if (allow && binder_data_manager_set_preferred_data_modem_allowed(data->dm)) {
        binder_data_request_queue(binder_data_set_preferred_data_modem_new(data));
    }
    return FALSE;
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static
BinderDataCall*
binder_data_call_new_1_0(
    const RadioDataCall* dc)
{
    BinderDataCall* call = binder_data_call_new();

    call->cid = dc->cid;
    call->status = dc->status;
    call->active = dc->active;
    call->prot = binder_ofono_proto_from_proto_str(dc->type.data.str);
    call->retry_time = dc->suggestedRetryTime;
    call->mtu = dc->mtu;
    call->ifname = g_strdup(dc->ifname.data.str);
    call->dnses = g_strsplit(dc->dnses.data.str, " ", -1);
    call->gateways = g_strsplit(dc->gateways.data.str, " ", -1);
    call->addresses = g_strsplit(dc->addresses.data.str, " ", -1);
    call->pcscf = g_strsplit(dc->pcscf.data.str, " ", -1);

    DBG("[status=%d,retry=%d,cid=%d,active=%d,type=%s,ifname=%s,"
        "mtu=%d,address=%s,dns=%s,gateways=%s,pcscf=%s]",
        call->status, call->retry_time, call->cid, call->active,
        dc->type.data.str, call->ifname, call->mtu, dc->addresses.data.str,
        dc->dnses.data.str, dc->gateways.data.str, dc->pcscf.data.str);
    return call;
}

static
GSList*
binder_data_call_list_1_0(
    const RadioDataCall* calls,
    gsize n)
{
    if (n) {
        gsize i;
        GSList* l = NULL;

        DBG("num=%u", (guint) n);
        for (i = 0; i < n; i++) {
            l = g_slist_insert_sorted(l, binder_data_call_new_1_0(calls + i),
                binder_data_call_compare);
        }
        return l;
    } else {
        DBG("no data calls");
        return NULL;
    }
}

static
GSList*
binder_data_api_read_get_data_call_list_resp_hidl_1_0(
   GBinderReader* reader,
   RADIO_RESP resp)
{
    if (resp == RADIO_RESP_GET_DATA_CALL_LIST) {
        gsize count = 0;
        const RadioDataCall* calls =
            gbinder_reader_read_hidl_type_vec(reader,
                RadioDataCall, &count);

        return binder_data_call_list_1_0(calls, count);
    } else {
        ofono_error("Unexpected getDataCallList response %d", resp);
        return NULL;
    }
}

static
RadioDataProfile*
binder_data_write_radio_data_profile_new(
    GBinderWriter* writer,
    const BinderDataRequestSetup* setup,
    const BinderDataProfileConfig* config)
{
    RadioDataProfile* dp = gbinder_writer_new0(writer, RadioDataProfile);

    dp->profileId = setup->profile_id;
    binder_copy_hidl_string(writer, &dp->apn, setup->apn);
    binder_copy_hidl_string(writer, &dp->protocol,
        binder_proto_str_from_ofono_proto(setup->proto));
    dp->roamingProtocol = dp->protocol;
    dp->authType = binder_data_call_setup_auth(setup);;
    binder_copy_hidl_string(writer, &dp->user, setup->username);
    binder_copy_hidl_string(writer, &dp->password, setup->password);
    dp->enabled = TRUE;
    dp->supportedApnTypesBitmap =
        binder_radio_apn_types_for_profile(setup->profile_id, config);
    binder_copy_hidl_string(writer, &dp->mvnoMatchData, NULL);
    return dp;
}

static
void
binder_data_api_write_setup_data_call_args_hidl_1_0(
    GBinderWriter* args,
    const BinderDataRequestSetup* setup,
    const BinderDataProfileConfig* config,
    RADIO_TECH tech)
{
    /*
     * 1.0/IRadio.hal:
     * oneway setupDataCall(int32_t serial, RadioTechnology radioTechnology,
     *     DataProfileInfo dataProfileInfo, bool modemCognitive,
     *     bool roamingAllowed, bool isRoaming);
     */
    gbinder_writer_append_int32(args, tech); /* radioTechnology */
    gbinder_writer_append_struct(args,       /* dataProfileInfo */
        binder_data_write_radio_data_profile_new(args, setup, config),
            &binder_data_profile_type, NULL);
    gbinder_writer_append_bool(args, FALSE); /* modemCognitive */
    gbinder_writer_append_bool(args, TRUE);  /* roamingAllowed */
    gbinder_writer_append_bool(args, FALSE); /* isRoaming */
}

static
BinderDataCall*
binder_data_api_read_setup_data_call_resp_hidl_1_0(
    GBinderReader* reader,
    RADIO_RESP resp)
{
    if (resp == RADIO_RESP_SETUP_DATA_CALL) {
        const RadioDataCall* dc =
            gbinder_reader_read_hidl_struct(reader, RadioDataCall);

        if (dc) {
            return binder_data_call_new_1_0(dc);
        }
    } else {
        ofono_error("Unexpected setupDataCall response %d", resp);
    }
    return NULL;
}

static
void
binder_data_api_write_deactivate_data_call_args_hidl(
    GBinderWriter* args,
    int cid)
{
    /*
     * 1.0/IRadio.hal:
     * oneway deactivateDataCall(int32_t serial, int32_t cid,
     *     bool reasonRadioShutDown);
     */
    gbinder_writer_append_int32(args, cid);
    gbinder_writer_append_bool(args, FALSE);
}

static
GSList*
binder_data_api_read_data_call_list_changed_ind_hidl(
    GBinderReader* reader,
    RADIO_IND ind)
{
    if (ind == RADIO_IND_DATA_CALL_LIST_CHANGED) {
        gsize count = 0;
        const RadioDataCall* calls =
            gbinder_reader_read_hidl_type_vec(reader,
                RadioDataCall, &count);

        return binder_data_call_list_1_0(calls, count);
    } else {
        ofono_error("Unexpected dataCallListChanged indication %d", ind);
        return NULL;
    }
}

static const BinderDataApi binder_data_api_hidl = {
    "hidl",
    RADIO_REQ_SET_DATA_ALLOWED,
    RADIO_REQ_GET_DATA_CALL_LIST,
    binder_data_api_read_get_data_call_list_resp_hidl_1_0,
    RADIO_REQ_SETUP_DATA_CALL,
    binder_data_api_write_setup_data_call_args_hidl_1_0,
    binder_data_api_read_setup_data_call_resp_hidl_1_0,
    RADIO_REQ_DEACTIVATE_DATA_CALL,
    binder_data_api_write_deactivate_data_call_args_hidl,
    RADIO_IND_NONE,
    RADIO_IND_RESTRICTED_STATE_CHANGED,
    {
        RADIO_IND_DATA_CALL_LIST_CHANGED,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_4,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_5
    },
    binder_data_api_read_data_call_list_changed_ind_hidl
};

/*==========================================================================*
 * HIDL 1.2 API flavor
 *==========================================================================*/

static
void
binder_data_api_write_setup_data_call_args_hidl_1_2(
    GBinderWriter* args,
    const BinderDataRequestSetup* setup,
    const BinderDataProfileConfig* config,
    RADIO_TECH tech)
{
    const char* nothing = NULL;

    /*
     * 1.2/IRadio.hal:
     * oneway setupDataCall_1_2(int32_t serial, AccessNetwork accessNetwork,
     *     DataProfileInfo dataProfileInfo, bool modemCognitive,
     *     bool roamingAllowed, bool isRoaming, DataRequestReason reason,
     *     vec<string> addresses, vec<string> dnses);
     */
    gbinder_writer_append_int32(args,        /* accessNetwork */
        binder_radio_access_network_for_tech(tech));
    gbinder_writer_append_struct(args,       /* dataProfileInfo */
        binder_data_write_radio_data_profile_new(args, setup, config),
            &binder_data_profile_type, NULL);
    gbinder_writer_append_bool(args, FALSE); /* modemCognitive */
    gbinder_writer_append_bool(args, TRUE);  /* roamingAllowed */
    gbinder_writer_append_bool(args, FALSE); /* isRoaming */
    gbinder_writer_append_int32(args,        /* reason */
        RADIO_DATA_REQUEST_REASON_NORMAL);
    gbinder_writer_append_hidl_string_vec(args, &nothing, 0); /* addresses */
    gbinder_writer_append_hidl_string_vec(args, &nothing, 0); /* dnses */
}

static
void
binder_data_api_write_deactivate_data_call_args_hidl_1_2(
    GBinderWriter* args,
    int cid)
{
    /*
     * 1.2/IRadio.hal:
     * oneway deactivateDataCall_1_2(int32_t serial, int32_t cid,
     *     DataRequestReason reason);
     */
    gbinder_writer_append_int32(args, cid);
    gbinder_writer_append_bool(args, FALSE);
}

static const BinderDataApi binder_data_api_hidl_1_2 = {
    "hidl_1_2",
    RADIO_REQ_SET_DATA_ALLOWED,
    RADIO_REQ_GET_DATA_CALL_LIST,
    binder_data_api_read_get_data_call_list_resp_hidl_1_0,
    RADIO_REQ_SETUP_DATA_CALL_1_2,
    binder_data_api_write_setup_data_call_args_hidl_1_2,
    binder_data_api_read_setup_data_call_resp_hidl_1_0,
    RADIO_REQ_DEACTIVATE_DATA_CALL_1_2,
    binder_data_api_write_deactivate_data_call_args_hidl_1_2,
    RADIO_IND_NONE,
    RADIO_IND_RESTRICTED_STATE_CHANGED,
    {
        RADIO_IND_DATA_CALL_LIST_CHANGED,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_4,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_5
    },
    binder_data_api_read_data_call_list_changed_ind_hidl
};

/*==========================================================================*
 * HIDL 1.4 API flavor
 *==========================================================================*/

static
BinderDataCall*
binder_data_call_new_1_4(
    const RadioDataCall_1_4* dc)
{
    BinderDataCall* call = binder_data_call_new();

    call->cid = dc->cid;
    call->status = dc->cause;
    call->active = dc->active;
    call->prot = dc->type;
    call->retry_time = dc->suggestedRetryTime;
    call->mtu = dc->mtu;
    call->ifname = g_strdup(dc->ifname.data.str);
    call->dnses = binder_strv_from_hidl_string_vec(&dc->dnses);
    call->gateways = binder_strv_from_hidl_string_vec(&dc->gateways);
    call->addresses = binder_strv_from_hidl_string_vec(&dc->addresses);
    call->pcscf = binder_strv_from_hidl_string_vec(&dc->pcscf);

    DBG("[status=%d,retry=%d,cid=%d,active=%d,type=%d,ifname=%s,"
        "mtu=%d,address=%s,dns=%s,gateways=%s,pcscf=%s]",
        call->status, call->retry_time, call->cid, call->active,
        dc->type, call->ifname, call->mtu,
        binder_print_strv(call->addresses, " "),
        binder_print_strv(call->dnses, " "),
        binder_print_strv(call->gateways, " "),
        binder_print_strv(call->pcscf, " "));
    return call;
}

static
GSList*
binder_data_call_list_1_4(
    const RadioDataCall_1_4* calls,
    gsize n)
{
    if (n) {
        gsize i;
        GSList* l = NULL;

        DBG("num=%u", (guint) n);
        for (i = 0; i < n; i++) {
            l = g_slist_insert_sorted(l, binder_data_call_new_1_4(calls + i),
                binder_data_call_compare);
        }
        return l;
    } else {
        DBG("no data calls");
        return NULL;
    }
}

static
GSList*
binder_data_api_read_get_data_call_list_resp_hidl_1_4(
   GBinderReader* reader,
   RADIO_RESP resp)
{
    if (resp == RADIO_RESP_GET_DATA_CALL_LIST_1_4) {
        gsize count = 0;
        const RadioDataCall_1_4* calls =
            gbinder_reader_read_hidl_type_vec(reader,
                RadioDataCall_1_4, &count);

        return binder_data_call_list_1_4(calls, count);
    } else {
        return binder_data_api_read_get_data_call_list_resp_hidl_1_0(reader,
            resp);
    }
}

static
void
binder_data_api_write_setup_data_call_args_hidl_1_4(
    GBinderWriter* args,
    const BinderDataRequestSetup* setup,
    const BinderDataProfileConfig* config,
    RADIO_TECH tech)
{
    const char* nothing = NULL;
    RadioDataProfile_1_4* dp = gbinder_writer_new0(args, RadioDataProfile_1_4);

    /*
     * 1.4/IRadio.hal:
     * oneway setupDataCall_1_4(int32_t serial, AccessNetwork accessNetwork,
     *     DataProfileInfo dataProfileInfo, bool roamingAllowed,
     *     DataRequestReason reason, vec<string> addresses,
     *     vec<string> dnses);
     */

    gbinder_writer_append_int32(args, /* accessNetwork */
        binder_radio_access_network_for_tech(tech));

    /* dataProfileInfo */
    dp->profileId = RADIO_DATA_PROFILE_INVALID;
    binder_copy_hidl_string(args, &dp->apn, setup->apn);
    dp->protocol = dp->roamingProtocol =
        binder_proto_from_ofono_proto(setup->proto);
    dp->authType = binder_data_call_setup_auth(setup);
    binder_copy_hidl_string(args, &dp->user, setup->username);
    binder_copy_hidl_string(args, &dp->password, setup->password);
    dp->enabled = TRUE;
    dp->supportedApnTypesBitmap =
        binder_radio_apn_types_for_profile(setup->profile_id, config);
    gbinder_writer_append_struct(args, dp, &binder_data_profile_1_4_type, NULL);

    gbinder_writer_append_bool(args, TRUE); /* roamingAllowed */
    gbinder_writer_append_int32(args,       /* reason */
        RADIO_DATA_REQUEST_REASON_NORMAL);
    gbinder_writer_append_hidl_string_vec(args, &nothing, 0); /* addresses */
    gbinder_writer_append_hidl_string_vec(args, &nothing, 0); /* dnses */
}

static
BinderDataCall*
binder_data_api_read_setup_data_call_resp_hidl_1_4(
    GBinderReader* reader,
    RADIO_RESP resp)
{
    if (resp == RADIO_RESP_SETUP_DATA_CALL_1_4) {
        const RadioDataCall_1_4* dc =
            gbinder_reader_read_hidl_struct(reader, RadioDataCall_1_4);

        if (dc) {
            return binder_data_call_new_1_4(dc);
        }
    } else {
        return binder_data_api_read_setup_data_call_resp_hidl_1_0(reader,
            resp);
    }
    return NULL;
}

static
GSList*
binder_data_api_read_data_call_list_changed_ind_hidl_1_4(
    GBinderReader* reader,
    RADIO_IND ind)
{
    if (ind == RADIO_IND_DATA_CALL_LIST_CHANGED_1_4) {
        gsize count = 0;
        const RadioDataCall_1_4* calls =
            gbinder_reader_read_hidl_type_vec(reader,
                RadioDataCall_1_4, &count);

        return binder_data_call_list_1_4(calls, count);
    } else {
        return binder_data_api_read_data_call_list_changed_ind_hidl(reader,
            ind);
    }
}

static const BinderDataApi binder_data_api_hidl_1_4 = {
    "hidl_1_4",
    RADIO_REQ_SET_DATA_ALLOWED,
    RADIO_REQ_GET_DATA_CALL_LIST,
    binder_data_api_read_get_data_call_list_resp_hidl_1_4,
    RADIO_REQ_SETUP_DATA_CALL_1_4,
    binder_data_api_write_setup_data_call_args_hidl_1_4,
    binder_data_api_read_setup_data_call_resp_hidl_1_4,
    RADIO_REQ_DEACTIVATE_DATA_CALL_1_2,
    binder_data_api_write_deactivate_data_call_args_hidl_1_2,
    RADIO_IND_NONE,
    RADIO_IND_RESTRICTED_STATE_CHANGED,
    {
        RADIO_IND_DATA_CALL_LIST_CHANGED,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_4,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_5
    },
    binder_data_api_read_data_call_list_changed_ind_hidl_1_4
};

/*==========================================================================*
 * HIDL 1.5 API flavor
 *==========================================================================*/

static
BinderDataCall*
binder_data_call_new_1_5(
    const RadioDataCall_1_5* dc)
{
    BinderDataCall* call = binder_data_call_new();

    call->cid = dc->cid;
    call->status = dc->cause;
    call->active = dc->active;
    call->prot = dc->type;
    call->retry_time = dc->suggestedRetryTime;
    call->mtu = dc->mtuV4;
    call->ifname = g_strdup(dc->ifname.data.str);
    call->dnses = binder_strv_from_hidl_string_vec(&dc->dnses);
    call->gateways = binder_strv_from_hidl_string_vec(&dc->gateways);
    call->addresses = binder_strv_from_hidl_string_vec(&dc->addresses);
    call->pcscf = binder_strv_from_hidl_string_vec(&dc->pcscf);

    DBG("[status=%d,retry=%d,cid=%d,active=%d,type=%d,ifname=%s,"
        "mtu=%d,address=%s,dns=%s,gateways=%s,pcscf=%s]",
        call->status, call->retry_time, call->cid, call->active,
        dc->type, call->ifname, call->mtu,
        binder_print_strv(call->addresses, " "),
        binder_print_strv(call->dnses, " "),
        binder_print_strv(call->gateways, " "),
        binder_print_strv(call->pcscf, " "));
    return call;
}

static
GSList*
binder_data_call_list_1_5(
    const RadioDataCall_1_5* calls,
    gsize n)
{
    if (n) {
        gsize i;
        GSList* l = NULL;

        DBG("num=%u", (guint) n);
        for (i = 0; i < n; i++) {
            l = g_slist_insert_sorted(l, binder_data_call_new_1_5(calls + i),
                binder_data_call_compare);
        }
        return l;
    } else {
        DBG("no data calls");
        return NULL;
    }
}

static
GSList*
binder_data_api_read_get_data_call_list_resp_hidl_1_5(
   GBinderReader* reader,
   RADIO_RESP resp)
{
    if (resp == RADIO_RESP_GET_DATA_CALL_LIST_1_5) {
        gsize count = 0;
        const RadioDataCall_1_5* calls =
            gbinder_reader_read_hidl_type_vec(reader,
                RadioDataCall_1_5, &count);

        return binder_data_call_list_1_5(calls, count);
    } else {
        return binder_data_api_read_get_data_call_list_resp_hidl_1_4(reader,
            resp);
    }
}

static
void
binder_data_api_write_setup_data_call_args_hidl_1_5(
    GBinderWriter* args,
    const BinderDataRequestSetup* setup,
    const BinderDataProfileConfig* config,
    RADIO_TECH tech)
{
    const char* nothing = NULL;
    RadioDataProfile_1_5* dp = gbinder_writer_new0(args, RadioDataProfile_1_5);

    /*
     * 1.5/IRadio.hal:
     * oneway setupDataCall_1_5(int32_t serial, AccessNetwork accessNetwork,
     *     DataProfileInfo dataProfileInfo, bool roamingAllowed,
     *     DataRequestReason reason, vec<LinkAddress> addresses,
     *     vec<string> dnses);
     */
    gbinder_writer_append_int32(args, /* accessNetwork */
        binder_radio_access_network_for_tech(tech));

    /* dataProfileInfo */
    dp->profileId = RADIO_DATA_PROFILE_INVALID;
    binder_copy_hidl_string(args, &dp->apn, setup->apn);
    dp->protocol = dp->roamingProtocol =
        binder_proto_from_ofono_proto(setup->proto);
    dp->authType = binder_data_call_setup_auth(setup);
    binder_copy_hidl_string(args, &dp->user, setup->username);
    binder_copy_hidl_string(args, &dp->password, setup->password);
    dp->enabled = TRUE;
    dp->supportedApnTypesBitmap =
        binder_radio_apn_types_for_profile(setup->profile_id, config);
    gbinder_writer_append_struct(args, dp, &binder_data_profile_1_5_type, NULL);

    gbinder_writer_append_bool(args, TRUE); /* roamingAllowed */
    gbinder_writer_append_int32(args,       /* reason */
        RADIO_DATA_REQUEST_REASON_NORMAL);
    gbinder_writer_append_hidl_string_vec(args, &nothing, 0); /* addresses */
    gbinder_writer_append_hidl_string_vec(args, &nothing, 0); /* dnses */
}

static
BinderDataCall*
binder_data_api_read_setup_data_call_resp_hidl_1_5(
    GBinderReader* reader,
    RADIO_RESP resp)
{
    if (resp == RADIO_RESP_SETUP_DATA_CALL_1_5) {
        const RadioDataCall_1_5* dc =
            gbinder_reader_read_hidl_struct(reader, RadioDataCall_1_5);

        if (dc) {
            return  binder_data_call_new_1_5(dc);
        }
    } else {
        return binder_data_api_read_setup_data_call_resp_hidl_1_4(reader,
            resp);
    }
    return NULL;
}

static
GSList*
binder_data_api_read_data_call_list_changed_ind_hidl_1_5(
    GBinderReader* reader,
    RADIO_IND ind)
{
    if (ind == RADIO_IND_DATA_CALL_LIST_CHANGED_1_5) {
        gsize count = 0;
        const RadioDataCall_1_5* calls =
            gbinder_reader_read_hidl_type_vec(reader,
                RadioDataCall_1_5, &count);

        return binder_data_call_list_1_5(calls, count);
    } else {
        return binder_data_api_read_data_call_list_changed_ind_hidl_1_4(reader,
            ind);
    }
}

static const BinderDataApi binder_data_api_hidl_1_5 = {
    "hidl_1_5",
    RADIO_REQ_SET_DATA_ALLOWED,
    RADIO_REQ_GET_DATA_CALL_LIST,
    binder_data_api_read_get_data_call_list_resp_hidl_1_5,
    RADIO_REQ_SETUP_DATA_CALL_1_5,
    binder_data_api_write_setup_data_call_args_hidl_1_5,
    binder_data_api_read_setup_data_call_resp_hidl_1_5,
    RADIO_REQ_DEACTIVATE_DATA_CALL_1_2,
    binder_data_api_write_deactivate_data_call_args_hidl_1_2,
    RADIO_IND_NONE,
    RADIO_IND_RESTRICTED_STATE_CHANGED,
    {
        RADIO_IND_DATA_CALL_LIST_CHANGED,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_4,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_5
    },
    binder_data_api_read_data_call_list_changed_ind_hidl_1_5
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static
BinderDataCall*
binder_data_call_new_aidl(
    GBinderReader* reader)
{
    BinderDataCall* call = NULL;
    GBinderReader parcel;

    /*
     * package android.hardware.radio.data;
     * parcelable SetupDataCallResult {
     *   DataCallFailCause cause;
     *   long suggestedRetryTime;
     *   int cid;
     *   int active;
     *   PdpProtocolType type;
     *   String ifname;
     *   LinkAddress[] addresses;
     *   String[] dnses;
     *   String[] gateways;
     *   String[] pcscf;
     *   int mtuV4;
     *   int mtuV6;
     *   Qos defaultQos;
     *   QosSession[] qosSessions;
     *   byte handoverFailureMode;
     *   int pduSessionId;
     *   @nullable SliceInfo sliceInfo;
     *   TrafficDescriptor[] trafficDescriptors;
     * }
     */

    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        gint64 retry_time;
        gint32 n;

        call = binder_data_call_new();
        gbinder_reader_read_int32(&parcel, &call->status);
        gbinder_reader_read_int64(&parcel, &retry_time);

        /* Is there better way to do this? */
        if (retry_time == G_MAXINT64) {
            call->retry_time = G_MAXINT32;
        } else if (retry_time < 0) {
            call->retry_time = -1;
        } else {
            call->retry_time = (retry_time & 0xffffffff);
        }

        gbinder_reader_read_int32(&parcel, &call->cid);
        gbinder_reader_read_uint32(&parcel, &call->active);
        gbinder_reader_read_uint32(&parcel, &call->prot);
        call->ifname = gbinder_reader_read_string16(&parcel);

        /* addresses */
        if (gbinder_reader_read_int32(&parcel, &n)) {
            char** ptr = (call->addresses = g_new(char*, MAX(n, 0) + 1));
            int i;

            for (i = 0; i < n; i++) {
                GBinderReader address;

                /*
                 * package android.hardware.radio.data;
                 * parcelable LinkAddress {
                 *   String address;
                 *   int addressProperties;
                 *   long deprecationTime;
                 *   long expirationTime;
                 * }
                 */
                if (gbinder_reader_start_parcelable(&parcel, &address, NULL)) {
                    char* str = gbinder_reader_read_string16(&address);

                    *ptr++ = str ? str : g_strdup("");
                    gbinder_reader_finish_parcelable(&address);
                }
            }
            *ptr = NULL;

            call->dnses = binder_strv_from_string16_array(&parcel);
            call->gateways = binder_strv_from_string16_array(&parcel);
            call->pcscf = binder_strv_from_string16_array(&parcel);
            gbinder_reader_read_int32(&parcel, &call->mtu); /* mtuV4 */

            DBG("[status=%d,retry=%d,cid=%d,active=%d,type=%d,ifname=%s,"
                "mtu=%d,address=%s,dns=%s,gateways=%s,pcscf=%s]",
                call->status, call->retry_time, call->cid, call->active,
                call->prot, call->ifname, call->mtu,
                binder_print_strv(call->addresses, " "),
                binder_print_strv(call->dnses, " "),
                binder_print_strv(call->gateways, " "),
                binder_print_strv(call->pcscf, " "));
        } else {
            binder_data_call_free(call);
            call = NULL;
        }
        gbinder_reader_finish_parcelable(&parcel);
    }
    return call;
}

static
GSList*
binder_data_call_list_aidl(
    GBinderReader* reader)
{
    gint32 n;
    gbinder_reader_read_int32(reader, &n);
    if (n > 0) {
        gsize i;
        GSList* l = NULL;

        DBG("num=%u", (guint) n);
        for (i = 0; i < n; i++) {
            BinderDataCall* call = binder_data_call_new_aidl(reader);

            if (call) {
                l = g_slist_insert_sorted(l, call, binder_data_call_compare);
            }
        }
        return l;
    } else {
        DBG("no data calls");
        return NULL;
    }
}

static
GSList*
binder_data_api_read_get_data_call_list_resp_aidl(
   GBinderReader* reader,
   RADIO_RESP resp)
{
    /*
     * IRadioDataResponse.aidl:
     * void getDataCallListResponse(in RadioResponseInfo info,
     *     in SetupDataCallResult[] dcResponse);
     */
    return binder_data_call_list_aidl(reader);
}

static
void
binder_data_api_write_setup_data_call_args_aidl(
    GBinderWriter* args,
    const BinderDataRequestSetup* setup,
    const BinderDataProfileConfig* config,
    RADIO_TECH tech)
{
    GBinderWriter dp, td;
    RADIO_PDP_PROTOCOL_TYPE pt = binder_proto_from_ofono_proto(setup->proto);

    /*
     * IRadioData.aidl:
     * void setupDataCall(in int serial, in AccessNetwork accessNetwork,
     *     in DataProfileInfo dataProfileInfo, in boolean roamingAllowed,
     *     in DataRequestReason reason, in LinkAddress[] addresses,
     *     in String[] dnses, in int pduSessionId,
     *     in @nullable SliceInfo sliceInfo, in boolean matchAllRuleAllowed);
     */
    gbinder_writer_append_int32(args,  /* accessNetwork */
        binder_radio_access_network_for_tech(tech));

    /* dataProfileInfo */
    /*
     * package android.hardware.radio.data;
     * parcelable DataProfileInfo {
     *   int profileId;
     *   String apn;
     *   PdpProtocolType protocol;
     *   PdpProtocolType roamingProtocol;
     *   ApnAuthType authType;
     *   String user;
     *   String password;
     *   int type;
     *   int maxConnsTime;
     *   int maxConns;
     *   int waitTime;
     *   boolean enabled;
     *   int supportedApnTypesBitmap;
     *   int bearerBitmap;
     *   int mtuV4;
     *   int mtuV6;
     *   boolean preferred;
     *   boolean persistent;
     *   boolean alwaysOn;
     *   TrafficDescriptor trafficDescriptor;
     *   int infrastructureBitmap; // Since v3
     * }
     *
     * parcelable TrafficDescriptor {
     *   @nullable String dnn;
     *   @nullable OsAppId osAppId;
     * }
     */
    gbinder_writer_start_parcelable(args, &dp);
    gbinder_writer_append_int32(&dp, RADIO_DATA_PROFILE_INVALID);/* profileId */
    gbinder_writer_append_string16(&dp, setup->apn); /* apn */
    gbinder_writer_append_int32(&dp, pt);   /* protocol */
    gbinder_writer_append_int32(&dp, pt);   /* roamingProtocol */
    gbinder_writer_append_int32(&dp,        /* authType*/
        binder_data_call_setup_auth(setup));
    gbinder_writer_append_string16(&dp, setup->username); /* user */
    gbinder_writer_append_string16(&dp, setup->password); /* password */
    gbinder_writer_append_int32(&dp, 0);    /* type */
    gbinder_writer_append_int32(&dp, 0);    /* maxConnsTime */
    gbinder_writer_append_int32(&dp, 0);    /* maxConns */
    gbinder_writer_append_int32(&dp, 0);    /* waitTime */
    gbinder_writer_append_bool(&dp, TRUE);  /* enabled */
    gbinder_writer_append_int32(&dp,        /* supportedApnTypesBitmap */
        binder_radio_apn_types_for_profile(setup->profile_id, config));
    gbinder_writer_append_int32(&dp, 0);    /* bearerBitmap */
    gbinder_writer_append_int32(&dp, 0);    /* mtuV4 */
    gbinder_writer_append_int32(&dp, 0);    /* mtuV6 */
    gbinder_writer_append_bool(&dp, FALSE); /* preferred */
    gbinder_writer_append_bool(&dp, FALSE); /* persistent */
    gbinder_writer_append_bool(&dp, FALSE); /* alwaysOn */

    gbinder_writer_start_parcelable(&dp, &td); /* trafficDescriptor */
    gbinder_writer_append_string16(&td, NULL);   /* trafficDescriptor.dnn */
    gbinder_writer_append_null_parcelable(&td);  /* trafficDescriptor.osAppId */
    gbinder_writer_finish_parcelable(&td);
    gbinder_writer_finish_parcelable(&dp);

    gbinder_writer_append_bool(args, TRUE);      /* roamingAllowed */
    gbinder_writer_append_int32(args,            /* reason */
        RADIO_DATA_REQUEST_REASON_NORMAL);
    gbinder_writer_append_int32(args, 0);        /* addresses [0] */
    gbinder_writer_append_int32(args, 0);        /* dnses [0] */
    gbinder_writer_append_int32(args, 0);        /* pduSessionId */
    gbinder_writer_append_null_parcelable(args); /* sliceInfo */
    gbinder_writer_append_bool(args, FALSE);     /* matchAllRuleAllowed */
}

static
BinderDataCall*
binder_data_api_read_setup_data_call_resp_aidl(
    GBinderReader* reader,
    RADIO_RESP resp)
{
    return binder_data_call_new_aidl(reader);
}

static
void
binder_data_api_write_deactivate_data_call_args_aidl(
    GBinderWriter* args,
    int cid)
{
    /*
     * IRadioData.aidl:
     * void deactivateDataCall(in int serial, in int cid,
     *     in DataRequestReason reason);
     */
    gbinder_writer_append_int32(args, cid);
    gbinder_writer_append_int32(args, RADIO_DATA_REQUEST_REASON_NORMAL);
}

static
GSList*
binder_data_api_read_data_call_list_changed_ind_aidl(
    GBinderReader* reader,
    RADIO_IND ind)
{
    return binder_data_call_list_aidl(reader);
}

static const BinderDataApi binder_data_api_aidl = {
    "aidl",
    RADIO_DATA_REQ_SET_DATA_ALLOWED,
    RADIO_DATA_REQ_GET_DATA_CALL_LIST,
    binder_data_api_read_get_data_call_list_resp_aidl,
    RADIO_DATA_REQ_SETUP_DATA_CALL,
    binder_data_api_write_setup_data_call_args_aidl,
    binder_data_api_read_setup_data_call_resp_aidl,
    RADIO_DATA_REQ_DEACTIVATE_DATA_CALL,
    binder_data_api_write_deactivate_data_call_args_aidl,
    RADIO_NETWORK_IND_RESTRICTED_STATE_CHANGED,
    RADIO_IND_NONE,
    { RADIO_DATA_IND_DATA_CALL_LIST_CHANGED, },
    binder_data_api_read_data_call_list_changed_ind_aidl
};

/*==========================================================================*
 * BinderDataObject
 *==========================================================================*/

static
enum ofono_radio_access_mode
binder_data_max_allowed_modes(
    BinderDataObject* data)
{
    return data->downgraded_tech ?
        OFONO_RADIO_ACCESS_UMTS_MASK :
        OFONO_RADIO_ACCESS_MODE_ALL;
}

gulong
binder_data_add_property_handler(
    BinderData* data,
    BINDER_DATA_PROPERTY property,
    BinderDataPropertyFunc callback,
    void* user_data)
{
    BinderDataObject* self = binder_data_cast(data);

    return G_LIKELY(self) ? binder_base_add_property_handler(&self->base,
        property, G_CALLBACK(callback), user_data) : 0;
}

void
binder_data_remove_handler(
    BinderData* data,
    gulong id)
{
    if (G_LIKELY(id)) {
        BinderDataObject* self = binder_data_cast(data);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

static
void
binder_data_imsi_changed(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data)
{
    BinderDataObject* data = THIS(user_data);

    GASSERT(property == BINDER_SIM_SETTINGS_PROPERTY_IMSI);
    if (!settings->imsi) {
        /*
         * Most likely, SIM removal. In any case, no data requests
         * make sense when IMSI is unavailable.
         */
        binder_data_cancel_all_requests(data);
    }
    binder_data_manager_check_network_mode(data->dm);
}

static
void
binder_data_pref_changed(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data)
{
    GASSERT(property == BINDER_SIM_SETTINGS_PROPERTY_PREF);
    binder_data_manager_check_network_mode(THIS(user_data)->dm);
}

static
void
binder_data_client_dead_cb(
    RadioClient* client,
    void* user_data)
{
    BinderDataObject* data = THIS(user_data);

    DBG_(data, "disconnected");
    data->flags = BINDER_DATA_FLAG_NONE;
    data->restricted_state = 0;
    binder_data_cancel_all_requests(data);
}

static
gint
binder_data_compare_cb(
    gconstpointer a,
    gconstpointer b)
{
    const BinderDataObject* d1 = THIS(a);
    const BinderDataObject* d2 = THIS(b);

    return d1->slot < d2->slot ? (-1) : d1->slot > d2->slot ? 1 : 0;
}

BinderData*
binder_data_new(
    BinderDataManager* dm,
    BinderClients* clients,
    const char* name,
    BinderRadio* radio,
    BinderNetwork* network,
    const BinderDataOptions* options,
    const BinderSlotConfig* config,
    struct ofono_slot* slot)
{
    GASSERT(dm);
    if (G_LIKELY(dm)) {
        guint i;
        BinderDataObject* self = g_object_new(THIS_TYPE, NULL);
        BinderData* data = &self->pub;
        BinderSimSettings* settings = network->settings;
        RADIO_INTERFACE hidl = radio_client_interface(clients->data_client);
        const BinderDataApi* api =
            (radio_client_aidl_interface(clients->data_client) ==
             RADIO_DATA_INTERFACE) ? &binder_data_api_aidl :
            (hidl >= RADIO_INTERFACE_1_5) ? &binder_data_api_hidl_1_5 :
            (hidl >= RADIO_INTERFACE_1_4) ? &binder_data_api_hidl_1_4 :
            (hidl >= RADIO_INTERFACE_1_2) ? &binder_data_api_hidl_1_2 :
            &binder_data_api_hidl;

        self->options = *options;
        self->log_prefix = binder_dup_prefix(name);
        self->profile_config = config->data_profile_config;
        self->slot = config->slot;
        self->api = api;
        self->g = radio_request_group_new(clients->data_client);
        self->dm = binder_data_manager_ref(dm);
        self->radio = binder_radio_ref(radio);
        self->network = binder_network_ref(network);
        self->network_client = radio_client_ref(clients->network_client);
        DBG_(self, "%s api", api->name);

        for (i = 0; i < G_N_ELEMENTS(api->data_call_list_changed_ind); i++) {
            if (api->data_call_list_changed_ind[i]) {
                self->io_event_id[IO_EVENT_DATA_CALL_LIST_CHANGED + i] =
                    radio_client_add_indication_handler(clients->data_client,
                        api->data_call_list_changed_ind[i],
                        binder_data_call_list_changed, self);
            } else {
                break;
            }
        }

        if (api->restricted_state_changed_ind) {
            self->io_event_id[IO_EVENT_RESTRICTED_STATE_CHANGED] =
                radio_client_add_indication_handler(clients->data_client,
                    api->restricted_state_changed_ind,
                    binder_data_restricted_state_changed, self);
        }

        if (api->network_restricted_state_changed_ind) {
            self->network_client_restricted_state_change_id =
                radio_client_add_indication_handler(clients->network_client,
                    api->network_restricted_state_changed_ind,
                    binder_data_restricted_state_changed, self);
        }

        self->io_event_id[IO_EVENT_DEATH] =
            radio_client_add_death_handler(clients->data_client,
                binder_data_client_dead_cb, self);

        self->settings_event_id[SETTINGS_EVENT_IMSI_CHANGED] =
            binder_sim_settings_add_property_handler(settings,
                BINDER_SIM_SETTINGS_PROPERTY_IMSI,
                binder_data_imsi_changed, self);
        self->settings_event_id[SETTINGS_EVENT_PREF_MODE] =
            binder_sim_settings_add_property_handler(settings,
                BINDER_SIM_SETTINGS_PROPERTY_PREF,
                binder_data_pref_changed, self);

        /* Request the current state */
        binder_data_poll_call_state(data);

        /* Order data contexts according to slot numbers */
        dm->data_list = g_slist_insert_sorted(dm->data_list, self,
            binder_data_compare_cb);

        #pragma message("TODO: update flags according to the slot role")
        binder_data_manager_check_network_mode(dm);
        return data;
    }
    return NULL;
}

static
gboolean
binder_data_poll_call_state_retry(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    switch (error) {
    case RADIO_ERROR_NONE:
    case RADIO_ERROR_RADIO_NOT_AVAILABLE:
        return FALSE;
    default:
        return TRUE;
    }
}

void
binder_data_poll_call_state(
    BinderData* data)
{
    BinderDataObject* self = binder_data_cast(data);

    if (G_LIKELY(self) && !self->query_req) {
        RadioRequest* req = radio_request_new2(self->g,
            self->api->get_data_call_list_req, NULL,
            binder_data_query_data_calls_cb, NULL, self);

        radio_request_set_retry(req, BINDER_RETRY_SECS*1000, -1);
        radio_request_set_retry_func(req, binder_data_poll_call_state_retry);
        self->query_req = radio_request_try_submit(req);
    }
}

BinderData*
binder_data_ref(
    BinderData* data)
{
    BinderDataObject* self = binder_data_cast(data);

    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return data;
}

void
binder_data_unref(
    BinderData* data)
{
    gutil_object_unref(binder_data_cast(data));
}

gboolean
binder_data_allowed(
    BinderData* data)
{
    return binder_data_is_allowed(binder_data_cast(data));
}

static
void
binder_data_deactivate_all(
    BinderDataObject* self)
{
    GSList* l;

    for (l = self->pub.calls; l; l = l->next) {
        BinderDataCall* call = l->data;

        if (call->status == RADIO_DATA_CALL_FAIL_NONE) {
            DBG_(self, "deactivating call %u", call->cid);
            binder_data_call_deact_cid(self, call->cid);
        }
    }
}

static
void
binder_data_power_update(
    BinderDataObject* self)
{
    if (self->pending_req || self->req_queue) {
        binder_radio_power_on(self->radio, self);
    } else {
        binder_radio_power_off(self->radio, self);
    }
}

static
void
binder_data_cancel_requests(
    BinderDataObject* self,
    BINDER_DATA_REQUEST_FLAGS flags)
{
    BinderDataRequest* dr = self->req_queue;

    while (dr) {
        BinderDataRequest* next = dr->next;

        GASSERT(dr->data == self);
        if (dr->flags & flags) {
            binder_data_request_do_cancel(dr);
        }
        dr = next;
    }

    if (self->pending_req && (self->pending_req->flags & flags)) {
        binder_data_request_cancel(self->pending_req);
    }
}

static
void
binder_data_cancel_all_requests(
    BinderDataObject* self)
{
    BinderDataRequest* dr = self->req_queue;

    binder_data_request_do_cancel(self->pending_req);
    while (dr) {
        BinderDataRequest* next = dr->next;

        binder_data_request_do_cancel(dr);
        dr = next;
    }
}

static
void
binder_data_disallow(
    BinderDataObject* self)
{
    const gboolean was_allowed = binder_data_is_allowed(self);

    DBG_(self, "disallowed");
    GASSERT(self->flags & BINDER_DATA_FLAG_ALLOWED);
    self->flags &= ~BINDER_DATA_FLAG_ALLOWED;

    /*
     * Cancel all requests that can be canceled.
     */
    binder_data_cancel_requests(self, DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED);

    /*
     * Then deactivate active contexts (Hmm... what if deactivate
     * requests are already pending? That's quite unlikely though)
     */
    binder_data_deactivate_all(self);

    /* Tell the modem that the data is now disabled */
    if (!binder_data_allow_submit_request(self, FALSE)) {
        self->flags &= ~BINDER_DATA_FLAG_ON;
        GASSERT(!binder_data_is_allowed(self));
        DBG_(self, "data off");
        binder_data_power_update(self);
    }

    binder_data_check_allowed(self, was_allowed);
}

static
void
binder_data_max_speed_cb(
    gpointer data,
    gpointer max_speed)
{
    if (data != max_speed) {
        THIS(data)->flags &= ~BINDER_DATA_FLAG_MAX_SPEED;
    }
}

static
void
binder_data_disallow_cb(
    gpointer data,
    gpointer allowed)
{
    if (data != allowed) {
        BinderDataObject* obj = THIS(data);

        if (obj->flags & BINDER_DATA_FLAG_ALLOWED) {
            binder_data_disallow(obj);
        }
    }
}

void
binder_data_allow(
    BinderData* data,
    enum ofono_slot_data_role role)
{
    BinderDataObject* self = binder_data_cast(data);
    if (G_LIKELY(self)) {
        BinderDataManager* dm = self->dm;

        DBG_(self, "%s", (role == OFONO_SLOT_DATA_NONE) ? "none" :
            (role == OFONO_SLOT_DATA_MMS) ? "mms" : "internet");

        if (role != OFONO_SLOT_DATA_NONE) {
            gboolean speed_changed = FALSE;

            if (role == OFONO_SLOT_DATA_INTERNET &&
                !(self->flags & BINDER_DATA_FLAG_MAX_SPEED)) {
                self->flags |= BINDER_DATA_FLAG_MAX_SPEED;
                speed_changed = TRUE;

                /* Clear BINDER_DATA_FLAG_MAX_SPEED for all other slots */
                g_slist_foreach(dm->data_list, binder_data_max_speed_cb, self);
            }

            if (self->flags & BINDER_DATA_FLAG_ALLOWED) {
                /*
                 * Data is already allowed for this slot, just adjust
                 * the speed if necessary.
                 */
                if (speed_changed) {
                    binder_data_manager_check_network_mode(dm);
                }
            } else {
                self->flags |= BINDER_DATA_FLAG_ALLOWED;
                self->flags &= ~BINDER_DATA_FLAG_ON;

                /* Clear BINDER_DATA_FLAG_ALLOWED for all other slots */
                g_slist_foreach(dm->data_list, binder_data_disallow_cb, self);
                binder_data_cancel_requests(self,
                    DATA_REQUEST_FLAG_CANCEL_WHEN_ALLOWED);
                binder_data_manager_check_data(dm);
                binder_data_power_update(self);
            }
        } else if (self->flags & BINDER_DATA_FLAG_ALLOWED) {
            binder_data_disallow(self);
            binder_data_manager_check_data(dm);
        }
    }
}

BinderDataRequest*
binder_data_call_setup(
    BinderData* data,
    const struct ofono_gprs_primary_context* ctx,
    enum ofono_gprs_context_type type,
    BinderDataCallSetupFunc cb,
    void* user_data)
{
    BinderDataObject* self = binder_data_cast(data);
    BinderDataRequest* dr = NULL;

    if (self) {
        dr = binder_data_call_setup_new(self, ctx, type, cb, user_data);
        binder_data_request_queue(dr);
    }
    return dr;
}

BinderDataRequest*
binder_data_call_deactivate(
    BinderData* data,
    int cid,
    BinderDataCallDeactivateFunc cb,
    void* user_data)
{
    BinderDataObject* self = binder_data_cast(data);
    BinderDataRequest* dr = NULL;

    if (self) {
        dr = binder_data_call_deact_new(self, cid, cb, user_data);
        binder_data_request_queue(dr);
    }
    return dr;
}

RadioRequest*
binder_data_deactivate_data_call_request_new(
    BinderData* data,
    int cid,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    BinderDataObject* self = binder_data_cast(data);

    return self ? binder_data_object_deactivate_data_call_request_new(self,
        cid, complete, destroy, user_data) : NULL;
}

RadioRequest*
binder_data_set_data_allowed_request_new(
    BinderData* data,
    gboolean allow,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    BinderDataObject* self = binder_data_cast(data);

    return self ? binder_data_object_set_data_allowed_request_new(self, allow,
        complete, destroy, user_data) : NULL;
}

gboolean
binder_data_call_grab(
    BinderData* data,
    int cid,
    void* cookie)
{
    BinderDataObject* self = binder_data_cast(data);

    if (self && cookie && binder_data_call_find(data->calls, cid)) {
        gpointer key = GINT_TO_POINTER(cid);
        void* prev = g_hash_table_lookup(self->grab, key);

        if (!prev) {
            g_hash_table_insert(self->grab, key, cookie);
            return TRUE;
        } else {
            return (prev == cookie);
        }
    }
    return FALSE;
}

void
binder_data_call_release(
    BinderData* data,
    int cid,
    void* cookie)
{
    BinderDataObject* self = binder_data_cast(data);

    if (self && cookie) {
        g_hash_table_remove(self->grab, GUINT_TO_POINTER(cid));
    }
}

static
void
binder_data_object_init(
    BinderDataObject* self)
{
    self->api = &binder_data_api_hidl;
    self->grab = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static
void
binder_data_object_finalize(
    GObject* object)
{
    BinderDataObject* self = THIS(object);
    BinderData* data = &self->pub;
    BinderNetwork* network = self->network;
    BinderSimSettings* settings = network->settings;
    BinderDataManager* dm = self->dm;

    binder_data_cancel_all_requests(self);
    dm->data_list = g_slist_remove(dm->data_list, self);
    binder_data_manager_check_data(dm);

    radio_client_remove_all_handlers(self->g->client, self->io_event_id);
    radio_request_drop(self->query_req);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    radio_client_remove_handler(self->network_client,
        self->network_client_restricted_state_change_id);
    radio_client_unref(self->network_client);

    binder_radio_power_off(self->radio, self);
    binder_radio_unref(self->radio);

    binder_sim_settings_remove_all_handlers(settings, self->settings_event_id);
    binder_network_unref(self->network);
    binder_data_manager_unref(self->dm);
    binder_data_call_list_free(data->calls);

    g_hash_table_destroy(self->grab);
    g_free(self->log_prefix);

    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_data_object_class_init(
    BinderDataObjectClass* klass)
{

    G_OBJECT_CLASS(klass)->finalize = binder_data_object_finalize;
}

/*==========================================================================*
 * BinderDataManager
 *==========================================================================*/

static
void
binder_data_manager_get_phone_capability_hidl_resp(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_CONFIG_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataManager* dm = user_data;

    GASSERT(dm->phone_cap_req == req);
    radio_request_drop(dm->phone_cap_req);
    dm->phone_cap_req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG("getPhoneCapability failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG("getPhoneCapability error %s", binder_radio_error_string(error));
    } else {
        /* getPhoneCapabilityResponse(RadioResponseInfo, PhoneCapability); */
        guint maxActiveData = 0, maxActiveInternetData = 0;
        const RadioPhoneCapability* pcap = binder_read_hidl_struct(args,
            RadioPhoneCapability);

        GASSERT(pcap);
        if (pcap) {
            const GBinderHidlVec* modems = &pcap->logicalModemList;
            const RadioModemInfo* modem = modems->data.ptr;
            GUtilIntArray* modem_ids = gutil_int_array_sized_new(modems->count);
            guint i;

            maxActiveData = pcap->maxActiveData;
            maxActiveInternetData = pcap->maxActiveInternetData;
            for (i = 0; i < modems->count; i++) {
                gutil_int_array_append(modem_ids, modem[i].modemId);
            }

            if (binder_data_debug_desc.flags & OFONO_DEBUG_FLAG_PRINT) {
                GString* str = g_string_new(NULL);

                for (i = 0; i < modem_ids->count; i++) {
                    if (i > 0) g_string_append_c(str, ',');
                    g_string_append_printf(str, "%u", modem_ids->data[i]);
                }
                DBG("maxActiveData=%u, maxActiveInternetData=%u, "
                    "logicalModemList=[%s]", maxActiveData,
                    maxActiveInternetData, str->str);
                g_string_free(str, TRUE);
            }

            gutil_ints_unref(dm->modem_ids);
            dm->modem_ids = gutil_int_array_free_to_ints(modem_ids);
        }
    }
}

static
void
binder_data_manager_get_phone_capability_aidl_resp(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_CONFIG_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataManager* dm = user_data;

    GASSERT(dm->phone_cap_req == req);
    radio_request_drop(dm->phone_cap_req);
    dm->phone_cap_req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG("getPhoneCapability failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG("getPhoneCapability error %s", binder_radio_error_string(error));
    } else {
        GBinderReader reader, parcel;
        guint32 maxActiveData, maxActiveInternetData;
        GUtilIntArray* modem_ids = NULL;

        /*
         * package android.hardware.radio.config;
         * parcelable PhoneCapability {
         *   byte maxActiveData;
         *   byte maxActiveInternetData;
         *   boolean isInternetLingeringSupported;
         *   byte[] logicalModemIds;
         *   byte maxActiveVoice;
         * }
         */
        gbinder_reader_copy(&reader, args);
        if (gbinder_reader_start_parcelable(&reader, &parcel, NULL)) {
            gsize count;
            const guint8* ids;

            if (gbinder_reader_read_uint32(&parcel, &maxActiveData) &&
                gbinder_reader_read_uint32(&parcel, &maxActiveInternetData) &&
                gbinder_reader_read_bool(&parcel, NULL) &&
                (ids = gbinder_reader_read_byte_array(&parcel, &count))) {
                guint i;

                modem_ids = gutil_int_array_sized_new(count);
                for (i = 0; i < count; i++) {
                    gutil_int_array_append(modem_ids, ids[i]);
                }

                if (binder_data_debug_desc.flags & OFONO_DEBUG_FLAG_PRINT) {
                    GString* str = g_string_new(NULL);

                    for (i = 0; i < modem_ids->count; i++) {
                        if (i > 0) g_string_append_c(str, ',');
                        g_string_append_printf(str, "%u", modem_ids->data[i]);
                    }
                    DBG("maxActiveData=%u, maxActiveInternetData=%u, "
                        "logicalModemList=[%s]", maxActiveData,
                        maxActiveInternetData, str->str);
                    g_string_free(str, TRUE);
                }

                gutil_ints_unref(dm->modem_ids);
                dm->modem_ids = gutil_int_array_free_to_ints(modem_ids);
            }

            gbinder_reader_finish_parcelable(&parcel);
            GASSERT(gbinder_reader_at_end(&reader));
        }
    }
}

static
void
binder_data_manager_request_phone_capability(
    BinderDataManager* dm)
{
    guint code = RADIO_CONFIG_REQ_NONE;
    RADIO_INTERFACE_TYPE interface_type = radio_config_interface_type(dm->rc);
    RadioConfigRequestCompleteFunc resp = NULL;

    if (interface_type == RADIO_INTERFACE_TYPE_HIDL
        && radio_config_interface(dm->rc) >= RADIO_CONFIG_INTERFACE_1_1) {
        code = RADIO_CONFIG_REQ_GET_PHONE_CAPABILITY;
        resp = binder_data_manager_get_phone_capability_hidl_resp;
    } else if (interface_type == RADIO_INTERFACE_TYPE_AIDL) {
        code = RADIO_CONFIG_AIDL_REQ_GET_PHONE_CAPABILITY;
        resp = binder_data_manager_get_phone_capability_aidl_resp;
    }

    if (resp) {
        radio_request_drop(dm->phone_cap_req);
        dm->phone_cap_req = radio_request_try_submit(radio_config_request_new
            (dm->rc, code, NULL, resp, NULL, dm));
    }
}

BinderDataManager*
binder_data_manager_new(
    RadioConfig* rc,
    BINDER_DATA_MANAGER_FLAGS flags,
    enum ofono_radio_access_mode non_data_mode)
{
    BinderDataManager* dm = g_new0(BinderDataManager, 1);

    g_atomic_int_set(&dm->refcount, 1);
    dm->flags = flags;
    dm->non_data_mode = ofono_radio_access_max_mode(non_data_mode);
    dm->rc = radio_config_ref(rc);
    binder_data_manager_request_phone_capability(dm);
    return dm;
}

BinderDataManager*
binder_data_manager_ref(
    BinderDataManager* dm)
{
    if (dm) {
        GASSERT(dm->refcount > 0);
        g_atomic_int_inc(&dm->refcount);
    }
    return dm;
}

void
binder_data_manager_unref(
    BinderDataManager* dm)
{
    if (dm) {
        GASSERT(dm->refcount > 0);
        if (g_atomic_int_dec_and_test(&dm->refcount)) {
            GASSERT(!dm->data_list);
            radio_request_drop(dm->phone_cap_req);
            radio_config_unref(dm->rc);
            gutil_ints_unref(dm->modem_ids);
            g_free(dm);
        }
    }
}

void
binder_data_manager_set_radio_config(
    BinderDataManager* dm,
    RadioConfig* rc)
{
    if (dm && dm->rc != rc) {
        radio_config_unref(dm->rc);
        dm->rc = radio_config_ref(rc);

        /* Most likely modem ids wouldn't change, but let's double-check */
        radio_request_drop(dm->phone_cap_req);
        dm->phone_cap_req = NULL;
        binder_data_manager_request_phone_capability(dm);
    }
}

static
gboolean
binder_data_manager_handover(
    BinderDataManager* dm)
{
    /*
     * The 3G/LTE handover thing only makes sense if we are managing
     * more than one SIM slot. Otherwise leave things where they are.
     */
    return dm->data_list && dm->data_list->next &&
        (dm->flags & BINDER_DATA_MANAGER_3GLTE_HANDOVER);
}

static
gboolean
binder_data_manager_requests_pending(
    BinderDataManager* dm)
{
    GSList* l;

    for (l = dm->data_list; l; l = l->next) {
        BinderDataObject* data = THIS(l->data);

        if (data->pending_req || data->req_queue) {
            return TRUE;
        }
    }

    return FALSE;
}

static
void
binder_data_manager_check_network_mode(
    BinderDataManager* dm)
{
    GSList* l;

    if (dm->non_data_mode && binder_data_manager_handover(dm)) {
        BinderNetwork* lte_network = NULL;
        BinderNetwork* best_network = NULL;
        enum ofono_radio_access_mode best_mode = OFONO_RADIO_ACCESS_MODE_ANY;
        const enum ofono_radio_access_mode non_data_mask =
            (dm->non_data_mode << 1) - 1;

        /* Find a SIM for internet access */
        for (l = dm->data_list; l; l = l->next) {
            BinderDataObject* data = THIS(l->data);
            BinderNetwork* network = data->network;
            BinderSimSettings* sim = network->settings;
            enum ofono_radio_access_mode mode;

            /* Select the first network with internet role */
            if ((sim->pref > OFONO_RADIO_ACCESS_MODE_GSM) &&
                (data->flags & BINDER_DATA_FLAG_MAX_SPEED)) {
                lte_network = network;
                break;
            }

            /* At the same time, look for a suitable slot */
            mode = binder_network_max_supported_mode(network);
            if (mode > best_mode) {
                best_network = network;
                best_mode = mode;
            }
        }

        /*
         * If there's no SIM selected for internet access
         * then use a slot with highest capabilities for LTE.
         */
        if (!lte_network) {
            lte_network = best_network;
        }

        for (l = dm->data_list; l; l = l->next) {
            BinderDataObject* data = THIS(l->data);
            BinderNetwork* net = data->network;

            binder_network_set_allowed_modes(net, (net == lte_network) ?
                binder_data_max_allowed_modes(data) : non_data_mask, FALSE);
        }
    } else {
        /* Otherwise there's no reason to limit anything */
        for (l = dm->data_list; l; l = l->next) {
            BinderDataObject* data = THIS(l->data);

            binder_network_set_allowed_modes(data->network,
                binder_data_max_allowed_modes(data), FALSE);
        }
    }
}

static
BinderDataObject*
binder_data_manager_allowed(
    BinderDataManager* dm)
{
    if (dm) {
        GSList* l;

        for (l = dm->data_list; l; l = l->next) {
            BinderDataObject* data = THIS(l->data);

            if (data->flags & BINDER_DATA_FLAG_ALLOWED) {
                return data;
            }
        }
    }
    return NULL;
}

static
void
binder_data_manager_switch_data_on(
    BinderDataManager* dm,
    BinderDataObject* data)
{
    DBG_(data, "allowing data");
    GASSERT(!(data->flags & BINDER_DATA_FLAG_ON));

    if (binder_data_manager_handover(dm)) {
        binder_network_set_allowed_modes(data->network,
            binder_data_max_allowed_modes(data), TRUE);
    }

    if (!binder_data_allow_submit_request(data, TRUE)) {
        data->flags |= BINDER_DATA_FLAG_ON;
        GASSERT(binder_data_is_allowed(data));
        DBG_(data, "data on");
        binder_base_emit_property_change(&data->base,
            BINDER_DATA_PROPERTY_ALLOWED);
    }
}

void
binder_data_manager_check_data(
    BinderDataManager* dm)
{
    /*
     * Don't do anything if there're any requests pending.
     */
    if (!binder_data_manager_requests_pending(dm)) {
        BinderDataObject* data = binder_data_manager_allowed(dm);

        binder_data_manager_check_network_mode(dm);
        if (data && !(data->flags & BINDER_DATA_FLAG_ON)) {
            binder_data_manager_switch_data_on(dm, data);
        }
    }
}

void
binder_data_manager_assert_data_on(
    BinderDataManager* dm)
{
    if (binder_data_manager_need_set_data_allowed(dm)) {
        binder_data_allow_submit_request(binder_data_manager_allowed(dm), TRUE);
    }
}

gboolean
binder_data_manager_need_set_data_allowed(
    BinderDataManager* dm)
{
    return dm &&
        radio_config_interface_type(dm->rc) == RADIO_INTERFACE_TYPE_HIDL &&
        radio_config_interface(dm->rc) < RADIO_CONFIG_INTERFACE_1_1;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
