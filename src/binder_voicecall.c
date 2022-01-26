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

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"
#include "binder_voicecall.h"

#include <ofono/log.h>
#include <ofono/misc.h>
#include <ofono/voicecall.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_idlequeue.h>
#include <gutil_intarray.h>
#include <gutil_ints.h>
#include <gutil_macros.h>
#include <gutil_ring.h>

#define VOICECALL_BLOCK_TIMEOUT_MS (5*1000)

enum binder_voicecall_events {
    VOICECALL_EVENT_CALL_STATE_CHANGED,
    VOICECALL_EVENT_SUPP_SVC_NOTIFICATION,
    VOICECALL_EVENT_RINGBACK_TONE,
    VOICECALL_EVENT_ECCLIST_CHANGED, /* IRadio@1.4 specific */
    VOICECALL_EVENT_COUNT
};

typedef struct binder_voicecall {
    struct ofono_voicecall* vc;
    char* log_prefix;
    GSList* calls;
    RadioRequestGroup* g;
    ofono_voicecall_cb_t cb;
    void* data;
    GUtilIntArray* local_release_ids;
    GUtilIdleQueue* idleq;
    GUtilRing* dtmf_queue;
    GUtilInts* local_hangup_reasons;
    GUtilInts* remote_hangup_reasons;
    RadioRequest* send_dtmf_req;
    RadioRequest* clcc_poll_req;
    gulong event_id[VOICECALL_EVENT_COUNT];
    gulong supp_svc_notification_id;
    gulong ringback_tone_event_id;
} BinderVoiceCall;

typedef struct binder_voicecall_request_data {
    int ref_count;
    int pending_call_count;
    int success;
    struct ofono_voicecall* vc;
    ofono_voicecall_cb_t cb;
    gpointer data;
} BinderVoiceCallCbData;

