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

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_ints.h>
#include <gutil_intarray.h>
#include <gutil_strv.h>
#include <gutil_macros.h>

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
    IO_EVENT_DATA_CALL_LIST_CHANGED_1_0,
    IO_EVENT_DATA_CALL_LIST_CHANGED_1_4,
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

typedef struct binder_data_object {
    BinderBase base;
    BinderData pub;
    RadioRequestGroup* g;
    BinderRadio* radio;
    BinderNetwork* network;
    BinderDataManager* dm;

    BINDER_DATA_FLAGS flags;
    RADIO_RESTRICTED_STATE restricted_state;

    BinderDataRequest* req_queue;
    BinderDataRequest* pending_req;

    BinderDataOptions options;
    gboolean use_data_profiles;
    guint mms_data_profile_id;
    guint slot;
    char* log_prefix;
    RadioRequest* query_req;
    gulong io_event_id[IO_EVENT_COUNT];
    gulong settings_event_id[SETTINGS_EVENT_COUNT];
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
    DATA_REQUEST_FLAG_COMPLETED = 0x1,
    DATA_REQUEST_FLAG_CANCEL_WHEN_ALLOWED = 0x2,
    DATA_REQUEST_FLAG_CANCEL_WHEN_DISALLOWED = 0x4
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

static struct ofono_debug_desc binder_data_debug_desc OFONO_DEBUG_ATTR = {
    .file = __FILE__,
    .flags = OFONO_DEBUG_FLAG_DEFAULT,
};

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderDataObject* binder_data_cast(BinderData* net)
    { return net ? THIS(G_CAST(net, BinderDataObject, pub)) : NULL; }
static inline void binder_data_object_ref(BinderDataObject* data)
    { g_object_ref(data); }
static inline void binder_data_object_unref(BinderDataObject* data)
    { g_object_unref(data); }

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

/*==========================================================================*
 * Request builders
 *==========================================================================*/

RadioRequest*
binder_data_deactivate_data_call_request_new(
    RadioRequestGroup* group,
    int cid,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    RadioClient* client = group->client;
    const RADIO_INTERFACE iface = radio_client_interface(client);
    RadioRequest* req;
    GBinderWriter args;

    if (iface >= RADIO_INTERFACE_1_2) {
        req = radio_request_new(client,
            RADIO_REQ_DEACTIVATE_DATA_CALL_1_2, &args,
            complete, destroy, user_data);

        /*
         * deactivateDataCall_1_2(int32 serial, int32 cid,
         *   DataRequestReason reason);
         */
        gbinder_writer_append_int32(&args, cid);
        gbinder_writer_append_int32(&args, RADIO_DATA_REQUEST_REASON_NORMAL);
    } else {
        req = radio_request_new(client,
            RADIO_REQ_DEACTIVATE_DATA_CALL, &args,
            complete, destroy, user_data);

        /*
         * deactivateDataCall(int32 serial, int32 cid,
         *   bool reasonRadioShutDown);
         */
        gbinder_writer_append_int32(&args, cid);
        gbinder_writer_append_bool(&args, FALSE);
    }

    return req;
}

RadioRequest*
binder_data_set_data_allowed_request_new(
    RadioRequestGroup* group,
    gboolean allow,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(group,
        RADIO_REQ_SET_DATA_ALLOWED, &args,
        complete, destroy, user_data);

    /* setDataAllowed(int32 serial, bool allow) */
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
                binder_data_call_deact_cid(self, dc->cid);
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

    /* restrictedStateChanged(RadioIndicationType, PhoneRestrictedState); */
    GASSERT(code == RADIO_IND_RESTRICTED_STATE_CHANGED);
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
binder_data_call_list_changed(
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
binder_data_call_list_changed_1_0(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataObject* data = THIS(user_data);
    GBinderReader reader;
    const RadioDataCall* calls;
    gsize n = 0;

    /* dataCallListChanged(RadioIndicationType, vec<SetupDataCallResult>); */
    GASSERT(code == RADIO_IND_DATA_CALL_LIST_CHANGED);
    gbinder_reader_copy(&reader, args);
    calls = gbinder_reader_read_hidl_type_vec(&reader, RadioDataCall, &n);
    binder_data_call_list_changed(data, binder_data_call_list_1_0(calls, n));
}

static
void
binder_data_call_list_changed_1_4(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataObject* data = THIS(user_data);
    GBinderReader reader;
    const RadioDataCall_1_4* calls;
    gsize n = 0;

    /* dataCallListChanged_1_4(RadioIndicationType,vec<SetupDataCallResult>) */
    GASSERT(code == RADIO_IND_DATA_CALL_LIST_CHANGED_1_4);
    gbinder_reader_copy(&reader, args);
    calls = gbinder_reader_read_hidl_type_vec(&reader, RadioDataCall_1_4, &n);
    binder_data_call_list_changed(data, binder_data_call_list_1_4(calls, n));
}

static
void binder_data_query_data_calls_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderDataObject* data = THIS(user_data);

    GASSERT(data->query_req == req);
    radio_request_unref(data->query_req);
    data->query_req = NULL;

    /*
     * Only RADIO_ERROR_NONE and RADIO_ERROR_RADIO_NOT_AVAILABLE are expected,
     * all other errors are filtered out by binder_data_poll_call_state_retry()
     */
    if (status == RADIO_TX_STATUS_OK) {
        GSList* list = NULL;

        if (error == RADIO_ERROR_NONE) {
            GBinderReader reader;
            gsize count = 0;

            gbinder_reader_copy(&reader, args);
            if (resp == RADIO_RESP_GET_DATA_CALL_LIST) {
                /*
                 *  getDataCallListResponse(RadioResponseInfo,
                 *      vec<SetupDataCallResult> dcResponse);
                 */
                const RadioDataCall* calls =
                    gbinder_reader_read_hidl_type_vec(&reader,
                        RadioDataCall, &count);

                list = binder_data_call_list_1_0(calls, count);
            } else if (resp == RADIO_RESP_GET_DATA_CALL_LIST_1_4) {
                /*
                 * getDataCallListResponse_1_4(RadioResponseInfo,
                 *     vec<SetupDataCallResult> dcResponse);
                 */
                const RadioDataCall_1_4* calls =
                    gbinder_reader_read_hidl_type_vec(&reader,
                        RadioDataCall_1_4, &count);

                list = binder_data_call_list_1_4(calls, count);
            } else {
                ofono_error("Unexpected getDataCallList response %d", resp);
            }
        } else {
            DBG_(data, "setupDataCall error %s",
                binder_radio_error_string(error));
        }

        binder_data_set_calls(data, list);
    }
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
                binder_data_request_free(dr);
            }
        }

        if (!data->pending_req) {
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
void
binder_data_call_setup_cancel(
    BinderDataRequest* dr)
{
    BinderDataRequestSetup* setup = G_CAST(dr, BinderDataRequestSetup, req);

    binder_data_request_cancel_io(dr);
    if (setup->retry_delay_id) {
        g_source_remove(setup->retry_delay_id);
        setup->retry_delay_id = 0;
    }
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

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            GBinderReader reader;

            gbinder_reader_copy(&reader, args);
            if (resp == RADIO_RESP_SETUP_DATA_CALL) {
                /*
                 * setupDataCallResponse(RadioResponseInfo,
                 *     SetupDataCallResult dcResponse);
                 */
                const RadioDataCall* dc =
                    gbinder_reader_read_hidl_struct(&reader, RadioDataCall);

                if (dc) {
                    call = binder_data_call_new_1_0(dc);
                }
            } else if (resp == RADIO_RESP_SETUP_DATA_CALL_1_4) {
                /*
                 * setupDataCallResponse_1_4(RadioResponseInfo,
                 *     SetupDataCallResult dcResponse);
                 */
                const RadioDataCall_1_4* dc =
                    gbinder_reader_read_hidl_struct(&reader, RadioDataCall_1_4);

                if (dc) {
                    call = binder_data_call_new_1_4(dc);
                }
            } else {
                ofono_error("Unexpected setupDataCall response %d", resp);
            }
        } else {
            DBG_(self, "setupDataCall error %s",
                binder_radio_error_string(error));
        }
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
    BinderNetwork* network = data->network;
    RadioRequestGroup* g = data->g;
    const RADIO_INTERFACE iface = radio_client_interface(g->client);
    RadioRequest* req;
    GBinderWriter writer;
    const char* nothing = NULL;
    const RADIO_TECH tech = (setup->profile_id == RADIO_DATA_PROFILE_IMS) ?
        RADIO_TECH_LTE : network->data.radio_tech;
    const RADIO_APN_AUTH_TYPE auth =(setup->username && setup->username[0]) ?
        binder_radio_auth_from_ofono_method(setup->auth_method) :
        RADIO_APN_AUTH_NONE;

    if (iface >= RADIO_INTERFACE_1_4) {
        RadioDataProfile_1_4* dp;
        guint parent;

        req = radio_request_new2(g, RADIO_REQ_SETUP_DATA_CALL_1_4,
            &writer, binder_data_call_setup_cb, NULL, setup);

        /*
         * setupDataCall_1_4(int32_t serial, AccessNetwork accessNetwork,
         *   DataProfileInfo dataProfileInfo, bool roamingAllowed,
         *   DataRequestReason reason, vec<string> addresses,
         *   vec<string> dnses);
         */
        dp = gbinder_writer_new0(&writer, RadioDataProfile_1_4);
        dp->profileId = setup->profile_id;
        binder_copy_hidl_string(&writer, &dp->apn, setup->apn);
        dp->protocol = dp->roamingProtocol =
            binder_proto_from_ofono_proto(setup->proto);
        dp->authType = auth;
        binder_copy_hidl_string(&writer, &dp->user, setup->username);
        binder_copy_hidl_string(&writer, &dp->password, setup->password);
        dp->enabled = TRUE;

        gbinder_writer_append_int32(&writer,
            binder_radio_access_network_for_tech(tech)); /* accessNetwork */
        /* dataProfileInfo */
        parent = gbinder_writer_append_buffer_object(&writer, dp, sizeof(*dp));
        binder_append_hidl_string_data(&writer, dp, apn, parent);
        binder_append_hidl_string_data(&writer, dp, user, parent);
        binder_append_hidl_string_data(&writer, dp, password, parent);
        gbinder_writer_append_bool(&writer, TRUE);  /* roamingAllowed */
        gbinder_writer_append_int32(&writer,
            RADIO_DATA_REQUEST_REASON_NORMAL);      /* reason */
        gbinder_writer_append_hidl_string_vec(&writer, &nothing, -1);
        gbinder_writer_append_hidl_string_vec(&writer, &nothing, -1);
    } else {
        RadioDataProfile* dp;
        const char* proto_str = binder_proto_str_from_ofono_proto(setup->proto);
        guint parent;

        req = radio_request_new2(g, (iface >= RADIO_INTERFACE_1_2) ?
            RADIO_REQ_SETUP_DATA_CALL_1_2 : RADIO_REQ_SETUP_DATA_CALL,
            &writer, binder_data_call_setup_cb, NULL, setup);

        dp = gbinder_writer_new0(&writer, RadioDataProfile);
        dp->profileId = setup->profile_id;
        binder_copy_hidl_string(&writer, &dp->apn, setup->apn);
        binder_copy_hidl_string(&writer, &dp->protocol, proto_str);
        dp->roamingProtocol = dp->protocol;
        dp->authType = auth;
        binder_copy_hidl_string(&writer, &dp->user, setup->username);
        binder_copy_hidl_string(&writer, &dp->password, setup->password);
        dp->enabled = TRUE;
        dp->supportedApnTypesBitmap =
            binder_radio_apn_types_for_profile(setup->profile_id);
        binder_copy_hidl_string(&writer, &dp->mvnoMatchData, NULL);

        if (iface >= RADIO_INTERFACE_1_2) {
            /*
             * setupDataCall_1_2(int32_t serial, AccessNetwork accessNetwork,
             *   DataProfileInfo dataProfileInfo, bool modemCognitive,
             *   bool roamingAllowed, bool isRoaming, DataRequestReason reason,
             *   vec<string> addresses, vec<string> dnses);
             */
            gbinder_writer_append_int32(&writer,
                binder_radio_access_network_for_tech(tech)); /* accessNetwork */
        } else {
            /*
             * setupDataCall(int32 serial, RadioTechnology radioTechnology,
             *   DataProfileInfo dataProfileInfo, bool modemCognitive,
             *   bool roamingAllowed, bool isRoaming);
             */
            gbinder_writer_append_int32(&writer, tech); /* radioTechnology */
        }

        /* dataProfileInfo */
        parent = gbinder_writer_append_buffer_object(&writer, dp, sizeof(*dp));
        binder_append_hidl_string_data(&writer, dp, apn, parent);
        binder_append_hidl_string_data(&writer, dp, protocol, parent);
        binder_append_hidl_string_data(&writer, dp, roamingProtocol, parent);
        binder_append_hidl_string_data(&writer, dp, user, parent);
        binder_append_hidl_string_data(&writer, dp, password, parent);
        binder_append_hidl_string_data(&writer, dp, mvnoMatchData, parent);
        gbinder_writer_append_bool(&writer, FALSE); /* modemCognitive */
        gbinder_writer_append_bool(&writer, TRUE);  /* roamingAllowed */
        gbinder_writer_append_bool(&writer, FALSE); /* isRoaming */

        if (iface >= RADIO_INTERFACE_1_2) {
            gbinder_writer_append_int32(&writer,
                RADIO_DATA_REQUEST_REASON_NORMAL);      /* reason */
            gbinder_writer_append_hidl_string_vec(&writer, &nothing, -1);
            gbinder_writer_append_hidl_string_vec(&writer, &nothing, -1);
        }
    }

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
    BinderDataRequestSetup* setup = g_new0(BinderDataRequestSetup, 1);
    BinderDataRequest* dr = &setup->req;

    setup->profile_id = RADIO_DATA_PROFILE_DEFAULT;
    if (data->use_data_profiles) {
        switch (context_type) {
        case OFONO_GPRS_CONTEXT_TYPE_MMS:
            setup->profile_id = data->mms_data_profile_id;
            break;
        case OFONO_GPRS_CONTEXT_TYPE_IMS:
            setup->profile_id = RADIO_DATA_PROFILE_IMS;
            break;
        case OFONO_GPRS_CONTEXT_TYPE_ANY:
        case OFONO_GPRS_CONTEXT_TYPE_INTERNET:
        case OFONO_GPRS_CONTEXT_TYPE_WAP:
            break;
        }
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

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            if (resp == RADIO_RESP_DEACTIVATE_DATA_CALL) {
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
            } else {
                ofono_error("Unexpected deactivateDataCall response %d", resp);
            }
        } else {
            DBG_(self, "deactivateDataCall error %s",
                binder_radio_error_string(error));
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
    BinderDataObject* data = dr->data;

    return binder_data_request_call(dr,
        binder_data_deactivate_data_call_request_new(data->g, deact->cid,
            binder_data_call_deact_cb, NULL, deact));
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
            if (resp == RADIO_CONFIG_RESP_SET_PREFERRED_DATA_MODEM) {
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
    RadioRequest* req = radio_config_request_new(dm->rc,
            RADIO_CONFIG_REQ_SET_PREFERRED_DATA_MODEM, &args,
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

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            if (resp == RADIO_RESP_SET_DATA_ALLOWED) {
                const gboolean was_allowed = binder_data_is_allowed(data);

                if (ad->allow) {
                    data->flags |= BINDER_DATA_FLAG_ON;
                    DBG_(data, "data on");
                } else {
                    data->flags &= ~BINDER_DATA_FLAG_ON;
                    DBG_(data, "data off");
                }

                binder_data_check_allowed(data, was_allowed);
            } else {
                ofono_error("Unexpected setDataAllowed response %d", resp);
            }
        } else {
            DBG_(data, "setDataAllowed error %s",
                binder_radio_error_string(error));
        }
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
    BinderDataObject* data = dr->data;
    RadioRequest* req = binder_data_set_data_allowed_request_new(data->g,
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
    } else if (allow) {
        /* IRadioConfig >= 1.1 */
        binder_data_request_queue
            (binder_data_set_preferred_data_modem_new(data));
    }
    return FALSE;
}

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
void binder_data_client_dead_cb(
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
    RadioClient* client,
    const char* name,
    BinderRadio* radio,
    BinderNetwork* network,
    const BinderDataOptions* options,
    const BinderSlotConfig* config)
{
    GASSERT(dm);
    if (G_LIKELY(dm)) {
        BinderDataObject* self = g_object_new(THIS_TYPE, NULL);
        BinderData* data = &self->pub;
        BinderSimSettings* settings = network->settings;

        self->options = *options;
        self->log_prefix = binder_dup_prefix(name);
        self->use_data_profiles = config->use_data_profiles;
        self->mms_data_profile_id = config->mms_data_profile_id;
        self->slot = config->slot;
        self->g = radio_request_group_new(client); /* Keeps ref to client */
        self->dm = binder_data_manager_ref(dm);
        self->radio = binder_radio_ref(radio);
        self->network = binder_network_ref(network);

        self->io_event_id[IO_EVENT_DATA_CALL_LIST_CHANGED_1_0] =
            radio_client_add_indication_handler(client,
                RADIO_IND_DATA_CALL_LIST_CHANGED,
                binder_data_call_list_changed_1_0, self);
        self->io_event_id[IO_EVENT_DATA_CALL_LIST_CHANGED_1_4] =
            radio_client_add_indication_handler(client,
                RADIO_IND_DATA_CALL_LIST_CHANGED_1_4,
                binder_data_call_list_changed_1_4, self);
        self->io_event_id[IO_EVENT_RESTRICTED_STATE_CHANGED] =
            radio_client_add_indication_handler(client,
                RADIO_IND_RESTRICTED_STATE_CHANGED,
                binder_data_restricted_state_changed, self);
        self->io_event_id[IO_EVENT_DEATH] =
            radio_client_add_death_handler(client,
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
        RadioRequest* ioreq = radio_request_new2(self->g,
            RADIO_REQ_GET_DATA_CALL_LIST, NULL,
            binder_data_query_data_calls_cb, NULL, self);

        radio_request_set_retry(ioreq, BINDER_RETRY_SECS*1000, -1);
        radio_request_set_retry_func(ioreq, binder_data_poll_call_state_retry);
        self->query_req = ioreq;
        if (!radio_request_submit(self->query_req)) {
            radio_request_unref(self->query_req);
            self->query_req = NULL;
        }
    }
}

BinderData*
binder_data_ref(
    BinderData* data)
{
    BinderDataObject* self = binder_data_cast(data);

    if (G_LIKELY(self)) {
        binder_data_object_ref(self);
    }
    return data;
}

void
binder_data_unref(
    BinderData* data)
{
    BinderDataObject* self = binder_data_cast(data);

    if (G_LIKELY(self)) {
        binder_data_object_unref(self);
    }
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
binder_data_manager_get_phone_capability_done(
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

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_CONFIG_RESP_GET_PHONE_CAPABILITY) {
            if (error == RADIO_ERROR_NONE) {
                /*
                 * getPhoneCapabilityResponse(RadioResponseInfo,
                 *   PhoneCapability phoneCapability);
                 */
                const RadioPhoneCapability* pcap =
                    binder_read_hidl_struct(args, RadioPhoneCapability);

                GASSERT(pcap);
                if (pcap) {
                    const GBinderHidlVec* modems = &pcap->logicalModemList;
                    const RadioModemInfo* modem = modems->data.ptr;
                    const guint n = modems->count;
                    GUtilIntArray* modem_ids = gutil_int_array_sized_new(n);
                    guint i;

                    for (i = 0; i < n; i++) {
                        gutil_int_array_append(modem_ids, modem[i].modemId);
                    }

                    gutil_ints_unref(dm->modem_ids);
                    dm->modem_ids = gutil_int_array_free_to_ints(modem_ids);

                    if (binder_data_debug_desc.flags & OFONO_DEBUG_FLAG_PRINT) {
                        GString* str = g_string_new("");

                        for (i = 0; i < n; i++) {
                            if (i > 0) {
                                g_string_append_c(str, ',');
                            }
                            g_string_append_printf(str, "%u", modem[i].modemId);
                        }
                        DBG("maxActiveData=%u, maxActiveInternetData=%u, "
                            "logicalModemList=[%s]", pcap->maxActiveData,
                            pcap->maxActiveInternetData, str->str);
                        g_string_free(str, TRUE);
                    }
               }
            } else {
                DBG("%s error %s", radio_config_resp_name(dm->rc, resp),
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getPhoneCapability response %d", resp);
        }
    } else {
        DBG("getPhoneCapability error %s", binder_radio_error_string(error));
    }
}

static
void
binder_data_manager_request_phone_capability(
    BinderDataManager* dm)
{
    if (radio_config_interface(dm->rc) >= RADIO_CONFIG_INTERFACE_1_1) {
        RadioRequest* req = radio_config_request_new(dm->rc,
            RADIO_CONFIG_REQ_GET_PHONE_CAPABILITY, NULL,
            binder_data_manager_get_phone_capability_done, NULL, dm);

        if (radio_request_submit(req)) {
            dm->phone_cap_req = req; /* Keep the ref */
        } else {
            radio_request_unref(req);
        }
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
    return dm && radio_config_interface(dm->rc) < RADIO_CONFIG_INTERFACE_1_1;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
