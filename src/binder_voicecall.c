/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *  Copyright (C) 2024 Slava Monich <slava@monich.com>
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

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_ims_reg.h"
#include "binder_util.h"
#include "binder_voicecall.h"

#include "binder_ext_slot.h"
#include "binder_ext_call.h"

#include <ofono/ims.h>
#include <ofono/misc.h>
#include <ofono/voicecall.h>

#include <radio_client.h>
#include <radio_network_types.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_voice_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_idlequeue.h>
#include <gutil_intarray.h>
#include <gutil_ints.h>
#include <gutil_macros.h>
#include <gutil_ring.h>
#include <gutil_strv.h>

#define VOICECALL_BLOCK_TIMEOUT_MS (5*1000)

enum binder_voicecall_events {
    VOICECALL_EVENT_CALL_STATE_CHANGED,
    VOICECALL_EVENT_SUPP_SVC_NOTIFICATION,
    VOICECALL_EVENT_RINGBACK_TONE,
    VOICECALL_EVENT_ECCLIST_CHANGED, /* IRadio 1.4 specific */
    VOICECALL_EVENT_COUNT
};

enum binder_voicecall_network_events {
    VOICECALL_NETWORK_EVENT_SUPP_SVC_NOTIFICATION,
    VOICECALL_NETWORK_EVENT_COUNT
};

enum binder_voicecall_ext_events {
    VOICECALL_EXT_CALL_STATE_CHANGED,
    VOICECALL_EXT_CALL_DISCONNECTED,
    VOICECALL_EXT_CALL_SUPP_SVC_NOTIFICATION,
    VOICECALL_EXT_CALL_RINGBACK_TONE,
    VOICECALL_EXT_EVENT_COUNT
};

typedef struct binder_voicecall_api BinderVoiceCallApi;

typedef struct binder_voicecall {
    struct ofono_voicecall* vc;
    const BinderVoiceCallApi* api;
    char* log_prefix;
    GSList* calls;
    BinderExtCall* ext;
    BinderImsReg* ims_reg;
    RadioRequestGroup* voice_g;
    RadioRequestGroup* network_g;
    ofono_voicecall_cb_t cb;
    void* data;
    GUtilIntArray* local_release_ids;
    GUtilIdleQueue* idleq;
    GUtilRing* dtmf_queue;
    GUtilInts* local_hangup_reasons;
    GUtilInts* remote_hangup_reasons;
    RadioRequest* send_dtmf_req;
    RadioRequest* clcc_poll_req;
    guint ext_send_dtmf_id;
    guint ext_req_id;
    gulong ext_event[VOICECALL_EXT_EVENT_COUNT];
    gulong voice_event[VOICECALL_EVENT_COUNT];
    gulong network_event[VOICECALL_NETWORK_EVENT_COUNT];
    gulong supp_svc_notification_id;
    gulong ringback_tone_event_id;
} BinderVoiceCall;

typedef struct binder_voicecall_cb_data {
    int ref_count;
    int pending_call_count;
    int success;
    BinderVoiceCall* self;
    ofono_voicecall_cb_t cb;
    gpointer data;
} BinderVoiceCallCbData;

typedef struct binder_voicecall_lastcause_data {
    BinderVoiceCall* self;
    guint cid;
} BinderVoiceCallLastCauseData;

typedef struct binder_voicecall_info {
    struct ofono_call oc;
    BinderExtCall* ext; /* Not a ref */
} BinderVoiceCallInfo;

#define ANSWER_FLAGS BINDER_EXT_CALL_ANSWER_NO_FLAGS

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)
#define REQ_NAME_(self,req) radio_client_req_name((self)->voice_g->client, req)

/* Binder API flavors */
struct binder_voicecall_api {
    const char* name;
    BinderWriteStringArg write_string_arg;
    RADIO_IND voice_call_state_changed_ind;
    RADIO_IND voice_indicate_ringback_tone_ind;
    RADIO_IND voice_current_emergency_number_list_ind;
    RADIO_IND voice_supp_svc_notify_ind;
    RADIO_IND network_supp_svc_notify_ind;
    void (*handle_supp_svc_notify)(
        BinderVoiceCall* self,
        GBinderReader* reader);
    RADIO_REQ network_set_supp_service_notifications_req;
    RADIO_REQ voice_dial_req;
    void (*write_dial_args)(
        GBinderWriter* writer,
        const char* address,
        int clir);
    RADIO_REQ voice_accept_call_req;
    RADIO_REQ voice_reject_call_req;
    RADIO_REQ voice_hangup_req;
    RADIO_REQ voice_conference_req;
    RADIO_REQ voice_explicit_call_transfer_req;
    RADIO_REQ voice_separate_connection_req;
    RADIO_REQ voice_switch_waiting_or_holding_and_active_req;
    RADIO_REQ voice_hangup_foreground_resume_background_req;
    RADIO_REQ voice_hangup_waiting_or_background_req;
    RADIO_REQ voice_send_dtmf_req;
    RADIO_REQ voice_get_last_call_fail_cause_req;
    guint (*read_last_call_fail_cause)(
        GBinderReader* reader);
    RADIO_REQ voice_get_current_calls_req;
    GSList* (*read_call_list)(
        GBinderReader* reader,
        RADIO_RESP resp);
    char** (*read_ecclist)(
        BinderVoiceCall* self,
        GBinderReader* reader);
};

static const BinderVoiceCallApi binder_voicecall_api_hidl;
static const BinderVoiceCallApi binder_voicecall_api_aidl;

static
void
binder_voicecall_send_one_dtmf(
   BinderVoiceCall* self);

static
void
binder_voicecall_clear_dtmf_queue(
    BinderVoiceCall* self);

static inline BinderVoiceCall*
binder_voicecall_get_data(struct ofono_voicecall* vc)
    { return ofono_voicecall_get_data(vc); }

static
BinderVoiceCallCbData*
binder_voicecall_cbd_new(
    BinderVoiceCall* self,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCallCbData* req = g_slice_new0(BinderVoiceCallCbData);

    req->ref_count = 1;
    req->self = self;
    req->cb = cb;
    req->data = data;
    return req;
}

static
void
binder_voicecall_cbd_unref(
    BinderVoiceCallCbData* cbd)
{
    if (!--cbd->ref_count) {
        gutil_slice_free(cbd);
    }
}

static
void
binder_voicecall_clear_dtmf_queue(
    BinderVoiceCall* self)
{
    gutil_ring_clear(self->dtmf_queue);
    if (self->ext_send_dtmf_id) {
        binder_ext_call_cancel(self->ext, self->ext_send_dtmf_id);
        self->ext_send_dtmf_id = 0;
    }
    if (self->send_dtmf_req) {
        radio_request_drop(self->send_dtmf_req);
        self->send_dtmf_req = NULL;
    }
}

static
enum ofono_call_mode
binder_voicecall_ext_call_state_to_ofono(
    BINDER_EXT_CALL_STATE state)
{
    switch (state) {
    case BINDER_EXT_CALL_STATE_INVALID:
        break;
    case BINDER_EXT_CALL_STATE_ACTIVE:
        return OFONO_CALL_STATUS_ACTIVE;
    case BINDER_EXT_CALL_STATE_HOLDING:
        return OFONO_CALL_STATUS_HELD;
    case BINDER_EXT_CALL_STATE_DIALING:
        return OFONO_CALL_STATUS_DIALING;
    case BINDER_EXT_CALL_STATE_ALERTING:
        return OFONO_CALL_STATUS_ALERTING;
    case BINDER_EXT_CALL_STATE_INCOMING:
        return OFONO_CALL_STATUS_INCOMING;
    case BINDER_EXT_CALL_STATE_WAITING:
        return OFONO_CALL_STATUS_WAITING;
    }
    DBG("unexpected call state %d", state);
    return OFONO_CALL_STATUS_DISCONNECTED;
}

static
gboolean
binder_voicecall_ofono_call_equal(
    const struct ofono_call* c1,
    const struct ofono_call* c2)
{
    return c1->id == c2->id &&
        c1->type == c2->type &&
        c1->direction == c2->direction &&
        c1->status == c2->status &&
        c1->phone_number.type == c2->phone_number.type &&
        c1->called_number.type == c2->called_number.type &&
        c1->clip_validity == c2->clip_validity &&
        c1->cnap_validity == c2->cnap_validity &&
        !strcmp(c1->phone_number.number, c1->phone_number.number) &&
        !strcmp(c1->called_number.number, c1->called_number.number);
}

