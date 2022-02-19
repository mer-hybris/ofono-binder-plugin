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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "binder_ext_plugin_impl.h"

#define THIS(obj) BINDER_EXT_PLUGIN(obj)
#define THIS_TYPE BINDER_EXT_TYPE_PLUGIN
#define PARENT_CLASS binder_ext_plugin_parent_class

G_DEFINE_ABSTRACT_TYPE(BinderExtPlugin, binder_ext_plugin, G_TYPE_OBJECT)

#define GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS(obj, THIS_TYPE, \
    BinderExtPluginClass)

static GHashTable* binder_ext_plugin_table = NULL;

/*==========================================================================*
 * API
 *==========================================================================*/

BinderExtSlot*
binder_ext_slot_new(
    BinderExtPlugin* plugin,
    RadioInstance* radio,
    GHashTable* params)
{
    /* Well, this is kind of BinderExtSlot API but let it be here. */
    return plugin ? GET_CLASS(plugin)->new_slot(plugin, radio, params) : NULL;
}

BinderExtPlugin*
binder_ext_plugin_ref(
    BinderExtPlugin* self)
{
    if (G_LIKELY(self)) {
        g_object_ref(THIS(self));
    }
    return self;
}

void
binder_ext_plugin_unref(
    BinderExtPlugin* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(THIS(self));
    }
}

const char*
binder_ext_plugin_name(
    BinderExtPlugin* self)
{
    return G_LIKELY(self) ? GET_CLASS(self)->plugin_name : NULL;
}

BinderExtPlugin*
binder_ext_plugin_get(
    const char* name)
{
    return (binder_ext_plugin_table && G_LIKELY(name)) ?
        g_hash_table_lookup(binder_ext_plugin_table, name) :
        NULL;
}

void
binder_ext_plugin_register(
    BinderExtPlugin* self)
{
    const char* name = binder_ext_plugin_name(self);

    if (name) {
        if (!binder_ext_plugin_table) {
            binder_ext_plugin_table = g_hash_table_new_full(g_str_hash,
                g_str_equal, NULL, g_object_unref);
        }
        g_hash_table_replace(binder_ext_plugin_table, (gpointer) name,
            binder_ext_plugin_ref(self));
    }
}

void
binder_ext_plugin_unregister(
    const char* name)
{
    if (binder_ext_plugin_table) {
        g_hash_table_remove(binder_ext_plugin_table, name);
        if (!g_hash_table_size(binder_ext_plugin_table)) {
            g_hash_table_destroy(binder_ext_plugin_table);
            binder_ext_plugin_table = NULL;
        }
    }
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_ext_plugin_init(
    BinderExtPlugin* self)
{
}

static
void
binder_ext_plugin_class_init(
    BinderExtPluginClass* klass)
{
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
