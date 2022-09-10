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
#include "binder_oplist.h"
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

#include <gutil_macros.h>

#define REGISTRATION_MAX_RETRIES (2)
#define NETWORK_SCAN_TIMEOUT_SEC (60) /* 1 min */
#define OPERATOR_LIST_TIMEOUT_SEC (300) /* 5 min */
#define OPERATOR_LIST_TIMEOUT_MS (OPERATOR_LIST_TIMEOUT_SEC * 1000)

typedef struct binder_netreg_scan BinderNetRegScan;

enum binder_netreg_radio_ind {
    IND_NITZ_TIME_RECEIVED,
    IND_SIGNAL_STRENGTH,
    IND_SIGNAL_STRENGTH_1_2,
    IND_SIGNAL_STRENGTH_1_4,
    IND_NETWORK_SCAN_RESULT_1_2,
    IND_NETWORK_SCAN_RESULT_1_4,
    IND_NETWORK_SCAN_RESULT_1_5,
    IND_MODEM_RESET,
    IND_COUNT
};

enum binder_netreg_network_events {
    NETREG_NETWORK_EVENT_DATA_STATE_CHANGED,
    NETREG_NETWORK_EVENT_VOICE_STATE_CHANGED,
    NETREG_NETWORK_EVENT_COUNT
};

typedef struct binder_netreg {
    RadioClient* client;
    struct ofono_watch* watch;
    struct ofono_netreg* netreg;
    BinderNetwork* network;
    BinderRegistrationState reg_state;
    enum ofono_radio_access_mode techs;
    gboolean use_network_scan;
    gboolean replace_strange_oper;
    int signal_strength_dbm_weak;
    int signal_strength_dbm_strong;
    int network_selection_timeout_ms;
    RadioRequest* register_req;
    RadioRequest* strength_req;
    char* log_prefix;
    guint init_id;
    guint notify_id;
    guint current_operator_id;
    BinderNetRegScan* scan;
    gulong ind_id[IND_COUNT];
    gulong network_event_id[NETREG_NETWORK_EVENT_COUNT];
} BinderNetReg;

typedef struct binder_netreg_cbd {
    BinderNetReg* self;
    union {
        ofono_netreg_status_cb_t status;
        ofono_netreg_operator_cb_t operator;
        ofono_netreg_register_cb_t reg;
        ofono_netreg_strength_cb_t strength;
        BinderCallback f;
    } cb;
    gpointer data;
} BinderNetRegCbData;

struct binder_netreg_scan {
    RadioRequest* req;
    BinderOpList* oplist;
    ofono_netreg_operator_list_cb_t cb;
    gpointer data;
    gboolean stop; /* startNetworkScan succeeded */
    guint timeout_id;
};

typedef struct binder_netreg_radio_type {
    enum ofono_radio_access_mode mode;
    RADIO_ACCESS_NETWORKS ran;
    RADIO_NETWORK_SCAN_SPECIFIER_1_5_TYPE spec_1_5_type;
} BinderNetRegRadioType;

static const BinderNetRegRadioType binder_netreg_radio_types[] = {
    {
         OFONO_RADIO_ACCESS_MODE_GSM,
         RADIO_ACCESS_NETWORKS_GERAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_GERAN
    },{
         OFONO_RADIO_ACCESS_MODE_UMTS,
         RADIO_ACCESS_NETWORKS_UTRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_UTRAN
    },{
         OFONO_RADIO_ACCESS_MODE_LTE,
         RADIO_ACCESS_NETWORKS_EUTRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_EUTRAN
    }
};

static const BinderNetRegRadioType binder_netreg_radio_types_1_5[] = {
    {
         OFONO_RADIO_ACCESS_MODE_GSM,
         RADIO_ACCESS_NETWORKS_GERAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_GERAN
    },{
         OFONO_RADIO_ACCESS_MODE_UMTS,
         RADIO_ACCESS_NETWORKS_UTRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_UTRAN
    },{
         OFONO_RADIO_ACCESS_MODE_LTE,
         RADIO_ACCESS_NETWORKS_EUTRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_EUTRAN
    },{
         OFONO_RADIO_ACCESS_MODE_NR,
         RADIO_ACCESS_NETWORKS_NGRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_NGRAN
    }
};

#define N_RADIO_TYPES G_N_ELEMENTS(binder_netreg_radio_types)
#define N_RADIO_TYPES_1_5 G_N_ELEMENTS(binder_netreg_radio_types_1_5)
G_STATIC_ASSERT(N_RADIO_TYPES == (OFONO_RADIO_ACCESS_MODE_COUNT - 1));
G_STATIC_ASSERT(N_RADIO_TYPES_1_5 == OFONO_RADIO_ACCESS_MODE_COUNT);

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
    BinderNetRegCbData* cbd)
{
    gutil_slice_free(cbd);
}

