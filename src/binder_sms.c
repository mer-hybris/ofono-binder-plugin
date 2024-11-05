/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2021-2022 Jolla Ltd.
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

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_ims_reg.h"
#include "binder_sms.h"
#include "binder_util.h"

#include "binder_ext_slot.h"
#include "binder_ext_sms.h"

#include <ofono/ims.h>
#include <ofono/misc.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/watch.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_messaging_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

#define BINDER_SMS_ACK_RETRY_MS    1000
#define BINDER_SMS_ACK_RETRY_COUNT 10

#define SIM_EFSMS_FILEID        0x6F3C
#define EFSMS_LENGTH            176

static unsigned char sim_path[4] = {0x3F, 0x00, 0x7F, 0x10};

enum binder_sms_events {
    SMS_RADIO_EVENT_NEW_SMS,
    SMS_RADIO_EVENT_NEW_STATUS_REPORT,
    SMS_RADIO_EVENT_NEW_SMS_ON_SIM,
    SMS_RADIO_EVENT_COUNT
};

enum binder_sms_ext_events {
    SMS_EXT_EVENT_INCOMING_SMS,
    SMS_EXT_EVENT_STATUS_REPORT,
    SMS_EXT_EVENT_COUNT
};

/*
 * Note: use_standard_ims_sms_api is initialized to zero (FALSE) and stays
 * that way. It's a leftover from the experiments with IRadio.sendImsSms
 * API which doesn't seem to work (returns REQUEST_NOT_SUPPORTED), at
 * least on the device where I tried it. Besides, even if it worked,
 * that would only cover SMS over IMS, and VoLTE would still require
 * use of proprietary vendor specific API, there isn't really much
 * to fight for.
 *
 * In other words, the IRadio.sendImsSms stuff is likely to be deleted
 * at some point, don't pay too much attention to it.
 */
typedef struct binder_sms {
    struct ofono_sms* sms;
    struct ofono_watch* watch;
    struct ofono_sim_context* sim_context;
    char* log_prefix;
    gboolean use_standard_ims_sms_api;
    guint ext_send_id;
    BinderExtSms* sms_ext;
    BinderImsReg* ims_reg;
    RadioRequestGroup* g;
    RADIO_AIDL_INTERFACE interface_aidl;
    gulong ext_event[SMS_EXT_EVENT_COUNT];
    gulong radio_event[SMS_RADIO_EVENT_COUNT];
    guint register_id;
} BinderSms;

typedef struct binder_sms_cbd {
    BinderSms* self;
    union _ofono_sms_cb {
        ofono_sms_sca_set_cb_t sca_set;
        ofono_sms_sca_query_cb_t sca_query;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderSmsCbData;

typedef struct binder_sms_submit_cbd {
    BinderSms* self;
    void* pdu;
    int pdu_len;
    int tpdu_len;
    ofono_sms_submit_cb_t cb;
    gpointer data;
} BinderSmsSubmitCbData;

typedef struct binder_sms_sim_read_data {
    BinderSms* self;
    int record;
} BinderSmsSimReadData;

typedef enum binder_sms_send_flags {
    BINDER_SMS_SEND_FLAGS_NONE = 0,
    BINDER_SMS_SEND_FLAG_FORCE_GSM = 0x01,
    BINDER_SMS_SEND_FLAG_EXPECT_MORE = 0x02
} BINDER_SMS_SEND_FLAGS;

static
void
binder_sms_send(
    BinderSms* self,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len,
    BINDER_SMS_SEND_FLAGS flags,
    ofono_sms_submit_cb_t cb,
    void* data);

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)
#define SMS_TYPE_STR(ext) \
    ((binder_ext_sms_get_interface_flags(ext) & \
      BINDER_EXT_SMS_INTERFACE_FLAG_IMS_REQUIRED) ? "ims " : "")

static inline BinderSms* binder_sms_get_data(struct ofono_sms *sms)
    { return ofono_sms_get_data(sms); }

static
BinderSmsCbData*
binder_sms_cbd_new(
    BinderSms* self,
    BinderCallback cb,
    void* data)
{
    BinderSmsCbData* cbd = g_slice_new0(BinderSmsCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_sms_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderSmsCbData, cbd);
}

static
BinderSmsSubmitCbData*
binder_sms_submit_cbd_new(
    BinderSms* self,
    const void* pdu,
    int pdu_len,
    int tpdu_len,
    ofono_sms_submit_cb_t cb,
    void* data)
{
    BinderSmsSubmitCbData* cbd = g_slice_new0(BinderSmsSubmitCbData);

    cbd->self = self;
    cbd->pdu = gutil_memdup(pdu, pdu_len);
    cbd->pdu_len = pdu_len;
    cbd->tpdu_len = tpdu_len;
    cbd->cb = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_sms_submit_cbd_free(
    gpointer data)
{
    BinderSmsSubmitCbData* cbd = data;

    g_free(cbd->pdu);
    gutil_slice_free(cbd);
}

static
BinderSmsSimReadData*
binder_sms_sim_read_data_new(
    BinderSms* self,
    int rec)
{
    BinderSmsSimReadData* rd = g_slice_new0(BinderSmsSimReadData);

    rd->self = self;
    rd->record = rec;
    return rd;
}

static
void
binder_sms_sim_read_data_free(
    BinderSmsSimReadData* rd)
{
    gutil_slice_free(rd);
}

static
void
binder_sms_sca_set_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSmsCbData* cbd = user_data;
    ofono_sms_sca_set_cb_t cb = cbd->cb.sca_set;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = cbd->self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_RESP_SET_SMSC_ADDRESS :
            RADIO_RESP_SET_SMSC_ADDRESS;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else {
                ofono_warn("smsc setting error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected setSmscAddress response %d", resp);
        }
    }

    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_sms_sca_set(
    struct ofono_sms* sms,
    const struct ofono_phone_number* sca,
    ofono_sms_sca_set_cb_t cb,
    void* data)
{
    BinderSms* self = binder_sms_get_data(sms);
    char* tmp = NULL;
    GBinderWriter writer;
    const char* number = (sca->type == OFONO_NUMBER_TYPE_INTERNATIONAL) ?
        (tmp = g_strconcat("+", sca->number, NULL)) : sca->number;
    guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_REQ_SET_SMSC_ADDRESS :
        RADIO_REQ_SET_SMSC_ADDRESS;

    /* setSmscAddress(int32_t serial, string smsc); */
    RadioRequest* req = radio_request_new2(self->g,
        code, &writer, binder_sms_sca_set_cb,
        binder_sms_cbd_free, binder_sms_cbd_new(self, BINDER_CB(cb), data));

    DBG_(self, "setting sca: %s", number);
    if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        binder_append_hidl_string(&writer, number);
    } else {
        gbinder_writer_append_string16(&writer, number);
    }

    if (!radio_request_submit(req)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }

    radio_request_unref(req);
    g_free(tmp);
}

