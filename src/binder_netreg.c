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

#include "binder_modem.h"
#include "binder_netreg.h"
#include "binder_network.h"
#include "binder_util.h"
#include "binder_log.h"

#include <ofono/watch.h>
#include <ofono/sim.h>
#include <ofono/gprs-provision.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#define REGISTRATION_MAX_RETRIES (2)
#define OPERATOR_LIST_TIMEOUT_MS (300000) /* 5 min */

enum binder_netreg_radio_ind {
    IND_NITZ_TIME_RECEIVED,
    IND_SIGNAL_STRENGTH,
    IND_SIGNAL_STRENGTH_1_2,
    IND_SIGNAL_STRENGTH_1_4,
    IND_COUNT
};

enum binder_netreg_network_events {
    NETREG_NETWORK_EVENT_OPERATOR_CHANGED,
    NETREG_NETWORK_EVENT_VOICE_STATE_CHANGED,
    NETREG_NETWORK_EVENT_COUNT
};

typedef struct binder_netreg {
    RadioClient* client;
    struct ofono_watch* watch;
    struct ofono_netreg* netreg;
    BinderNetwork* network;
    gboolean replace_strange_oper;
    int signal_strength_dbm_weak;
    int signal_strength_dbm_strong;
    int network_selection_timeout_ms;
    RadioRequest* list_req;
    RadioRequest* register_req;
    RadioRequest* strength_req;
    char* log_prefix;
    guint init_id;
    guint notify_id;
    guint current_operator_id;
    gulong ind_id[IND_COUNT];
    gulong network_event_id[NETREG_NETWORK_EVENT_COUNT];
} BinderNetReg;

typedef struct binder_netreg_cbd {
    BinderNetReg* self;
    union {
        ofono_netreg_status_cb_t status;
        ofono_netreg_operator_cb_t operator;
        ofono_netreg_operator_list_cb_t operator_list;
        ofono_netreg_register_cb_t reg;
        ofono_netreg_strength_cb_t strength;
        BinderCallback f;
    } cb;
    gpointer data;
} BinderNetRegCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderNetReg* binder_netreg_get_data(struct ofono_netreg *ofono)
    { return ofono ? ofono_netreg_get_data(ofono) : NULL; }

static
BinderNetRegCbData*
binder_netreg_cbd_new(
    BinderNetReg* self,
    BinderCallback cb,
    void* data)
{
    BinderNetRegCbData* cbd = g_slice_new0(BinderNetRegCbData);

    cbd->self = self;
    cbd->cb.f = cb;
    cbd->data = data;
    return cbd;
}

static
void
binder_netreg_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderNetRegCbData, cbd);
}

static
int
binder_netreg_check_status(
    BinderNetReg* self,
    int status)
{
    return (self && self->netreg) ?
        binder_netreg_check_if_really_roaming(self->netreg, status) :
        status;
}

static
gboolean
binder_netreg_status_notify_cb(
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    const BinderRegistrationState* reg = &self->network->voice;

    DBG_(self, "");
    GASSERT(self->notify_id);
    self->notify_id = 0;

    ofono_netreg_status_notify(self->netreg,
        binder_netreg_check_status(self, reg->status),
        reg->lac, reg->ci, reg->access_tech);
    return G_SOURCE_REMOVE;
}

static
void
binder_netreg_status_notify(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    void* user_data)
{
    BinderNetReg* self = user_data;

    /* Coalesce multiple notifications into one */
    if (self->notify_id) {
        DBG_(self, "notification aready queued");
    } else {
        DBG_(self, "queuing notification");
        self->notify_id = g_idle_add(binder_netreg_status_notify_cb, self);
    }
}

static
void
binder_netreg_registration_status(
    struct ofono_netreg* netreg,
    ofono_netreg_status_cb_t cb,
    void* data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);
    const BinderRegistrationState* reg = &self->network->voice;
    struct ofono_error error;

    DBG_(self, "");
    cb(binder_error_ok(&error), binder_netreg_check_status(self, reg->status),
        reg->lac, reg->ci, reg->access_tech, data);
}

