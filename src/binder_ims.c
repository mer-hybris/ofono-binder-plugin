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
#include "binder_ims_reg.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_util.h"

#include "binder_ext_call.h"
#include "binder_ext_ims.h"
#include "binder_ext_slot.h"
#include "binder_ext_sms.h"

#include <ofono/ims.h>
#include <ofono/log.h>

#include <gutil_macros.h>

enum binder_ims_events {
    EVENT_IMS_REGISTRATION_CHANGED,
    EVENT_COUNT
};

enum binder_ims_ext_events {
    IMS_EXT_STATE_CHANGED,
    IMS_EXT_EVENT_COUNT
};

typedef struct binder_ims {
    struct ofono_ims* ims;
    char* log_prefix;
    BinderImsReg* reg;
    BinderExtIms* ext;
    gulong radio_event[EVENT_COUNT];
    gulong ext_event[IMS_EXT_EVENT_COUNT];
    guint ext_req_id;
    guint start_id;
    int caps;
} BinderIms;

typedef struct binder_ims_cbd {
    BinderIms* self;
    ofono_ims_register_cb_t cb;
    void* cb_data;
} BinderImsCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderIms* binder_ims_get_data(struct ofono_ims* ims)
    { return ofono_ims_get_data(ims); }

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
gboolean
binder_ims_is_registered(
    BinderIms* self)
{
    if (self->ext) {
        return binder_ext_ims_get_state(self->ext) ==
            BINDER_EXT_IMS_STATE_REGISTERED;
    } else if (self->reg) {
        return self->reg->registered;
    } else {
        return FALSE;
    }
}

static
void
binder_ims_notify(
    BinderIms* self)
{
    const gboolean registered = binder_ims_is_registered(self);

    ofono_ims_status_notify(self->ims, registered, registered ? self->caps : 0);
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
    binder_ims_notify(self);
}

static
void
binder_ims_ext_state_changed(
    BinderExtIms* ext,
    void* user_data)
{
    BinderIms* self = user_data;

    DBG_(self, "");
    binder_ims_notify(self);
}

static
void
binder_ims_control(
    struct ofono_ims* ims,
    BINDER_EXT_IMS_REGISTRATION registration,
    ofono_ims_register_cb_t cb,
    void* data)
{
    BinderIms* self = binder_ims_get_data(ims);
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
    struct ofono_ims* ims,
    ofono_ims_register_cb_t cb,
    void* data)
{
    binder_ims_control(ims, BINDER_EXT_IMS_REGISTRATION_ON, cb, data);
}

static
void
binder_ims_unregister(
    struct ofono_ims* ims,
    ofono_ims_register_cb_t cb,
    void* data)
{
    binder_ims_control(ims, BINDER_EXT_IMS_REGISTRATION_OFF, cb, data);
}

static
void
binder_ims_registration_status(
    struct ofono_ims* ims,
    ofono_ims_status_cb_t cb,
    void* data)
{
    BinderIms* self = binder_ims_get_data(ims);
    const gboolean registered = binder_ims_is_registered(self);
    struct ofono_error err;

    cb(binder_error_ok(&err), registered, registered ? self->caps : 0, data);
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

    if (self->ext) {
        self->ext_event[IMS_EXT_STATE_CHANGED] =
            binder_ext_ims_add_state_handler(self->ext,
                binder_ims_ext_state_changed, self);
    }

    if (!self->ext_event[IMS_EXT_STATE_CHANGED]) {
        self->radio_event[EVENT_IMS_REGISTRATION_CHANGED] =
            binder_ims_reg_add_property_handler(self->reg,
                BINDER_IMS_REG_PROPERTY_REGISTERED,
                binder_ims_registration_changed, self);
    }

    ofono_ims_register(self->ims);
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

    self->reg = binder_ims_reg_ref(modem->ims);
    self->ims = ims;

    if (modem->ext && (self->ext =
        binder_ext_slot_get_interface(modem->ext,
        BINDER_EXT_TYPE_IMS)) != NULL) {
        BINDER_EXT_IMS_INTERFACE_FLAGS flags =
            binder_ext_ims_get_interface_flags(self->ext);

        DBG_(self, "using ims extension");
        binder_ext_ims_ref(self->ext);
        if (flags & BINDER_EXT_IMS_INTERFACE_FLAG_SMS_SUPPORT) {
            DBG_(self, "ims sms support is detected");
            self->caps |= OFONO_IMS_SMS_CAPABLE;
        }
        if (flags & BINDER_EXT_IMS_INTERFACE_FLAG_VOICE_SUPPORT) {
            DBG_(self, "ims call support is detected");
            self->caps |= OFONO_IMS_VOICE_CAPABLE;
        }
    } else {
        if (binder_ext_sms_get_interface_flags
           (binder_ext_slot_get_interface(modem->ext, BINDER_EXT_TYPE_SMS)) &
            BINDER_EXT_SMS_INTERFACE_FLAG_IMS_SUPPORT) {
            DBG_(self, "ims sms support is detected");
            self->caps |= OFONO_IMS_SMS_CAPABLE;
        }
        if (binder_ext_call_get_interface_flags
           (binder_ext_slot_get_interface(modem->ext, BINDER_EXT_TYPE_CALL)) &
            BINDER_EXT_CALL_INTERFACE_FLAG_IMS_SUPPORT) {
            DBG_(self, "ims call support is detected");
            self->caps |= OFONO_IMS_VOICE_CAPABLE;
        }
    }
    self->reg->caps = self->caps;

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

    if (self->ext) {
        binder_ext_ims_remove_all_handlers(self->ext, self->ext_event);
        binder_ext_ims_cancel(self->ext, self->ext_req_id);
        binder_ext_ims_unref(self->ext);
    }

    binder_ims_reg_remove_all_handlers(self->reg, self->radio_event);
    binder_ims_reg_unref(self->reg);

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
