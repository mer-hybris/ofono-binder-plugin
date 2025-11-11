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

#include "binder_call_forwarding.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/call-forwarding.h>
#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_voice_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

typedef struct binder_call_forwarding {
    struct ofono_call_forwarding* f;
    RadioRequestGroup* g;
    RADIO_AIDL_INTERFACE interface_aidl;
    char* log_prefix;
    guint register_id;
} BinderCallForwarding;

typedef struct binder_call_forwarding_cbd {
    BinderCallForwarding* self;
    union call_forwarding_cb {
        ofono_call_forwarding_query_cb_t query;
        ofono_call_forwarding_set_cb_t set;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderCallForwardingCbData;

#define CF_TIME_DEFAULT (0)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderCallForwarding*
binder_call_forwarding_get_data(struct ofono_call_forwarding* f)
    { return ofono_call_forwarding_get_data(f); }

static
BinderCallForwardingCbData*
binder_call_forwarding_callback_data_new(
    BinderCallForwarding* self,
    BinderCallback cb,
    void* data)
{
    BinderCallForwardingCbData* cbd = g_slice_new0(BinderCallForwardingCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_call_forwarding_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderCallForwardingCbData, cbd);
}

static
void
binder_call_forwarding_call(
    BinderCallForwarding* self,
    RADIO_REQ code,
    RADIO_CALL_FORWARD action,
    int reason,
    int cls,
    const struct ofono_phone_number* number,
    int time,
    RadioRequestCompleteFunc complete,
    BinderCallback cb,
    void* data)
{
    /*
     * getCallForwardStatus(int32_t serial, CallForwardInfo callInfo);
     * setCallForward(int32_t serial, CallForwardInfo callInfo);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g, code, &writer, complete,
        binder_call_forwarding_callback_data_free,
        binder_call_forwarding_callback_data_new(self, cb, data));

    if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        RadioCallForwardInfo* info = gbinder_writer_new0(&writer,
            RadioCallForwardInfo);
        guint parent;

        info->status = action;
        info->reason = reason;
        info->serviceClass = cls;
        info->timeSeconds = time;
        if (number) {
            info->toa = number->type;
            binder_copy_hidl_string(&writer, &info->number, number->number);
        } else {
            info->toa = OFONO_NUMBER_TYPE_UNKNOWN;
            binder_copy_hidl_string(&writer, &info->number, NULL);
        }
        parent = gbinder_writer_append_buffer_object(&writer, info, sizeof(*info));
        binder_append_hidl_string_data(&writer, info, number, parent);
    } else {
        gint32 initial_size;
        /* Non-null parcelable */
        gbinder_writer_append_int32(&writer, 1);
        initial_size = gbinder_writer_bytes_written(&writer);
        /* Dummy parcelable size, replaced at the end */
        gbinder_writer_append_int32(&writer, -1);

        gbinder_writer_append_int32(&writer, action);
        gbinder_writer_append_int32(&writer, reason);
        gbinder_writer_append_int32(&writer, cls);
        if (number) {
            gbinder_writer_append_int32(&writer, number->type);
            gbinder_writer_append_string16(&writer, number->number);
        } else {
            gbinder_writer_append_int32(&writer, OFONO_NUMBER_TYPE_UNKNOWN);
            gbinder_writer_append_string16(&writer, "");
        }
        gbinder_writer_append_int32(&writer, time);

        /* Overwrite parcelable size */
        gbinder_writer_overwrite_int32(&writer, initial_size,
            gbinder_writer_bytes_written(&writer) - initial_size);
    }

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_call_forwarding_set_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallForwardingCbData* cbd = user_data;
    ofono_call_forwarding_set_cb_t cb = cbd->cb.set;

    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = cbd->self->interface_aidl == RADIO_VOICE_INTERFACE ?
            RADIO_VOICE_RESP_SET_CALL_FORWARD :
            RADIO_RESP_SET_CALL_FORWARD;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_error("CF error %d", error);
            }
        } else {
            ofono_error("Unexpected setCallForward response %d", resp);
        }
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_call_forwarding_set(
    BinderCallForwarding* self,
    RADIO_CALL_FORWARD action,
    int reason,
    int cls,
    const struct ofono_phone_number* number,
    int time,
    ofono_call_forwarding_set_cb_t cb,
    void* data)
{
    guint32 code = self->interface_aidl == RADIO_VOICE_INTERFACE ?
        RADIO_VOICE_REQ_SET_CALL_FORWARD :
        RADIO_REQ_SET_CALL_FORWARD;
    binder_call_forwarding_call(self, code,
        action, reason, cls, number, time, binder_call_forwarding_set_cb,
        BINDER_CB(cb), data);
}

