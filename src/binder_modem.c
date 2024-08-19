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
#include "binder_network.h"
#include "binder_radio.h"
#include "binder_sim_card.h"
#include "binder_sim_settings.h"
#include "binder_cell_info.h"
#include "binder_ext_slot.h"
#include "binder_ims_reg.h"
#include "binder_data.h"
#include "binder_util.h"
#include "binder_log.h"

#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-settings.h>
#include <ofono/call-volume.h>
#include <ofono/cbs.h>
#include <ofono/cell-info.h>
#include <ofono/devinfo.h>
#include <ofono/gprs.h>
#include <ofono/ims.h>
#include <ofono/message-waiting.h>
#include <ofono/modem.h>
#include <ofono/netmon.h>
#include <ofono/phonebook.h>
#include <ofono/sim-auth.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/stk.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>
#include <ofono/watch.h>

#include <radio_client.h>
#include <radio_instance.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gutil_macros.h>

#include <errno.h>

#define ONLINE_TIMEOUT_SECS     (15) /* 20 sec is hardcoded in ofono core */

typedef enum binder_modem_power_state {
    POWERED_OFF,
    POWERED_ON,
    POWERING_OFF
} BINDER_MODEM_POWER_STATE;

enum binder_modem_watch_event {
    WATCH_IMSI,
    WATCH_ICCID,
    WATCH_SIM_STATE,
    WATCH_EVENT_COUNT
};

typedef struct binder_modem_priv BinderModemPriv;

typedef struct binder_modem_online_request {
    const char* name;
    BinderModemPriv* self;
    ofono_modem_online_cb_t cb;
    void* data;
    guint timeout_id;
} BinderModemOnlineRequest;

struct binder_modem_priv {
    BinderModem pub;
    RadioRequestGroup* g;
    char* log_prefix;
    char* imeisv;
    char* imei;

    gulong watch_event_id[WATCH_EVENT_COUNT];
    char* last_known_iccid;
    char* reset_iccid;

    guint online_check_id;
    BINDER_MODEM_POWER_STATE power_state;
    gulong radio_state_event_id;

    BinderModemOnlineRequest set_online;
    BinderModemOnlineRequest set_offline;
};

#define RADIO_POWER_TAG(md) (md)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static BinderModemPriv* binder_modem_cast(BinderModem* modem)
    { return G_CAST(modem, BinderModemPriv, pub); }
static BinderModemPriv* binder_modem_get_priv(struct ofono_modem* ofono)
    { return binder_modem_cast(binder_modem_get_data(ofono)); }

static
void
binder_modem_online_request_done(
    BinderModemOnlineRequest* req)
{
    if (req->cb) {
        struct ofono_error error;
        ofono_modem_online_cb_t cb = req->cb;
        void* data = req->data;

        req->cb = NULL;
        req->data = NULL;
        DBG_(req->self, "%s", req->name);
        cb(binder_error_ok(&error), data);
    }
}

static
void
binder_modem_online_request_ok(
    BinderModemOnlineRequest* req)
{
    if (req->timeout_id) {
        g_source_remove(req->timeout_id);
        req->timeout_id = 0;
    }
    binder_modem_online_request_done(req);
}

static
void
binder_modem_update_online_state(
    BinderModemPriv* self)
{
    switch (self->pub.radio->state) {
    case RADIO_STATE_ON:
        DBG_(self, "online");
        binder_modem_online_request_ok(&self->set_online);
        break;

    case RADIO_STATE_OFF:
    case RADIO_STATE_UNAVAILABLE:
        DBG_(self, "offline");
        binder_modem_online_request_ok(&self->set_offline);
        break;
    }

    if (!self->set_offline.timeout_id &&
        !self->set_online.timeout_id &&
        self->power_state == POWERING_OFF) {
        self->power_state = POWERED_OFF;
        struct ofono_modem* ofono = self->pub.ofono;

        if (ofono) {
            ofono_modem_set_powered(ofono, FALSE);
        }
    }
}

