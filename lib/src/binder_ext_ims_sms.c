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

#include "binder_ext_ims_sms_impl.h"

G_DEFINE_INTERFACE(BinderExtImsSms, binder_ext_ims_sms, G_TYPE_OBJECT)

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtImsSms*
binder_ext_ims_sms_ref(
    BinderExtImsSms* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return self;
}

void
binder_ext_ims_sms_unref(
    BinderExtImsSms* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

guint
binder_ext_ims_sms_send(
    BinderExtImsSms* self,
    const char* smsc,
    const void* pdu,
    gsize pdu_len,
    guint msg_ref,
    gboolean retry,
    BinderExtImsSmsSendFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    if (G_LIKELY(self)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);

        if (iface->send) {
            return iface->send(self, smsc, pdu, pdu_len, msg_ref, retry,
                complete, destroy, user_data);
        }
    }
    return 0;
}

void
binder_ext_ims_sms_cancel_send(
    BinderExtImsSms* self,
    guint id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);

        if (iface->cancel_send) {
            iface->cancel_send(self, id);
        }
    }
}

gulong
binder_ext_ims_sms_add_report_handler(
    BinderExtImsSms* self,
    BinderExtImsSmsReportFunc cb,
    void* user_data)
{
    if (G_LIKELY(self) && G_LIKELY(cb)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);

        if (iface->add_report_handler) {
            return iface->add_report_handler(self, cb, user_data);
        }
    }
    return 0;
}

gulong
binder_ext_ims_sms_add_incoming_handler(
    BinderExtImsSms* self,
    BinderExtImsSmsIncomingFunc cb,
    void* user_data)
{
    if (G_LIKELY(self) && G_LIKELY(cb)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);

        if (iface->add_incoming_handler) {
            return iface->add_incoming_handler(self, cb, user_data);
        }
    }
    return 0;
}

void
binder_ext_ims_sms_ack_report(
    BinderExtImsSms* self,
    guint msg_ref,
    gboolean ok)
{
    if (G_LIKELY(self)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);

        if (iface->ack_report) {
            iface->ack_report(self, msg_ref, ok);
        }
    }
}

void
binder_ext_ims_sms_ack_incoming(
    BinderExtImsSms* self,
    gboolean ok)
{
    if (G_LIKELY(self)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);

        if (iface->ack_incoming) {
            iface->ack_incoming(self, ok);
        }
    }
}

void
binder_ext_ims_sms_remove_handler(
    BinderExtImsSms* self,
    gulong id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        /*
         * Since we provide the default callback, we can safely assume
         * that remove_handler is always there.
         */
        BINDER_EXT_IMS_SMS_GET_IFACE(self)->remove_handler(self, id);
    }
}

void
binder_ext_ims_sms_remove_handlers(
    BinderExtImsSms* self,
    gulong* ids,
    guint count)
{
    if (G_LIKELY(self) && G_LIKELY(ids) && G_LIKELY(count)) {
        BinderExtImsSmsInterface* iface = BINDER_EXT_IMS_SMS_GET_IFACE(self);
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
binder_ext_ims_sms_default_remove_handler(
    BinderExtImsSms* self,
    gulong id)
{
    if (id) {
        g_signal_handler_disconnect(self, id);
    }
}

static
void
binder_ext_ims_sms_default_init(
    BinderExtImsSmsInterface* iface)
{
    /*
     * Assume the smallest interface version. Implementation must overwrite
     * iface->version with BINDER_EXT_IMS_SMS_INTERFACE_VERSION known to it
     * at the compile time.
     */
    iface->version = 1;
    iface->remove_handler = binder_ext_ims_sms_default_remove_handler;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
