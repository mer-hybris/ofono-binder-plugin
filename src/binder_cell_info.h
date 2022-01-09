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

#ifndef BINDER_CELL_INFO_H
#define BINDER_CELL_INFO_H

#include "binder_types.h"

#include <ofono/cell-info.h>

struct ofono_cell_info*
binder_cell_info_new(
    RadioClient* client,
    const char* log_prefix,
    BinderRadio* radio,
    BinderSimCard* sim)
    BINDER_INTERNAL;

#endif /* BINDER_CELL_INFO_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
