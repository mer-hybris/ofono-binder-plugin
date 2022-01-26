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

#ifndef BINDER_BASE_H
#define BINDER_BASE_H

#include "binder_types.h"

#include <glib-object.h>

typedef struct binder_base_class {
    GObjectClass object;
    int public_offset;
} BinderBaseClass;

typedef struct binder_base {
    GObject object;
    gsize queued_signals;
} BinderBase;

BINDER_INTERNAL GType binder_base_get_type(void);
#define BINDER_TYPE_BASE (binder_base_get_type())
#define BINDER_BASE_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), \
    BINDER_TYPE_BASE, BinderBaseClass))

typedef enum binder_base_property {
    BINDER_BASE_PROPERTY_ANY = 0,
    BINDER_BASE_MAX_PROPERTIES = 8
} BINDER_BASE_PROPERTY;

#define BINDER_BASE_PROPERTY_BIT(property) (1 << (property - 1))
#define BINDER_BASE_ASSERT_COUNT(count) \
    G_STATIC_ASSERT((int)count <= (int)BINDER_BASE_MAX_PROPERTIES)

gulong
binder_base_add_property_handler(
    BinderBase* base,
    guint property,
    GCallback callback,
    gpointer user_data)
    BINDER_INTERNAL;

void
binder_base_queue_property_change(
    BinderBase* base,
    guint property)
    BINDER_INTERNAL;

void
binder_base_emit_property_change(
    BinderBase* base,
    guint property)
    BINDER_INTERNAL;

void
binder_base_emit_queued_signals(
    BinderBase* base)
    BINDER_INTERNAL;

#endif /* BINDER_BASE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