static
gboolean
binder_netreg_current_operator_cb(
    gpointer user_data)
{
    BinderNetRegCbData* cbd = user_data;
    BinderNetReg* self = cbd->self;
    ofono_netreg_operator_cb_t cb = cbd->cb.operator;
    struct ofono_error error;

    DBG_(self, "");
    GASSERT(self->current_operator_id);
    self->current_operator_id = 0;

    cb(binder_error_ok(&error), self->network->operator, cbd->data);
    return G_SOURCE_REMOVE;
}

static
void
binder_netreg_current_operator(
    struct ofono_netreg* netreg,
    ofono_netreg_operator_cb_t cb,
    void *data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);

    /*
     * Calling ofono_netreg_status_notify() may result in
     * binder_netreg_current_operator() being invoked even if one
     * is already pending. Since ofono core doesn't associate
     * any context with individual calls, we can safely assume
     * that such a call essentially cancels the previous one.
     */
    if (self->current_operator_id) {
        g_source_remove(self->current_operator_id);
    }

    self->current_operator_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
        binder_netreg_current_operator_cb,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data),
        binder_netreg_cbd_free);
}

static
gboolean
binder_netreg_strange(
    const struct ofono_network_operator* op,
    struct ofono_sim* sim)
{
    gsize mcclen;

    if (sim && op->status != OFONO_OPERATOR_STATUS_CURRENT) {
        const char* spn = ofono_sim_get_spn(sim);
        const char* mcc = ofono_sim_get_mcc(sim);
        const char* mnc = ofono_sim_get_mnc(sim);

        if (spn && mcc && mnc && !strcmp(op->name, spn) &&
            (strcmp(op->mcc, mcc) || strcmp(op->mnc, mnc))) {
            /*
             * Status is not "current", SPN matches the SIM, but
             * MCC and/or MNC don't (e.g. Sony Xperia X where all
             * operators could be reported with the same name
             * which equals SPN).
             */
            DBG("%s %s%s (sim spn?)", op->name, op->mcc, op->mnc);
            return TRUE;
        }
    }

    mcclen = strlen(op->mcc);
    if (!strncmp(op->name, op->mcc, mcclen) &&
        !strcmp(op->name + mcclen, op->mnc)) {
        /* Some MediaTek modems only report numeric operator name */
        DBG("%s %s%s (numeric?)", op->name, op->mcc, op->mnc);
        return TRUE;
    }

    return FALSE;
}

static
void
binder_netreg_process_operators(
    BinderNetReg* self,
    struct ofono_network_operator* ops,
    int nops)
{
    if (self->replace_strange_oper) {
        int i;

        for (i = 0; i < nops; i++) {
            struct ofono_network_operator* op = ops + i;
            struct ofono_gprs_provision_data* prov = NULL;
            int np = 0;

            if (binder_netreg_strange(op, self->watch->sim) &&
                ofono_gprs_provision_get_settings(op->mcc, op->mnc,
                NULL, &prov, &np)) {
                /* Use the first entry */
                if (np > 0 && prov->provider_name && prov->provider_name[0]) {
                    DBG("%s %s%s -> %s", op->name, op->mcc, op->mnc,
                        prov->provider_name);
                    g_strlcpy(op->name, prov->provider_name,
                        OFONO_MAX_OPERATOR_NAME_LENGTH);
                }
                ofono_gprs_provision_free_settings(prov, np);
            }
        }
    }
}

static
struct ofono_network_operator*
binder_netreg_convert_oplist(
    const RadioOperatorInfo* ops,
    gsize count,
    enum ofono_access_technology default_tech)
{
    gsize i;
    struct ofono_network_operator* oplist =
        g_new0(struct ofono_network_operator, count);

