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

#include "binder_devmon.h"

BinderDevmonIo*
binder_devmon_start_io(
    BinderDevmon* devmon,
    RadioClient* client,
    struct ofono_slot* slot)
{
    return devmon ? devmon->start_io(devmon, client, slot) : NULL;
}

void
binder_devmon_io_free(
    BinderDevmonIo* io)
{
    if (io) {
        io->free(io);
    }
}

void
binder_devmon_free(
    BinderDevmon* devmon)
{
    if (devmon) {
        devmon->free(devmon);
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
