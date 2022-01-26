/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#include "test_watch.h"

#include <gutil_log.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#include <glib-object.h>

typedef GObjectClass TestOfonoWatchClass;

typedef struct test_ofono_watch {
    GObject object;
    OfonoWatch pub;
    char* path;
    char* iccid;
    char* imsi;
    char* spn;
    int queued_signals;
} TestOfonoWatch;

typedef struct test_ofono_watch_closure {
    GCClosure cclosure;
    ofono_watch_cb_t cb;
    void* user_data;
} TestOfonoWatchClosure;

#define SIGNAL_MODEM_CHANGED_NAME       "ofono-watch-modem-changed"
#define SIGNAL_ONLINE_CHANGED_NAME      "ofono-watch-online-changed"
#define SIGNAL_SIM_CHANGED_NAME         "ofono-watch-sim-changed"
#define SIGNAL_SIM_STATE_CHANGED_NAME   "ofono-watch-sim-state-changed"
#define SIGNAL_ICCID_CHANGED_NAME       "ofono-watch-iccid-changed"
#define SIGNAL_IMSI_CHANGED_NAME        "ofono-watch-imsi-changed"
#define SIGNAL_SPN_CHANGED_NAME         "ofono-watch-spn-changed"
#define SIGNAL_NETREG_CHANGED_NAME      "ofono-watch-netreg-changed"

static guint test_ofono_watch_signals[TEST_WATCH_SIGNAL_COUNT] = { 0 };
static GHashTable* test_ofono_watch_table = NULL;

G_DEFINE_TYPE(TestOfonoWatch, test_ofono_watch, G_TYPE_OBJECT)
#define PARENT_CLASS test_ofono_watch_parent_class
#define THIS_TYPE test_ofono_watch_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, TestOfonoWatch)

