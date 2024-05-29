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

#include "binder_call_barring.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_sim_card.h"
#include "binder_util.h"

#include <ofono/call-barring.h>
#include <ofono/log.h>

#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

typedef struct binder_call_barring {
    struct ofono_call_barring* b;
    BinderSimCard* card;
    RadioRequestGroup* g;
    char* log_prefix;
    guint register_id;
} BinderCallBarring;

typedef struct binder_call_barring_callback_data {
    BinderCallBarring* self;
    union call_barring_cb {
        ofono_call_barring_query_cb_t query;
        ofono_call_barring_set_cb_t set;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderCallBarringCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderCallBarring*
binder_call_barring_get_data(struct ofono_call_barring* b)
    { return ofono_call_barring_get_data(b); }

static
BinderCallBarringCbData*
binder_call_barring_callback_data_new(
    BinderCallBarring* self,
    BinderCallback cb,
    void* data)
{
    BinderCallBarringCbData* cbd = g_slice_new0(BinderCallBarringCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_call_barring_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderCallBarringCbData, cbd);
}

static
gboolean
binder_call_barring_query_ok(
    const BinderCallBarringCbData* cbd,
    const GBinderReader* args)
{
    GBinderReader reader;
    gint32 response;

    /*
     * getFacilityLockForAppResponse(RadioResponseInfo, int32_t response);
     *
     * response - the TS 27.007 service class bit vector of services
     * for which the specified barring facility is active.
     * 0 means "disabled for all"
     */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &response)) {
        struct ofono_error err;

        DBG_(cbd->self, "Active services: %d", response);
        cbd->cb.query(binder_error_ok(&err), response, cbd->data);
        return TRUE;
    }
    return FALSE;
}

static
void
binder_call_barring_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallBarringCbData* cbd = user_data;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_FACILITY_LOCK_FOR_APP) {
            if (error == RADIO_ERROR_NONE) {
                if (binder_call_barring_query_ok(cbd, args)) {
                    return;
                }
            } else {
                ofono_warn("Call Barring query error %d", error);
            }
        } else {
            ofono_error("Unexpected getFacilityLockForApp response %d", resp);
        }
    }
    cbd->cb.query(binder_error_failure(&err), 0, cbd->data);
}

