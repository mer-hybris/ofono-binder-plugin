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

#include "binder_data.h"
#include "binder_gprs.h"
#include "binder_modem.h"
#include "binder_log.h"
#include "binder_netreg.h"
#include "binder_network.h"
#include "binder_util.h"

#include <ofono/gprs.h>
#include <ofono/log.h>
#include <ofono/misc.h>
#include <ofono/netreg.h>
#include <ofono/watch.h>

enum binder_gprs_network_events {
    NETWORK_EVENT_DATA_STATE,
    NETWORK_EVENT_MAX_DATA_CALLS,
    NETWORK_EVENT_COUNT
};

typedef struct binder_gprs {
    struct ofono_gprs* gprs;
    struct ofono_watch* watch;
    BinderData* data;
    BinderNetwork* network;
    enum ofono_netreg_status reg_status;
    gboolean attached;
    gulong network_event_id[NETWORK_EVENT_COUNT];
    gulong data_event_id;
    guint set_attached_id;
    guint init_id;
    char* log_prefix;
} BinderGprs;

typedef struct binder_gprs_cbd {
    BinderGprs* self;
    ofono_gprs_cb_t cb;
    gpointer data;
} BinderGprsCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static BinderGprs* binder_gprs_get_data(struct ofono_gprs* ofono)
    { return ofono ? ofono_gprs_get_data(ofono) : NULL; }

static
BinderGprsCbData*
binder_gprs_cbd_new(
    BinderGprs* self,
    ofono_gprs_cb_t cb,
    void* data)
{
    BinderGprsCbData* cbd = g_slice_new0(BinderGprsCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    return cbd;
}
static
void
binder_gprs_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderGprsCbData, cbd);
}

static
enum ofono_netreg_status
binder_gprs_fix_registration_status(
    BinderGprs* self,
    enum ofono_netreg_status status)
{
    if (!binder_data_allowed(self->data)) {
        return OFONO_NETREG_STATUS_NOT_REGISTERED;
    } else {
        /*
         * TODO: need a way to make sure that SPDI information has
         * already been read from the SIM (i.e. sim_spdi_read_cb in
         * network.c has been called)
         */
        return binder_netreg_check_if_really_roaming(self->watch->netreg,
            status);
    }
}

static
void
binder_gprs_data_update_registration_state(
    BinderGprs* self)
{
    const enum ofono_netreg_status status = binder_gprs_fix_registration_status
        (self, self->network->data.status);

    if (self->reg_status != status) {
        ofono_info("data reg changed %d -> %d (%s), attached %d",
            self->reg_status, status, ofono_netreg_status_to_string(status),
            self->attached);
        self->reg_status = status;
        ofono_gprs_status_notify(self->gprs, self->reg_status);
    }
}

static
void
binder_gprs_check_data_allowed(
    BinderGprs* self)
{
    DBG_(self, "%d %d", binder_data_allowed(self->data), self->attached);
    if (!binder_data_allowed(self->data) && self->attached) {
        self->attached = FALSE;
        if (self->gprs) {
            ofono_gprs_detached_notify(self->gprs);
        }
    }

    binder_gprs_data_update_registration_state(self);
}

static
gboolean
binder_gprs_set_attached_cb(
    gpointer user_data)
{
    BinderGprsCbData* cbd = user_data;
    BinderGprs* self = cbd->self;
    struct ofono_error err;

    GASSERT(self->set_attached_id);
    self->set_attached_id = 0;
    binder_gprs_check_data_allowed(self);
    cbd->cb(binder_error_ok(&err), cbd->data);
    return G_SOURCE_REMOVE;
}