static
void
binder_sms_sca_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSmsCbData* cbd = user_data;
    BinderSms* self = cbd->self;
    ofono_sms_sca_query_cb_t cb = cbd->cb.sca_query;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_RESP_GET_SMSC_ADDRESS :
            RADIO_RESP_GET_SMSC_ADDRESS;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                char* smsc;

                gbinder_reader_copy(&reader, args);
                if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
                    smsc = gbinder_reader_read_hidl_string(&reader);
                } else {
                    smsc = gbinder_reader_read_string16(&reader);
                }
                if (smsc) {
                    struct ofono_phone_number sca;
                    const char* str = smsc;

                    if (str[0] == '+') {
                        str++;
                        sca.type = OFONO_NUMBER_TYPE_INTERNATIONAL;
                    } else {
                        sca.type = OFONO_NUMBER_TYPE_UNKNOWN;
                    }
                    g_strlcpy(sca.number, str, sizeof(sca.number));
                    DBG("csca_query_cb: %s, %d", sca.number, sca.type);
                    cb(binder_error_ok(&err), &sca, cbd->data);

                    g_free(smsc);
                    return;
                }
            } else {
                ofono_warn("smsc query error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getSmscAddress response %d", resp);
        }
    }

    /* Error path */
    cb(binder_error_failure(&err), NULL, cbd->data);
}

static
void
binder_sms_sca_query(
    struct ofono_sms* sms,
    ofono_sms_sca_query_cb_t cb,
    void* data)
{
    BinderSms* self = binder_sms_get_data(sms);
    guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_REQ_GET_SMSC_ADDRESS :
        RADIO_REQ_GET_SMSC_ADDRESS;
    RadioRequest* req = radio_request_new2(self->g,
        code, NULL, binder_sms_sca_query_cb,
        binder_sms_cbd_free, binder_sms_cbd_new(self, BINDER_CB(cb), data));

    DBG_(self, "sending csca_query");
    if (!radio_request_submit(req)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), NULL, data);
    }
    radio_request_unref(req);
}

static
gboolean
binder_sms_can_send_ims_message(
    BinderSms* self)
{
    return self->ims_reg && self->ims_reg->registered &&
        (self->ims_reg->caps & OFONO_IMS_SMS_CAPABLE);
}

static
gboolean
binder_sms_can_send_ext_message(
    BinderSms* self)
{
    return self->sms_ext && (!(binder_ext_sms_get_interface_flags
        (self->sms_ext) & BINDER_EXT_SMS_INTERFACE_FLAG_IMS_REQUIRED) ||
        binder_sms_can_send_ims_message(self));
}

