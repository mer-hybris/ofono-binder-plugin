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

#include "binder_call_settings.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/call-settings.h>
#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_voice_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_misc.h>

typedef struct binder_call_settings_api BinderCallSettingsApi;

typedef struct binder_call_settings {
    struct ofono_call_settings* s;
    const BinderCallSettingsApi* api;
    RadioRequestGroup* g;
    char* log_prefix;
    guint register_id;
} BinderCallSettings;

typedef struct binder_call_settings_cbd {
    BinderCallSettings* self;
    union call_settings_cb {
        ofono_call_settings_status_cb_t status;
        ofono_call_settings_set_cb_t set;
        ofono_call_settings_clir_cb_t clir;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderCallSettingsCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

/* Binder API flavors */
struct binder_call_settings_api {
    const char* name;
    RADIO_REQ get_call_waiting_req;
    RADIO_REQ set_call_waiting_req;
    RADIO_REQ get_clip_req;
    RADIO_REQ get_clir_req;
    RADIO_REQ set_clir_req;
};

static const BinderCallSettingsApi binder_call_settings_api_hidl;
static const BinderCallSettingsApi binder_call_settings_api_aidl;

static inline BinderCallSettings*
binder_call_settings_get_data(struct ofono_call_settings* s)
    { return ofono_call_settings_get_data(s); }

static
BinderCallSettingsCbData*
binder_call_settings_callback_data_new(
    BinderCallSettings* self,
    BinderCallback cb,
    void* data)
{
    BinderCallSettingsCbData* cbd = g_slice_new0(BinderCallSettingsCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_call_settings_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderCallSettingsCbData, cbd);
}

static
void
binder_call_settings_call(
    BinderCallSettings* self,
    RADIO_REQ code,
    RadioRequestCompleteFunc complete,
    BinderCallback cb,
    void* data)
{
    binder_submit_request2(self->g, code, complete,
        binder_call_settings_callback_data_free,
        binder_call_settings_callback_data_new(self, cb, data));
}

static
void
binder_call_settings_set_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallSettingsCbData* cbd = user_data;
    ofono_call_settings_set_cb_t cb = cbd->cb.set;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setCallWaiting tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("CW seting error %s",
            binder_radio_error_string(error));
    } else {
        cb(binder_error_ok(&err), cbd->data);
        return;
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_call_settings_cw_set(
    struct ofono_call_settings* s,
    int mode,
    int cls,
    ofono_call_settings_set_cb_t cb,
    void* data)
{
    BinderCallSettings* self = binder_call_settings_get_data(s);
    GBinderWriter args;
    RadioRequest* req;

    /*
     * Modem seems to respond with error to all queries
     * or settings made with bearer class
     * BEARER_CLASS_DEFAULT. Design decision: If given
     * class is BEARER_CLASS_DEFAULT let's map it to
     * SERVICE_CLASS_VOICE effectively making it the
     * default bearer.
     */
    if (cls == BEARER_CLASS_DEFAULT)
        cls = BEARER_CLASS_VOICE;

    req = radio_request_new2(self->g, self->api->set_call_waiting_req, &args,
        binder_call_settings_set_cb,
        binder_call_settings_callback_data_free,
        binder_call_settings_callback_data_new(self, BINDER_CB(cb), data));

    /*
     * IRadio.hal:
     * oneway setCallWaiting(int32_t serial,
     *     bool enable, int32_t serviceClass);
     *
     * IRadioVoice.aidl:
     * void setCallWaiting(in int serial,
     *     in boolean enable, in int serviceClass);
     */
    gbinder_writer_append_bool(&args, mode);  /* enable */
    gbinder_writer_append_int32(&args, cls);  /* serviceClass */

    radio_request_submit(req);
    radio_request_unref(req);
}

static
gboolean
binder_call_settings_cw_query_ok(
    const BinderCallSettingsCbData* cbd,
    const GBinderReader* args)
{
    struct ofono_error err;
    ofono_call_settings_status_cb_t cb = cbd->cb.status;
    GBinderReader reader;
    gboolean enable;
    gint32 cls;

    /*
     * getCallWaitingResponse(RadioResponseInfo, bool enable,
     *     int32_t serviceClass);
     */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_bool(&reader, &enable) &&
        gbinder_reader_read_int32(&reader, &cls)) {
        if (enable) {
            DBG_(cbd->self, "CW enabled for %d", cls);
            cb(binder_error_ok(&err), cls, cbd->data);
        } else {
            DBG_(cbd->self, "CW disabled");
            cb(binder_error_ok(&err), 0, cbd->data);
        }
        return TRUE;
    }
    ofono_warn("Unexpected getCallWaitingResponse payload");
    return FALSE;
}

static
void
binder_call_settings_cw_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallSettingsCbData* cbd = user_data;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "getCallWaiting tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("CW query error %s",
            binder_radio_error_string(error));
    } else if (binder_call_settings_cw_query_ok(cbd, args)) {
        return;
    }
    cbd->cb.status(binder_error_failure(&err), -1, cbd->data);
}

