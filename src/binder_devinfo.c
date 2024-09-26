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

#include "binder_devinfo.h"
#include "binder_modem.h"
#include "binder_util.h"

#include <ofono/devinfo.h>
#include <ofono/log.h>

#include <radio_modem_types.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>

#include <gutil_idlequeue.h>
#include <gutil_log.h>

enum binder_devinfo_cb_tag {
    DEVINFO_QUERY_SERIAL = 1,
    DEVINFO_QUERY_SVN
};

typedef struct binder_devinfo {
    struct ofono_devinfo* di;
    RadioRequestGroup* g;
    GUtilIdleQueue* iq;
    char* log_prefix;
    char* imeisv;
    char* imei;
} BinderDevInfo;

typedef struct binder_devinfo_callback_data {
    BinderDevInfo* self;
    ofono_devinfo_query_cb_t cb;
    gpointer data;
} BinderDevInfoCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderDevInfo* binder_devinfo_get_data(struct ofono_devinfo* di)
    { return ofono_devinfo_get_data(di); }

static
BinderDevInfoCbData*
binder_devinfo_callback_data_new(
    BinderDevInfo* self,
    ofono_devinfo_query_cb_t cb,
    void* data)
{
    BinderDevInfoCbData* cbd = g_slice_new0(BinderDevInfoCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_devinfo_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderDevInfoCbData, cbd);
}

static
void
binder_devinfo_query_unsupported(
    struct ofono_devinfo* di,
    ofono_devinfo_query_cb_t cb,
    void* data)
{
    struct ofono_error error;

    cb(binder_error_failure(&error), "", data);
}

static
void
binder_devinfo_query_revision_ok(
    const BinderDevInfoCbData* cbd,
    const GBinderReader* args)
{
    struct ofono_error err;
    GBinderReader reader;
    char* res;
    RADIO_AIDL_INTERFACE interface_aidl =
        radio_client_aidl_interface(cbd->self->g->client);

    /* getBasebandVersionResponse(RadioResponseInfo, string version); */
    gbinder_reader_copy(&reader, args);
    if (interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        res = gbinder_reader_read_hidl_string(&reader);
    } else {
        res = gbinder_reader_read_string16(&reader);

    }

    DBG_(cbd->self, "%s", res);
    cbd->cb(binder_error_ok(&err), res ? res : "", cbd->data);

    g_free(res);
}

static
void
binder_devinfo_query_revision_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    struct ofono_error err;
    const BinderDevInfoCbData* cbd = user_data;
    guint32 code =
        radio_client_aidl_interface(
            cbd->self->g->client) == RADIO_MODEM_INTERFACE ?
                RADIO_MODEM_RESP_GET_BASEBAND_VERSION :
                RADIO_RESP_GET_BASEBAND_VERSION;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                binder_devinfo_query_revision_ok(cbd, args);
                return;
            } else {
                ofono_error("getBasebandVersion error %d", error);
            }
        } else {
            ofono_error("Unexpected getBasebandVersion response %d", resp);
        }
    }
    cbd->cb(binder_error_failure(&err), NULL, cbd->data);
}