    for (i = 0; i < count; i++) {
        const RadioOperatorInfo* src = ops + i;
        struct ofono_network_operator* dest = oplist + i;

        /* Try to use long by default */
        if (src->alphaLong.len) {
            g_strlcpy(dest->name, src->alphaLong.data.str,
                OFONO_MAX_OPERATOR_NAME_LENGTH);
        } else if (src->alphaShort.len) {
            g_strlcpy(dest->name, src->alphaShort.data.str,
                OFONO_MAX_OPERATOR_NAME_LENGTH);
        }

        /* Set the proper status  */
        dest->status = OFONO_OPERATOR_STATUS_UNKNOWN;
        switch (src->status) {
        case RADIO_OP_STATUS_UNKNOWN:
            break;
        case RADIO_OP_AVAILABLE:
            dest->status = OFONO_OPERATOR_STATUS_AVAILABLE;
            break;
        case RADIO_OP_CURRENT:
            dest->status = OFONO_OPERATOR_STATUS_CURRENT;
            break;
        case RADIO_OP_FORBIDDEN:
            dest->status = OFONO_OPERATOR_STATUS_FORBIDDEN;
            break;
        }

        dest->tech = default_tech;
        binder_parse_mcc_mnc(src->operatorNumeric.data.str, dest);
        DBG("[operator=%s, %s, %s, %s, %s]",
            dest->name, dest->mcc, dest->mnc,
            binder_ofono_access_technology_string(dest->tech),
            binder_radio_op_status_string(src->status));
    }

    return oplist;
}

static
void
binder_netreg_list_operators_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetRegCbData* cbd = user_data;
    BinderNetReg* self = cbd->self;
    ofono_netreg_operator_list_cb_t cb = cbd->cb.operator_list;
    struct ofono_error err;

    GASSERT(self->list_req == req);
    radio_request_unref(self->list_req);
    self->list_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_AVAILABLE_NETWORKS) {
            if (error == RADIO_ERROR_NONE) {
                 GBinderReader reader;
                 const RadioOperatorInfo* ops;
                 gsize count = 0;

                 /*
                  * getAvailableNetworksResponse(RadioResponseInfo,
                  *   vec<OperatorInfo> networkInfos);
                  */
                 gbinder_reader_copy(&reader, args);
                 ops = gbinder_reader_read_hidl_type_vec(&reader,
                     RadioOperatorInfo, &count);
                 if (ops) {
                     struct ofono_network_operator* list =
                         binder_netreg_convert_oplist(ops, count,
                             self->network->voice.access_tech);

                     /* Success */
                     binder_netreg_process_operators(self, list, count);
                     cb(binder_error_ok(&err), count, list, cbd->data);
                     g_free(list);
                     return;
                 }
            } else {
                ofono_warn("Failed to retrive the list of operators: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getAvailableNetworks response %d", resp);
        }
    }

    /* Error path */
    cb(binder_error_failure(&err), 0, NULL, cbd->data);
}

static
void
binder_netreg_list_operators(
    struct ofono_netreg* netreg,
    ofono_netreg_operator_list_cb_t cb,
    void* data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);
    RadioRequest* req = radio_request_new(self->client,
        RADIO_REQ_GET_AVAILABLE_NETWORKS, NULL,
        binder_netreg_list_operators_cb,
        binder_netreg_cbd_free,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    radio_request_set_timeout(req, OPERATOR_LIST_TIMEOUT_MS);
    radio_request_drop(self->list_req);
    if (radio_request_submit(req)) {
        DBG_(self, "querying available networks");
        self->list_req = req; /* Keep the ref */
    } else {
        struct ofono_error err;

        DBG_(self, "failed to query available networks");
        radio_request_unref(req);
        self->list_req = NULL;
        cb(binder_error_failure(&err), 0, NULL, data);
    }
}

