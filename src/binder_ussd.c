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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_ussd.h"
#include "binder_util.h"

#include <ofono/ussd.h>
#include <ofono/misc.h>

#include <radio_client.h>
#include <radio_request.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_misc.h>

#define SEC (1000)
#define USSD_REQUEST_TIMEOUT_MS (30 * SEC)
#define USSD_CANCEL_TIMEOUT_MS (20 * SEC)

typedef struct binder_ussd {
    struct ofono_ussd *ussd;
    char* log_prefix;
    RadioClient* client;
    RadioRequest* send_req;
    RadioRequest* cancel_req;
    gulong event_id;
    guint register_id;
} BinderUssd;

typedef struct binder_ussd_cbd {
    BinderUssd* self;
    ofono_ussd_cb_t cb;
    gpointer data;
} BinderUssdCbData;

#define DBG_(cd,fmt,args...) DBG("%s" fmt, (cd)->log_prefix, ##args)

static inline BinderUssd* binder_ussd_get_data(struct ofono_ussd *ussd)
    { return ofono_ussd_get_data(ussd); }

static
BinderUssdCbData*
binder_ussd_cbd_new(
    BinderUssd* self,
    ofono_ussd_cb_t cb,
    void* data)
{
    BinderUssdCbData* cbd = g_slice_new(struct binder_ussd_cbd);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_ussd_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderUssdCbData, cbd);
}

static
void
binder_ussd_cancel_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderUssdCbData* cbd = user_data;
    BinderUssd* self = cbd->self;
    struct ofono_error err;

    GASSERT(self->cancel_req == req);
    radio_request_unref(self->cancel_req);
    self->cancel_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_CANCEL_PENDING_USSD) {
            if (error != RADIO_ERROR_NONE) {
                ofono_warn("Error cancelling USSD: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected cancelPendingUssd response %d", resp);
        }
    } else {
        ofono_warn("Failed to cancel USSD");
    }

    /*
     * Always report sucessful completion, otherwise ofono may get
     * stuck in the USSD_STATE_ACTIVE state.
     */
    cbd->cb(binder_error_ok(&err), cbd->data);
}

static
void
binder_ussd_send_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderUssdCbData* cbd = user_data;
    BinderUssd* self = cbd->self;
    struct ofono_error err;

    GASSERT(self->send_req == req);
    radio_request_unref(self->send_req);
    self->send_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SEND_USSD) {
            if (error == RADIO_ERROR_NONE) {
                cbd->cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_warn("Error sending USSD: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected sendUssd response %d", resp);
        }
    } else {
        ofono_warn("Failed to send USSD");
    }
    cbd->cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_ussd_request(
    struct ofono_ussd* ussd,
    int dcs,
    const unsigned char* pdu,
    int len,
    ofono_ussd_cb_t cb,
    void* data)
{
    BinderUssd* self = binder_ussd_get_data(ussd);
    char* text = ofono_ussd_decode(dcs, pdu, len);
    struct ofono_error err;

    DBG_(self, "ussd request: %s", text);
    GASSERT(!self->send_req);
    radio_request_drop(self->send_req);
    self->send_req = NULL;

    if (text) {
        /* sendUssd(int32 serial, string ussd); */
        GBinderWriter writer;
        RadioRequest* req = radio_request_new(self->client,
            RADIO_REQ_SEND_USSD, &writer,
            binder_ussd_send_cb, binder_ussd_cbd_free,
            binder_ussd_cbd_new(self, cb, data));

        /* USSD text will be deallocated together with the request */
        gbinder_writer_append_hidl_string(&writer, text);
        gbinder_writer_add_cleanup(&writer, (GDestroyNotify)
            ofono_ussd_decode_free, text);

        radio_request_set_timeout(req, USSD_REQUEST_TIMEOUT_MS);
        if (radio_request_submit(req)) {
            /* Request was successfully submitted, keep the ref */
            self->send_req = req;
            return;
        }

        radio_request_unref(req);
    }

    /* Something went wrong */
    cb(binder_error_failure(&err), data);
}

static
void
binder_ussd_cancel(
    struct ofono_ussd* ussd,
    ofono_ussd_cb_t cb,
    void* data)
{
    BinderUssd* self = binder_ussd_get_data(ussd);

    ofono_info("sending ussd cancel");
    GASSERT(!self->cancel_req);
    radio_request_drop(self->cancel_req);

    /* cancelPendingUssd(int32 serial); */
    self->cancel_req = radio_request_new(self->client,
        RADIO_REQ_CANCEL_PENDING_USSD, NULL,
        binder_ussd_cancel_cb, binder_ussd_cbd_free,
        binder_ussd_cbd_new(self, cb, data));

    radio_request_set_timeout(self->cancel_req, USSD_CANCEL_TIMEOUT_MS);
    if (!radio_request_submit(self->cancel_req)) {
        struct ofono_error err;

        radio_request_unref(self->cancel_req);
        self->cancel_req = NULL;
        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_ussd_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderUssd* self = user_data;
    GBinderReader reader;
    gint32 type = 0;

    ofono_info("ussd received");

    /* onUssd(RadioIndicationType, UssdModeType modeType, string msg); */
    GASSERT(code == RADIO_IND_ON_USSD);
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &type)) {
        const char* msg = gbinder_reader_read_hidl_string_c(&reader);

        if (msg && msg[0]) {
            const int len = (int) strlen(msg);

            DBG_(self, "ussd length %d", len);

            /*
             * Message is freed by core if dcs is 0xff, we have to
             * duplicate it.
             */
            ofono_ussd_notify(self->ussd, type, 0xff,
                gutil_memdup(msg, len + 1), len);
        } else {
            ofono_ussd_notify(self->ussd, type, 0, NULL, 0);
        }
    }
}

static
gboolean
binder_ussd_register(
    gpointer user_data)
{
    BinderUssd* self = user_data;

    DBG_(self, "");
    GASSERT(self->register_id);
    self->register_id = 0;

    ofono_ussd_register(self->ussd);

    /* Register for USSD events */
    self->event_id = radio_client_add_indication_handler(self->client,
        RADIO_IND_ON_USSD, binder_ussd_notify, self);

    return G_SOURCE_REMOVE;
}

static
int
binder_ussd_probe(
    struct ofono_ussd* ussd,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderUssd* self = g_new0(BinderUssd, 1);

    self->ussd = ussd;
    self->client = radio_client_ref(modem->client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_ussd_register, self);

    DBG_(self, "");
    ofono_ussd_set_data(ussd, self);
    return 0;
}

static
void
binder_ussd_remove(
    struct ofono_ussd* ussd)
{
    BinderUssd* self = binder_ussd_get_data(ussd);

    DBG_(self, "");

    if (self->register_id) {
        g_source_remove(self->register_id);
    }

    radio_request_drop(self->send_req);
    radio_request_drop(self->cancel_req);
    radio_client_remove_handler(self->client, self->event_id);
    radio_client_unref(self->client);

    g_free(self->log_prefix);
    g_free(self);

    ofono_ussd_set_data(ussd, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_ussd_driver binder_ussd_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_ussd_probe,
    .remove         = binder_ussd_remove,
    .request        = binder_ussd_request,
    .cancel         = binder_ussd_cancel
};

void
binder_ussd_init()
{
    ofono_ussd_driver_register(&binder_ussd_driver);
}

void
binder_ussd_cleanup()
{
    ofono_ussd_driver_unregister(&binder_ussd_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
