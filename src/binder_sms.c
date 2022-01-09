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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_sms.h"
#include "binder_util.h"

#include <ofono/log.h>
#include <ofono/misc.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/watch.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

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
    SMS_EVENT_NEW_SMS,
    SMS_EVENT_NEW_STATUS_REPORT,
    SMS_EVENT_NEW_SMS_ON_SIM,
    SMS_EVENT_COUNT
};

typedef struct binder_sms {
    struct ofono_sms* sms;
    struct ofono_watch* watch;
    struct ofono_sim_context* sim_context;
    char* log_prefix;
    RadioRequestGroup* g;
    gulong event_id[SMS_EVENT_COUNT];
    guint register_id;
} BinderSms;

typedef struct binder_sms_cbd {
    BinderSms* self;
    union _ofono_sms_cb {
        ofono_sms_sca_set_cb_t sca_set;
        ofono_sms_sca_query_cb_t sca_query;
        ofono_sms_submit_cb_t submit;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderSmsCbData;

typedef struct binder_sms_sim_read_data {
    BinderSms* self;
    int record;
} BinderSmsSimReadData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

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
        if (resp == RADIO_RESP_SET_SMSC_ADDRESS) {
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

    /* setSmscAddress(int32_t serial, string smsc); */
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SET_SMSC_ADDRESS, &writer, binder_sms_sca_set_cb,
        binder_sms_cbd_free, binder_sms_cbd_new(self, BINDER_CB(cb), data));

    DBG_(self, "setting sca: %s", number);
    binder_append_hidl_string(&writer, number);

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
    ofono_sms_sca_query_cb_t cb = cbd->cb.sca_query;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_SMSC_ADDRESS) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                const char* smsc;

                gbinder_reader_copy(&reader, args);
                smsc = gbinder_reader_read_hidl_string_c(&reader);
                if (smsc) {
                    struct ofono_phone_number sca;

                    if (smsc[0] == '+') {
                        smsc++;
                        sca.type = OFONO_NUMBER_TYPE_INTERNATIONAL;
                    } else {
                        sca.type = OFONO_NUMBER_TYPE_UNKNOWN;
                    }
                    g_strlcpy(sca.number, smsc, sizeof(sca.number));
                    DBG("csca_query_cb: %s, %d", sca.number, sca.type);
                    cb(binder_error_ok(&err), &sca, cbd->data);
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
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_GET_SMSC_ADDRESS, NULL, binder_sms_sca_query_cb,
        binder_sms_cbd_free, binder_sms_cbd_new(self, BINDER_CB(cb), data));

    DBG_(self, "sending csca_query");
    if (!radio_request_submit(req)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), NULL, data);
    }
    radio_request_unref(req);
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
    BinderSmsCbData* cbd = user_data;
    ofono_sms_submit_cb_t cb = cbd->cb.submit;
    struct ofono_error err;

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SEND_SMS ||
            resp == RADIO_RESP_SEND_SMS_EXPECT_MORE) {
            /*
             * sendSmsResponse(RadioResponseInfo, SendSmsResult sms);
             * sendSMSExpectMoreResponse(RadioResponseInfo, SendSmsResult sms);
             */
            if (error == RADIO_ERROR_NONE) {
                const RadioSendSmsResult* res;
                GBinderReader reader;

                gbinder_reader_copy(&reader, args);
                res = gbinder_reader_read_hidl_struct(&reader,
                    RadioSendSmsResult);

                if (res) {
                    DBG("sms msg ref: %d, ack: %s err: %d", res->messageRef,
                        res->ackPDU.data.str, res->errorCode);

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
                ofono_error("sms send error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected sendSms response %d", resp);
        }
    }
    /* Error path */
    cb(&err, 0, cbd->data);
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
    RadioGsmSmsMessage* msg;
    GBinderWriter writer;
    char* tpdu;
    guint parent;
    int smsc_len;

    /* sendSms(int32 serial, GsmSmsMessage message); */
    RadioRequest* req = radio_request_new2(self->g, expect_more ?
        RADIO_REQ_SEND_SMS_EXPECT_MORE : RADIO_REQ_SEND_SMS, &writer,
        binder_sms_submit_cb, binder_sms_cbd_free,
        binder_sms_cbd_new(self, BINDER_CB(cb), data));

    DBG("pdu_len: %d, tpdu_len: %d more: %d", pdu_len, tpdu_len, expect_more);

    msg = gbinder_writer_new0(&writer, RadioGsmSmsMessage);

    /*
     * SMSC address:
     *
     * smsc_len == 1, then zero-length SMSC was specified but IRadio
     * interface expects an empty string for default SMSC.
     */
    smsc_len = pdu_len - tpdu_len;
    if (smsc_len > 1) {
        binder_copy_hidl_string_len(&writer, &msg->smscPdu, (char*)
            pdu, smsc_len);
    } else {
        /* Default SMSC address */
        binder_copy_hidl_string(&writer, &msg->smscPdu, NULL);
    }

    /* PDU is sent as an ASCII hex string */
    msg->pdu.len = tpdu_len * 2;
    tpdu = gbinder_writer_malloc(&writer, msg->pdu.len + 1);
    ofono_encode_hex(pdu + smsc_len, tpdu_len, tpdu);
    msg->pdu.data.str = tpdu;
    DBG_(self, "%s", tpdu);

    /* Write GsmSmsMessage and its strings */
    parent = gbinder_writer_append_buffer_object(&writer, msg, sizeof(*msg));
    binder_append_hidl_string_data(&writer, msg, smscPdu, parent);
    binder_append_hidl_string_data(&writer, msg, pdu, parent);

    /* Submit the request */
    if (!radio_request_submit(req)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), 0, data);
    }
    radio_request_unref(req);
}