typedef struct binder_voicecall_lastcause_data {
    BinderVoiceCall* self;
    guint cid;
} BinderVoiceCallLastCauseData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)
#define DBG__(vc,fmt,args...) \
    DBG("%s" fmt, binder_voicecall_get_data(vc)->log_prefix, ##args)

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
binder_voicecall_request_data_new(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCallCbData* req = g_slice_new0(BinderVoiceCallCbData);

    req->ref_count = 1;
    req->vc = vc;
    req->cb = cb;
    req->data = data;
    return req;
}

static
void
binder_voicecall_request_data_unref(
    BinderVoiceCallCbData* req)
{
    if (!--req->ref_count) {
        gutil_slice_free(req);
    }
}

static
void
binder_voicecall_request_data_free(
    gpointer data)
{
    binder_voicecall_request_data_unref(data);
}

static
void
binder_voicecall_clear_dtmf_queue(
    BinderVoiceCall* self)
{
    gutil_ring_clear(self->dtmf_queue);
    radio_request_drop(self->send_dtmf_req);
    self->send_dtmf_req = NULL;
}

static
gint
binder_voicecall_compare(
    gconstpointer a,
    gconstpointer b)
{
    const struct ofono_call* ca = a;
    const struct ofono_call* cb = b;

    return (ca->id < cb->id) ? -1 :
        (ca->id > cb->id) ? 1 : 0;
}

static
void
binder_voicecall_ofono_call_free(
    gpointer data)
{
    g_slice_free(struct ofono_call, data);
}

static
struct ofono_call*
binder_voicecall_ofono_call_new(
    const RadioCall* rc)
{
    struct ofono_call* call = g_slice_new0(struct ofono_call);

    ofono_call_init(call);

    call->status = rc->state;
    call->id = rc->index;
    call->direction = rc->isMT ?
        OFONO_CALL_DIRECTION_MOBILE_TERMINATED :
        OFONO_CALL_DIRECTION_MOBILE_ORIGINATED;
    call->type = rc->isVoice ?
        OFONO_CALL_MODE_VOICE :
        OFONO_CALL_MODE_UNKNOWN;
    if (rc->name.len) {
        g_strlcpy(call->name, rc->name.data.str, OFONO_MAX_CALLER_NAME_LENGTH);
    }
    call->phone_number.type = rc->toa;
    if (rc->number.len) {
        call->clip_validity = OFONO_CLIP_VALIDITY_VALID;
        g_strlcpy(call->phone_number.number, rc->number.data.str,
                OFONO_MAX_PHONE_NUMBER_LENGTH);
    } else {
        call->clip_validity = OFONO_CLIP_VALIDITY_NOT_AVAILABLE;
    }

    DBG("[id=%d,status=%d,type=%d,number=%s,name=%s]", call->id,
        call->status, call->type, call->phone_number.number, call->name);

    return call;
}

static
enum ofono_call_status
binder_voicecall_status_with_id(
    struct ofono_voicecall *vc,
    unsigned int id)
{
    struct ofono_call *call = ofono_voicecall_find_call(vc, id);

    /* Valid call statuses have value >= 0 */
    return call ? call->status : -1;
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
        DBG("hangup cause %d => local hangup", last_cause);
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
            call_status = binder_voicecall_status_with_id(self->vc, cid);
            if (call_status == OFONO_CALL_STATUS_ACTIVE ||
                call_status == OFONO_CALL_STATUS_HELD ||
                call_status == OFONO_CALL_STATUS_DIALING ||
                call_status == OFONO_CALL_STATUS_ALERTING) {
                return OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
            } else if (call_status == OFONO_CALL_STATUS_INCOMING) {
                return OFONO_DISCONNECT_REASON_LOCAL_HANGUP;
            }
            break;

        case RADIO_LAST_CALL_FAIL_ERROR_UNSPECIFIED:
            call_status = binder_voicecall_status_with_id(self->vc, cid);
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

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            if (resp == RADIO_RESP_GET_LAST_CALL_FAIL_CAUSE) {
                GBinderReader reader;
                const RadioLastCallFailCauseInfo* info;

                /*
                 * getLastCallFailCauseResponse(RadioResponseInfo,
                 *   LastCallFailCauseInfo failCauseinfo);
                 */
                gbinder_reader_copy(&reader, args);
                info = gbinder_reader_read_hidl_struct(&reader,
                    RadioLastCallFailCauseInfo);
                if (info) {
                    enum ofono_disconnect_reason reason =
                        binder_voicecall_map_cause(self, cid, info->causeCode);

                    ofono_info("Call %d ended with cause %d -> ofono reason %d",
                        cid, info->causeCode, reason);
                    ofono_voicecall_disconnected(vc, cid, reason, NULL);
                    return;
                }
            } else {
                ofono_error("Unexpected getLastCallFailCause response %d",
                    resp);
            }
        } else {
            ofono_warn("Failed to retrive last call fail cause: %s",
                binder_radio_error_string(error));
        }
    }

    ofono_info("Call %d ended with unknown reason", cid);
    ofono_voicecall_disconnected(vc, cid, OFONO_DISCONNECT_REASON_ERROR, NULL);
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
    struct ofono_voicecall* vc = self->vc;
    struct ofono_error err;
    GSList* list = NULL;
    GSList* n;
    GSList* o;

    GASSERT(self->clcc_poll_req == req);
    radio_request_unref(self->clcc_poll_req);
    self->clcc_poll_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            GBinderReader reader;
            gsize i, count = 0;

            /* getCurrentCallsResponse(RadioResponseInfo, vec<Call> calls); */
            gbinder_reader_copy(&reader, args);
            if (resp == RADIO_RESP_GET_CURRENT_CALLS) {
                const RadioCall* calls =
                    gbinder_reader_read_hidl_type_vec(&reader,
                        RadioCall, &count);

                if (calls) {
                    /* Build sorted list */
                    for (i = 0; i < count; i++) {
                        list = g_slist_insert_sorted(list,
                            binder_voicecall_ofono_call_new(calls + i),
                            binder_voicecall_compare);
                    }
                }
            } else if (resp == RADIO_RESP_GET_CURRENT_CALLS_1_2) {
                const RadioCall_1_2* calls =
                    gbinder_reader_read_hidl_type_vec(&reader,
                        RadioCall_1_2, &count);

                if (calls) {
                    /* Build sorted list */
                    for (i = 0; i < count; i++) {
                        list = g_slist_insert_sorted(list,
                            binder_voicecall_ofono_call_new(&calls[i].base),
                            binder_voicecall_compare);
                    }
                }
            } else {
                ofono_error("Unexpected getCurrentCalls response %d", resp);
            }
        } else {
            /*
             * Only RADIO_ERROR_NONE and RADIO_ERROR_RADIO_NOT_AVAILABLE
             * are expected here, all other errors are filtered out by
             * binder_voicecall_clcc_retry()
             */
            GASSERT(error == RADIO_ERROR_RADIO_NOT_AVAILABLE);
        }
    }

    /* Note: the lists are sorted by id */

    n = list;
    o = self->calls;

    while (n || o) {
        struct ofono_call* nc = n ? n->data : NULL;
        struct ofono_call* oc = o ? o->data : NULL;

        if (oc && (!nc || (nc->id > oc->id))) {
            /* old call is gone */
            if (gutil_int_array_remove_all_fast(self->local_release_ids,
                oc->id)) {
                ofono_voicecall_disconnected(vc, oc->id,
                    OFONO_DISCONNECT_REASON_LOCAL_HANGUP, NULL);
            } else {
                /* Get disconnect cause before informing oFono core */
                BinderVoiceCallLastCauseData* reqdata =
                    g_new0(BinderVoiceCallLastCauseData, 1);
                /* getLastCallFailCause(int32 serial); */
                RadioRequest* req2 = radio_request_new2(self->g,
                    RADIO_REQ_GET_LAST_CALL_FAIL_CAUSE, NULL,
                    binder_voicecall_lastcause_cb, g_free, reqdata);

                reqdata->self = self;
                reqdata->cid = oc->id;
                radio_request_submit(req2);
                radio_request_unref(req2);
            }

            binder_voicecall_clear_dtmf_queue(self);
            o = o->next;

        } else if (nc && (!oc || (nc->id < oc->id))) {
            /* new call, signal it */
            if (nc->type == OFONO_CALL_MODE_VOICE) {
                ofono_voicecall_notify(vc, nc);
                if (self->cb) {
                    ofono_voicecall_cb_t cb = self->cb;
                    void* cbdata = self->data;

                    self->cb = NULL;
                    self->data = NULL;
                    cb(binder_error_ok(&err), cbdata);
                }
            }

            n = n->next;

        } else {
            /* Both old and new call exist */
            if (memcmp(nc, oc, sizeof(*nc))) {
                ofono_voicecall_notify(vc, nc);
            }
            n = n->next;
            o = o->next;
        }
    }

    g_slist_free_full(self->calls, binder_voicecall_ofono_call_free);
    self->calls = list;
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
        /* getCurrentCalls(int32 serial); */
        RadioRequest* req = radio_request_new(self->g->client,
            RADIO_REQ_GET_CURRENT_CALLS, NULL,
            binder_voicecall_clcc_poll_cb, NULL, self);

        radio_request_set_retry(req, BINDER_RETRY_MS, -1);
        radio_request_set_retry_func(req, binder_voicecall_clcc_retry);
        if (radio_request_submit(req)) {
            self->clcc_poll_req = req;
        } else {
            radio_request_unref(req);
        }
    }
}

