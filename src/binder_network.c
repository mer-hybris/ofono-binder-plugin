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

#include "binder_base.h"
#include "binder_data.h"
#include "binder_log.h"
#include "binder_network.h"
#include "binder_radio.h"
#include "binder_radio_caps.h"
#include "binder_sim_card.h"
#include "binder_sim_settings.h"
#include "binder_util.h"

#include <ofono/netreg.h>
#include <ofono/watch.h>
#include <ofono/misc.h>
#include <ofono/gprs.h>
#include <ofono/log.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

#define SET_PREF_MODE_HOLDOFF_SEC BINDER_RETRY_SECS
#define INTINITE_TIMEOUT UINT_MAX
#define MAX_DATA_CALLS 16

typedef enum binder_network_timer {
    TIMER_SET_RAT_HOLDOFF,
    TIMER_FORCE_CHECK_PREF_MODE,
    TIMER_COUNT
} BINDER_NETWORK_TIMER;

enum binder_network_radio_event {
    RADIO_EVENT_STATE_CHANGED,
    RADIO_EVENT_ONLINE_CHANGED,
    RADIO_EVENT_COUNT
};

enum binder_network_radio_caps_mgr_events {
    RADIO_CAPS_MGR_TX_DONE,
    RADIO_CAPS_MGR_TX_ABORTED,
    RADIO_CAPS_MGR_EVENT_COUNT
};

enum binder_network_sim_events {
    SIM_EVENT_STATUS_CHANGED,
    SIM_EVENT_IO_ACTIVE_CHANGED,
    SIM_EVENT_COUNT
};

enum binder_network_ind_events {
    IND_NETWORK_STATE,
    IND_MODEM_RESET,
    IND_CURRENT_PHYSICAL_CHANNEL_CONFIGS_1_4,
    IND_COUNT
};

enum binder_network_watch_event {
    WATCH_EVENT_GPRS,
    WATCH_EVENT_GPRS_SETTINGS,
    WATCH_EVENT_COUNT
};

typedef struct binder_network_location {
    int lac;
    int ci;
} BinderNetworkLocation;

typedef struct binder_network_data_profile {
    RADIO_DATA_PROFILE_ID id;
    RADIO_DATA_PROFILE_TYPE type;
    const char* apn;
    const char* username;
    const char* password;
    enum ofono_gprs_auth_method auth_method;
    enum ofono_gprs_proto proto;
    int max_conns_time;
    int max_conns;
    int wait_time;
    gboolean enabled;
} BinderNetworkDataProfile;

typedef struct binder_network_object {
    BinderBase base;
    BinderNetwork pub;
    RadioRequestGroup* g;
    BinderRadio* radio;
    BinderRadioCaps* caps;
    BinderSimCard* simcard;
    struct ofono_watch* watch;
    RADIO_ACCESS_FAMILY raf;
    RADIO_PREF_NET_TYPE rat;
    RADIO_PREF_NET_TYPE lte_network_mode;
    RADIO_PREF_NET_TYPE umts_network_mode;
    gboolean nr_connected;
    int network_mode_timeout_ms;
    char* log_prefix;
    RadioRequest* operator_poll_req;
    RadioRequest* voice_poll_req;
    RadioRequest* data_poll_req;
    RadioRequest* query_rat_req;
    RadioRequest* set_rat_req;
    RadioRequest* set_data_profiles_req;
    RadioRequest* set_ia_apn_req;
    guint timer[TIMER_COUNT];
    gulong ind_id[IND_COUNT];
    gulong settings_event_id;
    gulong caps_raf_event_id;
    gulong caps_mgr_event_id[RADIO_CAPS_MGR_EVENT_COUNT];
    gulong radio_event_id[RADIO_EVENT_COUNT];
    gulong simcard_event_id[SIM_EVENT_COUNT];
    gulong watch_ids[WATCH_EVENT_COUNT];
    gboolean need_initial_attach_apn;
    gboolean set_initial_attach_apn;
    struct ofono_network_operator operator;
    gboolean assert_rat;
    gboolean force_gsm_when_radio_off;
    BinderDataProfileConfig data_profile_config;
    GSList* data_profiles;
} BinderNetworkObject;

typedef BinderBaseClass BinderNetworkObjectClass;
GType binder_network_object_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderNetworkObject, binder_network_object, BINDER_TYPE_BASE)
#define PARENT_CLASS binder_network_object_parent_class
#define THIS_TYPE binder_network_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj,THIS_TYPE,BinderNetworkObject)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

/* Some assumptions: */
BINDER_BASE_ASSERT_COUNT(BINDER_NETWORK_PROPERTY_COUNT);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_ANY == 0);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_GSM > OFONO_RADIO_ACCESS_MODE_ANY);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_UMTS > OFONO_RADIO_ACCESS_MODE_GSM);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_LTE > OFONO_RADIO_ACCESS_MODE_UMTS);
G_STATIC_ASSERT(OFONO_RADIO_ACCESS_MODE_NR > OFONO_RADIO_ACCESS_MODE_LTE);

static
void
binder_network_query_pref_mode(
    BinderNetworkObject* self);

static
void
binder_network_check_pref_mode(
    BinderNetworkObject* self,
    gboolean immediate);

static
void
binder_network_check_initial_attach_apn(
    BinderNetworkObject* self);

static inline BinderNetworkObject* binder_network_cast(BinderNetwork* net)
    { return net ? THIS(G_CAST(net, BinderNetworkObject, pub)) : NULL; }
static inline void binder_network_object_ref(BinderNetworkObject* self)
    { g_object_ref(self); }
static inline void binder_network_object_unref(BinderNetworkObject* self)
    { g_object_unref(self); }

static
void
binder_network_stop_timer(
    BinderNetworkObject* self,
    BINDER_NETWORK_TIMER tid)
{
    if (self->timer[tid]) {
        g_source_remove(self->timer[tid]);
        self->timer[tid] = 0;
    }
}

static
void
binder_network_reset_state(
    BinderRegistrationState* reg)
{
    memset(reg, 0, sizeof(*reg));
    reg->status = OFONO_NETREG_STATUS_NONE;
    reg->access_tech = OFONO_ACCESS_TECHNOLOGY_NONE;
    reg->radio_tech = RADIO_TECH_UNKNOWN;
    reg->lac = -1;
    reg->ci = -1;
}

static
struct ofono_network_operator*
binder_network_op_copy(
    struct ofono_network_operator* dest,
    const struct ofono_network_operator* src)
{
    g_strlcpy(dest->mcc, src->mcc, sizeof(dest->mcc));
    g_strlcpy(dest->mnc, src->mnc, sizeof(dest->mnc));
    g_strlcpy(dest->name, src->name, sizeof(dest->name));
    dest->status = src->status;
    dest->tech = src->tech;
    return dest;
}

static
gboolean
binder_network_op_equal(
    const struct ofono_network_operator* op1,
    const struct ofono_network_operator* op2)
{
    if (op1 == op2) {
        return TRUE;
    } else if (!op1 || !op2) {
        return FALSE;
    } else {
        return op1->status == op2->status &&
            op1->tech == op2->tech &&
            !strncmp(op1->mcc, op2->mcc, sizeof(op2->mcc)) &&
            !strncmp(op1->mnc, op2->mnc, sizeof(op2->mnc)) &&
            !strncmp(op1->name, op2->name, sizeof(op2->name));
    }
}

static
void
binder_network_poll_operator_ok(
    BinderNetworkObject* self,
    GBinderReader* reader)
{
    /*
     * getOperatorResponse(RadioResponseInfo, string longName,
     *     string shortName, string numeric);
     */
    const char* lalpha = gbinder_reader_read_hidl_string_c(reader);
    const char* salpha = gbinder_reader_read_hidl_string_c(reader);
    const char* numeric = gbinder_reader_read_hidl_string_c(reader);
    BinderNetwork* net = &self->pub;
    struct ofono_network_operator op;
    gboolean changed = FALSE;

    memset(&op, 0, sizeof(op));
    op.tech = OFONO_ACCESS_TECHNOLOGY_NONE;
    if (binder_parse_mcc_mnc(numeric, &op)) {
        if (op.tech == OFONO_ACCESS_TECHNOLOGY_NONE) {
            op.tech = net->voice.access_tech;
        }
        op.status = OFONO_OPERATOR_STATUS_CURRENT;
        if (lalpha) {
            g_strlcpy(op.name, lalpha, sizeof(op.name));
        } else if (salpha) {
            g_strlcpy(op.name, salpha, sizeof(op.name));
        } else {
            g_strlcpy(op.name, numeric, sizeof(op.name));
        }
        if (!net->operator) {
            net->operator = binder_network_op_copy(&self->operator, &op);
            changed = TRUE;
        } else if (!binder_network_op_equal(&op, &self->operator)) {
            binder_network_op_copy(&self->operator, &op);
            changed = TRUE;
        }
    } else if (net->operator) {
        net->operator = NULL;
        changed = TRUE;
    }

    if (changed) {
        if (net->operator) {
            DBG_(self, "lalpha=%s, salpha=%s, numeric=%s, %s, mcc=%s, mnc=%s,"
                 " %s", lalpha, salpha, numeric, op.name, op.mcc, op.mnc,
                 ofono_access_technology_to_string(op.tech));
        } else {
            DBG_(self, "no operator");
        }
        binder_base_emit_property_change(&self->base,
            BINDER_NETWORK_PROPERTY_OPERATOR);
    }
}