static
gboolean
binder_modem_online_request_timeout(
    gpointer data)
{
    BinderModemOnlineRequest* req = data;

    GASSERT(req->timeout_id);
    req->timeout_id = 0;
    DBG_(req->self, "%s", req->name);
    binder_modem_online_request_done(req);
    binder_modem_update_online_state(req->self);
    return G_SOURCE_REMOVE;
}

static
gboolean
binder_modem_online_check(
    gpointer data)
{
    BinderModemPriv* self = data;

    GASSERT(self->online_check_id);
    self->online_check_id = 0;
    binder_modem_update_online_state(self);
    return G_SOURCE_REMOVE;
}

static
void
binder_modem_schedule_online_check(
    BinderModemPriv* self)
{
    if (!self->online_check_id) {
        self->online_check_id = g_idle_add(binder_modem_online_check, self);
    }
}

static
void
binder_modem_update_radio_settings(
    BinderModemPriv* self)
{
    BinderModem* modem = &self->pub;
    struct ofono_watch* watch = modem->watch;
    struct ofono_radio_settings* rs =
        ofono_modem_get_radio_settings(modem->ofono);

    if (watch->imsi) {
        /* radio-settings.c assumes that IMSI is available */
        if ((modem->config.features & BINDER_FEATURE_RADIO_SETTINGS) && !rs) {
            DBG_(self, "initializing radio settings interface");
            ofono_radio_settings_create(modem->ofono, 0, BINDER_DRIVER,
                modem->ofono);
        }
    } else if (rs) {
        DBG_(self, "removing radio settings interface");
        ofono_radio_settings_remove(rs);
    }
}

static
void
binder_modem_radio_state_cb(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    void* data)
{
     binder_modem_update_online_state((BinderModemPriv*)data);
}

static
void
binder_modem_imsi_cb(
    struct ofono_watch* watch,
    void* data)
{
    binder_modem_update_radio_settings((BinderModemPriv*)data);
}

static
void
binder_modem_iccid_cb(
    struct ofono_watch* watch,
    void* data)
{
    BinderModemPriv* self = data;

    if (watch->iccid) {
        g_free(self->last_known_iccid);
        self->last_known_iccid = g_strdup(watch->iccid);
        DBG_(self, "%s", self->last_known_iccid);
    }
}

static
void
binder_modem_sim_state_cb(
    struct ofono_watch* watch,
    void* data)
{
    BinderModemPriv* self = data;
    const enum ofono_sim_state state = ofono_sim_get_state(watch->sim);

    if (state == OFONO_SIM_STATE_RESETTING) {
        g_free(self->reset_iccid);
        self->reset_iccid = self->last_known_iccid;
        self->last_known_iccid = NULL;
        DBG_(self, "%s is resetting", self->reset_iccid);
    }
}

static
void
binder_modem_pre_sim(
    struct ofono_modem* ofono)
{
    BinderModem* modem = binder_modem_get_data(ofono);
    BinderModemPriv* self = binder_modem_cast(modem);
    const BINDER_FEATURE_MASK features = modem->config.features;

    DBG_(self, "");
    ofono_devinfo_create(ofono, 0, BINDER_DRIVER, ofono);
    ofono_sim_create(ofono, 0, BINDER_DRIVER, ofono);
    if (features & BINDER_FEATURE_VOICE) {
        ofono_voicecall_create(ofono, 0, BINDER_DRIVER, ofono);
    }
    if (!self->radio_state_event_id) {
        self->radio_state_event_id =
            binder_radio_add_property_handler(modem->radio,
                BINDER_RADIO_PROPERTY_STATE,
                binder_modem_radio_state_cb, self);
    }
}

static
void
binder_modem_post_sim(
    struct ofono_modem* ofono)
{
    BinderModem* modem = binder_modem_get_data(ofono);
    BinderModemPriv* self = binder_modem_cast(modem);
    const BINDER_FEATURE_MASK features = modem->config.features;
    struct ofono_gprs* gprs;

