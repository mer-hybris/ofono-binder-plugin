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

#include "binder_util.h"

#include <ofono/misc.h>
#include <ofono/netreg.h>
#include <ofono/log.h>

#include <radio_request.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_idlepool.h>
#include <gutil_misc.h>

static GUtilIdlePool* binder_util_pool = NULL;
static const char binder_empty_str[] = "";
static const char PROTO_IP_STR[] = "IP";
static const char PROTO_IPV6_STR[] = "IPV6";
static const char PROTO_IPV4V6_STR[] = "IPV4V6";

#define RADIO_ACCESS_FAMILY_GSM \
    (RAF_GSM|RAF_GPRS|RAF_EDGE)
#define RADIO_ACCESS_FAMILY_UMTS \
    (RAF_UMTS|RAF_HSDPA|RAF_HSUPA|RAF_HSPA|RAF_HSPAP|RAF_TD_SCDMA|RAF_EHRPD)
#define RADIO_ACCESS_FAMILY_LTE \
    (RAF_LTE|RAF_LTE_CA|RAF_EHRPD)
#define RADIO_ACCESS_FAMILY_NR \
    (RAF_NR)

static
const char*
binder_pool_string(
    char* str)
{
    GUtilIdlePool* pool = gutil_idle_pool_get(&binder_util_pool);

    gutil_idle_pool_add(pool, str, g_free);
    return str;
}

RADIO_ACCESS_NETWORK
binder_radio_access_network_for_tech(
    RADIO_TECH tech)
{
    switch (tech) {
    case RADIO_TECH_GPRS:
    case RADIO_TECH_EDGE:
    case RADIO_TECH_GSM:
        return RADIO_ACCESS_NETWORK_GERAN;
    case RADIO_TECH_UMTS:
    case RADIO_TECH_HSDPA:
    case RADIO_TECH_HSPAP:
    case RADIO_TECH_HSUPA:
    case RADIO_TECH_HSPA:
    case RADIO_TECH_TD_SCDMA:
        return RADIO_ACCESS_NETWORK_UTRAN;
    case RADIO_TECH_IS95A:
    case RADIO_TECH_IS95B:
    case RADIO_TECH_ONE_X_RTT:
    case RADIO_TECH_EVDO_0:
    case RADIO_TECH_EVDO_A:
    case RADIO_TECH_EVDO_B:
    case RADIO_TECH_EHRPD:
        return RADIO_ACCESS_NETWORK_CDMA2000;
    case RADIO_TECH_LTE:
    case RADIO_TECH_LTE_CA:
        return RADIO_ACCESS_NETWORK_EUTRAN;
    case RADIO_TECH_IWLAN:
        return RADIO_ACCESS_NETWORK_IWLAN;
    case RADIO_TECH_NR:
        return RADIO_ACCESS_NETWORK_NGRAN;
    case RADIO_TECH_UNKNOWN:
        break;
    }
    return RADIO_ACCESS_NETWORK_UNKNOWN;
}

RADIO_APN_TYPES
binder_radio_apn_types_for_profile(
    guint profile_id,
    const BinderDataProfileConfig* config)
{
    RADIO_APN_TYPES apn_types = RADIO_APN_TYPE_NONE;

    if (profile_id == config->mms_profile_id) {
        apn_types |= RADIO_APN_TYPE_MMS;
    }

    if (profile_id == config->default_profile_id) {
        apn_types |= (RADIO_APN_TYPE_DEFAULT |
            RADIO_APN_TYPE_SUPL |
            RADIO_APN_TYPE_IA);
    }

    switch (profile_id) {
    case RADIO_DATA_PROFILE_IMS:
        apn_types |= RADIO_APN_TYPE_IMS;
        break;
    case RADIO_DATA_PROFILE_CBS:
        apn_types |= RADIO_APN_TYPE_CBS;
        break;
    case RADIO_DATA_PROFILE_FOTA:
        apn_types |= RADIO_APN_TYPE_FOTA;
        break;
    default:
        if (profile_id == config->mms_profile_id ||
            profile_id == config->default_profile_id) {
            break;
        }
        /* fallthrough */
    case RADIO_DATA_PROFILE_INVALID:
    case RADIO_DATA_PROFILE_DEFAULT:
        apn_types |= (RADIO_APN_TYPE_DEFAULT |
            RADIO_APN_TYPE_SUPL |
            RADIO_APN_TYPE_IA);
        break;
    }

    return apn_types;
}

