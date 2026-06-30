/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *  Copyright (C) 2024 Slava Monich <slava@monich.com>
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

typedef struct binder_sms_api BinderSmsApi;

typedef struct binder_sms {
    struct ofono_sms* sms;
    struct ofono_watch* watch;
    struct ofono_sim_context* sim_context;
    const BinderSmsApi* api;
    char* log_prefix;
    guint ext_send_id;
    BinderExtSms* sms_ext;
    BinderImsReg* ims_reg;
    RadioRequestGroup* g;
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

typedef enum binder_sms_send_flags {
    BINDER_SMS_SEND_FLAGS_NONE = 0,
    BINDER_SMS_SEND_FLAG_EXPECT_MORE = 0x01,
    BINDER_SMS_SEND_FLAG_TRIED_EXT_API = 0x02,
    BINDER_SMS_SEND_FLAG_TRIED_IRADIO_API = 0x04
} BINDER_SMS_SEND_FLAGS;

typedef struct binder_sms_submit_cbd {
    int ref_count;
    BINDER_SMS_SEND_FLAGS flags;
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

struct binder_sms_api {
    const char* name;
    BinderReadStringArg read_string_arg;
    BinderWriteStringArg write_string_arg;
    BinderTakeStringArg take_string_arg;
    BinderReadByteArrayArg read_byte_array;
    RADIO_REQ get_smsc_address_req;
    RADIO_REQ set_smsc_address_req;
    RADIO_REQ send_sms_req;
    RADIO_REQ send_sms_expect_more_req;
    void (*write_send_sms_args)(
        GBinderWriter* writer,
        const char* smsc,
        const char* tpdu_hex);
    gboolean (*read_send_sms_resp)(
        GBinderReader* reader,
        gint32* message_ref,
        gint32* error_code);
    RADIO_REQ acknowledge_last_incoming_gsm_sms_req;
    RADIO_REQ delete_sms_on_sim_req;
    RADIO_IND new_sms_ind;
    RADIO_IND new_sms_status_report_ind;
    RADIO_IND new_sms_on_sim_ind;
};

static const BinderSmsApi binder_sms_api_hidl;
static const BinderSmsApi binder_sms_api_aidl;

static
gboolean
binder_sms_send(
    BinderSmsSubmitCbData* cbd);

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
    BINDER_SMS_SEND_FLAGS flags,
    const void* pdu,
    int pdu_len,
    int tpdu_len,
    ofono_sms_submit_cb_t cb,
    void* data)
{
    BinderSmsSubmitCbData* cbd = g_slice_new0(BinderSmsSubmitCbData);

    cbd->ref_count = 1;
    cbd->flags = flags;
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
binder_sms_submit_cbd_unref(
    BinderSmsSubmitCbData* cbd)
{
    if (!--cbd->ref_count) {
        g_free(cbd->pdu);
        gutil_slice_free(cbd);
    }
}

static
void
binder_sms_submit_cbd_destroy(
    gpointer cbd)
{
    binder_sms_submit_cbd_unref((BinderSmsSubmitCbData*)cbd);
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "setSmscAddress tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("smsc setting error %s", binder_radio_error_string(error));
    } else {
        cb(binder_error_ok(&err), cbd->data);
        return;
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
    const BinderSmsApi* api = self->api;
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        api->set_smsc_address_req, &writer, binder_sms_sca_set_cb,
        binder_sms_cbd_free, binder_sms_cbd_new(self, BINDER_CB(cb), data));

    /*
     * IRadio.hal:
     * oneway setSmscAddress(int32_t serial, string smsc);
     *
     * IRadioMessaging.aidl:
     * void setSmscAddress(in int serial, in String smsc);
     */
    DBG_(self, "setting sca: %s", sca->number);
    if (sca->type == OFONO_NUMBER_TYPE_INTERNATIONAL) {
        api->take_string_arg(&writer, g_strconcat("+", sca->number, NULL));
    } else {
        api->write_string_arg(&writer, sca->number);
    }

    if (!radio_request_submit(req)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }

    radio_request_unref(req);
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "getSmscAddress tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("smsc query error %s", binder_radio_error_string(error));
    } else {
        GBinderReader reader;
        const char* smsc;
        char* arg;

        /*
         * IRadioResponse.hal:
         * oneway getSmscAddressResponse(RadioResponseInfo info,
         *     string smsc);
         *
         * IRadioMessagingResponse.aidl:
         * void getSmscAddressResponse(in RadioResponseInfo info,
         *     in String smsc);
         */
        gbinder_reader_copy(&reader, args);
        smsc = self->api->read_string_arg(&reader, &arg);

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
            g_free(arg);
            return;
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
    RadioRequest* req = radio_request_new2(self->g,
        self->api->get_smsc_address_req, NULL, binder_sms_sca_query_cb,
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
    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "sendSms tx error");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("sms send error %s",
          binder_radio_error_string(error));
    } else {
        GBinderReader reader;
        gint32 msg_ref, error_code;

        gbinder_reader_copy(&reader, args);
        if (self->api->read_send_sms_resp(&reader, &msg_ref, &error_code)) {
            /*
             * Error is -1 if unknown or not applicable,
             * otherwise 3GPP 27.005, 3.2.5
             */
            if (error_code > 0) {
                err.type = OFONO_ERROR_TYPE_CMS;
                err.error = error_code;
            } else {
                /* Success */
                cb(binder_error_ok(&err), msg_ref, cbd->data);
                return;
            }

            /* Try an alternative way of sending SMS, if there is one */
            if (binder_sms_send(cbd)) {
                return;
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
    binder_error_init_failure(&err);
    switch (result) {
    case BINDER_EXT_SMS_SEND_RESULT_OK:
        /* SMS has been sent */
        cbd->cb(binder_error_ok(&err), msg_ref, cbd->data);
        return;
    case BINDER_EXT_SMS_SEND_RESULT_RETRY:
    case BINDER_EXT_SMS_SEND_RESULT_ERROR_RADIO_OFF:
    case BINDER_EXT_SMS_SEND_RESULT_ERROR_NO_SERVICE:
        /* Try an alternative way of sending SMS, if there is one */
        if (binder_sms_send(cbd)) {
            return;
        }
        break;
    case BINDER_EXT_SMS_SEND_RESULT_ERROR_NETWORK_TIMEOUT:
        /*
         * 332 (network timeout) defined in 3GPP 27.005, 3.2.5
         * is the only one actually handled by the ofono core.
         */
        err.type = OFONO_ERROR_TYPE_CMS;
        err.error = 332; /* network timeout */
        break;
    case BINDER_EXT_SMS_SEND_RESULT_ERROR:
        break;
    }

    /* Error path */
    cbd->cb(&err, 0, cbd->data);
}

static
gboolean
binder_sms_send(
    BinderSmsSubmitCbData* cbd)
{
    BinderSms* self = cbd->self;
    const BinderSmsApi* api = self->api;

    DBG_(self, "pdu_len: %d, tpdu_len: %d flags: 0x%02x", cbd->pdu_len,
        cbd->tpdu_len, cbd->flags);

    if (!(cbd->flags & BINDER_SMS_SEND_FLAG_TRIED_EXT_API) &&
        binder_sms_can_send_ext_message(self)) {
        /*
         * SMSC address:
         *
         * smsc_len == 1, then zero-length SMSC was specified
         */
        const int smsc_len = cbd->pdu_len - cbd->tpdu_len;
        char* smsc = (smsc_len > 1) ?
            g_strndup((char*)cbd->pdu, smsc_len) :
            NULL;

        /* binder_sms_submit_cbd_destroy will drop this ref */
        cbd->ref_count++;

        /* Use vendor specific mechanism */
        cbd->flags |= BINDER_SMS_SEND_FLAG_TRIED_EXT_API;
        binder_ext_sms_cancel(self->sms_ext, self->ext_send_id);
        self->ext_send_id = binder_ext_sms_send(self->sms_ext,
            smsc, cbd->pdu + smsc_len, cbd->tpdu_len, 0,
            (cbd->flags & BINDER_SMS_SEND_FLAG_EXPECT_MORE) ?
            BINDER_EXT_SMS_SEND_EXPECT_MORE : BINDER_EXT_SMS_SEND_NO_FLAGS,
            binder_sms_submit_ext_cb, binder_sms_submit_cbd_destroy, cbd);
        g_free(smsc);
        if (self->ext_send_id) {
            /* Request submitted */
            return TRUE;
        }

        /*
         * Since binder_ext_sms_send failed, binder_sms_submit_cbd_destroy
         * wasn't and won't be invoked and we need to undo our increment of
         * cbd->ref_count.
         */
        binder_sms_submit_cbd_unref(cbd);
    }

    /* No luck with ext api, try the regular one */
    if (!(cbd->flags & BINDER_SMS_SEND_FLAG_TRIED_IRADIO_API)) {
        char* tpdu;
        const int smsc_len = cbd->pdu_len - cbd->tpdu_len;
        char* smsc = NULL;
        gboolean submitted;
        GBinderWriter writer;
        RadioRequest* req = radio_request_new2(self->g,
            (cbd->flags & BINDER_SMS_SEND_FLAG_EXPECT_MORE) ?
            api->send_sms_expect_more_req : api->send_sms_req, &writer,
            binder_sms_submit_cb, binder_sms_submit_cbd_destroy, cbd);

        /* binder_sms_submit_cbd_destroy will drop this ref */
        cbd->ref_count++;

        /* If smsc_len == 1 then zero-length SMSC was specified */
        if (smsc_len > 1) {
            smsc = gbinder_writer_malloc(&writer, smsc_len + 1);
            memcpy(smsc, cbd->pdu, smsc_len);
            smsc[smsc_len] = 0;
        }

        /* PDU is sent as an ASCII hex string */
        tpdu = gbinder_writer_malloc(&writer, cbd->tpdu_len * 2 + 1);
        ofono_encode_hex(cbd->pdu + smsc_len, cbd->tpdu_len, tpdu);
        DBG_(self, "%s", tpdu);

        /* Try to submit the request */
        cbd->flags |= BINDER_SMS_SEND_FLAG_TRIED_IRADIO_API;
        api->write_send_sms_args(&writer, smsc, tpdu);
        submitted = radio_request_submit(req);
        radio_request_unref(req);
        return submitted;
    }

    return FALSE;
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
    BinderSms* self = binder_sms_get_data(sms);
    BinderSmsSubmitCbData* cbd = binder_sms_submit_cbd_new(self, expect_more ?
        BINDER_SMS_SEND_FLAG_EXPECT_MORE : BINDER_SMS_SEND_FLAGS_NONE,
        pdu, pdu_len, tpdu_len, cb, data);

    if (!binder_sms_send(cbd)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), 0, data);
    }
    binder_sms_submit_cbd_unref(cbd);
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "acknowledgeLastIncomingGsmSms tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("SMS acknowledgement failed: %s",
            binder_radio_error_string(error));
    }
}