static
void
binder_network_poll_operator_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->operator_poll_req == req);
    radio_request_unref(self->operator_poll_req);
    self->operator_poll_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_OPERATOR) {
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;

                gbinder_reader_copy(&reader, args);
                binder_network_poll_operator_ok(self, &reader);
            } else {
                DBG_(self, "Failed get operator, error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getOperator response %d", resp);
        }
    }
}

static
void
binder_network_set_registration_state(
    BinderRegistrationState* reg,
    RADIO_REG_STATE reg_state,
    RADIO_TECH rat,
    int lac,
    int ci)
{
    reg->status = OFONO_NETREG_STATUS_NONE;
    reg->access_tech = binder_access_tech_from_radio_tech(rat);
    reg->radio_tech = rat;
    reg->em_enabled = FALSE;
    reg->lac = lac;
    reg->ci = ci;

    switch (reg_state) {
    case RADIO_REG_STATE_REG_HOME:
        reg->em_enabled = TRUE;
        reg->status = OFONO_NETREG_STATUS_REGISTERED;
        break;

    case RADIO_REG_STATE_REG_ROAMING:
        reg->em_enabled = TRUE;
        reg->status = OFONO_NETREG_STATUS_ROAMING;
        break;

    case RADIO_REG_STATE_NOT_REG_MT_NOT_SEARCHING_EM:
        reg->em_enabled = TRUE;
        /* fallthrough */
    case RADIO_REG_STATE_NOT_REG_NOT_SEARCHING:
        reg->status = OFONO_NETREG_STATUS_NOT_REGISTERED;
        break;

    case RADIO_REG_STATE_NOT_REG_MT_SEARCHING_EM:
        reg->em_enabled = TRUE;
        /* fallthrough */
    case RADIO_REG_STATE_NOT_REG_MT_SEARCHING:
        reg->status = OFONO_NETREG_STATUS_SEARCHING;
        break;

    case RADIO_REG_STATE_REG_DENIED_EM:
        reg->em_enabled = TRUE;
        /* fallthrough */
    case RADIO_REG_STATE_REG_DENIED:
        reg->status = OFONO_NETREG_STATUS_DENIED;
        break;

    case RADIO_REG_STATE_UNKNOWN_EM:
        reg->em_enabled = TRUE;
        /* fallthrough */
    case RADIO_REG_STATE_UNKNOWN:
        reg->status = OFONO_NETREG_STATUS_UNKNOWN;
        break;
    }
}

static
void
binder_network_location_1_0(
    const RadioCellIdentity* cell,
    BinderNetworkLocation* l)
{
    switch (cell->cellInfoType) {
    case RADIO_CELL_INFO_GSM:
        if (cell->gsm.count > 0 && cell->gsm.data.ptr) {
            const RadioCellIdentityGsm* gsm = cell->gsm.data.ptr;

            l->lac = gsm->lac;
            l->ci = gsm->cid;
            return;
        }
        break;
    case RADIO_CELL_INFO_WCDMA:
        if (cell->wcdma.count > 0 && cell->wcdma.data.ptr) {
            const RadioCellIdentityWcdma* wcdma = cell->wcdma.data.ptr;

            l->lac = wcdma->lac;
            l->ci = wcdma->cid;
            return;
        }
        break;
    case RADIO_CELL_INFO_TD_SCDMA:
        if (cell->tdscdma.count > 0 && cell->tdscdma.data.ptr) {
            const RadioCellIdentityTdscdma* tds = cell->tdscdma.data.ptr;

            l->lac = tds->lac;
            l->ci = tds->cid;
            return;
        }
        break;
    case RADIO_CELL_INFO_LTE:
        if (cell->lte.count > 0 && cell->lte.data.ptr) {
            const RadioCellIdentityLte* lte = cell->lte.data.ptr;

            l->lac = -1;
            l->ci = lte->ci;
            return;
        }
        break;
    default:
        break;
    }

    /* Unknown location */
    l->lac = l->ci = -1;
}

static
void
binder_network_location_1_2(
    const RadioCellIdentity_1_2* cell,
    BinderNetworkLocation* l)
{
    switch (cell->cellInfoType) {
    case RADIO_CELL_INFO_GSM:
        if (cell->gsm.count > 0 && cell->gsm.data.ptr) {
            const RadioCellIdentityGsm_1_2* gsm = cell->gsm.data.ptr;

            l->lac = gsm->base.lac;
            l->ci = gsm->base.cid;
            return;
        }
        break;
    case RADIO_CELL_INFO_WCDMA:
        if (cell->wcdma.count > 0 && cell->wcdma.data.ptr) {
            const RadioCellIdentityWcdma_1_2* wcdma = cell->wcdma.data.ptr;

            l->lac = wcdma->base.lac;
            l->ci = wcdma->base.cid;
            return;
        }
        break;
    case RADIO_CELL_INFO_TD_SCDMA:
        if (cell->tdscdma.count > 0 && cell->tdscdma.data.ptr) {
            const RadioCellIdentityTdscdma_1_2* tds = cell->tdscdma.data.ptr;

            l->lac = tds->base.lac;
            l->ci = tds->base.cid;
            return;
        }
        break;
    case RADIO_CELL_INFO_LTE:
        if (cell->lte.count > 0 && cell->lte.data.ptr) {
            const RadioCellIdentityLte_1_2* lte = cell->lte.data.ptr;

            l->lac = -1;
            l->ci = lte->base.ci;
            return;
        }
        break;
    default:
        break;
    }

    /* Unknown location */
    l->lac = l->ci = -1;
}

static
void
binder_network_location_1_5(
    const RadioCellIdentity_1_5* cell,
    BinderNetworkLocation* l)
{
    switch (cell->cellIdentityType) {
    case RADIO_CELL_IDENTITY_1_5_GSM: {
        const RadioCellIdentityGsm_1_5* gsm = &cell->identity.gsm;

        l->lac = gsm->base.base.lac;
        l->ci = gsm->base.base.cid;
        return;
    }
    case RADIO_CELL_IDENTITY_1_5_WCDMA: {
        const RadioCellIdentityWcdma_1_5* wcdma = &cell->identity.wcdma;

        l->lac = wcdma->base.base.lac;
        l->ci = wcdma->base.base.cid;
        return;
    }
    case RADIO_CELL_IDENTITY_1_5_TD_SCDMA: {
        const RadioCellIdentityTdscdma_1_5* tds = &cell->identity.tdscdma;

        l->lac = tds->base.base.lac;
        l->ci = tds->base.base.cid;
        return;
    }
    case RADIO_CELL_IDENTITY_1_5_LTE: {
        const RadioCellIdentityLte_1_5* lte = &cell->identity.lte;

        l->lac = -1;
        l->ci = lte->base.base.ci;
        return;
    }
    case RADIO_CELL_IDENTITY_1_5_NR: {
        const RadioCellIdentityNr_1_5* nr = &cell->identity.nr;

        l->lac = -1;
        l->ci = nr->base.nci;
        return;
    }
    default:
        break;
    }

    /* Unknown location */
    l->lac = l->ci = -1;
}

static
void
binder_network_poll_voice_state_1_0(
    BinderRegistrationState* state,
    const RadioVoiceRegStateResult* result)
{
    BinderNetworkLocation l;

    binder_network_location_1_0(&result->cellIdentity, &l);
    binder_network_set_registration_state(state, result->regState,
        result->rat, l.lac, l.ci);
}

static
void
binder_network_poll_voice_state_1_2(
    BinderRegistrationState* state,
    const RadioVoiceRegStateResult_1_2* result)
{
    BinderNetworkLocation l;

    binder_network_location_1_2(&result->cellIdentity, &l);
    binder_network_set_registration_state(state, result->regState,
        result->rat, l.lac, l.ci);
}

static
void
binder_network_poll_voice_state_1_5(
    BinderRegistrationState* state,
    const RadioRegStateResult_1_5* result)
{
    BinderNetworkLocation l;

    binder_network_location_1_5(&result->cellIdentity, &l);
    binder_network_set_registration_state(state, result->regState,
        result->rat, l.lac, l.ci);
}

static
void
binder_network_poll_voice_state_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->voice_poll_req == req);
    radio_request_unref(self->voice_poll_req);
    self->voice_poll_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            GBinderReader reader;
            BinderRegistrationState state;
            BinderRegistrationState* reg = NULL;
            int reason = -1;

            gbinder_reader_copy(&reader, args);
            if (resp == RADIO_RESP_GET_VOICE_REGISTRATION_STATE) {
                const RadioVoiceRegStateResult* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioVoiceRegStateResult);

                if (result) {
                    reg = &state;
                    reason = result->reasonForDenial;
                    binder_network_poll_voice_state_1_0(reg, result);
                }
            } else if (resp == RADIO_RESP_GET_VOICE_REGISTRATION_STATE_1_2) {
                const RadioVoiceRegStateResult_1_2* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioVoiceRegStateResult_1_2);

                if (result) {
                    reg = &state;
                    reason = result->reasonForDenial;
                    binder_network_poll_voice_state_1_2(reg, result);
                }
            } else if (resp == RADIO_RESP_GET_VOICE_REGISTRATION_STATE_1_5) {
                const RadioRegStateResult_1_5* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioRegStateResult_1_5);

                if (result) {
                    reg = &state;
                    reason = result->reasonDataDenied;
                    binder_network_poll_voice_state_1_5(reg, result);
                }
            } else {
                ofono_error("Unexpected getVoiceRegistrationState response %d",
                    resp);
            }

            if (reg) {
                BinderNetwork* net = &self->pub;

                DBG_(self, "%s,%s,%d,%d,%d,%d",
                     ofono_netreg_status_to_string(reg->status),
                     ofono_access_technology_to_string(reg->access_tech),
                     reg->radio_tech, reg->lac, reg->ci, reason);
                if (memcmp(&state, &net->voice, sizeof(state))) {
                    DBG_(self, "voice registration changed");
                    net->voice = state;
                    binder_base_emit_property_change(&self->base,
                        BINDER_NETWORK_PROPERTY_VOICE_STATE);
                }
            }
        } else {
            DBG_(self, "Failed get voice reg state, error %d", error);
        }
    }
}