RADIO_PDP_PROTOCOL_TYPE
binder_proto_from_ofono_proto(
    enum ofono_gprs_proto proto)
{
    switch (proto) {
    case OFONO_GPRS_PROTO_IP:
        return RADIO_PDP_PROTOCOL_IP;
    case OFONO_GPRS_PROTO_IPV6:
        return RADIO_PDP_PROTOCOL_IPV6;
    case OFONO_GPRS_PROTO_IPV4V6:
        return RADIO_PDP_PROTOCOL_IPV4V6;
    }
    return RADIO_PDP_PROTOCOL_UNKNOWN;
}

const char*
binder_proto_str_from_ofono_proto(
    enum ofono_gprs_proto proto)
{
    switch (proto) {
    case OFONO_GPRS_PROTO_IP:
        return PROTO_IP_STR;
    case OFONO_GPRS_PROTO_IPV6:
        return PROTO_IPV6_STR;
    case OFONO_GPRS_PROTO_IPV4V6:
        return PROTO_IPV4V6_STR;
    }
    return NULL;
}

enum ofono_gprs_proto
binder_ofono_proto_from_proto_type(
    RADIO_PDP_PROTOCOL_TYPE type)
{
    switch (type) {
    case RADIO_PDP_PROTOCOL_IP:
        return OFONO_GPRS_PROTO_IP;
    case RADIO_PDP_PROTOCOL_IPV6:
        return OFONO_GPRS_PROTO_IPV6;
    case RADIO_PDP_PROTOCOL_IPV4V6:
        return OFONO_GPRS_PROTO_IPV4V6;
    case RADIO_PDP_PROTOCOL_PPP:
    case RADIO_PDP_PROTOCOL_NON_IP:
    case RADIO_PDP_PROTOCOL_UNSTRUCTURED:
    case RADIO_PDP_PROTOCOL_UNKNOWN:
        break;
    }
    /* Need OFONO_GPRS_PROTO_UNKNOWN? */
    return OFONO_GPRS_PROTO_IP;
}

enum ofono_gprs_proto
binder_ofono_proto_from_proto_str(
    const char* type)
{
    if (type) {
        if (!g_ascii_strcasecmp(type, PROTO_IP_STR)) {
            return OFONO_GPRS_PROTO_IP;
        } else if (!g_ascii_strcasecmp(type, PROTO_IPV6_STR)) {
            return OFONO_GPRS_PROTO_IPV6;
        } else if (!g_ascii_strcasecmp(type, PROTO_IPV4V6_STR)) {
            return OFONO_GPRS_PROTO_IPV4V6;
        }
    }
    /* Need OFONO_GPRS_PROTO_UNKNOWN? */
    return OFONO_GPRS_PROTO_IP;
}

RADIO_APN_AUTH_TYPE
binder_radio_auth_from_ofono_method(
    enum ofono_gprs_auth_method auth)
{
    switch (auth) {
    case OFONO_GPRS_AUTH_METHOD_NONE:
        return RADIO_APN_AUTH_NONE;
    case OFONO_GPRS_AUTH_METHOD_CHAP:
        return RADIO_APN_AUTH_CHAP;
    case OFONO_GPRS_AUTH_METHOD_PAP:
        return RADIO_APN_AUTH_PAP;
    case OFONO_GPRS_AUTH_METHOD_ANY:
        /* Use default */
        break;
    }
    /* Default */
    return RADIO_APN_AUTH_PAP_CHAP;
}

