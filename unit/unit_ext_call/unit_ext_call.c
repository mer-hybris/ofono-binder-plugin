/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
 *  Copyright (C) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#include "binder_ext_call_impl.h"

#include <gutil_log.h>

/*==========================================================================*
 * Dummy BinderExtCallInterface implementation (no callbacks)
 *==========================================================================*/

typedef GObjectClass TestDummyCallClass;
typedef struct test_dummy_call {
    GObject parent;
} TestDummyCall;

static void test_dummy_call_iface_init(BinderExtCallInterface* iface);
G_DEFINE_TYPE_WITH_CODE(TestDummyCall, test_dummy_call, G_TYPE_OBJECT,
G_IMPLEMENT_INTERFACE(BINDER_EXT_TYPE_CALL, test_dummy_call_iface_init))

#define TEST_TYPE_DUMMY_CALL test_dummy_call_get_type()

static
void
test_dummy_call_iface_init(
    BinderExtCallInterface* iface)
{
    iface->version = BINDER_EXT_CALL_INTERFACE_VERSION;
}

static
void
test_dummy_call_init(
    TestDummyCall* self)
{
}

static
void
test_dummy_call_class_init(
    TestDummyCallClass* klass)
{
}

/*==========================================================================*
 * Test BinderExtCallInterface implementation
 *
 * Every add_*_handler connects to the same signal, which is enough to
 * see that the id comes back to the caller and that removing it by
 * that id disconnects the handler.
 *==========================================================================*/

typedef GObjectClass TestCallClass;
typedef struct test_call {
    GObject parent;
} TestCall;

static void test_call_iface_init(BinderExtCallInterface* iface);
G_DEFINE_TYPE_WITH_CODE(TestCall, test_call, G_TYPE_OBJECT,
G_IMPLEMENT_INTERFACE(BINDER_EXT_TYPE_CALL, test_call_iface_init))

#define TEST_TYPE_CALL test_call_get_type()
#define TEST_CALL(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, TEST_TYPE_CALL, TestCall)
#define TEST_SIGNAL_NAME "test-call-event"

static
gulong
test_call_connect(
    BinderExtCall* ext,
    GCallback handler,
    void* user_data)
{
    return G_LIKELY(handler) ? g_signal_connect(TEST_CALL(ext),
        TEST_SIGNAL_NAME, handler, user_data) : 0;
}

static
gulong
test_call_add_calls_changed_handler(
    BinderExtCall* ext,
    BinderExtCallFunc handler,
    void* user_data)
{
    return test_call_connect(ext, G_CALLBACK(handler), user_data);
}

static
gulong
test_call_add_disconnect_handler(
    BinderExtCall* ext,
    BinderExtCallDisconnectFunc handler,
    void* user_data)
{
    return test_call_connect(ext, G_CALLBACK(handler), user_data);
}

static
gulong
test_call_add_ring_handler(
    BinderExtCall* ext,
    BinderExtCallFunc handler,
    void* user_data)
{
    return test_call_connect(ext, G_CALLBACK(handler), user_data);
}

static
gulong
test_call_add_ssn_handler(
    BinderExtCall* ext,
    BinderExtCallSuppSvcNotifyFunc handler,
    void* user_data)
{
    return test_call_connect(ext, G_CALLBACK(handler), user_data);
}

static
gulong
test_call_add_ringback_tone_handler(
    BinderExtCall* ext,
    BinderExtCallRingbackToneFunc handler,
    void* user_data)
{
    return test_call_connect(ext, G_CALLBACK(handler), user_data);
}

static
void
test_call_iface_init(
    BinderExtCallInterface* iface)
{
    iface->version = BINDER_EXT_CALL_INTERFACE_VERSION;
    iface->flags = BINDER_EXT_CALL_INTERFACE_FLAG_IMS_SUPPORT;
    iface->add_calls_changed_handler = test_call_add_calls_changed_handler;
    iface->add_disconnect_handler = test_call_add_disconnect_handler;
    iface->add_ring_handler = test_call_add_ring_handler;
    iface->add_ssn_handler = test_call_add_ssn_handler;
    /* can use the base remove_handler implementation */
    iface->add_ringback_tone_handler = test_call_add_ringback_tone_handler;
}

static
void
test_call_init(
    TestCall* self)
{
}