static
void
binder_call_forwarding_registration(
    struct ofono_call_forwarding* f,
    int type,
    int cls,
    const struct ofono_phone_number* number,
    int time,
    ofono_call_forwarding_set_cb_t cb,
    void* data)
{
    BinderCallForwarding* self = binder_call_forwarding_get_data(f);

    DBG_(self, "%d", type);
    binder_call_forwarding_set(self, RADIO_CALL_FORWARD_REGISTRATION,
        type, cls, number, time, cb, data);
}

static
void
binder_call_forwarding_erasure(
    struct ofono_call_forwarding* f,
    int type,
    int cls,
    ofono_call_forwarding_set_cb_t cb,
    void* data)
{
    BinderCallForwarding* self = binder_call_forwarding_get_data(f);

    DBG_(self, "%d", type);
    binder_call_forwarding_set(self, RADIO_CALL_FORWARD_ERASURE,
        type, cls, NULL, CF_TIME_DEFAULT, cb, data);
}

static
void
binder_call_forwarding_deactivate(
    struct ofono_call_forwarding* f,
    int type,
    int cls,
    ofono_call_forwarding_set_cb_t cb,
    void* data)
{
    BinderCallForwarding* self = binder_call_forwarding_get_data(f);

    DBG_(self, "%d", type);
    binder_call_forwarding_set(self, RADIO_CALL_FORWARD_DISABLE,
        type, cls, NULL, CF_TIME_DEFAULT, cb, data);
}

static
void
binder_call_forwarding_activate(
    struct ofono_call_forwarding* f,
    int type,
    int cls,
    ofono_call_forwarding_set_cb_t cb,
    void* data)
{
    BinderCallForwarding* self = binder_call_forwarding_get_data(f);

    DBG_(self, "%d", type);
    binder_call_forwarding_set(self, RADIO_CALL_FORWARD_ENABLE,
        type, cls, NULL, CF_TIME_DEFAULT, cb, data);
}

static
void
binder_call_forwarding_query_ok(
    const BinderCallForwardingCbData* cbd,
    const GBinderReader* args)
{
    struct ofono_error err;
    struct ofono_call_forwarding_condition* list = NULL;
    GBinderReader reader;
    gsize count = 0;

    gbinder_reader_copy(&reader, args);

    if (cbd->self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        /* getCallForwardStatusResponse(RadioResponseInfo, vec<CallForwardInfo>) */
        const RadioCallForwardInfo* infos = gbinder_reader_read_hidl_type_vec(
            &reader, RadioCallForwardInfo, &count);
        if (count) {
            gsize i;

            list = g_new0(struct ofono_call_forwarding_condition, count);
            for (i = 0; i < count; i++) {
                const RadioCallForwardInfo* info = infos + i;
                struct ofono_call_forwarding_condition* fw = list + i;

                fw->status = info->status;
                fw->cls = info->serviceClass;
                fw->time = info->timeSeconds;
                fw->phone_number.type = info->toa;
                memcpy(fw->phone_number.number, info->number.data.str,
                    MIN(OFONO_MAX_PHONE_NUMBER_LENGTH, info->number.len));
            }
        }
    } else {
        /* getCallForwardStatusResponse(RadioResponseInfo, CallForwardInfo[] callForwardInfos) */
        gbinder_reader_read_uint32(&reader, (guint32 *)&count);
        if (count) {
            gsize i;

            list = g_new0(struct ofono_call_forwarding_condition, count);
            for (i = 0; i < count; i++) {
                struct ofono_call_forwarding_condition* fw = list + i;

                // Non-null parcelable
                gbinder_reader_read_int32(&reader, NULL);
                // Parcelable size
                gbinder_reader_read_int32(&reader, NULL);
                gbinder_reader_read_int32(&reader, &fw->status);
                gbinder_reader_read_int32(&reader, NULL);
                gbinder_reader_read_int32(&reader, &fw->cls);
                gbinder_reader_read_int32(&reader, &fw->phone_number.type);
                gchar* number = gbinder_reader_read_string16(&reader);
                if (number) {
                    memcpy(fw->phone_number.number, number,
                        MIN(OFONO_MAX_PHONE_NUMBER_LENGTH, strlen(number)));
                    g_free(number);
                }
                gbinder_reader_read_int32(&reader, &fw->time);
            }
        }
    }
    cbd->cb.query(binder_error_ok(&err), count, list, cbd->data);
    g_free(list);
}