static
void
binder_sms_submit_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSmsSubmitCbData* cbd = user_data;
    BinderSms* self = cbd->self;
    ofono_sms_submit_cb_t cb = cbd->cb;
    struct ofono_error err;

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        const gboolean ims = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            ((RADIO_MESSAGING_RESP)resp == RADIO_MESSAGING_RESP_SEND_IMS_SMS) :
            (resp == RADIO_RESP_SEND_IMS_SMS);

        /*
         * Luckily, all 3 responses that we're handling here have the
         * same parameter list:
         *
         * sendSmsResponse(RadioResponseInfo, SendSmsResult sms);
         * sendSMSExpectMoreResponse(RadioResponseInfo, SendSmsResult sms);
         * sendImsSmsResponse(RadioResponseInfo, SendSmsResult sms);
         */
        if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
            if (resp == RADIO_RESP_SEND_SMS ||
                resp == RADIO_RESP_SEND_SMS_EXPECT_MORE ||
                resp == RADIO_RESP_SEND_IMS_SMS) {
                if (error == RADIO_ERROR_NONE) {
                    const RadioSendSmsResult* res;
                    GBinderReader reader;

                    gbinder_reader_copy(&reader, args);
                    res = gbinder_reader_read_hidl_struct(&reader,
                        RadioSendSmsResult);

                    if (res) {
                        DBG("%ssms msg ref: %d, ack: %s err: %d", ims ? "ims " : "",
                            res->messageRef, res->ackPDU.data.str, res->errorCode);

                        /*
                         * Error is -1 if unknown or not applicable,
                         * otherwise 3GPP 27.005, 3.2.5
                         */
                        if (res->errorCode > 0) {
                            err.type = OFONO_ERROR_TYPE_CMS;
                            err.error = res->errorCode;
                        } else {
                            /* Success */
                            cb(binder_error_ok(&err), res->messageRef, cbd->data);
                            return;
                        }
                    }
                } else {
                    ofono_error("%ssms send error %s", ims ? "ims " : "",
                        binder_radio_error_string(error));
                }
                if (ims && cbd->pdu) {
                    /* Failed to send IMS SMS, try GSM */
                    binder_sms_send(cbd->self, cbd->pdu, cbd->pdu_len,
                        cbd->tpdu_len, BINDER_SMS_SEND_FLAG_FORCE_GSM,
                        cbd->cb, cbd->data);
                    return;
                }
            } else {
                ofono_error("Unexpected send sms response %d", resp);
            }
        } else {
            if ((RADIO_MESSAGING_RESP)resp == RADIO_MESSAGING_RESP_SEND_SMS ||
                (RADIO_MESSAGING_RESP)resp == RADIO_MESSAGING_RESP_SEND_SMS_EXPECT_MORE ||
                (RADIO_MESSAGING_RESP)resp == RADIO_MESSAGING_RESP_SEND_IMS_SMS) {
                if (error == RADIO_ERROR_NONE) {
                    GBinderReader reader;
                    gint32 message_ref;
                    char* ack_pdu = NULL;
                    gint32 error_code;

                    gbinder_reader_copy(&reader, args);

                    if (binder_read_parcelable_size(&reader)) {
                        gbinder_reader_read_int32(&reader, &message_ref);
                        ack_pdu = gbinder_reader_read_string16(&reader);
                        gbinder_reader_read_int32(&reader, &error_code);
                        DBG("%ssms msg ref: %d, ack: %s err: %d", ims ? "ims " : "",
                        message_ref, ack_pdu, error_code);
                        g_free(ack_pdu);

                        /*
                         * Error is -1 if unknown or not applicable,
                         * otherwise 3GPP 27.005, 3.2.5
                         */
                        if (error_code > 0) {
                            err.type = OFONO_ERROR_TYPE_CMS;
                            err.error = error_code;
                        } else {
                            /* Success */
                            cb(binder_error_ok(&err), message_ref, cbd->data);
                            return;
                        }
                    }
                    g_free(ack_pdu);
                } else {
                    ofono_error("%ssms send error %s", ims ? "ims " : "",
                        binder_radio_error_string(error));
                }
                if (ims && cbd->pdu) {
                    /* Failed to send IMS SMS, try GSM */
                    binder_sms_send(cbd->self, cbd->pdu, cbd->pdu_len,
                        cbd->tpdu_len, BINDER_SMS_SEND_FLAG_FORCE_GSM,
                        cbd->cb, cbd->data);
                    return;
                }
            }
        }
    }
    /* Error path */
    cb(&err, 0, cbd->data);
}

