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

#ifndef BINDER_EXT_CALL_IMPL_H
#define BINDER_EXT_CALL_IMPL_H

#include "binder_ext_call.h"

/* Internal API for use by BinderExtCall implemenations */

G_BEGIN_DECLS

#define BINDER_EXT_CALL_INTERFACE_VERSION 1

/*
 * Implementation sets field to BINDER_EXT_CALL_INTERFACE_VERSION.
 *
 * It allows more callbacks to be added to BinderExtCallInterface
 * in the future. If the vtable gets extended, the version field will be
 * used to figure out the size of the (possibly smaller) vtable which
 * the implementation was compiled against.
 *
 * But let's hope that we will never run out of the reserved fields.
 */
typedef struct binder_ext_call_interface {
    GTypeInterface parent;
    BINDER_EXT_CALL_INTERFACE_FLAGS flags;
    int version;

    const BinderExtCallInfo* const* (*get_calls)(BinderExtCall* ext);
    guint (*dial)(BinderExtCall* ext, const char* number, BINDER_EXT_TOA toa,
        BINDER_EXT_CALL_CLIR clir, BINDER_EXT_CALL_DIAL_FLAGS flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*deflect)(BinderExtCall* ext, const char* number, BINDER_EXT_TOA toa,
        BINDER_EXT_CALL_DEFLECT_FLAGS flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*answer)(BinderExtCall* ext, BINDER_EXT_CALL_ANSWER_FLAGS flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*swap)(BinderExtCall* ext, BINDER_EXT_CALL_SWAP_FLAGS swap_flags,
        BINDER_EXT_CALL_ANSWER_FLAGS answer_flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*transfer)(BinderExtCall* ext, BINDER_EXT_CALL_TRANSFER_FLAGS flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*conference)(BinderExtCall* ext,
        BINDER_EXT_CALL_CONFERENCE_FLAGS flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*send_dtmf)(BinderExtCall* ext, const char* tones,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    guint (*hangup)(BinderExtCall* ext, guint call_id,
        BINDER_EXT_CALL_HANGUP_REASON reason,
        BINDER_EXT_CALL_HANGUP_FLAGS flags,
        BinderExtCallResultFunc complete, GDestroyNotify destroy,
        void* user_data);
    void (*cancel)(BinderExtCall* ext, guint id);
    gulong (*add_calls_changed_handler)(BinderExtCall* ext,
        BinderExtCallFunc handler, void* user_data);
    gulong (*add_disconnect_handler)(BinderExtCall* ext,
        BinderExtCallDisconnectFunc handler, void* user_data);
    gulong (*add_ring_handler)(BinderExtCall* ext,
        BinderExtCallFunc handler, void* user_data);
    gulong (*add_ssn_handler)(BinderExtCall* ext,
        BinderExtCallSuppSvcNotifyFunc handler, void* user_data);
    void (*remove_handler)(BinderExtCall* ext, gulong id);
    gulong (*add_ringback_tone_handler)(BinderExtCall* ext,
        BinderExtCallRingbackToneFunc handler, void* user_data);

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
} BinderExtCallInterface;

#define BINDER_EXT_CALL_GET_IFACE(obj) G_TYPE_INSTANCE_GET_INTERFACE(obj,\
    BINDER_EXT_TYPE_CALL, BinderExtCallInterface)

G_END_DECLS

#endif /* BINDER_EXT_CALL_IMPL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