static
void
binder_devinfo_query_revision(
    struct ofono_devinfo* di,
    ofono_devinfo_query_cb_t cb,
    void* data)
{
    BinderDevInfo* self = binder_devinfo_get_data(di);
    guint32 code =
        (radio_client_aidl_interface(self->g->client) == RADIO_MODEM_INTERFACE) ?
            RADIO_MODEM_REQ_GET_BASEBAND_VERSION :
            RADIO_REQ_GET_BASEBAND_VERSION;

    RadioRequest* req = radio_request_new2(self->g,
        code, NULL,
        binder_devinfo_query_revision_cb,
        binder_devinfo_callback_data_free,
        binder_devinfo_callback_data_new(self, cb, data));

    DBG_(self, "");
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_devinfo_query_serial_cb(
    gpointer user_data)
{
    BinderDevInfoCbData* cbd = user_data;
    BinderDevInfo* self = cbd->self;
    struct ofono_error error;

    DBG_(self, "%s", self->imei);
    cbd->cb(binder_error_ok(&error), self->imei, cbd->data);
}

static
void
binder_devinfo_query_svn_cb(
    gpointer user_data)
{
    BinderDevInfoCbData* cbd = user_data;
    BinderDevInfo* self = cbd->self;
    struct ofono_error error;

    DBG_(self, "%s", self->imeisv);
    if (self->imeisv && self->imeisv[0]) {
        cbd->cb(binder_error_ok(&error), self->imeisv, cbd->data);
    } else {
        cbd->cb(binder_error_failure(&error), "", cbd->data);
    }
}

static
void
binder_devinfo_query(
    BinderDevInfo* self,
    enum binder_devinfo_cb_tag tag,
    GUtilIdleFunc fn,
    ofono_devinfo_query_cb_t cb,
    void* data)
{
    GVERIFY_FALSE(gutil_idle_queue_cancel_tag(self->iq, tag));
    gutil_idle_queue_add_tag_full(self->iq, tag, fn,
        binder_devinfo_callback_data_new(self, cb, data),
        binder_devinfo_callback_data_free);
}

static
void
binder_devinfo_query_serial(
    struct ofono_devinfo* devinfo,
    ofono_devinfo_query_cb_t cb,
    void* data)
{
    BinderDevInfo* self = binder_devinfo_get_data(devinfo);

    DBG_(self, "");
    binder_devinfo_query(self, DEVINFO_QUERY_SERIAL,
        binder_devinfo_query_serial_cb, cb, data);
}

static
void
binder_devinfo_query_svn(
    struct ofono_devinfo* devinfo,
    ofono_devinfo_query_cb_t cb,
    void* data)
{
    BinderDevInfo* self = binder_devinfo_get_data(devinfo);

    DBG_(self, "");
    binder_devinfo_query(self, DEVINFO_QUERY_SVN,
        binder_devinfo_query_svn_cb, cb, data);
}

static
void
binder_devinfo_register(
    gpointer user_data)
{
    BinderDevInfo* self = user_data;

    DBG_(self, "");
    ofono_devinfo_register(self->di);
}

static
int
binder_devinfo_probe(
    struct ofono_devinfo* di,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderDevInfo* self = g_new0(BinderDevInfo, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);

    DBG_(self, "%s", modem->imei);
    self->g = radio_request_group_new(modem->client);
    self->di = di;
    self->imeisv = g_strdup(modem->imeisv);
    self->imei = g_strdup(modem->imei);
    self->iq = gutil_idle_queue_new();
    gutil_idle_queue_add(self->iq, binder_devinfo_register, self);
    ofono_devinfo_set_data(di, self);
    return 0;
}

static
void
binder_devinfo_remove(
    struct ofono_devinfo* di)
{
    BinderDevInfo* self = binder_devinfo_get_data(di);

    DBG_(self, "");
    ofono_devinfo_set_data(di, NULL);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    gutil_idle_queue_cancel_all(self->iq);
    gutil_idle_queue_unref(self->iq);
    g_free(self->log_prefix);
    g_free(self->imeisv);
    g_free(self->imei);
    g_free(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_devinfo_driver binder_devinfo_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_devinfo_probe,
    .remove         = binder_devinfo_remove,
    /* query_revision won't be called if query_model is missing */
    .query_model    = binder_devinfo_query_unsupported,
    .query_revision = binder_devinfo_query_revision,
    .query_serial   = binder_devinfo_query_serial,
    .query_svn      = binder_devinfo_query_svn
};

void
binder_devinfo_init()
{
    ofono_devinfo_driver_register(&binder_devinfo_driver);
}

void
binder_devinfo_cleanup()
{
    ofono_devinfo_driver_unregister(&binder_devinfo_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