#define binder_netreg_cbd_destroy ((GDestroyNotify)binder_netreg_cbd_free)

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
    BinderNetwork* network = self->network;
    BinderRegistrationState* current = &self->reg_state;
    const BinderRegistrationState* data = &network->data;

    /*
     * Use data registration state if we are registered for data.
     * Data connectivity makes perfect sense without voice, VoLTE
     * may also work without voice registration. In that sense,
     * data registration is even more functional than voice. In
     * any case, if we have any sort of registration, we have to
     * report that to the ofono core.
     */
    const BinderRegistrationState* reg =
        (data->status == OFONO_NETREG_STATUS_REGISTERED ||
         data->status == OFONO_NETREG_STATUS_ROAMING) ? data :
        &network->voice;
    const enum ofono_netreg_status reg_status =
        binder_netreg_check_status(self, reg->status);

    GASSERT(self->notify_id);
    self->notify_id = 0;

    if (current->status != reg_status ||
        current->access_tech != reg->access_tech ||
        current->lac != reg->lac ||
        current->ci != reg->ci) {
        /* Registration state has changed */
        current->status = reg_status;
        current->access_tech = reg->access_tech;
        current->lac = reg->lac;
        current->ci = reg->ci;
        ofono_netreg_status_notify(self->netreg, current->status,
            current->lac, current->ci, current->access_tech);
    }

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
        DBG_(self, "notification already queued");
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
        binder_netreg_cbd_destroy);
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
    BinderOpList* oplist)
{
    if (self->replace_strange_oper && oplist) {
        guint i;

        for (i = 0; i < oplist->count; i++) {
            struct ofono_network_operator* op = oplist->op + i;
            struct ofono_gprs_provision_data* prov = NULL;
            int np = 0;

            if (binder_netreg_strange(op, self->watch->sim) &&
                ofono_gprs_provision_get_settings(op->mcc, op->mnc,
                NULL, &prov, &np)) {
                /* Use the first entry */
                if (np > 0 && prov->provider_name && prov->provider_name[0]) {
                    DBG("%s %s%s -> %s", op->name, op->mcc, op->mnc,
                        prov->provider_name);
                    g_strlcpy(op->name, prov->provider_name, sizeof(op->name));
                }
                ofono_gprs_provision_free_settings(prov, np);
            }
        }
    }
}

static
BinderNetRegScan*
binder_netreg_scan_new(
    ofono_netreg_operator_list_cb_t cb,
    void* data)
{
    BinderNetRegScan* scan = g_slice_new0(BinderNetRegScan);

    scan->cb = cb;
    scan->data = data;
    return scan;
}

static
void
binder_netreg_scan_free(
    BinderNetReg* self,
    BinderNetRegScan* scan)
{
    if (scan) {
        if (scan->cb) {
            struct ofono_error err;

            scan->cb(binder_error_failure(&err), 0, NULL, scan->data);
        }
        if (scan->timeout_id) {
            g_source_remove(scan->timeout_id);
        }
        if (scan->stop) {
            RadioRequest* req = radio_request_new(self->client,
                RADIO_REQ_STOP_NETWORK_SCAN, NULL, NULL, NULL, NULL);

            radio_request_submit(req);
            radio_request_unref(req);
        }
        binder_oplist_free(scan->oplist);
        radio_request_drop(scan->req);
        gutil_slice_free(scan);
    }
}

static
void
binder_netreg_scan_complete(
    BinderNetReg* self,
    BinderNetRegScan* scan)
{
    if (scan) {
        if (scan->cb) {
            struct ofono_error ok;
            ofono_netreg_operator_list_cb_t cb = scan->cb;

            scan->cb = NULL;
            binder_error_init_ok(&ok);
            if (scan->oplist) {
                BinderOpList* oplist = scan->oplist;

                scan->oplist = NULL;
                binder_netreg_process_operators(self, oplist);
                cb(&ok, oplist->count, oplist->op, scan->data);
                binder_oplist_free(oplist);
            } else {
                cb(&ok, 0, NULL, scan->data);
            }
        }
        binder_netreg_scan_free(self, scan);
    }
}

static
void
binder_netreg_scan_drop(
    BinderNetReg* self,
    BinderNetRegScan* scan)
{
    if (scan) {
        scan->cb = NULL;
        binder_netreg_scan_free(self, scan);
    }
}

static
BinderOpList*
binder_netreg_oplist_fill(
    BinderOpList* oplist,
    const RadioOperatorInfo* ops,
    gsize count,
    enum ofono_access_technology default_tech)
{
    guint i;

