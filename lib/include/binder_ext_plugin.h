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

#ifndef BINDER_EXT_PLUGIN_H
#define BINDER_EXT_PLUGIN_H

#include "binder_ext_types.h"

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct binder_ext_plugin_priv BinderExtPluginPriv;

struct binder_ext_plugin {
    GObject object;
    BinderExtPluginPriv* priv;
};

GType binder_ext_plugin_get_type(void);
#define BINDER_EXT_TYPE_PLUGIN (binder_ext_plugin_get_type())
#define BINDER_EXT_PLUGIN(obj) (G_TYPE_CHECK_INSTANCE_CAST(obj, \
        BINDER_EXT_TYPE_PLUGIN, BinderExtPlugin))

BinderExtPlugin*
binder_ext_plugin_ref(
    BinderExtPlugin* plugin);

void
binder_ext_plugin_unref(
    BinderExtPlugin* plugin);

const char*
binder_ext_plugin_name(
    BinderExtPlugin* plugin);

BinderExtPlugin*
binder_ext_plugin_get(
    const char* name);

void
binder_ext_plugin_register(
    BinderExtPlugin* plugin);

void
binder_ext_plugin_unregister(
    const char* name);

G_END_DECLS

#endif /* BINDER_EXT_PLUGIN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