static
void
binder_voicecall_request_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCallCbData* data = user_data;
    BinderVoiceCall* self = binder_voicecall_get_data(data->vc);

    binder_voicecall_clcc_poll(self);

    /*
     * The ofono API call is considered successful if at least one
     * associated request succeeds.
     */
    if (status == RADIO_TX_STATUS_OK && error == RADIO_ERROR_NONE) {
        data->success++;
    }

    /*
     * Only invoke the callback if this is the last request associated
     * with this ofono api call (pending call count becomes zero).
     */
    GASSERT(data->pending_call_count > 0);
    if (!--data->pending_call_count && data->cb) {
        struct ofono_error error;

        if (data->success) {
            binder_error_init_ok(&error);
        } else {
            binder_error_init_failure(&error);
        }

        data->cb(&error, data->data);
    }
}

static
void
binder_voicecall_request(
    struct ofono_voicecall* vc,
    RADIO_REQ code,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd =
        binder_voicecall_request_data_new(vc, cb, data);
    RadioRequest* req = radio_request_new2(self->g, code, NULL,
        binder_voicecall_request_cb, binder_voicecall_request_data_free, cbd);

    if (radio_request_submit(req)) {
        cbd->pending_call_count++;
    } else {
        binder_voicecall_request_data_unref(cbd);
    }
    radio_request_unref(req);
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

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            if (resp == RADIO_RESP_DIAL) {
                if (self->cb) {
                    /*
                     * CLCC will update the oFono call list with
                     * proper ids if it's not done yet.
                     */
                    binder_voicecall_clcc_poll(self);
                    return;
                }
            } else {
                ofono_error("Unexpected dial response %d", resp);
            }
        } else {
            ofono_error("call failed: %s", binder_radio_error_string(error));
        }
    }

    /*
     * Even though this dial request may have already been completed
     * successfully by binder_voicecall_clcc_poll_cb, RADIO_REQ_DIAL
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
binder_voicecall_dial(
    struct ofono_voicecall* vc,
    const struct ofono_phone_number* ph,
    enum ofono_clir_option clir,
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    char phbuf[OFONO_PHONE_NUMBER_BUFFER_SIZE];
    const char* phstr = ofono_phone_number_to_string(ph, phbuf);
    GBinderParent parent;
    RadioDial* dialInfo;

    /* dial(int32 serial, Dial dialInfo) */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g, RADIO_REQ_DIAL, &writer,
        binder_voicecall_dial_cb, NULL, self);

    ofono_info("dialing \"%s\"", phstr);
    DBG_(self, "%s,%d,0", phstr, clir);
    GASSERT(!self->cb);
    self->cb = cb;
    self->data = data;

    /* Prepare the Dial structure */
    dialInfo = gbinder_writer_new0(&writer, RadioDial);
    dialInfo->clir = clir;
    binder_copy_hidl_string(&writer, &dialInfo->address, phstr);

    /* Write the parent structure */
    parent.index = gbinder_writer_append_buffer_object(&writer, dialInfo,
        sizeof(*dialInfo));

    /* Write the string data */
    binder_append_hidl_string_data(&writer, dialInfo, address, parent.index);

    /* UUS information is empty but we still need to write a buffer */
    parent.offset = G_STRUCT_OFFSET(RadioDial, uusInfo.data.ptr);
    gbinder_writer_append_buffer_object_with_parent(&writer, NULL, 0, &parent);

    /* Submit the request */
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_voicecall_submit_hangup_req(
    struct ofono_voicecall* vc,
    guint cid,
    BinderVoiceCallCbData* cbd)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);

    /* hangup(int32 serial, int32 index) */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g, RADIO_REQ_HANGUP, &writer,
        binder_voicecall_request_cb, binder_voicecall_request_data_free, cbd);

    gbinder_writer_append_int32(&writer, cid);

    /* Append the call id to the list of calls being released locally */
    GASSERT(!gutil_int_array_contains(self->local_release_ids, cid));
    gutil_int_array_append(self->local_release_ids, cid);

    /* binder_voicecall_request_data_free will unref the request data */
    if (radio_request_submit(req)) {
        cbd->ref_count++;
        cbd->pending_call_count++;
    }
    radio_request_unref(req);
}

