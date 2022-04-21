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

#ifndef BINDER_EXT_IMS_H
#define BINDER_EXT_IMS_H

#include "binder_ext_types.h"

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum binder_ext_ims_interface_flags {
    BINDER_EXT_IMS_INTERFACE_NO_FLAGS = 0,
    BINDER_EXT_IMS_INTERFACE_FLAG_SMS_SUPPORT = 0x01,
    BINDER_EXT_IMS_INTERFACE_FLAG_VOICE_SUPPORT = 0x02
} BINDER_EXT_IMS_INTERFACE_FLAGS;

typedef enum binder_ext_ims_state {
    BINDER_EXT_IMS_STATE_UNKNOWN,
    BINDER_EXT_IMS_STATE_NOT_REGISTERED,
    BINDER_EXT_IMS_STATE_REGISTERING,
    BINDER_EXT_IMS_STATE_REGISTERED
} BINDER_EXT_IMS_STATE;

typedef enum binder_ext_ims_flags {
    BINDER_EXT_IMS_NO_FLAGS = 0,
    BINDER_EXT_IMS_FLAG_AUTO = 0x01
} BINDER_EXT_IMS_FLAGS;

typedef enum binder_ext_ims_registration {
    BINDER_EXT_IMS_REGISTRATION_OFF,
    BINDER_EXT_IMS_REGISTRATION_ON
} BINDER_EXT_IMS_REGISTRATION;

typedef enum binder_ext_ims_result {
    BINDER_EXT_IMS_RESULT_OK = 0,
    BINDER_EXT_IMS_RESULT_ERROR
} BINDER_EXT_IMS_RESULT;

typedef
void
(*BinderExtImsFunc)(
    BinderExtIms* ext,
    void* user_data);

typedef
void
(*BinderExtImsResultFunc)(
    BinderExtIms* ext,
    BINDER_EXT_IMS_RESULT result,
    void* user_data);

GType binder_ext_ims_get_type(void);
#define BINDER_EXT_TYPE_IMS (binder_ext_ims_get_type())
#define BINDER_EXT_IMS(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, \
        BINDER_EXT_TYPE_IMS, BinderExtIms))

BinderExtIms*
binder_ext_ims_ref(
    BinderExtIms* ext);

void
binder_ext_ims_unref(
    BinderExtIms* ext);

BINDER_EXT_IMS_INTERFACE_FLAGS
binder_ext_ims_get_interface_flags(
    BinderExtIms* ext);

BINDER_EXT_IMS_STATE
binder_ext_ims_get_state(
    BinderExtIms* ext);

guint
binder_ext_ims_set_registration(
    BinderExtIms* ext,
    BINDER_EXT_IMS_REGISTRATION registration,
    BinderExtImsResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

void
binder_ext_ims_cancel(
    BinderExtIms* ext,
    guint id);

gulong
binder_ext_ims_add_state_handler(
    BinderExtIms* ext,
    BinderExtImsFunc handler,
    void* user_data);

void
binder_ext_ims_remove_handler(
    BinderExtIms* ext,
    gulong id);

void
binder_ext_ims_remove_handlers(
    BinderExtIms* ext,
    gulong* ids,
    guint count);

#define binder_ext_ims_remove_all_handlers(ext, ids) \
    binder_ext_ims_remove_handlers(ext, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* BINDER_EXT_IMS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
