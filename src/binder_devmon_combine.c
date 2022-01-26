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

#include "binder_devmon.h"

#include <ofono/log.h>

#include <gutil_macros.h>

typedef struct binder_devmon_combine {
    BinderDevmon pub;
    BinderDevmon** impl;
    guint count;
} DevMon;

typedef struct binder_devmon_combine_io {
    BinderDevmonIo pub;
    BinderDevmonIo** impl;
    guint count;
} DevMonIo;

static inline DevMon* binder_devmon_combine_cast(BinderDevmon* devmon)
    { return G_CAST(devmon, DevMon, pub); }

static inline DevMonIo* binder_devmon_combine_io_cast(BinderDevmonIo* io)
    { return G_CAST(io, DevMonIo, pub); }

static
void
binder_devmon_combine_io_free(
    BinderDevmonIo* io)
{
    guint i;
    DevMonIo* self = binder_devmon_combine_io_cast(io);

    for (i = 0; i < self->count; i++) {
        binder_devmon_io_free(self->impl[i]);
    }
    g_free(self);
}

static
BinderDevmonIo*
binder_devmon_combine_start_io(
    BinderDevmon* devmon,
    RadioClient* client,
    struct ofono_slot* slot)
{
    guint i;
    DevMon* self = binder_devmon_combine_cast(devmon);
    DevMonIo* io = g_malloc0(sizeof(DevMonIo) +
        sizeof(BinderDevmonIo*) * self->count);

    io->pub.free = binder_devmon_combine_io_free;
    io->impl = (BinderDevmonIo**)(io + 1);
    io->count = self->count;
    for (i = 0; i < io->count; i++) {
        io->impl[i] = binder_devmon_start_io(self->impl[i], client, slot);
    }
    return &io->pub;
}

static
void
binder_devmon_combine_free(
    BinderDevmon* dm)
{
    DevMon* self = binder_devmon_combine_cast(dm);
    guint i;

    for (i = 0; i < self->count; i++) {
        binder_devmon_free(self->impl[i]);
    }
    g_free(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderDevmon*
binder_devmon_combine(
    BinderDevmon* dm[],
    guint n)
{
    guint i;
    DevMon* self = g_malloc0(sizeof(DevMon) + sizeof(BinderDevmon*) * n);

    self->pub.free = binder_devmon_combine_free;
    self->pub.start_io = binder_devmon_combine_start_io;
    self->impl = (BinderDevmon**)(self + 1);
    self->count = n;
    for (i = 0; i < n; i++) {
        self->impl[i] = dm[i];
    }
    return &self->pub;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
