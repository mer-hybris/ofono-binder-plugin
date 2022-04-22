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

#include "binder_ext_ims_impl.h"

#include <gutil_log.h>

/*==========================================================================*
 * Dummy BinderExtImsInterface implementation
 *==========================================================================*/

typedef GObjectClass TestDummyImsClass;
typedef struct test_dummy_ims {
    GObject parent;
} TestDummyIms;

static void test_dummy_ims_iface_init(BinderExtImsInterface* iface);
G_DEFINE_TYPE_WITH_CODE(TestDummyIms, test_dummy_ims, G_TYPE_OBJECT,
G_IMPLEMENT_INTERFACE(BINDER_EXT_TYPE_IMS, test_dummy_ims_iface_init))

#define TEST_TYPE_DUMMY_IMS test_dummy_ims_get_type()

static
void
test_dummy_ims_iface_init(
    BinderExtImsInterface* iface)
{
    iface->version = BINDER_EXT_IMS_INTERFACE_VERSION;
    /* No callbacks at all */
}

static
void
test_dummy_ims_init(
    TestDummyIms* self)
{
}

static
void
test_dummy_ims_class_init(
    TestDummyImsClass* klass)
{
}

/*==========================================================================*
 * Test BinderExtImsInterface implementation
 *==========================================================================*/

typedef GObjectClass TestImsClass;
typedef struct test_ims {
    GObject parent;
    int cancelled;
} TestIms;

static void test_ims_iface_init(BinderExtImsInterface* iface);
G_DEFINE_TYPE_WITH_CODE(TestIms, test_ims, G_TYPE_OBJECT,
G_IMPLEMENT_INTERFACE(BINDER_EXT_TYPE_IMS, test_ims_iface_init))

#define TEST_TYPE_IMS test_ims_get_type()
#define TEST_IMS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, TEST_TYPE_IMS, TestIms)
#define TEST_IMS_FLAGS BINDER_EXT_IMS_INTERFACE_FLAG_SMS_SUPPORT
#define TEST_CALL_ID 42

enum test_ims_signal {
    TEST_SIGNAL_STATE_CHANGED,
    TEST_SIGNAL_COUNT
};

#define TEST_SIGNAL_STATE_CHANGED_NAME  "test-ims-state-changed"

static guint test_ims_signals[TEST_SIGNAL_COUNT] = { 0 };

static
BINDER_EXT_IMS_STATE
test_ims_get_state(
    BinderExtIms* ext)
{
    return BINDER_EXT_IMS_STATE_NOT_REGISTERED;
}

static
guint
test_ims_set_registration(
    BinderExtIms* ext,
    BINDER_EXT_IMS_REGISTRATION registration,
    BinderExtImsResultFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return TEST_CALL_ID;
}

static
void
test_ims_cancel(
    BinderExtIms* ext,
    guint id)
{
    g_assert_cmpuint(id, == ,TEST_CALL_ID);
    TEST_IMS(ext)->cancelled++;
}

static
gulong
test_ims_add_state_handler(
    BinderExtIms* ext,
    BinderExtImsFunc handler,
    void* user_data)
{
    return G_LIKELY(handler) ? g_signal_connect(TEST_IMS(ext),
        TEST_SIGNAL_STATE_CHANGED_NAME, G_CALLBACK(handler), user_data) : 0;
}

static
void
test_ims_iface_init(
    BinderExtImsInterface* iface)
{
    iface->flags |= TEST_IMS_FLAGS;
    iface->version = BINDER_EXT_IMS_INTERFACE_VERSION;
    iface->get_state = test_ims_get_state;
    iface->set_registration = test_ims_set_registration;
    iface->cancel = test_ims_cancel;
    iface->add_state_handler = test_ims_add_state_handler;
}

static
void
test_ims_init(
    TestIms* self)
{
}