    DBG_(self, "");
    ofono_call_forwarding_create(ofono, 0, BINDER_DRIVER, ofono);
    ofono_call_barring_create(ofono, 0, BINDER_DRIVER, ofono);
    ofono_message_waiting_register(ofono_message_waiting_create(ofono));
    if (features & BINDER_FEATURE_SMS) {
        ofono_sms_create(ofono, 0, BINDER_DRIVER, ofono);
    }
    if (features & BINDER_FEATURE_DATA) {
        gprs = ofono_gprs_create(ofono, 0, BINDER_DRIVER, ofono);
        if (gprs) {
            guint i;
            static const enum ofono_gprs_context_type ap_types[] = {
                OFONO_GPRS_CONTEXT_TYPE_INTERNET,
                OFONO_GPRS_CONTEXT_TYPE_MMS,
                OFONO_GPRS_CONTEXT_TYPE_IMS
            };

            /* Create a context for each type */
            for (i = 0; i < G_N_ELEMENTS(ap_types); i++) {
                struct ofono_gprs_context* gc =
                    ofono_gprs_context_create(ofono, 0, BINDER_DRIVER, ofono);

                if (gc == NULL) {
                    break;
                } else {
                    ofono_gprs_context_set_type(gc, ap_types[i]);
                    ofono_gprs_add_context(gprs, gc);
                }
            }
        }
    }
    if (features & BINDER_FEATURE_PHONEBOOK) {
        ofono_phonebook_create(ofono, 0, "generic", ofono);
    }
    if (features & BINDER_FEATURE_STK) {
        struct ofono_watch* watch = modem->watch;

        if (!self->reset_iccid || g_strcmp0(self->reset_iccid, watch->iccid)) {
            /* This SIM was never reset */
            ofono_stk_create(ofono, 0, BINDER_DRIVER, ofono);
        } else {
            ofono_warn("Disabling STK after SIM reset");
        }
    }
    if (features & BINDER_FEATURE_CBS) {
        ofono_cbs_create(ofono, 0, BINDER_DRIVER, ofono);
    }
    if (features & BINDER_FEATURE_SIM_AUTH) {
        ofono_sim_auth_create(ofono);
    }
}

static
void
binder_modem_post_online(
    struct ofono_modem* ofono)
{
    BinderModem* modem = binder_modem_get_data(ofono);
    const BINDER_FEATURE_MASK features = modem->config.features;

    DBG_(binder_modem_cast(modem),"");
    ofono_call_volume_create(ofono, 0, BINDER_DRIVER, ofono);
    ofono_call_settings_create(ofono, 0, BINDER_DRIVER, ofono);
    if (features & BINDER_FEATURE_NETREG) {
        ofono_netreg_create(ofono, 0, BINDER_DRIVER, ofono);
    }
    if (features & BINDER_FEATURE_USSD) {
        ofono_ussd_create(ofono, 0, BINDER_DRIVER, ofono);
    }
    if (features & BINDER_FEATURE_IMS) {
        ofono_ims_create(ofono, BINDER_DRIVER, ofono);
    }
    ofono_netmon_create(ofono, 0, "cellinfo", ofono);
}

static
void
binder_modem_set_online(
    struct ofono_modem* ofono,
    ofono_bool_t online,
    ofono_modem_online_cb_t cb,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(ofono);
    BinderModemPriv* self = binder_modem_cast(modem);
    struct binder_radio* radio = modem->radio;
    BinderModemOnlineRequest* req;

    DBG_(self, "going %sline", online ? "on" : "off");

    binder_radio_set_online(radio, online);
    if (online) {
        binder_radio_power_on(radio, RADIO_POWER_TAG(self));
        req = &self->set_online;
    } else {
        binder_radio_power_off(radio, RADIO_POWER_TAG(self));
        req = &self->set_offline;
    }

    req->cb = cb;
    req->data = data;
    if (req->timeout_id) {
        g_source_remove(req->timeout_id);
    }
    req->timeout_id = g_timeout_add_seconds(ONLINE_TIMEOUT_SECS,
        binder_modem_online_request_timeout, req);
    binder_modem_schedule_online_check(self);
}

