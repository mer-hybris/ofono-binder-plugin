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

#ifndef BINDER_EXT_SLOT_IMPL_H
#define BINDER_EXT_SLOT_IMPL_H

#include "binder_ext_slot.h"

/* Internal API for use by BinderExtSlot implemenations */

G_BEGIN_DECLS

typedef struct binder_ext_slot_class {
    GObjectClass parent;

    gpointer (*get_interface)(BinderExtSlot* slot, GType iface);
    void (*shutdown)(BinderExtSlot* slot);

    /* Padding for future expansion */
    void (*_reserved1)(void);
    void (*_reserved2)(void);
    void (*_reserved3)(void);
    void (*_reserved4)(void);
    void (*_reserved5)(void);
    void (*_reserved6)(void);
    void (*_reserved7)(void);
    void (*_reserved8)(void);
    void (*_reserved9)(void);
    void (*_reserved10)(void);
} BinderExtSlotClass;

#define BINDER_EXT_SLOT_CLASS(klass) G_TYPE_CHECK_CLASS_CAST(klass, \
        BINDER_EXT_TYPE_SLOT, BinderExtSlotClass)

G_END_DECLS

#endif /* BINDER_EXT_SLOT_IMPL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