RADIO_PREF_NET_TYPE
binder_pref_from_raf(
    RADIO_ACCESS_FAMILY raf)
{
    if (raf & RADIO_ACCESS_FAMILY_GSM) {
        if (raf & RADIO_ACCESS_FAMILY_UMTS) {
            if (raf & RADIO_ACCESS_FAMILY_LTE) {
                if (raf & RADIO_ACCESS_FAMILY_NR) {
                    return RADIO_PREF_NET_NR_LTE_GSM_WCDMA;
                }
                return RADIO_PREF_NET_LTE_GSM_WCDMA;
            }
            return RADIO_PREF_NET_GSM_WCDMA;
        }
        return RADIO_PREF_NET_GSM_ONLY;
    } else if (raf & RADIO_ACCESS_FAMILY_UMTS) {
        if (raf & RADIO_ACCESS_FAMILY_LTE) {
            if (raf & RADIO_ACCESS_FAMILY_NR) {
                return RADIO_PREF_NET_NR_LTE_WCDMA;
            }
            return RADIO_PREF_NET_LTE_WCDMA;
        }
        return RADIO_PREF_NET_WCDMA;
    } else if (raf & RADIO_ACCESS_FAMILY_LTE) {
        if (raf & RADIO_ACCESS_FAMILY_NR) {
            return RADIO_PREF_NET_NR_LTE;
        }
        return RADIO_PREF_NET_LTE_ONLY;
    } else if (raf & RADIO_ACCESS_FAMILY_NR) {
        return RADIO_PREF_NET_NR_ONLY;
    } else {
        return RADIO_PREF_NET_INVALID;
    }
}

static
int
binder_pref_mask(
    RADIO_PREF_NET_TYPE pref,
    int none,
    int gsm_mask,
    int umts_mask,
    int lte_mask,
    int nr_mask)
{
    switch (pref) {
    case RADIO_PREF_NET_GSM_ONLY:
        return gsm_mask;

    case RADIO_PREF_NET_WCDMA:
    case RADIO_PREF_NET_TD_SCDMA_ONLY:
    case RADIO_PREF_NET_TD_SCDMA_WCDMA:
        return umts_mask;

    case RADIO_PREF_NET_LTE_ONLY:
    case RADIO_PREF_NET_LTE_CDMA_EVDO:
        return lte_mask;

    case RADIO_PREF_NET_NR_ONLY:
        return nr_mask;

    case RADIO_PREF_NET_NR_LTE:
        return lte_mask | nr_mask;

    case RADIO_PREF_NET_TD_SCDMA_GSM:
    case RADIO_PREF_NET_GSM_WCDMA:
    case RADIO_PREF_NET_GSM_WCDMA_AUTO:
    case RADIO_PREF_NET_GSM_WCDMA_CDMA_EVDO_AUTO:
    case RADIO_PREF_NET_TD_SCDMA_GSM_WCDMA:
    case RADIO_PREF_NET_TD_SCDMA_GSM_WCDMA_CDMA_EVDO_AUTO:
        return gsm_mask | umts_mask;

    case RADIO_PREF_NET_LTE_WCDMA:
    case RADIO_PREF_NET_TD_SCDMA_LTE:
    case RADIO_PREF_NET_TD_SCDMA_WCDMA_LTE:
        return umts_mask | lte_mask;

    case RADIO_PREF_NET_LTE_GSM_WCDMA:
    case RADIO_PREF_NET_TD_SCDMA_GSM_LTE:
    case RADIO_PREF_NET_LTE_CMDA_EVDO_GSM_WCDMA:
    case RADIO_PREF_NET_TD_SCDMA_GSM_WCDMA_LTE:
    case RADIO_PREF_NET_TD_SCDMA_LTE_CDMA_EVDO_GSM_WCDMA:
        return gsm_mask | umts_mask | lte_mask;

    case RADIO_PREF_NET_NR_LTE_CDMA_EVDO:
    case RADIO_PREF_NET_NR_LTE_WCDMA:
    case RADIO_PREF_NET_NR_LTE_TD_SCDMA:
    case RADIO_PREF_NET_NR_LTE_TD_SCDMA_WCDMA:
        return umts_mask | lte_mask | nr_mask;

    case RADIO_PREF_NET_NR_LTE_GSM_WCDMA:
    case RADIO_PREF_NET_NR_LTE_TD_SCDMA_GSM:
    case RADIO_PREF_NET_NR_LTE_CDMA_EVDO_GSM_WCDMA:
    case RADIO_PREF_NET_NR_LTE_TD_SCDMA_GSM_WCDMA:
    case RADIO_PREF_NET_NR_LTE_TD_SCDMA_CDMA_EVDO_GSM_WCDMA:
        return gsm_mask | umts_mask | lte_mask | nr_mask;

    case RADIO_PREF_NET_CDMA_ONLY:
    case RADIO_PREF_NET_EVDO_ONLY:
    case RADIO_PREF_NET_CDMA_EVDO_AUTO:
    case RADIO_PREF_NET_INVALID:
        return none;
    }

    DBG("unexpected pref mode %d", pref);
    return none;
}