static
int
binder_modem_enable(
    struct ofono_modem* ofono)
{
    BinderModemPriv* self = binder_modem_get_priv(ofono);

    DBG_(self, "");
    self->power_state = POWERED_ON;
    return 0;
}

static
int
binder_modem_disable(
    struct ofono_modem* ofono)
{
    BinderModemPriv* self = binder_modem_get_priv(ofono);

    DBG_(self, "");
    if (self->set_online.timeout_id || self->set_offline.timeout_id) {
        self->power_state = POWERING_OFF;
        return -EINPROGRESS;
    } else {
        self->power_state = POWERED_OFF;
        return 0;
    }
}

static
int
binder_modem_probe(
    struct ofono_modem* ofono)
{
    DBG("%s", ofono_modem_get_path(ofono));
    return 0;
}

static
void
binder_modem_remove(
    struct ofono_modem* ofono)
{
    BinderModem* modem = binder_modem_get_data(ofono);
    BinderModemPriv* self = binder_modem_cast(modem);

    DBG_(self, "");
    ofono_modem_set_data(ofono, NULL);

    binder_radio_remove_handler(modem->radio, self->radio_state_event_id);
    binder_radio_set_online(modem->radio, FALSE);
    binder_radio_power_off(modem->radio, RADIO_POWER_TAG(self));
    binder_radio_set_online(modem->radio, FALSE);
    binder_radio_unref(modem->radio);
    binder_sim_settings_unref(modem->sim_settings);

    ofono_watch_remove_all_handlers(modem->watch, self->watch_event_id);
    ofono_watch_unref(modem->watch);

    if (self->online_check_id) {
        g_source_remove(self->online_check_id);
    }
    if (self->set_online.timeout_id) {
        g_source_remove(self->set_online.timeout_id);
    }
    if (self->set_offline.timeout_id) {
        g_source_remove(self->set_offline.timeout_id);
    }

    binder_ext_slot_unref(modem->ext);
    binder_ims_reg_unref(modem->ims);
    binder_network_unref(modem->network);
    binder_sim_card_unref(modem->sim_card);
    binder_data_unref(modem->data);
    ofono_cell_info_unref(modem->cell_info);

    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    radio_client_unref(modem->client);
    radio_client_unref(modem->data_client);
    radio_client_unref(modem->messaging_client);
    radio_client_unref(modem->network_client);
    radio_client_unref(modem->sim_client);
    radio_client_unref(modem->voice_client);
    radio_instance_unref(modem->instance);