static
void
binder_call_forwarding_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallForwardingCbData* cbd = user_data;

    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = cbd->self->interface_aidl == RADIO_VOICE_INTERFACE ?
            RADIO_VOICE_RESP_GET_CALL_FORWARD_STATUS :
            RADIO_RESP_GET_CALL_FORWARD_STATUS;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                binder_call_forwarding_query_ok(cbd, args);
                return;
            } else {
                ofono_error("CF query error %d", error);
            }
        } else {
            ofono_error("Unexpected getCallForwardStatus response %d", resp);
        }
    }
    cbd->cb.query(binder_error_failure(&err), 0, NULL, cbd->data);
}

static
void
binder_call_forwarding_query(
    struct ofono_call_forwarding* f,
    int type,
    int cls,
    ofono_call_forwarding_query_cb_t cb,
    void* data)
{
    BinderCallForwarding* self = binder_call_forwarding_get_data(f);
    guint32 code = self->interface_aidl == RADIO_VOICE_INTERFACE ?
        RADIO_VOICE_REQ_GET_CALL_FORWARD_STATUS :
        RADIO_REQ_GET_CALL_FORWARD_STATUS;

    DBG_(self, "%d", type);

    /* Modem doesn't seem to like class mask 7, replace it with 0 */
    if (cls == (RADIO_SERVICE_CLASS_VOICE |
        RADIO_SERVICE_CLASS_DATA | RADIO_SERVICE_CLASS_FAX)) {
        DBG_(self, "cls %d => %d", cls, RADIO_SERVICE_CLASS_NONE);
        cls = RADIO_SERVICE_CLASS_NONE;
    }
    binder_call_forwarding_call(self, code,
        RADIO_CALL_FORWARD_INTERROGATE, type, cls, NULL, CF_TIME_DEFAULT,
        binder_call_forwarding_query_cb, BINDER_CB(cb), data);
}

static
gboolean
binder_call_forwarding_register(
    gpointer user_data)
{
    BinderCallForwarding* self = user_data;

    GASSERT(self->register_id);
    self->register_id = 0;
    ofono_call_forwarding_register(self->f);
    return G_SOURCE_REMOVE;
}

static
int
binder_call_forwarding_probe(
    struct ofono_call_forwarding* f,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderCallForwarding* self = g_new0(BinderCallForwarding, 1);

    self->f = f;
    self->g = radio_request_group_new(modem->voice_client);
    self->interface_aidl = radio_client_aidl_interface(modem->voice_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_call_forwarding_register, self);

    DBG_(self, "");
    ofono_call_forwarding_set_data(f, self);
    return 0;
}

static
void
binder_call_forwarding_remove(
    struct ofono_call_forwarding* f)
{
    BinderCallForwarding* self = binder_call_forwarding_get_data(f);

    DBG_(self, "");
    if (self->register_id) {
        g_source_remove(self->register_id);
    }
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_call_forwarding_set_data(f, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_call_forwarding_driver
binder_call_forwarding_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_call_forwarding_probe,
    .remove         = binder_call_forwarding_remove,
    .erasure        = binder_call_forwarding_erasure,
    .deactivation   = binder_call_forwarding_deactivate,
    .query          = binder_call_forwarding_query,
    .registration   = binder_call_forwarding_registration,
    .activation     = binder_call_forwarding_activate
};

void
binder_call_forwarding_init()
{
    ofono_call_forwarding_driver_register(&binder_call_forwarding_driver);
}

void
binder_call_forwarding_cleanup()
{
    ofono_call_forwarding_driver_unregister(&binder_call_forwarding_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
