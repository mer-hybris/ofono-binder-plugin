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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "binder_devmon.h"
#include "binder_log.h"

#include <ofono/log.h>

#include <mce_battery.h>
#include <mce_charger.h>
#include <mce_display.h>

#include <radio_client.h>
#include <radio_request.h>

#include <gbinder_writer.h>

#include <gutil_macros.h>

enum binder_devmon_if_battery_event {
    BATTERY_EVENT_VALID,
    BATTERY_EVENT_STATUS,
    BATTERY_EVENT_COUNT
};

enum binder_devmon_if_charger_event {
    CHARGER_EVENT_VALID,
    CHARGER_EVENT_STATE,
    CHARGER_EVENT_COUNT
};

enum binder_devmon_if_display_event {
    DISPLAY_EVENT_VALID,
    DISPLAY_EVENT_STATE,
    DISPLAY_EVENT_COUNT
};

typedef struct binder_devmon_if {
    BinderDevmon pub;
    MceBattery* battery;
    MceCharger* charger;
    MceDisplay* display;
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
} DevMon;

typedef struct binder_devmon_if_io {
    BinderDevmonIo pub;
    struct ofono_slot* slot;
    MceBattery* battery;
    MceCharger* charger;
    MceDisplay* display;
    RadioClient* client;
    RadioRequest* req;
    gboolean display_on;
    gboolean ind_filter_supported;
    gulong battery_event_id[BATTERY_EVENT_COUNT];
    gulong charger_event_id[CHARGER_EVENT_COUNT];
    gulong display_event_id[DISPLAY_EVENT_COUNT];
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
} DevMonIo;

#define DBG_(self,fmt,args...) \
    DBG("%s: " fmt, radio_client_slot((self)->client), ##args)

inline static DevMon* binder_devmon_if_cast(BinderDevmon* pub)
    { return G_CAST(pub, DevMon, pub); }

inline static DevMonIo* binder_devmon_if_io_cast(BinderDevmonIo* pub)
    { return G_CAST(pub, DevMonIo, pub); }

static inline gboolean binder_devmon_if_battery_ok(MceBattery* battery)
    { return battery->valid && battery->status >= MCE_BATTERY_OK; }

static inline gboolean binder_devmon_if_charging(MceCharger* charger)
    { return charger->valid && charger->state == MCE_CHARGER_ON; }

static gboolean binder_devmon_if_display_on(MceDisplay* display)
    { return display->valid && display->state != MCE_DISPLAY_STATE_OFF; }

static
void
binder_devmon_if_io_indication_filter_sent(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    DevMonIo* self = user_data;

    GASSERT(self->req == req);
    radio_request_unref(self->req);
    self->req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SET_INDICATION_FILTER) {
            if (error == RADIO_ERROR_REQUEST_NOT_SUPPORTED) {
                /* This is a permanent failure */
                DBG_(self, "Indication response filter is not supported");
                self->ind_filter_supported = FALSE;
            }
        } else {
            ofono_error("Unexpected setIndicationFilter response %d", resp);
        }
    }
}

static
void
binder_devmon_if_io_set_indication_filter(
    DevMonIo* self)
{
    if (self->ind_filter_supported) {
        GBinderWriter args;
        RADIO_REQ code;
        gint32 value;

        /*
         * Both requests take the same args:
         *
         * setIndicationFilter(serial, bitfield<IndicationFilter>)
         * setIndicationFilter_1_2(serial, bitfield<IndicationFilter>)
         *
         * and both produce IRadioResponse.setIndicationFilterResponse()
         *
         * However setIndicationFilter_1_2 comments says "If unset, defaults
         * to @1.2::IndicationFilter:ALL" and it's unclear what "unset" means
         * wrt a bitmask. How is "unset" different from NONE which is zero.
         * To be on the safe side, let's always set the most innocently
         * looking bit which I think is DATA_CALL_DORMANCY.
         */
        if (radio_client_interface(self->client) < RADIO_INTERFACE_1_2) {
            code = RADIO_REQ_SET_INDICATION_FILTER;
            value = self->display_on ? RADIO_IND_FILTER_ALL :
                RADIO_IND_FILTER_DATA_CALL_DORMANCY;
        } else if (radio_client_interface(self->client) < RADIO_INTERFACE_1_5) {
            code = RADIO_REQ_SET_INDICATION_FILTER_1_2;
            value = self->display_on ? RADIO_IND_FILTER_ALL_1_2 :
                RADIO_IND_FILTER_DATA_CALL_DORMANCY;
        } else {
            code = RADIO_REQ_SET_INDICATION_FILTER_1_5;
            value = self->display_on ? RADIO_IND_FILTER_ALL_1_5 :
                RADIO_IND_FILTER_DATA_CALL_DORMANCY;
        }

        radio_request_drop(self->req);
        self->req = radio_request_new(self->client, code, &args,
            binder_devmon_if_io_indication_filter_sent, NULL, self);
        gbinder_writer_append_int32(&args, value);
        DBG_(self, "Setting indication filter: 0x%02x", value);
        radio_request_submit(self->req);
    }
}

