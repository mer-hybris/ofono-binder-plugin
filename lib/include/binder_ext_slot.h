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

#ifndef BINDER_EXT_SLOT_H
#define BINDER_EXT_SLOT_H

#include "binder_ext_types.h"

#include <radio_types.h>

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct binder_ext_slot_priv BinderExtSlotPriv;

struct binder_ext_slot {
    GObject object;
    BinderExtSlotPriv* priv;
};

GType binder_ext_slot_get_type(void);
#define BINDER_EXT_TYPE_SLOT (binder_ext_slot_get_type())
#define BINDER_EXT_SLOT(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, \
        BINDER_EXT_TYPE_SLOT, BinderExtSlot))

BinderExtSlot*
binder_ext_slot_new(
    BinderExtPlugin* plugin,
    RadioInstance* radio,
    GHashTable* params);

BinderExtSlot*
binder_ext_slot_ref(
    BinderExtSlot* slot);

void
binder_ext_slot_unref(
    BinderExtSlot* slot);

const char*
binder_ext_slot_plugin_name(
    BinderExtSlot* slot);

void
binder_ext_slot_drop(
    BinderExtSlot* slot);

gpointer
binder_ext_slot_get_interface(
    BinderExtSlot* slot,
    GType type);

G_END_DECLS

#endif /* BINDER_EXT_SLOT_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
