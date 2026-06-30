/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
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
binder_oplist_sized_new(
    guint reserved_size)
{
    return (BinderOpList*)g_array_sized_new(FALSE, TRUE,
        sizeof(struct ofono_network_operator), reserved_size);
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
    if (op) {
        g_array_append_vals((GArray*)oplist, op, 1);
    } else {
        g_array_set_size((GArray*)oplist, oplist->count + 1);
    }
    return oplist;
}

struct ofono_network_operator*
binder_oplist_append_op(
    BinderOpList* oplist)
{
    if (oplist) {
        g_array_set_size((GArray*)oplist, oplist->count + 1);
        return binder_oplist_last(oplist);
    }
    return NULL;
}

struct ofono_network_operator*
binder_oplist_find(
    BinderOpList* oplist,
    const char* mcc,
    const char* mnc)
{
    if (oplist) {
        guint i;

        for (i = 0; i < oplist->count; i++) {
            struct ofono_network_operator* op = oplist->op + i;

            if (!strcmp(op->mcc, mcc) && !strcmp(op->mnc, mnc)) {
                return op;
            }
        }
    }
    return NULL;
}

struct ofono_network_operator*
binder_oplist_last(
    BinderOpList* oplist)
{
    return oplist && oplist->count ? oplist->op + (oplist->count - 1) : NULL;
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
