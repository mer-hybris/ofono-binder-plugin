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

#ifndef BINDER_IMS_REG_H
#define BINDER_IMS_REG_H

#include "binder_types.h"
#include "binder_ext_types.h"

/* Object tracking IMS registration state */

typedef enum binder_ims_reg_property {
    BINDER_IMS_REG_PROPERTY_ANY,
    BINDER_IMS_REG_PROPERTY_REGISTERED,
    BINDER_IMS_REG_PROPERTY_COUNT
} BINDER_IMS_REG_PROPERTY;

struct binder_ims_reg {
    gboolean registered;
    int caps; /* OFONO_IMS_xxx bits */
};

typedef
void
(*BinderImsRegPropertyFunc)(
    BinderImsReg* ims,
    BINDER_IMS_REG_PROPERTY property,
    void* user_data);

BinderImsReg*
binder_ims_reg_new(
    RadioClient* client,
    BinderExtSlot* ext_slot,
    const char* log_prefix);

BinderImsReg*
binder_ims_reg_ref(
    BinderImsReg* ims)
    BINDER_INTERNAL;

void
binder_ims_reg_unref(
    BinderImsReg* ims)
    BINDER_INTERNAL;

gulong
binder_ims_reg_add_property_handler(
    BinderImsReg* ims,
    BINDER_IMS_REG_PROPERTY property,
    BinderImsRegPropertyFunc callback,
    void* user_data)
    BINDER_INTERNAL;

void
binder_ims_reg_remove_handler(
    BinderImsReg* ims,
    gulong id)
    BINDER_INTERNAL;

void
binder_ims_reg_remove_handlers(
    BinderImsReg* ims,
    gulong* ids,
    int count)
    BINDER_INTERNAL;

#define binder_ims_reg_remove_all_handlers(ims, ids) \
    binder_ims_reg_remove_handlers(ims, ids, G_N_ELEMENTS(ids))

#endif /* BINDER_IMS_REG_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
