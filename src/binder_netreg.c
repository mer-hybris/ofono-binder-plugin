/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *  Copyright (C) 2025 Slava Monich <slava@monich.com>
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

#include <radio_modem_types.h>
#include <radio_network_types.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

#include <stdlib.h>

#define REGISTRATION_MAX_RETRIES (2)
/* Some vendors require longer than the binder API minimum */
#define NETWORK_SCAN_MAX_SEARCH_TIME_SEC (70) /* 1 min 10 seconds */
#define NETWORK_SCAN_TIMEOUT_SEC (NETWORK_SCAN_MAX_SEARCH_TIME_SEC + 5) /* 1 min 15 seconds */
#define OPERATOR_LIST_TIMEOUT_SEC (300) /* 5 min */
#define OPERATOR_LIST_TIMEOUT_MS (OPERATOR_LIST_TIMEOUT_SEC * 1000)

#define INCREMENTAL_RESULTS_PERIODICITY_RANGE_MIN (1)
#define INCREMENTAL_RESULTS_PERIODICITY_RANGE_MAX (10)

typedef struct binder_netreg_scan BinderNetRegScan;
typedef struct binder_netreg_ss BinderSs;

enum binder_netreg_radio_ind {
    IND_NITZ_TIME_RECEIVED,
    IND_SIGNAL_STRENGTH,
    IND_NETWORK_SCAN_RESULT,
    IND_NETWORK_MODEM_RESET,
    IND_COUNT
};

enum binder_netreg_modem_ind_events {
    IND_MODEM_RESET,
    IND_MODEM_COUNT
};

enum binder_netreg_network_events {
    NETREG_NETWORK_EVENT_DATA_STATE_CHANGED,
    NETREG_NETWORK_EVENT_VOICE_STATE_CHANGED,
    NETREG_NETWORK_EVENT_OPERATOR_CHANGED,
    NETREG_NETWORK_EVENT_COUNT
};

typedef struct binder_netreg_api BinderNetRegApi;
typedef struct binder_netreg {
    RadioClient* client;
    RadioClient* modem_client;
    struct ofono_watch* watch;
    struct ofono_netreg* netreg;
    const BinderNetRegApi* api;
    BinderNetwork* network;
    BinderRegistrationState reg_state;
    enum ofono_radio_access_mode techs;
    gboolean operator_changed;
    gboolean use_network_scan;
    gboolean replace_strange_oper;
    gboolean prefer_lte_signal_strength;
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
    gulong modem_ind_id[IND_MODEM_COUNT];
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

typedef enum binder_radio_tech_type {
    BINDER_RADIO_TECH_NONE = 0,
    BINDER_RADIO_TECH_GSM = 0x01,
    BINDER_RADIO_TECH_TDSCDMA = 0x02,
    BINDER_RADIO_TECH_WCDMA = 0x04,
    BINDER_RADIO_TECH_LTE = 0x08,
    BINDER_RADIO_TECH_NR = 0x10,
    BINDER_RADIO_TECH_MAX = BINDER_RADIO_TECH_NR,
    BINDER_RADIO_TECH_ALL = ((BINDER_RADIO_TECH_MAX << 1) - 1)
} BINDER_RADIO_TECH_TYPE;

typedef enum binder_radio_access_specifier_bands_type {
    BINDER_RADIO_ACCESS_SPECIFIER_BANDS_NONE,
    BINDER_RADIO_ACCESS_SPECIFIER_BANDS_GERAN,
    BINDER_RADIO_ACCESS_SPECIFIER_BANDS_UTRAN,
    BINDER_RADIO_ACCESS_SPECIFIER_BANDS_EUTRAN,
    BINDER_RADIO_ACCESS_SPECIFIER_BANDS_NGRAN
} BINDER_RADIO_ACCESS_SPECIFIER_BANDS_TYPE;

typedef struct binder_netreg_radio_type {
    enum ofono_radio_access_mode mode;
    RADIO_ACCESS_NETWORKS ran;
    RADIO_NETWORK_SCAN_SPECIFIER_1_5_TYPE hidl_type;
    BINDER_RADIO_ACCESS_SPECIFIER_BANDS_TYPE aidl_type;
} BinderNetRegRadioType;

typedef struct binder_radio_signal_strength {
    const RadioSignalStrengthGsm* gsm;
    const RadioSignalStrengthLte* lte;
    const RadioSignalStrengthWcdma_1_2* wcdma;
    const RadioSignalStrengthTdScdma_1_2* tdscdma;
    const RadioSignalStrengthNr* nr;
} BinderRadioSignalStrength;

struct binder_netreg_api {
    const char* name;
    BinderReadStringArg read_string_arg;
    BinderOpList* (*read_oplist)(
        BinderNetReg* self,
        GBinderReader* reader);
    gboolean (*read_signal_strength_resp)(
        BinderNetReg* self,
        RADIO_RESP resp,
        GBinderReader* reader,
        BinderSs* ss);
    gboolean (*read_signal_strength_ind)(
        BinderNetReg* self,
        RADIO_IND ind,
        GBinderReader* reader,
        BinderSs* ss);
    BinderOpList* (*read_network_scan)(
        BinderNetReg* self,
        RADIO_IND code,
        GBinderReader* reader,
        RADIO_SCAN_STATUS* scan_status);
    void (*write_start_network_scan_args)(
        BinderNetReg* self,
        GBinderWriter* writer);
    void (*write_set_network_selection_mode_manual_args)(
        GBinderWriter* writer,
        const char* numeric);
    RADIO_REQ start_network_scan_req;
    RADIO_REQ stop_network_scan_req;
    RADIO_REQ get_available_networks_req;
    RADIO_REQ get_signal_strength_req;
    RADIO_REQ get_network_selection_mode_req;
    RADIO_REQ set_network_selection_mode_automatic_req;
    RADIO_REQ set_network_selection_mode_manual_req;
    RADIO_IND nitz_time_received_ind;
    RADIO_IND current_signal_strength_ind;
    RADIO_IND network_scan_result_ind;
    RADIO_IND network_modem_reset_ind;
    RADIO_IND modem_reset_ind;
};

static const BinderNetRegApi binder_netreg_api_hidl;
static const BinderNetRegApi binder_netreg_api_hidl_1_2;
static const BinderNetRegApi binder_netreg_api_hidl_1_4;
static const BinderNetRegApi binder_netreg_api_hidl_1_5;
static const BinderNetRegApi binder_netreg_api_aidl;

static const BinderNetRegRadioType binder_netreg_radio_types[] = {
    {
         OFONO_RADIO_ACCESS_MODE_GSM,
         RADIO_ACCESS_NETWORKS_GERAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_GERAN,
         BINDER_RADIO_ACCESS_SPECIFIER_BANDS_GERAN
    },{
         OFONO_RADIO_ACCESS_MODE_UMTS,
         RADIO_ACCESS_NETWORKS_UTRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_UTRAN,
         BINDER_RADIO_ACCESS_SPECIFIER_BANDS_UTRAN
    },{
         OFONO_RADIO_ACCESS_MODE_LTE,
         RADIO_ACCESS_NETWORKS_EUTRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_EUTRAN,
         BINDER_RADIO_ACCESS_SPECIFIER_BANDS_EUTRAN
    },{
         OFONO_RADIO_ACCESS_MODE_NR,
         RADIO_ACCESS_NETWORKS_NGRAN,
         RADIO_NETWORK_SCAN_SPECIFIER_1_5_NGRAN,
         BINDER_RADIO_ACCESS_SPECIFIER_BANDS_NGRAN
    }
};

#define N_RADIO_TYPES_1_5 G_N_ELEMENTS(binder_netreg_radio_types)
#define N_RADIO_TYPES (N_RADIO_TYPES_1_5 - 1) /* No NG */
#define N_RADIO_TYPES_AIDL N_RADIO_TYPES_1_5
G_STATIC_ASSERT(N_RADIO_TYPES_1_5 == OFONO_RADIO_ACCESS_MODE_COUNT);

/* enum ofono_operator_status and RADIO_OP_STATUS must be identical */
G_STATIC_ASSERT((int)RADIO_OP_STATUS_UNKNOWN == OFONO_OPERATOR_STATUS_UNKNOWN);
G_STATIC_ASSERT((int)RADIO_OP_AVAILABLE == OFONO_OPERATOR_STATUS_AVAILABLE);
G_STATIC_ASSERT((int)RADIO_OP_CURRENT == OFONO_OPERATOR_STATUS_CURRENT);
G_STATIC_ASSERT((int)RADIO_OP_FORBIDDEN == OFONO_OPERATOR_STATUS_FORBIDDEN);

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

/* Signal strength measurement */

typedef struct binder_ss_threshold {
    int value;
    int percent;
} BinderSsThreshold;

typedef struct binder_ss_percent_map {
    const char* name;
    BINDER_RADIO_TECH_TYPE tech_mask;
    const BinderSsThreshold* threshold;
    guint count;
} BinderSsPercentMap;

/* m(NAME,name) */
#define BINDER_SS_MEASUREMENTS(m) \
    m(RXLEV,rxlev)   /* GSM */ \
    m(RSSI,rssi)     /* WCDMA, TD-SCDMA, LTE */ \
    m(RSCP,rscp)     /* WCDMA, TD-SCDMA */ \
    m(RSRP,rsrp)     /* LTE */ \
    m(RSRQ,rsrq)     /* LTE */ \
    m(RSSNR,rssnr)   /* LTE */ \
    m(SSRSRP,ssrsrp) /* NR */ \
    m(SSRSRQ,ssrsrq) /* NR */ \
    m(SSSINR,sssinr) /* NR */

#define BINDER_RADIO_TECH_MASK_RXLEV  BINDER_RADIO_TECH_GSM
#define BINDER_RADIO_TECH_MASK_RSSI  (BINDER_RADIO_TECH_WCDMA | \
                                      BINDER_RADIO_TECH_TDSCDMA | \
                                      BINDER_RADIO_TECH_LTE)
#define BINDER_RADIO_TECH_MASK_RSCP  (BINDER_RADIO_TECH_WCDMA | \
                                      BINDER_RADIO_TECH_TDSCDMA)
#define BINDER_RADIO_TECH_MASK_RSRP   BINDER_RADIO_TECH_LTE
#define BINDER_RADIO_TECH_MASK_RSRQ   BINDER_RADIO_TECH_LTE
#define BINDER_RADIO_TECH_MASK_RSSNR  BINDER_RADIO_TECH_LTE
#define BINDER_RADIO_TECH_MASK_SSRSRP BINDER_RADIO_TECH_NR
#define BINDER_RADIO_TECH_MASK_SSRSRQ BINDER_RADIO_TECH_NR
#define BINDER_RADIO_TECH_MASK_SSSINR BINDER_RADIO_TECH_NR