    /* This allocates the list if necessary */
    oplist = binder_oplist_set_count(oplist, count);
    for (i = 0; i < count; i++) {
        const RadioOperatorInfo* src = ops + i;
        struct ofono_network_operator* dest = oplist->op + i;

        /* Try to use long by default */
        if (src->alphaLong.len) {
            g_strlcpy(dest->name, src->alphaLong.data.str, sizeof(dest->name));
        } else if (src->alphaShort.len) {
            g_strlcpy(dest->name, src->alphaShort.data.str, sizeof(dest->name));
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
gboolean
binder_netreg_scan_timeoult_cb(
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    BinderNetRegScan* scan = self->scan;

    /* Timeout gets cancelled when scan is done, no need to check for NULL */
    scan->timeout_id = 0;
    self->scan = NULL;
    DBG_(self, "network scan timed out");
    binder_netreg_scan_complete(self, scan);
    return G_SOURCE_REMOVE;
}

static
void
binder_netreg_start_scan_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    BinderNetRegScan* scan = self->scan;

    /*
     * Scan must be non-NULL because when it gets deallocated,
     * its request is dropped and the completion callback (like
     * this one) won't be invoked.
     */
    GASSERT(scan && scan->req == req);
    radio_request_unref(scan->req);
    scan->req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            /* Keep BinderNetRegScan alive */
            DBG_(self, "network scan started");
            return;
        } else {
            ofono_warn("Failed to start network scan: %s",
                binder_radio_error_string(error));
        }
    }

    /* Error path */
    scan->stop = FALSE;
    self->scan = NULL;
    binder_netreg_scan_free(self, scan);
}

