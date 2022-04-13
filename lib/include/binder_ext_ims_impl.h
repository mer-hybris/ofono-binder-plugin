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

#ifndef BINDER_EXT_IMS_IMPL_H
#define BINDER_EXT_IMS_IMPL_H

#include "binder_ext_ims.h"

/* Internal API for use by BinderExtIms implemenations */

G_BEGIN_DECLS

#define BINDER_EXT_IMS_INTERFACE_VERSION 1

/*
 * Implementation sets field to BINDER_EXT_IMS_INTERFACE_VERSION.
 *
 * It allows more callbacks to be added to BinderExtImsInterface in
 * the future. If the vtable gets extended, the version field will be
 * used to figure out the size of the (possibly smaller) vtable which
 * the implementation was compiled against.
 *
 * But let's hope that we will never run out of the reserved fields.
 */

typedef struct binder_ext_ims_interface {
    GTypeInterface parent;
    BINDER_EXT_IMS_INTERFACE_FLAGS flags;
    int version;

    BINDER_EXT_IMS_STATE (*get_state)(BinderExtIms* ext);
    guint (*set_registration)(BinderExtIms* ext,
        BINDER_EXT_IMS_REGISTRATION registration,
        BinderExtImsResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    void (*cancel)(BinderExtIms* ext, guint id);
    gulong (*add_state_handler)(BinderExtIms* ext,
        BinderExtImsFunc handler, void* user_data);
    void (*remove_handler)(BinderExtIms* ext, gulong id);

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
} BinderExtImsInterface;

#define BINDER_EXT_IMS_GET_IFACE(obj) G_TYPE_INSTANCE_GET_INTERFACE(obj, \
        BINDER_EXT_TYPE_IMS, BinderExtImsInterface)

G_END_DECLS

#endif /* BINDER_EXT_IMS_IMPL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
