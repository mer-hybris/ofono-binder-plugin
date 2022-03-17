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

#ifndef BINDER_EXT_SMS_H
#define BINDER_EXT_SMS_H

#include "binder_ext_types.h"

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum binder_ext_sms_interface_flags {
    BINDER_EXT_SMS_INTERFACE_NO_FLAGS = 0,
    BINDER_EXT_SMS_INTERFACE_FLAG_IMS = 0x01
} BINDER_EXT_SMS_INTERFACE_FLAGS;

typedef enum binder_ext_sms_send_flags {
    BINDER_EXT_SMS_SEND_NO_FLAGS = 0,
    BINDER_EXT_SMS_SEND_EXPECT_MORE = 0x01,
    BINDER_EXT_SMS_SEND_RETRY = 0x02
} BINDER_EXT_SMS_SEND_FLAGS;

typedef enum binder_ext_sms_send_result {
    BINDER_EXT_SMS_SEND_RESULT_OK = 0,
    BINDER_EXT_SMS_SEND_RESULT_RETRY,
    BINDER_EXT_SMS_SEND_RESULT_ERROR,
    BINDER_EXT_SMS_SEND_RESULT_ERROR_RADIO_OFF,
    BINDER_EXT_SMS_SEND_RESULT_ERROR_NO_SERVICE,
    BINDER_EXT_SMS_SEND_RESULT_ERROR_NETWORK_TIMEOUT
} BINDER_EXT_SMS_SEND_RESULT;

typedef
void
(*BinderExtSmsSendFunc)(
    BinderExtSms* ext,
    BINDER_EXT_SMS_SEND_RESULT result,
    guint msg_ref,
    void* user_data);

typedef
void
(*BinderExtSmsReportFunc)(
    BinderExtSms* ext,
    const void* pdu,
    guint pdu_len,
    guint msg_ref,
    void* user_data);

typedef
void
(*BinderExtSmsIncomingFunc)(
    BinderExtSms* ext,
    const void* pdu,
    guint pdu_len,
    void* user_data);

GType binder_ext_sms_get_type(void);
#define BINDER_EXT_TYPE_SMS (binder_ext_sms_get_type())
#define BINDER_EXT_SMS(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, \
        BINDER_EXT_TYPE_SMS, BinderExtSms))

BinderExtSms*
binder_ext_sms_ref(
    BinderExtSms* ext);

void
binder_ext_sms_unref(
    BinderExtSms* ext);

BINDER_EXT_SMS_INTERFACE_FLAGS
binder_ext_sms_get_interface_flags(
    BinderExtSms* self);

guint
binder_ext_sms_send(
    BinderExtSms* ext,
    const char* smsc,
    const void* pdu,
    gsize pdu_len,
    guint msg_ref,
    BINDER_EXT_SMS_SEND_FLAGS flags,
    BinderExtSmsSendFunc complete,
    GDestroyNotify destroy,
    void* user_data);

void
binder_ext_sms_cancel(
    BinderExtSms* ext,
    guint id);

gulong
binder_ext_sms_add_report_handler(
    BinderExtSms* ext,
    BinderExtSmsReportFunc handler,
    void* user_data);

gulong
binder_ext_sms_add_incoming_handler(
    BinderExtSms* ext,
    BinderExtSmsIncomingFunc handler,
    void* user_data);

void
binder_ext_sms_ack_report(
    BinderExtSms* ext,
    guint msg_ref,
    gboolean ok);

void
binder_ext_sms_ack_incoming(
    BinderExtSms* ext,
    gboolean ok);

void
binder_ext_sms_remove_handler(
    BinderExtSms* ext,
    gulong id);

void
binder_ext_sms_remove_handlers(
    BinderExtSms* ext,
    gulong* ids,
    guint count);

#define binder_ext_sms_remove_all_handlers(ext, ids) \
    binder_ext_sms_remove_handlers(ext, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* BINDER_EXT_SMS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