static
void
binder_network_poll_data_state_1_0(
    BinderRegistrationState* state,
    const RadioDataRegStateResult* result)
{
    BinderNetworkLocation l;

    binder_network_location_1_0(&result->cellIdentity, &l);
    binder_network_set_registration_state(state, result->regState,
        result->rat, l.lac, l.ci);
}

static
void
binder_network_poll_data_state_1_2(
    BinderRegistrationState* state,
    const RadioDataRegStateResult_1_2* result)
{
    BinderNetworkLocation l;

    binder_network_location_1_2(&result->cellIdentity, &l);
    binder_network_set_registration_state(state, result->regState,
        result->rat, l.lac, l.ci);
}

static
void
binder_network_poll_data_state_1_4(
    BinderRegistrationState* state,
    BinderNetworkObject* self,
    const RadioDataRegStateResult_1_4* result)
{
    BinderNetworkLocation l;
    RADIO_TECH rat = result->rat;

    binder_network_location_1_2(&result->cellIdentity, &l);

    if (result->rat == RADIO_TECH_LTE || result->rat == RADIO_TECH_LTE_CA) {
        const RadioDataRegNrIndicators *nrIndicators = &result->nrIndicators;

        if (self->nr_connected && nrIndicators->isEndcAvailable &&
            !nrIndicators->isDcNrRestricted &&
            nrIndicators->isNrAvailable) {
            rat = RADIO_TECH_NR;
        }
    }

    binder_network_set_registration_state(state, result->regState,
        rat, l.lac, l.ci);
}

static
void
binder_network_poll_data_state_1_5(
    BinderRegistrationState* state,
    BinderNetworkObject* self,
    const RadioRegStateResult_1_5* result)
{
    BinderNetworkLocation l;
    RADIO_TECH rat = result->rat;

    binder_network_location_1_5(&result->cellIdentity, &l);

    if (result->accessTechnologySpecificInfoType == RADIO_REG_ACCESS_TECHNOLOGY_SPECIFIC_INFO_EUTRAN) {
        RadioRegEutranRegistrationInfo *eutranInfo = (RadioRegEutranRegistrationInfo *)&result->accessTechnologySpecificInfo;
        RadioDataRegNrIndicators *nrIndicators = &eutranInfo->nrIndicators;

        if ((rat == RADIO_TECH_LTE || rat == RADIO_TECH_LTE_CA) &&
            self->nr_connected && nrIndicators->isEndcAvailable &&
            !nrIndicators->isDcNrRestricted &&
            nrIndicators->isNrAvailable) {
            DBG_(self, "Setting radio technology for NSA 5G");
            rat = RADIO_TECH_NR;
        }
    }
    binder_network_set_registration_state(state, result->regState,
        rat, l.lac, l.ci);
}

static
void
binder_network_poll_data_state_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->data_poll_req == req);
    radio_request_unref(self->data_poll_req);
    self->data_poll_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            GBinderReader reader;
            BinderRegistrationState state;
            BinderRegistrationState* reg = NULL;
            int reason = -1, max_data_calls = -1;

            gbinder_reader_copy(&reader, args);
            if (resp == RADIO_RESP_GET_DATA_REGISTRATION_STATE) {
                const RadioDataRegStateResult* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioDataRegStateResult);

                if (result) {
                    reg = &state;
                    reason = result->reasonDataDenied;
                    max_data_calls = result->maxDataCalls;
                    binder_network_poll_data_state_1_0(reg, result);
                }
            } else if (resp == RADIO_RESP_GET_DATA_REGISTRATION_STATE_1_2) {
                const RadioDataRegStateResult_1_2* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioDataRegStateResult_1_2);

                if (result) {
                    reg = &state;
                    reason = result->reasonDataDenied;
                    max_data_calls = result->maxDataCalls;
                    binder_network_poll_data_state_1_2(reg, result);
                }
            } else if (resp == RADIO_RESP_GET_DATA_REGISTRATION_STATE_1_4) {
                const RadioDataRegStateResult_1_4* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioDataRegStateResult_1_4);

                if (result) {
                    reg = &state;
                    reason = result->reasonDataDenied;
                    max_data_calls = result->maxDataCalls;
                    binder_network_poll_data_state_1_4(reg, self, result);
                }
            } else if (resp == RADIO_RESP_GET_DATA_REGISTRATION_STATE_1_5) {
                const RadioRegStateResult_1_5* result =
                    gbinder_reader_read_hidl_struct(&reader,
                        RadioRegStateResult_1_5);

                if (result) {
                    reg = &state;
                    reason = result->reasonDataDenied;
                    max_data_calls = MAX_DATA_CALLS;
                    binder_network_poll_data_state_1_5(reg, self, result);
                }
            } else {
                ofono_error("Unexpected getDataRegistrationState response %d",
                    resp);
            }

            if (reg) {
                BinderBase* base = &self->base;
                BinderNetwork* net = &self->pub;

                DBG_(self, "%s,%s,%d,%d,%d,%d,%d",
                     ofono_netreg_status_to_string(reg->status),
                     ofono_access_technology_to_string(reg->access_tech),
                     reg->radio_tech, reg->lac, reg->ci, reason,
                     max_data_calls);
                if (memcmp(&state, &net->data, sizeof(state))) {
                    DBG_(self, "data registration changed");
                    net->data = state;
                    binder_base_queue_property_change(base,
                        BINDER_NETWORK_PROPERTY_DATA_STATE);
                }
                if (net->max_data_calls != max_data_calls) {
                    net->max_data_calls = max_data_calls;
                    DBG_(self, "max data calls %d", max_data_calls);
                    binder_base_queue_property_change(base,
                        BINDER_NETWORK_PROPERTY_MAX_DATA_CALLS);
                }
                binder_base_emit_queued_signals(base);
            }
        } else {
            DBG_(self, "Failed get data reg state, error %d", error);
        }
    }
}

static
gboolean
binder_network_retry(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    switch (error) {
    case RADIO_ERROR_NONE:
    case RADIO_ERROR_RADIO_NOT_AVAILABLE:
        return FALSE;
    default:
        return TRUE;
    }
}

static
RadioRequest*
binder_network_poll_and_retry(
    BinderNetworkObject* self,
    RadioRequest* req,
    RADIO_REQ code,
    RadioRequestCompleteFunc complete)
{
    /* Don't wait for retry timeout to expire */
    if (!radio_request_retry(req)) {
        radio_request_drop(req);
        req = radio_request_new2(self->g, code, NULL, complete, NULL, self);
        radio_request_set_retry_func(req, binder_network_retry);
        radio_request_set_retry(req, BINDER_RETRY_MS * 1000, -1);
        radio_request_set_timeout(req, INTINITE_TIMEOUT);
        radio_request_submit(req);
    }
    return req;
}

static
void
binder_network_poll_registration_state(
    BinderNetworkObject* self)
{
    RadioClient* client = self->g->client;
    const RADIO_INTERFACE iface = radio_client_interface(client);

    self->voice_poll_req = binder_network_poll_and_retry(self,
        self->voice_poll_req, RADIO_REQ_GET_VOICE_REGISTRATION_STATE,
        binder_network_poll_voice_state_cb);

    if (iface >= RADIO_INTERFACE_1_5) {
        self->data_poll_req = binder_network_poll_and_retry(self,
            self->data_poll_req, RADIO_REQ_GET_DATA_REGISTRATION_STATE_1_5,
            binder_network_poll_data_state_cb);
    } else {
        self->data_poll_req = binder_network_poll_and_retry(self,
            self->data_poll_req, RADIO_REQ_GET_DATA_REGISTRATION_STATE,
            binder_network_poll_data_state_cb);
    }
}

static
void
binder_network_poll_state(
    BinderNetworkObject* self)
{
    DBG_(self, "");
    self->operator_poll_req = binder_network_poll_and_retry(self,
        self->operator_poll_req, RADIO_REQ_GET_OPERATOR,
        binder_network_poll_operator_cb);
    binder_network_poll_registration_state(self);
}

static
RADIO_PREF_NET_TYPE
binder_network_mode_to_pref(
    BinderNetworkObject* self,
    enum ofono_radio_access_mode mode)
{
    BinderSimSettings* settings = self->pub.settings;