static
void
binder_netreg_start_network_scan(
    BinderNetReg* self)
{
    /*
     * Caller has checked that a) self->scan exists and b) radio
     * interface version is >= 1.2
     */
    BinderNetRegScan* scan = self->scan;
    const RADIO_INTERFACE iface = radio_client_interface(self->client);

    /*
     * startNetworkScan(serial, NetworkScanRequest) <= we don't use that
     * startNetworkScan_1_2(serial, NetworkScanRequest_1_2)
     * startNetworkScan_1_4(serial, NetworkScanRequest_1_2)
     * startNetworkScan_1_5(serial, NetworkScanRequest_1_5)
     */
    const RADIO_REQ req_code =
        (iface < RADIO_INTERFACE_1_4) ? RADIO_REQ_START_NETWORK_SCAN_1_2 :
        (iface < RADIO_INTERFACE_1_5) ? RADIO_REQ_START_NETWORK_SCAN_1_4 :
        RADIO_REQ_START_NETWORK_SCAN_1_5;
    GBinderWriter writer;
    guint i, nspecs = 0;

    scan->stop = TRUE; /* Assume that startNetworkScan succeeds */
    scan->timeout_id = g_timeout_add_seconds(NETWORK_SCAN_TIMEOUT_SEC,
        binder_netreg_scan_timeoult_cb, self);
    scan->req = radio_request_new(self->client, req_code, &writer,
        binder_netreg_start_scan_cb, NULL, self);

    /* Write the arguments */
    if (iface <= RADIO_INTERFACE_1_4) {
        /*
         * typedef struct radio_network_scan_specifier {
         *     RADIO_ACCESS_NETWORKS radioAccessNetwork;
         *     GBinderHidlVec geranBands; // vec<RADIO_GERAN_BAND>
         *     GBinderHidlVec utranBands; // vec<RADIO_UTRAN_BAND>
         *     GBinderHidlVec eutranBands; // vec<RADIO_EUTRAN_BAND>
         *     GBinderHidlVec channels; // vec<int32_t>
         * } RadioAccessSpecifier;
         */
        static const GBinderWriterField radio_network_scan_specifier_f[] = {
           GBINDER_WRITER_FIELD_HIDL_VEC_INT32
               (RadioAccessSpecifier, geranBands),
           GBINDER_WRITER_FIELD_HIDL_VEC_INT32
               (RadioAccessSpecifier, utranBands),
           GBINDER_WRITER_FIELD_HIDL_VEC_INT32
               (RadioAccessSpecifier, eutranBands),
           GBINDER_WRITER_FIELD_HIDL_VEC_INT32
               (RadioAccessSpecifier, channels),
           GBINDER_WRITER_FIELD_END()
        };

        static const GBinderWriterType radio_network_scan_specifier_t = {
            GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioAccessSpecifier),
            radio_network_scan_specifier_f
        };

        /*
         * typedef struct radio_network_scan_request {
         *     RADIO_SCAN_TYPE type;
         *     gint32 interval;           // [5..300] seconds
         *     GBinderHidlVec specifiers; // vec <RadioAccessSpecifier>
         *     gint32 maxSearchTime;      // [60..3600] seconds
         *     guint8 incrementalResults; // TRUE/FALSE
         *     gint32 incrementalResultsPeriodicity; // [1..10]
         *     GBinderHidlVec mccMncs;    // vec<hidl_string>
         * } RadioNetworkScanRequest_1_2;
         */
        static const GBinderWriterField radio_network_scan_request_f[] = {
            GBINDER_WRITER_FIELD_HIDL_VEC
                (RadioNetworkScanRequest_1_2, specifiers,
                    &radio_network_scan_specifier_t),
            GBINDER_WRITER_FIELD_HIDL_VEC_STRING
                (RadioNetworkScanRequest_1_2, mccMncs),
            GBINDER_WRITER_FIELD_END()
        };
        static const GBinderWriterType radio_network_scan_request_t = {
            GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioNetworkScanRequest_1_2),
            radio_network_scan_request_f
        };

        RadioNetworkScanRequest_1_2* scan = gbinder_writer_new0(&writer,
            RadioNetworkScanRequest_1_2);
        RadioAccessSpecifier* specs = gbinder_writer_malloc0(&writer,
            nspecs * sizeof(*specs));

        /* Which modes are supported and enabled */
        const BinderNetRegRadioType* radio_types[N_RADIO_TYPES];

        for (i = 0; i < N_RADIO_TYPES; i++) {
            if (self->techs & binder_netreg_radio_types[i].mode) {
                radio_types[nspecs++] = binder_netreg_radio_types + i;
            }
        }

        for (i = 0; i < nspecs; i++) {
            const BinderNetRegRadioType* radio_type = radio_types[i];
            RadioAccessSpecifier* spec = specs + i;

            spec->radioAccessNetwork = radio_type->ran;
            /* The rest may (hopefully) remain zero-initialized */
        }
        scan->type = RADIO_SCAN_ONE_SHOT;
        scan->interval = 10;
        scan->specifiers.owns_buffer = TRUE;
        scan->specifiers.count = nspecs;
        scan->specifiers.data.ptr = specs;
        scan->incrementalResults = TRUE;
        scan->incrementalResultsPeriodicity = 3;
        gbinder_writer_append_struct(&writer, scan,
            &radio_network_scan_request_t, NULL);
    } else {
        /*
         * typedef struct radio_network_scan_specifier_1_5 {
         *     RADIO_ACCESS_NETWORKS radioAccessNetwork;
         *     guint8 type;
         *     GBinderHidlVec bands;    // vec<enum>
         *     GBinderHidlVec channels; // vec<int32_t>
         * } RadioAccessSpecifier_1_5;
         */
        static const GBinderWriterField radio_network_scan_specifier_1_5_f[] = {
            GBINDER_WRITER_FIELD_HIDL_VEC_INT32
                (RadioAccessSpecifier_1_5, bands),
            GBINDER_WRITER_FIELD_HIDL_VEC_INT32
                (RadioAccessSpecifier_1_5, channels),
            GBINDER_WRITER_FIELD_END()
        };
        static const GBinderWriterType radio_network_scan_specifier_1_5_t = {
            GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioAccessSpecifier_1_5),
            radio_network_scan_specifier_1_5_f
        };

        /*
         * typedef struct radio_network_scan_request {
         *     RADIO_SCAN_TYPE type;
         *     gint32 interval;           // [5..300] seconds
         *     GBinderHidlVec specifiers; // vec <RadioAccessSpecifier>
         *     gint32 maxSearchTime;      // [60..3600] seconds
         *     guint8 incrementalResults; // TRUE/FALSE
         *     gint32 incrementalResultsPeriodicity; // [1..10]
         *     GBinderHidlVec mccMncs;    // vec<hidl_string>
         * } RadioNetworkScanRequest_1_5;
         */
        static const GBinderWriterField radio_network_scan_request_1_5_f[] = {
            GBINDER_WRITER_FIELD_HIDL_VEC
                (RadioNetworkScanRequest_1_5, specifiers,
                    &radio_network_scan_specifier_1_5_t),
            GBINDER_WRITER_FIELD_HIDL_VEC_STRING
                (RadioNetworkScanRequest_1_5, mccMncs),
            GBINDER_WRITER_FIELD_END()
        };
        static const GBinderWriterType radio_network_scan_request_1_5_t = {
            GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioNetworkScanRequest_1_5),
            radio_network_scan_request_1_5_f
        };

        RadioNetworkScanRequest_1_5* scan = gbinder_writer_new0(&writer,
            RadioNetworkScanRequest_1_5);
        RadioAccessSpecifier_1_5* specs = gbinder_writer_malloc0(&writer,
            nspecs * sizeof(*specs));

        /* Which modes are supported and enabled */
        const BinderNetRegRadioType* radio_types[N_RADIO_TYPES_1_5];

        for (i = 0; i < N_RADIO_TYPES_1_5; i++) {
            if (self->techs & binder_netreg_radio_types_1_5[i].mode) {
                radio_types[nspecs++] = binder_netreg_radio_types + i;
            }
        }

        for (i = 0; i < nspecs; i++) {
            const BinderNetRegRadioType* radio_type = radio_types[i];
            RadioAccessSpecifier_1_5* spec = specs + i;

            specs[i].radioAccessNetwork = radio_type->ran;
            spec->type = radio_type->spec_1_5_type;
            /* The rest may (hopefully) remain zero-initialized */
        }
        scan->type = RADIO_SCAN_ONE_SHOT;
        scan->interval = 10;
        scan->specifiers.owns_buffer = TRUE;
        scan->specifiers.count = nspecs;
        scan->specifiers.data.ptr = specs;
        scan->maxSearchTime = 60;
        scan->incrementalResults = TRUE;
        scan->incrementalResultsPeriodicity = 3;
        gbinder_writer_append_struct(&writer, scan,
            &radio_network_scan_request_1_5_t, NULL);
    }

    /* Submit the request */
    if (radio_request_submit(scan->req)) {
        DBG_(self, "querying available networks");
    } else {
        DBG_(self, "failed to query available networks");
        self->scan = NULL;
        binder_netreg_scan_free(self, scan);
    }
}