static
void
binder_voicecall_hangup(
    struct ofono_voicecall* vc,
    gboolean (*filter)(struct ofono_call* call),
    ofono_voicecall_cb_t cb,
    void* data)
{
    BinderVoiceCall* self = binder_voicecall_get_data(vc);
    BinderVoiceCallCbData* cbd = NULL;
    GSList *l;

    /*
     * Here the idea is that we submit (potentially) multiple
     * hangup requests to BINDER and invoke the callback after
     * the last request has completed (pending call count
     * becomes zero).
     */
    for (l = self->calls; l; l = l->next) {
        struct ofono_call* call = l->data;

        if (!filter || filter(call)) {
            if (!cbd) {
                cbd = binder_voicecall_request_data_new(vc, cb, data);
            }

            /* Send request to BINDER */
            DBG("Hanging up call with id %d", call->id);
            binder_voicecall_submit_hangup_req(vc, call->id, cbd);
        } else {
            DBG("Skipping call with id %d", call->id);
        }
    }

    if (cbd) {
        /* Release our reference (if any) */
        binder_voicecall_request_data_unref(cbd);
    } else {
        /* No requests were submitted */
        struct ofono_error err;

        cb(binder_error_ok(&err), data);
    }
}

static
gboolean
binder_voicecall_hangup_active_filter(
    struct ofono_call *call)
{
    switch (call->status) {
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
void
binder_voicecall_hangup_active(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    binder_voicecall_hangup(vc,
        binder_voicecall_hangup_active_filter, cb, data);
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
    BinderVoiceCallCbData* req =
        binder_voicecall_request_data_new(vc, cb, data);

    DBG("Hanging up call with id %d", id);
    binder_voicecall_submit_hangup_req(vc, id, req);
    binder_voicecall_request_data_unref(req);
}

static
void
binder_voicecall_call_state_changed_event(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;

    GASSERT(code == RADIO_IND_CALL_STATE_CHANGED);

    /* Just need to request the call list again */
    binder_voicecall_clcc_poll(self);
}

static
void
binder_voicecall_supp_svc_notification_event(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    const RadioSuppSvcNotification* ntf;
    GBinderReader reader;

    gbinder_reader_copy(&reader, args);
    ntf = gbinder_reader_read_hidl_struct(&reader, RadioSuppSvcNotification);
    if (ntf) {
        DBG_(self, "MT/MO: %d, code: %d, index: %d",  ntf->isMT,
            ntf->code, ntf->index);

        if (ntf->isMT) {
            struct ofono_phone_number phone;

            if (ntf->number.data.str) {
                g_strlcpy(phone.number, ntf->number.data.str,
                    sizeof(phone.number));
            } else {
                phone.number[0] = 0;
            }

            /* MT unsolicited result code */
            ofono_voicecall_ssn_mt_notify(self->vc, 0, ntf->code, ntf->index,
                &phone);
        } else {
            /* MO intermediate result code */
            ofono_voicecall_ssn_mo_notify(self->vc, 0, ntf->code, ntf->index);
        }
    }
}

static
void
binder_voicecall_answer(
    struct ofono_voicecall *vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "Answering current call");
    binder_voicecall_request(vc, RADIO_REQ_ACCEPT_CALL, cb, data);
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

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            if (resp == RADIO_RESP_SEND_DTMF) {
                /* Send the next one */
                binder_voicecall_send_one_dtmf(self);
                return;
            } else {
                ofono_error("Unexpected sendDtmf response %d", resp);
            }
        } else {
            ofono_error("failed to senf dtmf: %s",
                binder_radio_error_string(error));
        }
    }
    binder_voicecall_clear_dtmf_queue(self);
}