static
gint
binder_voicecall_info_compare(
    gconstpointer a,
    gconstpointer b)
{
    const guint ca = ((const BinderVoiceCallInfo*)a)->oc.id;
    const guint cb = ((const BinderVoiceCallInfo*)b)->oc.id;

    return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

static
void
binder_voicecall_info_free(
    gpointer data)
{
    g_slice_free(BinderVoiceCallInfo, data);
}

static
BinderVoiceCallInfo*
binder_voicecall_info_ext_new(
    const BinderExtCallInfo* ci,
    BinderExtCall* ext)
{
    BinderVoiceCallInfo* call = g_slice_new0(BinderVoiceCallInfo);
    struct ofono_call* oc = &call->oc;

    ofono_call_init(oc);

    oc->status = binder_voicecall_ext_call_state_to_ofono(ci->state);
    oc->id = ci->call_id;
    oc->direction = (ci->flags & BINDER_EXT_CALL_FLAG_INCOMING) ?
        OFONO_CALL_DIRECTION_MOBILE_TERMINATED :
        OFONO_CALL_DIRECTION_MOBILE_ORIGINATED;
    oc->type = (ci->type == BINDER_EXT_CALL_TYPE_VOICE) ?
        OFONO_CALL_MODE_VOICE :
        OFONO_CALL_MODE_UNKNOWN;
    if (ci->name) {
        g_strlcpy(oc->name, ci->name, OFONO_MAX_CALLER_NAME_LENGTH);
    }
    oc->phone_number.type = ci->toa;
    if (ci->number && ci->number[0]) {
        oc->clip_validity = OFONO_CLIP_VALIDITY_VALID;
        g_strlcpy(oc->phone_number.number, ci->number,
            OFONO_MAX_PHONE_NUMBER_LENGTH);
    } else {
        oc->clip_validity = OFONO_CLIP_VALIDITY_NOT_AVAILABLE;
    }

    DBG("[id=%d,status=%d,type=%d,number=%s,name=%s]", oc->id,
        oc->status, oc->type, oc->phone_number.number, oc->name);

    call->ext = ext;
    return call;
}

static
GSList*
binder_voicecall_list_find_call_link_with_id(
    GSList* list,
    guint call_id)
{
    GSList* l;

    /*
     * Normally, the list is either empty or very short, there's
     * nothing to optimize.
     */
    for (l = list; l; l = l->next) {
        const BinderVoiceCallInfo* call = l->data;

        if (call->oc.id == call_id) {
            return l;
        }
    }
    return NULL;
}

static
GSList*
binder_voicecall_merge_ext_calls(
    BinderVoiceCall* self,
    GSList* list,
    gboolean keep_ext)
{
    GSList* l;

    /*
     * Since IRadio and ext may have different lists of calls,
     * merge the ongoing calls initiated by the other component
     * before the current list of calls is going to be replaced
     * with the new list.
     */
    for (l = self->calls; l; l = l->next) {
        const BinderVoiceCallInfo* call = l->data;

        if (!!call->ext == keep_ext &&
            !binder_voicecall_list_find_call_link_with_id(list, call->oc.id)) {
            list = g_slist_insert_sorted(list,
                g_slice_dup(BinderVoiceCallInfo, call),
                binder_voicecall_info_compare);
        }
    }
    return list;
}

static
gboolean
binder_voicecall_have_ext_call(
    BinderVoiceCall* self)
{
    if (self->ext) {
        GSList* l;

        for (l = self->calls; l; l = l->next) {
            const BinderVoiceCallInfo* call = l->data;

            if (call->ext) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static
const BinderVoiceCallInfo*
binder_voicecall_find_call_with_status(
    BinderVoiceCall* self,
    enum ofono_call_status status)
{
    GSList* l;

    /*
     * Normally, the list is either empty or very short, there's
     * nothing to optimize.
     */
    for (l = self->calls; l; l = l->next) {
        const BinderVoiceCallInfo* call = l->data;

        if (call->oc.status == status) {
            return call;
        }
    }
    return NULL;
}

static
GSList*
binder_voicecall_find_call_link_with_id(
    BinderVoiceCall* self,
    guint call_id)
{
    return binder_voicecall_list_find_call_link_with_id(self->calls, call_id);
}

static
const BinderVoiceCallInfo*
binder_voicecall_find_call_with_id(
    BinderVoiceCall* self,
    unsigned int call_id)
{
    GSList* l = binder_voicecall_find_call_link_with_id(self, call_id);

    return l ? l->data : NULL;
}

static
enum ofono_call_status
binder_voicecall_status_with_id(
    BinderVoiceCall* self,
    unsigned int call_id)
{
    const BinderVoiceCallInfo* call =
        binder_voicecall_find_call_with_id(self, call_id);

    /* Valid call statuses have value >= 0 */
    return call ? call->oc.status : -1;
}

static
void
binder_voicecall_remove_call_id(
    BinderVoiceCall* self,
    guint call_id)
{
    GSList* l = binder_voicecall_find_call_link_with_id(self, call_id);

    if (l) {
        DBG_(self, "removed call %u", call_id);
        binder_voicecall_info_free(l->data);
        self->calls = g_slist_delete_link(self->calls, l);
    }
}

static
enum ofono_disconnect_reason
binder_voicecall_map_cause(
    BinderVoiceCall* self,
    guint cid,
    RADIO_LAST_CALL_FAIL_CAUSE last_cause)
{
    if (gutil_ints_contains(self->remote_hangup_reasons, last_cause)) {
        DBG_(self, "hangup cause %d => remote hangup", last_cause);
        return OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
    } else if (gutil_ints_contains(self->local_hangup_reasons, last_cause)) {
        DBG_(self, "hangup cause %d => local hangup", last_cause);
        return OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
    } else {
        enum ofono_call_status call_status;

        switch (last_cause) {
        case RADIO_LAST_CALL_FAIL_UNOBTAINABLE_NUMBER:
        case RADIO_LAST_CALL_FAIL_NORMAL:
        case RADIO_LAST_CALL_FAIL_BUSY:
        case RADIO_LAST_CALL_FAIL_NO_ROUTE_TO_DESTINATION:
        case RADIO_LAST_CALL_FAIL_CHANNEL_UNACCEPTABLE:
        case RADIO_LAST_CALL_FAIL_OPERATOR_DETERMINED_BARRING:
        case RADIO_LAST_CALL_FAIL_NO_USER_RESPONDING:
        case RADIO_LAST_CALL_FAIL_NO_ANSWER_FROM_USER:
        case RADIO_LAST_CALL_FAIL_CALL_REJECTED:
        case RADIO_LAST_CALL_FAIL_NUMBER_CHANGED:
        case RADIO_LAST_CALL_FAIL_PREEMPTION:
        case RADIO_LAST_CALL_FAIL_DESTINATION_OUT_OF_ORDER:
        case RADIO_LAST_CALL_FAIL_INVALID_NUMBER_FORMAT:
        case RADIO_LAST_CALL_FAIL_FACILITY_REJECTED:
            return OFONO_DISCONNECT_REASON_REMOTE_HANGUP;

        case RADIO_LAST_CALL_FAIL_NORMAL_UNSPECIFIED:
            call_status = binder_voicecall_status_with_id(self, cid);
            /* If call cid doesn't exist anymore, above method returns -1.
               This case can happen, when the cause response is received
               after the status response that removed the call from the
               list. We then assume that the remote is the cause. */
            if (call_status == -1 ||
                call_status == OFONO_CALL_STATUS_ACTIVE ||
                call_status == OFONO_CALL_STATUS_HELD ||
                call_status == OFONO_CALL_STATUS_DIALING ||
                call_status == OFONO_CALL_STATUS_ALERTING) {
                return OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
            } else if (call_status == OFONO_CALL_STATUS_INCOMING) {
                return OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
            }
            break;

        case RADIO_LAST_CALL_FAIL_ERROR_UNSPECIFIED:
            call_status = binder_voicecall_status_with_id(self, cid);
            if (call_status == OFONO_CALL_STATUS_DIALING ||
                call_status == OFONO_CALL_STATUS_ALERTING ||
                call_status == OFONO_CALL_STATUS_INCOMING) {
                return OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
            }
            break;

        default:
            break;
        }
    }
    return OFONO_DISCONNECT_REASON_ERROR;
}

void
binder_voicecall_lastcause_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCallLastCauseData* data = user_data;
    BinderVoiceCall* self = data->self;
    struct ofono_voicecall* vc = self->vc;
    const guint cid = data->cid;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "getLastCallFailCause tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to retrive last call fail cause: %s",
            binder_radio_error_string(error));
    } else {
        GBinderReader reader;
        gint32 cause_code;

        gbinder_reader_copy(&reader, args);
        cause_code = self->api->read_last_call_fail_cause(&reader);

        /*
         * Cause code 0 is invalid and can be used to check if code was
         * obtained.
         */
        if (cause_code) {
            enum ofono_disconnect_reason reason =
                binder_voicecall_map_cause(self, cid, cause_code);

            ofono_info("Call %d ended with cause %d -> ofono reason %d",
                cid, cause_code, reason);
            ofono_voicecall_disconnected(vc, cid, reason, NULL);
            return;
        }
        ofono_info("Call %d ended with unknown reason", cid);
    }
    ofono_voicecall_disconnected(vc, cid, OFONO_DISCONNECT_REASON_ERROR, NULL);
}

static
void
binder_voicecall_set_calls(
    BinderVoiceCall* self,
    GSList* list)
{
    struct ofono_voicecall* vc = self->vc;
    GSList* n = list;
    GSList* o = self->calls;

    /* Note: the lists are sorted by id */
    while (n || o) {
        const BinderVoiceCallInfo* nc = n ? n->data : NULL;
        const BinderVoiceCallInfo* oc = o ? o->data : NULL;

        if (oc && (!nc || (nc->oc.id > oc->oc.id))) {
            const guint id = oc->oc.id;

            /* old call is gone */
            if (gutil_int_array_remove_all_fast(self->local_release_ids, id)) {
                ofono_voicecall_disconnected(vc, id,
                    OFONO_DISCONNECT_REASON_LOCAL_HANGUP, NULL);
            } else {
                /* Get disconnect cause before informing oFono core */
                BinderVoiceCallLastCauseData* reqdata =
                    g_new0(BinderVoiceCallLastCauseData, 1);
                RadioRequest* req2 = radio_request_new2(self->voice_g,
                    self->api->voice_get_last_call_fail_cause_req, NULL,
                    binder_voicecall_lastcause_cb, g_free, reqdata);

                reqdata->self = self;
                reqdata->cid = id;
                radio_request_submit(req2);
                radio_request_unref(req2);
            }

            binder_voicecall_clear_dtmf_queue(self);
            o = o->next;

        } else if (nc && (!oc || (nc->oc.id < oc->oc.id))) {
            /* new call, signal it */
            if (nc->oc.type == OFONO_CALL_MODE_VOICE) {
                ofono_voicecall_notify(vc, &nc->oc);
                if (self->cb) {
                    ofono_voicecall_cb_t cb = self->cb;
                    void* cbdata = self->data;
                    struct ofono_error err;

                    self->cb = NULL;
                    self->data = NULL;
                    cb(binder_error_ok(&err), cbdata);
                }
            }

            n = n->next;

        } else {
            /* Both old and new call exist */
            if (!binder_voicecall_ofono_call_equal(&nc->oc, &oc->oc)) {
                ofono_voicecall_notify(vc, &nc->oc);
            }
            n = n->next;
            o = o->next;
        }
    }

    g_slist_free_full(self->calls, binder_voicecall_info_free);
    self->calls = list;
}

static
void
binder_voicecall_clcc_poll_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    GSList* list = NULL;

    GASSERT(self->clcc_poll_req == req);
    radio_request_unref(self->clcc_poll_req);
    self->clcc_poll_req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "getCurrentCalls tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        DBG_(self, "getCurrentCalls error %s",
             binder_radio_error_string(error));
    } else {
        GBinderReader reader;

        /*
         * 1.0/IRadioResponse.hal:
         * oneway getCurrentCallsResponse(RadioResponseInfo info,
         *     vec<Call> calls);
         *
         * 1.2/IRadioResponse.hal:
         * oneway getCurrentCallsResponse_1_2(RadioResponseInfo info,
         *     vec<Call> calls);
         *
         * IRadioVoiceResponse.aidl:
         * void getCurrentCallsResponse(in RadioResponseInfo info,
         *     in Call[] calls);
         */

        /* Build sorted list */
        gbinder_reader_copy(&reader, args);
        list = g_slist_sort(self->api->read_call_list(&reader, resp),
            binder_voicecall_info_compare);
    }

    /* Merge the ongoing ext calls since IRadio may not report them */
    binder_voicecall_set_calls(self,
        binder_voicecall_merge_ext_calls(self, list, TRUE));
}