#define NEW_SIGNAL(klass,name) \
	test_ofono_watch_signals[TEST_WATCH_SIGNAL_##name##_CHANGED] = \
		g_signal_new(SIGNAL_##name##_CHANGED_NAME, \
			G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
			0, NULL, NULL, NULL, G_TYPE_NONE, 0)

static inline TestOfonoWatch* test_ofono_watch_cast(struct ofono_watch* watch)
    { return watch ? THIS(G_CAST(watch, TestOfonoWatch, pub)) : NULL; }

static inline int test_ofono_watch_signal_bit(TEST_WATCH_SIGNAL id)
    { return (1 << id); }

static
void
test_ofono_watch_signal_emit(
    TestOfonoWatch* self,
    TEST_WATCH_SIGNAL id)
{
    self->queued_signals &= ~test_ofono_watch_signal_bit(id);
    g_signal_emit(self, test_ofono_watch_signals[id], 0);
}

static
void
test_ofono_watch_destroyed(
    gpointer key,
    GObject* obj)
{
    GASSERT(test_ofono_watch_table);
    GDEBUG("TestOfonoWatch %s destroyed", (char*) key);
    if (test_ofono_watch_table) {
        GASSERT(g_hash_table_lookup(test_ofono_watch_table,key) == obj);
        g_hash_table_remove(test_ofono_watch_table, key);
        if (g_hash_table_size(test_ofono_watch_table) == 0) {
            g_hash_table_unref(test_ofono_watch_table);
            test_ofono_watch_table = NULL;
            GDEBUG("Last TestOfonoWatch is gone");
        }
    }
}

static
void
test_watch_signal_cb(
    TestOfonoWatch* source,
    TestOfonoWatchClosure* closure)
{
    closure->cb(&source->pub, closure->user_data);
}

static
unsigned long
test_watch_add_signal_handler(
    OfonoWatch* watch,
    TEST_WATCH_SIGNAL signal,
    ofono_watch_cb_t cb,
    void* user_data)
{
    if (watch && cb) {
        TestOfonoWatch* self = test_ofono_watch_cast(watch);
        TestOfonoWatchClosure* closure = (TestOfonoWatchClosure*)
            g_closure_new_simple(sizeof(TestOfonoWatchClosure), NULL);

        closure->cclosure.closure.data = closure;
        closure->cclosure.callback = G_CALLBACK(test_watch_signal_cb);
        closure->cb = cb;
        closure->user_data = user_data;

        return g_signal_connect_closure_by_id(self, test_ofono_watch_signals
            [signal], 0, &closure->cclosure.closure, FALSE);
    }
    return 0;
}

static
void
test_ofono_watch_init(
    TestOfonoWatch* self)
{
}

static
void
test_ofono_watch_finalize(
    GObject* object)
{
    TestOfonoWatch* self = THIS(object);

    g_free(self->path);
    g_free(self->iccid);
    g_free(self->imsi);
    g_free(self->spn);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
test_ofono_watch_class_init(
    TestOfonoWatchClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = test_ofono_watch_finalize;
    NEW_SIGNAL(klass, MODEM);
    NEW_SIGNAL(klass, ONLINE);
    NEW_SIGNAL(klass, SIM);
    NEW_SIGNAL(klass, SIM_STATE);
    NEW_SIGNAL(klass, ICCID);
    NEW_SIGNAL(klass, IMSI);
    NEW_SIGNAL(klass, SPN);
    NEW_SIGNAL(klass, NETREG);
}

/*==========================================================================*
 * Test API
 *==========================================================================*/

void
test_watch_signal_queue(
    OfonoWatch* watch,
    TEST_WATCH_SIGNAL id)
{
    TestOfonoWatch* self = test_ofono_watch_cast(watch);

    self->queued_signals |= test_ofono_watch_signal_bit(id);
}

void
test_watch_emit_queued_signals(
    OfonoWatch* watch)
{
    TestOfonoWatch* self = test_ofono_watch_cast(watch);
    int i;

    for (i = 0; self->queued_signals && i < TEST_WATCH_SIGNAL_COUNT; i++) {
        if (self->queued_signals & test_ofono_watch_signal_bit(i)) {
            test_ofono_watch_signal_emit(self, i);
        }
    }
}

void
test_watch_set_ofono_iccid(
    OfonoWatch* watch,
    const char* iccid)
{
    TestOfonoWatch* self = test_ofono_watch_cast(watch);

    if (g_strcmp0(self->iccid, iccid)) {
        g_free(self->iccid);
        watch->iccid = self->iccid = g_strdup(iccid);
        test_watch_signal_queue(watch, TEST_WATCH_SIGNAL_ICCID_CHANGED);
    }
}

void
test_watch_set_ofono_imsi(
    OfonoWatch* watch,
    const char* imsi)
{
    TestOfonoWatch* self = test_ofono_watch_cast(watch);

    if (g_strcmp0(self->imsi, imsi)) {
        g_free(self->imsi);
        watch->imsi = self->imsi = g_strdup(imsi);
        test_watch_signal_queue(watch, TEST_WATCH_SIGNAL_IMSI_CHANGED);
    }
}

void
test_watch_set_ofono_spn(
    OfonoWatch* watch,
    const char* spn)
{
    TestOfonoWatch* self = test_ofono_watch_cast(watch);

    if (g_strcmp0(self->spn, spn)) {
        g_free(self->spn);
        watch->spn = self->spn = g_strdup(spn);
        test_watch_signal_queue(watch, TEST_WATCH_SIGNAL_SPN_CHANGED);
    }
}

void
test_watch_set_ofono_sim(
    OfonoWatch* watch,
    struct ofono_sim* sim)
{
    if (watch->sim != sim) {
        watch->sim = sim;
        test_watch_signal_queue(watch, TEST_WATCH_SIGNAL_SIM_CHANGED);
        if (!sim) {
            test_watch_set_ofono_iccid(watch, NULL);
            test_watch_set_ofono_imsi(watch, NULL);
            test_watch_set_ofono_spn(watch, NULL);
        }
    }
}

void
test_watch_set_ofono_netreg(
    OfonoWatch* watch,
    struct ofono_netreg* netreg)
{
    if (watch->netreg != netreg) {
        watch->netreg = netreg;
        test_watch_signal_queue(watch, TEST_WATCH_SIGNAL_NETREG_CHANGED);
    }
}

/*==========================================================================*
 * Ofono API
 *==========================================================================*/

OfonoWatch*
ofono_watch_new(
    const char* path)
{
    if (path) {
        TestOfonoWatch* self = NULL;

        if (test_ofono_watch_table) {
            self = g_hash_table_lookup(test_ofono_watch_table, path);
        }
        if (self) {
            g_object_ref(self);
        } else {
            char* key = g_strdup(path);

            self = g_object_new(THIS_TYPE, NULL);
            self->pub.path = self->path = g_strdup(path);
            if (!test_ofono_watch_table) {
                /* Create the table on demand */
                test_ofono_watch_table = g_hash_table_new_full(g_str_hash,
                    g_str_equal, g_free, NULL);
            }
            g_hash_table_replace(test_ofono_watch_table, key, self);
            g_object_weak_ref(G_OBJECT(self), test_ofono_watch_destroyed, key);
            GDEBUG("TestOfonoWatch %s created", path);
        }
        return &self->pub;
    }
    return NULL;
}

OfonoWatch*
ofono_watch_ref(
    OfonoWatch* watch)
{
    if (watch) {
        g_object_ref(test_ofono_watch_cast(watch));
    }
    return watch;
}

void
ofono_watch_unref(
    OfonoWatch* watch)
{
    if (watch) {
        g_object_unref(test_ofono_watch_cast(watch));
    }
}

unsigned long
ofono_watch_add_modem_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_MODEM_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_online_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_ONLINE_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_sim_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_SIM_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_sim_state_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_SIM_STATE_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_iccid_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_ICCID_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_imsi_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_IMSI_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_spn_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_SPN_CHANGED, cb, user_data);
}

unsigned long
ofono_watch_add_netreg_changed_handler(
    OfonoWatch* watch,
    ofono_watch_cb_t cb,
    void* user_data)
{
    return test_watch_add_signal_handler(watch,
        TEST_WATCH_SIGNAL_NETREG_CHANGED, cb, user_data);
}

void
ofono_watch_remove_handler(
    OfonoWatch* watch,
    unsigned long id)
{
    if (watch && id) {
        g_signal_handler_disconnect(test_ofono_watch_cast(watch), id);
    }
}

void
ofono_watch_remove_handlers(
    OfonoWatch* watch,
    unsigned long* ids,
    unsigned int count)
{
    gutil_disconnect_handlers(test_ofono_watch_cast(watch), ids, count);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
