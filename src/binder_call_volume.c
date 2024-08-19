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

#include "binder_call_volume.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/call-volume.h>
#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_voice_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

typedef struct binder_call_volume {
    struct ofono_call_volume* v;
    RadioRequestGroup* g;
    RADIO_AIDL_INTERFACE interface_aidl;
    char* log_prefix;
    guint register_id;
} BinderCallVolume;

typedef struct binder_call_volume_req {
    BinderCallVolume* self;
    ofono_call_volume_cb_t cb;
    gpointer data;
} BinderCallVolumeCbData;

#define DBG_(cd,fmt,args...) DBG("%s" fmt, (cd)->log_prefix, ##args)

static inline BinderCallVolume*
binder_call_volume_get_data(struct ofono_call_volume* v)
    { return ofono_call_volume_get_data(v); }

static
BinderCallVolumeCbData*
binder_call_volume_callback_data_new(
    BinderCallVolume* self,
    ofono_call_volume_cb_t cb,
    void* data)
{
    BinderCallVolumeCbData* cbd = g_slice_new0(BinderCallVolumeCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_call_volume_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderCallVolumeCbData, cbd);
}

static
void
binder_call_volume_mute_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallVolumeCbData* cbd = user_data;
    ofono_call_volume_cb_t cb = cbd->cb;

    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = cbd->self->interface_aidl == RADIO_VOICE_INTERFACE ?
            RADIO_VOICE_RESP_SET_MUTE : RADIO_RESP_SET_MUTE;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_warn("Could not set the mute state, error %d", error);
            }
        } else {
            ofono_error("Unexpected setMute response %d", resp);
        }
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_call_volume_mute(
    struct ofono_call_volume* v,
    int muted,
    ofono_call_volume_cb_t cb,
    void* data)
{
    BinderCallVolume* self = binder_call_volume_get_data(v);
    guint32 code = self->interface_aidl == RADIO_VOICE_INTERFACE ?
        RADIO_VOICE_REQ_SET_MUTE : RADIO_REQ_SET_MUTE;

    /* setMute(int32_t serial, bool enable); */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        code, &writer,
        binder_call_volume_mute_cb,
        binder_call_volume_callback_data_free,
        binder_call_volume_callback_data_new(self, cb, data));

    DBG_(self, "%d", muted);
    gbinder_writer_append_bool(&writer, muted);  /* enabled */

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_call_volume_query_mute_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    const BinderCallVolume* self = user_data;
    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = self->interface_aidl == RADIO_VOICE_INTERFACE ?
            RADIO_VOICE_RESP_GET_MUTE : RADIO_RESP_GET_MUTE;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                gboolean muted;

                /* getMuteResponse(RadioResponseInfo info, bool enable); */
                gbinder_reader_copy(&reader, args);
                if (gbinder_reader_read_bool(&reader, &muted)) {
                    BinderCallVolume* self = user_data;

                    DBG_(self, "%d", muted);
                    ofono_call_volume_set_muted(self->v, muted);
                }
            } else {
                ofono_warn("Could not get the mute state, error %d", error);
            }
        } else {
            ofono_error("Unexpected getMute response %d", resp);
        }
    }
}

static
gboolean
binder_call_volume_register(
    gpointer user_data)
{
    BinderCallVolume* self = user_data;
    guint32 code = self->interface_aidl == RADIO_VOICE_INTERFACE ?
        RADIO_VOICE_REQ_GET_MUTE : RADIO_REQ_GET_MUTE;

    DBG_(self, "");
    GASSERT(self->register_id);
    self->register_id = 0;
    ofono_call_volume_register(self->v);

    /* Probe the mute state */
    binder_submit_request2(self->g, code,
        binder_call_volume_query_mute_cb, NULL, self);

    return G_SOURCE_REMOVE;
}

static
int
binder_call_volume_probe(
    struct ofono_call_volume* v,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderCallVolume* self = g_new0(BinderCallVolume, 1);

    self->v = v;
    self->g = radio_request_group_new(modem->voice_client);
    self->interface_aidl = radio_client_aidl_interface(modem->voice_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_call_volume_register, self);

    DBG_(self, "");
    ofono_call_volume_set_data(v, self);
    return 0;
}

static
void
binder_call_volume_remove(
    struct ofono_call_volume* v)
{
    BinderCallVolume* self = binder_call_volume_get_data(v);

    DBG_(self, "");
    if (self->register_id) {
        g_source_remove(self->register_id);
    }
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_call_volume_set_data(v, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_call_volume_driver binder_call_volume_driver = {
    .name       = BINDER_DRIVER,
    .probe      = binder_call_volume_probe,
    .remove     = binder_call_volume_remove,
    .mute       = binder_call_volume_mute
};

void
binder_call_volume_init()
{
    ofono_call_volume_driver_register(&binder_call_volume_driver);
}

void
binder_call_volume_cleanup()
{
    ofono_call_volume_driver_unregister(&binder_call_volume_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