RADIO_ACCESS_FAMILY
binder_raf_from_pref(
    RADIO_PREF_NET_TYPE pref)
{
    return binder_pref_mask(pref, RAF_NONE,
        RADIO_ACCESS_FAMILY_GSM, RADIO_ACCESS_FAMILY_UMTS,
        RADIO_ACCESS_FAMILY_LTE, RADIO_ACCESS_FAMILY_NR);
}

enum ofono_radio_access_mode
binder_access_modes_from_pref(
    RADIO_PREF_NET_TYPE pref)
{
    return binder_pref_mask(pref, OFONO_RADIO_ACCESS_MODE_NONE,
        OFONO_RADIO_ACCESS_MODE_GSM, OFONO_RADIO_ACCESS_MODE_UMTS,
        OFONO_RADIO_ACCESS_MODE_LTE, OFONO_RADIO_ACCESS_MODE_NR);
}

enum ofono_radio_access_mode
binder_access_modes_from_raf(
    RADIO_ACCESS_FAMILY raf)
{

   if (raf == RAF_UNKNOWN) {
        return OFONO_RADIO_ACCESS_MODE_ALL;
   } else {
       enum ofono_radio_access_mode modes = OFONO_RADIO_ACCESS_MODE_NONE;

       if (raf & RADIO_ACCESS_FAMILY_GSM) {
           modes |= OFONO_RADIO_ACCESS_MODE_GSM;
       }
       if (raf & RADIO_ACCESS_FAMILY_UMTS) {
           modes |= OFONO_RADIO_ACCESS_MODE_UMTS;
       }
       if (raf & RADIO_ACCESS_FAMILY_LTE) {
           modes |= OFONO_RADIO_ACCESS_MODE_LTE;
       }
       if (raf & RADIO_ACCESS_FAMILY_NR) {
           modes |= OFONO_RADIO_ACCESS_MODE_NR;
       }
       return modes;
    }
}

enum ofono_radio_access_mode
binder_access_modes_up_to(
    enum ofono_radio_access_mode mode)
{
    /* Make sure only one bit is set in max_mode */
    enum ofono_radio_access_mode max_mode = ofono_radio_access_max_mode(mode);

    /* Treat ANY (zero) as ALL, otherwise set all lower bits */
    return (max_mode == OFONO_RADIO_ACCESS_MODE_ANY) ?
        OFONO_RADIO_ACCESS_MODE_ALL : (max_mode | (max_mode - 1));
}

enum ofono_access_technology
binder_access_tech_from_radio_tech(
    RADIO_TECH radio_tech)
{
    switch (radio_tech) {
    case RADIO_TECH_UNKNOWN:
        return OFONO_ACCESS_TECHNOLOGY_NONE;
    case RADIO_TECH_GPRS:
    case RADIO_TECH_GSM:
        return OFONO_ACCESS_TECHNOLOGY_GSM;
    case RADIO_TECH_EDGE:
        return OFONO_ACCESS_TECHNOLOGY_GSM_EGPRS;
    case RADIO_TECH_UMTS:
        return OFONO_ACCESS_TECHNOLOGY_UTRAN;
    case RADIO_TECH_HSDPA:
        return OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA;
    case RADIO_TECH_HSUPA:
        return OFONO_ACCESS_TECHNOLOGY_UTRAN_HSUPA;
    case RADIO_TECH_HSPA:
    case RADIO_TECH_HSPAP:
        return OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA;
    case RADIO_TECH_LTE:
    case RADIO_TECH_LTE_CA:
        return OFONO_ACCESS_TECHNOLOGY_EUTRAN;
    case RADIO_TECH_NR:
        return OFONO_ACCESS_TECHNOLOGY_NR_5GCN;
    case RADIO_TECH_IWLAN:
    case RADIO_TECH_IS95B:
    case RADIO_TECH_ONE_X_RTT:
    case RADIO_TECH_EVDO_0:
    case RADIO_TECH_EVDO_A:
    case RADIO_TECH_EVDO_B:
    case RADIO_TECH_EHRPD:
    case RADIO_TECH_TD_SCDMA:
    case RADIO_TECH_IS95A:
        break;
    }

    DBG("Unknown radio tech %d", radio_tech);
    return OFONO_ACCESS_TECHNOLOGY_NONE;
}