static
void
binder_voicecall_send_one_dtmf(
    BinderVoiceCall* self)
{
    if (!self->send_dtmf_req && gutil_ring_size(self->dtmf_queue) > 0) {
        /* sendDtmf(int32 serial, string s) */
        GBinderWriter writer;
        RadioRequest* req = radio_request_new(self->g->client,
            RADIO_REQ_SEND_DTMF, &writer,
            binder_voicecall_send_dtmf_cb, NULL, self);
        char dtmf_str[2];

        dtmf_str[0] = (char)GPOINTER_TO_UINT(gutil_ring_get(self->dtmf_queue));
        dtmf_str[1] = 0;
        gbinder_writer_append_hidl_string_copy(&writer, dtmf_str);

        DBG_(self, "%s", dtmf_str);
        if (radio_request_submit(req)) {
            self->send_dtmf_req = req;
        } else {
            radio_request_unref(req);
            binder_voicecall_clear_dtmf_queue(self);
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
    DBG("Queue '%s'", dtmf);
    while (*dtmf) {
        gutil_ring_put(self->dtmf_queue, GUINT_TO_POINTER(*dtmf));
        dtmf++;
    }

    binder_voicecall_send_one_dtmf(self);
    cb(binder_error_ok(&err), data);
}

static
void
binder_voicecall_create_multiparty(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "");
    binder_voicecall_request(vc, RADIO_REQ_CONFERENCE, cb, data);
}

static
void
binder_voicecall_transfer(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "");
    binder_voicecall_request(vc, RADIO_REQ_EXPLICIT_CALL_TRANSFER, cb, data);
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
    BinderVoiceCallCbData* cbd =
        binder_voicecall_request_data_new(vc, cb, data);

    /* separateConnection(int32 serial, int32 index) */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SEPARATE_CONNECTION, &writer,
        binder_voicecall_request_cb, binder_voicecall_request_data_free, cbd);

    DBG_(self, "Private chat with id %d", cid);
    gbinder_writer_append_int32(&writer, cid);
    if (radio_request_submit(req)) {
        cbd->ref_count++;
        cbd->pending_call_count++;
    }
    radio_request_unref(req);
}

static
void
binder_voicecall_swap_without_accept(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "");
    binder_voicecall_request(vc,
        RADIO_REQ_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE, cb, data);
}

static
void
binder_voicecall_release_all_held(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "");
    binder_voicecall_request(vc,
        RADIO_REQ_HANGUP_WAITING_OR_BACKGROUND, cb, data);
}

