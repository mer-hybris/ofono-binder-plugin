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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#ifndef BINDER_RADIO_CAPS_H
#define BINDER_RADIO_CAPS_H

#include "binder_types.h"

#include <ofono/radio-settings.h>
#include <ofono/slot.h>

struct ofono_watch;

typedef
void
(*BinderRadioCapsFunc)(
    BinderRadioCaps* caps,
    void* user_data);

typedef
void
(*BinderRadioCapsManagerFunc)(
    BinderRadioCapsManager* mgr,
    void* user_data);

/* RadioCapability pointer is NULL if functionality is unsupported */
typedef
void
(*BinderRadioCapsCheckFunc)(
    const RadioCapability* cap,
    void* user_data);

/* Caller must unref the request */
RadioRequest*
binder_radio_caps_check(
    RadioClient* client,
    BinderRadioCapsCheckFunc cb,
    void* user_data)
    G_GNUC_WARN_UNUSED_RESULT
    BINDER_INTERNAL;

/* There must be a single BinderRadioCapsManager shared by all modems */
BinderRadioCapsManager*
binder_radio_caps_manager_new(
    BinderDataManager* data)
    BINDER_INTERNAL;

BinderRadioCapsManager*
binder_radio_caps_manager_ref(
    BinderRadioCapsManager* mgr)
    BINDER_INTERNAL;

void
binder_radio_caps_manager_unref(
    BinderRadioCapsManager* mgr)
    BINDER_INTERNAL;

gulong
binder_radio_caps_manager_add_tx_aborted_handler(
    BinderRadioCapsManager* mgr,
    BinderRadioCapsManagerFunc cb,
    void* user_data)
    BINDER_INTERNAL;

gulong
binder_radio_caps_manager_add_tx_done_handler(
    BinderRadioCapsManager* mgr,
    BinderRadioCapsManagerFunc cb,
    void* user_data)
    BINDER_INTERNAL;

void
binder_radio_caps_manager_remove_handler(
    BinderRadioCapsManager* mgr,
    gulong id)
    BINDER_INTERNAL;

void
binder_radio_caps_manager_remove_handlers(
    BinderRadioCapsManager* mgr,
    gulong* ids,
    int count)
    BINDER_INTERNAL;

#define binder_radio_caps_manager_remove_all_handlers(mgr, ids) \
    binder_radio_caps_manager_remove_handlers(mgr, ids, G_N_ELEMENTS(ids))

/* And one BinderRadioCaps object per modem */

struct binder_radio_caps {
    BinderRadioCapsManager* mgr;
    RADIO_ACCESS_FAMILY raf;
};

BinderRadioCaps*
binder_radio_caps_new(
    BinderRadioCapsManager* mgr,
    const char* log_prefix,
    RadioClient* client,
    struct ofono_watch* watch,
    BinderData* data,
    BinderRadio* radio,
    BinderSimCard* sim,
    BinderSimSettings* settings,
    const BinderSlotConfig* config,
    const RadioCapability* cap)
    BINDER_INTERNAL;

BinderRadioCaps*
binder_radio_caps_ref(
    BinderRadioCaps* caps)
    BINDER_INTERNAL;

void
binder_radio_caps_unref(
    BinderRadioCaps* caps)
    BINDER_INTERNAL;

void
binder_radio_caps_drop(
    BinderRadioCaps* caps)
    BINDER_INTERNAL;

gulong
binder_radio_caps_add_raf_handler(
    BinderRadioCaps* caps,
    BinderRadioCapsFunc cb,
    void* user_data)
    BINDER_INTERNAL;

void
binder_radio_caps_remove_handler(
    BinderRadioCaps* caps,
    gulong id)
    BINDER_INTERNAL;

/* Data requests */

BinderRadioCapsRequest*
binder_radio_caps_request_new(
    BinderRadioCaps* caps,
    enum ofono_radio_access_mode modes, /* Mask */
    enum ofono_slot_data_role role)
    BINDER_INTERNAL;

void
binder_radio_caps_request_free(
    BinderRadioCapsRequest* req)
    BINDER_INTERNAL;

#endif /* BINDER_RADIO_CAPS_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
