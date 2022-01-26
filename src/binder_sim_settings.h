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

#ifndef BINDER_SIM_SETTINGS_H
#define BINDER_SIM_SETTINGS_H

#include "binder_types.h"

#include <ofono/radio-settings.h>

typedef enum binder_sim_settings_property {
    BINDER_SIM_SETTINGS_PROPERTY_ANY,
    BINDER_SIM_SETTINGS_PROPERTY_IMSI,
    BINDER_SIM_SETTINGS_PROPERTY_PREF,
    BINDER_SIM_SETTINGS_PROPERTY_COUNT
} BINDER_SIM_SETTINGS_PROPERTY;

struct binder_sim_settings {
    const char* imsi;
    enum ofono_radio_access_mode techs; /* Mask */
    enum ofono_radio_access_mode pref;  /* Mask */
};

typedef
void
(*BinderSimSettingsPropertyFunc)(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data);

BinderSimSettings*
binder_sim_settings_new(
    const char* path,
    enum ofono_radio_access_mode techs)
    BINDER_INTERNAL;

BinderSimSettings*
binder_sim_settings_ref(
    BinderSimSettings* settings)
    BINDER_INTERNAL;

void
binder_sim_settings_unref(
    BinderSimSettings* settings)
    BINDER_INTERNAL;

void
binder_sim_settings_set_pref(
    BinderSimSettings* settings,
    enum ofono_radio_access_mode pref)
    BINDER_INTERNAL;

gulong
binder_sim_settings_add_property_handler(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    BinderSimSettingsPropertyFunc callback,
    void* user_data)
    BINDER_INTERNAL;

void
binder_sim_settings_remove_handler(
    BinderSimSettings* settings,
    gulong id)
    BINDER_INTERNAL;

void
binder_sim_settings_remove_handlers(
    BinderSimSettings* settings,
    gulong* ids,
    int count)
    BINDER_INTERNAL;

#define binder_sim_settings_remove_all_handlers(settings,ids) \
    binder_sim_settings_remove_handlers(settings, ids, G_N_ELEMENTS(ids))

#endif /* BINDER_SIM_SETTINGS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