static
gboolean
binder_voicecall_clcc_retry(
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
        case RADIO_ERROR_RADIO_NOT_AVAILABLE:
            return FALSE;
        default:
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_voicecall_clcc_poll(
    BinderVoiceCall* self)
{
    if (!self->clcc_poll_req) {
        RadioRequest* req = radio_request_new2(self->voice_g,
            self->api->voice_get_current_calls_req, NULL,
            binder_voicecall_clcc_poll_cb, NULL, self);

        radio_request_set_retry(req, BINDER_RETRY_MS, -1);
        radio_request_set_retry_func(req, binder_voicecall_clcc_retry);
        self->clcc_poll_req = radio_request_try_submit(req);
    }
}

static
void
binder_voicecall_cbd_destroy(
    gpointer cbd)
{
    binder_voicecall_cbd_unref((BinderVoiceCallCbData*) cbd);
}

static
void
binder_voicecall_request_submitted(
    BinderVoiceCallCbData* cbd)
{
    cbd->ref_count++;
    cbd->pending_call_count++;
}

static
void
binder_voicecall_request_completed(
    BinderVoiceCallCbData* cbd,
    gboolean ok)
{
    /*
     * The ofono API call is considered successful if at least one
     * associated request succeeds.
     */
    if (ok) {
        cbd->success++;
    }

    /*
     * Only invoke the callback if this is the last request associated
     * with this ofono api call (pending call count becomes zero).
     */
    GASSERT(cbd->pending_call_count > 0);
    if (!--cbd->pending_call_count && cbd->cb) {
        struct ofono_error err;

        if (cbd->success) {
            binder_error_init_ok(&err);
        } else {
            binder_error_init_failure(&err);
        }

        cbd->cb(&err, cbd->data);
    }
}

static
void
binder_voicecall_cbd_complete(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCallCbData* data = user_data;

    binder_voicecall_clcc_poll(data->self);
    binder_voicecall_request_completed(data, status == RADIO_TX_STATUS_OK &&
        error == RADIO_ERROR_NONE);
}

static
void
binder_voicecall_cbd_ext_complete(
    BinderExtCall* ext,
    BINDER_EXT_CALL_RESULT result,
    void* user_data)
{
    BinderVoiceCallCbData* data = user_data;

    data->self->ext_req_id = 0;
    binder_voicecall_request_completed(data,
        result == BINDER_EXT_CALL_RESULT_OK);
}

static
void
binder_voicecall_request_submit(
    BinderVoiceCall* self,
    RADIO_REQ code,
    BinderVoiceCallCbData* cbd)
{
    /* Request data will be unref'ed when the request is done */
    if (binder_submit_request2(self->voice_g, code,
        binder_voicecall_cbd_complete,
        binder_voicecall_cbd_destroy, cbd)) {
        binder_voicecall_request_submitted(cbd);
    }
}

static
void
binder_voicecall_dial_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "dial tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("call failed: %s", binder_radio_error_string(error));
    } else {
        if (self->cb) {
            /*
             * CLCC will update the oFono call list with
             * proper ids if it's not done yet.
             */
            binder_voicecall_clcc_poll(self);
        }
        return;
    }

    /*
     * Even though this dial request may have already been completed
     * successfully by binder_voicecall_clcc_poll_cb, dial request
     * may still fail.
     */
    if (self->cb) {
        struct ofono_error err;
        ofono_voicecall_cb_t cb = self->cb;
        void* cbdata = self->data;

        self->cb = NULL;
        self->data = NULL;
        cb(binder_error_failure(&err), cbdata);
    }
}

static
void
binder_voicecall_ext_dial_cb(
    BinderExtCall* ext,
    BINDER_EXT_CALL_RESULT result,
    void* user_data)
{
    BinderVoiceCall* self = user_data;

    self->ext_req_id = 0;
    if (self->cb) {
        struct ofono_error err;
        ofono_voicecall_cb_t cb = self->cb;
        void* cbdata = self->data;

        self->cb = NULL;
        self->data = NULL;
        if (result == BINDER_EXT_CALL_RESULT_OK) {
            cb(binder_error_ok(&err), cbdata);
        } else {
            cb(binder_error_failure(&err), cbdata);
        }
    }
}

static
gboolean
binder_voicecall_can_ext_dial(
    BinderVoiceCall* self)
{
    return self->ext && (!(binder_ext_call_get_interface_flags
        (self->ext) & BINDER_EXT_CALL_INTERFACE_FLAG_IMS_REQUIRED) ||
        (self->ims_reg && self->ims_reg->registered &&
        (self->ims_reg->caps & OFONO_IMS_VOICE_CAPABLE)));
}

static
BINDER_EXT_CALL_CLIR
binder_voicecall_ext_clir(
    enum ofono_clir_option clir)
{
    switch (clir) {
    case OFONO_CLIR_OPTION_INVOCATION:
        return BINDER_EXT_CALL_CLIR_INVOCATION;
    case OFONO_CLIR_OPTION_SUPPRESSION:
        return BINDER_EXT_CALL_CLIR_SUPPRESSION;
    case OFONO_CLIR_OPTION_DEFAULT:
        break;
    }
    return BINDER_EXT_CALL_CLIR_DEFAULT;
}

static
void
binder_voicecall_dial(
    struct ofono_voicecall* vc,
    const struct ofono_phone_number* ph,
    enum ofono_clir_option clir,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    const BinderVoiceCallApi* api = self->api;
    char phbuf[OFONO_PHONE_NUMBER_BUFFER_SIZE];
    const char* phstr = ofono_phone_number_to_string(ph, phbuf);
    GBinderWriter args;
    RadioRequest* req;

    ofono_info("dialing \"%s\"", phstr);
    DBG_(self, "%s,%d,0", phstr, clir);

    binder_ext_call_cancel(self->ext, self->ext_req_id);
    if (binder_voicecall_can_ext_dial(self)) {
        self->ext_req_id = binder_ext_call_dial(self->ext, phstr, ph->type,
            binder_voicecall_ext_clir(clir), BINDER_EXT_CALL_DIAL_FLAGS_NONE,
            binder_voicecall_ext_dial_cb, NULL, self);
        if (self->ext_req_id) {
            self->cb = cb;
            self->data = data;
            return;
        }
    } else {
        self->ext_req_id = 0;
    }

    /* Default action */
    req = radio_request_new2(self->voice_g, api->voice_dial_req, &args,
        binder_voicecall_dial_cb, NULL, self);
    api->write_dial_args(&args, phstr, clir);

    if (radio_request_submit(req)) {
        self->cb = cb;
        self->data = data;
    } else {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }
    radio_request_unref(req);
}