static
void
test_ims_class_init(
    TestImsClass* klass)
{
    test_ims_signals[TEST_SIGNAL_STATE_CHANGED] =
        g_signal_new(TEST_SIGNAL_STATE_CHANGED_NAME,
            G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, 0,
            NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * basic
 *==========================================================================*/

static
void
test_not_reached(
    BinderExtIms* ext,
    void* user_data)
{
    g_assert_not_reached();
}

static
void
test_basic(
    void)
{
    TestIms* test = g_object_new(TEST_TYPE_IMS, NULL);
    BinderExtIms* ims = BINDER_EXT_IMS(test);
    gulong id;

    /* NULL resistance */
    binder_ext_ims_unref(NULL);
    binder_ext_ims_cancel(NULL, 0);
    binder_ext_ims_remove_handler(NULL, 0);
    binder_ext_ims_remove_handler(ims, 0);
    binder_ext_ims_remove_handlers(NULL, NULL, 0);
    binder_ext_ims_remove_handlers(ims, NULL, 1); /* NULL is checked first */
    binder_ext_ims_remove_handlers(ims, &id, 0);
    binder_ext_ims_cancel(ims, 0);
    g_assert(!binder_ext_ims_ref(NULL));
    g_assert_cmpint(binder_ext_ims_get_interface_flags(NULL), == ,
        BINDER_EXT_IMS_INTERFACE_NO_FLAGS);
    g_assert_cmpint(binder_ext_ims_get_state(NULL), == ,
        BINDER_EXT_IMS_STATE_UNKNOWN);
    g_assert_cmpint(binder_ext_ims_set_registration(NULL,
        BINDER_EXT_IMS_REGISTRATION_ON, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_ims_add_state_handler(NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_ims_add_state_handler(ims, NULL, NULL), == ,0);

    /* Basic operations */
    g_assert(binder_ext_ims_ref(ims) == ims);
    binder_ext_ims_unref(ims);
    g_assert_cmpint(binder_ext_ims_get_interface_flags(ims), == ,
        TEST_IMS_FLAGS);
    g_assert_cmpint(binder_ext_ims_get_state(ims), == ,
        BINDER_EXT_IMS_STATE_NOT_REGISTERED);
    g_assert_cmpuint(binder_ext_ims_set_registration(ims,
        BINDER_EXT_IMS_REGISTRATION_ON, NULL, NULL, NULL), == ,TEST_CALL_ID);
    id = binder_ext_ims_add_state_handler(ims, test_not_reached, NULL);
    g_assert(id);
    binder_ext_ims_remove_handler(ims, id);
    id = binder_ext_ims_add_state_handler(ims, test_not_reached, NULL);
    g_assert(id);
    binder_ext_ims_remove_handlers(ims, &id, 1);
    g_assert(!id);
    binder_ext_ims_remove_handlers(ims, &id, 1);
    binder_ext_ims_cancel(ims, TEST_CALL_ID);
    g_assert_cmpint(test->cancelled, == ,1);
    binder_ext_ims_unref(ims);
}

/*==========================================================================*
 * dummy
 *==========================================================================*/

static
void
test_dummy(
    void)
{
    TestDummyIms* test = g_object_new(TEST_TYPE_DUMMY_IMS, NULL);
    BinderExtIms* ims = BINDER_EXT_IMS(test);

    g_assert_cmpint(binder_ext_ims_get_interface_flags(ims), == ,
        BINDER_EXT_IMS_INTERFACE_NO_FLAGS);
    g_assert_cmpint(binder_ext_ims_get_state(ims), == ,
        BINDER_EXT_IMS_STATE_UNKNOWN);
    g_assert_cmpint(binder_ext_ims_set_registration(ims,
        BINDER_EXT_IMS_REGISTRATION_ON, NULL, NULL, NULL), == ,0);
    g_assert(!binder_ext_ims_add_state_handler(ims, test_not_reached, NULL));
    binder_ext_ims_cancel(ims, TEST_CALL_ID);
    binder_ext_ims_remove_handler(ims, 0);
    binder_ext_ims_unref(ims);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/ext_ims/"
#define TEST_(t) TEST_PREFIX t

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("basic"), test_basic);
    g_test_add_func(TEST_("dummy"), test_dummy);

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