static
void binder_call_settings_cw_query(
    struct ofono_call_settings* s,
    int cls,
    ofono_call_settings_status_cb_t cb,
    void* data)
{
    BinderCallSettings* self = binder_call_settings_get_data(s);
    GBinderWriter args;
    RadioRequest* req;

    /*
     * Modem seems to respond with error to all queries
     * or settings made with bearer class
     * BEARER_CLASS_DEFAULT. Design decision: If given
     * class is BEARER_CLASS_DEFAULT let's map it to
     * SERVICE_CLASS_VOICE effectively making it the
     * default bearer.
     */
    if (cls == BEARER_CLASS_DEFAULT)
        cls = BEARER_CLASS_VOICE;

    req = radio_request_new2(self->g, self->api->get_call_waiting_req, &args,
        binder_call_settings_cw_query_cb,
        binder_call_settings_callback_data_free,
        binder_call_settings_callback_data_new(self, BINDER_CB(cb), data));

    /*
     * IRadio.hal:
     * oneway getCallWaiting(int32_t serial, int32_t serviceClass);
     *
     * IRadioVoice.aidl:
     * void getCallWaiting(in int serial, in int serviceClass);
     */
    gbinder_writer_append_int32(&args, cls); /* serviceClass */

    radio_request_submit(req);
    radio_request_unref(req);
}

static
gboolean
binder_call_settings_clip_query_ok(
    const BinderCallSettingsCbData* cbd,
    const GBinderReader* args)
{
    struct ofono_error err;
    GBinderReader reader;
    gint32 status;

    /* getClipResponse(RadioResponseInfo, ClipStatus status); */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &status)) {
        cbd->cb.status(binder_error_ok(&err), status, cbd->data);
        return TRUE;
    }
    return FALSE;
}

static
void
binder_call_settings_clip_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallSettingsCbData* cbd = user_data;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "getClip tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("CLIP query error %s",
            binder_radio_error_string(error));
    } else if (binder_call_settings_clip_query_ok(cbd, args)) {
        return;
    }
    cbd->cb.status(binder_error_failure(&err), -1, cbd->data);
}

static
void
binder_call_settings_clip_query(
    struct ofono_call_settings* s,
    ofono_call_settings_status_cb_t cb,
    void* data)
{
    BinderCallSettings* self = binder_call_settings_get_data(s);

    DBG_(self, "");
    /* getClip(int32_t serial); */
    binder_call_settings_call(self, self->api->get_clip_req,
        binder_call_settings_clip_query_cb, BINDER_CB(cb), data);
}

static
gboolean
binder_call_settings_clir_ok(
    const BinderCallSettingsCbData* cbd,
    const GBinderReader* args)
{
    struct ofono_error err;
    GBinderReader reader;
    gint32 n, m;

    /* getClirResponse(RadioResponseInfo info, int32_t n, int32_t m); */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &n) &&
        gbinder_reader_read_int32(&reader, &m)) {
        cbd->cb.clir(binder_error_ok(&err), n, m, cbd->data);
        return TRUE;
    }
    ofono_warn("Unexpected getClirResponse payload");
    return FALSE;
}