static
void
binder_netreg_get_available_networks_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    BinderNetRegScan* scan = self->scan;

    /*
     * Scan must be non-NULL because when it gets deallocated,
     * its request is dropped and the completion callback (like
     * this one) won't be invoked.
     */
    GASSERT(scan && scan->req == req);
    radio_request_unref(scan->req);
    scan->req = NULL;

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
                     scan->oplist = binder_netreg_oplist_fill(scan->oplist,
                         ops, count, self->network->voice.access_tech);
                     self->scan = NULL;
                     binder_netreg_scan_complete(self, scan);
                     return;
                 }
            } else if (error == RADIO_ERROR_REQUEST_NOT_SUPPORTED &&
                radio_client_interface(self->client) >= RADIO_INTERFACE_1_2) {
                /* Try startNetworkScan instead */
                DBG_(self, "getAvailableNetworks not supported");
                binder_netreg_start_network_scan(self);
            } else {
                ofono_warn("Failed to get the list of operators: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getAvailableNetworks response %d", resp);
        }
    }

    /* This invokes the ofono core callback with an unspecified error: */
    self->scan = NULL;
    binder_netreg_scan_free(self, scan);
}

static
void
binder_netreg_list_operators(
    struct ofono_netreg* netreg,
    ofono_netreg_operator_list_cb_t cb,
    void* data)
{
    BinderNetReg* self = binder_netreg_get_data(netreg);
    BinderNetRegScan* scan = binder_netreg_scan_new(cb, data);

    /* Drop the pending request if there is one */
    binder_netreg_scan_drop(self, self->scan);
    self->scan = scan;

    /*
     * Even though startNetworkScan was introduced in IRadio 1.1
     * getAvailableNetworks works there as well, and in addition
     * to that 1.1 variant of CellIdentity doesn't contain operator
     * names which makes 1.1 variant of NetworkScanResult (almost)
     * useless for our purposes here. Therefore we require IRadio 1.2
     * for startNetworkScan.
     */
    if (!self->use_network_scan ||
        radio_client_interface(self->client) < RADIO_INTERFACE_1_2) {
        /* getAvailableNetworks(int32_t serial) */
        scan->req = radio_request_new(self->client,
            RADIO_REQ_GET_AVAILABLE_NETWORKS, NULL,
            binder_netreg_get_available_networks_cb,
            NULL, self);
        radio_request_set_timeout(scan->req, OPERATOR_LIST_TIMEOUT_MS);

        /* Submit the request */
        if (radio_request_submit(scan->req)) {
            DBG_(self, "querying available networks");
        } else {
            DBG_(self, "failed to query available networks");
            self->scan = NULL;
            binder_netreg_scan_free(self, scan);
        }
    } else {
        binder_netreg_start_network_scan(self);
    }
}

static
void
binder_netreg_scan_op_copy_name(
    const RadioCellIdentityOperatorNames* src,
    struct ofono_network_operator* dest)
{
    /* Try to use long by default */
    if (src->alphaLong.len) {
        g_strlcpy(dest->name, src->alphaLong.data.str, sizeof(dest->name));
    } else if (src->alphaShort.len) {
        g_strlcpy(dest->name, src->alphaShort.data.str, sizeof(dest->name));
    }
}

static
void
binder_netreg_scan_op_convert_gsm(
    gboolean registered,
    const RadioCellIdentityGsm_1_2* src,
    struct ofono_network_operator* dest)
{
    const RadioCellIdentityGsm* gsm = &src->base;