typedef enum binder_ss_measurement {
    #define SIGNAL_STRENGTH_ENUM_(NAME,name) \
    BINDER_SS_##NAME,
    BINDER_SS_MEASUREMENTS(SIGNAL_STRENGTH_ENUM_)
    #undef SIGNAL_STRENGTH_ENUM_
    BINDER_SS_MEASUREMENT_COUNT
} BINDER_SS_MEASUREMENT;

struct binder_netreg_ss {
    int m[BINDER_SS_MEASUREMENT_COUNT];
};

#define SIGNAL_STRENGTH_MAP(var,type,map...) \
    static const BinderSsThreshold var##_[] = map; \
    static const BinderSsPercentMap var = {#type, \
        BINDER_RADIO_TECH_MASK_##type, var##_, G_N_ELEMENTS(var##_)}

#define NO_MEASUREMENT INT_MAX

/*
 * RXLEV
 * Received signal strength level
 *
 * Reference: 3GPP TS 27.007 section 8.69
 * Range -110..-48 dBm
 * Valid values (0-61, 99)
 *
 * 0       less than -110 dBm
 * 1..62   -110... -49 dBm
 * 63      -48 dBm or greater
 * 99      not known or not detectable
 */
#define RXLEV_VALID(x) ((x) != INT_MAX && (x) != 99)
SIGNAL_STRENGTH_MAP(binder_rxlev_map, RXLEV,
    {{0, 1}, {3, 20}, {7, 40}, {13, 60}, {21, 80}, {59, 100}});

/*
 * RSSI
 * Received Signal Strength Indication
 *
 * Reference: 3GPP TS 27.007 section 8.5
 * Range -113..-51 dBm
 * Valid values (0-31, 99)
 *
 * 0       -113 dBm or less
 * 1       -111 dBm
 * 2...30  -109... -53 dBm
 * 31      -51 dBm or greater
 * 99      not known or not detectable
 */
#define RSSI_VALID(x) ((x) != INT_MAX && (x) != 99)
SIGNAL_STRENGTH_MAP(binder_rssi_map, RSSI,
    {{1, 1}, {3, 20}, {8, 40}, {13, 60}, {18, 80}, {31, 100}});

/*
 * RSCP
 * Received Signal Code Power
 *
 * Reference: 3GPP TS 27.007 section 8.69
 * Range: -120..-24 dBm
 * Valid values (0-96, 255)
 *
 * 0       -120 dBm or less
 * 1       -119 dBm
 * 2...95  -118...-25 dBm
 * 96      -24 dBm or greater
 * 255     not known or not detectable
 */
#define RSCP_VALID(x) ((x) != INT_MAX && (x) != 255)
SIGNAL_STRENGTH_MAP(binder_rscp_map, RSCP,
    {{0, 1}, {10, 20}, {23, 40}, {47, 60}, {71, 80}, {96, 100}});

/*
 * RSRP
 * Reference Signal Received Power
 *
 * Reference: 3GPP TS 36.133 section 9.1.4
 * Range -140..-44 dBm
 * Value is dBm multipled by -1
 */
#define RSRP_VALID(x) ((x) != INT_MAX)
SIGNAL_STRENGTH_MAP(binder_rsrp_map, RSRP,
    {{-115, 1}, {-105, 33}, {-95, 66}, {-85, 100}});

/*
 * RSRQ
 * Reference Signal Receive Quality in dB
 *
 * Reference: 3GPP TS 36.133 9.1.7
 * Range -34..3 dB
 * Value is dB multipled by -1
 */
#define RSRQ_VALID(x) ((x) != INT_MAX)
SIGNAL_STRENGTH_MAP(binder_rsrq_map, RSRQ,
   {{-19, 1}, {-17, 33}, {-14, 66}, {-12, 100}});

/*
 * RSSNR
 * Reference signal signal-to-noise ratio in 0.1 dB units.
 *
 * Reference: 3GPP TS 36.101 8.1.1
 * Range -200..300
 */
#define RSSNR_VALID(x) ((x) != INT_MAX)
SIGNAL_STRENGTH_MAP(binder_rssnr_map, RSSNR,
    {{-30, 1}, {10, 33}, {50, 66}, {130, 100}});

/*
 * SSRSRP
 * 5G SS reference signal received power
 *
 * Reference: 3GPP TS 38.215.
 * Range -140..-44 dBm
 * Value is dBm multipled by -1
 */
#define SSRSRP_VALID(x) ((x) != INT_MAX)
SIGNAL_STRENGTH_MAP(binder_ssrsrp_map, SSRSRP,
 /* {{-110, 1}, {-90, 33}, {-80, 66}, {-65, 100}}); */
    {{-130, 1}, {-110, 33}, {-100, 66}, {-85, 100}});

/*
 * SSRSRQ
 * 5G SS reference signal received quality
 *
 * Reference: 3GPP TS 38.215, 3GPP TS 38.133 section 10
 * Range: -43..20 dB
 * Value is dB multipled by -1
 */
#define SSRSRQ_VALID(x) ((x) != INT_MAX)
SIGNAL_STRENGTH_MAP(binder_ssrsrq_map, SSRSRQ,
    {{-31, 1}, {-19, 33}, {-7, 66}, {6, 100}});

/*
 * SSSINR
 * 5G SS signal-to-noise and interference ratio
 *
 * Reference: 3GPP TS 38.215 section 5.1, 3GPP TS 38.133 section 10.1.16.1.
 * Range: -23..40 dB
 */
#define SSSINR_VALID(x) ((x) != INT_MAX)
SIGNAL_STRENGTH_MAP(binder_sssinr_map, SSSINR,
    {{-5, 1}, {5, 33}, {15, 66}, {30, 100}});


static
BINDER_RADIO_TECH_TYPE
binder_netreg_tech_type(
    enum ofono_access_technology tech)
{
    switch (tech) {
    case OFONO_ACCESS_TECHNOLOGY_GSM:
    case OFONO_ACCESS_TECHNOLOGY_GSM_COMPACT:
    case OFONO_ACCESS_TECHNOLOGY_GSM_EGPRS:
        return BINDER_RADIO_TECH_GSM;
    case OFONO_ACCESS_TECHNOLOGY_UTRAN:
        return BINDER_RADIO_TECH_WCDMA;
    case OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA:
    case OFONO_ACCESS_TECHNOLOGY_UTRAN_HSUPA:
    case OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA:
        return BINDER_RADIO_TECH_TDSCDMA;
    case OFONO_ACCESS_TECHNOLOGY_EUTRAN:
    case OFONO_ACCESS_TECHNOLOGY_NB_IOT_M1:
    case OFONO_ACCESS_TECHNOLOGY_NB_IOT_NB1:
    case OFONO_ACCESS_TECHNOLOGY_EUTRA_5GCN:
        return BINDER_RADIO_TECH_LTE;
    case OFONO_ACCESS_TECHNOLOGY_NR_5GCN:
    case OFONO_ACCESS_TECHNOLOGY_NG_RAN:
    case OFONO_ACCESS_TECHNOLOGY_EUTRA_NR:
        return BINDER_RADIO_TECH_NR;
    case OFONO_ACCESS_TECHNOLOGY_NONE:
        break;
    }
    return BINDER_RADIO_TECH_NONE;
}

static
void
binder_netreg_ss_clear(
    BinderSs* ss)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(ss->m); i++) {
        ss->m[i] = NO_MEASUREMENT;
    }
}

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

    /*
     * The only way to report operator change to ofono is to call
     * ofono_netreg_status_notify and hope that it will invoke
     * our .current_operator callback.
     */
    if (self->operator_changed ||
        current->status != reg_status ||
        current->access_tech != reg->access_tech ||
        current->lac != reg->lac ||
        current->ci != reg->ci) {
        /* Registration state or operator has changed */
        current->status = reg_status;
        current->access_tech = reg->access_tech;
        current->lac = reg->lac;
        current->ci = reg->ci;
        self->operator_changed = FALSE; /* Clear the change */
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
binder_netreg_operator_notify(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    void* user_data)
{
    BinderNetReg* self = user_data;

    /* Force ofono_netreg_status_notify even if the state didn't change */
    self->operator_changed = TRUE;
    binder_netreg_status_notify(net, property, self);
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
    gutil_source_remove(self->current_operator_id);
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
        gutil_source_remove(scan->timeout_id);
        if (scan->stop) {
            RadioRequest* req = radio_request_new(self->client,
                self->api->stop_network_scan_req, NULL, NULL, NULL, NULL);

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
    const BinderNetRegApi* api = self->api;

    /*
     * Scan must be non-NULL because when it gets deallocated,
     * its request is dropped and the completion callback (like
     * this one) won't be invoked.
     */
    GASSERT(scan && scan->req == req);
    radio_request_unref(scan->req);
    scan->req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "getAvailableNetworks tx failed");
    } else if (error == RADIO_ERROR_REQUEST_NOT_SUPPORTED) {
        DBG_(self, "getAvailableNetworks not supported");
        if (api->start_network_scan_req) {
            GBinderWriter writer;

            /* Try startNetworkScan instead */
            scan->req = radio_request_new(self->client,
                api->start_network_scan_req, &writer,
                binder_netreg_start_scan_cb, NULL, self);
            api->write_start_network_scan_args(self, &writer);
            scan->stop = TRUE;
            scan->timeout_id = g_timeout_add_seconds(NETWORK_SCAN_TIMEOUT_SEC,
                binder_netreg_scan_timeoult_cb, self);
            if (radio_request_submit(scan->req)) {
                DBG_(self, "trying to scan available networks");
                return;
            }
        }
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to get the list of operators: %s",
              binder_radio_error_string(error));
    } else {
        GBinderReader reader;
        const BinderNetRegApi* api = self->api;

        gbinder_reader_copy(&reader, args);
        if ((scan->oplist = api->read_oplist(self, &reader)) != NULL) {
            /* Got the list */
            self->scan = NULL;
            binder_netreg_scan_complete(self, scan);
            return;
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
    const BinderNetRegApi* api = self->api;

    /* Drop the pending request if there is one */
    binder_netreg_scan_drop(self, self->scan);
    self->scan = scan;

    /* Prefer incremental scanning */
    if (self->use_network_scan && api->start_network_scan_req) {
        GBinderWriter writer;

        scan->req = radio_request_new(self->client,
            api->start_network_scan_req, &writer,
            binder_netreg_start_scan_cb, NULL, self);
        api->write_start_network_scan_args(self, &writer);
        scan->stop = TRUE; /* Assume that startNetworkScan succeeds */
        scan->timeout_id = g_timeout_add_seconds(NETWORK_SCAN_TIMEOUT_SEC,
            binder_netreg_scan_timeoult_cb, self);
    } else {
        /* Fallback to getAvailableNetworks */
        scan->req = radio_request_new(self->client,
            api->get_available_networks_req, NULL,
            binder_netreg_get_available_networks_cb,
            NULL, self);
        radio_request_set_timeout(scan->req, OPERATOR_LIST_TIMEOUT_MS);
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
        RADIO_SCAN_STATUS scan_status;
        BinderOpList* oplist;

        gbinder_reader_copy(&reader, args);
        oplist = self->api->read_network_scan(self, code,
            &reader, &scan_status);

        if (oplist) {
            guint i;

            /* Merge the lists avoiding dups */
            for (i = 0; i < oplist->count; i++) {
                struct ofono_network_operator* new_op = oplist->op + i;
                struct ofono_network_operator* old_op =
                    binder_oplist_find(scan->oplist, new_op->mcc, new_op->mnc);

                if (old_op) {
                    *old_op = *new_op;
                } else {
                    scan->oplist = binder_oplist_append(scan->oplist, new_op);
                }
            }
            binder_oplist_free(oplist);
        } else {
            DBG_(self, "failed to parse scan result");
            scan_status = RADIO_SCAN_COMPLETE;
        }

        if (scan_status == RADIO_SCAN_COMPLETE) {
            DBG_(self, "scan completed");
            self->scan = NULL;
            scan->stop = FALSE;
            binder_netreg_scan_complete(self, scan);
        } else {
            DBG_(self, "expecting more scan results");
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
    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "setNetworkSelectionMode tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_error("registration failed, error %s",
            binder_radio_error_string(error));
    } else {
        /* Success */
        cb(binder_error_ok(&err), cbd->data);
        return;
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
    const BinderNetRegApi* api = self->api;
    ofono_netreg_register_cb_t cb = cbd->cb.reg;
    struct ofono_error err;

    GASSERT(self->register_req == req);
    radio_request_unref(self->register_req);
    self->register_req = NULL;

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "getNetworkSelectionMode tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to query network selection mode: %s",
            binder_radio_error_string(status));
    } else {
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

    /*
     * Either selection is set to manual, or the query failed.
     * In either case, let's give it a try.
     */
    req = radio_request_new(self->client,
        api->set_network_selection_mode_automatic_req, NULL,
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
        self->api->get_network_selection_mode_req, NULL,
        binder_netreg_query_register_auto_cb,
        binder_netreg_cbd_destroy,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    /* getNetworkSelectionMode(int32 serial); */
    radio_request_drop(self->register_req);
    if (!(self->register_req = radio_request_try_submit(req))) {
        struct ofono_error err;

        DBG_(self, "failed to query bw selection mode");
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
    const BinderNetRegApi* api = self->api;
    char* numeric = g_strconcat(mcc, mnc, NULL);
    GBinderWriter writer;
    RadioRequest* req = radio_request_new(self->client,
        api->set_network_selection_mode_manual_req, &writer,
        binder_netreg_register_cb, binder_netreg_cbd_destroy,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    api->write_set_network_selection_mode_manual_args(&writer, numeric);
    radio_request_set_timeout(req, self->network_selection_timeout_ms);
    radio_request_drop(self->register_req);
    if ((self->register_req = radio_request_try_submit(req)) != NULL) {
        ofono_info("%snw select manual: %s", self->log_prefix, numeric);
        self->register_req = req; /* Keep the ref */
    } else {
        struct ofono_error err;

        DBG_(self, "failed to set nw select manual: %s", numeric);
        cb(binder_error_failure(&err), data);
    }
    g_free(numeric);
}

static
int
binder_netreg_ss_percent_map(
    const BinderSsPercentMap* map,
    int value)
{
    if (value <= map->threshold[0].value) {
        /* Minimum */
        return map->threshold[0].percent;
    } else {
        if (value < map->threshold[map->count - 1].value) {
            guint i;

            /* Somewhere in the middle */
            for (i = 1; i < map->count; i++) {
                const BinderSsThreshold* t = map->threshold + i;

                if (value <= t->value) {
                    const BinderSsThreshold* t0 = t - 1;

                    /* Linear interpolation between the thresholds */
                    return t0->percent + (t->percent - t0->percent) *
                        (value - t0->value) / (t->value - t0->value);
                }
            }
        }
        /* Maximum */
        return map->threshold[map->count - 1].percent;
    }
}

static
int
binder_netreg_tech_percent(
    BinderNetReg* self,
    const BinderSs* ss,
    BINDER_RADIO_TECH_TYPE techs)
{
    static const BinderSsPercentMap*
    ss_maps[BINDER_SS_MEASUREMENT_COUNT] = {
        #define SIGNAL_STRENGTH_MAP_(NAME,name) &binder_##name##_map,
        BINDER_SS_MEASUREMENTS(SIGNAL_STRENGTH_MAP_)
        #undef SIGNAL_STRENGTH_MAP_
    };

    int i, percent = 0;

    for (i = 0; i < BINDER_SS_MEASUREMENT_COUNT; i++) {
        if (ss->m[i] != NO_MEASUREMENT) {
            const BinderSsPercentMap* map = ss_maps[i];

            if (map->tech_mask & techs) {
                const int p = binder_netreg_ss_percent_map(map, ss->m[i]);

                DBG_(self, "%s %d (%d%%)", map->name, ss->m[i], p);
                if (!percent || p < percent) {
                    percent = p;
                }
            } else {
                DBG_(self, "%s %d (ignored)", map->name, ss->m[i]);
            }
        }
    }

    if (percent > 0) {
        DBG_(self, "%d%%", percent);
        return percent;
    }

    return 0;
}

static
int
binder_netreg_ss_percent(
    BinderNetReg* self,
    const BinderSs* ss)
{
    BINDER_RADIO_TECH_TYPE tech, techs = BINDER_RADIO_TECH_ALL;
    const BINDER_RADIO_TECH_TYPE access_tech =
        binder_netreg_tech_type(self->reg_state.access_tech);

    /*
     * If we are registered with NR, or not registered at all, see if
     * we should check LTE signal quality indicators first.
     */
    if ((access_tech == BINDER_RADIO_TECH_NR ||
         access_tech == BINDER_RADIO_TECH_NONE) &&
        self->prefer_lte_signal_strength) {
        int percent = binder_netreg_tech_percent(self, ss,
            BINDER_RADIO_TECH_LTE);

        if (percent > 0) {
            return percent;
        }

        /* Done with LTE */
        techs &= ~BINDER_RADIO_TECH_LTE;
    }

    /*
     * Then check signal quality indicators for the registration tech,
     * if there is one. Unless we have checked it already.
     */
    if (access_tech != BINDER_RADIO_TECH_NONE && (techs & access_tech)) {
        int percent = binder_netreg_tech_percent(self, ss, access_tech);

        if (percent > 0) {
            return percent;
        }

        /* Done with the current registration tech */
        techs &= ~access_tech;
    }

    /* Try all other techs that we haven't tried yet. */
    tech = BINDER_RADIO_TECH_MAX;
    while (techs) {
        if (techs & tech) {
            int percent = binder_netreg_tech_percent(self, ss, tech);

            if (percent > 0) {
                return percent;
            }
            techs &= ~tech;
        }
        tech >>= 1;
    }

    return 0;
}

static
void
binder_netreg_strength_notify(
    RadioClient* client,
    RADIO_IND resp,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetReg* self = user_data;
    GBinderReader reader;
    BinderSs ss;

    gbinder_reader_copy(&reader, args);
    binder_netreg_ss_clear(&ss);
    if (self->api->read_signal_strength_ind(self, resp, &reader, &ss)) {
        ofono_netreg_strength_notify(self->netreg,
            binder_netreg_ss_percent(self, &ss));
    }
}

static
void
binder_netreg_strength_cb(
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

    if (status != RADIO_TX_STATUS_OK) {
        DBG_(self, "getSignalStrength tx failed");
    } else if (error != RADIO_ERROR_NONE) {
        ofono_warn("Failed to retrive the signal strength: %s",
            binder_radio_error_string(status));
    } else {
        GBinderReader reader;
        BinderSs ss;

        gbinder_reader_copy(&reader, args);
        binder_netreg_ss_clear(&ss);
        if (self->api->read_signal_strength_resp(self, resp, &reader, &ss)) {
            cb(binder_error_ok(&err), binder_netreg_ss_percent(self, &ss),
                cbd->data);
            return;
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
        self->api->get_signal_strength_req, NULL,
        binder_netreg_strength_cb, binder_netreg_cbd_destroy,
        binder_netreg_cbd_new(self, BINDER_CB(cb), data));

    radio_request_set_retry(req, BINDER_RETRY_MS, -1);
    radio_request_drop(self->strength_req);
    if (!(self->strength_req = radio_request_try_submit(req))) {
        struct ofono_error err;

        DBG_(self, "failed to query signal strength");
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
    char* arg;

    /*
     * IRadioIndication.hal:
     * oneway nitzTimeReceived(RadioIndicationType type,
     *     string nitzTime, uint64_t receivedTime);
     *
     * IRadioNetworkIndication.aidl:
     * void nitzTimeReceived(in RadioIndicationType type,
     *     in String nitzTime, in long receivedTimeMs, in long ageMs);
     */
    gbinder_reader_copy(&reader, args);
    nitz = self->api->read_string_arg(&reader, &arg);

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

    g_free(arg);
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
    GBinderReader reader;
    char* arg = NULL;

    /*
     * IRadioIndication.hal:
     * oneway modemReset(RadioIndicationType type, string reason);
     *
     * IRadioModemIndication.aidl:
     * void modemReset(in RadioIndicationType type, in String reason);
     */

    gbinder_reader_copy(&reader, args);
    DBG_(self, "%s", self->api->read_string_arg(&reader, &arg));
    g_free(arg);

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
    const BinderNetRegApi* api = self->api;

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
    self->network_event_id[NETREG_NETWORK_EVENT_OPERATOR_CHANGED] =
        binder_network_add_property_handler(self->network,
            BINDER_NETWORK_PROPERTY_OPERATOR,
            binder_netreg_operator_notify, self);

    /* Register for network time updates */
    self->ind_id[IND_NITZ_TIME_RECEIVED] =
        radio_client_add_indication_handler(self->client,
            api->nitz_time_received_ind,
            binder_netreg_nitz_notify, self);

    /* Register for signal strength changes */
    self->ind_id[IND_SIGNAL_STRENGTH] =
        radio_client_add_indication_handler(self->client,
            api->current_signal_strength_ind,
          binder_netreg_strength_notify, self);

    /* Incremental scan results */
    if (api->network_scan_result_ind) {
        self->ind_id[IND_NETWORK_SCAN_RESULT] =
            radio_client_add_indication_handler(self->client,
                api->network_scan_result_ind,
                binder_netreg_scan_result_notify, self);
    }

    if (api->network_modem_reset_ind) {
        self->ind_id[IND_NETWORK_MODEM_RESET] =
            radio_client_add_indication_handler(self->client,
                api->network_modem_reset_ind,
                binder_netreg_modem_reset_notify, self);
    }

    if (api->modem_reset_ind) {
        self->modem_ind_id[IND_MODEM_RESET] =
            radio_client_add_indication_handler(self->modem_client,
                api->modem_reset_ind,
                binder_netreg_modem_reset_notify, self);
    }

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
    RadioClient* network_client = modem->clients.network_client;
    RADIO_INTERFACE hidl = radio_client_interface(network_client);
    RADIO_AIDL_INTERFACE aidl = radio_client_aidl_interface(network_client);
    const BinderNetRegApi* api =
        (aidl == RADIO_NETWORK_INTERFACE) ? &binder_netreg_api_aidl :
        (hidl >= RADIO_INTERFACE_1_5) ? &binder_netreg_api_hidl_1_5 :
        (hidl >= RADIO_INTERFACE_1_4) ? &binder_netreg_api_hidl_1_4 :
        (hidl >= RADIO_INTERFACE_1_2) ? &binder_netreg_api_hidl_1_2 :
        &binder_netreg_api_hidl;

    self->log_prefix = binder_dup_prefix(modem->log_prefix);

    DBG_(self, "%p %s", netreg, api->name);
    self->client = radio_client_ref(network_client);
    self->modem_client = radio_client_ref(modem->clients.modem_client);
    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->network = binder_network_ref(modem->network);
    self->netreg = netreg;
    self->techs = config->techs;
    self->use_network_scan = config->use_network_scan;
    self->replace_strange_oper = config->replace_strange_oper;
    self->prefer_lte_signal_strength = config->prefer_lte_signal_strength;
    self->network_selection_timeout_ms = config->network_selection_timeout_ms;
    self->api = api;

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

    gutil_source_remove(self->init_id);
    gutil_source_remove(self->notify_id);
    gutil_source_remove(self->current_operator_id);

    radio_request_drop(self->register_req);
    radio_request_drop(self->strength_req);

    ofono_watch_unref(self->watch);
    binder_network_remove_all_handlers(self->network, self->network_event_id);
    binder_network_unref(self->network);

    radio_client_remove_all_handlers(self->client, self->ind_id);
    radio_client_unref(self->client);

    radio_client_remove_all_handlers(self->modem_client, self->modem_ind_id);
    radio_client_unref(self->modem_client);

    binder_netreg_scan_drop(self, self->scan);
    g_free(self->log_prefix);
    g_free(self);

    ofono_netreg_set_data(netreg, NULL);
}

/*==========================================================================*
 * HIDL API flavor
 *==========================================================================*/

static
BinderOpList*
binder_netreg_api_read_oplist_hidl(
    BinderNetReg* self,
    GBinderReader* reader)
{
    /* vec<OperatorInfo> */
    gsize count;
    const RadioOperatorInfo* ops = gbinder_reader_read_hidl_type_vec(reader,
        RadioOperatorInfo, &count);

    if (ops) {
        BinderOpList* oplist = binder_oplist_sized_new(count);
        guint i;

        binder_oplist_set_count(oplist, count);
        for (i = 0; i < count; i++) {
            const RadioOperatorInfo* src = ops + i;
            struct ofono_network_operator* dest = oplist->op + i;

            /* Prefer long name */
            if (src->alphaLong.len) {
                g_strlcpy(dest->name, src->alphaLong.data.str,
                    sizeof(dest->name));
            } else if (src->alphaShort.len) {
                g_strlcpy(dest->name, src->alphaShort.data.str,
                    sizeof(dest->name));
            }

            /* enum ofono_operator_status and RADIO_OP_STATUS are identical */
            dest->status = (enum ofono_operator_status) src->status;
            dest->tech = self->network->voice.access_tech;
            binder_parse_mcc_mnc(src->operatorNumeric.data.str, dest);
            DBG("[operator=%s, %s, %s, %s, %s]",
                dest->name, dest->mcc, dest->mnc,
                binder_ofono_access_technology_string(dest->tech),
                binder_radio_op_status_string(src->status));
                binder_oplist_set_count(oplist, oplist->count + 1);
        }
        return oplist;
    }
    return NULL;
}

static
gboolean
binder_netreg_read_signal_strength_hidl(
    GBinderReader* reader,
    BinderRadioSignalStrength* out)
{
    const RadioSignalStrength* ss =
        gbinder_reader_read_hidl_struct(reader, RadioSignalStrength);

    if (ss) {
        out->gsm = &ss->gw;
        out->lte = &ss->lte;
        return TRUE;
    }
    return TRUE;
}

static
gboolean
binder_netreg_read_signal_strength_hidl_1_2(
    GBinderReader* reader,
    BinderRadioSignalStrength* out)
{
    const RadioSignalStrength_1_2* ss =
        gbinder_reader_read_hidl_struct(reader, RadioSignalStrength_1_2);

    if (ss) {
        out->gsm = &ss->gw;
        out->lte = &ss->lte;
        out->wcdma = &ss->wcdma;
        return TRUE;
    }
    return TRUE;
}

static
gboolean
binder_netreg_read_signal_strength_hidl_1_4(
    GBinderReader* reader,
    BinderRadioSignalStrength* out)
{
    const RadioSignalStrength_1_4* ss =
        gbinder_reader_read_hidl_struct(reader, RadioSignalStrength_1_4);

    if (ss) {
        out->gsm = &ss->gsm;
        out->lte = &ss->lte;
        out->wcdma = &ss->wcdma;
        out->tdscdma = &ss->tdscdma;
        out->nr = &ss->nr;
        return TRUE;
    }
    return FALSE;
}

static
void
binder_netreg_api_read_signal_strength_hidl(
    BinderNetReg* self,
    const BinderRadioSignalStrength* in,
    BinderSs* out)
{
    const RadioSignalStrengthGsm* gsm = in->gsm;
    const RadioSignalStrengthLte* lte = in->lte;
    const RadioSignalStrengthWcdma_1_2* wcdma = in->wcdma;
    const RadioSignalStrengthTdScdma_1_2* tdscdma  = in->tdscdma;
    const RadioSignalStrengthNr* nr = in->nr;

    if (gsm) {
        if (RXLEV_VALID(gsm->signalStrength)) {
            out->m[BINDER_SS_RXLEV] = gsm->signalStrength;
        }
    }

    if (lte) {
        if (RSSI_VALID(lte->signalStrength)) {
            out->m[BINDER_SS_RSSI] = lte->signalStrength;
        }

        if (RSRP_VALID(lte->rsrp)) {
            /* Value is dB multipled by -1 */
            out->m[BINDER_SS_RSRP] = -(int)lte->rsrp;
        }

        if (RSRQ_VALID(lte->rsrq)) {
            /* Value is dB multipled by -1 */
            out->m[BINDER_SS_RSRQ] = -(int)lte->rsrq;
        }

        if (RSSNR_VALID(lte->rssnr)) {
            out->m[BINDER_SS_RSSNR] = lte->rssnr;
        }
    }

    if (tdscdma) {
        const int rssi = tdscdma->signalStrength;
        const int rscp = tdscdma->rscp;

        if (RSSI_VALID(rssi)) {
            const int prev = out->m[BINDER_SS_RSSI];

            if (prev == NO_MEASUREMENT || prev > rssi) {
                DBG_(self, "RSSI %d => %d", prev, rssi);
                out->m[BINDER_SS_RSSI] = rssi;
            }
        }

        if (RSCP_VALID(rscp)) {
            out->m[BINDER_SS_RSCP] = rscp;
        }
    }

    if (wcdma) {
        const int rssi = wcdma->base.signalStrength;
        const int rscp = wcdma->rscp;

        if (RSSI_VALID(rssi)) {
            const int prev = out->m[BINDER_SS_RSSI];

            if (prev == NO_MEASUREMENT || prev > rssi) {
                DBG_(self, "RSSI %d => %d", prev, rssi);
                out->m[BINDER_SS_RSSI] = rssi;
            }
        }

        if (RSCP_VALID(rscp)) {
            const int prev = out->m[BINDER_SS_RSCP];

            if (prev == NO_MEASUREMENT || prev > rscp) {
                DBG_(self, "RSCP %d => %d", prev, rscp);
                out->m[BINDER_SS_RSCP] = rscp;
            }
        }
    }

    if (nr) {
        if (SSRSRP_VALID(nr->ssRsrp)) {
            /* Value is dBm multipled by -1 */
            out->m[BINDER_SS_SSRSRP] = -(int)nr->ssRsrp;
        }

        if (SSRSRQ_VALID(nr->ssRsrq)) {
            /* Value is dB multipled by -1 */
            out->m[BINDER_SS_SSRSRQ] = -(int)nr->ssRsrq;
        }

        if (SSSINR_VALID(nr->ssSinr)) {
            out->m[BINDER_SS_SSSINR] = nr->ssSinr;
        }
    }
}

static
gboolean
binder_netreg_api_read_signal_strength_resp_hidl(
    BinderNetReg* self,
    RADIO_RESP resp,
    GBinderReader* reader,
    BinderSs* out)
{
    BinderRadioSignalStrength ss;
    gboolean (*read_signal_strength)(
        GBinderReader* reader,
        BinderRadioSignalStrength* ss) = NULL;

    if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH) {
        read_signal_strength = binder_netreg_read_signal_strength_hidl;
    } else if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH_1_2) {
        read_signal_strength = binder_netreg_read_signal_strength_hidl_1_2;
    } else if (resp == RADIO_RESP_GET_SIGNAL_STRENGTH_1_4) {
        read_signal_strength = binder_netreg_read_signal_strength_hidl_1_4;
    } else {
        ofono_error("Unexpected getSignalStrength response %d", resp);
        return FALSE;
    }

    memset(&ss, 0, sizeof(ss));
    if (read_signal_strength(reader, &ss)) {
        binder_netreg_api_read_signal_strength_hidl(self, &ss, out);
        return TRUE;
    }

    return FALSE;
}

static
gboolean
binder_netreg_api_read_signal_strength_ind_hidl(
    BinderNetReg* self,
    RADIO_IND ind,
    GBinderReader* reader,
    BinderSs* out)
{
    BinderRadioSignalStrength ss;
    gboolean (*read_signal_strength)(
        GBinderReader* reader,
        BinderRadioSignalStrength* ss) = NULL;

    memset(&ss, 0, sizeof(ss));
    if (ind == RADIO_IND_CURRENT_SIGNAL_STRENGTH) {
        read_signal_strength = binder_netreg_read_signal_strength_hidl;
    } else if (ind == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_2) {
        read_signal_strength = binder_netreg_read_signal_strength_hidl_1_2;
    } else if (ind == RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4) {
        read_signal_strength = binder_netreg_read_signal_strength_hidl_1_4;
    } else {
        ofono_error("Unexpected currentSignalStrength indication %d", ind);
        return FALSE;
    }

    memset(&ss, 0, sizeof(ss));
    if (read_signal_strength(reader, &ss)) {
        binder_netreg_api_read_signal_strength_hidl(self, &ss, out);
        return TRUE;
    }

    return FALSE;
}

static
void
binder_netreg_api_write_set_network_selection_mode_manual_args_hidl(
    GBinderWriter* writer,
    const char* numeric)
{
    /*
     * 1.0/IRadio.hal:
     * oneway setNetworkSelectionModeManual(int32_t serial,
     *     string operatorNumeric);
     */
    gbinder_writer_append_hidl_string_copy(writer, numeric);
}

static const BinderNetRegApi binder_netreg_api_hidl = {
    "hidl",
    binder_read_string_arg_hidl,
    binder_netreg_api_read_oplist_hidl,
    binder_netreg_api_read_signal_strength_resp_hidl,
    binder_netreg_api_read_signal_strength_ind_hidl,
    NULL,
    NULL,
    binder_netreg_api_write_set_network_selection_mode_manual_args_hidl,
    RADIO_REQ_NONE,
    RADIO_REQ_STOP_NETWORK_SCAN,
    RADIO_REQ_GET_AVAILABLE_NETWORKS,
    RADIO_REQ_GET_SIGNAL_STRENGTH,
    RADIO_REQ_GET_NETWORK_SELECTION_MODE,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL,
    RADIO_IND_NITZ_TIME_RECEIVED,
    RADIO_IND_CURRENT_SIGNAL_STRENGTH,
    RADIO_IND_NONE,
    RADIO_IND_MODEM_RESET,
    RADIO_IND_NONE,
};

/*==========================================================================*
 * HIDL 1.2 API flavor
 *==========================================================================*/

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
BinderOpList*
binder_netreg_api_read_network_scan_hidl(
    BinderNetReg* self,
    RADIO_IND code,
    GBinderReader* reader,
    RADIO_SCAN_STATUS* scan_status)
{
    /*
     * Regardless of the interface version, it's always the same
     * RadioNetworkScanResult structure with networkInfos pointing
     * to different things.
     */
    BinderOpList* oplist = NULL;
    const RadioNetworkScanResult* result =
        gbinder_reader_read_hidl_struct(reader, RadioNetworkScanResult);

    if (result) {
        guint i;

        oplist = binder_oplist_sized_new(result->networkInfos.count);
        DBG_(self, "status=%d, error=%d, %u networks",
            result->status, result->error, result->networkInfos.count);

        if (code == RADIO_IND_NETWORK_SCAN_RESULT_1_2) {
            const RadioCellInfo_1_2* cells = result->networkInfos.data.ptr;

            for (i = 0; i < result->networkInfos.count; i++) {
                const RadioCellInfo_1_2* cell = cells + i;
                const RadioCellInfoGsm_1_2* gsm = cell->gsm.data.ptr;
                const RadioCellInfoWcdma_1_2* wcdma = cell->wcdma.data.ptr;
                const RadioCellInfoLte_1_2* lte = cell->lte.data.ptr;
                guint j;

                for (j = 0; j < cell->gsm.count; j++) {
                    binder_netreg_scan_op_convert_gsm(cell->registered,
                        &gsm[j].cellIdentityGsm,
                        binder_oplist_append_op(oplist));
                }
                for (j = 0; j < cell->wcdma.count; j++) {
                    binder_netreg_scan_op_convert_wcdma(cell->registered,
                        &wcdma[j].cellIdentityWcdma,
                        binder_oplist_append_op(oplist));
                }
                for (j = 0; j < cell->lte.count; j++) {
                    binder_netreg_scan_op_convert_lte(cell->registered,
                        &lte[j].cellIdentityLte,
                        binder_oplist_append_op(oplist));
                }
            }
        } else if (code == RADIO_IND_NETWORK_SCAN_RESULT_1_4) {
            const RadioCellInfo_1_4* cells = result->networkInfos.data.ptr;

            for (i = 0; i < result->networkInfos.count; i++) {
                const RadioCellInfo_1_4* cell = cells + i;

                switch ((RADIO_CELL_INFO_TYPE_1_4)cell->cellInfoType) {
                case RADIO_CELL_INFO_1_4_GSM:
                    binder_netreg_scan_op_convert_gsm(cell->registered,
                        &cell->info.gsm.cellIdentityGsm,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_4_WCDMA:
                    binder_netreg_scan_op_convert_wcdma(cell->registered,
                        &cell->info.wcdma.cellIdentityWcdma,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_4_LTE:
                    binder_netreg_scan_op_convert_lte(cell->registered,
                        &cell->info.lte.base.cellIdentityLte,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_4_NR:
                    binder_netreg_scan_op_convert_nr(cell->registered,
                        &cell->info.nr.cellIdentity,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_4_CDMA:
                case RADIO_CELL_INFO_1_4_TD_SCDMA:
                    break;
                }
            }
        } else if (code == RADIO_IND_NETWORK_SCAN_RESULT_1_5) {
            const RadioCellInfo_1_5* cells = result->networkInfos.data.ptr;

            for (i = 0; i < result->networkInfos.count; i++) {
                const RadioCellInfo_1_5* cell = cells + i;

                switch ((RADIO_CELL_INFO_TYPE_1_5)cell->cellInfoType) {
                case RADIO_CELL_INFO_1_5_GSM:
                    binder_netreg_scan_op_convert_gsm(cell->registered,
                        &cell->info.gsm.cellIdentityGsm.base,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_5_WCDMA:
                    binder_netreg_scan_op_convert_wcdma(cell->registered,
                        &cell->info.wcdma.cellIdentityWcdma.base,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_5_LTE:
                    binder_netreg_scan_op_convert_lte(cell->registered,
                        &cell->info.lte.cellIdentityLte.base,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_5_NR:
                    binder_netreg_scan_op_convert_nr(cell->registered,
                        &cell->info.nr.cellIdentityNr.base,
                        binder_oplist_append_op(oplist));
                    break;
                case RADIO_CELL_INFO_1_5_CDMA:
                case RADIO_CELL_INFO_1_5_TD_SCDMA:
                    break;
                }
            }
        }
    }
    return oplist;
}

static
void
binder_netreg_api_write_start_network_scan_args_hidl_1_2(
    BinderNetReg* self,
    GBinderWriter* writer)
{
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
        GBINDER_WRITER_FIELD_HIDL_VEC_INT32(RadioAccessSpecifier, geranBands),
        GBINDER_WRITER_FIELD_HIDL_VEC_INT32(RadioAccessSpecifier, utranBands),
        GBINDER_WRITER_FIELD_HIDL_VEC_INT32(RadioAccessSpecifier, eutranBands),
        GBINDER_WRITER_FIELD_HIDL_VEC_INT32(RadioAccessSpecifier, channels),
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
        GBINDER_WRITER_FIELD_HIDL_VEC(RadioNetworkScanRequest_1_2,
            specifiers, &radio_network_scan_specifier_t),
        GBINDER_WRITER_FIELD_HIDL_VEC_STRING(RadioNetworkScanRequest_1_2,
            mccMncs),
        GBINDER_WRITER_FIELD_END()
    };
    static const GBinderWriterType radio_network_scan_request_t = {
        GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioNetworkScanRequest_1_2),
        radio_network_scan_request_f
    };

    RadioAccessSpecifier* specs;
    RadioNetworkScanRequest_1_2* scan;
    guint i, nspecs = 0;
    const BinderNetRegRadioType* radio_types[N_RADIO_TYPES];

    /* Which modes are supported and enabled */
    for (i = 0; i < N_RADIO_TYPES; i++) {
        if (self->techs & binder_netreg_radio_types[i].mode) {
            radio_types[nspecs++] = binder_netreg_radio_types + i;
        }
    }

    scan = gbinder_writer_new0(writer, RadioNetworkScanRequest_1_2);
    specs = gbinder_writer_malloc0(writer, nspecs * sizeof(*specs));

    for (i = 0; i < nspecs; i++) {
        const BinderNetRegRadioType* radio_type = radio_types[i];
        RadioAccessSpecifier* spec = specs + i;

        spec->radioAccessNetwork = radio_type->ran;
        /* The rest may (hopefully) remain zero-initialized */
    }

    scan->type = RADIO_SCAN_ONE_SHOT;
    scan->interval = INCREMENTAL_RESULTS_PERIODICITY_RANGE_MAX;
    scan->specifiers.owns_buffer = TRUE;
    scan->specifiers.count = nspecs;
    scan->specifiers.data.ptr = specs;
    scan->maxSearchTime = NETWORK_SCAN_MAX_SEARCH_TIME_SEC;
    scan->incrementalResults = TRUE;
    scan->incrementalResultsPeriodicity = 3;
    gbinder_writer_append_struct(writer, scan,
        &radio_network_scan_request_t, NULL);
}

static const BinderNetRegApi binder_netreg_api_hidl_1_2 = {
    "hidl_1_2",
    binder_read_string_arg_hidl,
    binder_netreg_api_read_oplist_hidl,
    binder_netreg_api_read_signal_strength_resp_hidl,
    binder_netreg_api_read_signal_strength_ind_hidl,
    binder_netreg_api_read_network_scan_hidl,
    binder_netreg_api_write_start_network_scan_args_hidl_1_2,
    binder_netreg_api_write_set_network_selection_mode_manual_args_hidl,
    RADIO_REQ_START_NETWORK_SCAN_1_2,
    RADIO_REQ_STOP_NETWORK_SCAN,
    RADIO_REQ_GET_AVAILABLE_NETWORKS,
    RADIO_REQ_GET_SIGNAL_STRENGTH,
    RADIO_REQ_GET_NETWORK_SELECTION_MODE,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL,
    RADIO_IND_NITZ_TIME_RECEIVED,
    RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_2,
    RADIO_IND_NETWORK_SCAN_RESULT_1_2,
    RADIO_IND_MODEM_RESET,
    RADIO_IND_NONE,
};

/*==========================================================================*
 * HIDL 1.4 API flavor
 *==========================================================================*/

static const BinderNetRegApi binder_netreg_api_hidl_1_4 = {
    "hidl_1_4",
    binder_read_string_arg_hidl,
    binder_netreg_api_read_oplist_hidl,
    binder_netreg_api_read_signal_strength_resp_hidl,
    binder_netreg_api_read_signal_strength_ind_hidl,
    binder_netreg_api_read_network_scan_hidl,
    binder_netreg_api_write_start_network_scan_args_hidl_1_2,
    binder_netreg_api_write_set_network_selection_mode_manual_args_hidl,
    RADIO_REQ_START_NETWORK_SCAN_1_2,
    RADIO_REQ_STOP_NETWORK_SCAN,
    RADIO_REQ_GET_SIGNAL_STRENGTH_1_4,
    RADIO_REQ_GET_NETWORK_SELECTION_MODE,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL,
    RADIO_IND_NITZ_TIME_RECEIVED,
    RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4,
    RADIO_IND_NETWORK_SCAN_RESULT_1_4,
    RADIO_IND_MODEM_RESET,
    RADIO_IND_NONE,
};

/*==========================================================================*
 * HIDL 1.5 API flavor
 *==========================================================================*/

static
void
binder_netreg_api_write_start_network_scan_args_hidl_1_5(
    BinderNetReg* self,
    GBinderWriter* writer)
{
    /*
     * typedef struct radio_network_scan_specifier_1_5 {
     *     RADIO_ACCESS_NETWORKS radioAccessNetwork;
     *     guint8 type;
     *     GBinderHidlVec bands;    // vec<enum>
     *     GBinderHidlVec channels; // vec<int32_t>
     * } RadioAccessSpecifier_1_5;
     */
    static const GBinderWriterField radio_network_scan_specifier_1_5_f[] = {
        GBINDER_WRITER_FIELD_HIDL_VEC_INT32(RadioAccessSpecifier_1_5, bands),
        GBINDER_WRITER_FIELD_HIDL_VEC_INT32(RadioAccessSpecifier_1_5, channels),
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
        GBINDER_WRITER_FIELD_HIDL_VEC(RadioNetworkScanRequest_1_5, specifiers,
            &radio_network_scan_specifier_1_5_t),
        GBINDER_WRITER_FIELD_HIDL_VEC_STRING(RadioNetworkScanRequest_1_5,
            mccMncs),
        GBINDER_WRITER_FIELD_END()
    };
    static const GBinderWriterType radio_network_scan_request_1_5_t = {
        GBINDER_WRITER_STRUCT_NAME_AND_SIZE(RadioNetworkScanRequest_1_5),
        radio_network_scan_request_1_5_f
    };

    RadioAccessSpecifier_1_5* specs;
    RadioNetworkScanRequest_1_5* scan;
    guint i, nspecs = 0;
    const BinderNetRegRadioType* radio_types[N_RADIO_TYPES_1_5];

    /* Which modes are supported and enabled */
    for (i = 0; i < N_RADIO_TYPES_1_5; i++) {
        if (self->techs & binder_netreg_radio_types[i].mode) {
            radio_types[nspecs++] = binder_netreg_radio_types + i;
        }
    }

    specs = gbinder_writer_malloc0(writer, nspecs * sizeof(*specs));
    scan = gbinder_writer_new0(writer, RadioNetworkScanRequest_1_5);

    for (i = 0; i < nspecs; i++) {
        const BinderNetRegRadioType* radio_type = radio_types[i];
        RadioAccessSpecifier_1_5* spec = specs + i;

        specs[i].radioAccessNetwork = radio_type->ran;
        spec->type = radio_type->hidl_type;
        /* The rest may (hopefully) remain zero-initialized */
    }

    scan->type = RADIO_SCAN_ONE_SHOT;
    scan->interval = INCREMENTAL_RESULTS_PERIODICITY_RANGE_MAX;
    scan->specifiers.owns_buffer = TRUE;
    scan->specifiers.count = nspecs;
    scan->specifiers.data.ptr = specs;
    scan->maxSearchTime = NETWORK_SCAN_MAX_SEARCH_TIME_SEC;
    scan->incrementalResults = TRUE;
    scan->incrementalResultsPeriodicity = 3;
    gbinder_writer_append_struct(writer, scan,
        &radio_network_scan_request_1_5_t, NULL);
}

static
void
binder_netreg_api_write_set_network_selection_mode_manual_args_hidl_1_5(
    GBinderWriter* writer,
    const char* numeric)
{
    /*
     * 1.5/IRadio.hal:
     * oneway setNetworkSelectionModeManual_1_5(int32_t serial,
     *     string operatorNumeric, RadioAccessNetworks ran);
     */
    gbinder_writer_append_hidl_string_copy(writer, numeric);
    gbinder_writer_append_int32(writer, RADIO_ACCESS_NETWORKS_UNKNOWN);
}

static const BinderNetRegApi binder_netreg_api_hidl_1_5 = {
    "hidl_1_5",
    binder_read_string_arg_hidl,
    binder_netreg_api_read_oplist_hidl,
    binder_netreg_api_read_signal_strength_resp_hidl,
    binder_netreg_api_read_signal_strength_ind_hidl,
    binder_netreg_api_read_network_scan_hidl,
    binder_netreg_api_write_start_network_scan_args_hidl_1_5,
    binder_netreg_api_write_set_network_selection_mode_manual_args_hidl_1_5,
    RADIO_REQ_START_NETWORK_SCAN_1_4,
    RADIO_REQ_STOP_NETWORK_SCAN,
    RADIO_REQ_GET_AVAILABLE_NETWORKS,
    RADIO_REQ_GET_SIGNAL_STRENGTH_1_4,
    RADIO_REQ_GET_NETWORK_SELECTION_MODE,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
    RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL_1_5,
    RADIO_IND_NITZ_TIME_RECEIVED,
    RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4,
    RADIO_IND_NETWORK_SCAN_RESULT_1_5,
    RADIO_IND_MODEM_RESET,
    RADIO_IND_NONE,
};

/*==========================================================================*
 * AIDL API flavor
 *==========================================================================*/

static
gboolean
binder_netreg_scan_op_read_info_aidl(
    GBinderReader* reader,
    struct ofono_network_operator* op)
{
    gboolean ok = FALSE;
    GBinderReader info;

    /*
     * package android.hardware.radio.network;
     * parcelable OperatorInfo {
     *   String alphaLong;
     *   String alphaShort;
     *   String operatorNumeric;
     *   int status;
     * }
     */

    if (gbinder_reader_start_parcelable(reader, &info, NULL)) {
        char* name = gbinder_reader_read_string16(&info); /* alphaLong */
        gint32 status;

        /* Prefer long name, then short, fallback to numeric */
        if (name) {
            gbinder_reader_skip_string16(&info); /* alphaShort */
            gbinder_reader_skip_string16(&info); /* operatorNumeric */
        } else {
            name = gbinder_reader_read_string16(&info); /* alphaShort */
            /* operatorNumeric */
            if (name) {
                gbinder_reader_skip_string16(&info);
            } else {
                name = gbinder_reader_read_string16(&info);
            }
        }

        if (name) {
            g_strlcpy(op->name, name, sizeof(op->name));
            g_free(name);

            if (gbinder_reader_read_int32(&info, &status)) {
                op->status = status;
                ok = TRUE;
            }
        }
        gbinder_reader_finish_parcelable(&info);
    }
    return ok;
}

static
BinderOpList*
binder_netreg_api_read_oplist_aidl(
    BinderNetReg* self,
    GBinderReader* reader)
{
    guint32 count;

    /* OperatorInfo[] */
    if (gbinder_reader_read_uint32(reader, &count)) {
        BinderOpList* oplist = binder_oplist_sized_new(count);
        guint i;

        for (i = 0; i < count; i++) {
            binder_oplist_set_count(oplist, oplist->count + 1);
            if (!binder_netreg_scan_op_read_info_aidl(reader,
                binder_oplist_last(oplist))) {
                DBG("Failed to parse OperatorInfo[]");
                binder_oplist_free(oplist);
                return NULL;
            }
        }
        return oplist;
    }
    return NULL;
}

static
gboolean
binder_netreg_op_cell_info_gsm_aidl(
    GBinderReader* reader,
    struct ofono_network_operator* op)
{
    gboolean ok = FALSE;
    GBinderReader ident;

    /*
     * package android.hardware.radio.network;
     * parcelable CellInfoGsm {
     *   // <== The reader points here
     *   CellIdentityGsm cellIdentityGsm;
     *   GsmSignalStrength signalStrengthGsm;
     * }
     */

    /* cellIdentityGsm */
    if (gbinder_reader_start_parcelable(reader, &ident, NULL)) {

        /*
         * package android.hardware.radio.network;
         * parcelable CellIdentityGsm {
         *   String mcc;
         *   String mnc;
         *   int lac;
         *   int cid;
         *   int arfcn;
         *   byte bsic;
         *   OperatorInfo operatorNames;
         *   String[] additionalPlmns;
         * }
         */
        char* mcc = gbinder_reader_read_string16(&ident);

        if (mcc) {
            char* mnc = gbinder_reader_read_string16(&ident);

            if (mnc) {
                if (gbinder_reader_read_int32(&ident, NULL /* lac */) &&
                    gbinder_reader_read_int32(&ident, NULL /* cid */) &&
                    gbinder_reader_read_int32(&ident, NULL /* arfcn */) &&
                    gbinder_reader_read_int8(&ident, NULL /* bsic */) &&
                    binder_netreg_scan_op_read_info_aidl(&ident, op)) {
                    /* That's enough info to call it a success */
                    op->tech = OFONO_ACCESS_TECHNOLOGY_GSM;
                    g_strlcpy(op->mcc, mcc, sizeof(op->mcc));
                    g_strlcpy(op->mnc, mnc, sizeof(op->mnc));
                    ok = TRUE;
                }
                g_free(mnc);
            }
            g_free(mcc);
        }
        gbinder_reader_finish_parcelable(&ident);
    }
    return ok;
}

static
gboolean
binder_netreg_op_cell_info_wcdma_aidl(
    GBinderReader* reader,
    struct ofono_network_operator* op)
{
    gboolean ok = FALSE;
    GBinderReader ident;

    /*
     * package android.hardware.radio.network;
     * parcelable CellInfoWcdma {
     *   // <== The reader points here
     *   CellIdentityWcdma cellIdentityWcdma;
     *   WcdmaSignalStrength signalStrengthWcdma;
     */

    /* cellIdentityWcdma */
    if (gbinder_reader_start_parcelable(reader, &ident, NULL)) {
        /*
         * package android.hardware.radio.network;
         * parcelable CellIdentityWcdma {
         *   String mcc;
         *   String mnc;
         *   int lac;
         *   int cid;
         *   int psc;
         *   int uarfcn;
         *   OperatorInfo operatorNames;
         *   String[] additionalPlmns;
         *   ClosedSubscriberGroupInfo csgInfo;
         * }
         */
        char* mcc = gbinder_reader_read_string16(&ident);

        if (mcc) {
            char* mnc = gbinder_reader_read_string16(&ident);

            if (mnc) {
                if (gbinder_reader_read_int32(&ident, NULL /* lac */) &&
                    gbinder_reader_read_int32(&ident, NULL /* cid */) &&
                    gbinder_reader_read_int32(&ident, NULL /* psc */) &&
                    gbinder_reader_read_int32(&ident, NULL /* uarfcn */) &&
                    binder_netreg_scan_op_read_info_aidl(&ident, op)) {
                    /* That's enough info to call it a success */
                    op->tech = OFONO_ACCESS_TECHNOLOGY_UTRAN;
                    g_strlcpy(op->mcc, mcc, sizeof(op->mcc));
                    g_strlcpy(op->mnc, mnc, sizeof(op->mnc));
                    ok = TRUE;
                }
                g_free(mnc);
            }
            g_free(mcc);
        }
        gbinder_reader_finish_parcelable(&ident);
    }
    return ok;
}

static
gboolean
binder_netreg_op_cell_info_lte_aidl(
    GBinderReader* reader,
    struct ofono_network_operator* op)
{
    gboolean ok = FALSE;
    GBinderReader ident;

    /*
     * package android.hardware.radio.network;
     * parcelable CellInfoLte {
     *   // <== The reader points here
     *   CellIdentityLte cellIdentityLte;
     *   LteSignalStrength signalStrengthLte;
     * }
     */

    /* cellIdentityLte */
    if (gbinder_reader_start_parcelable(reader, &ident, NULL)) {
        /*
         * package android.hardware.radio.network;
         * parcelable CellIdentityLte {
         *   String mcc;
         *   String mnc;
         *   int ci;
         *   int pci;
         *   int tac;
         *   int earfcn;
         *   OperatorInfo operatorNames;
         *   int bandwidth;
         *   String[] additionalPlmns;
         *   @ClosedSubscriberGroupInfo csgInfo;
         *   EutranBands[] bands;
         * }
         */
        char* mcc = gbinder_reader_read_string16(&ident);

        if (mcc) {
            char* mnc = gbinder_reader_read_string16(&ident);

            if (mnc) {
                if (gbinder_reader_read_int32(&ident, NULL /* ci */) &&
                    gbinder_reader_read_int32(&ident, NULL /* pci */) &&
                    gbinder_reader_read_int32(&ident, NULL /* tac */) &&
                    gbinder_reader_read_int32(&ident, NULL /* earfcn */) &&
                    binder_netreg_scan_op_read_info_aidl(&ident, op)) {
                    /* That's enough info to call it a success */
                    op->tech = OFONO_ACCESS_TECHNOLOGY_EUTRAN;
                    g_strlcpy(op->mcc, mcc, sizeof(op->mcc));
                    g_strlcpy(op->mnc, mnc, sizeof(op->mnc));
                    ok = TRUE;
                }
                g_free(mnc);
            }
            g_free(mcc);
        }
        gbinder_reader_finish_parcelable(&ident);
    }
    return ok;
}

static
gboolean
binder_netreg_op_cell_info_nr_aidl(
    GBinderReader* reader,
    struct ofono_network_operator* op)
{
    gboolean ok = FALSE;
    GBinderReader ident;

    /*
     * package android.hardware.radio.network;
     * parcelable CellInfoNr {
     *   // <== The reader points here
     *   CellIdentityNr cellIdentityNr;
     *   NrSignalStrength signalStrengthNr;
     * }
     */

    /* cellIdentityNr */
    if (gbinder_reader_start_parcelable(reader, &ident, NULL)) {
        /*
         * package android.hardware.radio.network;
         * parcelable CellIdentityNr {
         *   String mcc;
         *   String mnc;
         *   long nci;
         *   int pci;
         *   int tac;
         *   int nrarfcn;
         *   OperatorInfo operatorNames;
         *   String[] additionalPlmns;
         *   NgranBands[] bands;
         * }
         */
        char* mcc = gbinder_reader_read_string16(&ident);

        if (mcc) {
            char* mnc = gbinder_reader_read_string16(&ident);

            if (mnc) {
                if (gbinder_reader_read_int64(&ident, NULL /* nci */) &&
                    gbinder_reader_read_int32(&ident, NULL /* pci */) &&
                    gbinder_reader_read_int32(&ident, NULL /* tac */) &&
                    gbinder_reader_read_int32(&ident, NULL /* nrarfcn */) &&
                    binder_netreg_scan_op_read_info_aidl(&ident, op)) {
                    /* That's enough info to call it a success */
                    op->tech = OFONO_ACCESS_TECHNOLOGY_NG_RAN;
                    g_strlcpy(op->mcc, mcc, sizeof(op->mcc));
                    g_strlcpy(op->mnc, mnc, sizeof(op->mnc));
                    ok = TRUE;
                }
                g_free(mnc);
            }
            g_free(mcc);
        }
        gbinder_reader_finish_parcelable(&ident);
    }
    return ok;
}

static
gboolean
binder_netreg_api_read_signal_strength_aidl(
    BinderNetReg* self,
    GBinderReader* reader,
    BinderSs* out)
{
    GBinderReader parcel;
    gboolean ok = TRUE;

    /*
     * package android.hardware.radio.network;
     * parcelable SignalStrength {
     *   GsmSignalStrength gsm;
     *   CdmaSignalStrength cdma;
     *   EvdoSignalStrength evdo;
     *   LteSignalStrength lte;
     *   TdscdmaSignalStrength tdscdma;
     *   WcdmaSignalStrength wcdma;
     *   NrSignalStrength nr;
     * }
     */
    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        GBinderReader gsm, lte, tdscdma, wcdma, nr;

        /* gsm */
        if (gbinder_reader_start_parcelable(&parcel, &gsm, NULL)) {
            gint32 signalStrength;

            /*
             * package android.hardware.radio.network;
             * parcelable GsmSignalStrength {
             *   int signalStrength;
             *   int bitErrorRate;
             *   int timingAdvance;
             * }
             */
            if (gbinder_reader_read_int32(&gsm, &signalStrength)) {
                if (RXLEV_VALID(signalStrength)) {
                    out->m[BINDER_SS_RXLEV] = signalStrength;
                }
            } else {
                ok = FALSE;
            }

            gbinder_reader_finish_parcelable(&gsm);
        } else {
            ok = FALSE;
        }

        if (ok && gbinder_reader_skip_parcelable(&parcel) /* cdma */ &&
            gbinder_reader_skip_parcelable(&parcel) /* evdo */ &&
            gbinder_reader_start_parcelable(&parcel, &lte, NULL)) {
            gint32 signalStrength, rsrp, rsrq, rssnr;

            /*
             * package android.hardware.radio.network;
             * parcelable LteSignalStrength {
             *   int signalStrength;
             *   int rsrp;
             *   int rsrq;
             *   int rssnr;
             *   int cqi;
             *   int timingAdvance;
             *   int cqiTableIndex;
             * }
             */
            if (gbinder_reader_read_int32(&lte, &signalStrength) &&
                gbinder_reader_read_int32(&lte, &rsrp) &&
                gbinder_reader_read_int32(&lte, &rsrq) &&
                gbinder_reader_read_int32(&lte, &rssnr)) {
                if (RSSI_VALID(signalStrength)) {
                    out->m[BINDER_SS_RSSI] = signalStrength;
                }

                if (RSRP_VALID(rsrp)) {
                    /* Value is dB multipled by -1 */
                    out->m[BINDER_SS_RSRP] = -rsrp;
                }

                if (RSRQ_VALID(rsrq)) {
                    /* Value is dB multipled by -1 */
                    out->m[BINDER_SS_RSRQ] = -rsrq;
                }

                if (RSSNR_VALID(rssnr)) {
                    out->m[BINDER_SS_RSSNR] = rssnr;
                }
            } else {
                ok = FALSE;
            }

            gbinder_reader_finish_parcelable(&lte);
        } else {
            ok = FALSE;
        }

        /* tdscdma */
        if (ok && gbinder_reader_start_parcelable(&parcel, &tdscdma, NULL)) {
            gint32 signalStrength, rscp;

            /*
             * package android.hardware.radio.network;
             * parcelable TdscdmaSignalStrength {
             *   int signalStrength;
             *   int bitErrorRate;
             *   int rscp;
             * }
             */
            if (gbinder_reader_read_int32(&tdscdma, &signalStrength) &&
                gbinder_reader_read_int32(&tdscdma, NULL /* bitErrorRate */) &&
                gbinder_reader_read_int32(&tdscdma, &rscp)) {
                if (RSSI_VALID(signalStrength)) {
                    const int prev = out->m[BINDER_SS_RSSI];

                    if (prev == NO_MEASUREMENT || prev > signalStrength) {
                        DBG_(self, "RSSI %d => %d", prev, signalStrength);
                        out->m[BINDER_SS_RSSI] = signalStrength;
                    }
                }

                if (RSCP_VALID(rscp)) {
                    out->m[BINDER_SS_RSCP] = rscp;
                }
            } else {
                ok = FALSE;
            }

            gbinder_reader_finish_parcelable(&tdscdma);
        } else {
            ok =  FALSE;
        }

        /* wcdma */
        if (ok && gbinder_reader_start_parcelable(&parcel, &wcdma, NULL)) {
            gint32 signalStrength, rscp;

            /*
             * package android.hardware.radio.network;
             * parcelable WcdmaSignalStrength {
             *   int signalStrength;
             *   int bitErrorRate;
             *   int rscp;
             *   int ecno;
             * }
             */
            if (gbinder_reader_read_int32(&wcdma, &signalStrength) &&
                gbinder_reader_read_int32(&wcdma, NULL /* bitErrorRate */) &&
                gbinder_reader_read_int32(&wcdma, &rscp)) {
                if (RSSI_VALID(signalStrength)) {
                    const int prev = out->m[BINDER_SS_RSSI];

                    if (prev == NO_MEASUREMENT || prev > signalStrength) {
                        DBG_(self, "RSSI %d => %d", prev, signalStrength);
                        out->m[BINDER_SS_RSSI] = signalStrength;
                    }
                }

                if (RSCP_VALID(rscp)) {
                    const int prev = out->m[BINDER_SS_RSCP];

                    if (prev == NO_MEASUREMENT || prev > rscp) {
                        DBG_(self, "RSCP %d => %d", prev, rscp);
                        out->m[BINDER_SS_RSCP] = rscp;
                    }
                }
            } else {
                ok = FALSE;
            }

            gbinder_reader_finish_parcelable(&wcdma);
        } else {
            ok = FALSE;
        }

        /* nr */
        if (ok && gbinder_reader_start_parcelable(&parcel, &nr, NULL)) {
            gint32 ssRsrp, ssRsrq, ssSinr;

            /*
             * package android.hardware.radio.network;
             * parcelable NrSignalStrength {
             *   int ssRsrp;
             *   int ssRsrq;
             *   int ssSinr;
             *   int csiRsrp;
             *   int csiRsrq;
             *   int csiSinr;
             *   int csiCqiTableIndex;
             *   byte[] csiCqiReport;
             *   int timingAdvance; // Since NrSignalStrength v3
             * }
             */
            if (gbinder_reader_read_int32(&nr, &ssRsrp) &&
                gbinder_reader_read_int32(&nr, &ssRsrq) &&
                gbinder_reader_read_int32(&nr, &ssSinr)) {

                if (SSRSRP_VALID(ssRsrp)) {
                    /* Value is dBm multipled by -1 */
                    out->m[BINDER_SS_SSRSRP] = -ssRsrp;
                }

                if (SSRSRQ_VALID(ssRsrq)) {
                    /* Value is dB multipled by -1 */
                    out->m[BINDER_SS_SSRSRQ] = -ssRsrq;
                }

                if (SSSINR_VALID(ssSinr)) {
                    out->m[BINDER_SS_SSSINR] = ssSinr;
                }
            } else {
                ok = FALSE;
            }

            gbinder_reader_finish_parcelable(&nr);
        } else {
            ok = FALSE;
        }

        gbinder_reader_finish_parcelable(&parcel);
    } else {
        ok = FALSE;
    }
    return ok;
}

static
int
binder_netreg_api_read_signal_strength_resp_aidl(
    BinderNetReg* self,
    RADIO_RESP resp,
    GBinderReader* reader,
    BinderSs* ss)
{
    return binder_netreg_api_read_signal_strength_aidl(self, reader, ss);
}

static
int
binder_netreg_api_read_signal_strength_ind_aidl(
    BinderNetReg* self,
    RADIO_IND ind,
    GBinderReader* reader,
    BinderSs* ss)
{
    return binder_netreg_api_read_signal_strength_aidl(self, reader, ss);
}

static
BinderOpList*
binder_netreg_api_read_network_scan_aidl(
    BinderNetReg* self,
    RADIO_IND code,
    GBinderReader* reader,
    RADIO_SCAN_STATUS* scan_status)
{
    BinderOpList* oplist = NULL;
    GBinderReader parcel;

    /*
     * package android.hardware.radio.network;
     * parcelable NetworkScanResult {
     *   int status;
     *   RadioError error;
     *   CellInfo[] networkInfos;
     * }
     *
     * parcelable CellInfo {
     *   boolean registered;
     *   CellConnectionStatus connectionStatus;
     *   CellInfoRatSpecificInfo ratSpecificInfo;
     * }
     *
     * union CellInfoRatSpecificInfo {
     *   CellInfoGsm gsm;
     *   CellInfoWcdma wcdma;
     *   CellInfoTdscdma tdscdma;
     *   CellInfoLte lte;
     *   CellInfoNr nr;
     *   CellInfoCdma cdma;
     * }
     */
    if (gbinder_reader_start_parcelable(reader, &parcel, NULL)) {
        gint32 status, error, count;

        if (gbinder_reader_read_int32(&parcel, &status) &&
            gbinder_reader_read_int32(&parcel, &error) &&
            gbinder_reader_read_int32(&parcel, &count)) {
            int i;

            oplist = binder_oplist_sized_new(count);
            DBG_(self, "status=%d, error=%d, %u networks",
                status, error, count);

            /* networkInfos */
            for (i = 0; i < count; i++) {
                GBinderReader info;

                if (gbinder_reader_start_parcelable(&parcel, &info, NULL)) {
                    gboolean registered;
                    gint32 type;
                    gboolean (*parse)(
                        GBinderReader* reader,
                        struct ofono_network_operator* op) = NULL;

                    if (gbinder_reader_read_bool(&info, &registered) &&
                        gbinder_reader_read_int32(&info, NULL) &&
                        binder_read_aidl_union_tag(&info, &type)) {
                        switch ((NETWORK_CELL_INFO_TYPE)type) {
                        case NETWORK_CELL_INFO_GSM:
                            parse = binder_netreg_op_cell_info_gsm_aidl;
                            break;
                        case NETWORK_CELL_INFO_WCDMA:
                            parse = binder_netreg_op_cell_info_wcdma_aidl;
                            break;
                        case NETWORK_CELL_INFO_LTE:
                            parse = binder_netreg_op_cell_info_lte_aidl;
                            break;
                        case NETWORK_CELL_INFO_NR:
                            parse = binder_netreg_op_cell_info_nr_aidl;
                            break;
                        case NETWORK_CELL_INFO_TDSCDMA:
                        case NETWORK_CELL_INFO_CDMA:
                            break;
                        }

                        if (parse) {
                            GBinderReader data;

                            if (gbinder_reader_start_parcelable(&info,
                                &data, NULL)) {
                                struct ofono_network_operator* op =
                                    binder_oplist_last(oplist =
                                        binder_oplist_append(oplist, NULL));

                                if (parse(&data, op)) {
                                    DBG_(self, "[registered=%d, operator=%s, "
                                        "%s, %s, %s, %s]", registered,
                                        op->name, op->mcc, op->mnc,
                                        binder_ofono_access_technology_string
                                        (op->tech),
                                        binder_radio_op_status_string
                                        (op->status));
                                } else {
                                    /* Drop the last added operator */
                                    DBG_(self, "error parsing cell type %d",
                                        type);
                                    binder_oplist_set_count(oplist,
                                        oplist->count - 1);
                                }
                                gbinder_reader_finish_parcelable(&data);
                            }
                        } else {
                            DBG("unsupported cell type %d", type);
                            gbinder_reader_skip_parcelable(&info);
                        }
                    }
                    gbinder_reader_finish_parcelable(&info);
                }
            }
            *scan_status = status;
        }
        gbinder_reader_finish_parcelable(&parcel);
    }
    return oplist;
}

static
void
binder_netreg_api_write_start_network_scan_args_aidl(
    BinderNetReg* self,
    GBinderWriter* writer)
{
    GBinderWriter parcel;
    guint i, nspecs = 0;
    const BinderNetRegRadioType* radio_types[N_RADIO_TYPES_AIDL];

    /* Which modes are supported and enabled */
    for (i = 0; i < N_RADIO_TYPES_AIDL; i++) {
        if (self->techs & binder_netreg_radio_types[i].mode) {
            radio_types[nspecs++] = binder_netreg_radio_types + i;
        }
    }

    /*
     * package android.hardware.radio.network;
     * parcelable NetworkScanRequest {
     *   int type;
     *   int interval;
     *   RadioAccessSpecifier[] specifiers;
     *   int maxSearchTime;
     *   boolean incrementalResults;
     *   int incrementalResultsPeriodicity;
     *   String[] mccMncs;
     * }
     */

    gbinder_writer_start_parcelable(writer, &parcel);

    gbinder_writer_append_int32(&parcel, RADIO_SCAN_ONE_SHOT); /* type */
    gbinder_writer_append_int32(&parcel, /* interval */
        INCREMENTAL_RESULTS_PERIODICITY_RANGE_MAX);

    /* specifiers */
    gbinder_writer_append_int32(&parcel, nspecs);
    for (i = 0; i < nspecs; i++) {
        const BinderNetRegRadioType* radio_type = radio_types[i];
        GBinderWriter spec;

        /*
         * parcelable RadioAccessSpecifier {
         *   AccessNetwork accessNetwork;
         *   RadioAccessSpecifierBands bands;
         *   int[] channels;
         * }
         *
         * union RadioAccessSpecifierBands {
         *   boolean noinit;
         *   GeranBands[] geranBands;
         *   UtranBands[] utranBands;
         *   EutranBands[] eutranBands;
         *   NgranBands[] ngranBands;
         * }
         */
        gbinder_writer_start_parcelable(&parcel, &spec);
        gbinder_writer_append_int32(&spec, radio_type->ran); /* accessNetwork */
        binder_append_aidl_union_tag(&spec, radio_type->aidl_type);
        gbinder_writer_append_int32(&spec, 0); /* bands (empty) */
        gbinder_writer_append_int32(&spec, 0); /* channels (empty)*/
        gbinder_writer_finish_parcelable(&spec);
    }

    gbinder_writer_append_int32(&parcel, /* maxSearchTime */
        NETWORK_SCAN_MAX_SEARCH_TIME_SEC);
    gbinder_writer_append_bool(&parcel, TRUE); /* incrementalResults */
    gbinder_writer_append_int32(&parcel, 3); /* incrementalResultsPeriodicity */
    gbinder_writer_append_int32(&parcel, 0); /* mccMncs (empty) */
    gbinder_writer_finish_parcelable(&parcel);
}

static
void
binder_netreg_api_write_set_network_selection_mode_manual_args_aidl(
    GBinderWriter* writer,
    const char* numeric)
{
    /*
     * IRadioNetwork.aidl:
     * void setNetworkSelectionModeManual(in int serial,
     *     in String operatorNumeric, in AccessNetwork ran);
     */
    gbinder_writer_append_string16(writer, numeric);
    gbinder_writer_append_int32(writer, RADIO_ACCESS_NETWORKS_UNKNOWN);
}

static const BinderNetRegApi binder_netreg_api_aidl = {
    "aidl",
    binder_read_string_arg_aidl,
    binder_netreg_api_read_oplist_aidl,
    binder_netreg_api_read_signal_strength_resp_aidl,
    binder_netreg_api_read_signal_strength_ind_aidl,
    binder_netreg_api_read_network_scan_aidl,
    binder_netreg_api_write_start_network_scan_args_aidl,
    binder_netreg_api_write_set_network_selection_mode_manual_args_aidl,
    RADIO_NETWORK_REQ_START_NETWORK_SCAN,
    RADIO_NETWORK_REQ_STOP_NETWORK_SCAN,
    RADIO_NETWORK_REQ_GET_AVAILABLE_NETWORKS,
    RADIO_NETWORK_REQ_GET_SIGNAL_STRENGTH,
    RADIO_NETWORK_REQ_GET_NETWORK_SELECTION_MODE,
    RADIO_NETWORK_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
    RADIO_NETWORK_REQ_SET_NETWORK_SELECTION_MODE_MANUAL,
    RADIO_NETWORK_IND_NITZ_TIME_RECEIVED,
    RADIO_NETWORK_IND_CURRENT_SIGNAL_STRENGTH,
    RADIO_NETWORK_IND_NETWORK_SCAN_RESULT,
    RADIO_IND_NONE,
    RADIO_MODEM_IND_MODEM_RESET,
};

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
