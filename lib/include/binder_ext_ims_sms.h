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

#ifndef BINDER_EXT_IMS_SMS_H
#define BINDER_EXT_IMS_SMS_H

#include "binder_ext_types.h"

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum binder_ext_ims_sms_send_result {
    BINDER_EXT_IMS_SMS_SEND_OK = 0,
    BINDER_EXT_IMS_SMS_SEND_RETRY,
    BINDER_EXT_IMS_SMS_SEND_ERROR,
    BINDER_EXT_IMS_SMS_SEND_ERROR_RADIO_OFF,
    BINDER_EXT_IMS_SMS_SEND_ERROR_NO_SERVICE
} BINDER_EXT_IMS_SMS_SEND_RESULT;

typedef
void
(*BinderExtImsSmsSendFunc)(
    BinderExtImsSms* sms,
    BINDER_EXT_IMS_SMS_SEND_RESULT result,
    guint msg_ref,
    void* user_data);

typedef
void
(*BinderExtImsSmsReportFunc)(
    BinderExtImsSms* sms,
    const void* pdu,
    guint pdu_len,
    guint msg_ref,
    void* user_data);

typedef
void
(*BinderExtImsSmsIncomingFunc)(
    BinderExtImsSms* sms,
    const void* pdu,
    guint pdu_len,
    void* user_data);

GType binder_ext_ims_sms_get_type(void);
#define BINDER_EXT_TYPE_IMS_SMS (binder_ext_ims_sms_get_type())
#define BINDER_EXT_IMS_SMS(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, \
        BINDER_EXT_TYPE_IMS_SMS, BinderExtImsSms))

BinderExtImsSms*
binder_ext_ims_sms_ref(
    BinderExtImsSms* sms);

void
binder_ext_ims_sms_unref(
    BinderExtImsSms* sms);

guint
binder_ext_ims_sms_send(
    BinderExtImsSms* sms,
    const char* smsc,
    const void* pdu,
    gsize pdu_len,
    guint msg_ref,
    gboolean retry,
    BinderExtImsSmsSendFunc complete,
    GDestroyNotify destroy,
    void* user_data);

void
binder_ext_ims_sms_cancel_send(
    BinderExtImsSms* sms,
    guint id);

gulong
binder_ext_ims_sms_add_report_handler(
    BinderExtImsSms* sms,
    BinderExtImsSmsReportFunc handler,
    void* user_data);

gulong
binder_ext_ims_sms_add_incoming_handler(
    BinderExtImsSms* sms,
    BinderExtImsSmsIncomingFunc handler,
    void* user_data);

void
binder_ext_ims_sms_ack_report(
    BinderExtImsSms* sms,
    guint msg_ref,
    gboolean ok);

void
binder_ext_ims_sms_ack_incoming(
    BinderExtImsSms* sms,
    gboolean ok);

void
binder_ext_ims_sms_remove_handler(
    BinderExtImsSms* sms,
    gulong id);

void
binder_ext_ims_sms_remove_handlers(
    BinderExtImsSms* sms,
    gulong* ids,
    guint count);

#define binder_ext_ims_sms_remove_all_handlers(sms, ids) \
    binder_ext_ims_sms_remove_handlers(sms, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* BINDER_EXT_IMS_SMS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
