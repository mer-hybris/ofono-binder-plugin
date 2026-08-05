/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
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

#include <gutil_misc.h>

typedef struct binder_call_forwarding_api BinderCallForwardingApi;

typedef struct binder_call_forwarding {
    struct ofono_call_forwarding* f;
    const BinderCallForwardingApi* api;
    RadioRequestGroup* g;
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

/* Binder API flavors */
struct binder_call_forwarding_api {
    const char* name;
    RADIO_REQ get_call_forward_status_req;
    RADIO_REQ set_call_forward_req;
    void (*write_call_forward_info_arg)(
        GBinderWriter* writer,
        RADIO_CALL_FORWARD action,
        int reason,
        int cls,
        const struct ofono_phone_number* number,
        int time);
    struct ofono_call_forwarding_condition* (*read_call_forwarding_conditions)(
        GBinderReader* reader,
        guint* count);
};

static const BinderCallForwardingApi binder_call_forwarding_api_hidl;
static const BinderCallForwardingApi binder_call_forwarding_api_aidl;

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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setCallForward tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("CF error %s", binder_radio_error_string(error));
    } else {
        cb(binder_error_ok(&err), cbd->data);
        return;
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
    const BinderCallForwardingApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        self->api->set_call_forward_req, &args,
        binder_call_forwarding_set_cb,
        binder_call_forwarding_callback_data_free,
        binder_call_forwarding_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "");

    /* Modems doen't seem to like class mask 7, replace it with 0 */
    if (cls == (RADIO_SERVICE_CLASS_VOICE |
        RADIO_SERVICE_CLASS_DATA | RADIO_SERVICE_CLASS_FAX)) {
        DBG_(self, "cls %d => %d", cls, RADIO_SERVICE_CLASS_NONE);
        cls = RADIO_SERVICE_CLASS_NONE;
    }

    /*
     * IRadio.hal:
     * oneway setCallForward(int32_t serial, CallForwardInfo callInfo);
     *
     * IRadioVoice.aidl:
     * void setCallForward(in int serial, in CallForwardInfo callInfo);
     */
    api->write_call_forward_info_arg(&args, action, reason, cls, number, time);

    radio_request_submit(req);
    radio_request_unref(req);
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "getCallForwardStatus tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("CF query error %s", binder_radio_error_string(error));
    } else {
        guint n = 0;
        struct ofono_call_forwarding_condition* list = NULL;
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        list = cbd->self->api->read_call_forwarding_conditions(&reader, &n);
        cbd->cb.query(binder_error_ok(&err), n, list, cbd->data);
        g_free(list);
        return;
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
    const BinderCallForwardingApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        api->get_call_forward_status_req, &args,
        binder_call_forwarding_query_cb,
        binder_call_forwarding_callback_data_free,
        binder_call_forwarding_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "%d", type);

    /* Modems doen't seem to like class mask 7, replace it with 0 */
    if (cls == (RADIO_SERVICE_CLASS_VOICE |
        RADIO_SERVICE_CLASS_DATA | RADIO_SERVICE_CLASS_FAX)) {
        DBG_(self, "cls %d => %d", cls, RADIO_SERVICE_CLASS_NONE);
        cls = RADIO_SERVICE_CLASS_NONE;
    }

    /*
     * IRadio.hal:
     * oneway getCallForwardStatus(int32_t serial, CallForwardInfo callInfo);
     *
     * IRadioVoice.aidl:
     * void getCallForwardStatus(in int serial, in CallForwardInfo callInfo);
     */
    api->write_call_forward_info_arg(&args, RADIO_CALL_FORWARD_INTERROGATE,
        type, cls, NULL, CF_TIME_DEFAULT);

    radio_request_submit(req);
    radio_request_unref(req);
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
    RadioClient* voice_client = modem->clients.voice_client;
    const BinderCallForwardingApi* api =
        radio_client_aidl_interface(voice_client) == RADIO_VOICE_INTERFACE ?
        &binder_call_forwarding_api_aidl : &binder_call_forwarding_api_hidl;

    self->f = f;
    self->api = api;
    self->g = radio_request_group_new(voice_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_call_forwarding_register, self);

    DBG_(self, "%s api", api->name);
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
    gutil_source_remove(self->register_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_call_forwarding_set_data(f, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static
void
binder_call_forwarding_api_write_call_forward_info_arg_hidl(
    GBinderWriter* writer,
    RADIO_CALL_FORWARD action,
    int reason,
    int cls,
    const struct ofono_phone_number* number,
    int time)
{
    guint parent;
    RadioCallForwardInfo* info = gbinder_writer_new0(writer,
        RadioCallForwardInfo);

    info->status = action;
    info->reason = reason;
    info->serviceClass = cls;
    info->timeSeconds = time;
    if (number) {
        info->toa = number->type;
        binder_copy_hidl_string(writer, &info->number, number->number);
    } else {
        info->toa = OFONO_NUMBER_TYPE_UNKNOWN;
        binder_copy_hidl_string(writer, &info->number, NULL);
    }
    parent = gbinder_writer_append_buffer_object(writer, info, sizeof(*info));
    binder_append_hidl_string_data(writer, info, number, parent);
}

static
struct ofono_call_forwarding_condition*
binder_call_forwarding_api_read_call_forwarding_conditions_hidl(
    GBinderReader* reader,
    guint* count)
{
    /*  vec<CallForwardInfo>) */
    gsize n;
    const RadioCallForwardInfo* infos =
        gbinder_reader_read_hidl_type_vec( reader, RadioCallForwardInfo, &n);

    if (infos) {
        gsize i;
        struct ofono_call_forwarding_condition* list =
            g_new0(struct ofono_call_forwarding_condition, n);

        for (i = 0; i < n; i++) {
            const RadioCallForwardInfo* info = infos + i;
            struct ofono_call_forwarding_condition* fw = list + i;

            fw->status = info->status;
            fw->cls = info->serviceClass;
            fw->time = info->timeSeconds;
            fw->phone_number.type = info->toa;
            memcpy(fw->phone_number.number, info->number.data.str,
                MIN(OFONO_MAX_PHONE_NUMBER_LENGTH, info->number.len));
        }
        *count = n;
        return list;
    }
    return NULL;
}

static const BinderCallForwardingApi binder_call_forwarding_api_hidl = {
    "hidl",
    RADIO_REQ_GET_CALL_FORWARD_STATUS,
    RADIO_REQ_SET_CALL_FORWARD,
    binder_call_forwarding_api_write_call_forward_info_arg_hidl,
    binder_call_forwarding_api_read_call_forwarding_conditions_hidl
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static
void
binder_call_forwarding_api_write_call_forward_info_arg_aidl(
    GBinderWriter* writer,
    RADIO_CALL_FORWARD action,
    int reason,
    int serviceClass,
    const struct ofono_phone_number* number,
    int timeSeconds)
{
    GBinderWriter parcel;

    /*
     * package android.hardware.radio.voice;
     * parcelable CallForwardInfo {
     *   int status;
     *   int reason;
     *   int serviceClass;
     *   int toa;
     *   String number;
     *   int timeSeconds;
     * }
     */
    gbinder_writer_start_parcelable(writer, &parcel);
    gbinder_writer_append_int32(&parcel, action);
    gbinder_writer_append_int32(&parcel, reason);
    gbinder_writer_append_int32(&parcel, serviceClass);
    if (number) {
        gbinder_writer_append_int32(&parcel, number->type);
        gbinder_writer_append_string16(&parcel, number->number);
    } else {
        gbinder_writer_append_int32(&parcel, OFONO_NUMBER_TYPE_UNKNOWN);
        gbinder_writer_append_string16(&parcel, "");
    }
    gbinder_writer_append_int32(&parcel, timeSeconds);
    gbinder_writer_finish_parcelable(&parcel);
}

static
struct ofono_call_forwarding_condition*
binder_call_forwarding_api_read_call_forwarding_conditions_aidl(
    GBinderReader* reader,
    guint* count)
{
    /* CallForwardInfo[] */
    if (gbinder_reader_read_uint32(reader, count)) {
        guint i;
        struct ofono_call_forwarding_condition* list =
            g_new0(struct ofono_call_forwarding_condition, *count);

        for (i = 0; i < *count; i++) {
            struct ofono_call_forwarding_condition* fw = list + i;
            GBinderReader parcel;

            if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
                gchar* number = NULL;

                if (gbinder_reader_read_int32(&parcel, &fw->status) &&
                    gbinder_reader_read_int32(&parcel, NULL) &&
                    gbinder_reader_read_int32(&parcel, &fw->cls) &&
                    gbinder_reader_read_int32(&parcel, &fw->phone_number.type)&&
                    gbinder_reader_read_nullable_string16(&parcel, &number) &&
                    gbinder_reader_read_int32(&parcel, &fw->time)) {
                    if (number) {
                        g_strlcpy(fw->phone_number.number, number,
                            sizeof(fw->phone_number.number));
                        g_free(number);
                    }
                    gbinder_reader_finish_parcelable(&parcel);
                    continue;
                }
                g_free(number);
            }
            g_free(list);
            return NULL;
        }
        return list;
    }
    return NULL;
}

static const BinderCallForwardingApi binder_call_forwarding_api_aidl = {
    "aidl",
    RADIO_VOICE_REQ_GET_CALL_FORWARD_STATUS,
    RADIO_VOICE_REQ_SET_CALL_FORWARD,
    binder_call_forwarding_api_write_call_forward_info_arg_aidl,
    binder_call_forwarding_api_read_call_forwarding_conditions_aidl
};

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
