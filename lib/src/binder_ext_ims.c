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

#include "binder_ext_ims_impl.h"

G_DEFINE_INTERFACE(BinderExtIms, binder_ext_ims, G_TYPE_OBJECT)
#define GET_IFACE(obj) BINDER_EXT_IMS_GET_IFACE(obj)

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtIms*
binder_ext_ims_ref(
    BinderExtIms* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return self;
}

void
binder_ext_ims_unref(
    BinderExtIms* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

BINDER_EXT_IMS_INTERFACE_FLAGS
binder_ext_ims_get_interface_flags(
    BinderExtIms* self)
{
    return G_LIKELY(self) ? GET_IFACE(self)->flags :
        BINDER_EXT_IMS_INTERFACE_NO_FLAGS;
}

BINDER_EXT_IMS_STATE
binder_ext_ims_get_state(
    BinderExtIms* self)
{
    if (G_LIKELY(self)) {
        BinderExtImsInterface* iface = GET_IFACE(self);

        if (iface->get_state) {
            return iface->get_state(self);
        }
    }
    return BINDER_EXT_IMS_STATE_UNKNOWN;
}

guint
binder_ext_ims_set_registration(
    BinderExtIms* self,
    BINDER_EXT_IMS_REGISTRATION registration,
    BinderExtImsResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtImsInterface* iface = GET_IFACE(self);

        if (iface->set_registration) {
            return iface->set_registration(self, registration, complete,
                destroy, user_data);
        }
    }
    return 0;
}

void
binder_ext_ims_cancel(
    BinderExtIms* self,
    guint id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        BinderExtImsInterface* iface = GET_IFACE(self);

        if (iface->cancel) {
            iface->cancel(self, id);
        }
    }
}

gulong
binder_ext_ims_add_state_handler(
    BinderExtIms* self,
    BinderExtImsFunc handler,
    void* user_data)
{
    if (G_LIKELY(self) && G_LIKELY(handler)) {
        BinderExtImsInterface* iface = GET_IFACE(self);

        if (iface->add_state_handler) {
            return iface->add_state_handler(self, handler, user_data);
        }
    }
    return 0;
}

void
binder_ext_ims_remove_handler(
    BinderExtIms* self,
    gulong id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        /*
         * Since we provide the default callback, we can safely assume
         * that remove_handler is always there.
         */
        GET_IFACE(self)->remove_handler(self, id);
    }
}

void
binder_ext_ims_remove_handlers(
    BinderExtIms* self,
    gulong* ids,
    guint count)
{
    if (G_LIKELY(self) && G_LIKELY(ids) && G_LIKELY(count)) {
        BinderExtImsInterface* iface = GET_IFACE(self);
        int i;

        /*
         * Since we provide the default callback, we can safely assume
         * that remove_handler is always there.
         */
        for (i = 0; i < count; i++) {
            if (ids[i]) {
                iface->remove_handler(self, ids[i]);
                ids[i] = 0;
            }
        }
    }
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_ext_ims_default_remove_handler(
    BinderExtIms* self,
    gulong id)
{
    if (id) {
        g_signal_handler_disconnect(self, id);
    }
}

static
void
binder_ext_ims_default_init(
    BinderExtImsInterface* iface)
{
    /*
     * Assume the smallest interface version. Implementation must overwrite
     * iface->version with BINDER_EXT_IMS_INTERFACE_VERSION known to it at
     * the compile time.
     */
    iface->version = 1;
    iface->remove_handler = binder_ext_ims_default_remove_handler;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