static
void
binder_call_barring_query(
    struct ofono_call_barring* b,
    const char* lock,
    int cls,
    ofono_call_barring_query_cb_t cb,
    void* data)
{
    BinderCallBarring* self = ofono_call_barring_get_data(b);

    ofono_warn("binder_call_barring_query cls %i", cls);
    /*
     * Modem seems to respond with error to all requests
     * made with bearer class BEARER_CLASS_DEFAULT.
     */
    if (cls == BEARER_CLASS_DEFAULT) {
        cls = RADIO_SERVICE_CLASS_NONE;
    }

    /*
     * getFacilityLockForApp(int32_t serial, string facility,
     *      string password, int32_t serviceClass, string appId);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_GET_FACILITY_LOCK_FOR_APP, &writer,
        binder_call_barring_query_cb,
        binder_call_barring_callback_data_free,
        binder_call_barring_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "lock: %s, services to query: 0x%02x", lock, cls);
    binder_append_hidl_string(&writer, lock);   /* facility */
    binder_append_hidl_string(&writer, "");     /* password */
    gbinder_writer_append_int32(&writer, cls);  /* serviceClass */
    binder_append_hidl_string(&writer, binder_sim_card_app_aid(self->card));
                                                /* appId */
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_call_barring_set_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallBarringCbData* cbd = user_data;
    ofono_call_barring_set_cb_t cb = cbd->cb.set;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SET_FACILITY_LOCK_FOR_APP) {
            /*
             * setFacilityLockForAppResponse(RadioResponseInfo, int32_t retry);
             *
             * retry - the number of retries remaining, or -1 if unknown
             */
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_error("Call Barring Set error %d", error);
            }
        } else {
            ofono_error("Unexpected setFacilityLockForApp response %d", resp);
        }
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_call_barring_set(
    struct ofono_call_barring* b,
    const char* lock,
    int enable,
    const char* passwd,
    int cls,
    ofono_call_barring_set_cb_t cb,
    void* data)
{
    BinderCallBarring* self = ofono_call_barring_get_data(b);

    ofono_warn("binder_call_barring_set cls %i", cls);
    /*
     * Modem seems to respond with error to all requests
     * made with bearer class BEARER_CLASS_DEFAULT.
     */
    if (cls == BEARER_CLASS_DEFAULT) {
        cls = RADIO_SERVICE_CLASS_NONE;
    }

    /*
     * setFacilityLockForApp(int32_t serial, string facility, bool lockState,
     *            string password, int32_t serviceClass, string appId);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SET_FACILITY_LOCK_FOR_APP, &writer,
        binder_call_barring_set_cb,
        binder_call_barring_callback_data_free,
        binder_call_barring_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "lock: %s, enable: %i, bearer class: %i", lock, enable, cls);

    binder_append_hidl_string(&writer, lock);     /* facility */
    gbinder_writer_append_bool(&writer, enable);  /* lockState */
    binder_append_hidl_string(&writer, passwd);   /* password */
    gbinder_writer_append_int32(&writer, cls);    /* serviceClass */
    binder_append_hidl_string(&writer, binder_sim_card_app_aid(self->card));
                                                  /* appId */
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_call_barring_set_passwd_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderCallBarringCbData* cbd = user_data;
    ofono_call_barring_set_cb_t cb = cbd->cb.set;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SET_BARRING_PASSWORD) {
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_error("Call Barring Set PW error %d", error);
            }
        } else {
            ofono_error("Unexpected setBarringPassword response %d", resp);
        }
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_call_barring_set_passwd(
    struct ofono_call_barring* b,
    const char* lock,
    const char* old_passwd,
    const char* new_passwd,
    ofono_call_barring_set_cb_t cb,
    void* data)
{
    BinderCallBarring* self = ofono_call_barring_get_data(b);

    /*
     * setBarringPassword(int32_t serial, string facility,
     *     string oldPassword, string newPassword);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SET_BARRING_PASSWORD, &writer,
        binder_call_barring_set_passwd_cb,
        binder_call_barring_callback_data_free,
        binder_call_barring_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "");
    binder_append_hidl_string(&writer, lock);         /* facility */
    binder_append_hidl_string(&writer, old_passwd);   /* oldPassword */
    binder_append_hidl_string(&writer, new_passwd);   /* newPassword */

    radio_request_submit(req);
    radio_request_unref(req);
}

static
gboolean
binder_call_barring_register(
    gpointer user_data)
{
    BinderCallBarring* self = user_data;

    GASSERT(self->register_id);
    self->register_id = 0;
    ofono_call_barring_register(self->b);
    return G_SOURCE_REMOVE;
}

static
int
binder_call_barring_probe(
    struct ofono_call_barring* b,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderCallBarring* self = g_new0(struct binder_call_barring, 1);

    self->b = b;
    self->card = binder_sim_card_ref(modem->sim_card);
    self->g = radio_request_group_new(modem->client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_call_barring_register, self);

    DBG_(self, "");
    ofono_call_barring_set_data(b, self);
    return 0;
}

static
void
binder_call_barring_remove(
    struct ofono_call_barring* b)
{
    BinderCallBarring* self = binder_call_barring_get_data(b);

    DBG_(self, "");
    if (self->register_id) {
        g_source_remove(self->register_id);
    }
    binder_sim_card_unref(self->card);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_call_barring_set_data(b, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_call_barring_driver binder_call_barring_driver = {
    .name       = BINDER_DRIVER,
    .probe      = binder_call_barring_probe,
    .remove     = binder_call_barring_remove,
    .query      = binder_call_barring_query,
    .set        = binder_call_barring_set,
    .set_passwd = binder_call_barring_set_passwd
};

void
binder_call_barring_init()
{
    ofono_call_barring_driver_register(&binder_call_barring_driver);
}

void
binder_call_barring_cleanup()
{
    ofono_call_barring_driver_unregister(&binder_call_barring_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
