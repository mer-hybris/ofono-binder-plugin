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

#ifndef BINDER_NETREG_H
#define BINDER_NETREG_H

#include "binder_types.h"

#include <ofono/netreg.h>

void
binder_netreg_init(void)
    BINDER_INTERNAL;

void
binder_netreg_cleanup(void)
    BINDER_INTERNAL;

enum ofono_netreg_status
binder_netreg_check_if_really_roaming(
    struct ofono_netreg* reg,
    enum ofono_netreg_status status)
    BINDER_INTERNAL;

#endif /* BINDER_NETREG_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
