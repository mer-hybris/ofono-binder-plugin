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

#ifndef BINDER_LOGGER_H
#define BINDER_LOGGER_H

#include "binder_types.h"

BinderLogger*
binder_logger_new_radio_trace(
    RadioInstance* instance,
    const char* prefix)
    BINDER_INTERNAL;

BinderLogger*
binder_logger_new_radio_dump(
    RadioInstance* instance,
    const char* prefix)
    BINDER_INTERNAL;

BinderLogger*
binder_logger_new_config_trace(
    RadioConfig* config)
    BINDER_INTERNAL;

BinderLogger*
binder_logger_new_config_dump(
    RadioConfig* config)
    BINDER_INTERNAL;

void
binder_logger_free(
    BinderLogger* logger)
    BINDER_INTERNAL;

extern GLogModule binder_logger_module BINDER_INTERNAL;

#endif /* BINDER_LOGGER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