static
void
binder_netreg_register_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetRegCbData* cbd = user_data;
    BinderNetReg* self = cbd->self;
    ofono_netreg_register_cb_t cb = cbd->cb.reg;
    struct ofono_error err;

    GASSERT(self->register_req == req);
    radio_request_unref(self->register_req);
    self->register_req = NULL;

    /*
     * Handles both:
     * setNetworkSelectionModeAutomaticResponse(RadioResponseInfo);
     * setNetworkSelectionModeManualResponse(RadioResponseInfo);
     */
    if (status == RADIO_TX_STATUS_OK) {
        GASSERT(resp == RADIO_RESP_SET_NETWORK_SELECTION_MODE_AUTOMATIC ||
                resp == RADIO_RESP_SET_NETWORK_SELECTION_MODE_MANUAL);
        if (error == RADIO_ERROR_NONE) {
            /* Success */
            cb(binder_error_ok(&err), cbd->data);
            return;
        } else {
            ofono_error("registration failed, error %s",
                binder_radio_error_string(error));
        }
    }

    /* Error path */
    cb(binder_error_failure(&err), cbd->data);
}

static
void
binder_netreg_query_register_auto_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetRegCbData* cbd = user_data;
    BinderNetReg* self = cbd->self;
    ofono_netreg_register_cb_t cb = cbd->cb.reg;
    struct ofono_error err;

    GASSERT(self->register_req == req);
    radio_request_unref(self->register_req);
    self->register_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_NETWORK_SELECTION_MODE) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                gboolean manual;

                /*
                 * getNetworkSelectionModeResponse(RadioResponseInfo,
                 * bool manual);
                 */
                 gbinder_reader_copy(&reader, args);
                 if (gbinder_reader_read_bool(&reader, &manual) && !manual) {
                     ofono_info("nw selection is already auto");
                     cb(binder_error_ok(&err), cbd->data);
                     return;
                 }
            }
        } else {
            ofono_error("Unexpected getNetworkSelectionMode response %d", resp);
        }
    }

    /*
     * Either selection is set to manual, or the query failed.
     * In either case, let's give it a try.
     */
    req = radio_request_new(self->client,
        RADIO_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC, NULL,
        binder_netreg_register_cb, binder_netreg_cbd_free,
        binder_netreg_cbd_new(self, cbd->cb.f, cbd->data));

    /* setNetworkSelectionModeAutomatic(int32 serial); */
    radio_request_set_timeout(req, self->network_selection_timeout_ms);
    radio_request_set_retry(req, 0, REGISTRATION_MAX_RETRIES);
    if (radio_request_submit(req)) {
        ofono_info("%snw select auto", self->log_prefix);
        self->register_req = req; /* Keep the ref */
    } else {
        ofono_warn("%sfailed to select auto nw", self->log_prefix);
        radio_request_unref(req);
        cb(binder_error_failure(&err), cbd->data);
    }
}