static
void
binder_gprs_set_attached(
    struct ofono_gprs* gprs,
    int attached,
    ofono_gprs_cb_t cb,
    void* data)
{
    BinderGprs* self = binder_gprs_get_data(gprs);
    struct ofono_error err;

    if (binder_data_allowed(self->data) || !attached) {
        DBG_(self, "attached: %d", attached);
        if (self->set_attached_id) {
            g_source_remove(self->set_attached_id);
        }
        self->attached = attached;
        self->set_attached_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
            binder_gprs_set_attached_cb, binder_gprs_cbd_new(self, cb, data),
            binder_gprs_cbd_free);
    } else {
        DBG_(self, "not allowed to attach");
        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_gprs_allow_data_changed(
    BinderData* data,
    BINDER_DATA_PROPERTY property,
    void* user_data)
{
    BinderGprs* self = user_data;

    DBG_(self, "%d", binder_data_allowed(data));
    if (!self->set_attached_id) {
        binder_gprs_check_data_allowed(self);
    }
}

static
void
binder_gprs_max_data_calls_changed(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    void* user_data)
{
    BinderGprs* self = user_data;

    if (net->max_data_calls > 0) {
        DBG_(self, "setting max cids to %d", net->max_data_calls);
        ofono_gprs_set_cid_range(self->gprs, 1, net->max_data_calls);
    }
}

static
void
binder_gprs_data_registration_state_changed(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    void* user_data)
{
    binder_gprs_data_update_registration_state((BinderGprs*)user_data);
}

static
void
binder_gprs_registration_status(
    struct ofono_gprs* gprs,
    ofono_gprs_status_cb_t cb,
    void* data)
{
    BinderGprs* self = binder_gprs_get_data(gprs);
    struct ofono_error err;
    const enum ofono_netreg_status status = self->attached ?
        self->reg_status : OFONO_NETREG_STATUS_NOT_REGISTERED;

    DBG("%d (%s)", status, ofono_netreg_status_to_string(status));
    cb(binder_error_ok(&err), status, data);
}

static
gboolean
binder_gprs_register(
    gpointer user_data)
{
    BinderGprs* self = user_data;
    BinderNetwork* network = self->network;
    struct ofono_gprs* gprs = self->gprs;

    self->init_id = 0;
    self->network_event_id[NETWORK_EVENT_DATA_STATE] =
        binder_network_add_property_handler(network,
            BINDER_NETWORK_PROPERTY_DATA_STATE,
            binder_gprs_data_registration_state_changed, self);
    self->network_event_id[NETWORK_EVENT_MAX_DATA_CALLS] =
        binder_network_add_property_handler(network,
            BINDER_NETWORK_PROPERTY_MAX_DATA_CALLS,
            binder_gprs_max_data_calls_changed, self);
    self->data_event_id =
        binder_data_add_property_handler(self->data,
            BINDER_DATA_PROPERTY_ALLOWED,
            binder_gprs_allow_data_changed, self);
    self->reg_status = binder_gprs_fix_registration_status(self,
        network->data.status);

    if (network->max_data_calls > 0) {
        DBG_(self, "setting max cids to %d", network->max_data_calls);
        ofono_gprs_set_cid_range(gprs, 1,network->max_data_calls);
    }

    ofono_gprs_register(gprs);
    return G_SOURCE_REMOVE;
}

static
int
binder_gprs_probe(
    struct ofono_gprs* gprs,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderGprs* self = g_new0(BinderGprs, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "");

    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->data = binder_data_ref(modem->data);
    self->network = binder_network_ref(modem->network);
    self->gprs = gprs;

    ofono_gprs_set_data(gprs, self);
    self->init_id = g_idle_add(binder_gprs_register, self);
    return 0;
}

static
void
binder_gprs_remove(
    struct ofono_gprs* gprs)
{
    BinderGprs* self = binder_gprs_get_data(gprs);

    DBG_(self, "");

    if (self->set_attached_id) {
        g_source_remove(self->set_attached_id);
    }

    if (self->init_id) {
        g_source_remove(self->init_id);
    }

    binder_network_remove_all_handlers(self->network, self->network_event_id);
    binder_network_unref(self->network);

    binder_data_remove_handler(self->data, self->data_event_id);
    binder_data_unref(self->data);

    ofono_watch_unref(self->watch);
    g_free(self->log_prefix);
    g_free(self);

    ofono_gprs_set_data(gprs, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_gprs_driver binder_gprs_driver = {
    .name             = BINDER_DRIVER,
    .probe            = binder_gprs_probe,
    .remove           = binder_gprs_remove,
    .set_attached     = binder_gprs_set_attached,
    .attached_status  = binder_gprs_registration_status
};

void
binder_gprs_init()
{
    ofono_gprs_driver_register(&binder_gprs_driver);
}

void
binder_gprs_cleanup()
{
    ofono_gprs_driver_unregister(&binder_gprs_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
