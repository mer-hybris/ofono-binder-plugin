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

#ifndef BINDER_UTIL_H
#define BINDER_UTIL_H

#include "binder_types.h"

#include <ofono/gprs.h>
#include <ofono/radio-settings.h>

#include <radio_request.h>

struct ofono_network_operator;

#define binder_dup_prefix(prefix) \
    (((prefix) && *(prefix)) ? (g_str_has_suffix(prefix, " ") ? \
    g_strdup(prefix) : g_strconcat(prefix, " ", NULL)) : g_strdup(""))

#define binder_error_init_ok(err) \
    ((err)->error = 0, (err)->type = OFONO_ERROR_TYPE_NO_ERROR)
#define binder_error_init_failure(err) \
    ((err)->error = 0, (err)->type = OFONO_ERROR_TYPE_FAILURE)
#define binder_error_init_sim_error(err,sw1,sw2) \
    ((err)->error = (((guint)(sw1)) << 8) | (sw2), \
     (err)->type = OFONO_ERROR_TYPE_SIM)

#define binder_error_ok(err) \
    (binder_error_init_ok(err), err)
#define binder_error_failure(err) \
    (binder_error_init_failure(err), err)
#define binder_error_sim(err,sw1,sw2) \
    (binder_error_init_sim_error(err,sw1,sw2), err)

/* Internal extension for RADIO_PREF_NET_TYPE */
typedef enum radio_pref_net_type_internal {
    RADIO_PREF_NET_NR_ONLY = RADIO_PREF_NET_TD_SCDMA_LTE_CDMA_EVDO_GSM_WCDMA + 1,
    RADIO_PREF_NET_NR_LTE,
    RADIO_PREF_NET_NR_LTE_CDMA_EVDO,
    RADIO_PREF_NET_NR_LTE_GSM_WCDMA,
    RADIO_PREF_NET_NR_LTE_CDMA_EVDO_GSM_WCDMA,
    RADIO_PREF_NET_NR_LTE_WCDMA,
    RADIO_PREF_NET_NR_LTE_TD_SCDMA,
    RADIO_PREF_NET_NR_LTE_TD_SCDMA_GSM,
    RADIO_PREF_NET_NR_LTE_TD_SCDMA_WCDMA,
    RADIO_PREF_NET_NR_LTE_TD_SCDMA_GSM_WCDMA,
    RADIO_PREF_NET_NR_LTE_TD_SCDMA_CDMA_EVDO_GSM_WCDMA
} RADIO_PREF_NET_TYPE_INTERNAL;

RADIO_ACCESS_NETWORK
binder_radio_access_network_for_tech(
    RADIO_TECH tech)
    BINDER_INTERNAL;

RADIO_APN_TYPES
binder_radio_apn_types_for_profile(
    guint profile_id,
    const BinderDataProfileConfig* config)
    BINDER_INTERNAL;

RADIO_PDP_PROTOCOL_TYPE
binder_proto_from_ofono_proto(
    enum ofono_gprs_proto proto)
    BINDER_INTERNAL;

const char*
binder_proto_str_from_ofono_proto(
    enum ofono_gprs_proto proto)
    BINDER_INTERNAL;

enum ofono_gprs_proto
binder_ofono_proto_from_proto_type(
    RADIO_PDP_PROTOCOL_TYPE type)
    BINDER_INTERNAL;

enum ofono_gprs_proto
binder_ofono_proto_from_proto_str(
    const char* type)
    BINDER_INTERNAL;

RADIO_APN_AUTH_TYPE
binder_radio_auth_from_ofono_method(
    enum ofono_gprs_auth_method auth)
    BINDER_INTERNAL;

RADIO_PREF_NET_TYPE
binder_pref_from_raf(
    RADIO_ACCESS_FAMILY raf)
    BINDER_INTERNAL;

RADIO_ACCESS_FAMILY
binder_raf_from_pref(
    RADIO_PREF_NET_TYPE pref)
    BINDER_INTERNAL;

