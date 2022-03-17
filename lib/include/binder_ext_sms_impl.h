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

#ifndef BINDER_EXT_SMS_IMPL_H
#define BINDER_EXT_SMS_IMPL_H

#include "binder_ext_sms.h"

/* Internal API for use by BinderExtSms implemenations */

G_BEGIN_DECLS

#define BINDER_EXT_SMS_INTERFACE_VERSION 1

/*
 * Implementation sets field to BINDER_EXT_SMS_INTERFACE_VERSION.
 *
 * It allows more callbacks to be added to BinderExtSmsInterface in
 * the future. If the vtable gets extended, the version field will be
 * used to figure out the size of the (possibly smaller) vtable which
 * the implementation was compiled against.
 *
 * But let's hope that we will never run out of the reserved fields.
 */

typedef struct binder_ext_sms_interface {
    GTypeInterface parent;
    BINDER_EXT_SMS_INTERFACE_FLAGS flags;
    int version;

    guint (*send)(BinderExtSms* ext, const char* smsc, const void* pdu,
        gsize pdu_len, guint msg_ref, BINDER_EXT_SMS_SEND_FLAGS flags,
        BinderExtSmsSendFunc complete, GDestroyNotify destroy,
        void* user_data);
    void (*cancel)(BinderExtSms* ext, guint id);
    void (*ack_report)(BinderExtSms* ext, guint msg_ref, gboolean ok);
    void (*ack_incoming)(BinderExtSms* ext, gboolean ok);
    gulong (*add_report_handler)(BinderExtSms* ext,
        BinderExtSmsReportFunc handler, void* user_data);
    gulong (*add_incoming_handler)(BinderExtSms* ext,
        BinderExtSmsIncomingFunc handler, void* user_data);
    void (*remove_handler)(BinderExtSms* ext, gulong id);

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
} BinderExtSmsInterface;

#define BINDER_EXT_SMS_GET_IFACE(obj) G_TYPE_INSTANCE_GET_INTERFACE(obj, \
        BINDER_EXT_TYPE_SMS, BinderExtSmsInterface)

G_END_DECLS

#endif /* BINDER_EXT_SMS_IMPL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