    switch (ofono_radio_access_max_mode(mode)) {
    case OFONO_RADIO_ACCESS_MODE_ANY:
    case OFONO_RADIO_ACCESS_MODE_NR:
        if (settings->techs & OFONO_RADIO_ACCESS_MODE_NR) {
            return RADIO_PREF_NET_NR_LTE_GSM_WCDMA;
        }
        /* fallthrough */
    case OFONO_RADIO_ACCESS_MODE_LTE:
        if (settings->techs & OFONO_RADIO_ACCESS_MODE_LTE) {
            return self->lte_network_mode;
        }
        /* fallthrough */
    default:
    case OFONO_RADIO_ACCESS_MODE_UMTS:
        if (settings->techs & OFONO_RADIO_ACCESS_MODE_UMTS) {
            return self->umts_network_mode;
        }
        /* fallthrough */
    case OFONO_RADIO_ACCESS_MODE_GSM:
        break;
    }

    return RADIO_PREF_NET_GSM_ONLY;
}

static
enum ofono_radio_access_mode
binder_network_actual_pref_modes(
    BinderNetworkObject* self)
{
    BinderNetwork* net = &self->pub;
    BinderSimSettings* settings = net->settings;
    BinderRadioCaps* caps = self->caps;
    const enum ofono_radio_access_mode supported = caps ?
        binder_access_modes_from_raf(caps->raf) :
        OFONO_RADIO_ACCESS_MODE_ALL;

    /*
     * On most dual-SIM phones only one slot at a time is allowed
     * to use LTE. On some modems, even if the slot which has been
     * using LTE gets powered off, we still need to explicitly set
     * its preferred mode to GSM, to make LTE machinery available
     * to the other slot. This behavior is configurable.
     */
    const enum ofono_radio_access_mode really_allowed =
        (self->radio->state == RADIO_STATE_ON) ?  net->allowed_modes:
        OFONO_RADIO_ACCESS_MODE_GSM;

    return settings->techs & settings->pref & supported & really_allowed;
}

static
void
binder_network_data_profile_init(
    BinderNetworkDataProfile* profile,
    const struct ofono_gprs_primary_context* ctx,
    RADIO_DATA_PROFILE_ID id)
{
    memset(profile, 0, sizeof(*profile));
    profile->id = id;
    profile->type = RADIO_DATA_PROFILE_3GPP;
    profile->proto = ctx->proto;
    profile->enabled = TRUE;
    profile->auth_method =
        (ctx->username[0] || ctx->password[0]) ?
        ctx->auth_method : OFONO_GPRS_AUTH_METHOD_NONE;

    profile->apn = ctx->apn;
    if (profile->auth_method == OFONO_GPRS_AUTH_METHOD_NONE) {
        profile->username = "";
        profile->password = "";
    } else {
        profile->username = ctx->username;
        profile->password = ctx->password;
    }
}

static
BinderNetworkDataProfile*
binder_network_data_profile_new(
    const struct ofono_gprs_primary_context* ctx,
    RADIO_DATA_PROFILE_ID id)
{
    /* Allocate the whole thing as a single memory block */
    BinderNetworkDataProfile profile;
    BinderNetworkDataProfile* out;
    gsize apn_size;
    gsize username_size = 0;
    gsize password_size = 0;
    char* ptr;

    binder_network_data_profile_init(&profile, ctx, id);

    apn_size = profile.apn[0] ? (strlen(profile.apn) + 1) : 0;
    username_size = profile.username[0] ? (strlen(profile.username) + 1) : 0;
    password_size = profile.password[0] ? (strlen(profile.password) + 1) : 0;
    ptr = g_malloc(G_ALIGN8(sizeof(profile)) + G_ALIGN8(apn_size) +
        G_ALIGN8(username_size) + G_ALIGN8(password_size));

    /* Copy the strucure */
    *(out = (BinderNetworkDataProfile*)ptr) = profile;
    ptr += G_ALIGN8(sizeof(profile));

    /* And the strings */
    if (profile.apn[0]) {
        out->apn = ptr;
        memcpy(ptr, profile.apn, apn_size);
        ptr += G_ALIGN8(apn_size);
    } else {
        out->apn = "";
    }

    if (profile.username[0]) {
        out->username = ptr;
        memcpy(ptr, profile.username, username_size);
        ptr += G_ALIGN8(username_size);
    } else {
        out->username = "";
    }

    if (profile.password[0]) {
        out->password = ptr;
        memcpy(ptr, profile.password, password_size);
        ptr += G_ALIGN8(password_size);
    } else {
        out->password = "";
    }

    return out;
}

static
RadioDataProfile*
binder_network_fill_radio_data_profile(
    GBinderWriter* writer,
    RadioDataProfile* dp,
    const BinderNetworkDataProfile* src,
    const BinderDataProfileConfig* dpc)
{
    const char* proto = binder_proto_str_from_ofono_proto(src->proto);

    binder_copy_hidl_string(writer, &dp->apn, src->apn);
    binder_copy_hidl_string(writer, &dp->protocol, proto);
    binder_copy_hidl_string(writer, &dp->roamingProtocol, proto);
    binder_copy_hidl_string(writer, &dp->user, src->username);
    binder_copy_hidl_string(writer, &dp->password, src->password);
    binder_copy_hidl_string(writer, &dp->mvnoMatchData, NULL);

    dp->authType = binder_radio_auth_from_ofono_method(src->auth_method);
    dp->supportedApnTypesBitmap = binder_radio_apn_types_for_profile(src->id,
        dpc);
    dp->enabled = TRUE;
    return dp;
}

static
RadioDataProfile*
binder_network_new_radio_data_profile(
    GBinderWriter* writer,
    const BinderNetworkDataProfile* src,
    const BinderDataProfileConfig* dpc)
{
    return binder_network_fill_radio_data_profile(writer,
        gbinder_writer_new0(writer, RadioDataProfile), src, dpc);
}

static
RadioDataProfile_1_4*
binder_network_fill_radio_data_profile_1_4(
    GBinderWriter* writer,
    RadioDataProfile_1_4* dp,
    const BinderNetworkDataProfile* src,
    const BinderDataProfileConfig* dpc)
{
    binder_copy_hidl_string(writer, &dp->apn, src->apn);
    binder_copy_hidl_string(writer, &dp->user, src->username);
    binder_copy_hidl_string(writer, &dp->password, src->password);

    dp->protocol = dp->roamingProtocol =
        binder_proto_from_ofono_proto(src->proto);
    dp->authType = binder_radio_auth_from_ofono_method(src->auth_method);
    dp->supportedApnTypesBitmap = binder_radio_apn_types_for_profile(src->id,
        dpc);
    dp->enabled = TRUE;
    dp->preferred = TRUE;
    return dp;
}

static
RadioDataProfile_1_5*
binder_network_fill_radio_data_profile_1_5(
    GBinderWriter* writer,
    RadioDataProfile_1_5* dp,
    const BinderNetworkDataProfile* src,
    const BinderDataProfileConfig* dpc)
{
    binder_copy_hidl_string(writer, &dp->apn, src->apn);
    binder_copy_hidl_string(writer, &dp->user, src->username);
    binder_copy_hidl_string(writer, &dp->password, src->password);

    dp->protocol = dp->roamingProtocol =
        binder_proto_from_ofono_proto(src->proto);
    dp->authType = binder_radio_auth_from_ofono_method(src->auth_method);
    dp->supportedApnTypesBitmap = binder_radio_apn_types_for_profile(src->id,
        dpc);
    dp->enabled = TRUE;
    dp->preferred = TRUE;
    return dp;
}

static
RadioDataProfile_1_4*
binder_network_new_radio_data_profile_1_4(
    GBinderWriter* writer,
    const BinderNetworkDataProfile* src,
    const BinderDataProfileConfig* dpc)
{
    return binder_network_fill_radio_data_profile_1_4(writer,
        gbinder_writer_new0(writer, RadioDataProfile_1_4), src, dpc);
}

static
RadioDataProfile_1_5*
binder_network_new_radio_data_profile_1_5(
    GBinderWriter* writer,
    const BinderNetworkDataProfile* src,
    const BinderDataProfileConfig* dpc)
{
    return binder_network_fill_radio_data_profile_1_5(writer,
        gbinder_writer_new0(writer, RadioDataProfile_1_5), src, dpc);
}

static
gboolean
binder_network_data_profile_equal(
    const BinderNetworkDataProfile* profile1,
    const BinderNetworkDataProfile* profile2)
{
    if (profile1 == profile2) {
        return TRUE;
    } else if (!profile1 || !profile2) {
        return FALSE;
    } else {
        return profile1->id == profile2->id &&
            profile1->type == profile2->type &&
            profile1->auth_method == profile2->auth_method &&
            profile1->proto == profile2->proto &&
            profile1->enabled == profile2->enabled &&
            !g_strcmp0(profile1->apn, profile2->apn) &&
            !g_strcmp0(profile1->username, profile2->username) &&
            !g_strcmp0(profile1->password, profile2->password);
    }
}

static
gboolean
binder_network_data_profiles_equal(
    GSList* list1,
    GSList* list2)
{
    if (g_slist_length(list1) != g_slist_length(list2)) {
        return FALSE;
    } else {
        GSList* l1 = list1;
        GSList* l2 = list2;

        while (l1 && l2) {
            const BinderNetworkDataProfile* p1 = l1->data;
            const BinderNetworkDataProfile* p2 = l2->data;

            if (!binder_network_data_profile_equal(p1, p2)) {
                return FALSE;
            }
            l1 = l1->next;
            l2 = l2->next;
        }

        return TRUE;
    }
}