static
void
binder_sms_ack(
    BinderSms* self,
    gboolean ok)
{
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        self->api->acknowledge_last_incoming_gsm_sms_req, &writer,
        binder_sms_ack_cb, NULL, self);

    /*
     * IRadio.hal:
     * oneway acknowledgeLastIncomingGsmSms(int32_t serial,
     *     bool success, SmsAcknowledgeFailCause cause);
     *
     * IRadioMessaging.aidl:
     * void acknowledgeLastIncomingGsmSms(in int serial,
     *     in boolean success, in SmsAcknowledgeFailCause cause);
     */
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
     * IRadioIndication.hal:
     * oneway newSms(RadioIndicationType type, vec<uint8_t> pdu);
     *
     * IRadioMessagingIndication.aidl:
     * void newSms(in RadioIndicationType type, in byte[] pdu);
     */
    gbinder_reader_copy(&reader, args);
    pdu = self->api->read_byte_array(&reader, &len);
    if (pdu) {
        const guint pdu_len = (guint) len;

        ofono_info("incoming sms, %u bytes", pdu_len);
        if (binder_sms_notify(self, pdu, pdu_len, ofono_sms_deliver_notify)) {
            binder_sms_ack(self, TRUE);
            return;
        }
    }
    ofono_error("Unable to parse SMS notification");
    binder_sms_ack(self, FALSE);
}

