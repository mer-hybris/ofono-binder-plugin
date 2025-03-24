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

#include "binder_ext_call_impl.h"

G_DEFINE_INTERFACE(BinderExtCall, binder_ext_call, G_TYPE_OBJECT)
#define GET_IFACE(obj) BINDER_EXT_CALL_GET_IFACE(obj)

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtCall*
binder_ext_call_ref(
    BinderExtCall* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return self;
}

void
binder_ext_call_unref(
    BinderExtCall* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

BINDER_EXT_CALL_INTERFACE_FLAGS
binder_ext_call_get_interface_flags(
    BinderExtCall* self)
{
    return G_LIKELY(self) ? GET_IFACE(self)->flags :
        BINDER_EXT_CALL_INTERFACE_NO_FLAGS;
}

const BinderExtCallInfo* const*
binder_ext_call_get_calls(
    BinderExtCall* self)
{
    static const BinderExtCallInfo* none = NULL;

    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->get_calls) {
            const BinderExtCallInfo* const* calls = iface->get_calls(self);

            if (calls) {
                return calls;
            }
        }
    }

    /* This function never returns NULL */
    return &none;
}

guint
binder_ext_call_dial(
    BinderExtCall* self,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_CLIR clir,
    BINDER_EXT_CALL_DIAL_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self) && G_LIKELY(number)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->dial) {
            return iface->dial(self, number, toa, clir, flags, complete,
                destroy, user_data);
        }
    }
    return 0;
}

guint
binder_ext_call_deflect(
    BinderExtCall* self,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_DEFLECT_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self) && G_LIKELY(number)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->deflect) {
            return iface->deflect(self, number, toa, flags, complete,
                destroy, user_data);
        }
    }
    return 0;
}

guint
binder_ext_call_answer(
    BinderExtCall* self,
    BINDER_EXT_CALL_ANSWER_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->answer) {
            return iface->answer(self, flags, complete, destroy, data);
        }
    }
    return 0;
}

guint
binder_ext_call_swap(
    BinderExtCall* self,
    BINDER_EXT_CALL_SWAP_FLAGS swap_flags,
    BINDER_EXT_CALL_ANSWER_FLAGS answer_flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->swap) {
            return iface->swap(self, swap_flags, answer_flags, complete,
                destroy, data);
        }
    }
    return 0;
}

guint
binder_ext_call_transfer(
    BinderExtCall* self,
    BINDER_EXT_CALL_TRANSFER_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->transfer) {
            return iface->transfer(self, flags, complete, destroy, user_data);
        }
    }
    return 0;
}

guint
binder_ext_call_conference(
    BinderExtCall* self,
    BINDER_EXT_CALL_CONFERENCE_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->conference) {
            return iface->conference(self, flags, complete, destroy, user_data);
        }
    }
    return 0;
}

guint
binder_ext_call_send_dtmf(
    BinderExtCall* self,
    const char* tones,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self) && G_LIKELY(tones)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->send_dtmf) {
            return iface->send_dtmf(self, tones, complete, destroy, user_data);
        }
    }
    return 0;
}

guint
binder_ext_call_hangup(
    BinderExtCall* self,
    guint call_id,
    BINDER_EXT_CALL_HANGUP_REASON reason,
    BINDER_EXT_CALL_HANGUP_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->hangup) {
            return iface->hangup(self, call_id, reason, flags, complete,
                destroy, user_data);
        }
    }
    return 0;
}

void
binder_ext_call_cancel(
    BinderExtCall* self,
    guint id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->cancel) {
            iface->cancel(self, id);
        }
    }
}

gulong
binder_ext_call_add_calls_changed_handler(
    BinderExtCall* self,
    BinderExtCallFunc handler,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->add_calls_changed_handler) {
            iface->add_calls_changed_handler(self, handler, user_data);
        }
    }
    return 0;
}

gulong
binder_ext_call_add_disconnect_handler(
    BinderExtCall* self,
    BinderExtCallDisconnectFunc handler,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->add_disconnect_handler) {
            iface->add_disconnect_handler(self, handler, user_data);
        }
    }
    return 0;
}

gulong
binder_ext_call_add_ring_handler(
    BinderExtCall* self,
    BinderExtCallFunc handler,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->add_ring_handler) {
            iface->add_ring_handler(self, handler, user_data);
        }
    }
    return 0;
}

gulong
binder_ext_call_add_ssn_handler(
    BinderExtCall* self,
    BinderExtCallSuppSvcNotifyFunc handler,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->add_ssn_handler) {
            iface->add_ssn_handler(self, handler, user_data);
        }
    }
    return 0;
}

void
binder_ext_call_remove_handler(
    BinderExtCall* self,
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

gulong
binder_ext_call_add_ringback_tone_handler(
    BinderExtCall* self,
    BinderExtCallRingbackToneFunc handler,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtCallInterface* iface = GET_IFACE(self);

        if (iface->add_ringback_tone_handler) {
            iface->add_ringback_tone_handler(self, handler, user_data);
        }
    }
    return 0;
}

void
binder_ext_call_remove_handlers(
    BinderExtCall* self,
    gulong* ids,
    guint count)
{
    if (G_LIKELY(self) && G_LIKELY(ids) && G_LIKELY(count)) {
        BinderExtCallInterface* iface = GET_IFACE(self);
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
binder_ext_call_default_remove_handler(
    BinderExtCall* self,
    gulong id)
{
    if (id) {
        g_signal_handler_disconnect(self, id);
    }
}

static
void
binder_ext_call_default_init(
    BinderExtCallInterface* iface)
{
    /*
     * Assume the smallest interface version. Implementation must overwrite
     * iface->version with BINDER_EXT_CALL_INTERFACE_VERSION known to it at
     * the compile time.
     */
    iface->version = 1;
    iface->remove_handler = binder_ext_call_default_remove_handler;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