static
void
binder_netreg_register_auto(
    struct ofono_netreg* netreg,
    ofono_netreg_register_cb_t cb,
    void* data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);
    RadioRequest* req = radio_request_new(self->client,
        RADIO_REQ_GET_NETWORK_SELECTION_MODE, NULL,
        binder_netreg_query_register_auto_cb,
        binder_netreg_cbd_free,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    /* getNetworkSelectionMode(int32 serial); */
    radio_request_drop(self->register_req);
    if (radio_request_submit(req)) {
        self->register_req = req; /* Keep the ref */
    } else {
        struct ofono_error err;

        DBG_(self, "failed to query bw selection mode");
        radio_request_unref(req);
        self->register_req = NULL;
        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_netreg_register_manual(
    struct ofono_netreg* netreg,
    const char* mcc,
    const char* mnc,
    ofono_netreg_register_cb_t cb,
    void* data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);
    char* numeric = g_strconcat(mcc, mnc, NULL);
    GBinderWriter writer;
    RadioRequest* req = radio_request_new(self->client,
        RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL, &writer,
        binder_netreg_register_cb, binder_netreg_cbd_free,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    /* setNetworkSelectionModeManual(int32 serial, string operatorNumeric); */
    gbinder_writer_add_cleanup(&writer, g_free, numeric);
    gbinder_writer_append_hidl_string(&writer, numeric);
    radio_request_set_timeout(req, self->network_selection_timeout_ms);

    radio_request_drop(self->register_req);
    if (radio_request_submit(req)) {
        ofono_info("%snw select manual: %s", self->log_prefix, numeric);
        self->register_req = req; /* Keep the ref */
    } else {
        struct ofono_error err;

        DBG_(self, "failed to set nw select manual: %s", numeric);
        radio_request_unref(req);
        self->register_req = NULL;
        cb(binder_error_failure(&err), data);
    }
}

/*
 * RSSI
 * Received Signal Strength Indication
 *
 * Reference: 3GPP TS 27.007 section 8.5
 * Range: -51..-113 dBm
 * Valid values are (0-31, 99)
 *
 * 0       -113 dBm or less
 * 1       -111 dBm
 * 2...30  -109... -53 dBm
 * 31      -51 dBm or greater
 * 99      not known or not detectable
 *
 * INT_MAX denotes that the value is invalid/unreported.
 */

#define RSSI_MIN 0
#define RSSI_MAX 31

static
int
binder_netreg_dbm_from_rssi(
    int rssi)
{
    return (rssi >= RSSI_MIN && rssi <= RSSI_MAX) ?
        (-113 + 2 * (rssi - RSSI_MIN)) : -140;
}

/*
 * RSRP
 * Reference Signal Received Power
 *
 * Reference: 3GPP TS 36.133 section 9.1.4
 * Range: -44..-140 dBm
 * Value is dBm multipled by -1
 *
 * INT_MAX denotes that the value is invalid/unreported.
 */

#define RSRP_MIN 44
#define RSRP_MAX 140

static
int
binder_netreg_dbm_from_rsrp(
    int rsrp)
{
    return (rsrp >= RSRP_MIN && rsrp <= RSRP_MAX) ? -rsrp : -140;
}

/*
 * RSCP
 * Received Signal Code Power
 *
 * Reference: 3GPP TS 27.007 section 8.69
 * Range: -24..-120 dBm
 * Valid values are (0-96, 255)
 *
 * 0       -120 dBm or less
 * 1       -119 dBm
 * 2...95  -118...-25 dBm
 * 96      -24 dBm or greater
 * 255     not known or not detectable
 *
 * INT_MAX denotes that the value is invalid/unreported.
 */

#define RSCP_MIN 0
#define RSCP_MAX 96

static
int
binder_netreg_dbm_from_rscp(
    int rscp)
{
    return (rscp >= RSCP_MIN && rscp <= RSCP_MAX) ?
        (-120 + (rscp - RSCP_MIN)) : -140;
}

static
int
binder_netreg_get_signal_strength_dbm(
    const RadioSignalStrengthGsm* gsm,
    const RadioSignalStrengthLte* lte,
    const RadioSignalStrengthWcdma_1_2* wcdma,
    const RadioSignalStrengthTdScdma_1_2* tdscdma)
{
    int rssi = -1, rscp = -1, rsrp = -1;

    if (gsm->signalStrength <= RSSI_MAX) {
        rssi = gsm->signalStrength;
    }

    if (lte->signalStrength <= RSSI_MAX &&
        (int)lte->signalStrength > rssi) {
        rssi = lte->signalStrength;
    }

    if (lte->rsrp >= RSRP_MIN && lte->rsrp <= RSRP_MAX) {
        rsrp = lte->rsrp;
    }

    if (wcdma) {
        if (wcdma->base.signalStrength <= RSSI_MAX &&
            (int)wcdma->base.signalStrength > rssi) {
            rssi = wcdma->base.signalStrength;
        }
        if (wcdma->rscp <= RSCP_MAX) {
            rscp = wcdma->rscp;
        }
    }

    if (tdscdma) {
        if (tdscdma->signalStrength <= RSSI_MAX &&
            (int)tdscdma->signalStrength > rssi) {
            rssi = tdscdma->signalStrength;
        }
        if (tdscdma->rscp <= RSCP_MAX &&
            (int)tdscdma->rscp > rscp) {
            rscp = tdscdma->rscp;
        }
    }

    if (rssi >= RSCP_MIN) {
        return binder_netreg_dbm_from_rssi(rssi);
    } else if (rscp >= RSCP_MIN) {
        return binder_netreg_dbm_from_rscp(rssi);
    } else if (rsrp >= RSRP_MIN) {
        return binder_netreg_dbm_from_rsrp(rssi);
    } else {
        return -140;
    }
}

static
int
binder_netreg_percent_from_dbm(
    BinderNetReg* self,
    int dbm)
{
    const int min_dbm = self->signal_strength_dbm_weak;    /* dBm */
    const int max_dbm = self->signal_strength_dbm_strong;  /* dBm */

    return (dbm <= min_dbm) ? 1 :
        (dbm >= max_dbm) ? 100 :
        (100 * (dbm - min_dbm) / (max_dbm - min_dbm));
}

static
void
binder_netreg_strength_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    GBinderReader reader;
    int dbm = 0;

    gbinder_reader_copy(&reader, args);
    if (code == RADIO_IND_CURRENT_SIGNAL_STRENGTH) {
        const RadioSignalStrength* ss = gbinder_reader_read_hidl_struct
            (&reader, RadioSignalStrength);

        if (ss) {
            dbm = binder_netreg_get_signal_strength_dbm
                (&ss->gw, &ss->lte, NULL, NULL);
        }
    } else if (code == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_2) {
        const RadioSignalStrength_1_2* ss = gbinder_reader_read_hidl_struct
            (&reader, RadioSignalStrength_1_2);

        if (ss) {
            dbm = binder_netreg_get_signal_strength_dbm
                (&ss->gw, &ss->lte, &ss->wcdma, NULL);
        }
    } else if (code == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4) {
        const RadioSignalStrength_1_4* ss = gbinder_reader_read_hidl_struct
            (&reader, RadioSignalStrength_1_4);

        if (ss) {
            dbm = binder_netreg_get_signal_strength_dbm
                (&ss->gsm, &ss->lte, &ss->wcdma, &ss->tdscdma);
        }
    }

    if (dbm) {
        const int percent = binder_netreg_percent_from_dbm(self, dbm);

        DBG_(self, "%d dBm (%d%%)", dbm, percent);
        ofono_netreg_strength_notify(self->netreg, percent);
    }
}

static void binder_netreg_strength_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetRegCbData* cbd = user_data;
    BinderNetReg* self = cbd->self;
    ofono_netreg_strength_cb_t cb = cbd->cb.strength;
    struct ofono_error err;

    GASSERT(self->strength_req == req);
    radio_request_unref(self->strength_req);
    self->strength_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            GBinderReader reader;
            int dbm = 0;

            gbinder_reader_copy(&reader, args);
            if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH) {
                const RadioSignalStrength* ss =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioSignalStrength);

                if (ss) {
                    dbm = binder_netreg_get_signal_strength_dbm
                        (&ss->gw, &ss->lte, NULL, NULL);
                }
            } else if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH_1_2) {
                const RadioSignalStrength_1_2* ss =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioSignalStrength_1_2);

                if (ss) {
                    dbm = binder_netreg_get_signal_strength_dbm
                        (&ss->gw, &ss->lte, &ss->wcdma, NULL);
                }
            } else if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH_1_4) {
                const RadioSignalStrength_1_4* ss =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioSignalStrength_1_4);

                if (ss) {
                    dbm = binder_netreg_get_signal_strength_dbm
                        (&ss->gsm, &ss->lte, &ss->wcdma, &ss->tdscdma);
                }
            } else {
                ofono_error("Unexpected getSignalStrength response %d", resp);
            }

            if (dbm) {
                const int percent = binder_netreg_percent_from_dbm(self, dbm);

                /* Success */
                DBG_(self, "%d dBm (%d%%)", dbm, percent);
                cb(binder_error_ok(&err), percent, cbd->data);
                return;
            }
        } else {
            ofono_warn("Failed to retrive the signal strength: %s",
                binder_radio_error_string(status));
        }
    }

    /* Error path */
    cb(binder_error_failure(&err), -1, cbd->data);
}