static inline
void
binder_network_data_profiles_free(
    GSList* list)
{
    /* Profiles are allocated as single memory blocks */
    g_slist_free_full(list, g_free);
}

static
void
binder_network_set_data_profiles_done(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->set_data_profiles_req == req);
    radio_request_unref(self->set_data_profiles_req);
    self->set_data_profiles_req = NULL;

    if (error != RADIO_ERROR_NONE) {
        ofono_error("Error setting data profiles: %s",
            binder_radio_error_string(error));
    }

    binder_network_check_initial_attach_apn(self);
}

static
void
binder_network_set_data_profiles(
    BinderNetworkObject* self)
{
    RadioClient* client = self->g->client;
    const BinderDataProfileConfig* dpc = &self->data_profile_config;
    const RADIO_INTERFACE iface = radio_client_interface(client);
    const guint n = g_slist_length(self->data_profiles);
    RadioRequest* req;
    GBinderWriter writer;
    GSList* l;
    guint i;

    if (iface >= RADIO_INTERFACE_1_5) {
        RadioDataProfile_1_5* dp;

        /* setDataProfile_1_5(int32 serial, vec<DataProfileInfo>); */
        req = radio_request_new(client, RADIO_REQ_SET_DATA_PROFILE_1_5,
            &writer, binder_network_set_data_profiles_done, NULL, self);

        dp = gbinder_writer_malloc0(&writer, sizeof(*dp) * n);
        for (l = self->data_profiles, i = 0; i < n; l = l->next, i++) {
            binder_network_fill_radio_data_profile_1_5(&writer, dp + i,
                (BinderNetworkDataProfile*) l->data, dpc);
        }

        gbinder_writer_append_struct_vec(&writer, dp, n,
            &binder_data_profile_1_5_type);
    } else if (iface >= RADIO_INTERFACE_1_4) {
        RadioDataProfile_1_4* dp;

        /* setDataProfile_1_4(int32 serial, vec<DataProfileInfo>); */
        req = radio_request_new(client, RADIO_REQ_SET_DATA_PROFILE_1_4,
            &writer, binder_network_set_data_profiles_done, NULL, self);

        dp = gbinder_writer_malloc0(&writer, sizeof(*dp) * n);
        for (l = self->data_profiles, i = 0; i < n; l = l->next, i++) {
            binder_network_fill_radio_data_profile_1_4(&writer, dp + i,
                (BinderNetworkDataProfile*) l->data, dpc);
        }

        gbinder_writer_append_struct_vec(&writer, dp, n,
            &binder_data_profile_1_4_type);
    } else {
        RadioDataProfile* dp;

        /* setDataProfile(int32 serial, vec<DataProfileInfo>, bool roaming); */
        req = radio_request_new(client, RADIO_REQ_SET_DATA_PROFILE,
            &writer, binder_network_set_data_profiles_done, NULL, self);

        dp = gbinder_writer_malloc0(&writer, sizeof(*dp) * n);
        for (l = self->data_profiles, i = 0; i < n; l = l->next, i++) {
            binder_network_fill_radio_data_profile(&writer, dp + i,
                (BinderNetworkDataProfile*) l->data, dpc);
        }

        gbinder_writer_append_struct_vec(&writer, dp, n,
            &binder_data_profile_type);
        gbinder_writer_append_bool(&writer, FALSE); /* isRoaming */
    }

    radio_request_drop(self->set_data_profiles_req);
    self->set_data_profiles_req = req; /* Keep the ref */
    radio_request_submit(req);
}

static
void
binder_network_check_data_profiles(
    BinderNetworkObject* self)
{
    const BinderDataProfileConfig* dpc = &self->data_profile_config;
    struct ofono_gprs* gprs = self->watch->gprs;

    if (gprs) {
        GSList* l = NULL;

        const struct ofono_gprs_primary_context* internet =
            ofono_gprs_context_settings_by_type(gprs,
                OFONO_GPRS_CONTEXT_TYPE_INTERNET);
        const struct ofono_gprs_primary_context* mms =
            ofono_gprs_context_settings_by_type(gprs,
                OFONO_GPRS_CONTEXT_TYPE_MMS);
        const struct ofono_gprs_primary_context* ims =
            ofono_gprs_context_settings_by_type(gprs,
                OFONO_GPRS_CONTEXT_TYPE_IMS);

        if (internet) {
            DBG_(self, "internet apn \"%s\"", internet->apn);
            l = g_slist_append(l, binder_network_data_profile_new(internet,
                dpc->default_profile_id));
        }

        if (mms) {
            DBG_(self, "mms apn \"%s\"", mms->apn);
            l = g_slist_append(l, binder_network_data_profile_new(mms,
                dpc->mms_profile_id));
        }

        if (ims) {
            DBG_(self, "ims apn \"%s\"", ims->apn);
            l = g_slist_append(l, binder_network_data_profile_new(ims,
                RADIO_DATA_PROFILE_IMS));
        }

        if (binder_network_data_profiles_equal(self->data_profiles, l)) {
            binder_network_data_profiles_free(l);
        } else {
            binder_network_data_profiles_free(self->data_profiles);
            self->data_profiles = l;
            binder_network_set_data_profiles(self);
        }
    } else {
        binder_network_data_profiles_free(self->data_profiles);
        self->data_profiles = NULL;
    }
}

static
gboolean
binder_network_need_initial_attach_apn(
    BinderNetworkObject* self)
{
    return (binder_network_actual_pref_modes(self) &
            (OFONO_RADIO_ACCESS_MODE_LTE | OFONO_RADIO_ACCESS_MODE_NR))
            ? TRUE : FALSE;
}

static
gboolean
binder_network_can_set_initial_attach_apn(
    BinderNetworkObject* self)
{
    struct ofono_watch* watch = self->watch;
    BinderRadio* radio = self->radio;

    return binder_network_need_initial_attach_apn(self) &&
        watch->gprs && radio->state == RADIO_STATE_ON &&
        !self->set_data_profiles_req;
}

static
void
binder_network_set_initial_attach_apn(
    BinderNetworkObject* self,
    const struct ofono_gprs_primary_context* ctx)
{
    const RADIO_INTERFACE iface = radio_client_interface(self->g->client);
    const BinderDataProfileConfig* dpc = &self->data_profile_config;
    BinderNetworkDataProfile profile;
    RadioRequest* req;
    GBinderWriter writer;

    binder_network_data_profile_init(&profile, ctx, RADIO_DATA_PROFILE_DEFAULT);

    if (iface >= RADIO_INTERFACE_1_5) {
        /* setInitialAttachApn_1_4(int32 serial, DataProfileInfo profile); */
        req = radio_request_new2(self->g, RADIO_REQ_SET_INITIAL_ATTACH_APN_1_5,
            &writer, NULL, NULL, NULL);

        gbinder_writer_append_struct(&writer,
            binder_network_new_radio_data_profile_1_5(&writer, &profile, dpc),
            &binder_data_profile_1_5_type, NULL);
    } else if (iface >= RADIO_INTERFACE_1_4) {
        /* setInitialAttachApn_1_4(int32 serial, DataProfileInfo profile); */
        req = radio_request_new2(self->g, RADIO_REQ_SET_INITIAL_ATTACH_APN_1_4,
            &writer, NULL, NULL, NULL);

        gbinder_writer_append_struct(&writer,
            binder_network_new_radio_data_profile_1_4(&writer, &profile, dpc),
            &binder_data_profile_1_4_type, NULL);
    } else {
        /*
         * setInitialAttachApn(int32 serial, DataProfileInfo profile,
         *     bool modemCognitive, bool isRoaming);
         */
        req = radio_request_new2(self->g, RADIO_REQ_SET_INITIAL_ATTACH_APN,
            &writer, NULL, NULL, NULL);

        gbinder_writer_append_struct(&writer,
            binder_network_new_radio_data_profile(&writer, &profile, dpc),
            &binder_data_profile_type, NULL);
        gbinder_writer_append_bool(&writer, FALSE);  /* modemCognitive */
        gbinder_writer_append_bool(&writer, FALSE); /* isRoaming */
    }

    DBG_(self, "\"%s\"", ctx->apn);
    radio_request_set_retry(req, BINDER_RETRY_MS, -1);
    radio_request_set_timeout(req, INTINITE_TIMEOUT);
    radio_request_drop(self->set_ia_apn_req);
    self->set_ia_apn_req = req; /* Keep the ref */
    radio_request_submit(req);
}

static
void
binder_network_try_set_initial_attach_apn(
    BinderNetworkObject* self)
{
    if (self->set_initial_attach_apn &&
        binder_network_can_set_initial_attach_apn(self)) {
        struct ofono_gprs* gprs = self->watch->gprs;
        const struct ofono_gprs_primary_context* ctx =
            ofono_gprs_context_settings_by_type(gprs,
                OFONO_GPRS_CONTEXT_TYPE_INTERNET);

        if (ctx) {
            self->set_initial_attach_apn = FALSE;
            binder_network_set_initial_attach_apn(self, ctx);
        }
    }
}

static
void
binder_network_check_initial_attach_apn(
    BinderNetworkObject* self)
{
    const gboolean need = binder_network_need_initial_attach_apn(self);

    if (self->need_initial_attach_apn != need) {
        DBG_(self, "%sneed initial attach apn", need ? "" : "don't ");
        self->need_initial_attach_apn = need;
        if (need) {
            /* We didn't need initial attach APN and now we do */
            self->set_initial_attach_apn = TRUE;
        }
    }
    binder_network_try_set_initial_attach_apn(self);
}