const char*
binder_ofono_access_technology_string(
    enum ofono_access_technology act)
{
    switch (act) {
    case OFONO_ACCESS_TECHNOLOGY_NONE:
        return "none";
    case OFONO_ACCESS_TECHNOLOGY_GSM:
        return "gsm";
    case OFONO_ACCESS_TECHNOLOGY_GSM_COMPACT:
        return "gsmc";
    case OFONO_ACCESS_TECHNOLOGY_UTRAN:
        return "utran";
    case OFONO_ACCESS_TECHNOLOGY_GSM_EGPRS:
        return "egprs";
    case OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA:
        return "hsdpa";
    case OFONO_ACCESS_TECHNOLOGY_UTRAN_HSUPA:
        return "hsupa";
    case OFONO_ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA:
        return "utran";
    case OFONO_ACCESS_TECHNOLOGY_EUTRAN:
        return "eutran";
    case OFONO_ACCESS_TECHNOLOGY_EUTRA_5GCN:
        return "eutran";
    case OFONO_ACCESS_TECHNOLOGY_NR_5GCN:
    case OFONO_ACCESS_TECHNOLOGY_NG_RAN:
    case OFONO_ACCESS_TECHNOLOGY_EUTRA_NR:
        return "nr";
    }
    return binder_pool_string(g_strdup_printf("%d (?)", act));
}

const char*
binder_radio_op_status_string(
    RADIO_OP_STATUS status)
{
    switch (status) {
    case RADIO_OP_AVAILABLE:
        return "available";
    case RADIO_OP_CURRENT:
        return "current";
    case RADIO_OP_FORBIDDEN:
        return "forbidden";
    case RADIO_OP_STATUS_UNKNOWN:
        break;
    }
    return "unknown";
}

const char*
binder_radio_state_string(
    RADIO_STATE state)
{
#define RADIO_STATE_(name) case RADIO_STATE_##name: return #name
    switch (state) {
    RADIO_STATE_(OFF);
    RADIO_STATE_(UNAVAILABLE);
    RADIO_STATE_(ON);
    }
    return binder_pool_string(g_strdup_printf("%d (?)", state));
}

