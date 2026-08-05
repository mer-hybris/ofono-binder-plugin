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

#include "binder_cbs.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/cbs.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_messaging_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include <stdlib.h>

typedef struct binder_call_cbs_api BinderCbsApi;

typedef struct binder_cbs {
    struct ofono_cbs* cbs;
    const BinderCbsApi* api;
    RadioRequestGroup* g;
    char* log_prefix;
    guint register_id;
    gulong event_id;
} BinderCbs;

typedef struct binder_cbs_cbd {
    BinderCbs* self;
    ofono_cbs_set_cb_t cb;
    gpointer data;
} BinderCbsCbData;

typedef struct binder_cbs_range {
    guint from, to;
} BinderCbsRange;

#define CBS_CHECK_RETRY_MS    1000
#define CBS_CHECK_RETRY_COUNT 30

#define DBG_(cd,fmt,args...) DBG("%s" fmt, (cd)->log_prefix, ##args)

/* Binder API flavors */
struct binder_call_cbs_api {
    const char* name;
    RADIO_REQ set_gsm_broadcast_config_req;
    RADIO_REQ set_gsm_broadcast_activation_req;
    RADIO_IND new_broadcast_sms_ind;
    BinderReadByteArrayArg read_byte_array_arg;
    void (*write_gsm_broadcast_config_args)(
        GBinderWriter* writer,
        const BinderCbsRange* ranges,
        guint count);
};

