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

#ifndef BINDER_DATA_H
#define BINDER_DATA_H

#include "binder_types.h"

#include <radio_request.h>

#include <ofono/slot.h>
#include <ofono/gprs-context.h>

typedef enum binder_data_property {
    BINDER_DATA_PROPERTY_ANY,
    BINDER_DATA_PROPERTY_CALLS,
    BINDER_DATA_PROPERTY_ALLOWED,
    BINDER_DATA_PROPERTY_COUNT
} BINDER_DATA_PROPERTY;

typedef struct binder_data_call {
    int cid;
    RADIO_DATA_CALL_FAIL_CAUSE status;
    RADIO_DATA_CALL_ACTIVE_STATUS active;
    enum ofono_gprs_proto prot;
    int retry_time;
    int mtu;
    char* ifname;
    char** dnses;
    char** gateways;
    char** addresses;
    char** pcscf;
} BinderDataCall;

struct binder_data {
    GSList* calls;
};

typedef enum binder_data_manager_flags {
    BINDER_DATA_MANAGER_NO_FLAGS = 0x00,
    BINDER_DATA_MANAGER_3GLTE_HANDOVER = 0x01
} BINDER_DATA_MANAGER_FLAGS;

typedef enum binder_data_allow_data {
    BINDER_ALLOW_DATA_DISABLED,
    BINDER_ALLOW_DATA_ENABLED
} BINDER_DATA_ALLOW_DATA;

typedef struct binder_data_options {
    BINDER_DATA_ALLOW_DATA allow_data;
    unsigned int data_call_retry_limit;
    unsigned int data_call_retry_delay_ms;
} BinderDataOptions;

typedef struct binder_data_request BinderDataRequest;

typedef
void
(*BinderDataPropertyFunc)(
    BinderData* data,
    BINDER_DATA_PROPERTY property,
    void* user_data);

typedef
void
(*BinderDataCallSetupFunc)(
    BinderData* data,
    RADIO_ERROR status,
    const BinderDataCall* call,
    void* user_data);

typedef
void
(*BinderDataCallDeactivateFunc)(
    BinderData* data,
    RADIO_ERROR status,
    void* user_data);

BinderDataManager*
binder_data_manager_new(
    RadioConfig* config,
    BINDER_DATA_MANAGER_FLAGS flags,
    enum ofono_radio_access_mode non_data_mode)
    BINDER_INTERNAL;

BinderDataManager*
binder_data_manager_ref(
    BinderDataManager* dm)
    BINDER_INTERNAL;

void
binder_data_manager_unref(
    BinderDataManager* dm)
    BINDER_INTERNAL;

void
binder_data_manager_set_radio_config(
    BinderDataManager* dm,
    RadioConfig* rc)
    BINDER_INTERNAL;

void
binder_data_manager_check_data(
    BinderDataManager* dm)
    BINDER_INTERNAL;

void
binder_data_manager_assert_data_on(
    BinderDataManager* dm)
    BINDER_INTERNAL;

gboolean
binder_data_manager_need_set_data_allowed(
    BinderDataManager* dm)
    BINDER_INTERNAL;

BinderData*
binder_data_new(
    BinderDataManager* dm,
    RadioClient* client,
    const char* name,
    BinderRadio* radio,
    BinderNetwork* network,
    const BinderDataOptions* options,
    const BinderSlotConfig* config)
    BINDER_INTERNAL;

BinderData*
binder_data_ref(
    BinderData* data)
    BINDER_INTERNAL;

void
binder_data_unref(
    BinderData* data)
    BINDER_INTERNAL;

gboolean
binder_data_allowed(
    BinderData* data)
    BINDER_INTERNAL;

void
binder_data_poll_call_state(
    BinderData* data)
    BINDER_INTERNAL;

gulong
binder_data_add_property_handler(
    BinderData* data,
    BINDER_DATA_PROPERTY property,
    BinderDataPropertyFunc cb,
    void* user_data)
    BINDER_INTERNAL;

void
binder_data_remove_handler(
    BinderData* data,
    gulong id)
    BINDER_INTERNAL;

void
binder_data_allow(
    BinderData* data,
    enum ofono_slot_data_role role)
    BINDER_INTERNAL;

BinderDataRequest*
binder_data_call_setup(
    BinderData* data,
    const struct ofono_gprs_primary_context* ctx,
    enum ofono_gprs_context_type context_type,
    BinderDataCallSetupFunc cb,
    void* user_data)
    BINDER_INTERNAL;

BinderDataRequest*
binder_data_call_deactivate(
    BinderData* data,
    int cid,
    BinderDataCallDeactivateFunc cb,
    void* user_data)
    BINDER_INTERNAL;

void
binder_data_request_detach(
    BinderDataRequest* req)
    BINDER_INTERNAL;

void
binder_data_request_cancel(
    BinderDataRequest* req)
    BINDER_INTERNAL;

gboolean
binder_data_call_grab(
    BinderData* data,
    int cid,
    void* cookie)
    BINDER_INTERNAL;

void
binder_data_call_release(
    BinderData* data,
    int cid,
    void* cookie)
    BINDER_INTERNAL;

void
binder_data_call_free(
    BinderDataCall* call)
    BINDER_INTERNAL;

BinderDataCall*
binder_data_call_dup(
    const BinderDataCall* call)
    BINDER_INTERNAL;

BinderDataCall*
binder_data_call_find(
    GSList* list,
    int cid)
    BINDER_INTERNAL;

RadioRequest*
binder_data_deactivate_data_call_request_new(
    RadioRequestGroup* group,
    int cid,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
    BINDER_INTERNAL;

RadioRequest*
binder_data_set_data_allowed_request_new(
    RadioRequestGroup* group,
    gboolean allow,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
    BINDER_INTERNAL;

#endif /* BINDER_DATA_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
