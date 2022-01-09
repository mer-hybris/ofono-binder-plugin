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

#include "binder_base.h"
#include "binder_log.h"

typedef
void
(*BinderBasePropertyFunc)(
    gpointer source,
    guint property,
    gpointer user_data);

typedef struct binder_base_closure {
    GCClosure cclosure;
    BinderBasePropertyFunc callback;
    gpointer user_data;
} BinderBaseClosure;

#define binder_base_closure_new() ((BinderBaseClosure*) \
    g_closure_new_simple(sizeof(BinderBaseClosure), NULL))

G_DEFINE_ABSTRACT_TYPE(BinderBase, binder_base, G_TYPE_OBJECT)
#define GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS((obj), \
   BINDER_TYPE_BASE, BinderBaseClass)

#define SIGNAL_PROPERTY_CHANGED_NAME    "binder-base-property-changed"
#define SIGNAL_PROPERTY_DETAIL          "%x"
#define SIGNAL_PROPERTY_DETAIL_MAX_LEN  (8)

enum binder_base_signal {
    SIGNAL_PROPERTY_CHANGED,
    SIGNAL_COUNT
};

static guint binder_base_signals[SIGNAL_COUNT];
static GQuark binder_base_property_quarks[BINDER_BASE_MAX_PROPERTIES];

static
GQuark
binder_base_property_quark(
    guint property)
{
    GASSERT(property < BINDER_BASE_MAX_PROPERTIES);
    /* For ANY property this function is expected to return zero */
    if (property > 0 && G_LIKELY(property < BINDER_BASE_MAX_PROPERTIES)) {
        const int i = property - 1;

        if (G_UNLIKELY(!binder_base_property_quarks[i])) {
            char buf[SIGNAL_PROPERTY_DETAIL_MAX_LEN + 1];

            snprintf(buf, sizeof(buf), SIGNAL_PROPERTY_DETAIL, property);
            buf[sizeof(buf) - 1] = 0;
            binder_base_property_quarks[i] = g_quark_from_string(buf);
        }
        return binder_base_property_quarks[i];
    }
    return 0;
}

static
void
binder_base_property_changed(
    BinderBase* self,
    guint property,
    BinderBaseClosure* closure)
{
    const BinderBaseClass* klass = GET_CLASS(self);

    closure->callback(((guint8*)self) + klass->public_offset, property,
        closure->user_data);
}

/*==========================================================================*
 * API
 *==========================================================================*/

gulong
binder_base_add_property_handler(
    BinderBase* self,
    guint property,
    GCallback callback,
    gpointer user_data)
{
    if (G_LIKELY(callback)) {
        /*
         * We can't directly connect the provided callback because
         * it expects the first parameter to point to public part
         * of the object but glib will call it with BinderBase as
         * the first parameter. binder_base_property_changed() will
         * do the conversion.
         */
        BinderBaseClosure* closure = binder_base_closure_new();
        GCClosure* cc = &closure->cclosure;

        cc->closure.data = closure;
        cc->callback = G_CALLBACK(binder_base_property_changed);
        closure->callback = (BinderBasePropertyFunc)callback;
        closure->user_data = user_data;

        return g_signal_connect_closure_by_id(self,
            binder_base_signals[SIGNAL_PROPERTY_CHANGED],
            binder_base_property_quark(property), &cc->closure, FALSE);
    }
    return 0;
}

void
binder_base_queue_property_change(
    BinderBase* self,
    guint property)
{
    self->queued_signals |= BINDER_BASE_PROPERTY_BIT(property);
}

void
binder_base_emit_property_change(
    BinderBase* self,
    guint property)
{
    binder_base_queue_property_change(self, property);
    binder_base_emit_queued_signals(self);
}

void
binder_base_emit_queued_signals(
    BinderBase* self)
{
    guint p;

    /* Signal handlers may release references to this object */
    g_object_ref(self);

    /* Emit the signals */
    for (p = 0; self->queued_signals && p < BINDER_BASE_MAX_PROPERTIES; p++) {
        if (self->queued_signals & BINDER_BASE_PROPERTY_BIT(p)) {
            self->queued_signals &= ~BINDER_BASE_PROPERTY_BIT(p);
            g_signal_emit(self, binder_base_signals[SIGNAL_PROPERTY_CHANGED],
                binder_base_property_quark(p), p);
        }
    }

    /* Release the temporary reference */
    g_object_unref(self);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_base_init(
    BinderBase* self)
{
}

static
void
binder_base_class_init(
    BinderBaseClass* klass)
{
    /* By default assume that public part immediately follows BinderBase */
    klass->public_offset = sizeof(BinderBase);
    binder_base_signals[SIGNAL_PROPERTY_CHANGED] =
        g_signal_new(SIGNAL_PROPERTY_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_UINT);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