    memset(dest, 0, sizeof(*dest));
    dest->status = registered ?
        OFONO_OPERATOR_STATUS_CURRENT :
        OFONO_OPERATOR_STATUS_AVAILABLE;
    dest->tech = OFONO_ACCESS_TECHNOLOGY_GSM;
    binder_netreg_scan_op_copy_name(&src->operatorNames, dest);
    g_strlcpy(dest->mcc, gsm->mcc.data.str, sizeof(dest->mcc));
    g_strlcpy(dest->mnc, gsm->mnc.data.str, sizeof(dest->mnc));
    DBG("[registered=%d, operator=%s, %s, %s, %s, %s]",
        registered, dest->name, dest->mcc, dest->mnc,
        binder_ofono_access_technology_string(dest->tech),
        binder_radio_op_status_string(dest->status));
}

static
void
binder_netreg_scan_op_convert_wcdma(
    gboolean registered,
    const RadioCellIdentityWcdma_1_2* src,
    struct ofono_network_operator* dest)
{
    const RadioCellIdentityWcdma* wcdma = &src->base;

    memset(dest, 0, sizeof(*dest));
    dest->status = registered ?
        OFONO_OPERATOR_STATUS_CURRENT :
        OFONO_OPERATOR_STATUS_AVAILABLE;
    dest->tech = OFONO_ACCESS_TECHNOLOGY_UTRAN;
    binder_netreg_scan_op_copy_name(&src->operatorNames, dest);
    g_strlcpy(dest->mcc, wcdma->mcc.data.str, sizeof(dest->mcc));
    g_strlcpy(dest->mnc, wcdma->mnc.data.str, sizeof(dest->mnc));
    DBG("[registered=%d, operator=%s, %s, %s, %s, %s]",
        registered, dest->name, dest->mcc, dest->mnc,
        binder_ofono_access_technology_string(dest->tech),
        binder_radio_op_status_string(dest->status));
}

static
void
binder_netreg_scan_op_convert_lte(
    gboolean registered,
    const RadioCellIdentityLte_1_2* src,
    struct ofono_network_operator* dest)
{
    const RadioCellIdentityLte* lte = &src->base;

    memset(dest, 0, sizeof(*dest));
    dest->status = registered ?
        OFONO_OPERATOR_STATUS_CURRENT :
        OFONO_OPERATOR_STATUS_AVAILABLE;
    dest->tech = OFONO_ACCESS_TECHNOLOGY_EUTRAN;
    binder_netreg_scan_op_copy_name(&src->operatorNames, dest);
    g_strlcpy(dest->mcc, lte->mcc.data.str, sizeof(dest->mcc));
    g_strlcpy(dest->mnc, lte->mnc.data.str, sizeof(dest->mnc));
    DBG("[registered=%d, operator=%s, %s, %s, %s, %s]",
        registered, dest->name, dest->mcc, dest->mnc,
        binder_ofono_access_technology_string(dest->tech),
        binder_radio_op_status_string(dest->status));
}

static
void
binder_netreg_scan_op_convert_nr(
    gboolean registered,
    const RadioCellIdentityNr* src,
    struct ofono_network_operator* dest)
{
    const RadioCellIdentityNr* nr = src;

    memset(dest, 0, sizeof(*dest));
    dest->status = registered ?
        OFONO_OPERATOR_STATUS_CURRENT :
        OFONO_OPERATOR_STATUS_AVAILABLE;
    dest->tech = OFONO_ACCESS_TECHNOLOGY_NG_RAN;
    binder_netreg_scan_op_copy_name(&src->operatorNames, dest);
    g_strlcpy(dest->mcc, nr->mcc.data.str, sizeof(dest->mcc));
    g_strlcpy(dest->mnc, nr->mnc.data.str, sizeof(dest->mnc));
    DBG("[registered=%d, operator=%s, %s, %s, %s, %s]",
        registered, dest->name, dest->mcc, dest->mnc,
        binder_ofono_access_technology_string(dest->tech),
        binder_radio_op_status_string(dest->status));
}

static
struct ofono_network_operator*
binder_netreg_scan_op_append(
    BinderNetRegScan* scan)
{
    const guint i = scan->oplist ? scan->oplist->count : 0;

    /*
     * Return a pointer to the newly allocated zero-initialized struct
     * ofono_network_operator
     */
    scan->oplist = binder_oplist_set_count(scan->oplist, i + 1);
    return scan->oplist->op + i;
}