const char*
binder_radio_error_string(
    RADIO_ERROR error)
{
    switch (error) {
#define RADIO_ERROR_STR_(name) case RADIO_ERROR_##name: return #name
    RADIO_ERROR_STR_(NONE);
    RADIO_ERROR_STR_(RADIO_NOT_AVAILABLE);
    RADIO_ERROR_STR_(GENERIC_FAILURE);
    RADIO_ERROR_STR_(PASSWORD_INCORRECT);
    RADIO_ERROR_STR_(SIM_PIN2);
    RADIO_ERROR_STR_(SIM_PUK2);
    RADIO_ERROR_STR_(REQUEST_NOT_SUPPORTED);
    RADIO_ERROR_STR_(CANCELLED);
    RADIO_ERROR_STR_(OP_NOT_ALLOWED_DURING_VOICE_CALL);
    RADIO_ERROR_STR_(OP_NOT_ALLOWED_BEFORE_REG_TO_NW);
    RADIO_ERROR_STR_(SMS_SEND_FAIL_RETRY);
    RADIO_ERROR_STR_(SIM_ABSENT);
    RADIO_ERROR_STR_(SUBSCRIPTION_NOT_AVAILABLE);
    RADIO_ERROR_STR_(MODE_NOT_SUPPORTED);
    RADIO_ERROR_STR_(FDN_CHECK_FAILURE);
    RADIO_ERROR_STR_(ILLEGAL_SIM_OR_ME);
    RADIO_ERROR_STR_(MISSING_RESOURCE);
    RADIO_ERROR_STR_(NO_SUCH_ELEMENT);
    RADIO_ERROR_STR_(DIAL_MODIFIED_TO_USSD);
    RADIO_ERROR_STR_(DIAL_MODIFIED_TO_SS);
    RADIO_ERROR_STR_(DIAL_MODIFIED_TO_DIAL);
    RADIO_ERROR_STR_(USSD_MODIFIED_TO_DIAL);
    RADIO_ERROR_STR_(USSD_MODIFIED_TO_SS);
    RADIO_ERROR_STR_(USSD_MODIFIED_TO_USSD);
    RADIO_ERROR_STR_(SS_MODIFIED_TO_DIAL);
    RADIO_ERROR_STR_(SS_MODIFIED_TO_USSD);
    RADIO_ERROR_STR_(SUBSCRIPTION_NOT_SUPPORTED);
    RADIO_ERROR_STR_(SS_MODIFIED_TO_SS);
    RADIO_ERROR_STR_(LCE_NOT_SUPPORTED);
    RADIO_ERROR_STR_(NO_MEMORY);
    RADIO_ERROR_STR_(INTERNAL_ERR);
    RADIO_ERROR_STR_(SYSTEM_ERR);
    RADIO_ERROR_STR_(MODEM_ERR);
    RADIO_ERROR_STR_(INVALID_STATE);
    RADIO_ERROR_STR_(NO_RESOURCES);
    RADIO_ERROR_STR_(SIM_ERR);
    RADIO_ERROR_STR_(INVALID_ARGUMENTS);
    RADIO_ERROR_STR_(INVALID_SIM_STATE);
    RADIO_ERROR_STR_(INVALID_MODEM_STATE);
    RADIO_ERROR_STR_(INVALID_CALL_ID);
    RADIO_ERROR_STR_(NO_SMS_TO_ACK);
    RADIO_ERROR_STR_(NETWORK_ERR);
    RADIO_ERROR_STR_(REQUEST_RATE_LIMITED);
    RADIO_ERROR_STR_(SIM_BUSY);
    RADIO_ERROR_STR_(SIM_FULL);
    RADIO_ERROR_STR_(NETWORK_REJECT);
    RADIO_ERROR_STR_(OPERATION_NOT_ALLOWED);
    RADIO_ERROR_STR_(EMPTY_RECORD);
    RADIO_ERROR_STR_(INVALID_SMS_FORMAT);
    RADIO_ERROR_STR_(ENCODING_ERR);
    RADIO_ERROR_STR_(INVALID_SMSC_ADDRESS);
    RADIO_ERROR_STR_(NO_SUCH_ENTRY);
    RADIO_ERROR_STR_(NETWORK_NOT_READY);
    RADIO_ERROR_STR_(NOT_PROVISIONED);
    RADIO_ERROR_STR_(NO_SUBSCRIPTION);
    RADIO_ERROR_STR_(NO_NETWORK_FOUND);
    RADIO_ERROR_STR_(DEVICE_IN_USE);
    RADIO_ERROR_STR_(ABORTED);
    RADIO_ERROR_STR_(INVALID_RESPONSE);
    RADIO_ERROR_STR_(OEM_ERROR_1);
    RADIO_ERROR_STR_(OEM_ERROR_2);
    RADIO_ERROR_STR_(OEM_ERROR_3);
    RADIO_ERROR_STR_(OEM_ERROR_4);
    RADIO_ERROR_STR_(OEM_ERROR_5);
    RADIO_ERROR_STR_(OEM_ERROR_6);
    RADIO_ERROR_STR_(OEM_ERROR_7);
    RADIO_ERROR_STR_(OEM_ERROR_8);
    RADIO_ERROR_STR_(OEM_ERROR_9);
    RADIO_ERROR_STR_(OEM_ERROR_10);
    RADIO_ERROR_STR_(OEM_ERROR_11);
    RADIO_ERROR_STR_(OEM_ERROR_12);
    RADIO_ERROR_STR_(OEM_ERROR_13);
    RADIO_ERROR_STR_(OEM_ERROR_14);
    RADIO_ERROR_STR_(OEM_ERROR_15);
    RADIO_ERROR_STR_(OEM_ERROR_16);
    RADIO_ERROR_STR_(OEM_ERROR_17);
    RADIO_ERROR_STR_(OEM_ERROR_18);
    RADIO_ERROR_STR_(OEM_ERROR_19);
    RADIO_ERROR_STR_(OEM_ERROR_20);
    RADIO_ERROR_STR_(OEM_ERROR_21);
    RADIO_ERROR_STR_(OEM_ERROR_22);
    RADIO_ERROR_STR_(OEM_ERROR_23);
    RADIO_ERROR_STR_(OEM_ERROR_24);
    RADIO_ERROR_STR_(OEM_ERROR_25);
    }

    return binder_pool_string(g_strdup_printf("%d", error));
}

