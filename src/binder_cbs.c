/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
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

#include "binder_cbs.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/cbs.h>
#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_messaging_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_strv.h>

#include <stdlib.h>

typedef struct binder_cbs {
    struct ofono_cbs* cbs;
    RadioRequestGroup* g;
    RADIO_AIDL_INTERFACE interface_aidl;
    char* log_prefix;
    guint register_id;
    gulong event_id;
} BinderCbs;

typedef struct binder_cbs_cbd {
    BinderCbs* self;
    ofono_cbs_set_cb_t cb;
    gpointer data;
} BinderCbsCbData;

#define CBS_CHECK_RETRY_MS    1000
#define CBS_CHECK_RETRY_COUNT 30

#define DBG_(cd,fmt,args...) DBG("%s" fmt, (cd)->log_prefix, ##args)

static inline BinderCbs* binder_cbs_get_data(struct ofono_cbs* cbs)
    { return ofono_cbs_get_data(cbs); }

static
BinderCbsCbData*
binder_cbs_callback_data_new(
    BinderCbs* self,
    ofono_cbs_set_cb_t cb,
    void* data)
{
    BinderCbsCbData* cbd = g_slice_new0(BinderCbsCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_cbs_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderCbsCbData, cbd);
}

static
gboolean
binder_cbs_retry(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    return error == RADIO_ERROR_INVALID_STATE;
}

static
void
binder_cbs_activate_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    struct ofono_error err;
    BinderCbsCbData* cbd = user_data;
    guint32 code = cbd->self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_RESP_SET_GSM_BROADCAST_ACTIVATION :
        RADIO_RESP_SET_GSM_BROADCAST_ACTIVATION;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                cbd->cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_warn("Failed to configure broadcasts, error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected setGsmBroadcastActivation response %d",
                resp);
        }
    }
    cbd->cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_cbs_activate(
    BinderCbs* self,
    gboolean activate,
    ofono_cbs_set_cb_t cb,
    void* data)
{
    /* setGsmBroadcastActivation(int32_t serial, bool activate); */
    GBinderWriter writer;
    guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_REQ_SET_GSM_BROADCAST_ACTIVATION :
        RADIO_REQ_SET_GSM_BROADCAST_ACTIVATION;
    RadioRequest* req = radio_request_new2(self->g,
        code, &writer,
        binder_cbs_activate_cb,
        binder_cbs_callback_data_free,
        binder_cbs_callback_data_new(self, cb, data));

    gbinder_writer_append_bool(&writer, activate);  /* activate */
    DBG_(self, "%sactivating CB", activate ? "" : "de");
    radio_request_set_retry_func(req, binder_cbs_retry);
    radio_request_set_retry(req, CBS_CHECK_RETRY_MS, CBS_CHECK_RETRY_COUNT);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_cbs_set_config_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCbsCbData* cbd = user_data;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = cbd->self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_RESP_SET_GSM_BROADCAST_CONFIG :
            RADIO_RESP_SET_GSM_BROADCAST_CONFIG;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                binder_cbs_activate(cbd->self, TRUE, cbd->cb, cbd->data);
                return;
            } else {
                ofono_warn("Failed to set broadcast config, error %d", error);
            }
        } else {
            ofono_error("Unexpected setGsmBroadcastConfig response %d",
                resp);
        }
    }
    cbd->cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_cbs_set_config(
    BinderCbs* self,
    const char* topics,
    ofono_cbs_set_cb_t cb,
    void* data)
{
    GBinderWriter writer;
    guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_REQ_SET_GSM_BROADCAST_CONFIG :
        RADIO_REQ_SET_GSM_BROADCAST_CONFIG;
    RadioRequest* req = radio_request_new2(self->g,
        code, &writer,
        binder_cbs_set_config_cb,
        binder_cbs_callback_data_free,
        binder_cbs_callback_data_new(self, cb, data));

    char** list = topics ? g_strsplit(topics, ",", 0) : NULL;
    const guint count = gutil_strv_length(list);

    if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        /* setGsmBroadcastConfig(int32_t serial, vec<GsmBroadcastSmsConfigInfo>); */
        GBinderParent parent;
        GBinderHidlVec* vec = gbinder_writer_new0(&writer, GBinderHidlVec);
        RadioGsmBroadcastSmsConfig* configs = NULL;
        guint i;

        vec->count = count;
        vec->owns_buffer = TRUE;
        vec->data.ptr = configs = gbinder_writer_malloc0(&writer,
            sizeof(RadioGsmBroadcastSmsConfig) * count);

        for (i = 0; i < count; i++) {
            RadioGsmBroadcastSmsConfig* config = configs + i;
            const char* entry = list[i];
            const char* delim = strchr(entry, '-');

            config->selected = TRUE;
            config->toCodeScheme = 0xff;
            if (delim) {
                char** range = g_strsplit(entry, "-", 0);

                config->fromServiceId = atoi(range[0]);
                config->toServiceId = atoi(range[1]);
                g_strfreev(range);
            } else {
                config->fromServiceId = config->toServiceId = atoi(entry);
            }
        }

        /* Every vector, even the one without data, requires two buffer objects */
        parent.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;
        parent.index = gbinder_writer_append_buffer_object(&writer, vec,
            sizeof(*vec));
        gbinder_writer_append_buffer_object_with_parent(&writer, configs,
            sizeof(configs[0]) * count, &parent);
    } else {
        /* setGsmBroadcastConfig(int32_t serial, GsmBroadcastSmsConfigInfo[]); */
        guint i;

        gbinder_writer_append_int32(&writer, count);

        for (i = 0; i < count; i++) {
            const char* entry = list[i];
            const char* delim = strchr(entry, '-');

            /* Non-null parcelable */
            gbinder_writer_append_int32(&writer, 1);
            /* Parcelable size */
            gbinder_writer_append_int32(&writer, 6 * sizeof(gint32));

            if (delim) {
                char** range = g_strsplit(entry, "-", 0);

                gbinder_writer_append_int32(&writer, atoi(range[0]));
                gbinder_writer_append_int32(&writer, atoi(range[1]));
                g_strfreev(range);
            } else {
                gbinder_writer_append_int32(&writer, atoi(entry));
                gbinder_writer_append_int32(&writer, atoi(entry));
            }
            gbinder_writer_append_int32(&writer, 0);
            gbinder_writer_append_int32(&writer, 0xff);
            gbinder_writer_append_bool(&writer, TRUE);
        }
    }

    DBG_(self, "configuring CB");
    radio_request_set_retry_func(req, binder_cbs_retry);
    radio_request_set_retry(req, CBS_CHECK_RETRY_MS, CBS_CHECK_RETRY_COUNT);
    radio_request_submit(req);
    radio_request_unref(req);
    g_strfreev(list);
}