static const BinderCbsApi binder_cbs_api_hidl;
static const BinderCbsApi binder_cbs_api_aidl;

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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setGsmBroadcastActivation tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to configure broadcasts, error %s",
            binder_radio_error_string(error));
    } else {
        cbd->cb(binder_error_ok(&err), cbd->data);
        return;
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
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        self->api->set_gsm_broadcast_activation_req, &args,
        binder_cbs_activate_cb,
        binder_cbs_callback_data_free,
        binder_cbs_callback_data_new(self, cb, data));

    DBG_(self, "%sactivating CB", activate ? "" : "de");

    /*
     * IRadio.hal:
     * oneway setGsmBroadcastActivation(int32_t serial, bool activate);
     *
     * IRadioMessaging.aidl:
     * void setGsmBroadcastActivation(in int serial, in boolean activate);
     */
    gbinder_writer_append_bool(&args, activate);  /* activate */

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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setGsmBroadcastConfig tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to set broadcast config, error %s",
            binder_radio_error_string(error));
    } else {
        binder_cbs_activate(cbd->self, TRUE, cbd->cb, cbd->data);
        return;
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
    const BinderCbsApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        api->set_gsm_broadcast_config_req, &args,
        binder_cbs_set_config_cb,
        binder_cbs_callback_data_free,
        binder_cbs_callback_data_new(self, cb, data));
    char** list = topics ? g_strsplit(topics, ",", 0) : NULL;
    guint i, n = gutil_strv_length(list);
    GArray* ranges = g_array_sized_new(FALSE, TRUE, sizeof(BinderCbsRange), n);

    /* Parce the ranges */
    for (i = 0; i < n; i++) {
        const char* entry = list[i];
        const char* delim = strchr(entry, '-');
        BinderCbsRange range;

        if (delim) {
            char** topics = g_strsplit(entry, "-", 2);

            range.from = atoi(topics[0]);
            range.to = atoi(topics[1]);
            g_strfreev(topics);
        } else {
            range.from = range.to = atoi(topics);
        }
        g_array_append_val(ranges, range);
    }
    g_strfreev(list);

    /*
     * IRadio.hal:
     * oneway setGsmBroadcastConfig(int32_t serial,
     *     vec<GsmBroadcastSmsConfigInfo> configInfo);
     *
     * IRadioMessaging.aidl:
     * void setGsmBroadcastConfig(in int serial,
     *     in GsmBroadcastSmsConfigInfo[] configInfo);
     */
    api->write_gsm_broadcast_config_args(&args, (BinderCbsRange*)
        ranges->data, ranges->len);
    g_array_free(ranges, TRUE);

    DBG_(self, "configuring CB");
    radio_request_set_retry_func(req, binder_cbs_retry);
    radio_request_set_retry(req, CBS_CHECK_RETRY_MS, CBS_CHECK_RETRY_COUNT);
    radio_request_submit(req);
    radio_request_unref(req);
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

    /*
     * IRadioIndication.hal:
     * oneway newBroadcastSms(RadioIndicationType type, vec<uint8_t> data);
     *
     * IRadioMessagingIndication.aidl:
     * void newBroadcastSms(in RadioIndicationType type, in byte[] data);
     */
    gbinder_reader_copy(&reader, args);
    ptr = self->api->read_byte_array_arg(&reader, &len);

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

    GASSERT(self->register_id);
    self->register_id = 0;
    DBG_(self, "registering for CB");
    self->event_id = radio_client_add_indication_handler(client,
        self->api->new_broadcast_sms_ind, binder_cbs_notify, self);
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
    RadioClient* messaging_client = modem->clients.messaging_client;
    const BinderCbsApi* api =
        radio_client_aidl_interface(messaging_client) ==
        RADIO_MESSAGING_INTERFACE ? &binder_cbs_api_aidl :
        &binder_cbs_api_hidl;

    self->cbs = cbs;
    self->api = api;

    self->g = radio_request_group_new(messaging_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_cbs_register, self);

    DBG_(self, "%s api", api->name);
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
    gutil_source_remove(self->register_id);
    radio_client_remove_handler(self->g->client, self->event_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_cbs_set_data(cbs, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static
void
binder_cbs_api_write_gsm_broadcast_config_args_hidl(
    GBinderWriter* writer,
    const BinderCbsRange* ranges,
    guint count)
{
    GBinderParent parent;
    GBinderHidlVec* vec = gbinder_writer_new0(writer, GBinderHidlVec);
    RadioGsmBroadcastSmsConfig* configs = NULL;
    guint i;

    vec->count = count;
    vec->owns_buffer = TRUE;
    vec->data.ptr = configs = gbinder_writer_malloc0(writer,
        sizeof(RadioGsmBroadcastSmsConfig) * count);

    for (i = 0; i < count; i++) {
        const BinderCbsRange* range = ranges + i;
        RadioGsmBroadcastSmsConfig* config = configs + i;

        config->selected = TRUE;
        config->toCodeScheme = 0xff;
        config->fromServiceId = range->from;
        config->toServiceId = range->to;
    }

    /* Every vector, even the one without data, requires two buffer objects */
    parent.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;
    parent.index = gbinder_writer_append_buffer_object(writer,
        vec, sizeof(*vec));
    gbinder_writer_append_buffer_object_with_parent(writer,
        configs, sizeof(configs[0]) * count, &parent);
}

static const BinderCbsApi binder_cbs_api_hidl = {
    "hidl",
    RADIO_REQ_SET_GSM_BROADCAST_CONFIG,
    RADIO_REQ_SET_GSM_BROADCAST_ACTIVATION,
    RADIO_IND_NEW_BROADCAST_SMS,
    binder_read_byte_array_hidl,
    binder_cbs_api_write_gsm_broadcast_config_args_hidl
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static
void
binder_cbs_api_write_gsm_broadcast_config_args_aidl(
    GBinderWriter* writer,
    const BinderCbsRange* ranges,
    guint count)
{
    guint i;

    gbinder_writer_append_int32(writer, count);
    for (i = 0; i < count; i++) {
        const BinderCbsRange* range = ranges + i;
        GBinderWriter parcel;

        /*
         * package android.hardware.radio.messaging;
         * parcelable GsmBroadcastSmsConfigInfo {
         *   int fromServiceId;
         *   int toServiceId;
         *   int fromCodeScheme;
         *   int toCodeScheme;
         *   boolean selected;
         * }
         */
        gbinder_writer_start_parcelable(writer, &parcel);
        gbinder_writer_append_int32(&parcel, range->from); /* fromServiceId */
        gbinder_writer_append_int32(&parcel, range->to);   /* toServiceId */
        gbinder_writer_append_int32(&parcel, 0);           /* fromCodeScheme */
        gbinder_writer_append_int32(&parcel, 0xff);        /* toCodeScheme  */
        gbinder_writer_append_bool(&parcel, TRUE);         /* selected */
        gbinder_writer_finish_parcelable(&parcel);
    }
}

static const BinderCbsApi binder_cbs_api_aidl = {
    "aidl",
    RADIO_MESSAGING_REQ_SET_GSM_BROADCAST_CONFIG,
    RADIO_MESSAGING_REQ_SET_GSM_BROADCAST_ACTIVATION,
    RADIO_MESSAGING_IND_NEW_BROADCAST_SMS,
    gbinder_reader_read_byte_array,
    binder_cbs_api_write_gsm_broadcast_config_args_aidl
};

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