static
void
binder_network_reset_initial_attach_apn(
    BinderNetworkObject* self)
{
    if (binder_network_need_initial_attach_apn(self) &&
        !self->set_initial_attach_apn) {
        /* Will set initial attach APN when we have a chance */
        DBG_(self, "need to set initial attach apn");
        self->set_initial_attach_apn = TRUE;
        binder_network_try_set_initial_attach_apn(self);
    }
}

static
gboolean
binder_network_can_set_pref_mode(
    BinderNetworkObject* self)
{
    /*
     * With some modems an attempt to set rat significantly slows
     * down SIM I/O, let's avoid that.
     */
    return self->radio->online && binder_sim_card_ready(self->simcard) &&
        !self->simcard->sim_io_active && !self->timer[TIMER_SET_RAT_HOLDOFF];
}

static
gboolean
binder_network_set_pref_holdoff_cb(
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->timer[TIMER_SET_RAT_HOLDOFF]);
    self->timer[TIMER_SET_RAT_HOLDOFF] = 0;

    binder_network_check_pref_mode(self, FALSE);
    return G_SOURCE_REMOVE;
}

static
void
binder_network_set_pref_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->set_rat_req == req);
    radio_request_unref(self->set_rat_req);
    self->set_rat_req = NULL;

    if (error != RADIO_ERROR_NONE) {
        ofono_error("Error %d setting pref mode", error);
    }

    binder_network_query_pref_mode(self);
}

static
void
binder_network_set_pref(
    BinderNetworkObject* self,
    RADIO_PREF_NET_TYPE rat)
{
    BinderSimCard* card = self->simcard;

    if (!self->set_rat_req &&
        self->radio->online &&
        binder_sim_card_ready(card) &&
        /*
         * With some modems an attempt to set rat significantly
         * slows down SIM I/O, let's avoid that.
         */
        !card->sim_io_active &&
        !self->timer[TIMER_SET_RAT_HOLDOFF]) {
        RadioClient* client = self->g->client;
        const RADIO_INTERFACE iface = radio_client_interface(client);
        GBinderWriter writer;

        if (iface >= RADIO_INTERFACE_1_4) {
            BinderRadioCaps* caps = self->caps;
            RADIO_ACCESS_FAMILY raf = binder_raf_from_pref(rat);

            /*
             * setPreferredNetworkTypeBitmap(int32 serial,
             *     bitfield<RadioAccessFamily> networkTypeBitmap);
             */
            self->set_rat_req = radio_request_new(client,
                RADIO_REQ_SET_PREFERRED_NETWORK_TYPE_BITMAP, &writer,
                binder_network_set_pref_cb, NULL, self);

            if (caps) raf &= caps->raf;
            gbinder_writer_append_int32(&writer, raf);
        } else {
            /* setPreferredNetworkType(int32 serial, PreferredNetworkType); */
            self->set_rat_req = radio_request_new(client,
                RADIO_REQ_SET_PREFERRED_NETWORK_TYPE, &writer,
                binder_network_set_pref_cb, NULL, self);
            gbinder_writer_append_int32(&writer, rat);
        }

        DBG_(self, "setting rat mode %d", rat);
        radio_request_set_timeout(self->set_rat_req,
            self->network_mode_timeout_ms);
        if (radio_request_submit(self->set_rat_req)) {
            /* We have submitted the request, clear the assertion flag */
            self->assert_rat = FALSE;
        }

        /* And don't do it too often */
        self->timer[TIMER_SET_RAT_HOLDOFF] =
            g_timeout_add_seconds(SET_PREF_MODE_HOLDOFF_SEC,
                binder_network_set_pref_holdoff_cb, self);
    } else {
        DBG_(self, "need to set rat mode %d", rat);
    }
}

static
void
binder_network_update_pref_mode(
    BinderNetworkObject* self,
    RADIO_PREF_NET_TYPE rat)
{
    if (self->rat != rat || self->assert_rat) {
        binder_network_set_pref(self, rat);
    }
}

static
void
binder_network_check_pref_mode(
    BinderNetworkObject* self,
    gboolean immediate)
{
    BinderRadio* radio = self->radio;

    /*
     * On most dual-SIM phones only one slot at a time is allowed
     * to use LTE. On some phones, even if the slot which has been
     * using LTE gets powered off, we still need to explicitly set
     * its preferred mode to GSM, to make LTE machinery available
     * to the other slot. This behavior is configurable.
     */
    if (radio->state == RADIO_STATE_ON || self->force_gsm_when_radio_off) {
        const enum ofono_radio_access_mode expected =
            binder_network_actual_pref_modes(self);
        const enum ofono_radio_access_mode actual =
            binder_access_modes_from_pref(self->rat);

        if (self->timer[TIMER_FORCE_CHECK_PREF_MODE]) {
            binder_network_stop_timer(self, TIMER_FORCE_CHECK_PREF_MODE);
            /*
             * TIMER_FORCE_CHECK_PREF_MODE is scheduled by
             * binder_network_settings_pref_changed_cb and is meant
             * to force radio tech check right now.
             */
            immediate = TRUE;
        }

        if (self->raf && actual != expected) {
            DBG_(self, "rat %d raf 0x%08x (%s), expected %s",
                self->rat, self->raf,
                ofono_radio_access_mode_to_string(actual),
                ofono_radio_access_mode_to_string(expected));
        }

        if (immediate) {
            binder_network_stop_timer(self, TIMER_SET_RAT_HOLDOFF);
        }

        if (actual != expected || self->assert_rat) {
            const RADIO_PREF_NET_TYPE pref =
                binder_network_mode_to_pref(self, expected);

            if (!self->timer[TIMER_SET_RAT_HOLDOFF]) {
                binder_network_update_pref_mode(self, pref);
            } else {
                /* OK, later */
                DBG_(self, "need to set rat mode %d", pref);
            }
        }
    }
}

static
void
binder_network_assert_pref_mode(
    BinderNetworkObject* self)
{
    self->assert_rat = TRUE;
    binder_network_check_pref_mode(self, FALSE);
}

static
gboolean
binder_network_query_rat_done(
    BinderNetworkObject* self,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args)
{
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_PREFERRED_NETWORK_TYPE) {
            /*
             * getPreferredNetworkTypeResponse(RadioResponseInfo,
             *     PreferredNetworkType nwType);
             */
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                guint32 rat;

                gbinder_reader_copy(&reader, args);
                if (gbinder_reader_read_uint32(&reader, &rat)) {
                    BinderNetwork* net = &self->pub;
                    enum ofono_radio_access_mode modes;

                    self->rat = rat;
                    self->raf = binder_raf_from_pref(rat);
                    modes = binder_access_modes_from_pref(self->rat);
                    DBG_(self, "rat %d => raf 0x%08x (%s)", rat, self->raf,
                        ofono_radio_access_mode_to_string(modes));

                    if (net->pref_modes != modes) {
                        net->pref_modes = modes;
                        binder_base_emit_property_change(&self->base,
                            BINDER_NETWORK_PROPERTY_PREF_MODES);
                    }
                    return TRUE;
                }
            } else {
                ofono_warn("getPreferredNetworkType error %d", error);
            }
        } else {
            ofono_error("Unexpected getPreferredNetworkType response %d", resp);
        }
    }
    return FALSE;
}

static
gboolean
binder_network_query_raf_done(
    BinderNetworkObject* self,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args)
{
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_PREFERRED_NETWORK_TYPE_BITMAP) {
            /*
             * getPreferredNetworkTypeBitmapResponse(RadioResponseInfo,
             *     bitfield<RadioAccessFamily> networkTypeBitmap);
             */
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                guint32 raf;

                gbinder_reader_copy(&reader, args);
                if (gbinder_reader_read_uint32(&reader, &raf)) {
                    BinderNetwork* net = &self->pub;
                    enum ofono_radio_access_mode modes;

                    self->raf = raf;
                    self->rat = binder_pref_from_raf(raf);
                    modes = binder_access_modes_from_raf(raf);
                    DBG_(self, "raf 0x%08x => rat %d (%s)", raf, self->rat,
                        ofono_radio_access_mode_to_string(modes));

                    if (net->pref_modes != modes) {
                        net->pref_modes = modes;
                        binder_base_emit_property_change(&self->base,
                            BINDER_NETWORK_PROPERTY_PREF_MODES);
                    }
                    return TRUE;
                }
            } else {
                ofono_warn("getPreferredNetworkTypeBitmap error %d", error);
            }
        } else {
            ofono_error("Unexpected getPreferredNetworkTypeBitmap response %d",
                resp);
        }
    }
    return FALSE;
}

static
void
binder_network_initial_pref_query_done(
    BinderNetworkObject* self,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gboolean (*handle)(
        BinderNetworkObject* self,
        RADIO_TX_STATUS status,
        RADIO_RESP resp,
        RADIO_ERROR error,
        const GBinderReader* args))
{
    binder_network_object_ref(self);
    if (handle(self, status, resp, error, args)) {
        /*
         * At startup, the device may have an inconsistency between
         * voice and data network modes, so it needs to be asserted.
         */
        binder_network_assert_pref_mode(self);
    }
    binder_network_object_unref(self);
}

