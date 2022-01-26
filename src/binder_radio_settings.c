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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "binder_modem.h"
#include "binder_radio_settings.h"
#include "binder_sim_settings.h"
#include "binder_util.h"
#include "binder_log.h"

#include <ofono/radio-settings.h>

typedef struct binder_radio_settings {
    struct ofono_radio_settings* rs;
    BinderSimSettings* settings;
    char* log_prefix;
    guint callback_id;
} BinderRadioSettings;

typedef struct binder_radio_settings_cbd {
    BinderRadioSettings* self;
    union _ofono_radio_settings_cb {
        ofono_radio_settings_rat_mode_set_cb_t rat_mode_set;
        ofono_radio_settings_rat_mode_query_cb_t rat_mode_query;
        ofono_radio_settings_available_rats_query_cb_t available_rats;
        BinderCallback ptr;
    } cb;
    gpointer data;
} BinderRadioSettingsCbData;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderRadioSettings*
binder_radio_settings_get_data(struct ofono_radio_settings* rs)
    { return ofono_radio_settings_get_data(rs); }

static
void
binder_radio_settings_callback_data_free(
    gpointer cbd)
{
    g_slice_free(BinderRadioSettingsCbData, cbd);
}

static
void
binder_radio_settings_later(
    BinderRadioSettings* self,
    GSourceFunc fn,
    BinderCallback cb,
    void* data)
{
    BinderRadioSettingsCbData* cbd = g_slice_new0(BinderRadioSettingsCbData);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;

    GASSERT(!self->callback_id);
    self->callback_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, fn, cbd,
        binder_radio_settings_callback_data_free);
}

static
gboolean
binder_radio_settings_set_rat_mode_cb(
    gpointer user_data)
{
    BinderRadioSettingsCbData* cbd = user_data;
    BinderRadioSettings* self = cbd->self;
    struct ofono_error error;

    GASSERT(self->callback_id);
    self->callback_id = 0;
    cbd->cb.rat_mode_set(binder_error_ok(&error), cbd->data);
    return G_SOURCE_REMOVE;
}

static
void
binder_radio_settings_set_rat_mode(
    struct ofono_radio_settings* rs,
    enum ofono_radio_access_mode mode,
    ofono_radio_settings_rat_mode_set_cb_t cb,
    void* data)
{
    BinderRadioSettings* self = binder_radio_settings_get_data(rs);

    DBG_(self, "%s", ofono_radio_access_mode_to_string(mode));
    binder_sim_settings_set_pref(self->settings,
        binder_access_modes_up_to(mode));
    binder_radio_settings_later(self,
        binder_radio_settings_set_rat_mode_cb, BINDER_CB(cb), data);
}

static
gboolean
binder_radio_settings_query_rat_mode_cb(
    gpointer user_data)
{
    BinderRadioSettingsCbData* cbd = user_data;
    BinderRadioSettings* self = cbd->self;
    const enum ofono_radio_access_mode mode =
        ofono_radio_access_max_mode(self->settings->pref);
    struct ofono_error error;

    DBG_(self, "rat mode %s", ofono_radio_access_mode_to_string(mode));
    GASSERT(self->callback_id);
    self->callback_id = 0;
    cbd->cb.rat_mode_query(binder_error_ok(&error), mode, cbd->data);
    return G_SOURCE_REMOVE;
}

static
void
binder_radio_settings_query_rat_mode(
    struct ofono_radio_settings* rs,
    ofono_radio_settings_rat_mode_query_cb_t cb,
    void* data)
{
    BinderRadioSettings* self = binder_radio_settings_get_data(rs);

    DBG_(self, "");
    binder_radio_settings_later(self,
        binder_radio_settings_query_rat_mode_cb, BINDER_CB(cb), data);
}

static
gboolean
binder_radio_settings_query_available_rats_cb(
    gpointer data)
{
    BinderRadioSettingsCbData* cbd = data;
    BinderRadioSettings* self = cbd->self;
    struct ofono_error error;

    GASSERT(self->callback_id);
    self->callback_id = 0;
    cbd->cb.available_rats(binder_error_ok(&error),
        self->settings->techs, cbd->data);
    return G_SOURCE_REMOVE;
}

static
void
binder_radio_settings_query_available_rats(
    struct ofono_radio_settings* rs,
    ofono_radio_settings_available_rats_query_cb_t cb,
    void* data)
{
    BinderRadioSettings* self = binder_radio_settings_get_data(rs);

    DBG_(self, "");
    binder_radio_settings_later(self,
        binder_radio_settings_query_available_rats_cb, BINDER_CB(cb), data);
}

static
gboolean
binder_radio_settings_register(
    gpointer user_data)
{
    BinderRadioSettings* self = user_data;

    GASSERT(self->callback_id);
    self->callback_id = 0;
    ofono_radio_settings_register(self->rs);
    return G_SOURCE_REMOVE;
}

static
int
binder_radio_settings_probe(
    struct ofono_radio_settings* rs,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderRadioSettings* self = g_new0(struct binder_radio_settings, 1);

    self->rs = rs;
    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->settings = binder_sim_settings_ref(modem->sim_settings);
    self->callback_id = g_idle_add(binder_radio_settings_register, self);

    DBG_(self, "");
    ofono_radio_settings_set_data(rs, self);
    return 0;
}

static
void
binder_radio_settings_remove(
    struct ofono_radio_settings* rs)
{
    BinderRadioSettings* self = binder_radio_settings_get_data(rs);

    DBG_(self, "");
    if (self->callback_id) {
        g_source_remove(self->callback_id);
    }
    binder_sim_settings_unref(self->settings);
    g_free(self->log_prefix);
    g_free(self);

    ofono_radio_settings_set_data(rs, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_radio_settings_driver binder_radio_settings_driver = {
    .name                 = BINDER_DRIVER,
    .probe                = binder_radio_settings_probe,
    .remove               = binder_radio_settings_remove,
    .query_rat_mode       = binder_radio_settings_query_rat_mode,
    .set_rat_mode         = binder_radio_settings_set_rat_mode,
    .query_available_rats = binder_radio_settings_query_available_rats
};

void
binder_radio_settings_init()
{
    ofono_radio_settings_driver_register(&binder_radio_settings_driver);
}

void
binder_radio_settings_cleanup()
{
    ofono_radio_settings_driver_unregister(&binder_radio_settings_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
