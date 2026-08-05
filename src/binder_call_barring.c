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

#include "binder_call_barring.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_sim_card.h"
#include "binder_util.h"

#include <ofono/call-barring.h>
#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_network_types.h>
#include <radio_sim_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_misc.h>

typedef struct binder_call_barring_api BinderCallBarringApi;

typedef struct binder_call_barring {
    struct ofono_call_barring* b;
    const BinderCallBarringApi* api;
    BinderSimCard* card;
    RadioRequestGroup* sim_g;
    RadioRequestGroup* network_g;
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

/* Binder API flavors */
struct binder_call_barring_api {
    const char* name;
    BinderWriteStringArg write_string_arg;
    RADIO_REQ sim_get_facility_lock_for_app_req;
    RADIO_REQ sim_set_facility_lock_for_app_req;
    RADIO_REQ network_set_barring_password_req;
};

static const BinderCallBarringApi binder_call_barring_api_hidl;
static const BinderCallBarringApi binder_call_barring_api_aidl;

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
     * IRadioResponse.hal:
     * oneway getFacilityLockForAppResponse(RadioResponseInfo info, int32_t response);
     *
     * IRadioSimResponse.aidl:
     * void getFacilityLockForAppResponse(in RadioResponseInfo info, in int response);
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "getFacilityLockForApp tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Call Barring query error %s",
            binder_radio_error_string(error));
    } else if (binder_call_barring_query_ok(cbd, args)) {
        return;
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
    const BinderCallBarringApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->sim_g,
        api->sim_get_facility_lock_for_app_req, &args,
        binder_call_barring_query_cb,
        binder_call_barring_callback_data_free,
        binder_call_barring_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "lock: %s, services to query: 0x%02x", lock, cls);

    /*
     * IRadio.hal:
     * oneway getFacilityLockForApp(int32_t serial, string facility,
     *     string password, int32_t serviceClass, string appId);
     *
     * IRadioSim.aidl:
     * void getFacilityLockForApp(in int serial, in String facility,
     *     in String password, in int serviceClass, in String appId);
     */
    binder_append_hidl_string(&args, lock);   /* facility */
    api->write_string_arg(&args, "");         /* password */
    gbinder_writer_append_int32(&args, cls);  /* serviceClass */
    api->write_string_arg(&args,              /* appId */
        binder_sim_card_app_aid(self->card));

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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setFacilityLockForApp tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("Call Barring Set error %s",
            binder_radio_error_string(error));
    } else {
        cb(binder_error_ok(&err), cbd->data);
        return;
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
    const BinderCallBarringApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->sim_g,
        api->sim_set_facility_lock_for_app_req, &args,
        binder_call_barring_set_cb,
        binder_call_barring_callback_data_free,
        binder_call_barring_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "lock: %s, enable: %i, bearer class: %i", lock, enable, cls);

    /*
     * IRadio.hal:
     * oneway setFacilityLockForApp(int32_t serial, string facility,
     *     bool lockState, string password, int32_t serviceClass,
     *     string appId);
     *
     * IRadioSim.aidl:
     * void setFacilityLockForApp(in int serial, in String facility,
     *     in boolean lockState, in String password, in int serviceClass,
     *     in String appId);
     */
    api->write_string_arg(&args, lock);         /* facility */
    gbinder_writer_append_bool(&args, enable);  /* lockState */
    api->write_string_arg(&args, passwd);       /* password */
    gbinder_writer_append_int32(&args, cls);    /* serviceClass */
    api->write_string_arg(&args,                /* appId */
        binder_sim_card_app_aid(self->card));

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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setBarringPassword tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("Call Barring Set PW error %s",
            binder_radio_error_string(error));
    } else {
        cb(binder_error_ok(&err), cbd->data);
        return;
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
    const BinderCallBarringApi* api = self->api;

    /*
     * setBarringPassword(int32_t serial, string facility,
     *     string oldPassword, string newPassword);
     */
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->network_g,
        api->network_set_barring_password_req, &args,
        binder_call_barring_set_passwd_cb,
        binder_call_barring_callback_data_free,
        binder_call_barring_callback_data_new(self, BINDER_CB(cb), data));

    DBG_(self, "%s", lock);

    /*
     * IRadio.hal:
     * oneway setBarringPassword(int32_t serial, string facility,
     *     string oldPassword, string newPassword);
     *
     * IRadioNetwork.aidl:
     * void setBarringPassword(in int serial, in String facility,
     *     in String oldPassword, in String newPassword);
     */
    api->write_string_arg(&args, lock);         /* facility */
    api->write_string_arg(&args, old_passwd);   /* oldPassword */
    api->write_string_arg(&args, new_passwd);   /* newPassword */

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
    RadioClient* sim_client = modem->clients.sim_client;
    const BinderCallBarringApi* api =
        radio_client_aidl_interface(sim_client) == RADIO_SIM_INTERFACE ?
        &binder_call_barring_api_aidl : &binder_call_barring_api_hidl;

    self->b = b;
    self->api = api;
    self->card = binder_sim_card_ref(modem->sim_card);
    self->sim_g = radio_request_group_new(sim_client);
    self->network_g = radio_request_group_new(modem->clients.network_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_call_barring_register, self);

    DBG_(self, "%s api", api->name);
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
    gutil_source_remove(self->register_id);
    binder_sim_card_unref(self->card);
    radio_request_group_cancel(self->sim_g);
    radio_request_group_unref(self->sim_g);
    radio_request_group_cancel(self->network_g);
    radio_request_group_unref(self->network_g);
    g_free(self->log_prefix);
    g_free(self);

    ofono_call_barring_set_data(b, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static const BinderCallBarringApi binder_call_barring_api_hidl = {
    "hidl",
    binder_write_string_arg_hidl,
    RADIO_REQ_GET_FACILITY_LOCK_FOR_APP,
    RADIO_REQ_SET_FACILITY_LOCK_FOR_APP,
    RADIO_REQ_SET_BARRING_PASSWORD,
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static const BinderCallBarringApi binder_call_barring_api_aidl = {
    "aidl",
    binder_write_string_arg_aidl,
    RADIO_SIM_REQ_GET_FACILITY_LOCK_FOR_APP,
    RADIO_SIM_REQ_SET_FACILITY_LOCK_FOR_APP,
    RADIO_NETWORK_REQ_SET_BARRING_PASSWORD,
};

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