static
void
binder_voicecall_submit_hangup_req(
    struct ofono_voicecall* vc,
    int cid,
    BinderVoiceCallCbData* cbd)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    RadioRequest* req;
    const BinderVoiceCallApi* api = self->api;
    const BinderVoiceCallInfo* call =
        binder_voicecall_find_call_with_id(self, cid);

    /*
     * hangup() for incoming calls doesn't always work the way we would like
     * it to work, i.e. some inter-operator calls get terminated locally but
     * not remotely (the other side keeps thinking that the call is still
     * ringing). For whatever reason, hangupWaitingOrBackground() seems to
     * work better.
     */
    if (call && call->oc.status == OFONO_CALL_STATUS_INCOMING) {
        /* hangupWaitingOrBackground(int32_t serial) */
        req = radio_request_new2(self->voice_g,
            api->voice_hangup_waiting_or_background_req, NULL,
            binder_voicecall_cbd_complete, binder_voicecall_cbd_destroy, cbd);
    } else {
        GBinderWriter writer;

        /* hangup(int32 serial, int32 index) */
        req = radio_request_new2(self->voice_g, api->voice_hangup_req, &writer,
            binder_voicecall_cbd_complete, binder_voicecall_cbd_destroy, cbd);
        gbinder_writer_append_int32(&writer, cid);
    }

    /* Append the call id to the list of calls being released locally */
    GASSERT(!gutil_int_array_contains(self->local_release_ids, cid));
    gutil_int_array_append(self->local_release_ids, cid);

    /* Request data will be unref'ed when the request is done */
    if (radio_request_submit(req)) {
        binder_voicecall_request_submitted(cbd);
    }
    radio_request_unref(req);
}

static
BINDER_EXT_CALL_DISCONNECT_REASON
binder_voicecall_ext_disconnect_reason(
    const BinderVoiceCallInfo* call)
{
    switch (call->oc.status) {
    case OFONO_CALL_STATUS_INCOMING:
    case OFONO_CALL_STATUS_WAITING:
        return BINDER_EXT_CALL_HANGUP_REJECT;
    case OFONO_CALL_STATUS_ACTIVE:
    case OFONO_CALL_STATUS_HELD:
    case OFONO_CALL_STATUS_DIALING:
    case OFONO_CALL_STATUS_ALERTING:
    case OFONO_CALL_STATUS_DISCONNECTED:
        break;
    }
    return BINDER_EXT_CALL_HANGUP_TERMINATE;
}

static
gboolean
binder_voicecall_hangup_filter_active(
    const BinderVoiceCallInfo* call)
{
    switch (call->oc.status) {
    case OFONO_CALL_STATUS_ACTIVE:
    case OFONO_CALL_STATUS_DIALING:
    case OFONO_CALL_STATUS_ALERTING:
    case OFONO_CALL_STATUS_INCOMING:
        return TRUE;
    case OFONO_CALL_STATUS_HELD:
    case OFONO_CALL_STATUS_WAITING:
    case OFONO_CALL_STATUS_DISCONNECTED:
        break;
    }
    return FALSE;
}

static
gboolean
binder_voicecall_hangup_filter_incoming_waiting(
    const BinderVoiceCallInfo* call)
{
    switch (call->oc.status) {
    case OFONO_CALL_STATUS_INCOMING:
    case OFONO_CALL_STATUS_WAITING:
        return TRUE;
    case OFONO_CALL_STATUS_ACTIVE:
    case OFONO_CALL_STATUS_DIALING:
    case OFONO_CALL_STATUS_ALERTING:
    case OFONO_CALL_STATUS_HELD:
    case OFONO_CALL_STATUS_DISCONNECTED:
        break;
    }
    return FALSE;
}

static
gboolean
binder_voicecall_hangup_filter_held(
    const BinderVoiceCallInfo* call)
{
    switch (call->oc.status) {
    case OFONO_CALL_STATUS_WAITING:
    case OFONO_CALL_STATUS_HELD:
        return TRUE;
    case OFONO_CALL_STATUS_INCOMING:
    case OFONO_CALL_STATUS_ACTIVE:
    case OFONO_CALL_STATUS_DIALING:
    case OFONO_CALL_STATUS_ALERTING:
    case OFONO_CALL_STATUS_DISCONNECTED:
        break;
    }
    return FALSE;
}

static
void
binder_voicecall_hangup(
    struct ofono_voicecall* vc,
    gboolean (*filter)(const BinderVoiceCallInfo* call),
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = NULL;
    GSList* l;

    /*
     * The idea is that we submit (potentially) multiple hangup
     * requests and invoke the callback after the last request
     * has completed (pending call count becomes zero).
     */
    for (l = self->calls; l; l = l->next) {
        const BinderVoiceCallInfo* call = l->data;
        const guint id = call->oc.id;

        if (!filter || filter(call)) {
            if (!cbd) {
                cbd = binder_voicecall_cbd_new(self, cb, data);
            }

            /* Send request to the modem */
            if (call->ext) {
                DBG_(self, "hanging up ext call id %u", id);
                if (binder_ext_call_hangup(call->ext, id,
                    binder_voicecall_ext_disconnect_reason(call),
                    BINDER_EXT_CALL_HANGUP_NO_FLAGS,
                    binder_voicecall_cbd_ext_complete,
                    binder_voicecall_cbd_destroy, cbd)) {
                    binder_voicecall_request_submitted(cbd);
                    continue;
                }
            }
            DBG_(self, "hanging up call with id %u", id);
            binder_voicecall_submit_hangup_req(vc, id, cbd);
        } else {
            DBG_(self, "Skipping call with id %u", id);
        }
    }

    if (cbd) {
        /* Release our reference (if any) */
        binder_voicecall_cbd_unref(cbd);
    } else {
        /* No requests were submitted */
        struct ofono_error err;

        cb(binder_error_ok(&err), data);
    }
}

static
void
binder_voicecall_hangup_active(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    binder_voicecall_hangup(vc,
        binder_voicecall_hangup_filter_active, cb, data);
}

static
void
binder_voicecall_hangup_all(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    binder_voicecall_hangup(vc, NULL, cb, data);
}

static
void
binder_voicecall_release_specific(
    struct ofono_voicecall* vc,
    int id,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);
    const BinderVoiceCallInfo* call =
        binder_voicecall_find_call_with_id(self, id);

    if (call) {
        if (call->ext) {
            DBG_(self, "hanging up ext call with id %u", id);
            if (binder_ext_call_hangup(call->ext, id,
                binder_voicecall_hangup_filter_incoming_waiting(call) ?
                BINDER_EXT_CALL_HANGUP_REJECT :
                BINDER_EXT_CALL_HANGUP_TERMINATE,
                BINDER_EXT_CALL_HANGUP_NO_FLAGS,
                binder_voicecall_cbd_ext_complete,
                binder_voicecall_cbd_destroy, cbd)) {
                binder_voicecall_request_submitted(cbd);
                return;
            }
        }
        DBG_(self, "hanging up call with id %d", id);
        binder_voicecall_submit_hangup_req(vc, id, cbd);
        binder_voicecall_cbd_unref(cbd);
    } else if (cb) {
        struct ofono_error err;

        DBG_(self, "call id %d not found", id);
        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_voicecall_call_state_changed_event(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer self)
{
    /* Just need to request the call list again */
    binder_voicecall_clcc_poll((BinderVoiceCall*) self);
}

static
void
binder_voicecall_ext_supp_svc_notification(
    BinderExtCall* ext,
    const BinderExtCallSuppSvcNotify* ssn,
    void* user_data)
{
    BinderVoiceCall* self = user_data;

    if (ssn->mt) {
        struct ofono_phone_number ph;

        /* MT unsolicited result code */
        DBG_(self, "MT code: %d, index: %d type: %d number: %s",  ssn->code,
            ssn->index, ssn->type, ssn->number);

        if (ssn->number && ssn->number[0]) {
            ph.type = ssn->type;
            g_strlcpy(ph.number, ssn->number, sizeof(ph.number));
        } else {
            ph.type = OFONO_NUMBER_TYPE_UNKNOWN;
            ph.number[0] = 0;
        }
        ofono_voicecall_ssn_mt_notify(self->vc, 0, ssn->code, ssn->index, &ph);
    } else {
        /* MO intermediate result code */
        DBG_(self, "MO code: %d, index: %d",  ssn->code, ssn->index);
        ofono_voicecall_ssn_mo_notify(self->vc, 0, ssn->code, ssn->index);
    }
}

static
void
binder_voicecall_supp_svc_notification(
    BinderVoiceCall* self,
    gboolean isMT,
    int code,
    int index,
    int type,
    const char* number)
{
    if (isMT) {
        struct ofono_phone_number ph;

        /* MT unsolicited result code */
        DBG_(self, "MT code: %d, index: %d type: %d number: %s", code,
            index, type, number);
        if (number && number[0]) {
            ph.type = type;
            g_strlcpy(ph.number, number, sizeof(ph.number));
        } else {
            ph.type = OFONO_NUMBER_TYPE_UNKNOWN;
            ph.number[0] = 0;
        }

        ofono_voicecall_ssn_mt_notify(self->vc, 0, code, index, &ph);
    } else {
        /* MO intermediate result code */
        ofono_voicecall_ssn_mo_notify(self->vc, 0, code, index);
    }
}

static
void
binder_voicecall_supp_svc_notification_cb(
    RadioClient* client,
    RADIO_IND ind,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    GBinderReader reader;

    /*
     * If handle_supp_svc_notify successfully parses the args, it calls
     * binder_voicecall_supp_svc_notification
     */
    gbinder_reader_copy(&reader, args);
    self->api->handle_supp_svc_notify(self, &reader);
}

static
gboolean
binder_voicecall_ext_answer(
    BinderVoiceCall* self,
    BinderVoiceCallCbData* cbd)
{
    if (self->ext) {
        binder_ext_call_cancel(self->ext, self->ext_req_id);
        self->ext_req_id = binder_ext_call_answer(self->ext, ANSWER_FLAGS,
            binder_voicecall_cbd_ext_complete,
            binder_voicecall_cbd_destroy, cbd);
        if (self->ext_req_id) {
            binder_voicecall_request_submitted(cbd);
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_voicecall_answer(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);
    RADIO_REQ req = self->api->voice_accept_call_req;
    const BinderVoiceCallInfo* call =
        binder_voicecall_find_call_with_status(self,
            OFONO_CALL_STATUS_INCOMING);

    if (call && call->ext) {
        DBG_(self, "answering ext call");
        if (!binder_voicecall_ext_answer(self, cbd)) {
            /* If it's not handled by the extension, revert to IRadio */
            DBG_(self, "answering ext call (fallback)");
            binder_voicecall_request_submit(self, req, cbd);
        }
    } else {
        /* Default action */
        DBG_(self, "answering current call");
        binder_voicecall_request_submit(self, req, cbd);
    }
    binder_voicecall_cbd_unref(cbd);
}

static
void
binder_voicecall_send_dtmf_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;

    GASSERT(self->send_dtmf_req == req);
    radio_request_unref(self->send_dtmf_req);
    self->send_dtmf_req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "sendDtmf tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("failed to send dtmf: %s",
            binder_radio_error_string(error));
    } else {
        /* Send the next one */
        binder_voicecall_send_one_dtmf(self);
        return;
    }

    binder_voicecall_clear_dtmf_queue(self);
}