static
void
binder_netreg_strength(
    struct ofono_netreg* netreg,
    ofono_netreg_strength_cb_t cb,
    void* data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);
    RadioRequest* req = radio_request_new(self->client,
        (radio_client_interface(self->client) >= RADIO_INTERFACE_1_4) ?
        RADIO_REQ_GET_SIGNAL_STRENGTH_1_4 : RADIO_REQ_GET_SIGNAL_STRENGTH,
        NULL, binder_netreg_strength_cb, binder_netreg_cbd_free,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    radio_request_set_retry(req, BINDER_RETRY_MS, -1);
    radio_request_drop(self->strength_req);
    if (radio_request_submit(req)) {
        self->strength_req = req; /* Keep the ref */
    } else {
        struct ofono_error err;

        DBG_(self, "failed to query signal strength");
        radio_request_unref(req);
        self->strength_req = NULL;
        cb(binder_error_failure(&err), -1, data);
    }
}

static
void
binder_netreg_nitz_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    GBinderReader reader;
    int year, mon, mday, hour, min, sec, tzi, dst = 0;
    char tzs;
    const char* nitz;

    /*
     * nitzTimeReceived(RadioIndicationType, string nitzTime,
     * uint64 receivedTime);
     */
    GASSERT(code == RADIO_IND_NITZ_TIME_RECEIVED);
    gbinder_reader_copy(&reader, args);
    nitz = gbinder_reader_read_hidl_string_c(&reader);

    DBG_(self, "%s", nitz);

    /*
     * Format: yy/mm/dd,hh:mm:ss(+/-)tz[,ds]
     * The ds part is considered optional, initialized to zero.
     */
    if (nitz && sscanf(nitz, "%u/%u/%u,%u:%u:%u%c%u,%u", &year, &mon, &mday,
        &hour, &min, &sec, &tzs, &tzi, &dst) >= 8 &&
        (tzs == '+' || tzs == '-')) {
        struct ofono_network_time time;
        char tz[4];

        snprintf(tz, sizeof(tz), "%c%d", tzs, tzi);
        time.sec = sec;
        time.min = min;
        time.hour = hour;
        time.mday = mday;
        time.mon = mon;
        time.year = 2000 + year;
        time.dst = dst;
        time.utcoff = atoi(tz) * 15 * 60;

        ofono_netreg_time_notify(self->netreg, &time);
    } else {
        ofono_warn("Failed to parse NITZ string \"%s\"", nitz);
    }
}