static
void
binder_voicecall_release_all_active(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "");
    binder_voicecall_request(vc,
        RADIO_REQ_HANGUP_FOREGROUND_RESUME_BACKGROUND, cb, data);
}

static
void
binder_voicecall_set_udub(
    struct ofono_voicecall* vc,
    ofono_voicecall_cb_t cb,
    void* data)
{
    DBG__(vc, "");
    binder_voicecall_request(vc, RADIO_REQ_REJECT_CALL, cb, data);
}

static
void
binder_voicecall_enable_supp_svc(
    BinderVoiceCall* self)
{
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SET_SUPP_SERVICE_NOTIFICATIONS, &writer,
        NULL, NULL, NULL);

    /* setSuppServiceNotifications(int32 serial, bool enable); */
    gbinder_writer_append_bool(&writer, TRUE);
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

    /* indicateRingbackTone(RadioIndicationType, bool start) */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_bool(&reader, &start)) {
        DBG("play ringback tone: %d", start);
        ofono_voicecall_ringback_tone_notify(self->vc, start);
    }
}

static
void
binder_voicecall_register(
    gpointer user_data)
{
    BinderVoiceCall* self = user_data;
    RadioClient* client = self->g->client;

    ofono_voicecall_register(self->vc);

    /* Initialize call list */
    binder_voicecall_clcc_poll(self);

    /* Request supplementary service notifications*/
    binder_voicecall_enable_supp_svc(self);

    /* Unsol when call state changes */
    self->event_id[VOICECALL_EVENT_CALL_STATE_CHANGED] =
        radio_client_add_indication_handler(client,
            RADIO_IND_CALL_STATE_CHANGED,
            binder_voicecall_call_state_changed_event, self);

    /* Unsol when call set in hold */
    self->event_id[VOICECALL_EVENT_SUPP_SVC_NOTIFICATION] =
        radio_client_add_indication_handler(client,
            RADIO_IND_SUPP_SVC_NOTIFY,
            binder_voicecall_supp_svc_notification_event, self);

    /* Register for ringback tone notifications */
    self->event_id[VOICECALL_EVENT_RINGBACK_TONE] =
        radio_client_add_indication_handler(client,
            RADIO_IND_INDICATE_RINGBACK_TONE,
            binder_voicecall_ringback_tone_event, self);

#pragma message("TODO: Set up ECC list watcher")
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

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "");

    self->dtmf_queue = gutil_ring_new();
    self->g = radio_request_group_new(modem->client); /* Keeps ref to client */
    self->local_hangup_reasons = gutil_ints_ref(cfg->local_hangup_reasons);
    self->remote_hangup_reasons = gutil_ints_ref(cfg->remote_hangup_reasons);
    self->local_release_ids = gutil_int_array_new();
    self->idleq = gutil_idle_queue_new();
    self->vc = vc;

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

    DBG("");
    g_slist_free_full(self->calls, binder_voicecall_ofono_call_free);

    radio_request_drop(self->send_dtmf_req);
    radio_request_drop(self->clcc_poll_req);
    radio_client_remove_all_handlers(self->g->client, self->event_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    gutil_ring_unref(self->dtmf_queue);
    gutil_ints_unref(self->local_hangup_reasons);
    gutil_ints_unref(self->remote_hangup_reasons);
    gutil_int_array_free(self->local_release_ids, TRUE);
    gutil_idle_queue_free(self->idleq);

    g_free(self->log_prefix);
    g_free(self);

    ofono_voicecall_set_data(vc, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_voicecall_driver binder_voicecall_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_voicecall_probe,
    .remove                 = binder_voicecall_remove,
    .dial                   = binder_voicecall_dial,
    .answer                 = binder_voicecall_answer,
    .hangup_active          = binder_voicecall_hangup_active,
    .hangup_all             = binder_voicecall_hangup_all,
    .release_specific       = binder_voicecall_release_specific,
    .send_tones             = binder_voicecall_send_dtmf,
    .create_multiparty      = binder_voicecall_create_multiparty,
    .transfer               = binder_voicecall_transfer,
    .private_chat           = binder_voicecall_private_chat,
    .swap_without_accept    = binder_voicecall_swap_without_accept,
    .release_all_held       = binder_voicecall_release_all_held,
    .set_udub               = binder_voicecall_set_udub,
    .release_all_active     = binder_voicecall_release_all_active
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