static
void
test_call_class_init(
    TestCallClass* klass)
{
    g_signal_new(TEST_SIGNAL_NAME, G_OBJECT_CLASS_TYPE(klass),
        G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*==========================================================================*
 * null
 *==========================================================================*/

static
void
test_null(
    void)
{
    g_assert_cmpint(binder_ext_call_get_interface_flags(NULL), == ,0);
    g_assert_null(binder_ext_call_get_calls(NULL)[0]);
    g_assert_cmpint(binder_ext_call_dial(NULL,
        NULL, 0, 0, 0, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_deflect(NULL,
        NULL, 0, 0, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_answer(NULL,
        0, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_swap(NULL,
        0, 0, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_transfer(NULL,
        0, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_conference(NULL,
        0, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_send_dtmf(NULL,
        NULL, NULL, NULL, NULL), == ,0);
    g_assert_cmpint(binder_ext_call_hangup(NULL,
        0, 0, 0, NULL, NULL, NULL), == ,0);
    g_assert_null(binder_ext_call_ref(NULL));
    binder_ext_call_remove_handlers(NULL, NULL, 0);
    binder_ext_call_remove_handler(NULL, 0);
    binder_ext_call_cancel(NULL, 0);
    binder_ext_call_unref(NULL);
}

/*==========================================================================*
 * basic
 *==========================================================================*/

static
void
test_basic(
    void)
{
    TestCall* test = g_object_new(TEST_TYPE_CALL, NULL);
    BinderExtCall* ext = BINDER_EXT_CALL(test);

    g_assert_cmpint(binder_ext_call_get_interface_flags(ext), == ,
        BINDER_EXT_CALL_INTERFACE_FLAG_IMS_SUPPORT);

    g_assert(binder_ext_call_ref(ext) == ext);
    binder_ext_call_unref(ext);
    binder_ext_call_unref(ext);
}

/*==========================================================================*
 * handlers
 *==========================================================================*/

static
void
test_count(
    GObject* obj,
    int* count)
{
    (*count)++;
}

enum test_handler {
    TEST_CALLS_CHANGED,
    TEST_DISCONNECT,
    TEST_RING,
    TEST_SSN,
    TEST_RINGBACK_TONE,
    TEST_HANDLER_COUNT
};

static
void
test_add_all(
    BinderExtCall* ext,
    gulong* id,
    int* count)
{
    id[TEST_CALLS_CHANGED] = binder_ext_call_add_calls_changed_handler(ext,
        (BinderExtCallFunc) test_count, count);
    id[TEST_DISCONNECT] = binder_ext_call_add_disconnect_handler(ext,
        (BinderExtCallDisconnectFunc) test_count, count);
    id[TEST_RING] = binder_ext_call_add_ring_handler(ext,
        (BinderExtCallFunc) test_count, count);
    id[TEST_SSN] = binder_ext_call_add_ssn_handler(ext,
        (BinderExtCallSuppSvcNotifyFunc) test_count, count);
    id[TEST_RINGBACK_TONE] = binder_ext_call_add_ringback_tone_handler(ext,
        (BinderExtCallRingbackToneFunc) test_count, count);
}

static
void
test_handlers(
    void)
{
    TestCall* test = g_object_new(TEST_TYPE_CALL, NULL);
    BinderExtCall* ext = BINDER_EXT_CALL(test);
    gulong id[TEST_HANDLER_COUNT];
    int count = 0;
    guint i;

    /* Each wrapper hands back the id the implementation returned */
    test_add_all(ext, id, &count);
    for (i = 0; i < TEST_HANDLER_COUNT; i++) {
        g_assert(id[i]);
        g_assert(g_signal_handler_is_connected(test, id[i]));
    }
    g_signal_emit_by_name(test, TEST_SIGNAL_NAME);
    g_assert_cmpint(count, == ,TEST_HANDLER_COUNT);

    /* One by one */
    for (i = 0; i < TEST_HANDLER_COUNT; i++) {
        binder_ext_call_remove_handler(ext, id[i]);
        g_assert(!g_signal_handler_is_connected(test, id[i]));
    }
    count = 0;
    g_signal_emit_by_name(test, TEST_SIGNAL_NAME);
    g_assert_cmpint(count, == ,0);

    /* All at once, the way binder_voicecall_remove() does it */
    test_add_all(ext, id, &count);
    binder_ext_call_remove_all_handlers(ext, id);
    for (i = 0; i < TEST_HANDLER_COUNT; i++) {
        g_assert(!id[i]);
    }
    g_signal_emit_by_name(test, TEST_SIGNAL_NAME);
    g_assert_cmpint(count, == ,0);

    /* NULL handlers are not connected */
    g_assert(!binder_ext_call_add_calls_changed_handler(ext, NULL, NULL));
    g_assert(!binder_ext_call_add_disconnect_handler(ext, NULL, NULL));
    g_assert(!binder_ext_call_add_ring_handler(ext, NULL, NULL));
    g_assert(!binder_ext_call_add_ssn_handler(ext, NULL, NULL));
    g_assert(!binder_ext_call_add_ringback_tone_handler(ext, NULL, NULL));
    binder_ext_call_unref(ext);
}

/*==========================================================================*
 * dummy
 *==========================================================================*/

static
void
test_dummy(
    void)
{
    TestDummyCall* test = g_object_new(TEST_TYPE_DUMMY_CALL, NULL);
    BinderExtCall* ext = BINDER_EXT_CALL(test);
    gulong id[TEST_HANDLER_COUNT];
    int count = 0;
    guint i;

    /* No implementation, no ids, and removing nothing is fine */
    test_add_all(ext, id, &count);
    for (i = 0; i < TEST_HANDLER_COUNT; i++) {
        g_assert(!id[i]);
    }
    binder_ext_call_remove_all_handlers(ext, id);
    binder_ext_call_remove_handlers(ext, NULL, 0);
    binder_ext_call_remove_handler(ext, 0);

    /* NULL object */
    test_add_all(NULL, id, &count);
    for (i = 0; i < TEST_HANDLER_COUNT; i++) {
        g_assert(!id[i]);
    }
    binder_ext_call_unref(ext);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/ext_call/"
#define TEST_(t) TEST_PREFIX t

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("null"), test_null);
    g_test_add_func(TEST_("basic"), test_basic);
    g_test_add_func(TEST_("handlers"), test_handlers);
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