static
void
binder_sms_submit_ext_cb(
    BinderExtSms* ext,
    BINDER_EXT_SMS_SEND_RESULT result,
    guint msg_ref,
    void* user_data)
{
    BinderSmsSubmitCbData* cbd = user_data;
    BinderSms* self = cbd->self;
    struct ofono_error err;

    self->ext_send_id = 0;
    switch (result) {
    case BINDER_EXT_SMS_SEND_RESULT_OK:
        /* SMS has been sent */
        cbd->cb(binder_error_ok(&err), msg_ref, cbd->data);
        return;
    case BINDER_EXT_SMS_SEND_RESULT_RETRY:
    case BINDER_EXT_SMS_SEND_RESULT_ERROR_RADIO_OFF:
    case BINDER_EXT_SMS_SEND_RESULT_ERROR_NO_SERVICE:
        /* Failed to send SMS over IMS, try GSM */
        binder_sms_send(cbd->self, cbd->pdu, cbd->pdu_len,
            cbd->tpdu_len, BINDER_SMS_SEND_FLAG_FORCE_GSM,
            cbd->cb, cbd->data);
        return;
    case BINDER_EXT_SMS_SEND_RESULT_ERROR_NETWORK_TIMEOUT:
        /*
         * 332 (network timeout) defined in 3GPP 27.005, 3.2.5
         * is the only one actually handled by the ofono core.
         */
        err.type = OFONO_ERROR_TYPE_CMS;
        err.error = 332; /* network timeout */
        cbd->cb(&err, 0, cbd->data);
        return;
    case BINDER_EXT_SMS_SEND_RESULT_ERROR:
        break;
    }

    /* Error path */
    cbd->cb(binder_error_failure(&err), 0, cbd->data);
}

static
void
binder_sms_gsm_message(
    BinderSms* self,
    GBinderWriter* writer,
    RadioGsmSmsMessage* msg,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len,
    const GBinderParent* parent)
{
    /*
     * SMSC address:
     *
     * smsc_len == 1, then zero-length SMSC was specified but IRadio
     * interface expects an empty string for default SMSC.
     */
    char* tpdu;
    int smsc_len = pdu_len - tpdu_len;
    guint msg_index;

    if (smsc_len > 1) {
        binder_copy_hidl_string_len(writer, &msg->smscPdu, (const char*) pdu,
            smsc_len);
    } else {
        /* Default SMSC address */
        binder_copy_hidl_string(writer, &msg->smscPdu, NULL);
    }

    /* PDU is sent as an ASCII hex string */
    msg->pdu.len = tpdu_len * 2;
    tpdu = gbinder_writer_malloc(writer, msg->pdu.len + 1);
    ofono_encode_hex(pdu + smsc_len, tpdu_len, tpdu);
    msg->pdu.data.str = tpdu;
    DBG_(self, "%s", tpdu);

    /* Write GsmSmsMessage and its strings */
    msg_index = gbinder_writer_append_buffer_object_with_parent(writer,
        msg, sizeof(*msg), parent);
    binder_append_hidl_string_data(writer, msg, smscPdu, msg_index);
    binder_append_hidl_string_data(writer, msg, pdu, msg_index);
}

static
void
binder_sms_ims_message(
    BinderSms* self,
    GBinderWriter* writer,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len)
{
    RadioImsSmsMessage* ims = gbinder_writer_new0(writer, RadioImsSmsMessage);
    RadioGsmSmsMessage* gsm = gbinder_writer_new0(writer, RadioGsmSmsMessage);
    GBinderParent p;

    ims->tech = RADIO_TECH_FAMILY_3GPP2;
    ims->gsmMessage.count = 1;
    ims->gsmMessage.data.ptr = gsm;
    ims->gsmMessage.owns_buffer = TRUE;

    p.index = gbinder_writer_append_buffer_object(writer, ims, sizeof(*ims));
    p.offset = G_STRUCT_OFFSET(RadioImsSmsMessage, cdmaMessage.data.ptr);
    gbinder_writer_append_buffer_object_with_parent(writer, NULL, 0, &p);

    p.offset = G_STRUCT_OFFSET(RadioImsSmsMessage, gsmMessage.data.ptr);
    binder_sms_gsm_message(self, writer, gsm, pdu, pdu_len, tpdu_len, &p);
}

static
void
binder_sms_gsm_message_aidl(
    BinderSms* self,
    GBinderWriter* writer,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len)
{
    /*
     * SMSC address:
     *
     * smsc_len == 1, then zero-length SMSC was specified but IRadio
     * interface expects an empty string for default SMSC.
     */
    char* tpdu;
    tpdu = g_malloc0(sizeof(char) * (tpdu_len * 2));
    int smsc_len = pdu_len - tpdu_len;
    gint32 initial_size;

    /* PDU is sent as an ASCII hex string */
    ofono_encode_hex(pdu + smsc_len, tpdu_len, tpdu);
    DBG_(self, "%s", tpdu);

    /* Non-null parcelable */
    gbinder_writer_append_int32(writer, 1);
    initial_size = gbinder_writer_bytes_written(writer);
    /* Dummy parcelable size, replaced at the end */
    gbinder_writer_append_int32(writer, -1);

    gbinder_writer_append_string16_len(writer, (const char*) pdu, smsc_len);
    gbinder_writer_append_string16_len(writer, tpdu, tpdu_len * 2);

    /* Overwrite parcelable size */
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);

    g_free(tpdu);
}

