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

#ifndef BINDER_NETWORK_H
#define BINDER_NETWORK_H

#include "binder_types.h"

#include <ofono/netreg.h>
#include <ofono/radio-settings.h>

typedef enum binder_network_property {
    BINDER_NETWORK_PROPERTY_ANY,
    BINDER_NETWORK_PROPERTY_VOICE_STATE,
    BINDER_NETWORK_PROPERTY_DATA_STATE,
    BINDER_NETWORK_PROPERTY_MAX_DATA_CALLS,
    BINDER_NETWORK_PROPERTY_OPERATOR,
    BINDER_NETWORK_PROPERTY_PREF_MODES,
    BINDER_NETWORK_PROPERTY_ALLOWED_MODES,
    BINDER_NETWORK_PROPERTY_COUNT
} BINDER_NETWORK_PROPERTY;

typedef struct binder_registration_state {
    enum ofono_netreg_status status;
    enum ofono_access_technology access_tech;
    RADIO_TECH radio_tech;
    gboolean em_enabled; /* TRUE is emergency calls are enabled */
    int lac;
    int ci;
} BinderRegistrationState;

struct binder_network {
    BinderSimSettings* settings;
    BinderRegistrationState voice;
    BinderRegistrationState data;
    int max_data_calls;
    const struct ofono_network_operator* operator;
    enum ofono_radio_access_mode pref_modes;     /* Mask */
    enum ofono_radio_access_mode allowed_modes;  /* Mask */
};

typedef
void
(*BinderNetworkPropertyFunc)(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    void* user_data);

BinderNetwork*
binder_network_new(
    const char* path,
    RadioClient* client,
    const char* log_prefix,
    BinderRadio* radio,
    BinderSimCard* sim_card,
    BinderSimSettings* settings,
    const BinderSlotConfig* config)
    BINDER_INTERNAL;

BinderNetwork*
binder_network_ref(
    BinderNetwork* net)
    BINDER_INTERNAL;

void
binder_network_unref(
    BinderNetwork* net)
    BINDER_INTERNAL;

void
binder_network_set_radio_caps(
    BinderNetwork* net,
    BinderRadioCaps* caps)
    BINDER_INTERNAL;

void
binder_network_set_allowed_modes(
    BinderNetwork* net,
    enum ofono_radio_access_mode modes,
    gboolean force_check)
    BINDER_INTERNAL;

enum ofono_radio_access_mode
binder_network_max_supported_mode(
    BinderNetwork* self)
    BINDER_INTERNAL;

void
binder_network_query_registration_state(
    BinderNetwork* net)
    BINDER_INTERNAL;

gulong
binder_network_add_property_handler(
    BinderNetwork* net,
    BINDER_NETWORK_PROPERTY property,
    BinderNetworkPropertyFunc callback,
    void* user_data)
    BINDER_INTERNAL;

void
binder_network_remove_handler(
    BinderNetwork* net,
    gulong id)
    BINDER_INTERNAL;

void
binder_network_remove_handlers(
    BinderNetwork* net,
    gulong* ids,
    int count)
    BINDER_INTERNAL;

#define binder_network_remove_all_handlers(net, ids) \
    binder_network_remove_handlers(net, ids, G_N_ELEMENTS(ids))

#endif /* BINDER_NETWORK_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
