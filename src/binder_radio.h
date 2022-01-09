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

#ifndef BINDER_RADIO_H
#define BINDER_RADIO_H

#include "binder_types.h"

typedef enum binder_radio_property {
    BINDER_RADIO_PROPERTY_ANY,
    BINDER_RADIO_PROPERTY_STATE,
    BINDER_RADIO_PROPERTY_ONLINE,
    BINDER_RADIO_PROPERTY_COUNT
} BINDER_RADIO_PROPERTY;

struct binder_radio {
    RADIO_STATE state;
    gboolean online;
};

typedef
void
(*BinderRadioPropertyFunc)(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    void* user_data);

BinderRadio*
binder_radio_new(
    RadioClient* client,
    const char* log_prefix)
    BINDER_INTERNAL;

BinderRadio*
binder_radio_ref(
    BinderRadio* radio)
    BINDER_INTERNAL;

void
binder_radio_unref(
    BinderRadio* radio)
    BINDER_INTERNAL;

void
binder_radio_power_on(
    BinderRadio* radio,
    gpointer tag)
    BINDER_INTERNAL;

void
binder_radio_power_off(
    BinderRadio* radio,
    gpointer tag)
    BINDER_INTERNAL;

void
binder_radio_power_cycle(
    BinderRadio* radio)
    BINDER_INTERNAL;

void
binder_radio_confirm_power_on(
    BinderRadio* radio)
    BINDER_INTERNAL;

void
binder_radio_set_online(
    BinderRadio* radio,
    gboolean online)
    BINDER_INTERNAL;

gulong
binder_radio_add_property_handler(
    BinderRadio* net,
    BINDER_RADIO_PROPERTY property,
    BinderRadioPropertyFunc callback,
    void* user_data)
    BINDER_INTERNAL;

void
binder_radio_remove_handler(
    BinderRadio* radio,
    gulong id)
    BINDER_INTERNAL;

void
binder_radio_remove_handlers(
    BinderRadio* radio,
    gulong* ids,
    int count)
    BINDER_INTERNAL;

#define binder_radio_remove_all_handlers(r,ids) \
    binder_radio_remove_handlers(r, ids, G_N_ELEMENTS(ids))

#endif /* BINDER_RADIO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