static
void
binder_voicecall_send_dtmf_ext_cb(
    BinderExtCall* ext,
    BINDER_EXT_CALL_RESULT result,
    void* user_data)
{
    BinderVoiceCall* self = user_data;

    self->ext_send_dtmf_id = 0;
    if (result == BINDER_EXT_CALL_RESULT_OK) {
        /* Send the next one */
        binder_voicecall_send_one_dtmf(self);
    } else {
        ofono_error("failed to send ext dtmf tone");
        binder_voicecall_clear_dtmf_queue(self);
    }
}

static
void
binder_voicecall_send_one_dtmf(
    BinderVoiceCall* self)
{
    if (!self->send_dtmf_req &&
        !self->ext_send_dtmf_id &&
        gutil_ring_size(self->dtmf_queue) > 0) {
        char tone[2];

        tone[0] = (char)GPOINTER_TO_UINT(gutil_ring_get(self->dtmf_queue));
        tone[1] = 0;
        DBG_(self, "'%s'", tone);

        /* If self->ext is NULL then binder_ext_call_send_dtmf is a noop */
        self->ext_send_dtmf_id = binder_ext_call_send_dtmf(self->ext, tone,
            binder_voicecall_send_dtmf_ext_cb, NULL, self);

        /* If it's not handled by the extension, to the default thing */
        if (!self->ext_send_dtmf_id) {
            const BinderVoiceCallApi* api = self->api;
            GBinderWriter args;
            RadioRequest* req = radio_request_new2(self->voice_g,
                api->voice_send_dtmf_req, &args,
                binder_voicecall_send_dtmf_cb, NULL, self);

            /*
             * IRadio.hal:
             * oneway sendDtmf(int32_t serial, string s);
             *
             * IRadioVoice.aidl
             * void sendDtmf(in int serial, in String s);
             */
            api->write_string_arg(&args, tone);
            if (!(self->send_dtmf_req = radio_request_try_submit(req))) {
                binder_voicecall_clear_dtmf_queue(self);
            }
        }
    }
}

static
void
binder_voicecall_send_dtmf(
    struct ofono_voicecall* vc,
    const char* dtmf,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    struct ofono_error err;

    /*
     * Queue any incoming DTMF, send them to BINDER one-by-one,
     * immediately call back core with no error
     */
    DBG_(self, "queue '%s'", dtmf);
    while (*dtmf) {
        gutil_ring_put(self->dtmf_queue, GUINT_TO_POINTER(*dtmf));
        dtmf++;
    }

    binder_voicecall_send_one_dtmf(self);
    cb(binder_error_ok(&err), data);
}