static
gboolean
binder_netreg_register(
    gpointer user_data)
{
    BinderNetReg* self = user_data;

    GASSERT(self->init_id);
    self->init_id = 0;
    ofono_netreg_register(self->netreg);

    /* Register for network state changes */
    self->network_event_id[NETREG_NETWORK_EVENT_OPERATOR_CHANGED] =
        binder_network_add_property_handler(self->network,
            BINDER_NETWORK_PROPERTY_OPERATOR,
            binder_netreg_status_notify, self);
    self->network_event_id[NETREG_NETWORK_EVENT_VOICE_STATE_CHANGED] =
        binder_network_add_property_handler(self->network,
            BINDER_NETWORK_PROPERTY_VOICE_STATE,
            binder_netreg_status_notify, self);

    /* Register for network time updates */
    self->ind_id[IND_NITZ_TIME_RECEIVED] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_NITZ_TIME_RECEIVED,
            binder_netreg_nitz_notify, self);

    /* Register for signal strength changes */
    self->ind_id[IND_SIGNAL_STRENGTH] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_CURRENT_SIGNAL_STRENGTH,
            binder_netreg_strength_notify, self);
    self->ind_id[IND_SIGNAL_STRENGTH_1_2] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_2,
            binder_netreg_strength_notify, self);
    self->ind_id[IND_SIGNAL_STRENGTH_1_4] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4,
            binder_netreg_strength_notify, self);

    return G_SOURCE_REMOVE;
}