static
void
binder_netreg_scan_result_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    BinderNetRegScan* scan = self->scan;

    if (scan) {
        GBinderReader reader;
        const RadioNetworkScanResult* result;

        /*
         * Regardless of the interface version, it's always the same
         * RadioNetworkScanResult structure with networkInfos pointing
         * to different things.
         */
        gbinder_reader_copy(&reader, args);
        result = gbinder_reader_read_hidl_struct(&reader,
            RadioNetworkScanResult);
        if (result) {
            guint i;
            const guint n = result->networkInfos.count;

            DBG_(self, "status=%d, error=%d, %u networks", result->status,
                result->error, n);
            if (code == RADIO_IND_NETWORK_SCAN_RESULT_1_2) {
                const RadioCellInfo_1_2* cells = result->networkInfos.data.ptr;

                for (i = 0; i < n; i++) {
                    const RadioCellInfo_1_2* cell = cells + i;
                    const RadioCellInfoGsm_1_2* gsm = cell->gsm.data.ptr;
                    const RadioCellInfoWcdma_1_2* wcdma = cell->wcdma.data.ptr;
                    const RadioCellInfoLte_1_2* lte = cell->lte.data.ptr;
                    guint j;

                    for (j = 0; j < cell->gsm.count; j++) {
                        binder_netreg_scan_op_convert_gsm(cell->registered,
                            &gsm[j].cellIdentityGsm,
                            binder_netreg_scan_op_append(scan));
                    }
                    for (j = 0; j < cell->wcdma.count; j++) {
                        binder_netreg_scan_op_convert_wcdma(cell->registered,
                            &wcdma[j].cellIdentityWcdma,
                            binder_netreg_scan_op_append(scan));
                    }
                    for (j = 0; j < cell->lte.count; j++) {
                        binder_netreg_scan_op_convert_lte(cell->registered,
                            &lte[j].cellIdentityLte,
                            binder_netreg_scan_op_append(scan));
                    }
                }
            } else if (code == RADIO_IND_NETWORK_SCAN_RESULT_1_4) {
                const RadioCellInfo_1_4* cells = result->networkInfos.data.ptr;

                for (i = 0; i < n; i++) {
                    const RadioCellInfo_1_4* cell = cells + i;

                    switch ((RADIO_CELL_INFO_TYPE_1_4)cell->cellInfoType) {
                    case RADIO_CELL_INFO_1_4_GSM:
                        binder_netreg_scan_op_convert_gsm(cell->registered,
                            &cell->info.gsm.cellIdentityGsm,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_4_WCDMA:
                        binder_netreg_scan_op_convert_wcdma(cell->registered,
                            &cell->info.wcdma.cellIdentityWcdma,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_4_LTE:
                        binder_netreg_scan_op_convert_lte(cell->registered,
                            &cell->info.lte.base.cellIdentityLte,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_4_NR:
                        binder_netreg_scan_op_convert_nr(cell->registered,
                            &cell->info.nr.cellIdentity,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_4_CDMA:
                    case RADIO_CELL_INFO_1_4_TD_SCDMA:
                        break;
                    }
                }
            } else if (code == RADIO_IND_NETWORK_SCAN_RESULT_1_5) {
                const RadioCellInfo_1_5* cells = result->networkInfos.data.ptr;

                for (i = 0; i < n; i++) {
                    const RadioCellInfo_1_5* cell = cells + i;

                    switch ((RADIO_CELL_INFO_TYPE_1_5)cell->cellInfoType) {
                    case RADIO_CELL_INFO_1_5_GSM:
                        binder_netreg_scan_op_convert_gsm(cell->registered,
                            &cell->info.gsm.cellIdentityGsm.base,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_5_WCDMA:
                        binder_netreg_scan_op_convert_wcdma(cell->registered,
                            &cell->info.wcdma.cellIdentityWcdma.base,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_5_LTE:
                        binder_netreg_scan_op_convert_lte(cell->registered,
                            &cell->info.lte.cellIdentityLte.base,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_5_NR:
                        binder_netreg_scan_op_convert_nr(cell->registered,
                            &cell->info.nr.cellIdentityNr.base,
                            binder_netreg_scan_op_append(scan));
                        break;
                    case RADIO_CELL_INFO_1_5_CDMA:
                    case RADIO_CELL_INFO_1_5_TD_SCDMA:
                        break;
                    }
                }
            }
            if (result->status == RADIO_SCAN_COMPLETE) {
                DBG_(self, "scan completed");
                self->scan = NULL;
                scan->stop = FALSE;
                binder_netreg_scan_complete(self, scan);
            } else {
                DBG_(self, "expecting more scan results");
            }
        } else {
            DBG_(self, "failed to parse scan result");
            self->scan = NULL;
            binder_netreg_scan_free(self, scan);
        }
    } else {
        DBG_(self, "got scan result without a scan");
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
        binder_netreg_register_cb, binder_netreg_cbd_destroy,
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
        binder_netreg_cbd_destroy,
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
        (radio_client_interface(self->client) >= RADIO_INTERFACE_1_5) ?
        RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL_1_5 :
        RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL, &writer,
        binder_netreg_register_cb, binder_netreg_cbd_destroy,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    /* setNetworkSelectionModeManual(int32 serial, string operatorNumeric); */
    gbinder_writer_add_cleanup(&writer, g_free, numeric);
    gbinder_writer_append_hidl_string(&writer, numeric);
    /* setNetworkSelectionModeManual_1_5 adds also suggested radio access network */
    if (radio_client_interface(self->client) >= RADIO_INTERFACE_1_5) {
        gbinder_writer_append_int32(&writer, RADIO_ACCESS_NETWORKS_UNKNOWN);
    }

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
    const RadioSignalStrengthTdScdma_1_2* tdscdma,
    const RadioSignalStrengthNr* nr)
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

    if (nr) {
        if (nr->ssRsrp >= RSRP_MIN && nr->ssRsrp <= RSRP_MAX) {
            rsrp = nr->ssRsrp;
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
                (&ss->gw, &ss->lte, NULL, NULL, NULL);
        }
    } else if (code == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_2) {
        const RadioSignalStrength_1_2* ss = gbinder_reader_read_hidl_struct
            (&reader, RadioSignalStrength_1_2);

        if (ss) {
            dbm = binder_netreg_get_signal_strength_dbm
                (&ss->gw, &ss->lte, &ss->wcdma, NULL, NULL);
        }
    } else if (code == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4) {
        const RadioSignalStrength_1_4* ss = gbinder_reader_read_hidl_struct
            (&reader, RadioSignalStrength_1_4);

        if (ss) {
            dbm = binder_netreg_get_signal_strength_dbm
                (&ss->gsm, &ss->lte, &ss->wcdma, &ss->tdscdma, &ss->nr);
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
                        (&ss->gw, &ss->lte, NULL, NULL, NULL);
                }
            } else if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH_1_2) {
                const RadioSignalStrength_1_2* ss =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioSignalStrength_1_2);

                if (ss) {
                    dbm = binder_netreg_get_signal_strength_dbm
                        (&ss->gw, &ss->lte, &ss->wcdma, NULL, NULL);
                }
            } else if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH_1_4) {
                const RadioSignalStrength_1_4* ss =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioSignalStrength_1_4);

                if (ss) {
                    dbm = binder_netreg_get_signal_strength_dbm
                        (&ss->gsm, &ss->lte, &ss->wcdma, &ss->tdscdma, &ss->nr);
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
        NULL, binder_netreg_strength_cb, binder_netreg_cbd_destroy,
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
void
binder_netreg_modem_reset_notify(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;

    DBG_(self, "%s", binder_read_hidl_string(args));

    /* Drop pending requests */
    radio_request_drop(self->register_req);
    radio_request_drop(self->strength_req);
    self->register_req = NULL;
    self->strength_req = NULL;

    /* And complete the scan (successfully if there were any results) */
    if (self->scan) {
        BinderNetRegScan* scan = self->scan;

        self->scan = NULL;
        if (scan->oplist && scan->oplist->count) {
            binder_netreg_scan_complete(self, scan);
        } else {
            binder_netreg_scan_free(self, scan);
        }
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
    self->network_event_id[NETREG_NETWORK_EVENT_DATA_STATE_CHANGED] =
        binder_network_add_property_handler(self->network,
            BINDER_NETWORK_PROPERTY_DATA_STATE,
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

    /* Incremental scan results */
    self->ind_id[IND_NETWORK_SCAN_RESULT_1_2] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_NETWORK_SCAN_RESULT_1_2,
            binder_netreg_scan_result_notify, self);
    self->ind_id[IND_NETWORK_SCAN_RESULT_1_4] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_NETWORK_SCAN_RESULT_1_4,
            binder_netreg_scan_result_notify, self);
    self->ind_id[IND_NETWORK_SCAN_RESULT_1_5] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_NETWORK_SCAN_RESULT_1_5,
            binder_netreg_scan_result_notify, self);

    /* Miscellaneous */
    self->ind_id[IND_MODEM_RESET] =
        radio_client_add_indication_handler(self->client,
            RADIO_IND_MODEM_RESET,
            binder_netreg_modem_reset_notify, self);
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
    self->techs = config->techs;
    self->use_network_scan = config->use_network_scan;
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

    radio_request_drop(self->register_req);
    radio_request_drop(self->strength_req);

    ofono_watch_unref(self->watch);
    binder_network_remove_all_handlers(self->network, self->network_event_id);
    binder_network_unref(self->network);

    radio_client_remove_all_handlers(self->client, self->ind_id);
    radio_client_unref(self->client);

    binder_netreg_scan_drop(self, self->scan);
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