static
void
binder_sms_ims_message_aidl(
    BinderSms* self,
    GBinderWriter* writer,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len)
{
    gint32 initial_size;
    /* Non-null parcelable */
    gbinder_writer_append_int32(writer, 1);
    initial_size = gbinder_writer_bytes_written(writer);
    /* Dummy parcelable size, replaced at the end */
    gbinder_writer_append_int32(writer, -1);

    gbinder_writer_append_int32(writer, RADIO_TECH_FAMILY_3GPP2);
    gbinder_writer_append_bool(writer, FALSE);
    gbinder_writer_append_int32(writer, 0);

    /* CDMA message count */
    gbinder_writer_append_int32(writer, 0);

    /* GSM message count */
    gbinder_writer_append_int32(writer, 1);

    /* Overwrite parcelable size */
    gbinder_writer_overwrite_int32(writer, initial_size,
        gbinder_writer_bytes_written(writer) - initial_size);

    binder_sms_gsm_message_aidl(self, writer, pdu, pdu_len, tpdu_len);
}

static
void
binder_sms_send(
    BinderSms* self,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len,
    BINDER_SMS_SEND_FLAGS flags,
    ofono_sms_submit_cb_t cb,
    void* data)
{
    BinderSmsSubmitCbData* cbd = NULL;
    struct ofono_error err;

    DBG("pdu_len: %d, tpdu_len: %d flags: 0x%02x", pdu_len, tpdu_len, flags);
    if (!(flags & BINDER_SMS_SEND_FLAG_FORCE_GSM) &&
        binder_sms_can_send_ext_message(self)) {
        const int smsc_len = pdu_len - tpdu_len;
        const void* tpdu = pdu + smsc_len;
        char* smsc = (smsc_len > 1) ? g_strndup((char*)pdu, smsc_len) : NULL;

        /* Vendor specific mechanism */
        binder_ext_sms_cancel(self->sms_ext, self->ext_send_id);
        /* Copy the PDU for GSM SMS fallback */
        cbd = binder_sms_submit_cbd_new(self, pdu,pdu_len,tpdu_len, cb, data);
        self->ext_send_id = binder_ext_sms_send(self->sms_ext, smsc,
            tpdu, tpdu_len, 0, (flags & BINDER_SMS_SEND_FLAG_EXPECT_MORE) ?
            BINDER_EXT_SMS_SEND_EXPECT_MORE : BINDER_EXT_SMS_SEND_NO_FLAGS,
            binder_sms_submit_ext_cb, binder_sms_submit_cbd_free, cbd);
        g_free(smsc);
        if (self->ext_send_id) {
            /* Request submitted */
            return;
        }
        /* cbd will be reused */
    }

    if ((flags & BINDER_SMS_SEND_FLAG_FORCE_GSM) ||
        !binder_sms_can_send_ims_message(self)) {
        /*
         * sendSms(serial, GsmSmsMessage message);
         * sendSMSExpectMore(serial, GsmSmsMessage message);
         */
        GBinderWriter writer;
        guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            ((flags & BINDER_SMS_SEND_FLAG_EXPECT_MORE) ?
                RADIO_MESSAGING_REQ_SEND_SMS_EXPECT_MORE :
                RADIO_MESSAGING_REQ_SEND_SMS) :
            ((flags & BINDER_SMS_SEND_FLAG_EXPECT_MORE) ?
                RADIO_REQ_SEND_SMS_EXPECT_MORE : RADIO_REQ_SEND_SMS);
        RadioRequest* req = radio_request_new2(self->g,
            code, &writer,
            binder_sms_submit_cb, binder_sms_submit_cbd_free,
            cbd ? cbd : /* No need to copy the PDU */
            binder_sms_submit_cbd_new(self, NULL, 0, 0, cb, data));

        if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
            binder_sms_gsm_message(self, &writer,
                gbinder_writer_new0(&writer, RadioGsmSmsMessage),
                pdu, pdu_len, tpdu_len, NULL);
        } else {
            binder_sms_gsm_message_aidl(self, &writer,
                pdu, pdu_len, tpdu_len);
        }
        if (radio_request_submit(req)) {
            radio_request_unref(req);
            /* Request submitted */
            return;
        }
    } else if (self->use_standard_ims_sms_api) {
        /* sendImsSms(serial, ImsSmsMessage message); */
        GBinderWriter writer;
        guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_REQ_SEND_IMS_SMS : RADIO_REQ_SEND_IMS_SMS;
        RadioRequest* req = radio_request_new2(self->g,
            code, &writer,
            binder_sms_submit_cb, binder_sms_submit_cbd_free,
            cbd ? cbd :  /* Copy the PDU for GSM SMS fallback */
            binder_sms_submit_cbd_new(self, pdu, pdu_len, tpdu_len, cb, data));

        DBG("sending ims message");

        if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
            binder_sms_ims_message(self, &writer, pdu, pdu_len, tpdu_len);
        } else {
            binder_sms_ims_message_aidl(self, &writer, pdu, pdu_len, tpdu_len);
        }
        if (radio_request_submit(req)) {
            radio_request_unref(req);
            /* Request submitted */
            return;
        }
    } else if (cbd) {
        /* cbd wasn't reused */
        binder_sms_submit_cbd_free(cbd);
    }

    /* Error path */
    cb(binder_error_failure(&err), 0, data);
}

