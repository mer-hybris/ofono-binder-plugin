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

#ifndef BINDER_EXT_CALL_H
#define BINDER_EXT_CALL_H

#include "binder_ext_types.h"

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum binder_ext_call_interface_flags {
    BINDER_EXT_CALL_INTERFACE_NO_FLAGS = 0,
    BINDER_EXT_CALL_INTERFACE_FLAG_IMS_SUPPORT = 0x01,
    BINDER_EXT_CALL_INTERFACE_FLAG_IMS_REQUIRED = 0x02
} BINDER_EXT_CALL_INTERFACE_FLAGS;

typedef enum binder_ext_call_type {
    BINDER_EXT_CALL_TYPE_UNKNOWN,
    BINDER_EXT_CALL_TYPE_VOICE
} BINDER_EXT_CALL_TYPE;

typedef enum binder_ext_call_state {
    BINDER_EXT_CALL_STATE_INVALID = -1,
    BINDER_EXT_CALL_STATE_ACTIVE,
    BINDER_EXT_CALL_STATE_HOLDING,
    BINDER_EXT_CALL_STATE_DIALING,
    BINDER_EXT_CALL_STATE_ALERTING,
    BINDER_EXT_CALL_STATE_INCOMING,
    BINDER_EXT_CALL_STATE_WAITING
} BINDER_EXT_CALL_STATE;

typedef enum binder_ext_call_flags {
    BINDER_EXT_CALL_FLAGS_NONE = 0,
    BINDER_EXT_CALL_FLAG_INCOMING = 0x0001,
    BINDER_EXT_CALL_FLAG_EMERGENCY = 0x0002,
    BINDER_EXT_CALL_FLAG_ENCRYPTED = 0x0004,
    BINDER_EXT_CALL_FLAG_MPTY = 0x0008,
    BINDER_EXT_CALL_FLAG_RTT = 0x0010,
    BINDER_EXT_CALL_FLAG_IMS = 0x0020
} BINDER_EXT_CALL_FLAGS;

typedef enum binder_ext_call_direction {
    BINDER_EXT_CALL_DIRECTION_NONE,
    BINDER_EXT_CALL_DIRECTION_RECEIVE,
    BINDER_EXT_CALL_DIRECTION_SEND,
    BINDER_EXT_CALL_DIRECTION_SEND_RECEIVE
} BINDER_EXT_CALL_DIRECTION;

typedef enum binder_ext_call_clir {
    BINDER_EXT_CALL_CLIR_DEFAULT,
    BINDER_EXT_CALL_CLIR_INVOCATION, /* Restrict CLI presentation */
    BINDER_EXT_CALL_CLIR_SUPPRESSION /* Allow CLI presentation */
} BINDER_EXT_CALL_CLIR;

typedef enum binder_ext_call_hangup_reason {
    BINDER_EXT_CALL_HANGUP_TERMINATE,
    BINDER_EXT_CALL_HANGUP_IGNORE,
    BINDER_EXT_CALL_HANGUP_REJECT
} BINDER_EXT_CALL_HANGUP_REASON;

typedef enum binder_ext_call_disconnect_reason {
    BINDER_EXT_CALL_DISCONNECT_UNKNOWN,
    BINDER_EXT_CALL_DISCONNECT_LOCAL,
    BINDER_EXT_CALL_DISCONNECT_REMOTE,
    BINDER_EXT_CALL_DISCONNECT_ERROR
} BINDER_EXT_CALL_DISCONNECT_REASON;

typedef enum binder_ext_call_result {
    BINDER_EXT_CALL_RESULT_OK = 0,
    BINDER_EXT_CALL_RESULT_ERROR
} BINDER_EXT_CALL_RESULT;

typedef enum binder_ext_call_dial_flags {
    BINDER_EXT_CALL_DIAL_FLAGS_NONE = 0
} BINDER_EXT_CALL_DIAL_FLAGS;

typedef enum binder_ext_call_deflect_flags {
    BINDER_EXT_CALL_DEFLECT_FLAGS_NONE = 0
} BINDER_EXT_CALL_DEFLECT_FLAGS;

typedef enum binder_ext_call_answer_flags {
    BINDER_EXT_CALL_ANSWER_NO_FLAGS = 0,
    BINDER_EXT_CALL_ANSWER_FLAG_RTT = 0x01   /* Enable Real-Time Text */
} BINDER_EXT_CALL_ANSWER_FLAGS;

typedef enum binder_ext_call_transfer_flags {
    BINDER_EXT_CALL_TRANSFER_FLAGS_NONE = 0
} BINDER_EXT_CALL_TRANSFER_FLAGS;

typedef enum binder_ext_call_conference_flags {
    BINDER_EXT_CALL_CONFERENCE_FLAGS_NONE = 0
} BINDER_EXT_CALL_CONFERENCE_FLAGS;

