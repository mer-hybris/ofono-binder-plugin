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
#include "binder_ext_slot.h"
#include "binder_ext_sms.h"

#include <ofono/ims.h>
#include <ofono/log.h>

enum binder_ims_events {
    EVENT_IMS_REGISTRATION_CHANGED,
    EVENT_COUNT
};

typedef struct binder_ims {
    struct ofono_ims* ims;
    char* log_prefix;
    BinderImsReg* reg;
    gulong event_id[EVENT_COUNT];
    guint start_id;
    int caps;
} BinderIms;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderIms* binder_ims_get_data(struct ofono_ims* ims)
    { return ofono_ims_get_data(ims); }
static gboolean binder_ims_registered(BinderIms* self)
    { return self->reg && self->reg->registered; }
static int binder_ims_flags(BinderIms* self)
    { return binder_ims_registered(self) ? self->caps : 0; }

static
void
binder_ims_registration_changed(
    BinderImsReg* reg,
    BINDER_IMS_REG_PROPERTY property,
    gpointer user_data)
{
    BinderIms* self = user_data;

    DBG_(self, "");
    ofono_ims_status_notify(self->ims, binder_ims_registered(self),
        binder_ims_flags(self));
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

    cb(binder_error_ok(&err), binder_ims_registered(self),
        binder_ims_flags(self), data);
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

    self->event_id[EVENT_IMS_REGISTRATION_CHANGED] =
        binder_ims_reg_add_property_handler(self->reg,
            BINDER_IMS_REG_PROPERTY_REGISTERED,
            binder_ims_registration_changed, self);

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

    binder_ims_reg_remove_all_handlers(self->reg, self->event_id);
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