    g_free(self->last_known_iccid);
    g_free(self->reset_iccid);
    g_free(self->log_prefix);
    g_free(self->imeisv);
    g_free(self->imei);
    g_free(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_modem_driver binder_modem_driver = {
    .name           = BINDER_DRIVER,
    .probe          = binder_modem_probe,
    .remove         = binder_modem_remove,
    .enable         = binder_modem_enable,
    .disable        = binder_modem_disable,
    .pre_sim        = binder_modem_pre_sim,
    .post_sim       = binder_modem_post_sim,
    .post_online    = binder_modem_post_online,
    .set_online     = binder_modem_set_online
};

void
binder_modem_init()
{
    ofono_modem_driver_register(&binder_modem_driver);
}

void
binder_modem_cleanup()
{
    ofono_modem_driver_unregister(&binder_modem_driver);
}

BinderModem*
binder_modem_create(
    RadioInstance* instance,
    RadioClient* client,
    RadioClient* data_client,
    RadioClient* messaging_client,
    RadioClient* network_client,
    RadioClient* sim_client,
    RadioClient* voice_client,
    const char* log_prefix,
    const char* path,
    const char* imei,
    const char* imeisv,
    const BinderSlotConfig* config,
    BinderExtSlot* ext,
    BinderRadio* radio,
    BinderNetwork* network,
    BinderSimCard* card,
    BinderData* data,
    BinderSimSettings* settings,
    struct ofono_cell_info* cell_info)
{
    /* Skip the slash from the path, it looks like "/ril_0" */
    struct ofono_modem* ofono = ofono_modem_create(path + 1, BINDER_DRIVER);

    if (ofono) {
        int err;
        BinderModemPriv* self = g_new0(BinderModemPriv, 1);
        BinderModem* modem = &self->pub;

        /*
         * binder_plugin.c must wait until IMEI becomes known before
         * creating the modem
         */
        GASSERT(imei);

        /* Copy config */
        modem->config = *config;
        modem->imei = self->imei = g_strdup(imei);
        modem->imeisv = self->imeisv = g_strdup(imeisv);
        modem->log_prefix = self->log_prefix = binder_dup_prefix(log_prefix);
        modem->ofono = ofono;
        modem->radio = binder_radio_ref(radio);
        modem->network = binder_network_ref(network);
        modem->sim_card = binder_sim_card_ref(card);
        modem->sim_settings = binder_sim_settings_ref(settings);
        modem->cell_info = ofono_cell_info_ref(cell_info);
        modem->data = binder_data_ref(data);
        modem->watch = ofono_watch_new(path);
        modem->instance = radio_instance_ref(instance);
        modem->client = radio_client_ref(client);
        modem->data_client = radio_client_ref(data_client);
        modem->messaging_client = radio_client_ref(messaging_client);
        modem->network_client = radio_client_ref(network_client);
        modem->sim_client = radio_client_ref(sim_client);
        modem->voice_client = radio_client_ref(voice_client);
        modem->ims = binder_ims_reg_new(client, ext, log_prefix);
        modem->ext = binder_ext_slot_ref(ext);
        self->g = radio_request_group_new(client);
        self->last_known_iccid = g_strdup(modem->watch->iccid);

        self->watch_event_id[WATCH_IMSI] =
            ofono_watch_add_imsi_changed_handler(modem->watch,
                binder_modem_imsi_cb, self);
        self->watch_event_id[WATCH_ICCID] =
            ofono_watch_add_iccid_changed_handler(modem->watch,
                binder_modem_iccid_cb, self);
        self->watch_event_id[WATCH_SIM_STATE] =
            ofono_watch_add_sim_state_changed_handler(modem->watch,
                binder_modem_sim_state_cb, self);

        self->set_online.name = "online";
        self->set_online.self = self;
        self->set_offline.name = "offline";
        self->set_offline.self = self;
        ofono_modem_set_data(ofono, self);
        err = ofono_modem_register(ofono);
        if (!err) {
            if (config->radio_power_cycle) {
                binder_radio_power_cycle(modem->radio);
            }

            /*
             * ofono_modem_reset() sets Powered to TRUE without
             * issuing PropertyChange signal.
             */
            ofono_modem_set_powered(modem->ofono, FALSE);
            ofono_modem_set_powered(modem->ofono, TRUE);
            self->power_state = POWERED_ON;

            /*
             * With some implementations, querying available band modes
             * causes some magic Android properties to appear. That's
             * the only reason for making this call.
             */
            if (config->query_available_band_mode) {
                 guint32 code =
                     radio_client_aidl_interface(
                         modem->network_client) == RADIO_NETWORK_INTERFACE ?
                             RADIO_NETWORK_REQ_GET_AVAILABLE_BAND_MODES :
                             RADIO_REQ_GET_AVAILABLE_BAND_MODES;
                /* oneway getAvailableBandModes(int32 serial); */
                RadioRequest* req = radio_request_new2(self->g,
                    code, NULL,
                    NULL, NULL, NULL);

                radio_request_submit(req);
                radio_request_unref(req);
            }

            binder_modem_update_radio_settings(self);
            return modem;
        } else {
            ofono_error("Error %d registering %s", err, BINDER_DRIVER);

            /*
             * If ofono_modem_register() failed, then
             * ofono_modem_remove() won't invoke
             * binder_modem_remove() callback.
             */
            binder_modem_remove(ofono);
        }

        ofono_modem_remove(ofono);
    }

    return NULL;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