static
gboolean
binder_voicecall_ext_conference(
    BinderVoiceCall* self,
    BinderVoiceCallCbData* cbd)
{
    if (self->ext) {
        binder_ext_call_cancel(self->ext, self->ext_req_id);
        self->ext_req_id = binder_ext_call_conference(self->ext,
            BINDER_EXT_CALL_CONFERENCE_FLAGS_NONE,
            binder_voicecall_cbd_ext_complete,
            binder_voicecall_cbd_destroy, cbd);
        if (self->ext_req_id) {
            binder_voicecall_request_submitted(cbd);
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_voicecall_create_multiparty(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);
    RADIO_REQ req = self->api->voice_conference_req;

    if (binder_voicecall_have_ext_call(self)) {
        if (!binder_voicecall_ext_conference(self, cbd)) {
            DBG_(self, "(fallback)");
            binder_voicecall_request_submit(self, req, cbd);
        }
    } else {
        /* Default action */
        DBG_(self, "");
        binder_voicecall_request_submit(self, req, cbd);
    }
    binder_voicecall_cbd_unref(cbd);
}

static
gboolean
binder_voicecall_ext_transfer(
    BinderVoiceCall* self,
    BinderVoiceCallCbData* cbd)
{
    if (self->ext) {
        binder_ext_call_cancel(self->ext, self->ext_req_id);
        self->ext_req_id = binder_ext_call_transfer(self->ext,
            BINDER_EXT_CALL_TRANSFER_FLAGS_NONE,
            binder_voicecall_cbd_ext_complete,
            binder_voicecall_cbd_destroy, cbd);
        if (self->ext_req_id) {
            binder_voicecall_request_submitted(cbd);
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_voicecall_transfer(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);
    RADIO_REQ req = self->api->voice_explicit_call_transfer_req;

    if (binder_voicecall_have_ext_call(self)) {
        if (!binder_voicecall_ext_transfer(self, cbd)) {
            DBG_(self, "(fallback)");
            binder_voicecall_request_submit(self, req, cbd);
        }
    } else {
        /* Default action */
        DBG_(self, "");
        binder_voicecall_request_submit(self, req, cbd);
    }
    binder_voicecall_cbd_unref(cbd);
}

static
void
binder_voicecall_private_chat(
    struct ofono_voicecall* vc,
    int cid,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);

    /* separateConnection(int32 serial, int32 index) */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->voice_g,
        self->api->voice_separate_connection_req, &writer,
        binder_voicecall_cbd_complete, binder_voicecall_cbd_destroy, cbd);

    DBG_(self, "Private chat with id %d", cid);
    gbinder_writer_append_int32(&writer, cid);
    if (radio_request_submit(req)) {
        binder_voicecall_request_submitted(cbd);
    }
    radio_request_unref(req);
}

static
gboolean
binder_voicecall_ext_swap(
    BinderVoiceCall* self,
    BinderVoiceCallCbData* cbd,
    BINDER_EXT_CALL_SWAP_FLAGS swap_flags)
{
    if (self->ext) {
        binder_ext_call_cancel(self->ext, self->ext_req_id);
        self->ext_req_id = binder_ext_call_swap(self->ext, swap_flags,
            ANSWER_FLAGS, binder_voicecall_cbd_ext_complete,
            binder_voicecall_cbd_destroy, cbd);
        if (self->ext_req_id) {
            binder_voicecall_request_submitted(cbd);
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_voicecall_swap_with_fallback(
    struct ofono_voicecall* vc,
    BINDER_EXT_CALL_SWAP_FLAGS flags,
    RADIO_REQ fallback,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);

    if (binder_voicecall_have_ext_call(self)) {
        if (!binder_voicecall_ext_swap(self, cbd, flags)) {
            DBG_(self, "%s (fallback)", REQ_NAME_(self, fallback));
            binder_voicecall_request_submit(self, fallback, cbd);
        }
    } else {
        /* Default action */
        DBG_(self, "%s", REQ_NAME_(self, fallback));
        binder_voicecall_request_submit(self, fallback, cbd);
    }
    binder_voicecall_cbd_unref(cbd);
}

static
void
binder_voicecall_swap_without_accept(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    binder_voicecall_swap_with_fallback(vc, BINDER_EXT_CALL_SWAP_NO_FLAGS,
        self->api->voice_switch_waiting_or_holding_and_active_req, cb, data);
}

static
void
binder_voicecall_hold_all_active(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    DBG_(self, "");
    #pragma message("TODO: Revisit binder_voicecall_hold_all_active")
    binder_voicecall_swap_with_fallback(vc, BINDER_EXT_CALL_SWAP_NO_FLAGS,
        self->api->voice_switch_waiting_or_holding_and_active_req, cb, data);
}

static
void
binder_voicecall_release_all_active(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    DBG_(self, "");
    binder_voicecall_swap_with_fallback(vc, BINDER_EXT_CALL_SWAP_FLAG_HANGUP,
        self->api->voice_hangup_foreground_resume_background_req, cb, data);
}

static
void
binder_voicecall_hangup_with_fallback(
    struct ofono_voicecall* vc,
    gboolean (*filter)(const BinderVoiceCallInfo* call),
    BINDER_EXT_CALL_HANGUP_REASON reason,
    RADIO_REQ fallback,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = binder_voicecall_cbd_new(self, cb, data);

    /*
     * The idea is that we submit (potentially) multiple hangup
     * requests and invoke the callback after the last request
     * has completed (pending call count becomes zero).
     */
    if (self->ext) {
        gboolean use_fallback = FALSE;
        GSList* l;

        for (l = self->calls; l; l = l->next) {
            const BinderVoiceCallInfo* call = l->data;

            if (filter(call)) {
                const guint id = call->oc.id;

                if (call->ext) {
                    DBG_(self, "hanging up ext call id %u", id);
                    if (binder_ext_call_hangup(call->ext, id, reason,
                        BINDER_EXT_CALL_HANGUP_NO_FLAGS,
                        binder_voicecall_cbd_ext_complete,
                        binder_voicecall_cbd_destroy, cbd)) {
                        binder_voicecall_request_submitted(cbd);
                        continue;
                    }
                } else {
                    DBG_(self, "%s %u", REQ_NAME_(self, fallback), id);
                }

                /* Request wasn't submitted - will use the fallback */
                use_fallback = TRUE;
            }
        }

        if (use_fallback || !cbd->pending_call_count) {
            DBG_(self, "%s", REQ_NAME_(self, fallback));
            binder_voicecall_request_submit(self, fallback, cbd);
        }
    } else {
        DBG_(self, "%s", REQ_NAME_(self, fallback));
        binder_voicecall_request_submit(self, fallback, cbd);
    }
    binder_voicecall_cbd_unref(cbd);
}

static
void
binder_voicecall_release_all_held(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    DBG_(self, "");
    binder_voicecall_hangup_with_fallback(vc,
        binder_voicecall_hangup_filter_held,
        BINDER_EXT_CALL_HANGUP_REJECT,
        self->api->voice_hangup_waiting_or_background_req, cb, data);
}

static
void
binder_voicecall_set_udub(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    DBG_(self, "");
    binder_voicecall_hangup_with_fallback(vc,
        binder_voicecall_hangup_filter_incoming_waiting,
        BINDER_EXT_CALL_HANGUP_REJECT,
        self->api->voice_reject_call_req, cb, data);
}

static
void
binder_voicecall_enable_supp_svc(
    BinderVoiceCall* self)
{
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->network_g,
        self->api->network_set_supp_service_notifications_req, &args,
        NULL, NULL, NULL);

    /*
     * IRadio.hal:
     * oneway setSuppServiceNotifications(int32_t serial, bool enable);
     *
     * IRadioNetwork.aidl:
     * void setSuppServiceNotifications(in int serial, in boolean enable);
     */
    gbinder_writer_append_bool(&args, TRUE);

    radio_request_set_timeout(req, VOICECALL_BLOCK_TIMEOUT_MS);
    radio_request_set_blocking(req, TRUE);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_voicecall_ringback_tone_event(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    GBinderReader reader;
    gboolean start;

   /*
    * IRadioIndication.hal:
    * oneway indicateRingbackTone(RadioIndicationType type,
    *     bool start);
    *
    * IRadioVoiceIndication.aidl:
    * oneway void indicateRingbackTone(in RadioIndicationType type,
    *     in boolean start);
    */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_bool(&reader, &start)) {
        DBG_(self, "play ringback tone: %d", start);
        ofono_voicecall_ringback_tone_notify(self->vc, start);
    }
}

static
void
binder_voicecall_ecclist_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    GBinderReader reader;
    char** ecclist;

    /*
     * 1.4/IRadioIndication.hal:
     * oneway currentEmergencyNumberList(RadioIndicationType type,
     *     vec<EmergencyNumber> emergencyNumberList);
     *
     * IRadioVoiceIndication.aidl:
     * void currentEmergencyNumberList(in RadioIndicationType type,
     *     in EmergencyNumber[] emergencyNumberList);
     */
    gbinder_reader_copy(&reader, args);
    ecclist = self->api->read_ecclist(self, &reader);
    ofono_voicecall_en_list_notify(self->vc, ecclist);
    g_strfreev(ecclist);
}

static
void
binder_voicecall_ext_calls_changed(
    BinderExtCall* ext,
    void* user_data)
{
    BinderVoiceCall* self = user_data;
    const BinderExtCallInfo* const* calls = binder_ext_call_get_calls(ext);
    const BinderExtCallInfo* const* ptr = calls;
    GSList* list = NULL;

    /* binder_ext_call_get_calls never returns NULL */
    while (*ptr) {
        const BinderExtCallInfo* call = *ptr++;

        /* We don't report multiparty calls to the core */
        if (!(call->flags & BINDER_EXT_CALL_FLAG_MPTY)) {
            list = g_slist_insert_sorted(list,
                binder_voicecall_info_ext_new(call, ext),
                binder_voicecall_info_compare);
        }
    }

    /* Merge the IRadio calls back into the list */
    binder_voicecall_set_calls(self,
        binder_voicecall_merge_ext_calls(self, list, FALSE));
}

static
void
binder_voicecall_ext_call_disconnected(
    BinderExtCall* ext,
    guint call_id,
    BINDER_EXT_CALL_DISCONNECT_REASON reason,
    void* user_data)
{
    BinderVoiceCall* self = user_data;

    if (binder_voicecall_find_call_link_with_id(self, call_id)) {
        DBG_(self, "ext call %u disconnected", call_id);
        binder_voicecall_remove_call_id(self, call_id);
        binder_voicecall_clear_dtmf_queue(self);
        ofono_voicecall_disconnected(self->vc, call_id,
            gutil_int_array_remove_all_fast(self->local_release_ids, call_id) ?
            OFONO_DISCONNECT_REASON_LOCAL_HANGUP :
            (reason == BINDER_EXT_CALL_DISCONNECT_ERROR) ?
            OFONO_DISCONNECT_REASON_ERROR :
            OFONO_DISCONNECT_REASON_REMOTE_HANGUP, NULL);
    } else {
        /* We don't report multipart calls to the core */
        DBG_(self, "ignoring ext call %u hangup (mpty?)", call_id);
    }
}

static
void
binder_voicecall_ext_call_ringback_tone(
    BinderExtCall* ext,
    gboolean start,
    void* user_data)
{
    BinderVoiceCall* self = user_data;

    DBG_(self, "play ringback tone: %d", start);
    ofono_voicecall_ringback_tone_notify(self->vc, start);
}

static
void
binder_voicecall_register(
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    RadioClient* voice_client = self->voice_g->client;
    const BinderVoiceCallApi* api = self->api;

    ofono_voicecall_register(self->vc);

    /* Initialize call list */
    binder_voicecall_clcc_poll(self);

    /* Request supplementary service notifications*/
    binder_voicecall_enable_supp_svc(self);

    /* Unsol when call state changes */
    self->voice_event[VOICECALL_EVENT_CALL_STATE_CHANGED] =
        radio_client_add_indication_handler(voice_client,
            api->voice_call_state_changed_ind,
            binder_voicecall_call_state_changed_event, self);

    /* Register for ringback tone notifications */
    self->voice_event[VOICECALL_EVENT_RINGBACK_TONE] =
        radio_client_add_indication_handler(voice_client,
            api->voice_indicate_ringback_tone_ind,
            binder_voicecall_ringback_tone_event, self);

    /* Register for emergency number list notifications */
    self->voice_event[VOICECALL_EVENT_ECCLIST_CHANGED] =
        radio_client_add_indication_handler(voice_client,
            api->voice_current_emergency_number_list_ind,
            binder_voicecall_ecclist_changed, self);

    /* Unsol when call set in hold */
    if (api->voice_supp_svc_notify_ind) {
        self->voice_event[VOICECALL_EVENT_SUPP_SVC_NOTIFICATION] =
            radio_client_add_indication_handler(voice_client,
                api->voice_supp_svc_notify_ind,
                binder_voicecall_supp_svc_notification_cb, self);
    }

    if (api->network_supp_svc_notify_ind) {
        self->network_event[VOICECALL_NETWORK_EVENT_SUPP_SVC_NOTIFICATION] =
            radio_client_add_indication_handler(self->network_g->client,
                api->network_supp_svc_notify_ind,
                binder_voicecall_supp_svc_notification_cb, self);
    }

    /* Register extension event handlers if there is an extension */
    if (self->ext) {
        self->ext_event[VOICECALL_EXT_CALL_STATE_CHANGED] =
            binder_ext_call_add_calls_changed_handler(self->ext,
                binder_voicecall_ext_calls_changed, self);
        self->ext_event[VOICECALL_EXT_CALL_DISCONNECTED] =
            binder_ext_call_add_disconnect_handler(self->ext,
                binder_voicecall_ext_call_disconnected, self);
        self->ext_event[VOICECALL_EXT_CALL_SUPP_SVC_NOTIFICATION] =
            binder_ext_call_add_ssn_handler(self->ext,
                binder_voicecall_ext_supp_svc_notification, self);
        self->ext_event[VOICECALL_EXT_CALL_RINGBACK_TONE] =
            binder_ext_call_add_ringback_tone_handler(self->ext,
                binder_voicecall_ext_call_ringback_tone, self);
    }
}

static
int
binder_voicecall_probe(
    struct ofono_voicecall* vc,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderVoiceCall* self = g_new0(BinderVoiceCall, 1);
    const BinderSlotConfig* cfg = &modem->config;
    RadioClient* voice_client = modem->clients.voice_client;
    const BinderVoiceCallApi* api =
        radio_client_aidl_interface(voice_client) == RADIO_VOICE_INTERFACE ?
        &binder_voicecall_api_aidl : &binder_voicecall_api_hidl;

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "%s api", api->name);

    self->vc = vc;
    self->api = api;
    self->dtmf_queue = gutil_ring_new();
    self->voice_g = radio_request_group_new(voice_client);
    self->network_g = radio_request_group_new(modem->clients.network_client);
    self->local_hangup_reasons = gutil_ints_ref(cfg->local_hangup_reasons);
    self->remote_hangup_reasons = gutil_ints_ref(cfg->remote_hangup_reasons);
    self->local_release_ids = gutil_int_array_new();
    self->idleq = gutil_idle_queue_new();
    self->ims_reg = binder_ims_reg_ref(modem->ims);

    if (modem->ext && (self->ext =
        binder_ext_slot_get_interface(modem->ext,
        BINDER_EXT_TYPE_CALL)) != NULL) {
        DBG_(self, "using call extension");
        binder_ext_call_ref(self->ext);
    }

    binder_voicecall_clear_dtmf_queue(self);
    gutil_idle_queue_add(self->idleq, binder_voicecall_register, self);
    ofono_voicecall_set_data(vc, self);
    return 0;
}

static
void
binder_voicecall_remove(
    struct ofono_voicecall* vc)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    DBG_(self, "");
    g_slist_free_full(self->calls, binder_voicecall_info_free);

    radio_request_drop(self->send_dtmf_req);
    radio_request_drop(self->clcc_poll_req);

    radio_client_remove_all_handlers(self->voice_g->client,
        self->voice_event);
    radio_request_group_cancel(self->voice_g);
    radio_request_group_unref(self->voice_g);

    radio_client_remove_all_handlers(self->network_g->client,
        self->network_event);
    radio_request_group_cancel(self->network_g);
    radio_request_group_unref(self->network_g);

    gutil_ring_unref(self->dtmf_queue);
    gutil_ints_unref(self->local_hangup_reasons);
    gutil_ints_unref(self->remote_hangup_reasons);
    gutil_int_array_free(self->local_release_ids, TRUE);
    gutil_idle_queue_free(self->idleq);

    if (self->ext) {
        binder_ext_call_remove_all_handlers(self->ext, self->ext_event);
        binder_ext_call_cancel(self->ext, self->ext_send_dtmf_id);
        binder_ext_call_cancel(self->ext, self->ext_req_id);
        binder_ext_call_unref(self->ext);
    }

    binder_ims_reg_unref(self->ims_reg);
    g_free(self->log_prefix);
    g_free(self);

    ofono_voicecall_set_data(vc, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static
void
binder_voicecall_api_handle_supp_svc_notify_hidl(
    BinderVoiceCall* self,
    GBinderReader* reader)
{
    const RadioSuppSvcNotification* ssn =
        gbinder_reader_read_hidl_struct(reader, RadioSuppSvcNotification);

    if (ssn) {
        binder_voicecall_supp_svc_notification(self, ssn->isMT, ssn->code,
            ssn->index, ssn->type, ssn->number.data.str);
    }
}

static
void
binder_voicecall_api_write_dial_args_hidl(
    GBinderWriter* writer,
    const char* address,
    int clir)
{
    /* dial(int32 serial, Dial dialInfo) */
    GBinderParent parent;
    RadioDial* dialInfo = gbinder_writer_new0(writer, RadioDial);

    /* Prepare the Dial structure */
    dialInfo->clir = clir;
    binder_copy_hidl_string(writer, &dialInfo->address, address);

    /* Write the parent structure */
    parent.index = gbinder_writer_append_buffer_object(writer, dialInfo,
        sizeof(*dialInfo));

    /* Write the string data */
    binder_append_hidl_string_data(writer, dialInfo, address, parent.index);

    /* UUS information is empty but we still need to write a buffer */
    parent.offset = G_STRUCT_OFFSET(RadioDial, uusInfo.data.ptr);
    gbinder_writer_append_buffer_object_with_parent(writer, NULL, 0, &parent);
}

static
guint
binder_voicecall_api_read_last_call_fail_cause_hidl(
    GBinderReader* reader)
{
    gint32 cause_code = 0;
    const RadioLastCallFailCauseInfo* info =
        gbinder_reader_read_hidl_struct(reader, RadioLastCallFailCauseInfo);

    if (info) {
        if (info->vendorCause.len) {
            DBG("%s", info->vendorCause.data.str);
        }
        cause_code = info->causeCode;
    }
    return cause_code;
}

static
BinderVoiceCallInfo*
binder_voicecall_info_new_hidl(
    const RadioCall* rc)
{
    BinderVoiceCallInfo* call = g_slice_new0(BinderVoiceCallInfo);
    struct ofono_call* oc = &call->oc;

    ofono_call_init(oc);

    oc->status = rc->state;
    oc->id = rc->index;
    oc->direction = rc->isMT ?
        OFONO_CALL_DIRECTION_MOBILE_TERMINATED :
        OFONO_CALL_DIRECTION_MOBILE_ORIGINATED;
    oc->type = rc->isVoice ?
        OFONO_CALL_MODE_VOICE :
        OFONO_CALL_MODE_UNKNOWN;
    if (rc->name.len) {
        g_strlcpy(oc->name, rc->name.data.str, OFONO_MAX_CALLER_NAME_LENGTH);
    }
    oc->phone_number.type = rc->toa;
    if (rc->number.len) {
        oc->clip_validity = OFONO_CLIP_VALIDITY_VALID;
        g_strlcpy(oc->phone_number.number, rc->number.data.str,
            OFONO_MAX_PHONE_NUMBER_LENGTH);
    } else {
        oc->clip_validity = OFONO_CLIP_VALIDITY_NOT_AVAILABLE;
    }

    DBG("[id=%d,status=%d,type=%d,number=%s,name=%s]", oc->id,
        oc->status, oc->type, oc->phone_number.number, oc->name);

    return call;
}

static
GSList*
binder_voicecall_api_read_call_list_hidl(
    GBinderReader* reader,
    RADIO_RESP resp)
{
    GSList* list = NULL;
    gsize i, count;

    /*
     * 1.0/IRadioResponse.hal:
     * oneway getCurrentCallsResponse(RadioResponseInfo info, vec<Call> calls);
     *
     * 1.2/IRadioResponse.hal:
     * oneway getCurrentCallsResponse_1_2(RadioResponseInfo info,
     *     vec<Call> calls);
     */
    if (resp == RADIO_RESP_GET_CURRENT_CALLS) {
        const RadioCall* calls = gbinder_reader_read_hidl_type_vec(reader,
            RadioCall, &count);

        if (calls) {
            for (i = 0; i < count; i++) {
                list = g_slist_append(list,
                    binder_voicecall_info_new_hidl(calls + i));
            }
        }
    } else if (resp == RADIO_RESP_GET_CURRENT_CALLS_1_2) {
        const RadioCall_1_2* calls = gbinder_reader_read_hidl_type_vec(reader,
            RadioCall_1_2, &count);

        if (calls) {
            for (i = 0; i < count; i++) {
                list = g_slist_append(list,
                    binder_voicecall_info_new_hidl(&calls[i].base));
            }
        }
    } else {
        ofono_error("Unexpected getCurrentCalls response %d", resp);
    }

    return list;
}

static
char**
binder_voicecall_api_read_ecclist_hidl(
    BinderVoiceCall* self,
    GBinderReader* reader)
{
    char** ecclist = NULL;
    gsize count = 0;
    const RadioEmergencyNumber* list =
        gbinder_reader_read_hidl_type_vec(reader,
            RadioEmergencyNumber, &count);

    DBG_(self, "%u emergency number(s)", (guint) count);
    if (count) {
        char** ptr = ecclist = g_new(char*, count + 1);
        guint i;

        for (i = 0; i < count; i++, ptr++) {
            *ptr = g_strdup(list[i].number.data.str);
            DBG("  %s", *ptr);
        }

        *ptr = NULL;
    }
    return ecclist;
}

static const BinderVoiceCallApi binder_voicecall_api_hidl = {
    "hidl",
    binder_write_string_arg_hidl,
    RADIO_IND_CALL_STATE_CHANGED,
    RADIO_IND_INDICATE_RINGBACK_TONE,
    RADIO_IND_CURRENT_EMERGENCY_NUMBER_LIST,
    RADIO_IND_SUPP_SVC_NOTIFY,
    RADIO_IND_NONE,
    binder_voicecall_api_handle_supp_svc_notify_hidl,
    RADIO_REQ_SET_SUPP_SERVICE_NOTIFICATIONS,
    RADIO_REQ_DIAL,
    binder_voicecall_api_write_dial_args_hidl,
    RADIO_REQ_ACCEPT_CALL,
    RADIO_REQ_REJECT_CALL,
    RADIO_REQ_HANGUP,
    RADIO_REQ_CONFERENCE,
    RADIO_REQ_EXPLICIT_CALL_TRANSFER,
    RADIO_REQ_SEPARATE_CONNECTION,
    RADIO_REQ_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE,
    RADIO_REQ_HANGUP_FOREGROUND_RESUME_BACKGROUND,
    RADIO_REQ_HANGUP_WAITING_OR_BACKGROUND,
    RADIO_REQ_SEND_DTMF,
    RADIO_REQ_GET_LAST_CALL_FAIL_CAUSE,
    binder_voicecall_api_read_last_call_fail_cause_hidl,
    RADIO_REQ_GET_CURRENT_CALLS,
    binder_voicecall_api_read_call_list_hidl,
    binder_voicecall_api_read_ecclist_hidl
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static
void
binder_voicecall_api_handle_supp_svc_notify_aidl(
    BinderVoiceCall* self,
    GBinderReader* reader)
{
    GBinderReader parcel;

    /*
     * package android.hardware.radio.network;
     * parcelable SuppSvcNotification {
     *   boolean isMT;
     *   int code;
     *   int index;
     *   int type;
     *   String number;
     * }
     */
    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        gboolean isMT;
        gint32 code, index, type;
        char* number;

        if (gbinder_reader_read_bool(&parcel, &isMT) &&
            gbinder_reader_read_int32(&parcel, &code) &&
            gbinder_reader_read_int32(&parcel, &index) &&
            gbinder_reader_read_int32(&parcel, &type) &&
            gbinder_reader_read_nullable_string16(&parcel, &number)) {
            binder_voicecall_supp_svc_notification(self, isMT, code, index,
                type, number);
            g_free(number);
        }
        gbinder_reader_finish_parcelable(&parcel);
    }
}

static
void
binder_voicecall_api_write_dial_args_aidl(
    GBinderWriter* writer,
    const char* address,
    int clir)
{
    GBinderWriter parcel;

    /*
     * package android.hardware.radio.voice;
     * parcelable Dial {
     *   String address;
     *   int clir;
     *   UusInfo[] uusInfo;
     * }
     *
     * dial(in int serial, in Dial dialInfo)
     */
    gbinder_writer_start_parcelable(writer, &parcel);
    gbinder_writer_append_string16(&parcel, address);
    gbinder_writer_append_int32(&parcel, clir);
    gbinder_writer_append_int32(&parcel, 0); /* no uusInfo */
    gbinder_writer_finish_parcelable(&parcel);
}

static
guint
binder_voicecall_api_read_last_call_fail_cause_aidl(
    GBinderReader* reader)
{
    gint32 cause_code = 0;
    GBinderReader parcel;

    /*
     * package android.hardware.radio.voice;
     * parcelable LastCallFailCauseInfo {
     *   LastCallFailCause causeCode;
     *   String vendorCause;
     * }
     */
    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        if (gbinder_reader_read_int32(&parcel, &cause_code)) {
            char* vendor_cause = gbinder_reader_read_string16(&parcel);

            if (vendor_cause) {
                DBG("%s", vendor_cause);
                g_free(vendor_cause);
            }
        }
        gbinder_reader_finish_parcelable(&parcel);
    }
    return cause_code;
}

static
BinderVoiceCallInfo*
binder_voicecall_info_new_aidl(
    GBinderReader* reader)
{
    BinderVoiceCallInfo* call = NULL;
    GBinderReader parcel;

    /*
     * package android.hardware.radio.voice;
     * parcelable Call {
     *   int state;
     *   int index;
     *   int toa;
     *   boolean isMpty;
     *   boolean isMT;
     *   byte als;
     *   boolean isVoice;
     *   boolean isVoicePrivacy;
     *   String number;
     *   int numberPresentation;
     *   String name;
     *   int namePresentation;
     *   android.hardware.radio.voice.UusInfo[] uusInfo;
     *   android.hardware.radio.voice.AudioQuality audioQuality;
     *   String forwardedNumber;
     * }
     */
    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        gint32 state, index, toa, numberPresentation;
        gboolean isMpty, isMT, isVoice, isVoicePrivacy;
        gint8 als;
        char* number = NULL;
        char* name;

        if (gbinder_reader_read_int32(&parcel, &state) &&
            gbinder_reader_read_int32(&parcel, &index) &&
            gbinder_reader_read_int32(&parcel, &toa) &&
            gbinder_reader_read_bool(&parcel, &isMpty) &&
            gbinder_reader_read_bool(&parcel, &isMT) &&
            gbinder_reader_read_int8(&parcel, &als) &&
            gbinder_reader_read_bool(&parcel, &isVoice) &&
            gbinder_reader_read_bool(&parcel, &isVoicePrivacy) &&
            gbinder_reader_read_nullable_string16(&parcel, &number) &&
            gbinder_reader_read_int32(&parcel, &numberPresentation) &&
            gbinder_reader_read_nullable_string16(&parcel, &name)) {
            struct ofono_call* oc;

            call = g_slice_new0(BinderVoiceCallInfo);
            oc = &call->oc;
            ofono_call_init(oc);

            oc->id = index;
            oc->status = state;
            oc->direction = isMT ?
                OFONO_CALL_DIRECTION_MOBILE_TERMINATED :
                OFONO_CALL_DIRECTION_MOBILE_ORIGINATED;
            oc->type =  isVoice ?
                OFONO_CALL_MODE_VOICE :
                OFONO_CALL_MODE_UNKNOWN;

            oc->phone_number.type = toa;
            if (number && strlen(number)) {
                oc->clip_validity = OFONO_CLIP_VALIDITY_VALID;
                g_strlcpy(oc->phone_number.number, number,
                    OFONO_MAX_PHONE_NUMBER_LENGTH);
            } else {
                oc->clip_validity = OFONO_CLIP_VALIDITY_NOT_AVAILABLE;
            }

            if (name) {
                g_strlcpy(oc->name, name, OFONO_MAX_CALLER_NAME_LENGTH);
            }

            DBG("[id=%d,status=%d,type=%d,number=%s,name=%s]", oc->id,
                oc->status, oc->type, oc->phone_number.number, oc->name);
        }
        g_free(name);
        g_free(number);
        gbinder_reader_finish_parcelable(&parcel);
    }
    return call;
}