static
void
binder_devmon_if_io_set_cell_info_update_interval(
    DevMonIo* self)
{
    ofono_slot_set_cell_info_update_interval(self->slot, self,
        (self->display_on && (binder_devmon_if_charging(self->charger) ||
            binder_devmon_if_battery_ok(self->battery))) ?
                self->cell_info_interval_short_ms :
                self->cell_info_interval_long_ms);
}

static
void
binder_devmon_if_io_battery_cb(
    MceBattery* battery,
    void* user_data)
{
    binder_devmon_if_io_set_cell_info_update_interval((DevMonIo*)user_data);
}

static
void binder_devmon_if_io_charger_cb(
    MceCharger* charger,
    void* user_data)
{
    binder_devmon_if_io_set_cell_info_update_interval((DevMonIo*)user_data);
}

static
void
binder_devmon_if_io_display_cb(
    MceDisplay* display,
    void* user_data)
{
    DevMonIo* self = user_data;
    const gboolean display_on = binder_devmon_if_display_on(display);

    if (self->display_on != display_on) {
        self->display_on = display_on;
        binder_devmon_if_io_set_indication_filter(self);
        binder_devmon_if_io_set_cell_info_update_interval(self);
    }
}

static
void
binder_devmon_if_io_free(
    BinderDevmonIo* io)
{
    DevMonIo* self = binder_devmon_if_io_cast(io);

    mce_battery_remove_all_handlers(self->battery, self->battery_event_id);
    mce_battery_unref(self->battery);

    mce_charger_remove_all_handlers(self->charger, self->charger_event_id);
    mce_charger_unref(self->charger);

    mce_display_remove_all_handlers(self->display, self->display_event_id);
    mce_display_unref(self->display);

    radio_request_drop(self->req);
    radio_client_unref(self->client);

    ofono_slot_drop_cell_info_requests(self->slot, self);
    ofono_slot_unref(self->slot);
    g_free(self);
}

static
BinderDevmonIo*
binder_devmon_if_start_io(
    BinderDevmon* devmon,
    RadioClient* client,
    struct ofono_slot* slot)
{
    DevMon* impl = binder_devmon_if_cast(devmon);
    DevMonIo* self = g_new0(DevMonIo, 1);

    self->pub.free = binder_devmon_if_io_free;
    self->ind_filter_supported = TRUE;
    self->client = radio_client_ref(client);
    self->slot = ofono_slot_ref(slot);

    self->battery = mce_battery_ref(impl->battery);
    self->battery_event_id[BATTERY_EVENT_VALID] =
        mce_battery_add_valid_changed_handler(self->battery,
            binder_devmon_if_io_battery_cb, self);
    self->battery_event_id[BATTERY_EVENT_STATUS] =
        mce_battery_add_status_changed_handler(self->battery,
            binder_devmon_if_io_battery_cb, self);

    self->charger = mce_charger_ref(impl->charger);
    self->charger_event_id[CHARGER_EVENT_VALID] =
        mce_charger_add_valid_changed_handler(self->charger,
            binder_devmon_if_io_charger_cb, self);
    self->charger_event_id[CHARGER_EVENT_STATE] =
        mce_charger_add_state_changed_handler(self->charger,
            binder_devmon_if_io_charger_cb, self);

    self->display = mce_display_ref(impl->display);
    self->display_on = binder_devmon_if_display_on(self->display);
    self->display_event_id[DISPLAY_EVENT_VALID] =
        mce_display_add_valid_changed_handler(self->display,
            binder_devmon_if_io_display_cb, self);
    self->display_event_id[DISPLAY_EVENT_STATE] =
        mce_display_add_state_changed_handler(self->display,
            binder_devmon_if_io_display_cb, self);

    self->cell_info_interval_short_ms = impl->cell_info_interval_short_ms;
    self->cell_info_interval_long_ms = impl->cell_info_interval_long_ms;

    binder_devmon_if_io_set_indication_filter(self);
    binder_devmon_if_io_set_cell_info_update_interval(self);
    return &self->pub;
}

static
void
binder_devmon_if_free(
    BinderDevmon* devmon)
{
    DevMon* self = binder_devmon_if_cast(devmon);

    mce_battery_unref(self->battery);
    mce_charger_unref(self->charger);
    mce_display_unref(self->display);
    g_free(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderDevmon*
binder_devmon_if_new(
    const BinderSlotConfig* config)
{
    DevMon* self = g_new0(DevMon, 1);

    self->pub.free = binder_devmon_if_free;
    self->pub.start_io = binder_devmon_if_start_io;
    self->battery = mce_battery_new();
    self->charger = mce_charger_new();
    self->display = mce_display_new();
    self->cell_info_interval_short_ms = config->cell_info_interval_short_ms;
    self->cell_info_interval_long_ms = config->cell_info_interval_long_ms;
    return &self->pub;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