enum ofono_radio_access_mode
binder_access_modes_from_pref(
    RADIO_PREF_NET_TYPE pref)
    BINDER_INTERNAL;

enum ofono_radio_access_mode
binder_access_modes_from_raf(
    RADIO_ACCESS_FAMILY raf)
    BINDER_INTERNAL;

enum ofono_radio_access_mode
binder_access_modes_up_to(
    enum ofono_radio_access_mode mode)
    BINDER_INTERNAL;

enum ofono_access_technology
binder_access_tech_from_radio_tech(
    RADIO_TECH radio_tech)
    BINDER_INTERNAL;

const char*
binder_ofono_access_technology_string(
    enum ofono_access_technology tech)
    BINDER_INTERNAL;

const char*
binder_radio_op_status_string(
    RADIO_OP_STATUS status)
    BINDER_INTERNAL;

const char*
binder_radio_state_string(
    RADIO_STATE state)
    BINDER_INTERNAL;

const char*
binder_radio_error_string(
    RADIO_ERROR error)
    BINDER_INTERNAL;

enum ofono_access_technology
binder_parse_tech(
    const char* stech,
    RADIO_TECH* radio_tech)
    BINDER_INTERNAL;

gboolean
binder_parse_mcc_mnc(
    const char* str,
    struct ofono_network_operator* op)
    BINDER_INTERNAL;

char*
binder_encode_hex(
    const void* in,
    guint size)
    BINDER_INTERNAL;

void*
binder_decode_hex(
    const char* hex,
    int len,
    guint* out_size)
    BINDER_INTERNAL;

const char*
binder_print_strv(
    char** strv,
    const char* sep)
    BINDER_INTERNAL;

const char*
binder_print_hex(
    const void* data,
    gsize size)
    BINDER_INTERNAL;

gboolean
binder_submit_request(
    RadioRequestGroup* g,
    RADIO_REQ code)
    BINDER_INTERNAL;

gboolean
binder_submit_request2(
    RadioRequestGroup* g,
    RADIO_REQ code,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
    BINDER_INTERNAL;

const char*
binder_read_hidl_string(
    const GBinderReader* args)
    BINDER_INTERNAL;

gboolean
binder_read_int32(
    const GBinderReader* args,
    gint32* value)
    BINDER_INTERNAL;

const void*
binder_read_hidl_struct1(
    const GBinderReader* reader,
    gsize size);

#define binder_read_hidl_struct(reader,type) \
    ((const type*)binder_read_hidl_struct1(reader, sizeof(type)))

char**
binder_strv_from_hidl_string_vec(
    const GBinderHidlVec* vec)
    BINDER_INTERNAL;

guint
binder_append_vec_with_data(
    GBinderWriter* writer,
    const void* data,
    guint elemsize,
    guint count,
    const GBinderParent* parent)
    BINDER_INTERNAL;

void
binder_copy_hidl_string(
    GBinderWriter* writer,
    GBinderHidlString* dest,
    const char* src)
    BINDER_INTERNAL;

void
binder_copy_hidl_string_len(
    GBinderWriter* writer,
    GBinderHidlString* dest,
    const char* src,
    gssize len)
    BINDER_INTERNAL;

void
binder_append_hidl_string_with_parent(
    GBinderWriter* writer,
    const GBinderHidlString* str,
    guint32 index,
    guint32 offset)
    BINDER_INTERNAL;

#define binder_append_hidl_string(writer,str) \
    gbinder_writer_append_hidl_string_copy(writer,str)
#define binder_append_hidl_string_data(writer,ptr,field,index) \
    binder_append_hidl_string_data2(writer,ptr,field,index,0)
#define binder_append_hidl_string_data2(writer,ptr,field,index,off) \
    binder_append_hidl_string_with_parent(writer, &ptr->field, index, \
        (off) + ((guint8*)(&ptr->field) - (guint8*)ptr))

#endif /* BINDER_UTIL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