static
void
binder_sms_submit(
    struct ofono_sms* sms,
    const unsigned char* pdu,
    int pdu_len,
    int tpdu_len,
    int expect_more,
    ofono_sms_submit_cb_t cb,
    void* data)
{
    binder_sms_send(binder_sms_get_data(sms), pdu, pdu_len, tpdu_len,
        expect_more ? BINDER_SMS_SEND_FLAG_EXPECT_MORE :
        BINDER_SMS_SEND_FLAGS_NONE, cb, data);
}

static
void
binder_sms_ack_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSms* self = user_data;
    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_RESP_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS :
            RADIO_RESP_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS;
        if (resp == code) {
            if (error != RADIO_ERROR_NONE) {
                ofono_error("SMS acknowledgement failed: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected acknowledgeLastIncomingGsmSms response %d",
                resp);
        }
    } else {
        ofono_error("SMS acknowledgement failed");
    }
}

static
void
binder_sms_ack(
    BinderSms* self,
    gboolean ok)
{
    GBinderWriter writer;
    guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_REQ_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS :
        RADIO_REQ_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS;

    /*
     * acknowledgeLastIncomingGsmSms(int32 serial, bool success,
     *     SmsAcknowledgeFailCause cause);
     */
    RadioRequest* req = radio_request_new2(self->g,
        code, &writer,
        binder_sms_ack_cb, NULL, self);

    DBG_(self, "%s", ok ? "ok" : "fail");
    gbinder_writer_append_bool(&writer, ok);
    gbinder_writer_append_int32(&writer, ok ? RADIO_SMS_ACK_FAIL_NONE :
        RADIO_SMS_ACK_FAIL_UNSPECIFIED_ERROR);

    radio_request_set_retry(req, BINDER_SMS_ACK_RETRY_MS,
        BINDER_SMS_ACK_RETRY_COUNT);
    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_sms_incoming(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSms* self = user_data;
    GBinderReader reader;
    const guint8* pdu;
    gsize len;

    /*
     * newSms(RadioIndicationType, vec<uint8_t> pdu);
     * newSmsStatusReport(RadioIndicationType, vec<uint8_t> pdu);
     */
    gbinder_reader_copy(&reader, args);
    if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        pdu = gbinder_reader_read_hidl_byte_vec(&reader, &len);
    } else {
        pdu = gbinder_reader_read_byte_array(&reader, &len);
    }
    if (pdu) {
        const guint pdu_len = (guint) len;
        const guint smsc_len = (guint) pdu[0] + 1;

        guint32 ind_code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_IND_NEW_SMS : RADIO_IND_NEW_SMS;

        ofono_info("%s, %u bytes",(code == ind_code) ?
            "incoming sms" : "sms status", pdu_len);
        if (pdu_len > smsc_len) {
            /* The PDU starts with the SMSC address per TS 27.005 (+CMT:) */
            const guint tpdu_len = pdu_len - smsc_len;

            DBG_(self, "smsc: %s", binder_print_hex(pdu, smsc_len));
            DBG_(self, "tpdu: %s", binder_print_hex(pdu + smsc_len, tpdu_len));

            if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
                switch (code) {
                case RADIO_IND_NEW_SMS:
                    ofono_sms_deliver_notify(self->sms, pdu, pdu_len, tpdu_len);
                    binder_sms_ack(self, TRUE);
                    break;
                case RADIO_IND_NEW_SMS_STATUS_REPORT:
                    ofono_sms_status_notify(self->sms, pdu, pdu_len, tpdu_len);
                    binder_sms_ack(self, TRUE);
                    break;
                default:
                    binder_sms_ack(self, FALSE);
                    break;
                }
            } else {
                switch (code) {
                case RADIO_MESSAGING_IND_NEW_SMS:
                    ofono_sms_deliver_notify(self->sms, pdu, pdu_len, tpdu_len);
                    binder_sms_ack(self, TRUE);
                    break;
                case RADIO_MESSAGING_IND_NEW_SMS_STATUS_REPORT:
                    ofono_sms_status_notify(self->sms, pdu, pdu_len, tpdu_len);
                    binder_sms_ack(self, TRUE);
                    break;
                default:
                    binder_sms_ack(self, FALSE);
                    break;
                }
            }
            return;
        }
    }
    ofono_error("Unable to parse SMS notification");
    binder_sms_ack(self, FALSE);
}

