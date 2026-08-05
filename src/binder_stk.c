/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
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
#include "binder_stk.h"
#include "binder_util.h"

#include <ofono/log.h>
#include <ofono/stk.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>
#include <radio_sim_types.h>
#include <radio_voice_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_misc.h>

enum binder_stk_events {
    STK_EVENT_PROACTIVE_COMMAND,
    STK_EVENT_SESSION_END,
    STK_EVENT_NOTIFY,
    STK_EVENT_COUNT
};

typedef struct binder_stk_api BinderStkApi;

typedef struct binder_stk {
    struct ofono_stk* stk;
    const BinderStkApi* api;
    char* log_prefix;
    RadioRequestGroup* sim_g;
    RadioRequestGroup* voice_g;
    gulong sim_event_id[STK_EVENT_COUNT];
    guint register_id;
} BinderStk;

typedef struct binder_stk_cbd {
    BinderStk* self;
    union _ofono_stk_cb {
        ofono_stk_envelope_cb_t envelope;
        ofono_stk_generic_cb_t generic;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderStkCbData;

struct binder_stk_api {
    const char* name;
    BinderReadStringArg read_string_arg;
    BinderTakeStringArg take_string_arg;
    RADIO_REQ voice_handle_stk_call_setup_request_from_sim_req;
    RADIO_REQ sim_report_stk_service_is_running_req;
    RADIO_REQ sim_send_envelope_req;
    RADIO_REQ sim_send_terminal_response_to_sim_req;
    RADIO_IND sim_stk_proactive_command_ind;
    RADIO_IND sim_stk_session_end_ind;
    RADIO_IND sim_stk_event_notify_ind;
};

static const BinderStkApi binder_stk_api_hidl;
static const BinderStkApi binder_stk_api_aidl;

#define DBG_(cd,fmt,args...) DBG("%s" fmt, (cd)->log_prefix, ##args)

static inline BinderStk* binder_stk_get_data(struct ofono_stk* stk)
    { return ofono_stk_get_data(stk); }

static
BinderStkCbData*
binder_stk_cbd_new(
    BinderStk* self,
    BinderCallback cb,
    void* data)
{
    BinderStkCbData* cbd = g_slice_new0(BinderStkCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_stk_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderStkCbData, cbd);
}

static
void binder_stk_envelope_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStkCbData* cbd = user_data;
    ofono_stk_envelope_cb_t cb = cbd->cb.envelope;
    struct ofono_error err;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "sendEnvelope tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Error sending envelope: %s",
            binder_radio_error_string(error));
    } else {
        DBG_(cbd->self, "");
        cb(binder_error_ok(&err), NULL, 0, cbd->data);
        return;
    }
    cb(binder_error_failure(&err), NULL, 0, cbd->data);
}