static
void
binder_sms_incoming_status_report(
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
     * RadioIndication.hal:
     * oneway newSmsStatusReport(RadioIndicationType type, vec<uint8_t> pdu);
     *
     * IRadioMessagingIndication.aidl:
     * void newSmsStatusReport(in RadioIndicationType type, in byte[] pdu);
     */
    gbinder_reader_copy(&reader, args);
    pdu = self->api->read_byte_array(&reader, &len);
    if (pdu) {
        const guint pdu_len = (guint) len;

        ofono_info("sms status, %u bytes", pdu_len);
        if (binder_sms_notify(self, pdu, pdu_len, ofono_sms_status_notify)) {
            binder_sms_ack(self, TRUE);
            return;
        }
    }
    ofono_error("Unable to parse SMS status notification");
    binder_sms_ack(self, FALSE);
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "deleteSmsOnSim tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to delete sms from sim: %s",
            binder_radio_error_string(error));
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

                ofono_info("read sms from sim, %u bytes", pdu_len);
                DBG_(self, "smsc: %s", binder_print_hex(pdu, smsc_len));
                DBG_(self, "tpdu: %s", binder_print_hex(pdu + smsc_len,
                    tpdu_len));

                ofono_sms_deliver_notify(self->sms, pdu, pdu_len, tpdu_len);

                /*
                 * IRadio.hal:
                 * oneway deleteSmsOnSim(int32_t serial, int32_t index);
                 *
                 * IRadioMessaging.aidl:
                 * void deleteSmsOnSim(in int serial, in int index);
                 */
                DBG_(self, "deleting record: %d", cbd->record);
                req = radio_request_new2(self->g,
                    self->api->delete_sms_on_sim_req, &writer,
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
    gint32 rec;

    ofono_info("new sms on sim");

    /* newSmsOnSim(RadioIndicationType type, int32 recordNumber); */
    if (binder_read_int32(args, &rec)) {
        BinderSms* self = user_data;

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
gboolean
binder_sms_register(
    gpointer user_data)
{
    BinderSms* self = user_data;
    RadioClient* client = self->g->client;
    const BinderSmsApi* api = self->api;

    DBG("");
    GASSERT(self->register_id);
    self->register_id = 0;

    ofono_sms_register(self->sms);

    /* Register event handlers */
    self->radio_event[SMS_RADIO_EVENT_NEW_SMS] =
        radio_client_add_indication_handler(client,
            api->new_sms_ind, binder_sms_incoming, self);
    self->radio_event[SMS_RADIO_EVENT_NEW_STATUS_REPORT] =
        radio_client_add_indication_handler(client,
            api->new_sms_status_report_ind,
            binder_sms_incoming_status_report, self);
    self->radio_event[SMS_RADIO_EVENT_NEW_SMS_ON_SIM] =
        radio_client_add_indication_handler(client,
            api->new_sms_on_sim_ind, binder_sms_on_sim, self);

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
    RadioClient* messaging_client = modem->clients.messaging_client;
    const BinderSmsApi* api = radio_client_aidl_interface(messaging_client) ==
        RADIO_MESSAGING_INTERFACE ? &binder_sms_api_aidl :
        &binder_sms_api_hidl;

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "%s api", api->name);

    self->sms = sms;
    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->sim_context = ofono_sim_context_create(self->watch->sim);
    self->ims_reg = binder_ims_reg_ref(modem->ims);
    self->g = radio_request_group_new(messaging_client);
    self->api = api;

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

    gutil_source_remove(self->register_id);

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
 * HIDL API flavor
 *==========================================================================*/

static
void
binder_sms_api_write_send_sms_args_hidl(
    GBinderWriter* writer,
    const char* smsc,
    const char* tpdu_hex)
{
    RadioGsmSmsMessage* msg =  gbinder_writer_new0(writer, RadioGsmSmsMessage);
    guint msg_index;

    /* The strings are guaranteed to outlive the request */
    msg->smscPdu.len = gutil_strlen0(smsc);
    msg->smscPdu.data.str = smsc ? smsc : "";
    msg->pdu.len = gutil_strlen0(tpdu_hex);
    msg->pdu.data.str = tpdu_hex;

    /* Write GsmSmsMessage and its strings */
    msg_index = gbinder_writer_append_buffer_object(writer, msg, sizeof(*msg));
    binder_append_hidl_string_data(writer, msg, smscPdu, msg_index);
    binder_append_hidl_string_data(writer, msg, pdu, msg_index);
}

static
gboolean
binder_sms_api_read_send_sms_resp_hidl(
    GBinderReader* reader,
    gint32* message_ref,
    gint32* error_code)
{
    const RadioSendSmsResult* res =
        gbinder_reader_read_hidl_struct(reader, RadioSendSmsResult);

    if (res) {
        DBG("sms msg ref: %d, ack: %s err: %d", res->messageRef,
            res->ackPDU.data.str, res->errorCode);
        *message_ref = res->messageRef;
        *error_code = res->errorCode;
        return TRUE;
    }
    return FALSE;
}

static const BinderSmsApi binder_sms_api_hidl = {
    "hidl",
    binder_read_string_arg_hidl,
    binder_write_string_arg_hidl,
    binder_take_string_arg_hidl,
    binder_read_byte_array_hidl,
    RADIO_REQ_GET_SMSC_ADDRESS,
    RADIO_REQ_SET_SMSC_ADDRESS,
    RADIO_REQ_SEND_SMS,
    RADIO_REQ_SEND_SMS_EXPECT_MORE,
    binder_sms_api_write_send_sms_args_hidl,
    binder_sms_api_read_send_sms_resp_hidl,
    RADIO_REQ_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS,
    RADIO_REQ_DELETE_SMS_ON_SIM,
    RADIO_IND_NEW_SMS,
    RADIO_IND_NEW_SMS_STATUS_REPORT,
    RADIO_IND_NEW_SMS_ON_SIM
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static
void
binder_sms_api_write_send_sms_args_aidl(
    GBinderWriter* writer,
    const char* smsc,
    const char* tpdu_hex)
{
    GBinderWriter parcel;

    /*
     * package android.hardware.radio.messaging;
     * parcelable GsmSmsMessage {
     *   String smscPdu;
     *   String pdu;
     * }
     */
    gbinder_writer_start_parcelable(writer, &parcel);
    gbinder_writer_append_string16(&parcel, smsc ? smsc : "");
    gbinder_writer_append_string16(&parcel, tpdu_hex);
    gbinder_writer_finish_parcelable(&parcel);
}

static
gboolean
binder_sms_api_read_send_sms_resp_aidl(
    GBinderReader* reader,
    gint32* message_ref,
    gint32* error_code)
{
    GBinderReader parcel;
    gboolean ok = FALSE;

    /*
     * package android.hardware.radio.messaging;
     * parcelable SendSmsResult {
     *   int messageRef;
     *   String ackPDU;
     *   int errorCode;
     * }
     */
    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        char* pdu;

        if (gbinder_reader_read_int32(&parcel, message_ref) &&
            gbinder_reader_read_nullable_string16(&parcel, &pdu) &&
            gbinder_reader_read_int32(&parcel, error_code)) {
            DBG("sms msg ref: %d, ack: %s err: %d", *message_ref,
                pdu, *error_code);
            g_free(pdu);
            ok = TRUE;
        }
        gbinder_reader_finish_parcelable(&parcel);
    }
    return ok;
}

static const BinderSmsApi binder_sms_api_aidl = {
    "aidl",
    binder_read_string_arg_aidl,
    binder_write_string_arg_aidl,
    binder_take_string_arg_aidl,
    gbinder_reader_read_byte_array,
    RADIO_MESSAGING_REQ_GET_SMSC_ADDRESS,
    RADIO_MESSAGING_REQ_SET_SMSC_ADDRESS,
    RADIO_MESSAGING_REQ_SEND_SMS,
    RADIO_MESSAGING_REQ_SEND_SMS_EXPECT_MORE,
    binder_sms_api_write_send_sms_args_aidl,
    binder_sms_api_read_send_sms_resp_aidl,
    RADIO_MESSAGING_REQ_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS,
    RADIO_MESSAGING_REQ_DELETE_SMS_ON_SIM,
    RADIO_MESSAGING_IND_NEW_SMS,
    RADIO_MESSAGING_IND_NEW_SMS_STATUS_REPORT,
    RADIO_MESSAGING_IND_NEW_SMS_ON_SIM
};

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
