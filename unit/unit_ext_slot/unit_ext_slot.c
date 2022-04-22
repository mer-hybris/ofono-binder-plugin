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
 * basic
 *==========================================================================*/

static
void
test_basic(
    void)
{
    TestSlot* test = g_object_new(TEST_TYPE_SLOT, NULL);
    BinderExtSlot* slot = BINDER_EXT_SLOT(test);
    int shutdown_count = 0;

    /* NULL resistance */
    binder_ext_slot_drop(NULL);
    binder_ext_slot_unref(NULL);
    g_assert(!binder_ext_slot_ref(NULL));
    g_assert(!binder_ext_slot_get_interface(NULL, BINDER_EXT_TYPE_IMS));

    /* Basic operations */
    test->shutdown = &shutdown_count;
    g_assert(!binder_ext_slot_get_interface(slot, BINDER_EXT_TYPE_IMS));

    g_assert(binder_ext_slot_ref(slot) == slot);
    binder_ext_slot_drop(slot);
    g_assert_cmpint(shutdown_count, == ,1);
    g_assert(binder_ext_slot_ref(slot) == slot);
    binder_ext_slot_drop(slot);
    g_assert_cmpint(shutdown_count, == ,1); /* It's done only once */

    binder_ext_slot_unref(slot);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/ext_slot/"
#define TEST_(t) TEST_PREFIX t

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("basic"), test_basic);

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
