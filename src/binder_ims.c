/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2024 Slava Monich <slava@monich.com>
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
#include "binder_ims_reg.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include "binder_ext_ims.h"

#include <ofono/ims.h>

#include <gutil_macros.h>

typedef struct binder_ims {
    struct ofono_ims* handle;
    char* log_prefix;
    BinderImsReg* ims;
    BinderExtIms* ext;
    gulong event_id;
    guint ext_req_id;
    guint start_id;
} BinderIms;

typedef struct binder_ims_cbd {
    BinderIms* self;
    ofono_ims_register_cb_t cb;
    void* cb_data;
} BinderImsCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderIms* binder_ims_get_data(struct ofono_ims* handle)
    { return ofono_ims_get_data(handle); }

static
BinderImsCbData*
binder_ims_cbd_new(
    BinderIms* self,
    ofono_ims_register_cb_t cb,
    void* cb_data)
{
    BinderImsCbData* cbd = g_slice_new0(BinderImsCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->cb_data = cb_data;
    return cbd;
}

static
void
binder_ims_cbd_free(
    BinderImsCbData* cbd)
{
    gutil_slice_free(cbd);
}

static
void
binder_ims_register_complete(
    BinderExtIms* ext,
    BINDER_EXT_IMS_RESULT result,
    void* user_data)
{
    BinderImsCbData* cbd = user_data;
    BinderIms* self = cbd->self;
    struct ofono_error err;

    self->ext_req_id = 0;
    if (result == BINDER_EXT_IMS_RESULT_OK) {
        binder_error_init_ok(&err);
    } else {
        binder_error_init_failure(&err);
    }
    cbd->cb(&err, cbd->cb_data);
}

static
void
binder_ims_notify(
    BinderIms* self)
{
    BinderImsReg* ims = self->ims;

    ofono_ims_status_notify(self->handle, ims->registered,
        ims->registered ? ims->caps : 0);
}

static
void
binder_ims_registration_changed(
    BinderImsReg* reg,
    BINDER_IMS_REG_PROPERTY property,
    gpointer user_data)
{
    BinderIms* self = user_data;

    DBG_(self, "");
    GASSERT(property == BINDER_IMS_REG_PROPERTY_REGISTERED);
    binder_ims_notify(self);
}

static
void
binder_ims_control(
    struct ofono_ims* handle,
    BINDER_EXT_IMS_REGISTRATION registration,
    ofono_ims_register_cb_t cb,
    void* data)
{
    BinderIms* self = binder_ims_get_data(handle);
    struct ofono_error err;

    if (self->ext) {
        BinderImsCbData* cbd = binder_ims_cbd_new(self, cb, data);

        binder_ext_ims_cancel(self->ext, self->ext_req_id);
        self->ext_req_id = binder_ext_ims_set_registration(self->ext,
            registration, binder_ims_register_complete, (GDestroyNotify)
            binder_ims_cbd_free, cbd);
        if (self->ext_req_id) {
            return;
        }
        binder_ims_cbd_free(cbd);
    }

    cb(binder_error_failure(&err), data);
}

static
void
binder_ims_register(
    struct ofono_ims* handle,
    ofono_ims_register_cb_t cb,
    void* data)
{
    binder_ims_control(handle, BINDER_EXT_IMS_REGISTRATION_ON, cb, data);
}

static
void
binder_ims_unregister(
    struct ofono_ims* handle,
    ofono_ims_register_cb_t cb,
    void* data)
{
    binder_ims_control(handle, BINDER_EXT_IMS_REGISTRATION_OFF, cb, data);
}

static
void
binder_ims_registration_status(
    struct ofono_ims* handle,
    ofono_ims_status_cb_t cb,
    void* data)
{
    BinderIms* self = binder_ims_get_data(handle);
    BinderImsReg* ims = self->ims;
    struct ofono_error err;

    cb(binder_error_ok(&err), ims->registered,
        ims->registered ? ims->caps : 0, data);
}

static
gboolean
binder_ims_start(
    gpointer user_data)
{
    BinderIms* self = user_data;

    DBG_(self, "");
    GASSERT(self->start_id);
    self->start_id = 0;

    GASSERT(!self->event_id);
    self->event_id = binder_ims_reg_add_property_handler(self->ims,
        BINDER_IMS_REG_PROPERTY_REGISTERED, binder_ims_registration_changed,
        self);

    ofono_ims_register(self->handle);
    return G_SOURCE_REMOVE;
}

static
int
binder_ims_probe(
    struct ofono_ims* handle,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderIms* self = g_new0(BinderIms, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "");

    self->handle = handle;
    self->ims = binder_ims_reg_ref(modem->ims);
    self->start_id = g_idle_add(binder_ims_start, self);
    ofono_ims_set_data(handle, self);
    return 0;
}

static
void
binder_ims_remove(
    struct ofono_ims* handle)
{
    BinderIms* self = binder_ims_get_data(handle);

    DBG_(self, "");

    if (self->start_id) {
        g_source_remove(self->start_id);
    }

    if (self->ext) {
        binder_ext_ims_cancel(self->ext, self->ext_req_id);
        binder_ext_ims_unref(self->ext);
    }

    binder_ims_reg_remove_handler(self->ims, self->event_id);
    binder_ims_reg_unref(self->ims);

    g_free(self->log_prefix);
    g_free(self);

    ofono_ims_set_data(handle, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_ims_driver binder_ims_driver = {
    .name                = BINDER_DRIVER,
    .probe               = binder_ims_probe,
    .remove              = binder_ims_remove,
    .ims_register        = binder_ims_register,
    .ims_unregister      = binder_ims_unregister,
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