typedef enum binder_ext_call_hangup_flags {
    BINDER_EXT_CALL_HANGUP_NO_FLAGS = 0
} BINDER_EXT_CALL_HANGUP_FLAGS;

typedef enum binder_ext_call_swap_flags {
    BINDER_EXT_CALL_SWAP_NO_FLAGS = 0,
    BINDER_EXT_CALL_SWAP_FLAG_HANGUP = 0x01  /* Hangup active call */
} BINDER_EXT_CALL_SWAP_FLAGS;

typedef struct binder_ext_call_supp_svc_notify {
    gboolean mt;
    int code;
    int index;
    BINDER_EXT_TOA type; /* MT only */
    const char* number;  /* MT only */
} BinderExtCallSuppSvcNotify;

typedef struct binder_ext_call_info {
    guint call_id;
    BINDER_EXT_CALL_TYPE type;
    BINDER_EXT_CALL_STATE state;
    BINDER_EXT_CALL_FLAGS flags;
    BINDER_EXT_TOA toa;
    const char* number;
    const char* name;
} BinderExtCallInfo;

typedef
void
(*BinderExtCallResultFunc)(
    BinderExtCall* ext,
    BINDER_EXT_CALL_RESULT result,
    void* user_data);

typedef
void
(*BinderExtCallFunc)(
    BinderExtCall* ext,
    void* user_data);

typedef
void
(*BinderExtCallSuppSvcNotifyFunc)(
    BinderExtCall* ext,
    const BinderExtCallSuppSvcNotify* ssn,
    void* user_data);

typedef
void
(*BinderExtCallDisconnectFunc)(
    BinderExtCall* ext,
    guint call_id,
    BINDER_EXT_CALL_DISCONNECT_REASON reason,
    void* user_data);

typedef
void
(*BinderExtCallRingbackToneFunc)(
    BinderExtCall* ext,
    gboolean start,
    void* user_data);

GType binder_ext_call_get_type(void);
#define BINDER_EXT_TYPE_CALL (binder_ext_call_get_type())
#define BINDER_EXT_CALL(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, \
        BINDER_EXT_TYPE_CALL, BinderExtCall))

BinderExtCall*
binder_ext_call_ref(
    BinderExtCall* ext);

void
binder_ext_call_unref(
    BinderExtCall* ext);

BINDER_EXT_CALL_INTERFACE_FLAGS
binder_ext_call_get_interface_flags(
    BinderExtCall* ext);

const BinderExtCallInfo* const*
binder_ext_call_get_calls(
    BinderExtCall* ext);

guint
binder_ext_call_dial(
    BinderExtCall* ext,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_CLIR clir,
    BINDER_EXT_CALL_DIAL_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_deflect(
    BinderExtCall* ext,
    const char* number,
    BINDER_EXT_TOA toa,
    BINDER_EXT_CALL_DEFLECT_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_answer(
    BinderExtCall* ext,
    BINDER_EXT_CALL_ANSWER_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_swap(
    BinderExtCall* ext,
    BINDER_EXT_CALL_SWAP_FLAGS swap_flags,
    BINDER_EXT_CALL_ANSWER_FLAGS answer_flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_transfer(
    BinderExtCall* ext,
    BINDER_EXT_CALL_TRANSFER_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_conference(
    BinderExtCall* ext,
    BINDER_EXT_CALL_CONFERENCE_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_send_dtmf(
    BinderExtCall* ext,
    const char* tones,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

guint
binder_ext_call_hangup(
    BinderExtCall* ext,
    guint call_id,
    BINDER_EXT_CALL_HANGUP_REASON reason,
    BINDER_EXT_CALL_HANGUP_FLAGS flags,
    BinderExtCallResultFunc complete,
    GDestroyNotify destroy,
    void* user_data);

void
binder_ext_call_cancel(
    BinderExtCall* ext,
    guint id);

gulong
binder_ext_call_add_calls_changed_handler(
    BinderExtCall* ext,
    BinderExtCallFunc handler,
    void* user_data);

gulong
binder_ext_call_add_disconnect_handler(
    BinderExtCall* ext,
    BinderExtCallDisconnectFunc handler,
    void* user_data);

gulong
binder_ext_call_add_ring_handler(
    BinderExtCall* ext,
    BinderExtCallFunc handler,
    void* user_data);

gulong
binder_ext_call_add_ssn_handler(
    BinderExtCall* ext,
    BinderExtCallSuppSvcNotifyFunc handler,
    void* user_data);

void
binder_ext_call_remove_handler(
    BinderExtCall* ext,
    gulong id);

gulong
binder_ext_call_add_ringback_tone_handler(
    BinderExtCall* ext,
    BinderExtCallRingbackToneFunc handler,
    void* user_data);

void
binder_ext_call_remove_handlers(
    BinderExtCall* ext,
    gulong* ids,
    guint count);

#define binder_ext_call_remove_all_handlers(ext, ids) \
    binder_ext_call_remove_handlers(ext, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* BINDER_EXT_CALL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