static
void
binder_network_initial_rat_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_network_initial_pref_query_done(THIS(user_data),
        status, resp, error, args, binder_network_query_rat_done);
}

static
void
binder_network_initial_raf_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_network_initial_pref_query_done(THIS(user_data),
        status, resp, error, args, binder_network_query_raf_done);
}

static
void
binder_network_initial_rat_query(
    BinderNetworkObject* self)
{
    RadioClient* client = self->g->client;
    const RADIO_INTERFACE iface = radio_client_interface(client);
    RadioRequest* req;

    if (iface >= RADIO_INTERFACE_1_4) {
        /* getPreferredNetworkTypeBitmap(int32 serial) */
        req = radio_request_new2(self->g,
            RADIO_REQ_GET_PREFERRED_NETWORK_TYPE_BITMAP, NULL,
            binder_network_initial_raf_query_cb, NULL, self);
    } else {
        /* getPreferredNetworkType(int32 serial) */
        req = radio_request_new2(self->g,
            RADIO_REQ_GET_PREFERRED_NETWORK_TYPE, NULL,
            binder_network_initial_rat_query_cb, NULL, self);
    }

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_network_pref_query_done(
    BinderNetworkObject* self,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gboolean (*handle)(
        BinderNetworkObject* self,
        RADIO_TX_STATUS status,
        RADIO_RESP resp,
        RADIO_ERROR error,
        const GBinderReader* args))
{
    GASSERT(self->query_rat_req);
    radio_request_unref(self->query_rat_req);
    self->query_rat_req = NULL;

    binder_network_object_ref(self);
    if (handle(self, status, resp, error, args) &&
        binder_network_can_set_pref_mode(self)) {
        binder_network_check_pref_mode(self, FALSE);
    }
    binder_network_object_unref(self);
}

static
void
binder_network_rat_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_network_pref_query_done(THIS(user_data),
        status, resp, error, args, binder_network_query_rat_done);
}

static
void
binder_network_raf_query_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_network_pref_query_done(THIS(user_data),
        status, resp, error, args, binder_network_query_raf_done);
}

static
void
binder_network_query_pref_mode(
    BinderNetworkObject* self)
{
    RadioClient* client = self->g->client;
    const RADIO_INTERFACE iface = radio_client_interface(client);
    RadioRequest* req;

    if (iface >= RADIO_INTERFACE_1_4) {
        /* getPreferredNetworkTypeBitmap(int32 serial); */
        req = radio_request_new(client,
            RADIO_REQ_GET_PREFERRED_NETWORK_TYPE_BITMAP, NULL,
            binder_network_raf_query_cb, NULL, self);
    } else {
        /* getPreferredNetworkType(int32 serial); */
        req = radio_request_new(client,
            RADIO_REQ_GET_PREFERRED_NETWORK_TYPE, NULL,
            binder_network_rat_query_cb, NULL, self);
    }

    radio_request_set_retry_func(req, binder_network_retry);
    radio_request_set_retry(req, BINDER_RETRY_MS, -1);
    radio_request_set_timeout(req, INTINITE_TIMEOUT);
    radio_request_drop(self->query_rat_req);
    self->query_rat_req = req; /* Keep the ref */
    if (!radio_request_submit(req)) {
        radio_request_drop(self->query_rat_req);
        self->query_rat_req = NULL;
    }
}

enum ofono_radio_access_mode
binder_network_max_supported_mode(
    BinderNetwork* net)
{
    BinderNetworkObject* self = binder_network_cast(net);

    if (G_LIKELY(self)) {
        BinderSimSettings* settings = net->settings;
        BinderRadioCaps* caps = self->caps;

        if (caps) {
            return ofono_radio_access_max_mode(settings->techs &
                binder_access_modes_from_raf(caps->raf));
        } else {
            return ofono_radio_access_max_mode(settings->techs);
        }
    }
    return OFONO_RADIO_ACCESS_MODE_NONE;
}

static
void
binder_network_caps_raf_handler(
    BinderRadioCaps* caps,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    DBG_(self, "raf 0x%08x (%s)", caps->raf, ofono_radio_access_mode_to_string
        (binder_access_modes_from_raf(caps->raf)));
    binder_network_check_pref_mode(self, TRUE);
}

static
void
binder_network_radio_capability_tx_done_cb(
    BinderRadioCapsManager* mgr,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    DBG_(self, "");
    binder_network_assert_pref_mode(self);
}

static
void
binder_network_release_radio_caps(
    BinderNetworkObject* self)
{
    BinderRadioCaps* caps = self->caps;

    if (caps) {
        binder_radio_caps_manager_remove_all_handlers(caps->mgr,
            self->caps_mgr_event_id);
        binder_radio_caps_remove_handler(caps, self->caps_raf_event_id);
        binder_radio_caps_unref(caps);

        self->caps_raf_event_id = 0;
        self->caps = NULL;
    }
}

static
void
binder_network_attach_radio_caps(
    BinderNetworkObject* self,
    BinderRadioCaps* caps)
{
    self->caps = binder_radio_caps_ref(caps);
    self->caps_raf_event_id = binder_radio_caps_add_raf_handler(caps,
        binder_network_caps_raf_handler, self);
    self->caps_mgr_event_id[RADIO_CAPS_MGR_TX_DONE] =
        binder_radio_caps_manager_add_tx_done_handler(caps->mgr,
            binder_network_radio_capability_tx_done_cb, self);
    self->caps_mgr_event_id[RADIO_CAPS_MGR_TX_ABORTED] =
        binder_radio_caps_manager_add_tx_aborted_handler(caps->mgr,
            binder_network_radio_capability_tx_done_cb, self);
}

static
void
binder_network_state_changed_cb(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    DBG_(self, "");
    GASSERT(code == RADIO_IND_NETWORK_STATE_CHANGED);
    binder_network_poll_state(self);
}

static
void
binder_network_modem_reset_cb(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    DBG_(self, "%s", binder_read_hidl_string(args));
    GASSERT(code == RADIO_IND_MODEM_RESET);

    /* Drop all pending requests */
    radio_request_drop(self->operator_poll_req);
    radio_request_drop(self->voice_poll_req);
    radio_request_drop(self->data_poll_req);
    radio_request_drop(self->query_rat_req);
    radio_request_drop(self->set_rat_req);
    radio_request_drop(self->set_data_profiles_req);
    radio_request_drop(self->set_ia_apn_req);
    self->operator_poll_req = NULL;
    self->voice_poll_req = NULL;
    self->data_poll_req = NULL;
    self->query_rat_req = NULL;
    self->set_rat_req = NULL;
    self->set_data_profiles_req = NULL;
    self->set_ia_apn_req  = NULL;

    binder_network_initial_rat_query(self);
    binder_network_reset_initial_attach_apn(self);
}

static
void
binder_network_current_physical_channel_configs_cb(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);
    GBinderReader reader;
    gboolean nr_connected = FALSE;

    gbinder_reader_copy(&reader, args);

    if (code == RADIO_IND_CURRENT_PHYSICAL_CHANNEL_CONFIGS_1_4) {
        gsize count;
        guint i;
        const RadioPhysicalChannelConfig_1_4* configs = gbinder_reader_read_hidl_type_vec(&reader,
            RadioPhysicalChannelConfig_1_4, &count);

        for (i = 0; i < count; i++) {
            if (configs[i].rat == RADIO_TECH_NR &&
                configs[i].base.connectionStatus == RADIO_CELL_CONNECTION_SECONDARY_SERVING) {
                DBG_(self, "NSA 5G connected");
                nr_connected = TRUE;
            }
        }
    } else {
        ofono_warn("Unexpected current physical channel configs code %d", code);
    }
    self->nr_connected = nr_connected;
}

static
void
binder_network_radio_state_cb(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    binder_network_check_pref_mode(self, FALSE);
    if (radio->state == RADIO_STATE_ON) {
        binder_network_poll_state(self);
        binder_network_try_set_initial_attach_apn(self);
    }
}

static
void
binder_network_radio_online_cb(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    if (binder_network_can_set_pref_mode(self)) {
        binder_network_check_pref_mode(self, TRUE);
    }
}

static
gboolean
binder_network_check_pref_mode_cb(
    gpointer user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    GASSERT(self->timer[TIMER_FORCE_CHECK_PREF_MODE]);
    self->timer[TIMER_FORCE_CHECK_PREF_MODE] = 0;

    DBG_(self, "checking pref mode");
    binder_network_check_pref_mode(self, TRUE);
    binder_network_check_initial_attach_apn(self);
    return G_SOURCE_REMOVE;
}

static
void
binder_network_settings_pref_changed_cb(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);

    /*
     * Postpone binder_network_check_pref_mode because other pref mode
     * listeners (namely, binder_data) may want to tweak allowed_modes
     */
    if (!self->timer[TIMER_FORCE_CHECK_PREF_MODE]) {
        DBG_(self, "scheduling pref mode check");
        self->timer[TIMER_FORCE_CHECK_PREF_MODE] =
            g_idle_add(binder_network_check_pref_mode_cb, self);
    } else {
        DBG_(self, "pref mode check already scheduled");
    }
}

static
void
binder_network_sim_status_changed_cb(
    BinderSimCard* card,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);
    const BinderSimCardStatus* status = card->status;

    if (!status || status->card_state != RADIO_CARD_STATE_PRESENT) {
        binder_network_reset_initial_attach_apn(self);
    }
    if (binder_network_can_set_pref_mode(self)) {
        binder_network_check_pref_mode(self, FALSE);
    }
}

