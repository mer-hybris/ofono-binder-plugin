/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2022 Jolla Ltd.
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

#include "binder_ims.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/ims.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>

enum binder_sms_events {
    IMS_EVENT_IMS_NETWORK_STATE_CHANGED,
    IMS_EVENT_COUNT
};

typedef struct binder_ims {
    struct ofono_ims* ims;
    gboolean registered;
    int flags;
    char* log_prefix;
    RadioRequestGroup* g;
    gulong event_id[IMS_EVENT_COUNT];
    guint start_id;
} BinderIms;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderIms* binder_ims_get_data(struct ofono_ims* ims)
    { return ofono_ims_get_data(ims); }

static
void
binder_ims_query_registration_state_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderIms* self = user_data;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_IMS_REGISTRATION_STATE) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                gboolean registered;
                gint32 rat;

                /*
                 * getImsRegistrationStateResponse(RadioResponseInfo info,
                 * bool isRegistered, RadioTechnologyFamily ratFamily)
                 */
                gbinder_reader_copy(&reader, args);
                if (gbinder_reader_read_bool(&reader, &registered) &&
                    gbinder_reader_read_int32(&reader, &rat)) {
                    DBG_(self, "registered: %d, rat: 0x%08x", registered, rat);
                    if (self->registered != registered) {
                        self->registered = registered;
                        self->flags = registered ? OFONO_IMS_SMS_CAPABLE : 0;
                        ofono_ims_status_notify(self->ims, registered,
                            self->flags);
                    }
                }
            } else {
                DBG_(self, "%s", binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getImsRegistrationState response %d",
                resp);
        }
    }
}

static
void
binder_ims_query_registration_state(
    BinderIms* self)
{
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_GET_IMS_REGISTRATION_STATE, NULL,
        binder_ims_query_registration_state_cb, NULL, self);

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_ims_network_state_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderIms* self = user_data;

    DBG_(self, "");
    binder_ims_query_registration_state(self);
}

static
void
binder_ims_registration_status(
    struct ofono_ims* ims,
    ofono_ims_status_cb_t cb,
    void* data)
{
    BinderIms* self = binder_ims_get_data(ims);
    struct ofono_error err;

    cb(binder_error_ok(&err), self->registered, self->flags, data);
}

static
gboolean
binder_ims_start(
    gpointer user_data)
{
    BinderIms* self = user_data;
    RadioClient* client = self->g->client;

    DBG_(self, "");
    GASSERT(self->start_id);
    self->start_id = 0;

    ofono_ims_register(self->ims);

    /* Register event handlers */
    self->event_id[IMS_EVENT_IMS_NETWORK_STATE_CHANGED] =
        radio_client_add_indication_handler(client,
            RADIO_IND_IMS_NETWORK_STATE_CHANGED,
            binder_ims_network_state_changed, self);

    /* Request the initial state */
    binder_ims_query_registration_state(self);
    return G_SOURCE_REMOVE;
}

static
int
binder_ims_probe(
    struct ofono_ims* ims,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderIms* self = g_new0(BinderIms, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "");

    self->g = radio_request_group_new(modem->client); /* Keeps ref to client */
    self->ims = ims;

    self->start_id = g_idle_add(binder_ims_start, self);
    ofono_ims_set_data(ims, self);
    return 0;
}

static
void
binder_ims_remove(
    struct ofono_ims* ims)
{
    BinderIms* self = binder_ims_get_data(ims);

    DBG_(self, "");

    if (self->start_id) {
        g_source_remove(self->start_id);
    }

    radio_client_remove_all_handlers(self->g->client, self->event_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    g_free(self->log_prefix);
    g_free(self);

    ofono_ims_set_data(ims, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_ims_driver binder_ims_driver = {
    .name                = BINDER_DRIVER,
    .probe               = binder_ims_probe,
    .remove              = binder_ims_remove,
    .registration_status = binder_ims_registration_status
};

void
binder_ims_init()
{
    ofono_ims_driver_register(&binder_ims_driver);
}

void
binder_ims_cleanup()
{
    ofono_ims_driver_unregister(&binder_ims_driver);
}


/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
