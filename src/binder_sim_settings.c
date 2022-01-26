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
#include "binder_sim_settings.h"

#include <ofono/watch.h>

#include <gutil_misc.h>
#include <gutil_macros.h>

BINDER_BASE_ASSERT_COUNT(BINDER_SIM_SETTINGS_PROPERTY_COUNT);

enum ofono_watch_events {
    WATCH_EVENT_IMSI,
    WATCH_EVENT_COUNT
};

typedef struct binder_sim_settings_object {
    BinderBase base;
    BinderSimSettings pub;
    gulong watch_event_id[WATCH_EVENT_COUNT];
    struct ofono_watch* watch;
    char* imsi;
} BinderSimSettingsObject;

typedef BinderBaseClass BinderSimSettingsObjectClass;
GType binder_sim_settings_object_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderSimSettingsObject, binder_sim_settings_object,
    BINDER_TYPE_BASE)
#define PARENT_CLASS binder_sim_settings_object_parent_class
#define THIS_TYPE binder_sim_settings_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    BinderSimSettingsObject)

static inline
BinderSimSettingsObject*
binder_sim_settings_cast(
    BinderSimSettings* settings)
{
    return G_LIKELY(settings) ?
        THIS(G_CAST(settings, BinderSimSettingsObject, pub)) :
        NULL;
}

static
void
binder_sim_settings_imsi_changed(
    struct ofono_watch* watch,
    void* user_data)
{
    BinderSimSettingsObject* self = THIS(user_data);

    if (g_strcmp0(self->imsi, watch->imsi)) {
        g_object_ref(self);
        g_free(self->imsi);
        self->pub.imsi = self->imsi = g_strdup(watch->imsi);
        binder_base_emit_property_change(&self->base,
            BINDER_SIM_SETTINGS_PROPERTY_IMSI);
        g_object_unref(self);
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderSimSettings*
binder_sim_settings_new(
    const char* path,
    enum ofono_radio_access_mode techs)
{
    BinderSimSettings* settings = NULL;

    if (G_LIKELY(path)) {
        BinderSimSettingsObject* self = g_object_new(THIS_TYPE, NULL);

        settings = &self->pub;
        settings->techs = techs;
        settings->pref = techs;
        self->watch = ofono_watch_new(path);
        self->watch_event_id[WATCH_EVENT_IMSI] =
            ofono_watch_add_imsi_changed_handler(self->watch,
                binder_sim_settings_imsi_changed, self);
        settings->imsi = self->imsi = g_strdup(self->watch->imsi);
    }
    return settings;
}

BinderSimSettings*
binder_sim_settings_ref(
    BinderSimSettings* settings)
{
    BinderSimSettingsObject* self = binder_sim_settings_cast(settings);

    if (G_LIKELY(self)) {
        g_object_ref(self);
    }
    return settings;
}

void
binder_sim_settings_unref(
    BinderSimSettings* settings)
{
    BinderSimSettingsObject* self = binder_sim_settings_cast(settings);

    if (G_LIKELY(self)) {
        g_object_unref(self);
    }
}

void
binder_sim_settings_set_pref(
    BinderSimSettings* settings,
    enum ofono_radio_access_mode pref)
{
    BinderSimSettingsObject* self = binder_sim_settings_cast(settings);

    if (G_LIKELY(self) && settings->pref != pref) {
        settings->pref = pref;
        binder_base_emit_property_change(&self->base,
            BINDER_SIM_SETTINGS_PROPERTY_PREF);
    }
}

gulong
binder_sim_settings_add_property_handler(
    BinderSimSettings* settings,
    BINDER_SIM_SETTINGS_PROPERTY property,
    BinderSimSettingsPropertyFunc callback,
    void* user_data)
{
    BinderSimSettingsObject* self = binder_sim_settings_cast(settings);

    return G_LIKELY(self) ? binder_base_add_property_handler(&self->base,
        property, G_CALLBACK(callback), user_data) : 0;
}

void
binder_sim_settings_remove_handler(
    BinderSimSettings* settings,
    gulong id)
{
    BinderSimSettingsObject* self = binder_sim_settings_cast(settings);

    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_signal_handler_disconnect(self, id);
    }
}

void
binder_sim_settings_remove_handlers(
    BinderSimSettings* settings,
    gulong* ids,
    int count)
{
    gutil_disconnect_handlers(binder_sim_settings_cast(settings), ids, count);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_sim_settings_object_init(
    BinderSimSettingsObject* self)
{
}

static
void
binder_sim_settings_object_finalize(
    GObject* object)
{
    BinderSimSettingsObject* self = THIS(object);

    ofono_watch_remove_all_handlers(self->watch, self->watch_event_id);
    ofono_watch_unref(self->watch);
    g_free(self->imsi);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_sim_settings_object_class_init(
    BinderSimSettingsObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_sim_settings_object_finalize;
    BINDER_BASE_CLASS(klass)->public_offset =
        G_STRUCT_OFFSET(BinderSimSettingsObject, pub);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
