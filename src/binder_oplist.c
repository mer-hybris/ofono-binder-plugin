/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2022 Jolla Ltd.
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

#include "binder_oplist.h"

#include <ofono/netreg.h>

BinderOpList*
binder_oplist_new()
{
    return (BinderOpList*) g_array_new(FALSE, TRUE,
        sizeof(struct ofono_network_operator));
}

BinderOpList*
binder_oplist_set_count(
    BinderOpList* oplist,
    guint count)
{
    if (!oplist) {
        oplist = binder_oplist_new();
    }
    g_array_set_size((GArray*)oplist, count);
    return oplist;
}

BinderOpList*
binder_oplist_append(
    BinderOpList* oplist,
    const struct ofono_network_operator* op)
{
    if (!oplist) {
        oplist = binder_oplist_new();
    }
    g_array_append_vals((GArray*)oplist, op, 1);
    return oplist;
}

void
binder_oplist_free(
    BinderOpList* oplist)
{
    if (oplist) {
        g_array_free((GArray*)oplist, TRUE);
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