static
void
binder_stk_envelope(
    struct ofono_stk* stk,
    int length,
    const unsigned char* cmd,
    ofono_stk_envelope_cb_t cb,
    void* data)
{
    BinderStk* self = binder_stk_get_data(stk);
    const BinderStkApi* api = self->api;
    char* hex = binder_encode_hex(cmd, length);
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->sim_g,
        api->sim_send_envelope_req, &args,
        binder_stk_envelope_cb, binder_stk_cbd_free,
        binder_stk_cbd_new(self, BINDER_CB(cb), data));

    /*
     * IRadio.hal:
     * oneway sendEnvelope(int32_t serial, string command);
     *
     * IRadioSim.aidl:
     * void sendEnvelope(in int serial, in String contents);
     */
    DBG("envelope %s", hex);
    api->take_string_arg(&args, hex);

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_stk_terminal_response_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStkCbData* cbd = user_data;
    ofono_stk_generic_cb_t cb = cbd->cb.generic;
    struct ofono_error err;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(cbd->self, "sendTerminalResponseToSim tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Error sending terminal response: %s",
            binder_radio_error_string(error));
    } else {
        DBG_(cbd->self, "");
        cb(binder_error_ok(&err), cbd->data);
        return;
    }
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_stk_terminal_response(
    struct ofono_stk* stk,
    int length,
    const unsigned char* resp,
    ofono_stk_generic_cb_t cb,
    void* data)
{
    BinderStk* self = binder_stk_get_data(stk);
    const BinderStkApi* api = self->api;
    char* hex = binder_encode_hex(resp, length);
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->sim_g,
        api->sim_send_terminal_response_to_sim_req, &args,
        binder_stk_terminal_response_cb, binder_stk_cbd_free,
        binder_stk_cbd_new(self, BINDER_CB(cb), data));

    /*
     * IRadio.hal:
     * oneway sendTerminalResponseToSim(int32_t serial, string contents);
     *
     * IRadioSim.aidl:
     * void sendTerminalResponseToSim(in int serial, in String contents);
     */
    DBG_(self, "terminal response: %s", hex);
    api->take_string_arg(&args, hex);

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_stk_user_confirmation(
    struct ofono_stk* stk,
    ofono_bool_t confirm)
{
    BinderStk* self = binder_stk_get_data(stk);
    const BinderStkApi* api = self->api;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->voice_g,
        api->voice_handle_stk_call_setup_request_from_sim_req, &args,
        NULL, NULL, NULL);

    /*
     * IRadio.hal:
     * oneway handleStkCallSetupRequestFromSim(int32_t serial, bool accept);
     *
     * IRadioVoice.aidl:
     * void handleStkCallSetupRequestFromSim(in int serial, in boolean accept);
     */
    DBG_(self, "%d", confirm);
    gbinder_writer_append_bool(&args, confirm);

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_stk_proactive_command(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStk* self = user_data;
    GBinderReader reader;
    char* tmp = NULL;
    const char* pcmd;

    /*
     * IRadioIndication.hal:
     * oneway stkProactiveCommand(RadioIndicationType type, string cmd);
     *
     * IRadioSimIndication.aidl:
     * void stkProactiveCommand(in RadioIndicationType type, in String cmd);
     *
     * cmd - SAT/USAT proactive represented as byte array starting with
     * command tag.
     *
     * Refer to ETSI TS 102.223 section 9.4 for command types.
     */
    gbinder_reader_copy(&reader, args);
    pcmd = self->api->read_string_arg(&reader, &tmp);
    if (pcmd) {
        guint len;
        void* pdu = binder_decode_hex(pcmd, -1, &len);

        if (pdu) {
            DBG_(self, "pcmd: %s", pcmd);
            ofono_stk_proactive_command_notify(self->stk, len, pdu);
            g_free(pdu);
        } else {
            ofono_warn("Failed to parse STK command %s", pcmd);
        }
        g_free(tmp);
    } else {
        ofono_warn("Failed to parse STK command");
    }
}

static
void
binder_stk_event_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStk* self = user_data;
    GBinderReader reader;
    char* tmp = NULL;
    const char* pcmd;

    /*
     * IRadioIndication.hal:
     * oneway stkEventNotify(RadioIndicationType type, string cmd);
     *
     * IRadioSimIndication.aidl:
     * void stkEventNotify(in RadioIndicationType type, in String cmd);
     *
     * cmd - SAT/USAT commands or responses sent by ME to SIM or commands
     * handled by ME, represented as byte array starting with first byte
     * of response data for command tag.
     *
     * Refer to ETSI TS 102.223 section 9.4 for command types.
     */
    gbinder_reader_copy(&reader, args);
    pcmd = self->api->read_string_arg(&reader, &tmp);
    if (pcmd) {
        guint len;
        void* pdu = binder_decode_hex(pcmd, -1, &len);

        if (pdu) {
            DBG_(self, "pcmd: %s", pcmd);
            ofono_stk_proactive_command_handled_notify(self->stk, len, pdu);
            g_free(pdu);
        } else {
            ofono_warn("Failed to parse STK event %s", pcmd);
        }
        g_free(tmp);
    } else {
        ofono_warn("Failed to parse STK event");
    }
}

static
void
binder_stk_session_end_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderStk* self = user_data;

    DBG_(self, "");
    /* stkSessionEnd(RadioIndicationType); */
    ofono_stk_proactive_session_end_notify(self->stk);
}

static
void
binder_stk_agent_ready(
    struct ofono_stk* stk)
{
    BinderStk* self = binder_stk_get_data(stk);
    const BinderStkApi* api = self->api;

    DBG_(self, "");

