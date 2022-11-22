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

#ifndef BINDER_OPLIST_H
#define BINDER_OPLIST_H

#include "binder_types.h"

/*
 * This is basically a GArray providing better type safety at compile time.
 * If NULL is passed to binder_oplist_set_count() and binder_oplist_append()
 * they allocate a new list with binder_oplist_new() and return it.
 */
typedef struct binder_oplist {
    struct ofono_network_operator* op;
    guint count;
} BinderOpList;

BinderOpList*
binder_oplist_new()
    BINDER_INTERNAL;

BinderOpList*
binder_oplist_set_count(
    BinderOpList* oplist,
    guint count)
    BINDER_INTERNAL;

BinderOpList*
binder_oplist_append(
    BinderOpList* oplist,
    const struct ofono_network_operator* op)
    BINDER_INTERNAL;

void
binder_oplist_free(
    BinderOpList* oplist)
    BINDER_INTERNAL;

#endif /* BINDER_OPLIST_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
