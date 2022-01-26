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

#include "test_watch.h"

#include "binder_sim_settings.h"
#include "binder_log.h"

#include <gutil_misc.h>
#include <gutil_log.h>

GLOG_MODULE_DEFINE("unit_sim_settings");

/*==========================================================================*
 * null
 *==========================================================================*/
static
void
test_null(
    void)
{
    g_assert(!binder_sim_settings_new(NULL, OFONO_RADIO_ACCESS_MODE_NONE));
    g_assert(!binder_sim_settings_add_property_handler(NULL, 0, NULL, NULL));
    g_assert(!binder_sim_settings_ref(NULL));
    binder_sim_settings_unref(NULL);
    binder_sim_settings_set_pref(NULL, OFONO_RADIO_ACCESS_MODE_NONE);
    binder_sim_settings_remove_handler(NULL, 0);
    binder_sim_settings_remove_handlers(NULL, NULL, 0);
}

/*==========================================================================*
 * basic
 *==========================================================================*/

typedef struct test_basic_data {
    int count[BINDER_SIM_SETTINGS_PROPERTY_COUNT];
} TestBasicData;

static
void
test_basic_total_cb(
    BinderSimSettings* data,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data)
{
    TestBasicData* test = user_data;

    test->count[BINDER_SIM_SETTINGS_PROPERTY_ANY]++;
    GDEBUG("%d %d", property, test->count[BINDER_SIM_SETTINGS_PROPERTY_ANY]);
}

static
void
test_basic_property_cb(
    BinderSimSettings* data,
    BINDER_SIM_SETTINGS_PROPERTY property,
    void* user_data)
{
    TestBasicData* test = user_data;

    g_assert_cmpint(property, < ,BINDER_SIM_SETTINGS_PROPERTY_COUNT);
    g_assert_cmpint(property, > ,BINDER_SIM_SETTINGS_PROPERTY_ANY);
    test->count[property]++;
    GDEBUG("%d %d", property, test->count[property]);
}

static
void
test_basic(
    void)
{
    TestBasicData test;
    const char* path = "/test_0";
    const char* iccid = "8888888888888888888";
    const char* imsi = "222222222222222";
    enum ofono_radio_access_mode pref = OFONO_RADIO_ACCESS_MODE_GSM;
    OfonoWatch* watch = ofono_watch_new(path);
    BinderSimSettings* settings = binder_sim_settings_new(path,
        OFONO_RADIO_ACCESS_MODE_ALL);
    ulong id[BINDER_SIM_SETTINGS_PROPERTY_COUNT];
    guint i;

    memset(&test, 0, sizeof(test));
    id[0] = binder_sim_settings_add_property_handler(settings,
        BINDER_SIM_SETTINGS_PROPERTY_ANY, test_basic_total_cb, &test);
    g_assert(id[0]);
    for (i = 1; i < G_N_ELEMENTS(id); i++) {
        id[i] = binder_sim_settings_add_property_handler(settings, i,
            test_basic_property_cb, &test);
        g_assert(id[i]);
    }

    /* NULL callback and zero id are tolerated */
    binder_sim_settings_remove_handler(settings, 0);
    g_assert(!binder_sim_settings_add_property_handler(settings,
        BINDER_SIM_SETTINGS_PROPERTY_ANY, NULL, NULL));

    /* Change some properties */
    test_watch_set_ofono_iccid(watch, iccid); /* Ignored */
    test_watch_emit_queued_signals(watch);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_ANY], == ,0);

    test_watch_set_ofono_imsi(watch, imsi); /* Handled */
    test_watch_emit_queued_signals(watch);
    g_assert_cmpstr(settings->imsi, == ,imsi);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_ANY], == ,1);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_IMSI], == ,1);

    /* No signal is emitted if IMSI hasn't actually changed */
    test_watch_signal_queue(watch, TEST_WATCH_SIGNAL_IMSI_CHANGED);
    test_watch_emit_queued_signals(watch);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_ANY], == ,1);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_IMSI], == ,1);

    /* Second time doesn't emit anything since the value doesn't change */
    binder_sim_settings_set_pref(settings, pref);
    binder_sim_settings_set_pref(settings, pref);
    g_assert_cmpint(settings->pref, == ,pref);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_ANY], == ,2);
    g_assert_cmpint(test.count[BINDER_SIM_SETTINGS_PROPERTY_PREF], == ,1);

    binder_sim_settings_remove_handler(settings, id[0]);
    id[0] = 0; /* binder_sim_settings_remove_handlers ignores zeros */
    binder_sim_settings_remove_all_handlers(settings, id);
    g_assert(binder_sim_settings_ref(settings) == settings);
    binder_sim_settings_unref(settings);
    binder_sim_settings_unref(settings);
    ofono_watch_unref(watch);
}

/*==========================================================================*
 * Common
 *==========================================================================*/

#define TEST_PREFIX "/sim_settings/"
#define TEST_(t) TEST_PREFIX t

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func(TEST_("null"), test_null);
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