    if (!self->sim_event_id[STK_EVENT_PROACTIVE_COMMAND]) {
        RadioClient* sim_client = self->sim_g->client;

        DBG_(self, "Subscribing for notifications");
        self->sim_event_id[STK_EVENT_PROACTIVE_COMMAND] =
            radio_client_add_indication_handler(sim_client,
                api->sim_stk_proactive_command_ind,
                binder_stk_proactive_command, self);

        GASSERT(!self->sim_event_id[STK_EVENT_SESSION_END]);
        self->sim_event_id[STK_EVENT_SESSION_END] =
            radio_client_add_indication_handler(sim_client,
                api->sim_stk_session_end_ind,
                binder_stk_session_end_notify, self);

        GASSERT(!self->sim_event_id[STK_EVENT_NOTIFY]);
        self->sim_event_id[STK_EVENT_NOTIFY] =
            radio_client_add_indication_handler(sim_client,
                api->sim_stk_event_notify_ind,
                binder_stk_event_notify, self);
    }

    /*
     * IRadio.hal:
     * oneway reportStkServiceIsRunning(int32_t serial);
     *
     * IRadioSim.aidl:
     * void reportStkServiceIsRunning(in int serial);
     */
    binder_submit_request(self->sim_g,
        api->sim_report_stk_service_is_running_req);
}

static
gboolean binder_stk_register(
    gpointer user_data)
{
    BinderStk* self = user_data;

    DBG_(self, "");
    GASSERT(self->register_id);
    self->register_id = 0;

    ofono_stk_register(self->stk);

    return G_SOURCE_REMOVE;
}

static
int
binder_stk_probe(
    struct ofono_stk* stk,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderStk* self = g_new0(BinderStk, 1);
    RadioClient* sim_client = modem->clients.sim_client;
    const BinderStkApi* api = radio_client_aidl_interface(sim_client) ==
        RADIO_SIM_INTERFACE ? &binder_stk_api_aidl : &binder_stk_api_hidl;

    self->stk = stk;
    self->api = api;
    self->sim_g = radio_request_group_new(sim_client);
    self->voice_g = radio_request_group_new(modem->clients.voice_client);
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->register_id = g_idle_add(binder_stk_register, self);

    DBG_(self, "%s api", api->name);
    ofono_stk_set_data(stk, self);
    return 0;
}

static
void
binder_stk_remove(
    struct ofono_stk* stk)
{
    BinderStk* self = binder_stk_get_data(stk);

    DBG_(self, "");

    gutil_source_remove(self->register_id);
    radio_client_remove_all_handlers(self->sim_g->client, self->sim_event_id);
    radio_request_group_cancel(self->sim_g);
    radio_request_group_cancel(self->voice_g);
    radio_request_group_unref(self->sim_g);
    radio_request_group_unref(self->voice_g);

    g_free(self->log_prefix);
    g_free(self);

    ofono_stk_set_data(stk, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static const BinderStkApi binder_stk_api_hidl = {
    "hidl",
    binder_read_string_arg_hidl,
    binder_take_string_arg_hidl,
    RADIO_REQ_HANDLE_STK_CALL_SETUP_REQUEST_FROM_SIM,
    RADIO_REQ_REPORT_STK_SERVICE_IS_RUNNING,
    RADIO_REQ_SEND_ENVELOPE,
    RADIO_REQ_SEND_TERMINAL_RESPONSE_TO_SIM,
    RADIO_IND_STK_PROACTIVE_COMMAND,
    RADIO_IND_STK_SESSION_END,
    RADIO_IND_STK_EVENT_NOTIFY,
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static const BinderStkApi binder_stk_api_aidl = {
    "aidl",
    binder_read_string_arg_aidl,
    binder_take_string_arg_aidl,
    RADIO_VOICE_REQ_HANDLE_STK_CALL_SETUP_REQUEST_FROM_SIM,
    RADIO_SIM_REQ_REPORT_STK_SERVICE_IS_RUNNING,
    RADIO_SIM_REQ_SEND_ENVELOPE,
    RADIO_SIM_REQ_SEND_TERMINAL_RESPONSE_TO_SIM,
    RADIO_SIM_IND_STK_PROACTIVE_COMMAND,
    RADIO_SIM_IND_STK_SESSION_END,
    RADIO_SIM_IND_STK_EVENT_NOTIFY
};

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_stk_driver binder_stk_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_stk_probe,
    .remove                 = binder_stk_remove,
    .envelope               = binder_stk_envelope,
    .terminal_response      = binder_stk_terminal_response,
    .user_confirmation      = binder_stk_user_confirmation,
    .ready                  = binder_stk_agent_ready
};

void
binder_stk_init()
{
    ofono_stk_driver_register(&binder_stk_driver);
}

void
binder_stk_cleanup()
{
    ofono_stk_driver_unregister(&binder_stk_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