static
void
binder_cbs_set_topics(
    struct ofono_cbs* cbs,
    const char* topics,
    ofono_cbs_set_cb_t cb,
    void* data)
{
    BinderCbs* self = binder_cbs_get_data(cbs);

    DBG_(self, "%s", topics);
    binder_cbs_set_config(self, topics, cb, data);
}

static
void
binder_cbs_clear_topics(
    struct ofono_cbs* cbs,
    ofono_cbs_set_cb_t cb,
    void *data)
{
    BinderCbs* self = binder_cbs_get_data(cbs);

    DBG_(self, "");
    binder_cbs_activate(self, FALSE, cb, data);
}

static
void
binder_cbs_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCbs* self = user_data;
    GBinderReader reader;
    const guchar* ptr;
    gsize len;
    guint32 ind_code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_IND_NEW_BROADCAST_SMS :
        RADIO_IND_NEW_BROADCAST_SMS;

    /* newBroadcastSms(RadioIndicationType type, vec<uint8_t> data); */
    GASSERT(code == ind_code);
    gbinder_reader_copy(&reader, args);
    if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        ptr = gbinder_reader_read_hidl_byte_vec(&reader, &len);
    } else {
        ptr = gbinder_reader_read_byte_array(&reader, &len);
    }

    /* By default assume that it's a length followed by the binary PDU data. */
    if (ptr) {
        if (len > 4) {
            const guint32 pdu_len = GUINT32_FROM_LE(*(guint32*)ptr);

            if (G_ALIGN4(pdu_len) == (len - 4)) {
                DBG_(self, "%u bytes", pdu_len);
                ofono_cbs_notify(self->cbs, ptr + 4, pdu_len);
                return;
            }
        }

        /*
         * But I've seen cell broadcasts arriving without the length,
         * simply as a blob.
         */
        ofono_cbs_notify(self->cbs, ptr, (guint) len);
    }
}

static
gboolean
binder_cbs_register(
    gpointer user_data)
{
    BinderCbs* self = user_data;
    RadioClient* client = self->g->client;
    guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_IND_NEW_BROADCAST_SMS :
        RADIO_IND_NEW_BROADCAST_SMS;

    GASSERT(self->register_id);
    self->register_id = 0;
    DBG_(self, "registering for CB");
    self->event_id = radio_client_add_indication_handler(client,
        code, binder_cbs_notify, self);
    ofono_cbs_register(self->cbs);
    return G_SOURCE_REMOVE;
}

static
int
binder_cbs_probe(
    struct ofono_cbs* cbs,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderCbs* self = g_new0(BinderCbs, 1);

    self->cbs = cbs;
    self->g = radio_request_group_new(modem->messaging_client); /* Keeps ref to client */
    self->interface_aidl = radio_client_aidl_interface(modem->messaging_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_cbs_register, self);

    DBG_(self, "");
    ofono_cbs_set_data(cbs, self);
    return 0;
}

static
void
binder_cbs_remove(
    struct ofono_cbs* cbs)
{
    BinderCbs* self = binder_cbs_get_data(cbs);

    DBG_(self, "");
    if (self->register_id) {
        g_source_remove(self->register_id);
    }
    radio_client_remove_handler(self->g->client, self->event_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_cbs_set_data(cbs, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_cbs_driver binder_cbs_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_cbs_probe,
    .remove         = binder_cbs_remove,
    .set_topics     = binder_cbs_set_topics,
    .clear_topics   = binder_cbs_clear_topics
};

void
binder_cbs_init()
{
    ofono_cbs_driver_register(&binder_cbs_driver);
}

void
binder_cbs_cleanup()
{
    ofono_cbs_driver_unregister(&binder_cbs_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
