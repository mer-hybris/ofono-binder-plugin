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

#include "binder_base.h"
#include "binder_log.h"

#include <gutil_misc.h>
#include <gutil_log.h>

GLOG_MODULE_DEFINE("unit_base");

/*==========================================================================*
 * Test object
 *==========================================================================*/

typedef enum test_property {
    TEST_PROPERTY_ANY,
    TEST_PROPERTY_ONE,
    TEST_PROPERTY_TWO,
    TEST_PROPERTY_COUNT
} TEST_PROPERTY;

typedef BinderBaseClass TestObjectClass;
typedef struct test_object_data {
    void* ptr;
} TestObjectData;
typedef struct test_object {
    BinderBase base;
    TestObjectData pub;
} TestObject;

G_DEFINE_TYPE(TestObject, test_object, BINDER_TYPE_BASE)
#define PARENT_CLASS test_object_parent_class
#define TEST_TYPE test_object_get_type()
#define TEST(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, TEST_TYPE, TestObject)
BINDER_BASE_ASSERT_COUNT(TEST_PROPERTY_COUNT);

static
void
test_object_init(
    TestObject* self)
{
}

static
void
test_object_class_init(
    TestObjectClass* klass)
{
    BINDER_BASE_CLASS(klass)->public_offset = G_STRUCT_OFFSET(TestObject, pub);
}

/*==========================================================================*
 * basic
 *==========================================================================*/

typedef struct test_basic_data {
    int total;
    int count[TEST_PROPERTY_COUNT];
} TestBasicData;

static
void
test_basic_cb(
    TestObjectData* data,
    TEST_PROPERTY property,
    void* user_data)
{
    TestBasicData* test = user_data;

    g_assert(data->ptr == user_data);
    g_assert_cmpint(property, > ,TEST_PROPERTY_ANY);
    g_assert_cmpint(property, < ,TEST_PROPERTY_COUNT);
    test->count[property]++;
    test->total++;
    GDEBUG("%d %d", property, test->total);
}

static
void
test_basic(
    void)
{
    TestBasicData test;
    TestObject* obj = g_object_new(TEST_TYPE, NULL);
    BinderBase* base = &obj->base;
    ulong id;

    obj->pub.ptr = &test;
    memset(&test, 0, sizeof(test));
    id = binder_base_add_property_handler(base, TEST_PROPERTY_ANY,
        G_CALLBACK(test_basic_cb), &test);
    g_assert(id);

    /* NULL callback is tolerated */
    g_assert(!binder_base_add_property_handler(base, TEST_PROPERTY_ANY,
        NULL, NULL));

    /* Queue two events. Not signals is emitted yet */
    binder_base_queue_property_change(base, TEST_PROPERTY_ONE);
    binder_base_queue_property_change(base, TEST_PROPERTY_TWO);
    g_assert_cmpint(test.total, == ,0);

    /* Emit queued signals */
    binder_base_emit_queued_signals(base);
    g_assert_cmpint(test.total, == ,2);
    g_assert_cmpint(test.count[TEST_PROPERTY_ONE], == ,1);
    g_assert_cmpint(test.count[TEST_PROPERTY_TWO], == ,1);

    /* And emit one more */
    binder_base_emit_property_change(base, TEST_PROPERTY_ONE);
    g_assert_cmpint(test.total, == ,3);
    g_assert_cmpint(test.count[TEST_PROPERTY_ONE], == ,2);

    g_signal_handler_disconnect(base, id);
    g_object_unref(obj);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/base/"
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