static
GSList*
binder_voicecall_api_read_call_list_aidl(
    GBinderReader* reader,
    RADIO_RESP resp)
{
    gint32 i, count = 0;
    GSList* list = NULL;

    /*
     * IRadioVoiceResponse.aidl:
     * void getCurrentCallsResponse(in RadioResponseInfo info, in Call[] calls);
     */
    gbinder_reader_read_int32(reader, &count);
    for (i = 0; i < count; i++) {
        BinderVoiceCallInfo* call = binder_voicecall_info_new_aidl(reader);

        if (call) {
            list = g_slist_append(list, call);
        }
    }

    return list;
}

static
char**
binder_voicecall_api_read_ecclist_aidl(
    BinderVoiceCall* self,
    GBinderReader* reader)
{
    char** ecclist = NULL;
    guint32 count;

    if (gbinder_reader_read_uint32(reader, &count)) {
        char** ptr = ecclist = g_new(char*, count + 1);
        guint i;

        /*
         * package android.hardware.radio.voice;
         * parcelable EmergencyNumber {
         *   String number;
         *   String mcc;
         *   String mnc;
         *   int categories;
         *   String[] urns;
         *   int sources;
         * }
         */
        DBG_(self, "%u emergency number(s)", count);
        for (i = 0; i < count; i++) {
            GBinderReader en;

            if (gbinder_reader_start_parcelable(reader, &en, NULL)) {
                char* number = gbinder_reader_read_string16(&en);

                if (number) {
                    DBG("  %s", number);
                    *ptr++ = number;
                }
                gbinder_reader_finish_parcelable(&en);
            }
        }
        *ptr = NULL;
    }
    return ecclist;
}

