/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2019-2021 Jolla Ltd.
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

#ifndef BINDER_CONNMAN_H
#define BINDER_CONNMAN_H

#include "binder_types.h"

typedef struct binder_connman {
    gboolean valid;          /* TRUE if other fields are valid */
    gboolean present;        /* ConnMan is present on D-Bus */
    gboolean tethering;      /* At least one technology is tethering */
    gboolean wifi_connected; /* WiFi network is connected */
} BinderConnman;

typedef enum binder_connman_property {
    BINDER_CONNMAN_PROPERTY_ANY,
    BINDER_CONNMAN_PROPERTY_VALID,
    BINDER_CONNMAN_PROPERTY_PRESENT,
    BINDER_CONNMAN_PROPERTY_TETHERING,
    BINDER_CONNMAN_PROPERTY_WIFI_CONNECTED,
    BINDER_CONNMAN_PROPERTY_COUNT
} BINDER_CONNMAN_PROPERTY;

typedef
void
(*BinderConnmanPropertyFunc)(
    BinderConnman* connman,
    BINDER_CONNMAN_PROPERTY property,
    void* user_data);

BinderConnman*
binder_connman_new(
    void)
    BINDER_INTERNAL;

BinderConnman*
binder_connman_ref(
    BinderConnman* connman)
    BINDER_INTERNAL;

void
binder_connman_unref(
    BinderConnman* connman)
    BINDER_INTERNAL;

gulong
binder_connman_add_property_changed_handler(
    BinderConnman* connman,
    BINDER_CONNMAN_PROPERTY property,
    BinderConnmanPropertyFunc fn,
    void* user_data)
    BINDER_INTERNAL;

void
binder_connman_remove_handler(
    BinderConnman* connman,
    gulong id)
    BINDER_INTERNAL;

void
binder_connman_remove_handlers(
    BinderConnman* connman,
    gulong* ids,
    int n)
    BINDER_INTERNAL;

#define binder_connman_remove_all_handlers(connman, ids) \
    binder_connman_remove_handlers(connman, ids, G_N_ELEMENTS(ids))

#endif /* BINDER_CONNMAN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