static
gboolean
binder_sms_notify(
    BinderSms* self,
    const guchar* pdu,
    guint pdu_len,
    void (*notify)(
        struct ofono_sms* sms,
        const unsigned char* pdu,
        int len,
        int tpdu_len))
{
    if (pdu_len > 0) {
        const guint smsc_len = (guint)pdu[0] + 1;

        if (pdu_len > smsc_len) {
            const guint tpdu_len = pdu_len - smsc_len;

            DBG_(self, "smsc: %s", binder_print_hex(pdu, smsc_len));
            DBG_(self, "tpdu: %s", binder_print_hex(pdu + smsc_len, tpdu_len));
            notify(self->sms, pdu, pdu_len, tpdu_len);
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_sms_ext_incoming(
    BinderExtSms* ext,
    const void* pdu,
    guint pdu_len,
    void* user_data)
{
    ofono_info("incoming %ssms, %u bytes", SMS_TYPE_STR(ext), pdu_len);
    if (binder_sms_notify((BinderSms*) user_data, pdu, pdu_len,
        ofono_sms_deliver_notify)) {
        binder_ext_sms_ack_incoming(ext, TRUE);
    } else {
        ofono_error("Unable to parse %sSMS notification", SMS_TYPE_STR(ext));
        binder_ext_sms_ack_incoming(ext, FALSE);
    }
}

static
void
binder_sms_ext_status_report(
    BinderExtSms* ext,
    const void* pdu,
    guint pdu_len,
    guint msg_ref,
    void* user_data)
{
    ofono_info("incoming %ssms report, %u bytes", SMS_TYPE_STR(ext), pdu_len);
    if (binder_sms_notify((BinderSms*) user_data, pdu, pdu_len,
        ofono_sms_status_notify)) {
        binder_ext_sms_ack_report(ext, msg_ref, TRUE);
    } else {
        ofono_error("Unable to parse %sSMS report", SMS_TYPE_STR(ext));
        binder_ext_sms_ack_report(ext, msg_ref, FALSE);
    }
}

static
void
binder_sms_delete_on_sim_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSms* self = user_data;
    if (status == RADIO_TX_STATUS_OK) {
        guint32 code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
            RADIO_MESSAGING_RESP_DELETE_SMS_ON_SIM :
            RADIO_RESP_DELETE_SMS_ON_SIM;
        if (resp == code) {
            if (error == RADIO_ERROR_NONE) {
                ofono_info("sms deleted from sim");
            } else {
                ofono_warn("Failed to delete sms from sim: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected deleteSmsOnSim response %d", resp);
        }
    } else {
        ofono_error("Deleting SMS from SIM failed");
    }
}

static
void
binder_sms_on_sim_cb(
    int ok,
    int total_length,
    int record,
    const unsigned char* sdata,
    int length,
    void* userdata)
{
    BinderSmsSimReadData* cbd = userdata;
    BinderSms* self = cbd->self;

    /*
     * EFsms contains status byte followed by SMS PDU (including
     * SMSC address) per TS 31.103 4.2.12
     */
    if (ok) {
        if (length > 1) {
            /* Skip status byte */
            guint pdu_len = length - 1;
            const guint8* pdu = sdata + 1;
            const guint smsc_len = (guint) pdu[0] + 1;

            if (pdu_len > smsc_len) {
                RadioRequest* req;
                GBinderWriter writer;
                const guint tpdu_len = pdu_len - smsc_len;
                guint32 code =
                    self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
                        RADIO_MESSAGING_REQ_DELETE_SMS_ON_SIM :
                        RADIO_REQ_DELETE_SMS_ON_SIM;

                ofono_info("read sms from sim, %u bytes", pdu_len);
                DBG_(self, "smsc: %s", binder_print_hex(pdu, smsc_len));
                DBG_(self, "tpdu: %s", binder_print_hex(pdu + smsc_len,
                    tpdu_len));

                ofono_sms_deliver_notify(self->sms, pdu, pdu_len, tpdu_len);

                /* deleteSmsOnSim(int32 serial, int32 index); */
                DBG_(self, "deleting record: %d", cbd->record);
                req = radio_request_new2(self->g,
                    code, &writer,
                    binder_sms_delete_on_sim_cb, NULL, self);
                gbinder_writer_append_int32(&writer, cbd->record);
                radio_request_submit(req);
                radio_request_unref(req);
            } else {
                ofono_warn("Failed to extract PDU from EFsms");
            }
        } else {
            ofono_warn("Empty EFsms?");
        }
    } else {
        ofono_error("Cannot read SMS from SIM");
    }

    binder_sms_sim_read_data_free(cbd);
}

static
void
binder_sms_on_sim(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSms* self = user_data;
    GBinderReader reader;
    gint32 rec;

    ofono_info("new sms on sim");
    guint32 ind_code = self->interface_aidl == RADIO_MESSAGING_INTERFACE ?
        RADIO_MESSAGING_IND_NEW_SMS_ON_SIM : RADIO_IND_NEW_SMS_ON_SIM;

    /* newSmsOnSim(RadioIndicationType type, int32 recordNumber); */
    GASSERT(code == ind_code);
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, &rec)) {
        DBG("rec %d", rec);
        if (self->sim_context) {
            ofono_sim_read_record(self->sim_context, SIM_EFSMS_FILEID,
                OFONO_SIM_FILE_STRUCTURE_FIXED, rec, EFSMS_LENGTH,
                sim_path, sizeof(sim_path), binder_sms_on_sim_cb,
                binder_sms_sim_read_data_new(self, rec));
        }
    }
}

static
gboolean binder_sms_register(
    gpointer user_data)
{
    BinderSms* self = user_data;
    RadioClient* client = self->g->client;

    DBG("");
    GASSERT(self->register_id);
    self->register_id = 0;

    ofono_sms_register(self->sms);

    /* Register event handlers */
    if (self->interface_aidl == RADIO_AIDL_INTERFACE_NONE) {
        self->radio_event[SMS_RADIO_EVENT_NEW_SMS] =
            radio_client_add_indication_handler(client,
                RADIO_IND_NEW_SMS, binder_sms_incoming, self);
        self->radio_event[SMS_RADIO_EVENT_NEW_STATUS_REPORT] =
            radio_client_add_indication_handler(client,
                RADIO_IND_NEW_SMS_STATUS_REPORT, binder_sms_incoming, self);
        self->radio_event[SMS_RADIO_EVENT_NEW_SMS_ON_SIM] =
            radio_client_add_indication_handler(client,
                RADIO_IND_NEW_SMS_ON_SIM, binder_sms_on_sim, self);
    } else {
        self->radio_event[SMS_RADIO_EVENT_NEW_SMS] =
            radio_client_add_indication_handler(client,
                RADIO_MESSAGING_IND_NEW_SMS, binder_sms_incoming, self);
        self->radio_event[SMS_RADIO_EVENT_NEW_STATUS_REPORT] =
            radio_client_add_indication_handler(client,
                RADIO_MESSAGING_IND_NEW_SMS_STATUS_REPORT, binder_sms_incoming, self);
        self->radio_event[SMS_RADIO_EVENT_NEW_SMS_ON_SIM] =
            radio_client_add_indication_handler(client,
                RADIO_MESSAGING_IND_NEW_SMS_ON_SIM, binder_sms_on_sim, self);
    }

    if (self->sms_ext) {
        /* Extension */
        self->ext_event[SMS_EXT_EVENT_INCOMING_SMS] =
            binder_ext_sms_add_incoming_handler(self->sms_ext,
                binder_sms_ext_incoming, self);
        self->ext_event[SMS_EXT_EVENT_STATUS_REPORT] =
            binder_ext_sms_add_report_handler(self->sms_ext,
                binder_sms_ext_status_report, self);
    }

    return G_SOURCE_REMOVE;
}

static
int
binder_sms_probe(
    struct ofono_sms* sms,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderSms* self = g_new0(BinderSms, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "");

    self->sms = sms;
    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->sim_context = ofono_sim_context_create(self->watch->sim);
    self->ims_reg = binder_ims_reg_ref(modem->ims);
    self->g = radio_request_group_new(modem->messaging_client); /* Keeps ref to client */
    self->interface_aidl = radio_client_aidl_interface(modem->messaging_client);

    if (modem->ext && (self->sms_ext = binder_ext_slot_get_interface(modem->ext,
        BINDER_EXT_TYPE_SMS)) != NULL) {
        DBG_(self, "using %ssms extension", SMS_TYPE_STR(self->sms_ext));
        binder_ext_sms_ref(self->sms_ext);
    }

    GASSERT(self->sim_context);
    self->register_id = g_idle_add(binder_sms_register, self);
    ofono_sms_set_data(sms, self);
    return 0;
}

static
void
binder_sms_remove(
    struct ofono_sms* sms)
{
    BinderSms* self = binder_sms_get_data(sms);

    DBG_(self, "");

    if (self->sim_context) {
        ofono_sim_context_free(self->sim_context);
    }

    if (self->register_id) {
        g_source_remove(self->register_id);
    }

    if (self->sms_ext) {
        binder_ext_sms_remove_all_handlers(self->sms_ext, self->ext_event);
        binder_ext_sms_cancel(self->sms_ext, self->ext_send_id);
        binder_ext_sms_unref(self->sms_ext);
    }

    radio_client_remove_all_handlers(self->g->client, self->radio_event);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    binder_ims_reg_unref(self->ims_reg);
    g_free(self->log_prefix);
    g_free(self);

    ofono_sms_set_data(sms, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_sms_driver binder_sms_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_sms_probe,
    .remove         = binder_sms_remove,
    .sca_query      = binder_sms_sca_query,
    .sca_set        = binder_sms_sca_set,
    .submit         = binder_sms_submit
};

void
binder_sms_init()
{
    ofono_sms_driver_register(&binder_sms_driver);
}

void
binder_sms_cleanup()
{
    ofono_sms_driver_unregister(&binder_sms_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