static
void
binder_network_watch_gprs_cb(
    struct ofono_watch* watch,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);
    const BinderDataProfileConfig* dpc = &self->data_profile_config;

    DBG_(self, "gprs %s", watch->gprs ? "appeared" : "is gone");
    if (dpc->use_data_profiles) {
        binder_network_check_data_profiles(self);
    }
    binder_network_check_initial_attach_apn(self);
}

static
void
binder_network_watch_gprs_settings_cb(
    struct ofono_watch* watch,
    enum ofono_gprs_context_type type,
    const struct ofono_gprs_primary_context* settings,
    void* user_data)
{
    BinderNetworkObject* self = THIS(user_data);
    const BinderDataProfileConfig* dpc = &self->data_profile_config;

    if (dpc->use_data_profiles) {
        binder_network_check_data_profiles(self);
    }

    if (type == OFONO_GPRS_CONTEXT_TYPE_INTERNET) {
        binder_network_check_initial_attach_apn(self);
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderNetwork*
binder_network_new(
    const char* path,
    RadioClient* client,
    const char* log_prefix,
    BinderRadio* radio,
    BinderSimCard* simcard,
    BinderSimSettings* settings,
    const BinderSlotConfig* config)
{
    const BinderDataProfileConfig* dpc = &config->data_profile_config;
    BinderNetworkObject* self = g_object_new(THIS_TYPE, NULL);
    BinderNetwork* net = &self->pub;

    net->settings = binder_sim_settings_ref(settings);
    self->g = radio_request_group_new(client); /* Keeps ref to client */
    self->radio = binder_radio_ref(radio);
    self->simcard = binder_sim_card_ref(simcard);
    self->watch = ofono_watch_new(path);
    self->log_prefix = binder_dup_prefix(log_prefix);
    DBG_(self, "");

    /* Copy relevant config values */
    self->lte_network_mode = config->lte_network_mode;
    self->umts_network_mode = config->umts_network_mode;
    self->network_mode_timeout_ms = config->network_mode_timeout_ms;
    self->force_gsm_when_radio_off = config->force_gsm_when_radio_off;
    self->data_profile_config = *dpc;

    /* Register listeners */
    self->ind_id[IND_NETWORK_STATE] =
        radio_client_add_indication_handler(client,
            RADIO_IND_NETWORK_STATE_CHANGED,
            binder_network_state_changed_cb, self);
    self->ind_id[IND_MODEM_RESET] =
        radio_client_add_indication_handler(client,
            RADIO_IND_MODEM_RESET,
            binder_network_modem_reset_cb, self);
    self->ind_id[IND_CURRENT_PHYSICAL_CHANNEL_CONFIGS_1_4] =
        radio_client_add_indication_handler(client,
            RADIO_IND_CURRENT_PHYSICAL_CHANNEL_CONFIGS_1_4,
            binder_network_current_physical_channel_configs_cb, self);

    self->radio_event_id[RADIO_EVENT_STATE_CHANGED] =
        binder_radio_add_property_handler(self->radio,
            BINDER_RADIO_PROPERTY_STATE,
            binder_network_radio_state_cb, self);
    self->radio_event_id[RADIO_EVENT_ONLINE_CHANGED] =
        binder_radio_add_property_handler(self->radio,
            BINDER_RADIO_PROPERTY_ONLINE,
            binder_network_radio_online_cb, self);

    self->simcard_event_id[SIM_EVENT_STATUS_CHANGED] =
        binder_sim_card_add_status_changed_handler(self->simcard,
            binder_network_sim_status_changed_cb, self);
    self->simcard_event_id[SIM_EVENT_IO_ACTIVE_CHANGED] =
        binder_sim_card_add_sim_io_active_changed_handler(self->simcard,
            binder_network_sim_status_changed_cb, self);
    self->settings_event_id =
        binder_sim_settings_add_property_handler(settings,
            BINDER_SIM_SETTINGS_PROPERTY_PREF,
            binder_network_settings_pref_changed_cb, self);

    self->watch_ids[WATCH_EVENT_GPRS] =
        ofono_watch_add_gprs_changed_handler(self->watch,
            binder_network_watch_gprs_cb, self);
    self->watch_ids[WATCH_EVENT_GPRS_SETTINGS] =
        ofono_watch_add_gprs_settings_changed_handler(self->watch,
            binder_network_watch_gprs_settings_cb, self);

    /* Query the initial state */
    binder_network_initial_rat_query(self);

    if (radio->state == RADIO_STATE_ON) {
        binder_network_poll_state(self);
    }

    self->set_initial_attach_apn = self->need_initial_attach_apn =
        binder_network_need_initial_attach_apn(self);

    if (dpc->use_data_profiles) {
        binder_network_check_data_profiles(self);
    }
    binder_network_try_set_initial_attach_apn(self);
    return net;
}

BinderNetwork*
binder_network_ref(
    BinderNetwork* net)
{
    BinderNetworkObject* self = binder_network_cast(net);

    if (G_LIKELY(self)) {
        binder_network_object_ref(self);
        return net;
    } else {
        return NULL;
    }
}

void
binder_network_unref(
    BinderNetwork* net)
{
    BinderNetworkObject* self = binder_network_cast(net);

    if (G_LIKELY(self)) {
        binder_network_object_unref(self);
    }
}

void
binder_network_query_registration_state(
    BinderNetwork* net)
{
    BinderNetworkObject* self = binder_network_cast(net);

    if (self) {
        DBG_(self, "");
        binder_network_poll_registration_state(self);
    }
}

void
binder_network_set_allowed_modes(
    BinderNetwork* net,
    enum ofono_radio_access_mode modes,
    gboolean force_check)
{
    BinderNetworkObject* self = binder_network_cast(net);

    if (G_LIKELY(self)) {
        const gboolean changed = (net->allowed_modes != modes);

        if (changed) {
            net->allowed_modes = modes;
            DBG_(self, "allowed modes 0x%02x (%s)", modes,
                ofono_radio_access_mode_to_string(modes));
            binder_base_emit_property_change(&self->base,
                BINDER_NETWORK_PROPERTY_ALLOWED_MODES);
        }

        if (changed || force_check) {
            binder_network_check_pref_mode(self, TRUE);
        }
    }
}

void
binder_network_set_radio_caps(
    BinderNetwork* net,
    BinderRadioCaps* caps)
{
    BinderNetworkObject* self = binder_network_cast(net);

    if (G_LIKELY(self) && self->caps != caps) {
        binder_network_release_radio_caps(self);
        if (caps) {
            binder_network_attach_radio_caps(self, caps);
        }
        binder_network_check_pref_mode(self, TRUE);
    }
}

gulong
binder_network_add_property_handler(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    BinderNetworkPropertyFunc callback,
    void* user_data)
{
    BinderNetworkObject* self = binder_network_cast(net);

    return G_LIKELY(self) ? binder_base_add_property_handler(&self->base,
        property, G_CALLBACK(callback), user_data) : 0;
}

void
binder_network_remove_handler(
    BinderNetwork* net,
    gulong id)
{
    if (G_LIKELY(id)) {
        BinderNetworkObject* self = binder_network_cast(net);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
binder_network_remove_handlers(
    BinderNetwork* net,
    gulong* ids,
    int count)
{
    gutil_disconnect_handlers(binder_network_cast(net), ids, count);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_network_object_init(
    BinderNetworkObject* self)
{
    BinderNetwork* net = &self->pub;

    self->rat = RADIO_PREF_NET_INVALID;
    binder_network_reset_state(&net->voice);
    binder_network_reset_state(&net->data);
}

static
void
binder_network_object_finalize(
    GObject* object)
{
    BinderNetworkObject* self = THIS(object);
    BinderNetwork* net = &self->pub;
    BINDER_NETWORK_TIMER tid;

    DBG_(self, "");
    for (tid=0; tid<TIMER_COUNT; tid++) {
        binder_network_stop_timer(self, tid);
    }

    radio_request_drop(self->operator_poll_req);
    radio_request_drop(self->voice_poll_req);
    radio_request_drop(self->data_poll_req);
    radio_request_drop(self->query_rat_req);
    radio_request_drop(self->set_rat_req);
    radio_request_drop(self->set_data_profiles_req);
    radio_request_drop(self->set_ia_apn_req);

    ofono_watch_remove_all_handlers(self->watch, self->watch_ids);
    ofono_watch_unref(self->watch);

    radio_client_remove_all_handlers(self->g->client, self->ind_id);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    binder_network_release_radio_caps(self);
    binder_radio_remove_all_handlers(self->radio, self->radio_event_id);
    binder_radio_unref(self->radio);
    binder_sim_card_remove_all_handlers(self->simcard, self->simcard_event_id);
    binder_sim_card_unref(self->simcard);
    binder_sim_settings_remove_handler(net->settings, self->settings_event_id);
    binder_sim_settings_unref(net->settings);

    g_slist_free_full(self->data_profiles, g_free);
    g_free(self->log_prefix);

    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_network_object_class_init(
    BinderNetworkObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_network_object_finalize;
    BINDER_BASE_CLASS(klass)->public_offset =
        G_STRUCT_OFFSET(BinderNetworkObject, pub);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