static const BinderVoiceCallApi binder_voicecall_api_aidl = {
    "aidl",
    binder_write_string_arg_aidl,
    RADIO_VOICE_IND_CALL_STATE_CHANGED,
    RADIO_VOICE_IND_INDICATE_RINGBACK_TONE,
    RADIO_VOICE_IND_CURRENT_EMERGENCY_NUMBER_LIST,
    RADIO_IND_NONE,
    RADIO_NETWORK_IND_SUPP_SVC_NOTIFY,
    binder_voicecall_api_handle_supp_svc_notify_aidl,
    RADIO_NETWORK_REQ_SET_SUPP_SERVICE_NOTIFICATIONS,
    RADIO_VOICE_REQ_DIAL,
    binder_voicecall_api_write_dial_args_aidl,
    RADIO_VOICE_REQ_ACCEPT_CALL,
    RADIO_VOICE_REQ_REJECT_CALL,
    RADIO_VOICE_REQ_HANGUP,
    RADIO_VOICE_REQ_CONFERENCE,
    RADIO_VOICE_REQ_EXPLICIT_CALL_TRANSFER,
    RADIO_VOICE_REQ_SEPARATE_CONNECTION,
    RADIO_VOICE_REQ_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE,
    RADIO_VOICE_REQ_HANGUP_FOREGROUND_RESUME_BACKGROUND,
    RADIO_VOICE_REQ_HANGUP_WAITING_OR_BACKGROUND,
    RADIO_VOICE_REQ_SEND_DTMF,
    RADIO_VOICE_REQ_GET_LAST_CALL_FAIL_CAUSE,
    binder_voicecall_api_read_last_call_fail_cause_aidl,
    RADIO_VOICE_REQ_GET_CURRENT_CALLS,
    binder_voicecall_api_read_call_list_aidl,
    binder_voicecall_api_read_ecclist_aidl
};

/*==========================================================================*
 * API
 *==========================================================================*/

#pragma message("TODO: Implement deflect callback?")

static const struct ofono_voicecall_driver binder_voicecall_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_voicecall_probe,
    .remove                 = binder_voicecall_remove,
    .dial                   = binder_voicecall_dial,
    .answer                 = binder_voicecall_answer,
    .hangup_active          = binder_voicecall_hangup_active,
    .hangup_all             = binder_voicecall_hangup_all,
    .hold_all_active        = binder_voicecall_hold_all_active,
    .release_all_held       = binder_voicecall_release_all_held,
    .set_udub               = binder_voicecall_set_udub,
    .release_all_active     = binder_voicecall_release_all_active,
    .release_specific       = binder_voicecall_release_specific,
    .private_chat           = binder_voicecall_private_chat,
    .create_multiparty      = binder_voicecall_create_multiparty,
    .transfer               = binder_voicecall_transfer,
    .swap_without_accept    = binder_voicecall_swap_without_accept,
    .send_tones             = binder_voicecall_send_dtmf
};

void
binder_voicecall_init()
{
    ofono_voicecall_driver_register(&binder_voicecall_driver);
}

void
binder_voicecall_cleanup()
{
    ofono_voicecall_driver_unregister(&binder_voicecall_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