static
int
binder_netreg_probe(
    struct ofono_netreg* netreg,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderNetReg* self = g_new0(BinderNetReg, 1);
    const BinderSlotConfig* config = &modem->config;

    self->log_prefix = binder_dup_prefix(modem->log_prefix);

    DBG_(self, "%p", netreg);
    self->client = radio_client_ref(modem->client);
    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->network = binder_network_ref(modem->network);
    self->netreg = netreg;
    self->replace_strange_oper = config->replace_strange_oper;
    self->signal_strength_dbm_weak = config->signal_strength_dbm_weak;
    self->signal_strength_dbm_strong = config->signal_strength_dbm_strong;
    self->network_selection_timeout_ms = config->network_selection_timeout_ms;

    ofono_netreg_set_data(netreg, self);
    self->init_id = g_idle_add(binder_netreg_register, self);
    return 0;
}

static
void
binder_netreg_remove(
    struct ofono_netreg* netreg)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);

    DBG_(self, "%p", netreg);

    if (self->init_id) {
        g_source_remove(self->init_id);
    }

    if (self->notify_id) {
        g_source_remove(self->notify_id);
    }

    if (self->current_operator_id) {
        g_source_remove(self->current_operator_id);
    }

    radio_request_drop(self->list_req);
    radio_request_drop(self->register_req);
    radio_request_drop(self->strength_req);

    ofono_watch_unref(self->watch);
    binder_network_remove_all_handlers(self->network, self->network_event_id);
    binder_network_unref(self->network);

    radio_client_remove_all_handlers(self->client, self->ind_id);
    radio_client_unref(self->client);

    g_free(self->log_prefix);
    g_free(self);

    ofono_netreg_set_data(netreg, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_netreg_driver binder_netreg_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_netreg_probe,
    .remove                 = binder_netreg_remove,
    .registration_status    = binder_netreg_registration_status,
    .current_operator       = binder_netreg_current_operator,
    .list_operators         = binder_netreg_list_operators,
    .register_auto          = binder_netreg_register_auto,
    .register_manual        = binder_netreg_register_manual,
    .strength               = binder_netreg_strength
};

void
binder_netreg_init()
{
    ofono_netreg_driver_register(&binder_netreg_driver);
}

void
binder_netreg_cleanup()
{
    ofono_netreg_driver_unregister(&binder_netreg_driver);
}

enum ofono_netreg_status
binder_netreg_check_if_really_roaming(
    struct ofono_netreg* netreg,
    enum ofono_netreg_status status)
{
    if (status == OFONO_NETREG_STATUS_ROAMING && netreg) {
        /* These functions tolerate NULL argument */
        const char* net_mcc = ofono_netreg_get_mcc(netreg);
        const char* net_mnc = ofono_netreg_get_mnc(netreg);

        if (ofono_netreg_spdi_lookup(netreg, net_mcc, net_mnc)) {
            ofono_info("not roaming based on spdi");
            return OFONO_NETREG_STATUS_REGISTERED;
        }
    }
    return status;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
