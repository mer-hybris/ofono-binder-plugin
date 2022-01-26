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

#ifndef BINDER_DEVMON_H
#define BINDER_DEVMON_H

#include "binder_types.h"

#include <ofono/slot.h>

/*
 * Separate instance of BinderDevmon is created for each modem.
 * Device monitor is started after connection to the modem has
 * been established.
 */

typedef struct binder_devmon_io BinderDevmonIo;
struct binder_devmon_io {
    void (*free)(BinderDevmonIo* io);
};

struct binder_devmon {
    void (*free)(BinderDevmon* devmon);
    BinderDevmonIo* (*start_io)(BinderDevmon* devmon, RadioClient* client,
        struct ofono_slot* slot);
};

/*
 * This Device Monitor uses sendDeviceState() call to let the modem
 * choose the right power saving strategy. It basically mirrors the
 * logic of DeviceStateMonitor class in Android.
 */
BinderDevmon*
binder_devmon_ds_new(
    const BinderSlotConfig* config)
    BINDER_INTERNAL;

/*
 * This Device Monitor implementation controls network state updates
 * by calling setIndicationFilter().
 */
BinderDevmon*
binder_devmon_if_new(
    const BinderSlotConfig* config)
    BINDER_INTERNAL;

/*
 * This one combines several methods. Takes ownership of binder_devmon objects.
 */
BinderDevmon*
binder_devmon_combine(
    BinderDevmon* devmon[],
    guint n)
    BINDER_INTERNAL;

/* Utilities (NULL tolerant) */

BinderDevmonIo*
binder_devmon_start_io(
    BinderDevmon* devmon,
    RadioClient* client,
    struct ofono_slot* slot)
    BINDER_INTERNAL;

void
binder_devmon_io_free(
    BinderDevmonIo* io)
    BINDER_INTERNAL;

void
binder_devmon_free(
    BinderDevmon* devmon)
    BINDER_INTERNAL;

#endif /* BINDER_DEVMON_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