enum ofono_access_technology
binder_parse_tech(
    const char* stech,
    RADIO_TECH* radio_tech)
{
    int rt = RADIO_TECH_UNKNOWN;
    const enum ofono_access_technology at = gutil_parse_int(stech, 0, &rt) ?
        binder_access_tech_from_radio_tech(rt) : OFONO_ACCESS_TECHNOLOGY_NONE;

    if (radio_tech) {
        *radio_tech = rt;
    }
    return at;
}

gboolean
binder_parse_mcc_mnc(
    const char* str,
    struct ofono_network_operator* op)
{
    if (str) {
        int i;
        const char* ptr = str;

        /* Three digit country code */
        for (i = 0;
             i < OFONO_MAX_MCC_LENGTH && *ptr && g_ascii_isdigit(*ptr);
             i++) {
            op->mcc[i] = *ptr++;
        }
        op->mcc[i] = 0;

        if (i == OFONO_MAX_MCC_LENGTH) {
            /* Usually 2 but sometimes 3 digit network code */
            for (i = 0;
                 i < OFONO_MAX_MNC_LENGTH && *ptr && g_ascii_isdigit(*ptr);
                 i++) {
                op->mnc[i] = *ptr++;
            }
            op->mnc[i] = 0;

            if (i > 0) {
                /*
                 * Sometimes MCC/MNC are followed by + and what looks
                 * like the technology code. This seems to be modem
                 * specific.
                 */
                if (*ptr == '+') {
                    const enum ofono_access_technology at =
                        binder_parse_tech(ptr + 1, NULL);

                    if (at != OFONO_ACCESS_TECHNOLOGY_NONE) {
                        op->tech = at;
                    }
                }
                return TRUE;
            }
        }
    }
    return FALSE;
}

char*
binder_encode_hex(
    const void* in,
    guint size)
{
    char *out = g_new(char, size * 2 + 1);

    ofono_encode_hex(in, size, out);
    return out;
}

void*
binder_decode_hex(
    const char* hex,
    int len,
    guint* out_size)
{
    void* out = NULL;
    guint size = 0;

    if (hex) {
        if (len < 0) {
            len = (int) strlen(hex);
        }
        if (len > 0 && !(len & 1)) {
            size = len/2;
            out = g_malloc(size);
            if (!gutil_hex2bin(hex, len, out)) {
                g_free(out);
                out = NULL;
                size = 0;
            }
        }
    }
    if (out_size) {
        *out_size = size;
    }
    return out;
}

const char*
binder_print_strv(
    char** strv,
    const char* sep)
{
    if (!strv) {
        return NULL;
    } else if (!strv[0]) {
        return binder_empty_str;
    } else {
        GUtilIdlePool* pool = gutil_idle_pool_get(&binder_util_pool);
        char* str = g_strjoinv(sep, strv);

        gutil_idle_pool_add(pool, str, g_free);
        return str;
    }
}

const char*
binder_print_hex(
    const void* data,
    gsize size)
{
    if (data && size) {
        const guint8* bytes = data;
        GUtilIdlePool* pool = gutil_idle_pool_get(&binder_util_pool);
        char* str = g_new(char, size * 2 + 1);
        char* ptr = str;
        gsize i;

        for (i = 0; i < size; i++) {
            static const char hex[] = "0123456789abcdef";
            const guint8 b = bytes[i];

            *ptr++ = hex[(b >> 4) & 0xf];
            *ptr++ = hex[b & 0xf];
        }
        *ptr++ = 0;
        gutil_idle_pool_add(pool, str, g_free);
        return str;
    }
    return binder_empty_str;
}