static
void
binder_call_settings_clir_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallSettingsCbData* cbd = user_data;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "getClir tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("CW query error %s",
            binder_radio_error_string(error));
    } else if (binder_call_settings_clir_ok(cbd, args)) {
        return;
    }
    cbd->cb.clir(binder_error_failure(&err), -1, -1, cbd->data);
}

static
void
binder_call_settings_clir_query(
    struct ofono_call_settings* s,
    ofono_call_settings_clir_cb_t cb,
    void* data)
{
    BinderCallSettings* self = binder_call_settings_get_data(s);

    DBG_(self, "");
    /* getClir(int32_t serial); */
    binder_call_settings_call(self, self->api->get_clir_req,
       binder_call_settings_clir_cb , BINDER_CB(cb), data);
}

static
void
binder_call_settings_clir_set(
    struct ofono_call_settings* s,
    int mode,
    ofono_call_settings_set_cb_t cb,
    void* data)
{
    BinderCallSettings* self = binder_call_settings_get_data(s);
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        self->api->set_clir_req, &args,
        binder_call_settings_set_cb,
        binder_call_settings_callback_data_free,
        binder_call_settings_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "%d", mode);

    /*
     * IRadio.hal:
     * oneway setClir(int32_t serial, int32_t status);
     *
     * IRadioVoice.aidl:
     * void setClir(in int serial, in int status);
     */
    gbinder_writer_append_int32(&args, mode); /* status */

    radio_request_submit(req);
    radio_request_unref(req);
}

static
gboolean
binder_call_settings_register(
    gpointer user_data)
{
    BinderCallSettings* self = user_data;

    DBG_(self, "");
    GASSERT(self->register_id);
    self->register_id = 0;
    ofono_call_settings_register(self->s);
    return G_SOURCE_REMOVE;
}

static
int
binder_call_settings_probe(
    struct ofono_call_settings* s,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderCallSettings* self = g_new0(BinderCallSettings, 1);
    RadioClient* voice_client = modem->clients.voice_client;
    const BinderCallSettingsApi* api =
        radio_client_aidl_interface(voice_client) == RADIO_VOICE_INTERFACE ?
        &binder_call_settings_api_aidl : &binder_call_settings_api_hidl;

    self->s = s;
    self->api = api;
    self->g = radio_request_group_new(voice_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_call_settings_register, self);

    DBG_(self, "");
    ofono_call_settings_set_data(s, self);
    return 0;
}

static
void
binder_call_settings_remove(
    struct ofono_call_settings* s)
{
    BinderCallSettings* self = binder_call_settings_get_data(s);

    DBG_(self, "");
    gutil_source_remove(self->register_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_call_settings_set_data(s, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static const BinderCallSettingsApi binder_call_settings_api_hidl = {
    "hidl",
    RADIO_REQ_GET_CALL_WAITING,
    RADIO_REQ_SET_CALL_WAITING,
    RADIO_REQ_GET_CLIP,
    RADIO_REQ_GET_CLIR,
    RADIO_REQ_SET_CLIR
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static const BinderCallSettingsApi binder_call_settings_api_aidl = {
    "aidl",
    RADIO_VOICE_REQ_GET_CALL_WAITING,
    RADIO_VOICE_REQ_SET_CALL_WAITING,
    RADIO_VOICE_REQ_GET_CLIP,
    RADIO_VOICE_REQ_GET_CLIR,
    RADIO_VOICE_REQ_SET_CLIR
};

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_call_settings_driver binder_call_settings_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_call_settings_probe,
    .remove         = binder_call_settings_remove,
    .clip_query     = binder_call_settings_clip_query,
    .clir_query     = binder_call_settings_clir_query,
    .clir_set       = binder_call_settings_clir_set,
    .cw_query       = binder_call_settings_cw_query,
    .cw_set         = binder_call_settings_cw_set
};

void
binder_call_settings_init()
{
    ofono_call_settings_driver_register(&binder_call_settings_driver);
}

void
binder_call_settings_cleanup()
{
    ofono_call_settings_driver_unregister(&binder_call_settings_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