static
void
binder_ack_delivery_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS) {
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
binder_ack_delivery(
    BinderSms* self,
    gboolean ok)
{
    GBinderWriter writer;

    /*
     * acknowledgeLastIncomingGsmSms(int32 serial, bool success,
     *     SmsAcknowledgeFailCause cause);
     */
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS, &writer,
        binder_ack_delivery_cb, NULL, NULL);

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
binder_sms_notify(
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
    pdu = gbinder_reader_read_hidl_byte_vec(&reader, &len);
    if (pdu) {
        const guint pdu_len = (guint) len;
        const guint smsc_len = (guint) pdu[0] + 1;

        ofono_info("%s, %u bytes",(code == RADIO_IND_NEW_SMS) ?
            "incoming sms" : "sms status", pdu_len);
        if (pdu_len > smsc_len) {
            /* The PDU starts with the SMSC address per TS 27.005 (+CMT:) */
            const guint tpdu_len = pdu_len - smsc_len;

            DBG_(self, "smsc: %s", binder_print_hex(pdu, smsc_len));
            DBG_(self, "tpdu: %s", binder_print_hex(pdu + smsc_len, tpdu_len));

            switch (code) {
            case RADIO_IND_NEW_SMS:
                ofono_sms_deliver_notify(self->sms, pdu, pdu_len, tpdu_len);
                binder_ack_delivery(self, TRUE);
                break;
            case RADIO_IND_NEW_SMS_STATUS_REPORT:
                ofono_sms_status_notify(self->sms, pdu, pdu_len, tpdu_len);
                binder_ack_delivery(self, TRUE);
                break;
            default:
                binder_ack_delivery(self, FALSE);
                break;
            }
            return;
        }
    }
    ofono_error("Unable to parse SMS notification");
    binder_ack_delivery(self, FALSE);
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
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_DELETE_SMS_ON_SIM) {
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

                ofono_info("read sms from sim, %u bytes", pdu_len);
                DBG_(self, "smsc: %s", binder_print_hex(pdu, smsc_len));
                DBG_(self, "tpdu: %s", binder_print_hex(pdu + smsc_len,
                    tpdu_len));

                ofono_sms_deliver_notify(self->sms, pdu, pdu_len, tpdu_len);

                /* deleteSmsOnSim(int32 serial, int32 index); */
                DBG_(self, "deleting record: %d", cbd->record);
                req = radio_request_new2(self->g,
                    RADIO_REQ_DELETE_SMS_ON_SIM, &writer,
                    binder_sms_delete_on_sim_cb, NULL, NULL);
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

    /* newSmsOnSim(RadioIndicationType type, int32 recordNumber); */
    GASSERT(code == RADIO_IND_NEW_SMS_ON_SIM);
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
    self->event_id[SMS_EVENT_NEW_SMS] =
        radio_client_add_indication_handler(client,
            RADIO_IND_NEW_SMS, binder_sms_notify, self);
    self->event_id[SMS_EVENT_NEW_STATUS_REPORT] =
        radio_client_add_indication_handler(client,
            RADIO_IND_NEW_SMS_STATUS_REPORT, binder_sms_notify, self);
    self->event_id[SMS_EVENT_NEW_SMS_ON_SIM] =
        radio_client_add_indication_handler(client,
            RADIO_IND_NEW_SMS_ON_SIM, binder_sms_on_sim, self);

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

    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->sim_context = ofono_sim_context_create(self->watch->sim);
    self->g = radio_request_group_new(modem->client); /* Keeps ref to client */
    self->sms = sms;

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

    radio_client_remove_all_handlers(self->g->client, self->event_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

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
