/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2021 Jolla Ltd.
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

#include "binder_ext_ims.h"
#include "binder_ext_plugin_impl.h"
#include "binder_ext_slot_impl.h"

#include <gutil_log.h>

/*==========================================================================*
 * Test slot
 *==========================================================================*/

typedef BinderExtSlotClass TestSlotClass;
typedef struct test_slot {
    BinderExtSlot parent;
    int* shutdown;
} TestSlot;

G_DEFINE_TYPE(TestSlot, test_slot, BINDER_EXT_TYPE_SLOT)

#define TEST_TYPE_SLOT test_slot_get_type()
#define TEST_SLOT(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, TEST_TYPE_SLOT, TestSlot)
#define TEST_IS_SLOT(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, TEST_TYPE_SLOT)

static
gpointer
test_slot_get_interface(
    BinderExtSlot* slot,
    GType iface)
{
    return BINDER_EXT_SLOT_CLASS(test_slot_parent_class)->
        get_interface(slot, iface);
}

static
void
test_slot_shutdown(
    BinderExtSlot* slot)
{
    TestSlot* self = TEST_SLOT(slot);

    if (self->shutdown) {
        (*self->shutdown)++;
    }
    BINDER_EXT_SLOT_CLASS(test_slot_parent_class)->shutdown(slot);
}

static
void
test_slot_init(
    TestSlot* self)
{
}

static
void
test_slot_class_init(
    TestSlotClass* klass)
{
    klass->get_interface = test_slot_get_interface;
    klass->shutdown = test_slot_shutdown;
}

/*==========================================================================*
 * Test plugin
 *==========================================================================*/

typedef BinderExtPluginClass TestPluginClass;
typedef struct test_plugin {
    BinderExtPlugin parent;
} TestPlugin;

G_DEFINE_TYPE(TestPlugin, test_plugin, BINDER_EXT_TYPE_PLUGIN)

#define TEST_TYPE_PLUGIN test_plugin_get_type()
#define TEST_PLUGIN(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, \
        TEST_TYPE_PLUGIN, TestPlugin)

static const char test_plugin_name[] = "test";

static
BinderExtSlot*
test_plugin_new_slot(
    BinderExtPlugin* plugin,
    RadioInstance* radio,
    GHashTable* params)
{
    return g_object_new(TEST_TYPE_SLOT, NULL);
}

static
void
test_plugin_init(
    TestPlugin* self)
{
}

static
void
test_plugin_class_init(
    TestPluginClass* klass)
{
    klass->plugin_name = test_plugin_name;
    klass->new_slot = test_plugin_new_slot;
}

/*==========================================================================*
 * null
 *==========================================================================*/

static
void
test_null(
    void)
{
    g_assert(!binder_ext_plugin_ref(NULL));
    g_assert(!binder_ext_plugin_name(NULL));
    g_assert(!binder_ext_plugin_get(NULL));
    g_assert(!binder_ext_slot_new(NULL, NULL, NULL));
    binder_ext_plugin_unref(NULL);
    binder_ext_plugin_register(NULL);
    binder_ext_plugin_unregister(NULL);
}

/*==========================================================================*
 * register
 *==========================================================================*/

static
void
test_register(
    void)
{
    BinderExtPlugin* plugin = g_object_new(TEST_TYPE_PLUGIN, NULL);
    BinderExtSlot* slot;

    binder_ext_plugin_register(plugin);
    binder_ext_plugin_register(plugin);
    g_assert(binder_ext_plugin_get(test_plugin_name) == plugin);
    g_assert(!binder_ext_plugin_get("foo"));
    g_assert(!binder_ext_plugin_get(NULL));

    slot = binder_ext_slot_new(plugin, NULL, NULL);
    g_assert(TEST_IS_SLOT(slot));
    binder_ext_slot_unref(slot);

    binder_ext_plugin_unregister("foo");
    binder_ext_plugin_unregister(test_plugin_name);
    g_assert(!binder_ext_plugin_get(test_plugin_name));
    binder_ext_plugin_unregister(test_plugin_name);
    binder_ext_plugin_unref(plugin);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/ext_plugin/"
#define TEST_(t) TEST_PREFIX t

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("null"), test_null);
    g_test_add_func(TEST_("register"), test_register);

    gutil_log_default.level = g_test_verbose() ?
        GLOG_LEVEL_VERBOSE : GLOG_LEVEL_NONE;
    gutil_log_timestamp = FALSE;

    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
