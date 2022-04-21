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

#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include "binder_ext_slot_impl.h"

struct binder_ext_slot_priv {
    gboolean dropped;
};

#define THIS(obj) BINDER_EXT_SLOT(obj)
#define THIS_TYPE BINDER_EXT_TYPE_SLOT

G_DEFINE_ABSTRACT_TYPE(BinderExtSlot, binder_ext_slot, G_TYPE_OBJECT)

#define GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS(obj, THIS_TYPE, \
    BinderExtSlotClass)

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtSlot*
binder_ext_slot_ref(
    BinderExtSlot* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return self;
}

void
binder_ext_slot_unref(
    BinderExtSlot* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

void
binder_ext_slot_drop(
    BinderExtSlot* self)
{
    if (G_LIKELY(self)) {
        BinderExtSlotPriv* priv = self->priv;

        if (!priv->dropped) {
            priv->dropped = TRUE;
            GET_CLASS(self)->shutdown(self);
        }
        g_object_unref(self);
    }
}

gpointer
binder_ext_slot_get_interface(
    BinderExtSlot* self,
    GType type)
{
    return G_LIKELY(self) ? GET_CLASS(self)->get_interface(self, type) : NULL;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
gpointer
binder_ext_slot_default_get_interface(
    BinderExtSlot* self,
    GType iface)
{
    return NULL;
}

static
void
binder_ext_slot_nop(
    BinderExtSlot* slot)
{
}

static
void
binder_ext_slot_init(
    BinderExtSlot* self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE(self, THIS_TYPE,
        BinderExtSlotPriv);
}

static
void
binder_ext_slot_class_init(
    BinderExtSlotClass* klass)
{
    g_type_class_add_private(klass, sizeof(BinderExtSlotPriv));
    klass->get_interface = binder_ext_slot_default_get_interface;
    klass->shutdown = binder_ext_slot_nop;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