gboolean
binder_submit_request(
    RadioRequestGroup* g,
    RADIO_REQ code)
{
    RadioRequest* req = radio_request_new2(g, code, NULL, NULL, NULL, NULL);
    gboolean ok = radio_request_submit(req);

    radio_request_unref(req);
    return ok;
}

gboolean
binder_submit_request2(
    RadioRequestGroup* g,
    RADIO_REQ code,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    RadioRequest* req = radio_request_new2(g, code, NULL, complete, destroy,
        user_data);
    gboolean ok = radio_request_submit(req);

    radio_request_unref(req);
    return ok;
}

const char*
binder_read_hidl_string(
    const GBinderReader* args)
{
    GBinderReader reader;

    /* Read a single string arg */
    gbinder_reader_copy(&reader, args);
    return gbinder_reader_read_hidl_string_c(&reader);
}

gboolean
binder_read_int32(
    const GBinderReader* args,
    gint32* value)
{
    GBinderReader reader;

    /* Read a single int32 arg */
    gbinder_reader_copy(&reader, args);
    return gbinder_reader_read_int32(&reader, value);
}

const void*
binder_read_hidl_struct1(
    const GBinderReader* args,
    gsize size)
{
    GBinderReader reader;

    /* Read a single struct */
    gbinder_reader_copy(&reader, args);
    return gbinder_reader_read_hidl_struct1(&reader, size);
}

char**
binder_strv_from_hidl_string_vec(
    const GBinderHidlVec* vec)
{
    if (vec) {
        const GBinderHidlString* strings = vec->data.ptr;
        char** out = g_new(char*, vec->count + 1);
        char** ptr = out;
        guint i;

        for (i = 0; i < vec->count; i++, ptr++) {
            const char* str = strings[i].data.str;

            *ptr = str ? gutil_memdup(str, strings[i].len + 1) : g_strdup("");
        }
        *ptr = NULL;
        return out;
    }
    return NULL;
}

guint
binder_append_vec_with_data(
    GBinderWriter* writer,
    const void* data,
    guint elemsize,
    guint count,
    const GBinderParent* parent)
{
    GBinderHidlVec* vec = gbinder_writer_new0(writer, GBinderHidlVec);
    GBinderParent p;

    vec->data.ptr = data;
    vec->count = count;

    p.index = gbinder_writer_append_buffer_object(writer, vec, sizeof(*vec));
    p.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;

    /* Return the index of the data buffer */
    return gbinder_writer_append_buffer_object_with_parent(writer,
        data, count * elemsize, &p);
}

static
void
binder_copy_hidl_string_impl(
    GBinderWriter* writer,
    GBinderHidlString* dest,
    const char* src,
    gssize len)
{
    dest->owns_buffer = TRUE;
    if (len > 0) {
        /* GBinderWriter takes ownership of the string contents */
        dest->len = (guint32) len;
        dest->data.str = gbinder_writer_memdup(writer, src, len + 1);
    } else {
        /* Replace NULL strings with empty strings */
        dest->data.str = binder_empty_str;
        dest->len = 0;
    }
}

void
binder_copy_hidl_string(
    GBinderWriter* writer,
    GBinderHidlString* dest,
    const char* src)
{
    binder_copy_hidl_string_impl(writer, dest, src, src ? strlen(src) : 0);
}

void
binder_copy_hidl_string_len(
    GBinderWriter* writer,
    GBinderHidlString* dest,
    const char* src,
    gssize len)
{
    binder_copy_hidl_string_impl(writer, dest, src, (src && len < 0) ?
        strlen(src) : 0);
}

void
binder_append_hidl_string_with_parent(
    GBinderWriter* writer,
    const GBinderHidlString* str,
    guint32 index,
    guint32 offset)
{
    GBinderParent parent;

    parent.index = index;
    parent.offset = offset;

    /* Strings are NULL-terminated, hence len + 1 */
    gbinder_writer_append_buffer_object_with_parent(writer, str->data.str,
        str->len + 1, &parent);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
