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

#include "binder_call_barring.h"
#include "binder_call_forwarding.h"
#include "binder_call_settings.h"
#include "binder_call_volume.h"
#include "binder_cbs.h"
#include "binder_cell_info.h"
#include "binder_data.h"
#include "binder_devinfo.h"
#include "binder_devmon.h"
#include "binder_gprs.h"
#include "binder_gprs_context.h"
#include "binder_ims.h"
#include "binder_log.h"
#include "binder_logger.h"
#include "binder_modem.h"
#include "binder_netreg.h"
#include "binder_network.h"
#include "binder_radio.h"
#include "binder_radio_caps.h"
#include "binder_radio_settings.h"
#include "binder_sim.h"
#include "binder_sim_card.h"
#include "binder_sim_settings.h"
#include "binder_sms.h"
#include "binder_stk.h"
#include "binder_ussd.h"
#include "binder_util.h"
#include "binder_voicecall.h"

#include "binder_ext_plugin.h"
#include "binder_ext_slot.h"

#include <ofono/conf.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/storage.h>
#include <ofono/slot.h>
#include <ofono/watch.h>

#include <mce_log.h>

#include <radio_client.h>
#include <radio_config.h>
#include <radio_instance.h>

#include <gbinder_servicemanager.h>
#include <gbinder_reader.h>

#include <gutil_intarray.h>
#include <gutil_ints.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include <gio/gio.h>

#include <linux/capability.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

#define BINDER_SLOT_NUMBER_AUTOMATIC (0xffffffff)
#define BINDER_GET_DEVICE_IDENTITY_RETRIES_LAST 2

#define BINDER_CONF_FILE                "binder.conf"
#define BINDER_CONF_LIST_DELIMITER      ','
#define BINDER_SLOT_RADIO_INTERFACE_1_0 "1.0"
#define BINDER_SLOT_RADIO_INTERFACE_1_1 "1.1"
#define BINDER_SLOT_RADIO_INTERFACE_1_2 "1.2"
#define BINDER_SLOT_RADIO_INTERFACE_1_3 "1.3"
#define BINDER_SLOT_RADIO_INTERFACE_1_4 "1.4"
#define BINDER_SLOT_RADIO_INTERFACE_1_5 "1.5"

static const char* const binder_radio_ifaces[] = {
    RADIO_1_0, /* android.hardware.radio@1.0::IRadio */
    RADIO_1_1, /* android.hardware.radio@1.1::IRadio */
    RADIO_1_2, /* android.hardware.radio@1.2::IRadio */
    RADIO_1_3, /* android.hardware.radio@1.3::IRadio */
    RADIO_1_4, /* android.hardware.radio@1.4::IRadio */
    RADIO_1_5  /* android.hardware.radio@1.5::IRadio */
};

/*
 * The convention is that the keys which can only appear in the [Settings]
 * section start with the upper case, those which may appear in the [slotX]
 * i.e. slot specific section start with lower case.
 *
 * Slot specific settings may also appear in the [Settings] section in which
 * case they apply to all modems. The exceptions are "path" and "slot" values
 * which must be unique and therefore must appear in the section(s) for the
 * respective slot(s).
 */
#define BINDER_CONF_PLUGIN_DEVICE             "Device"
#define BINDER_CONF_PLUGIN_IDENTITY           "Identity"
#define BINDER_CONF_PLUGIN_3GLTE_HANDOVER     "3GLTEHandover"
#define BINDER_CONF_PLUGIN_MAX_NON_DATA_MODE  "MaxNonDataMode"
#define BINDER_CONF_PLUGIN_SET_RADIO_CAP      "SetRadioCapability"
#define BINDER_CONF_PLUGIN_EXPECT_SLOTS       "ExpectSlots"
#define BINDER_CONF_PLUGIN_IGNORE_SLOTS       "IgnoreSlots"

/* Slot specific */
#define BINDER_CONF_SLOT_PATH                 "path"
#define BINDER_CONF_SLOT_NUMBER               "slot"
#define BINDER_CONF_SLOT_EXT_PLUGIN           "extPlugin"
#define BINDER_CONF_SLOT_RADIO_INTERFACE      "radioInterface"
#define BINDER_CONF_SLOT_START_TIMEOUT_MS     "startTimeout"
#define BINDER_CONF_SLOT_REQUEST_TIMEOUT_MS   "timeout"
#define BINDER_CONF_SLOT_DISABLE_FEATURES     "disableFeatures"
#define BINDER_CONF_SLOT_EMPTY_PIN_QUERY      "emptyPinQuery"
#define BINDER_CONF_SLOT_DEVMON               "deviceStateTracking"
#define BINDER_CONF_SLOT_USE_DATA_PROFILES    "useDataProfiles"
#define BINDER_CONF_SLOT_DEFAULT_DATA_PROFILE_ID "defaultDataProfileId"
#define BINDER_CONF_SLOT_MMS_DATA_PROFILE_ID  "mmsDataProfileId"
#define BINDER_CONF_SLOT_ALLOW_DATA_REQ       "allowDataReq"
#define BINDER_CONF_SLOT_USE_NETWORK_SCAN     "useNetworkScan"
#define BINDER_CONF_SLOT_REPLACE_STRANGE_OPER "replaceStrangeOperatorNames"
#define BINDER_CONF_SLOT_SIGNAL_STRENGTH_RANGE "signalStrengthRange"
#define BINDER_CONF_SLOT_LTE_MODE             "lteNetworkMode"
#define BINDER_CONF_SLOT_UMTS_MODE            "umtsNetworkMode"
#define BINDER_CONF_SLOT_TECHNOLOGIES         "technologies"

/* Defaults */
#define BINDER_DEFAULT_RADIO_INTERFACE        RADIO_INTERFACE_1_2
#define BINDER_DEFAULT_PLUGIN_DEVICE          GBINDER_DEFAULT_HWBINDER
#define BINDER_DEFAULT_PLUGIN_IDENTITY        "radio:radio"
#define BINDER_DEFAULT_PLUGIN_DM_FLAGS        BINDER_DATA_MANAGER_3GLTE_HANDOVER
#define BINDER_DEFAULT_MAX_NON_DATA_MODE      OFONO_RADIO_ACCESS_MODE_UMTS
#define BINDER_DEFAULT_SLOT_PATH_PREFIX       "ril"
#define BINDER_DEFAULT_SLOT_TECHS             OFONO_RADIO_ACCESS_MODE_ALL
#define BINDER_DEFAULT_SLOT_LTE_MODE          RADIO_PREF_NET_LTE_GSM_WCDMA
#define BINDER_DEFAULT_SLOT_UMTS_MODE         RADIO_PREF_NET_GSM_WCDMA_AUTO
#define BINDER_DEFAULT_SLOT_NETWORK_MODE_TIMEOUT_MS (20*1000) /* ms */
#define BINDER_DEFAULT_SLOT_NETWORK_SELECTION_TIMEOUT_MS (100*1000) /* ms */
#define BINDER_DEFAULT_SLOT_DBM_WEAK          (-100) /* 0.0000000001 mW */
#define BINDER_DEFAULT_SLOT_DBM_STRONG        (-60)  /* 0.000001 mW */
#define BINDER_DEFAULT_SLOT_FEATURES          BINDER_FEATURE_ALL
#define BINDER_DEFAULT_SLOT_EMPTY_PIN_QUERY   TRUE
#define BINDER_DEFAULT_SLOT_RADIO_POWER_CYCLE FALSE
#define BINDER_DEFAULT_SLOT_CONFIRM_RADIO_POWER_ON FALSE
#define BINDER_DEFAULT_SLOT_QUERY_AVAILABLE_BAND_MODE TRUE
#define BINDER_DEFAULT_SLOT_REPLACE_STRANGE_OPER FALSE
#define BINDER_DEFAULT_SLOT_FORCE_GSM_WHEN_RADIO_OFF FALSE
#define BINDER_DEFAULT_SLOT_USE_DATA_PROFILES TRUE
#define BINDER_DEFAULT_SLOT_MMS_DATA_PROFILE_ID RADIO_DATA_PROFILE_DEFAULT
#define BINDER_DEFAULT_SLOT_DATA_PROFILE_ID   RADIO_DATA_PROFILE_DEFAULT
#define BINDER_DEFAULT_SLOT_FLAGS             OFONO_SLOT_NO_FLAGS
#define BINDER_DEFAULT_SLOT_CELL_INFO_INTERVAL_SHORT_MS (2000) /* 2 sec */
#define BINDER_DEFAULT_SLOT_CELL_INFO_INTERVAL_LONG_MS  (30000) /* 30 sec */
#define BINDER_DEFAULT_SLOT_REQ_TIMEOUT_MS    0 /* Use library default */
#define BINDER_DEFAULT_SLOT_START_TIMEOUT_MS  (30*1000) /* 30 sec */
#define BINDER_DEFAULT_SLOT_DEVMON            BINDER_DEVMON_ALL
#define BINDER_DEFAULT_SLOT_ALLOW_DATA        BINDER_ALLOW_DATA_ENABLED
#define BINDER_DEFAULT_SLOT_DATA_CALL_RETRY_LIMIT 4
#define BINDER_DEFAULT_SLOT_DATA_CALL_RETRY_DELAY_MS 200 /* ms */

/* The overall start timeout is the longest slot timeout plus this */
#define BINDER_SLOT_REGISTRATION_TIMEOUT_MS         (10*1000) /* 10 sec */

/* Modem error ids */
#define BINDER_ERROR_ID_DEATH                 "binder-death"
#define BINDER_ERROR_ID_CAPS_SWITCH_ABORTED   "binder-caps-switch-aborted"

enum binder_plugin_client_events {
    CLIENT_EVENT_CONNECTED,
    CLIENT_EVENT_DEATH,
    CLIENT_EVENT_RADIO_STATE_CHANGED,
    CLIENT_EVENT_COUNT
};

enum binder_plugin_watch_events {
    WATCH_EVENT_MODEM,
    WATCH_EVENT_COUNT
};

enum binder_slot_events {
    SLOT_EVENT_ENABLED,
    SLOT_EVENT_DATA_ROLE,
    SLOT_EVENT_COUNT
};

typedef enum binder_set_radio_cap_opt {
    BINDER_SET_RADIO_CAP_AUTO,
    BINDER_SET_RADIO_CAP_ENABLED,
    BINDER_SET_RADIO_CAP_DISABLED
} BINDER_SET_RADIO_CAP_OPT;

typedef enum binder_devmon_opt {
    BINDER_DEVMON_NONE = 0x01,
    BINDER_DEVMON_DS = 0x02,
    BINDER_DEVMON_IF = 0x04,
    BINDER_DEVMON_ALL = BINDER_DEVMON_DS | BINDER_DEVMON_IF
} BINDER_DEVMON_OPT;

typedef enum binder_plugin_flags {
    BINDER_PLUGIN_NO_FLAGS = 0x00,
    BINDER_PLUGIN_HAVE_CONFIG_SERVICE = 0x01,
    BINDER_PLUGIN_NEED_CONFIG_SERVICE = 0x02
} BINDER_PLUGIN_FLAGS;

typedef enum binder_plugin_slot_flags {
    BINDER_PLUGIN_SLOT_NO_FLAGS = 0x00,
    BINDER_PLUGIN_SLOT_HAVE_RADIO_SERVICE = 0x01
} BINDER_PLUGIN_SLOT_FLAGS;

typedef struct binder_plugin_identity {
    uid_t uid;
    gid_t gid;
} BinderPluginIdentity;

typedef struct binder_plugin_settings {
    BINDER_DATA_MANAGER_FLAGS dm_flags;
    BINDER_SET_RADIO_CAP_OPT set_radio_cap;
    BinderPluginIdentity identity;
    enum ofono_radio_access_mode non_data_mode;
} BinderPluginSettings;

typedef struct ofono_slot_driver_data {
    struct ofono_slot_manager* slot_manager;
    BINDER_PLUGIN_FLAGS flags;
    GBinderServiceManager* svcmgr;
    GDBusConnection* system_bus;
    RadioConfig* radio_config;
    BinderLogger* radio_config_trace;
    BinderLogger* radio_config_dump;
    BinderDataManager* data_manager;
    BinderRadioCapsManager* caps_manager;
    BinderPluginSettings settings;
    gulong caps_manager_event_id;
    gulong radio_config_watch_id;
    gulong list_call_id;
    guint start_timeout_id;
    char* dev;
    GSList* slots;
} BinderPlugin;

typedef struct binder_slot {
    BINDER_PLUGIN_SLOT_FLAGS flags;
    GBinderServiceManager* svcmgr;
    RADIO_INTERFACE version;
    RadioInstance* instance;
    RadioClient* client;
    GHashTable* ext_params;
    BinderExtPlugin* ext_plugin;
    BinderExtSlot* ext_slot;
    BinderPlugin* plugin;
    BinderLogger* log_trace;
    BinderLogger* log_dump;
    BinderData* data;
    BinderDevmon* devmon;
    BinderDevmonIo* devmon_io;
    BinderRadio* radio;
    BinderModem* modem;
    BinderNetwork* network;
    BinderRadioCaps* caps;
    BinderRadioCapsRequest* caps_req;
    BinderSimCard* sim_card;
    BinderSimSettings* sim_settings;
    BinderSlotConfig config;
    BinderDataOptions data_opt;
    struct ofono_slot* handle;
    struct ofono_cell_info* cell_info;
    struct ofono_watch* watch;
    enum ofono_slot_flags slot_flags;
    RadioRequest* imei_req;
    RadioRequest* caps_check_req;
    gulong radio_watch_id;
    gulong list_call_id;
    gulong connected_id;
    gulong client_event_id[CLIENT_EVENT_COUNT];
    gulong watch_event_id[WATCH_EVENT_COUNT];
    gulong slot_event_id[SLOT_EVENT_COUNT];
    gulong sim_card_state_event_id;
    gboolean received_sim_status;
    char* name;
    char* path;
    char* imei;
    char* imeisv;
    int req_timeout_ms; /* Request timeout, in milliseconds */
    guint start_timeout_ms;
    guint start_timeout_id;
} BinderSlot;

typedef struct binder_plugin_module {
    void (*init)(void);
    void (*cleanup)(void);
} BinderPluginModule;

static const BinderPluginModule binder_plugin_modules[] = {
    { binder_call_barring_init, binder_call_barring_cleanup },
    { binder_call_forwarding_init, binder_call_forwarding_cleanup },
    { binder_call_settings_init, binder_call_settings_cleanup },
    { binder_call_volume_init, binder_call_volume_cleanup },
    { binder_cbs_init, binder_cbs_cleanup },
    { binder_devinfo_init, binder_devinfo_cleanup },
    { binder_gprs_context_init, binder_gprs_context_cleanup },
    { binder_gprs_init, binder_gprs_cleanup },
    { binder_ims_init, binder_ims_cleanup },
    { binder_modem_init, binder_modem_cleanup },
    { binder_netreg_init, binder_netreg_cleanup },
    { binder_radio_settings_init, binder_radio_settings_cleanup },
    { binder_sim_init, binder_sim_cleanup },
    { binder_sms_init, binder_sms_cleanup },
    { binder_stk_init, binder_stk_cleanup },
    { binder_ussd_init, binder_ussd_cleanup },
    { binder_voicecall_init, binder_voicecall_cleanup }
};

GLOG_MODULE_DEFINE("binderplugin");

typedef
void
(*BinderPluginSlotFunc)(
    BinderSlot* slot);

typedef
void
(*BinderPluginSlotParamFunc)(
    BinderSlot* slot,
    void* param);

static
void
binder_plugin_slot_check(
    BinderSlot* slot);

static
void
binder_plugin_slot_check_radio_client(
    BinderSlot* slot);

static
void
binder_plugin_check_config_client(
    BinderPlugin* plugin);

static
void
binder_plugin_manager_started(
    BinderPlugin* plugin);

static
void
binder_logger_trace_notify(
    struct ofono_debug_desc* desc);

static
void
binder_logger_dump_notify(
    struct ofono_debug_desc* desc);

static struct ofono_debug_desc binder_logger_trace OFONO_DEBUG_ATTR = {
    .name = "binder_trace",
    .flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
    .notify = binder_logger_trace_notify
};

static struct ofono_debug_desc binder_logger_dump OFONO_DEBUG_ATTR = {
    .name = "binder_dump",
    .flags = OFONO_DEBUG_FLAG_DEFAULT | OFONO_DEBUG_FLAG_HIDE_NAME,
    .notify = binder_logger_dump_notify
};

static inline gboolean binder_plugin_multisim(BinderPlugin* plugin)
    { return plugin->slots && plugin->slots->next; }

static
void
binder_radio_config_trace_update(
    BinderPlugin* plugin)
{
    if (plugin) {
        if (binder_logger_trace.flags & OFONO_DEBUG_FLAG_PRINT) {
            if (!plugin->radio_config_trace) {
                plugin->radio_config_trace =
                    binder_logger_new_config_trace(plugin->radio_config);
            }
        } else if (plugin->radio_config_trace) {
            binder_logger_free(plugin->radio_config_trace);
            plugin->radio_config_trace = NULL;
        }
    }
}

static
void
binder_radio_config_dump_update(
    BinderPlugin* plugin)
{
    if (plugin) {
        if (binder_logger_dump.flags & OFONO_DEBUG_FLAG_PRINT) {
            if (!plugin->radio_config_dump) {
                plugin->radio_config_dump =
                    binder_logger_new_config_dump(plugin->radio_config);
            }
        } else if (plugin->radio_config_dump) {
            binder_logger_free(plugin->radio_config_dump);
            plugin->radio_config_dump = NULL;
        }
    }
}

static
void
binder_plugin_foreach_slot_param(
    BinderPlugin* plugin,
    BinderPluginSlotParamFunc fn,
    void* param)
{
    if (plugin) {
        GSList* l = plugin->slots;

        while (l) {
            GSList* next = l->next;

            fn((BinderSlot*)l->data, param);
            l = next;
        }
    }
}

static
void
binder_plugin_foreach_slot(
    BinderPlugin* plugin,
    BinderPluginSlotFunc fn)
{
    if (plugin) {
        GSList* l = plugin->slots;

        while (l) {
            GSList* next = l->next;

            fn((BinderSlot*)l->data);
            l = next;
        }
    }
}

static
void
binder_logger_dump_update_slot(
    BinderSlot* slot)
{
    if (binder_logger_dump.flags & OFONO_DEBUG_FLAG_PRINT) {
        if (!slot->log_dump) {
            slot->log_dump = binder_logger_new_radio_dump(slot->instance,
                slot->name);
        }
    } else if (slot->log_dump) {
        binder_logger_free(slot->log_dump);
        slot->log_dump = NULL;
    }
}

static
void
binder_logger_trace_update_slot(
    BinderSlot* slot)
{
    if (binder_logger_trace.flags & OFONO_DEBUG_FLAG_PRINT) {
        if (!slot->log_trace) {
            slot->log_trace = binder_logger_new_radio_trace(slot->instance,
                slot->name);
        }
    } else if (slot->log_trace) {
        binder_logger_free(slot->log_trace);
        slot->log_trace = NULL;
    }
}

static
void
binder_plugin_check_if_started(
    BinderPlugin* plugin)
{
    if (plugin->start_timeout_id) {
        GSList* l;

        for (l = plugin->slots; l; l = l->next) {
            BinderSlot* slot = l->data;

            if (!slot->handle) {
                break;
            }
        }

        if (!l) {
            DBG("Startup done!");
            g_source_remove(plugin->start_timeout_id);
            /* id is zeroed by binder_plugin_manager_start_done */
            GASSERT(!plugin->start_timeout_id);
            binder_plugin_manager_started(plugin);
        }
    }
}

static
void
binder_plugin_slot_shutdown(
    BinderSlot* slot,
    gboolean kill_io)
{
    if (slot->modem) {
        binder_data_allow(slot->data, OFONO_SLOT_DATA_NONE);
        ofono_modem_remove(slot->modem->ofono);

        /*
         * The above call is expected to result in
         * binder_plugin_slot_modem_changed getting
         * called which will set slot->modem to NULL.
         */
        GASSERT(!slot->modem);
    }

    if (kill_io) {
        if (slot->devmon_io) {
            binder_devmon_io_free(slot->devmon_io);
            slot->devmon_io = NULL;
        }

        if (slot->cell_info) {
            ofono_slot_set_cell_info(slot->handle, NULL);
            ofono_cell_info_unref(slot->cell_info);
            slot->cell_info = NULL;
        }

        if (slot->caps) {
            binder_network_set_radio_caps(slot->network, NULL);
            binder_radio_caps_request_free(slot->caps_req);
            binder_radio_caps_drop(slot->caps);
            slot->caps_req = NULL;
            slot->caps = NULL;
        }

        if (slot->data) {
            binder_data_allow(slot->data, OFONO_SLOT_DATA_NONE);
            binder_data_unref(slot->data);
            slot->data = NULL;
        }

        if (slot->radio) {
            binder_radio_unref(slot->radio);
            slot->radio = NULL;
        }

        if (slot->network) {
            binder_network_unref(slot->network);
            slot->network = NULL;
        }

        if (slot->sim_card) {
            binder_sim_card_remove_handler(slot->sim_card,
                slot->sim_card_state_event_id);
            binder_sim_card_unref(slot->sim_card);
            slot->sim_card_state_event_id = 0;
            slot->sim_card = NULL;
            slot->received_sim_status = FALSE;
        }

        if (slot->client) {
            binder_logger_free(slot->log_trace);
            binder_logger_free(slot->log_dump);
            slot->log_trace = NULL;
            slot->log_dump = NULL;

            radio_request_drop(slot->caps_check_req);
            radio_request_drop(slot->imei_req);
            slot->caps_check_req = NULL;
            slot->imei_req = NULL;

            radio_client_remove_all_handlers(slot->client,
                slot->client_event_id);

            radio_instance_unref(slot->instance);
            radio_client_unref(slot->client);
            slot->instance = NULL;
            slot->client = NULL;

            binder_ext_slot_drop(slot->ext_slot);
            slot->ext_slot = NULL;
        }
    }
}

/*
 * It seems to be necessary to kick (with RADIO_REQ_SET_RADIO_POWER)
 * the modems with power on after one of the modems has been powered
 * off. Which (depending on the device) may actually turn on the modems
 * which are supposed to be powered off but it's better than powering
 * off the modems which are supposed to be on.
 */
static
void
binder_plugin_power_check(
    BinderSlot* slot)
{
    binder_radio_confirm_power_on(slot->radio);
}

static
void
binder_plugin_radio_state_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    GBinderReader reader;
    RADIO_STATE radio_state = RADIO_STATE_UNAVAILABLE;

    /* radioStateChanged(RadioIndicationType, RadioState radioState); */
    gbinder_reader_copy(&reader, args);
    if (gbinder_reader_read_int32(&reader, (gint32*) &radio_state) &&
        radio_state == RADIO_STATE_OFF) {
        BinderSlot* slot = user_data;

        DBG("power off for slot %u", slot->config.slot);
        binder_plugin_foreach_slot(slot->plugin, binder_plugin_power_check);
    }
}

static
enum ofono_slot_sim_presence
binder_plugin_sim_presence(BinderSlot *slot)
{
    const BinderSimCardStatus* status = slot->sim_card->status;

    if (status) {
        switch (status->card_state) {
        case RADIO_CARD_STATE_ABSENT:
            return OFONO_SLOT_SIM_ABSENT;
        case RADIO_CARD_STATE_PRESENT:
            return OFONO_SLOT_SIM_PRESENT;
        case RADIO_CARD_STATE_ERROR:
        case RADIO_CARD_STATE_RESTRICTED:
            break;
        }
    }

    return OFONO_SLOT_SIM_UNKNOWN;
}

static
void
binder_plugin_slot_data_role_changed(
    struct ofono_slot* ofono_slot,
    enum ofono_slot_property property,
    void* user_data)
{
    BinderSlot* slot = user_data;
    const enum ofono_slot_data_role role = ofono_slot->data_role;

    binder_data_allow(slot->data, role);
    binder_radio_caps_request_free(slot->caps_req);
    if (role == OFONO_SLOT_DATA_NONE) {
        slot->caps_req = NULL;
    } else {
        /* Full speed connectivity isn't required for MMS. */
        slot->caps_req = binder_radio_caps_request_new(slot->caps,
            (role == OFONO_SLOT_DATA_MMS) ? OFONO_RADIO_ACCESS_MODE_GSM :
                slot->sim_settings->techs, role);
    }
}

static
void
binder_plugin_modem_check(
    BinderSlot* slot)
{
    if (!slot->modem && slot->handle && slot->handle->enabled &&
        radio_client_connected(slot->client)) {
        BinderModem* modem;

        DBG("%s registering modem", slot->name);
        modem = binder_modem_create(slot->client, slot->name, slot->path,
            slot->imei, slot->imeisv, &slot->config, slot->ext_slot,
            slot->radio, slot->network, slot->sim_card, slot->data,
            slot->sim_settings, slot->cell_info);

        if (modem) {
            slot->modem = modem;
        } else {
            binder_plugin_slot_shutdown(slot, TRUE);
        }
    }
}

static
void
binder_plugin_slot_enabled_changed(
    struct ofono_slot* ofono_slot,
    enum ofono_slot_property property,
    void* user_data)
{
    BinderSlot* slot = user_data;

    if (ofono_slot->enabled) {
        binder_plugin_modem_check(slot);
        radio_instance_set_enabled(slot->instance, TRUE);
    } else {
        radio_instance_set_enabled(slot->instance, FALSE);
        binder_plugin_slot_shutdown(slot, FALSE);
    }
}

static
void
binder_plugin_slot_startup_check(
    BinderSlot* slot)
{
    BinderPlugin* plugin = slot->plugin;

    if (!slot->handle && radio_client_connected(slot->client) &&
        !slot->imei_req && slot->imei) {
        struct ofono_slot* ofono_slot;

        if (slot->start_timeout_id) {
            /* We have made it before the slot timeout has expired */
            g_source_remove(slot->start_timeout_id);
            slot->start_timeout_id = 0;
        }

        /* Register this slot with the sailfish manager plugin */
        DBG("registering slot %s", slot->path);
        ofono_slot = slot->handle = ofono_slot_add(plugin->slot_manager,
            slot->path, slot->config.techs, slot->imei,
            slot->imeisv, binder_plugin_sim_presence(slot),
            slot->slot_flags);

        if (ofono_slot) {
            radio_instance_set_enabled(slot->instance, ofono_slot->enabled);
            ofono_slot_set_cell_info(ofono_slot, slot->cell_info);
            slot->slot_event_id[SLOT_EVENT_DATA_ROLE] =
                ofono_slot_add_property_handler(ofono_slot,
                    OFONO_SLOT_PROPERTY_DATA_ROLE,
                    binder_plugin_slot_data_role_changed, slot);
            slot->slot_event_id[SLOT_EVENT_ENABLED] =
                ofono_slot_add_property_handler(ofono_slot,
                    OFONO_SLOT_PROPERTY_ENABLED,
                    binder_plugin_slot_enabled_changed, slot);
        }
    }

    binder_plugin_modem_check(slot);
    binder_plugin_check_if_started(plugin);
}

static
void
binder_plugin_handle_error(
    BinderSlot* slot,
    const char* message)
{
    ofono_error("%s %s", slot->name, message);
    ofono_slot_error(slot->handle, BINDER_ERROR_ID_DEATH, message);
    binder_plugin_slot_shutdown(slot, TRUE);

    DBG("%s retrying", slot->name);
    binder_plugin_slot_check(slot);
}

static
void
binder_plugin_slot_death(
    RadioClient* client,
    void* user_data)
{
    binder_plugin_handle_error((BinderSlot*)user_data, "binder service died");
}

static
void
binder_plugin_caps_switch_aborted(
    BinderRadioCapsManager* mgr,
    void* user_data)
{
    BinderPlugin* plugin = user_data;

    DBG("radio caps switch aborted");
    ofono_slot_manager_error(plugin->slot_manager,
        BINDER_ERROR_ID_CAPS_SWITCH_ABORTED,
        "Capability switch transaction aborted");
}

static
void
binder_plugin_device_identity_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSlot *slot = user_data;

    GASSERT(slot->imei_req == req);
    radio_request_unref(slot->imei_req);
    slot->imei_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_GET_DEVICE_IDENTITY) {
            /*
             * getDeviceIdentityResponse(RadioResponseInfo, string imei,
             *   string imeisv, string esn, string meid)
             */
            if (error == RADIO_ERROR_NONE) {
                GBinderReader reader;
                const char* imei;
                const char* imeisv;

                gbinder_reader_copy(&reader, args);
                imei = gbinder_reader_read_hidl_string_c(&reader);
                imeisv = gbinder_reader_read_hidl_string_c(&reader);

                /*
                 * slot->imei should be either NULL (when we get connected
                 * to the radio service the very first time) or match the
                 * already known IMEI (if radio service has crashed and we
                 * have reconnected)
                 */
                DBG("%s %s %s", slot->name, imei, imeisv);
                if (slot->imei && imei && strcmp(slot->imei, imei)) {
                    ofono_warn("IMEI has changed \"%s\" -> \"%s\"",
                        slot->imei, imei);
                }

                /* We assume that IMEI never changes */
                if (!slot->imei) {
                    slot->imei = imei ? g_strdup(imei) :
                        g_strdup_printf("%d", slot->config.slot);
                }

                if (!slot->imeisv) {
                    slot->imeisv = g_strdup(imeisv ? imeisv : "");
                }
            } else {
                ofono_warn("getDeviceIdentity error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getDeviceIdentity response %d", resp);
        }
    } else {
        ofono_error("getDeviceIdentity error %d", status);
    }

    binder_plugin_slot_startup_check(slot);
}

static
void
binder_plugin_slot_get_device_identity(
    BinderSlot* slot,
    gboolean blocking,
    int retries)
{
    /* getDeviceIdentity(int32 serial) */
    RadioRequest* req = radio_request_new(slot->client,
        RADIO_REQ_GET_DEVICE_IDENTITY, NULL,
        binder_plugin_device_identity_cb, NULL, slot);

    radio_request_set_blocking(req, TRUE);
    radio_request_set_retry(req, BINDER_RETRY_MS, retries);
    radio_request_drop(slot->imei_req);
    if (radio_request_submit(req)) {
        DBG("%s submitted getDeviceIdentity", slot->name);
        slot->imei_req = req; /* Keep the ref */
    } else {
        ofono_error("Failed to submit getDeviceIdentity for %s", slot->name);
        slot->imei_req = NULL;
        radio_request_unref(req);
    }
}

static
void
binder_plugin_slot_sim_state_changed(
    BinderSimCard* card,
    void* data)
{
    BinderSlot* slot = data;
    enum ofono_slot_sim_presence presence = binder_plugin_sim_presence(slot);

    if (card->status) {
        switch (presence) {
        case OFONO_SLOT_SIM_PRESENT:
            DBG("SIM found in slot %u", slot->config.slot);
            break;
        case OFONO_SLOT_SIM_ABSENT:
            DBG("No SIM in slot %u", slot->config.slot);
            break;
        default:
            break;
        }

        if (!slot->received_sim_status && slot->imei_req) {
            /*
             * We have received the SIM status but haven't yet got IMEI
             * from the modem. Some modems behave this way if they don't
             * have their IMEI initialized yet. Cancel the current request
             * (which probably had unlimited number of retries) and give a
             * few more tries (this time, limited number).
             *
             * Some adaptations fail RADIO_REQ_GET_DEVICE_IDENTITY until
             * the modem has been properly initialized.
             */
            DBG("giving slot %u last chance", slot->config.slot);
            binder_plugin_slot_get_device_identity(slot, FALSE,
                BINDER_GET_DEVICE_IDENTITY_RETRIES_LAST);
        }
        slot->received_sim_status = TRUE;
    }

    ofono_slot_set_sim_presence(slot->handle, presence);
}

static
void
binder_plugin_slot_radio_caps_cb(
    const RadioCapability* cap,
    void *user_data)
{
    BinderSlot* slot = user_data;

    DBG("radio caps %s", cap ? "ok" : "NOT supported");
    GASSERT(slot->caps_check_req);
    radio_request_drop(slot->caps_check_req);
    slot->caps_check_req = NULL;

    if (cap) {
        BinderPlugin* plugin = slot->plugin;

        if (!plugin->caps_manager) {
            plugin->caps_manager =
                binder_radio_caps_manager_new(plugin->data_manager);
            plugin->caps_manager_event_id =
                binder_radio_caps_manager_add_tx_aborted_handler
                    (plugin->caps_manager, binder_plugin_caps_switch_aborted,
                        plugin);
        }

        GASSERT(!slot->caps);
        slot->caps = binder_radio_caps_new(plugin->caps_manager, slot->name,
            slot->client, slot->watch, slot->data, slot->radio, slot->sim_card,
            slot->sim_settings, &slot->config, cap);
        binder_network_set_radio_caps(slot->network, slot->caps);
    }
}

static
void
binder_plugin_slot_connected(
    BinderSlot* slot)
{
    BinderPlugin* plugin = slot->plugin;
    const BinderPluginSettings* ps = &plugin->settings;

    GASSERT(radio_client_connected(slot->client));
    GASSERT(!slot->client_event_id[CLIENT_EVENT_CONNECTED]);
    DBG("%s", slot->name);

    /*
     * Ofono modem will be registered after getDeviceIdentity() call
     * successfully completes. By the time ofono starts, modem may
     * not be completely functional. Waiting until it responds to
     * getDeviceIdentity() and retrying the request on failure
     * (hopefully) gives modem and/or adaptation enough time to
     * finish whatever is happening during initialization.
     */
    binder_plugin_slot_get_device_identity(slot, TRUE, -1);

    GASSERT(!slot->radio);
    slot->radio = binder_radio_new(slot->client, slot->name);

    /* Register RADIO_IND_RADIO_STATE_CHANGED handler only if we need one */
    GASSERT(!slot->client_event_id[CLIENT_EVENT_RADIO_STATE_CHANGED]);
    if (slot->config.confirm_radio_power_on) {
        slot->client_event_id[CLIENT_EVENT_RADIO_STATE_CHANGED] =
            radio_client_add_indication_handler(slot->client,
                RADIO_IND_RADIO_STATE_CHANGED,
                binder_plugin_radio_state_changed, slot);
    }

    GASSERT(!slot->sim_card);
    slot->sim_card = binder_sim_card_new(slot->client, slot->config.slot);
    slot->sim_card_state_event_id =
        binder_sim_card_add_state_changed_handler(slot->sim_card,
            binder_plugin_slot_sim_state_changed, slot);

    /*
     * BinderSimCard is expected to perform getIccCardStatus()
     * asynchronously and report back when request has completed:
     */
    GASSERT(!slot->sim_card->status);
    GASSERT(!slot->received_sim_status);

    GASSERT(!slot->network);
    slot->network = binder_network_new(slot->path, slot->client,
        slot->name, slot->radio, slot->sim_card, slot->sim_settings,
        &slot->config);

    GASSERT(!slot->data);
    slot->data = binder_data_new(plugin->data_manager, slot->client,
        slot->name, slot->radio, slot->network, &slot->data_opt,
        &slot->config);

    GASSERT(!slot->cell_info);
    slot->cell_info = binder_cell_info_new(slot->client,
        slot->name, slot->radio, slot->sim_card);

    GASSERT(!slot->caps);
    GASSERT(!slot->caps_check_req);
    if (binder_plugin_multisim(plugin) &&
        (ps->set_radio_cap == BINDER_SET_RADIO_CAP_ENABLED ||
         ps->set_radio_cap == BINDER_SET_RADIO_CAP_AUTO)) {
        /* Check if the device really supports radio capability management */
        slot->caps_check_req = binder_radio_caps_check(slot->client,
            binder_plugin_slot_radio_caps_cb, slot);
    }

    GASSERT(!slot->devmon_io);
    if (slot->devmon) {
        slot->devmon_io = binder_devmon_start_io(slot->devmon,
            slot->client, slot->handle);
    }

    binder_plugin_slot_startup_check(slot);
}

static
gboolean
binder_plugin_service_list_proc(
    GBinderServiceManager* sm,
    char** services,
    void* data)
{
    BinderPlugin* plugin = data;

    plugin->list_call_id = 0;

    /* IRadioConfig 1.0 is of no use to us */
    if (gutil_strv_contains(services, RADIO_CONFIG_1_2_FQNAME) ||
            gutil_strv_contains(services, RADIO_CONFIG_1_1_FQNAME)) {
        /* If it's there then we definitely need it */
        plugin->flags |= (BINDER_PLUGIN_HAVE_CONFIG_SERVICE |
                          BINDER_PLUGIN_NEED_CONFIG_SERVICE);
    } else {
        plugin->flags &= ~BINDER_PLUGIN_HAVE_CONFIG_SERVICE;
        if (gutil_strv_contains(services, RADIO_CONFIG_1_0_FQNAME)) {
            /*
             * If 1.0 has appeared but 1.1 has not, assume that 1.1
             * is not supported.
             */
            plugin->flags &= ~BINDER_PLUGIN_NEED_CONFIG_SERVICE;
        }
    }

    binder_plugin_check_config_client(plugin);

    /* Return FALSE to free the service list */
    return FALSE;
}

static
gboolean
binder_plugin_slot_service_list_proc(
    GBinderServiceManager* sm,
    char** services,
    void* data)
{
    BinderSlot* slot = data;
    char* fqname = g_strconcat(binder_radio_ifaces[slot->version], "/",
        slot->name, NULL);

    slot->list_call_id = 0;
    if (gutil_strv_contains(services, fqname)) {
        DBG("found %s", fqname);
        slot->flags |= BINDER_PLUGIN_SLOT_HAVE_RADIO_SERVICE;
    } else {
        DBG("not found %s", fqname);
        slot->flags &= ~BINDER_PLUGIN_SLOT_HAVE_RADIO_SERVICE;
    }

    binder_plugin_slot_check_radio_client(slot);
    g_free(fqname);

    /* Return FALSE to free the service list */
    return FALSE;
}

static
void
binder_plugin_service_check(
    BinderPlugin* plugin)
{
    gbinder_servicemanager_cancel(plugin->svcmgr, plugin->list_call_id);
    plugin->list_call_id = gbinder_servicemanager_list(plugin->svcmgr,
        binder_plugin_service_list_proc, plugin);
}

static
void
binder_plugin_slot_check(
    BinderSlot* slot)
{
    gbinder_servicemanager_cancel(slot->svcmgr, slot->list_call_id);
    slot->list_call_id = gbinder_servicemanager_list(slot->svcmgr,
        binder_plugin_slot_service_list_proc, slot);
}

static
void
binder_plugin_service_registration_proc(
    GBinderServiceManager* sm,
    const char* name,
    void* plugin)
{
    DBG("%s is there", name);
    binder_plugin_service_check((BinderPlugin*) plugin);
}

static
void
binder_plugin_slot_service_registration_proc(
    GBinderServiceManager* sm,
    const char* name,
    void* slot)
{
    DBG("%s is there", name);
    binder_plugin_slot_check((BinderSlot*) slot);
}

static
void
binder_plugin_drop_radio_config(
    BinderPlugin* plugin)
{
    binder_data_manager_set_radio_config(plugin->data_manager, NULL);
    binder_logger_free(plugin->radio_config_trace);
    binder_logger_free(plugin->radio_config_dump);
    radio_config_unref(plugin->radio_config);
    plugin->radio_config_trace = NULL;
    plugin->radio_config_dump = NULL;
    plugin->radio_config = NULL;
}

static
void
binder_plugin_check_config_client(
    BinderPlugin* plugin)
{
    if (plugin->flags & BINDER_PLUGIN_HAVE_CONFIG_SERVICE) {
        if (!plugin->radio_config) {
            plugin->radio_config = radio_config_new_with_version
                (RADIO_CONFIG_INTERFACE_1_1);
            binder_radio_config_trace_update(plugin);
            binder_radio_config_dump_update(plugin);
            if (plugin->data_manager) {
                /* Pass new RadioConfig to BinderDataManager */
                binder_data_manager_set_radio_config(plugin->data_manager,
                    plugin->radio_config);
            } else {
                const BinderPluginSettings* ps = &plugin->settings;

                /* Once we have seen it, we need it */
                plugin->flags |= BINDER_PLUGIN_NEED_CONFIG_SERVICE;
                plugin->data_manager = binder_data_manager_new(plugin->
                    radio_config, ps->dm_flags, ps->non_data_mode);
            }
        }
    } else {
        binder_plugin_drop_radio_config(plugin);
    }
    binder_plugin_foreach_slot(plugin, binder_plugin_slot_check_radio_client);
}

static
void
binder_plugin_check_data_manager(
    BinderPlugin* plugin)
{
    /*
     * If we haven't found any IRadioConfig (even 1.0) then still create
     * the data manager.
     */
    if (!plugin->data_manager) {
        const BinderPluginSettings* ps = &plugin->settings;

        plugin->data_manager = binder_data_manager_new(NULL, ps->dm_flags,
            ps->non_data_mode);
    }
}

static
void
binder_plugin_slot_connected_cb(
    RadioClient* client,
    void* user_data)
{
    BinderSlot* slot = user_data;

    radio_client_remove_handlers(client, slot->client_event_id +
        CLIENT_EVENT_CONNECTED, 1); /* Zeros the id */
    binder_plugin_slot_connected(slot);
}

static
void
binder_plugin_slot_check_radio_client(
    BinderSlot* slot)
{
    BinderPlugin* plugin = slot->plugin;
    const gboolean need_client =
        (slot->flags & BINDER_PLUGIN_SLOT_HAVE_RADIO_SERVICE) &&
        ((plugin->flags & BINDER_PLUGIN_HAVE_CONFIG_SERVICE) ||
         !(plugin->flags & BINDER_PLUGIN_NEED_CONFIG_SERVICE));

    if (!slot->client && need_client) {
        const char* dev = gbinder_servicemanager_device(slot->svcmgr);

        DBG("Bringing up %s", slot->name);
        slot->instance = radio_instance_new_with_modem_slot_and_version(dev,
            slot->name, slot->path, slot->config.slot, slot->version);
        slot->client = radio_client_new(slot->instance);
        if (slot->client) {
            radio_client_set_default_timeout(slot->client,
                slot->req_timeout_ms);
            slot->client_event_id[CLIENT_EVENT_DEATH] =
                radio_client_add_death_handler(slot->client,
                    binder_plugin_slot_death, slot);

            binder_logger_dump_update_slot(slot);
            binder_logger_trace_update_slot(slot);
            binder_plugin_check_data_manager(plugin);

            if (radio_client_connected(slot->client)) {
                binder_plugin_slot_connected(slot);
            } else {
                slot->client_event_id[CLIENT_EVENT_CONNECTED] =
                    radio_client_add_connected_handler(slot->client,
                        binder_plugin_slot_connected_cb, slot);
            }

            /* binder_ext_slot_new just returns NULL if plugin is NULL */
            slot->ext_slot = binder_ext_slot_new(slot->ext_plugin,
                slot->instance, slot->ext_params);
        } else {
            radio_instance_unref(slot->instance);
            slot->instance = NULL;
        }
    } else if (slot->client && !need_client) {
        DBG("Shutting down %s", slot->name);
        binder_plugin_slot_shutdown(slot, TRUE);
    }
}

static
GUtilInts*
binder_plugin_config_get_ints(
    GKeyFile* file,
    const char* group,
    const char* key)
{
    char* value = ofono_conf_get_string(file, group, key);

    if (value) {
        GUtilIntArray *array = gutil_int_array_new();
        char **values, **ptr;

        /*
         * Some people are thinking that # is a comment
         * anywhere on the line, not just at the beginning
         */
        char *comment = strchr(value, '#');

        if (comment) *comment = 0;
        values = g_strsplit(value, ",", -1);
        ptr = values;

        while (*ptr) {
            int val;

            if (gutil_parse_int(*ptr++, 0, &val)) {
                gutil_int_array_append(array, val);
            }
        }

        g_free(value);
        g_strfreev(values);
        return gutil_int_array_free_to_ints(array);
    }
    return NULL;
}

static
const char*
binder_plugin_radio_interface_name(
    RADIO_INTERFACE interface)
{
    switch (interface) {
    case RADIO_INTERFACE_1_0: return BINDER_SLOT_RADIO_INTERFACE_1_0;
    case RADIO_INTERFACE_1_1: return BINDER_SLOT_RADIO_INTERFACE_1_1;
    case RADIO_INTERFACE_1_2: return BINDER_SLOT_RADIO_INTERFACE_1_2;
    case RADIO_INTERFACE_1_3: return BINDER_SLOT_RADIO_INTERFACE_1_3;
    case RADIO_INTERFACE_1_4: return BINDER_SLOT_RADIO_INTERFACE_1_4;
    case RADIO_INTERFACE_1_5: return BINDER_SLOT_RADIO_INTERFACE_1_5;
    case RADIO_INTERFACE_NONE:
    case RADIO_INTERFACE_COUNT:
        break;
    }
    return NULL;
}

static
RADIO_INTERFACE
binder_plugin_parse_radio_interface(
    const char* name)
{
    if (name) {
        RADIO_INTERFACE i;

        for (i = RADIO_INTERFACE_1_0; i < RADIO_INTERFACE_COUNT; i++ ) {
            if (!g_strcmp0(name, binder_plugin_radio_interface_name(i))) {
                return i;
            }
        }
    }
    return BINDER_DEFAULT_RADIO_INTERFACE;
}

/*
 * Parse the spec according to the following grammar:
 *
 *   spec: name | name ':' parameters
 *   params: param | params ';' param
 *   param: key '=' value
 *   transport: STRING
 *   key: STRING
 *   value: STRING
 *
 * Returns the name, hashtable is filled with key-value pairs.
 */
static
char*
binder_plugin_parse_spec_params(
    const char* spec,
    GHashTable* params)
{
    char* name = NULL;
    char* sep = strchr(spec, ':');

    if (sep) {
        name = g_strstrip(g_strndup(spec, sep - spec));
        if (name[0]) {
            char** list = g_strsplit(sep + 1, ";", 0);
            char** ptr;

            for (ptr = list; *ptr; ptr++) {
                const char* p = *ptr;

                sep = strchr(p, '=');
                if (sep) {
                    char* key = g_strndup(p, sep - p);
                    char* value = g_strdup(sep + 1);

                    g_hash_table_insert(params, g_strstrip(key),
                        g_strstrip(value));
                }
            }
            g_strfreev(list);
            return name;
        }
    } else {
        /* Use default name attributes */
        name = g_strstrip(g_strdup(spec));
        if (name[0]) {
            return name;
        }
    }
    g_free(name);
    return NULL;
}

static
BinderSlot*
binder_plugin_create_slot(
    GBinderServiceManager* sm,
    const char* name,
    GKeyFile* file)
{
    BinderSlot* slot;
    BinderSlotConfig* config;
    BinderDataOptions* data_opt;
    BinderDataProfileConfig* dpc;
    GError* error = NULL;
    const char* group = name;
    GUtilInts* ints;
    char **strv;
    char* sval;
    int ival;

    /* path */
    sval = g_key_file_get_string(file, group, BINDER_CONF_SLOT_PATH, NULL);
    if (sval) {
        slot = g_new0(BinderSlot, 1);
        slot->path = sval;
        DBG("%s: " BINDER_CONF_SLOT_PATH " %s", group, slot->path);
    } else {
        /* Path is really required */
        ofono_error("Missing path for slot %s", name);
        return NULL;
    }

    config = &slot->config;
    data_opt = &slot->data_opt;
    dpc = &config->data_profile_config;

    config->slot = BINDER_SLOT_NUMBER_AUTOMATIC;
    config->techs = BINDER_DEFAULT_SLOT_TECHS;
    config->lte_network_mode = BINDER_DEFAULT_SLOT_LTE_MODE;
    config->umts_network_mode = BINDER_DEFAULT_SLOT_UMTS_MODE;
    config->network_mode_timeout_ms =
        BINDER_DEFAULT_SLOT_NETWORK_MODE_TIMEOUT_MS;
    config->network_selection_timeout_ms =
        BINDER_DEFAULT_SLOT_NETWORK_SELECTION_TIMEOUT_MS;
    config->signal_strength_dbm_weak = BINDER_DEFAULT_SLOT_DBM_WEAK;
    config->signal_strength_dbm_strong = BINDER_DEFAULT_SLOT_DBM_STRONG;
    config->empty_pin_query = BINDER_DEFAULT_SLOT_EMPTY_PIN_QUERY;
    config->radio_power_cycle = BINDER_DEFAULT_SLOT_RADIO_POWER_CYCLE;
    config->confirm_radio_power_on = BINDER_DEFAULT_SLOT_CONFIRM_RADIO_POWER_ON;
    config->features = BINDER_DEFAULT_SLOT_FEATURES;
    config->query_available_band_mode =
        BINDER_DEFAULT_SLOT_QUERY_AVAILABLE_BAND_MODE;
    config->replace_strange_oper = BINDER_DEFAULT_SLOT_REPLACE_STRANGE_OPER;
    config->force_gsm_when_radio_off =
        BINDER_DEFAULT_SLOT_FORCE_GSM_WHEN_RADIO_OFF;
    config->cell_info_interval_short_ms =
        BINDER_DEFAULT_SLOT_CELL_INFO_INTERVAL_SHORT_MS;
    config->cell_info_interval_long_ms =
        BINDER_DEFAULT_SLOT_CELL_INFO_INTERVAL_LONG_MS;

    dpc->use_data_profiles = BINDER_DEFAULT_SLOT_USE_DATA_PROFILES;
    dpc->mms_profile_id = BINDER_DEFAULT_SLOT_MMS_DATA_PROFILE_ID;
    dpc->default_profile_id = BINDER_DEFAULT_SLOT_DATA_PROFILE_ID;

    slot->name = g_strdup(name);
    slot->svcmgr = gbinder_servicemanager_ref(sm);
    slot->version = BINDER_DEFAULT_RADIO_INTERFACE;
    slot->req_timeout_ms = BINDER_DEFAULT_SLOT_REQ_TIMEOUT_MS;
    slot->slot_flags = BINDER_DEFAULT_SLOT_FLAGS;
    slot->start_timeout_ms = BINDER_DEFAULT_SLOT_START_TIMEOUT_MS;

    data_opt->allow_data = BINDER_DEFAULT_SLOT_ALLOW_DATA;
    data_opt->data_call_retry_limit =
        BINDER_DEFAULT_SLOT_DATA_CALL_RETRY_LIMIT;
    data_opt->data_call_retry_delay_ms =
        BINDER_DEFAULT_SLOT_DATA_CALL_RETRY_DELAY_MS;

    /* slot */
    ival = g_key_file_get_integer(file, group,
        BINDER_CONF_SLOT_NUMBER, &error);
    if (error) {
        g_clear_error(&error);
    } else if (ival >= 0) {
        config->slot = ival;
        DBG("%s: " BINDER_CONF_SLOT_NUMBER " %u", group, config->slot);
    }

    /* Everything else may be in the [Settings] section */

    /* extPlugin */
    sval = ofono_conf_get_string(file, group, BINDER_CONF_SLOT_EXT_PLUGIN);
    if (sval) {
        GHashTable* params = g_hash_table_new_full(g_str_hash, g_str_equal,
            g_free, g_free);
        char* name = binder_plugin_parse_spec_params(sval, params);

        if (name) {
            slot->ext_plugin = binder_ext_plugin_get(name);
            if (slot->ext_plugin) {
                DBG("%s: " BINDER_CONF_SLOT_EXT_PLUGIN " %s", group, sval);
                slot->ext_params = g_hash_table_ref(params);
                binder_ext_plugin_ref(slot->ext_plugin);
            } else {
                ofono_warn("Unknown extension plugin '%s'", name);
            }
            g_free(name);
        } else {
            ofono_warn("Failed to parse extension spec '%s'", sval);
        }
        g_hash_table_unref(params);
        g_free(sval);
    }

    /* radioInterface */
    sval = ofono_conf_get_string(file, group,
        BINDER_CONF_SLOT_RADIO_INTERFACE);
    if (sval) {
        DBG("%s: " BINDER_CONF_SLOT_RADIO_INTERFACE " %s", group, sval);
        slot->version = binder_plugin_parse_radio_interface(sval);
        g_free(sval);
    }

    /* startTimeout */
    if (ofono_conf_get_integer(file, group,
        BINDER_CONF_SLOT_START_TIMEOUT_MS, &ival) && ival >= 0) {
        DBG("%s: " BINDER_CONF_SLOT_START_TIMEOUT_MS " %d ms", group, ival);
        slot->start_timeout_ms = ival;
    }

    /* timeout */
    if (ofono_conf_get_integer(file, group,
        BINDER_CONF_SLOT_REQUEST_TIMEOUT_MS, &ival) && ival >= 0) {
        DBG("%s: " BINDER_CONF_SLOT_REQUEST_TIMEOUT_MS " %d ms", group, ival);
        slot->req_timeout_ms = ival;
    }

    /* disableFeatures */
    if (ofono_conf_get_mask(file, group,
        BINDER_CONF_SLOT_DISABLE_FEATURES, &ival,
        "cbs", BINDER_FEATURE_CBS,
        "data", BINDER_FEATURE_DATA,
        "netreg", BINDER_FEATURE_NETREG,
        "pb", BINDER_FEATURE_PHONEBOOK,
        "rat", BINDER_FEATURE_RADIO_SETTINGS,
        "auth", BINDER_FEATURE_SIM_AUTH,
        "sms", BINDER_FEATURE_SMS,
        "stk", BINDER_FEATURE_STK,
        "ussd", BINDER_FEATURE_USSD,
        "voice", BINDER_FEATURE_VOICE,
        "ims", BINDER_FEATURE_IMS,
        "all", BINDER_FEATURE_ALL, NULL) && ival) {
        config->features &= ~ival;
        DBG("%s: " BINDER_CONF_SLOT_DISABLE_FEATURES " 0x%04x", group, ival);
    }

    /* deviceStateTracking */
    if (ofono_conf_get_mask(file, group,
        BINDER_CONF_SLOT_DEVMON, &ival,
        "none", BINDER_DEVMON_NONE,
        "all", BINDER_DEVMON_ALL,
        "ds", BINDER_DEVMON_DS,
        "if", BINDER_DEVMON_IF, NULL) && ival) {
        DBG("%s: " BINDER_CONF_SLOT_DEVMON " 0x%04x", group, ival);
    } else {
        ival = BINDER_DEFAULT_SLOT_DEVMON;
    }

    if (ival != BINDER_DEVMON_NONE) {
        BinderDevmon* devmon[3];
        int n = 0;

        if (ival & BINDER_DEVMON_DS) {
            devmon[n++] = binder_devmon_ds_new(config);
        }
        if (ival & BINDER_DEVMON_IF) {
            devmon[n++] = binder_devmon_if_new(config);
        }
        slot->devmon = binder_devmon_combine(devmon, n);
    }

    /* emptyPinQuery */
    if (ofono_conf_get_boolean(file, group,
        BINDER_CONF_SLOT_EMPTY_PIN_QUERY, &config->empty_pin_query)) {
        DBG("%s: " BINDER_CONF_SLOT_EMPTY_PIN_QUERY " %s", group,
            config->empty_pin_query ? "yes" : "no");
    }

    /* useDataProfiles */
    if (ofono_conf_get_boolean(file, group,
        BINDER_CONF_SLOT_USE_DATA_PROFILES, &dpc->use_data_profiles)) {
        DBG("%s: " BINDER_CONF_SLOT_USE_DATA_PROFILES " %s", group,
            dpc->use_data_profiles ? "yes" : "no");
    }

    /* defaultDataProfileId */
    if (ofono_conf_get_integer(file, group,
        BINDER_CONF_SLOT_DEFAULT_DATA_PROFILE_ID, &ival)) {
        dpc->default_profile_id = ival;
        DBG("%s: " BINDER_CONF_SLOT_DEFAULT_DATA_PROFILE_ID " %d", group, ival);
    }

    /* mmsDataProfileId */
    if (ofono_conf_get_integer(file, group,
        BINDER_CONF_SLOT_MMS_DATA_PROFILE_ID, &ival)) {
        dpc->mms_profile_id = ival;
        DBG("%s: " BINDER_CONF_SLOT_MMS_DATA_PROFILE_ID " %d", group, ival);
    }

    /* allowDataReq */
    if (ofono_conf_get_enum(file, group,
        BINDER_CONF_SLOT_ALLOW_DATA_REQ, &ival,
        "on", BINDER_ALLOW_DATA_ENABLED,
        "off", BINDER_ALLOW_DATA_DISABLED, NULL)) {
        DBG("%s: " BINDER_CONF_SLOT_ALLOW_DATA_REQ " %s", group,
            (ival == BINDER_ALLOW_DATA_ENABLED) ? "enabled": "disabled");
        slot->data_opt.allow_data = ival;
    }

    /* technologies */
    strv = ofono_conf_get_strings(file, group, BINDER_CONF_SLOT_TECHNOLOGIES, ',');
    if (strv) {
        char **p;

        config->techs = 0;
        for (p = strv; *p; p++) {
            const char *s = *p;
            enum ofono_radio_access_mode m;

            if (!s[0]) {
                continue;
            }

            if (!strcmp(s, "all")) {
                config->techs = OFONO_RADIO_ACCESS_MODE_ALL;
                break;
            }

            if (!ofono_radio_access_mode_from_string(s, &m)) {
                ofono_warn("Unknown technology %s in [%s] "
                    "section of %s", s, group,
                    BINDER_CONF_FILE);
                continue;
            }

            if (m == OFONO_RADIO_ACCESS_MODE_ANY) {
                config->techs = OFONO_RADIO_ACCESS_MODE_ALL;
                break;
            }

            config->techs |= m;
        }
        g_strfreev(strv);
    }

    /* limit technologies based on radioInterface */
    if (slot->version < RADIO_INTERFACE_1_4) {
        config->techs &= ~OFONO_RADIO_ACCESS_MODE_NR;
    }

    /* lteNetworkMode */
    if (ofono_conf_get_integer(file, group,
        BINDER_CONF_SLOT_LTE_MODE, &ival)) {
        DBG("%s: " BINDER_CONF_SLOT_LTE_MODE " %d", group, ival);
        config->lte_network_mode = ival;
    }

    /* umtsNetworkMode */
    if (ofono_conf_get_integer(file, group,
        BINDER_CONF_SLOT_UMTS_MODE, &ival)) {
        DBG("%s: " BINDER_CONF_SLOT_UMTS_MODE " %d", group, ival);
        config->umts_network_mode = ival;
    }

    /* useNetworkScan */
    if (ofono_conf_get_boolean(file, group,
        BINDER_CONF_SLOT_USE_NETWORK_SCAN,
        &config->use_network_scan)) {
        DBG("%s: " BINDER_CONF_SLOT_USE_NETWORK_SCAN " %s", group,
            config->use_network_scan ? "yes" : "no");
    }

    /* replaceStrangeOperatorNames */
    if (ofono_conf_get_boolean(file, group,
        BINDER_CONF_SLOT_REPLACE_STRANGE_OPER,
        &config->replace_strange_oper)) {
        DBG("%s: " BINDER_CONF_SLOT_REPLACE_STRANGE_OPER " %s", group,
            config->replace_strange_oper ? "yes" : "no");
    }

    /* signalStrengthRange */
    ints = binder_plugin_config_get_ints(file, group,
        BINDER_CONF_SLOT_SIGNAL_STRENGTH_RANGE);
    if (gutil_ints_get_count(ints) == 2) {
        const int* dbms = gutil_ints_get_data(ints, NULL);

        /* MIN,MAX */
        if (dbms[0] < dbms[1]) {
            DBG("%s: " BINDER_CONF_SLOT_SIGNAL_STRENGTH_RANGE " [%d,%d]",
                group, dbms[0], dbms[1]);
            config->signal_strength_dbm_weak = dbms[0];
            config->signal_strength_dbm_strong = dbms[1];
        }
    }
    gutil_ints_unref(ints);

    return slot;
}

static
void
binder_plugin_slot_free(
    BinderSlot* slot)
{
    BinderPlugin* plugin = slot->plugin;

    DBG("%s", slot->name);
    binder_plugin_slot_shutdown(slot, TRUE);
    binder_ext_plugin_unref(slot->ext_plugin);
    plugin->slots = g_slist_remove(plugin->slots, slot);
    ofono_watch_remove_all_handlers(slot->watch, slot->watch_event_id);
    ofono_watch_unref(slot->watch);
    ofono_slot_remove_all_handlers(slot->handle, slot->slot_event_id);
    ofono_slot_unref(slot->handle);
    binder_devmon_free(slot->devmon);
    binder_sim_settings_unref(slot->sim_settings);
    gutil_ints_unref(slot->config.local_hangup_reasons);
    gutil_ints_unref(slot->config.remote_hangup_reasons);
    gbinder_servicemanager_remove_handler(slot->svcmgr, slot->radio_watch_id);
    gbinder_servicemanager_cancel(slot->svcmgr, slot->list_call_id);
    gbinder_servicemanager_unref(slot->svcmgr);
    if (slot->ext_params) {
        g_hash_table_unref(slot->ext_params);
    }
    g_free(slot->name);
    g_free(slot->path);
    g_free(slot->imei);
    g_free(slot->imeisv);
    g_free(slot);
}

static
GSList*
binder_plugin_add_slot(
    GSList* slots,
    BinderSlot* new_slot)
{
    GSList* link = slots;

    /* Slot numbers and paths must be unique */
    while (link) {
        GSList* next = link->next;
        BinderSlot* slot = link->data;
        gboolean delete_this_slot = FALSE;

        if (slot->path && !strcmp(slot->path, new_slot->path)) {
            ofono_error("Duplicate modem path '%s'", slot->path);
            delete_this_slot = TRUE;
        } else if (slot->config.slot != BINDER_SLOT_NUMBER_AUTOMATIC &&
            slot->config.slot == new_slot->config.slot) {
            ofono_error("Duplicate slot %u", slot->config.slot);
            delete_this_slot = TRUE;
        }

        if (delete_this_slot) {
            slots = g_slist_delete_link(slots, link);
            binder_plugin_slot_free(slot);
        }

        link = next;
    }

    return g_slist_append(slots, new_slot);
}

static
GSList*
binder_plugin_try_add_slot(
    GSList* slots,
    BinderSlot* new_slot)
{
    return new_slot ? binder_plugin_add_slot(slots, new_slot) : slots;
}

static
void
binder_plugin_parse_identity(
    BinderPluginIdentity* id,
    const char* value)
{
    char* sep = strchr(value, ':');
    const char* user = value;
    const char* group = NULL;
    char* tmp_user = NULL;
    const struct passwd* pw = NULL;
    const struct group* gr = NULL;

    if (sep) {
        /* Group */
        group = sep + 1;
        gr = getgrnam(group);
        user = tmp_user = g_strndup(value, sep - value);

        if (!gr) {
            int n;

            /* Try numeric */
            if (gutil_parse_int(group, 0, &n)) {
                gr = getgrgid(n);
            }
        }
    }

    /* User */
    pw = getpwnam(user);
    if (!pw) {
        int n;

        /* Try numeric */
        if (gutil_parse_int(user, 0, &n)) {
            pw = getpwuid(n);
        }
    }

    if (pw) {
        DBG("user %s -> %d", user, pw->pw_uid);
        id->uid = pw->pw_uid;
    } else {
        ofono_warn("Invalid user '%s'", user);
    }

    if (gr) {
        DBG("group %s -> %d", group, gr->gr_gid);
        id->gid = gr->gr_gid;
    } else if (group) {
        ofono_warn("Invalid group '%s'", group);
    }

    g_free(tmp_user);
}

static
char**
binder_plugin_find_slots(
    GBinderServiceManager* sm)
{
    char** slots = NULL;
    char** services = gbinder_servicemanager_list_sync(sm);

    if (services) {
        char** ptr;
        static const char prefix[] = RADIO_IFACE_PREFIX;
        static const char suffix[] = "::" RADIO_IFACE "/";
        const gsize prefix_len = sizeof(prefix) - 1;
        const gsize suffix_len = sizeof(suffix) - 1;

        for (ptr = services; *ptr; ptr++) {
            const char* service = *ptr;

            if (!strncmp(service, prefix, prefix_len)) {
                /* Skip the interface version */
                const char* slot = strstr(service + prefix_len, suffix);

                if (slot) {
                    slot += suffix_len;
                    if (*slot && !gutil_strv_contains(slots, slot)) {
                        DBG("found %s", slot);
                        slots = gutil_strv_add(slots, slot);
                    }
                }
            }
        }
        gutil_strv_sort(slots, TRUE);
        g_strfreev(services);
    }

    return slots;
}

static
gboolean
binder_plugin_pattern_match(
    GPatternSpec** patterns,
    const char* str)
{
    const guint len = strlen(str);

    while (*patterns) {
        GPatternSpec* pspec = *patterns++;

        if (g_pattern_match(pspec, len, str, NULL)) {
            return TRUE;
        }
    }
    return FALSE;
}

static
void
binder_plugin_parse_config_file(
    BinderPlugin* plugin,
    GKeyFile* file)
{
    GBinderServiceManager* sm = plugin->svcmgr;
    BinderPluginSettings* ps = &plugin->settings;
    GSList* list = NULL;
    int ival;
    char* sval;
    char** expect_slots;
    char** ignore_slots;
    gboolean ignore_all;

    ival = ps->dm_flags;

    /* 3GLTEHandover */
    if (ofono_conf_get_flag(file, OFONO_COMMON_SETTINGS_GROUP,
        BINDER_CONF_PLUGIN_3GLTE_HANDOVER,
        BINDER_DATA_MANAGER_3GLTE_HANDOVER, &ival)) {
        DBG(BINDER_CONF_PLUGIN_3GLTE_HANDOVER " %s", (ival &
            BINDER_DATA_MANAGER_3GLTE_HANDOVER) ? "yes" : "no");
        ps->dm_flags = ival;
    }

    /* MaxNonDataMode */
    ival = ps->non_data_mode;
    if (ofono_conf_get_enum(file, OFONO_COMMON_SETTINGS_GROUP,
        BINDER_CONF_PLUGIN_MAX_NON_DATA_MODE, &ival, "none",
        OFONO_RADIO_ACCESS_MODE_NONE,
        ofono_radio_access_mode_to_string(OFONO_RADIO_ACCESS_MODE_GSM),
        OFONO_RADIO_ACCESS_MODE_GSM,
        ofono_radio_access_mode_to_string(OFONO_RADIO_ACCESS_MODE_UMTS),
        OFONO_RADIO_ACCESS_MODE_UMTS,
        ofono_radio_access_mode_to_string(OFONO_RADIO_ACCESS_MODE_LTE),
        OFONO_RADIO_ACCESS_MODE_LTE,
        ofono_radio_access_mode_to_string(OFONO_RADIO_ACCESS_MODE_NR),
        OFONO_RADIO_ACCESS_MODE_NR, NULL)) {
        DBG(BINDER_CONF_PLUGIN_MAX_NON_DATA_MODE " %s",
            ofono_radio_access_mode_to_string(ival));
        ps->non_data_mode = ival;
    }

    /* SetRadioCapability */
    if (ofono_conf_get_enum(file, OFONO_COMMON_SETTINGS_GROUP,
        BINDER_CONF_PLUGIN_SET_RADIO_CAP, &ival,
        "auto", BINDER_SET_RADIO_CAP_AUTO,
        "on", BINDER_SET_RADIO_CAP_ENABLED,
        "off", BINDER_SET_RADIO_CAP_DISABLED, NULL)) {
        DBG(BINDER_CONF_PLUGIN_SET_RADIO_CAP " %d", ival);
        ps->set_radio_cap = ival;
    }

    /* Identity */
    sval = g_key_file_get_string(file, OFONO_COMMON_SETTINGS_GROUP,
        BINDER_CONF_PLUGIN_IDENTITY, NULL);
    if (sval) {
        DBG(BINDER_CONF_PLUGIN_IDENTITY " %s", sval);
        binder_plugin_parse_identity(&ps->identity, sval);
        g_free(sval);
    }

    /* ExpectSlots */
    expect_slots = gutil_strv_remove_all(ofono_conf_get_strings(file,
        OFONO_COMMON_SETTINGS_GROUP, BINDER_CONF_PLUGIN_EXPECT_SLOTS,
        BINDER_CONF_LIST_DELIMITER), "");

    /* IgnoreSlots */
    ignore_slots = gutil_strv_remove_all(ofono_conf_get_strings(file,
        OFONO_COMMON_SETTINGS_GROUP, BINDER_CONF_PLUGIN_IGNORE_SLOTS,
        BINDER_CONF_LIST_DELIMITER), "");

    /*
     * The way to stop the plugin from even trying to find any slots is
     * the IgnoreSlots entry containining '*' pattern in combination with
     * an empty (or missing) ExpectSlots.
     */
    ignore_all = gutil_strv_contains(ignore_slots, "*");
    if (gutil_strv_length(expect_slots) || !ignore_all) {
        char** s;
        char** slots = ignore_all ? NULL : binder_plugin_find_slots(sm);

        /*
         * Create the expected slots and remove them from the list.
         * The ignore list doesn't apply to the expected slots.
         */
        if (expect_slots) {
            for (s = expect_slots; *s; s++) {
                const char* slot = *s;

                list = binder_plugin_try_add_slot(list,
                    binder_plugin_create_slot(sm, slot, file));
                slots = gutil_strv_remove_all(slots, slot);
            }
        }

        /* Then check the remaining auto-discovered slots */
        if (!ignore_all) {
            const guint np = gutil_strv_length(ignore_slots);
            GPatternSpec** ignore = g_new(GPatternSpec*, np + 1);
            guint i;

            /* Compile ignore patterns */
            for (i = 0; i < np; i++) {
                ignore[i] = g_pattern_spec_new(ignore_slots[i]);
            }
            ignore[i] = NULL;

            /* Run the remaining slots through the ignore patterns */
            if (slots) {
                for (s = slots; *s; s++) {
                    const char* slot = *s;

                    if (binder_plugin_pattern_match(ignore, slot)) {
                        DBG("skipping %s", slot);
                    } else {
                        list = binder_plugin_try_add_slot(list,
                            binder_plugin_create_slot(sm, slot, file));
                    }
                }
            }

            for (i = 0; i < np; i++) {
                g_pattern_spec_free(ignore[i]);
            }
            g_free(ignore);
            g_strfreev(slots);
        }
    }

    g_strfreev(expect_slots);
    g_strfreev(ignore_slots);
    plugin->slots = list;
}

static
void
binder_plugin_load_config(
    BinderPlugin* plugin,
    const char* path)
{
    const char* dev;
    char* cfg_dev;
    GKeyFile* file = g_key_file_new();

    g_key_file_set_list_separator(file, BINDER_CONF_LIST_DELIMITER);
    ofono_conf_merge_files(file, path);

    /* Device */
    cfg_dev = g_key_file_get_string(file, OFONO_COMMON_SETTINGS_GROUP,
        BINDER_CONF_PLUGIN_DEVICE, NULL);

    /* If device is not configured, then try the default one */
    dev = cfg_dev ? cfg_dev : BINDER_DEFAULT_PLUGIN_DEVICE;
    if ((plugin->svcmgr = gbinder_servicemanager_new(dev)) != NULL) {
        DBG("using %sbinder device %s", cfg_dev ? "" : "default ", dev);
        binder_plugin_parse_config_file(plugin, file);
    } else {
        ofono_warn("Can't open %sbinder device %s",
            cfg_dev ? "" : "default ", dev);
    }
    g_free(cfg_dev);
    g_key_file_free(file);
}

static
void
binder_plugin_set_perm(
    const char* path,
    mode_t mode,
    const BinderPluginIdentity* id)
{
    if (chmod(path, mode)) {
        ofono_error("chmod(%s,%o) failed: %s", path, mode, strerror(errno));
    }
    if (chown(path, id->uid, id->gid)) {
        ofono_error("chown(%s,%d,%d) failed: %s", path, id->uid, id->gid,
            strerror(errno));
    }
}

/* Recursively updates file and directory ownership and permissions */
static
void
binder_plugin_set_storage_perm(
    const char* path,
    const BinderPluginIdentity* id)
{
    DIR* d;
    const mode_t dir_mode = S_IRUSR | S_IWUSR | S_IXUSR;
    const mode_t file_mode = S_IRUSR | S_IWUSR;

    binder_plugin_set_perm(path, dir_mode, id);
    d = opendir(path);
    if (d) {
        const struct dirent* p;

        while ((p = readdir(d)) != NULL) {
            if (strcmp(p->d_name, ".") && strcmp(p->d_name, "..")) {
                char* buf = g_build_filename(path, p->d_name, NULL);
                struct stat st;

                if (!stat(buf, &st)) {
                    mode_t mode;

                    if (S_ISDIR(st.st_mode)) {
                        binder_plugin_set_storage_perm(buf, id);
                        mode = dir_mode;
                    } else {
                        mode = file_mode;
                    }
                    binder_plugin_set_perm(buf, mode, id);
                }
                g_free(buf);
            }
        }
        closedir(d);
    }
}

static
void
binder_plugin_switch_identity(
    const BinderPluginIdentity* id)
{
    DBG("%d:%d", id->uid, id->gid);
    binder_plugin_set_storage_perm(ofono_storage_dir(), id);
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
        ofono_error("prctl(PR_SET_KEEPCAPS) failed: %s", strerror(errno));
    } else if (setgid(id->gid) < 0) {
        ofono_error("setgid(%d) failed: %s", id->gid, strerror(errno));
    } else if (setuid(id->uid) < 0) {
        ofono_error("setuid(%d) failed: %s", id->uid, strerror(errno));
    } else {
        struct __user_cap_header_struct header;
        struct __user_cap_data_struct cap;

        memset(&header, 0, sizeof(header));
        memset(&cap, 0, sizeof(cap));

        header.version = _LINUX_CAPABILITY_VERSION;
        cap.effective =
        cap.permitted = (1 << CAP_NET_ADMIN) | (1 << CAP_NET_RAW);

        if (syscall(SYS_capset, &header, &cap) < 0) {
            ofono_error("syscall(SYS_capset) failed: %s", strerror(errno));
        }
    }
}

static
void
binder_plugin_slot_modem_changed(
    struct ofono_watch* watch,
    void* user_data)
{
    BinderSlot *slot = user_data;

    DBG("%s", slot->path);
    if (!watch->modem) {
        GASSERT(slot->modem);
        slot->modem = NULL;
        binder_data_allow(slot->data, OFONO_SLOT_DATA_NONE);
        binder_radio_caps_request_free(slot->caps_req);
        slot->caps_req = NULL;
    }
}

static
gboolean
binder_plugin_manager_start_timeout(
    gpointer user_data)
{
    BinderPlugin* plugin = user_data;

    DBG("");
    plugin->start_timeout_id = 0;

    /*
     * If we haven't seen IRadioConfig before startup timeout has expired
     * then assume that we don't need it.
     */
    if (!(plugin->flags & BINDER_PLUGIN_HAVE_CONFIG_SERVICE)) {
        plugin->flags &= ~BINDER_PLUGIN_NEED_CONFIG_SERVICE;
    }

    binder_plugin_foreach_slot(plugin, binder_plugin_slot_check_radio_client);
    binder_plugin_manager_started(plugin);
    return G_SOURCE_REMOVE;
}

static
void
binder_plugin_drop_orphan_slots(
    BinderPlugin* plugin)
{
    GSList* l = plugin->slots;

    while (l) {
        GSList* next = l->next;
        BinderSlot* slot = l->data;

        if (!slot->handle) {
            plugin->slots = g_slist_delete_link(plugin->slots, l);
            binder_plugin_slot_free(slot);
        }
        l = next;
    }
}

static
void
binder_plugin_manager_start_done(
    gpointer user_data)
{
    BinderPlugin* plugin = user_data;

    DBG("");
    if (plugin->start_timeout_id) {
        /* Startup was cancelled */
        plugin->start_timeout_id = 0;
        binder_plugin_drop_orphan_slots(plugin);
    }
}

static
void
binder_plugin_slot_pick_shortest_timeout_cb(
    BinderSlot* slot,
    void* param)
{
    guint* timeout = param;

    if (!(*timeout) || (*timeout) < slot->start_timeout_ms) {
        (*timeout) = slot->start_timeout_ms;
    }
}

static
gboolean
binder_plugin_slot_start_timeout(
    gpointer user_data)
{
    BinderSlot* slot = user_data;
    BinderPlugin* plugin = slot->plugin;

    DBG("%s", slot->name);
    slot->start_timeout_id = 0;

    /*
     * If any slot times out and we haven't seen IRadioConfig so far,
     * then assume that we don't need it.
     */
    if (!(plugin->flags & BINDER_PLUGIN_HAVE_CONFIG_SERVICE)) {
        plugin->flags &= ~BINDER_PLUGIN_NEED_CONFIG_SERVICE;
    }
    binder_plugin_foreach_slot(plugin, binder_plugin_slot_check_radio_client);
    if (!slot->client) {
        plugin->slots = g_slist_remove(plugin->slots, slot);
        binder_plugin_slot_free(slot);
    }
    binder_plugin_check_if_started(plugin);
    return G_SOURCE_REMOVE;
}

static
void
binder_logger_slot_start(
    BinderSlot* slot)
{
    /* Watch the radio service */
    slot->radio_watch_id =
        gbinder_servicemanager_add_registration_handler(slot->svcmgr,
            binder_radio_ifaces[slot->version],
            binder_plugin_slot_service_registration_proc, slot);

    /* They could be already there */
    binder_plugin_slot_check(slot);
}

static BinderPlugin*
binder_plugin_slot_driver_init(
    struct ofono_slot_manager* sm)
{
    BinderPlugin* plugin = g_new0(BinderPlugin, 1);
    BinderPluginSettings* ps = &plugin->settings;
    char* config_file = g_build_filename(ofono_config_dir(),
        BINDER_CONF_FILE, NULL);
    GSList* l;
    GError* error = NULL;
    GHashTable* slot_paths;
    GHashTable* slot_numbers;
    guint i;

    DBG("");
    for (i = 0; i < G_N_ELEMENTS(binder_plugin_modules); i++) {
        binder_plugin_modules[i].init();
    }
    plugin->slot_manager = sm;
    binder_plugin_parse_identity(&ps->identity, BINDER_DEFAULT_PLUGIN_IDENTITY);
    ps->set_radio_cap = BINDER_SET_RADIO_CAP_AUTO;
    ps->dm_flags = BINDER_DEFAULT_PLUGIN_DM_FLAGS;
    ps->non_data_mode = BINDER_DEFAULT_MAX_NON_DATA_MODE;

    /* Connect to system bus before we switch the identity */
    plugin->system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!plugin->system_bus) {
        ofono_error("Failed to connect system bus: %s", error->message);
        g_error_free(error);
    }

    /* This populates plugin->slots */
    binder_plugin_load_config(plugin, config_file);

    /*
     * Finish slot initialization. Some of them may not have path and
     * slot index set up yet.
     */
    slot_paths = g_hash_table_new(g_str_hash, g_str_equal);
    slot_numbers = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (l = plugin->slots; l; l = l->next) {
        BinderSlot* slot = l->data;

        if (slot->path) {
            g_hash_table_add(slot_paths, (gpointer) slot->path);
        }
        if (slot->config.slot != BINDER_SLOT_NUMBER_AUTOMATIC) {
            g_hash_table_insert(slot_numbers,
                GUINT_TO_POINTER(slot->config.slot),
                GUINT_TO_POINTER(slot->config.slot));
        }
    }

    for (l = plugin->slots; l; l = l->next) {
        BinderSlot* slot = l->data;

        /* Auto-assign paths */
        if (!slot->path) {
            guint i = 0;

            do {
                g_free(slot->path);
                slot->path = g_strdup_printf("/%s_%u",
                    BINDER_DEFAULT_SLOT_PATH_PREFIX, i++);
            } while (g_hash_table_contains(slot_paths, slot->path));
            DBG("assigned %s => %s", slot->name, slot->path);
            g_hash_table_insert(slot_paths,
                (gpointer) slot->path,
                (gpointer) slot->path);
        }

        /* Auto-assign slot numbers */
        if (slot->config.slot == BINDER_SLOT_NUMBER_AUTOMATIC) {
            slot->config.slot = 0;
            while (g_hash_table_contains(slot_numbers,
                GUINT_TO_POINTER(slot->config.slot))) {
                slot->config.slot++;
            }
            DBG("assigned %s => %u", slot->name, slot->config.slot);
            g_hash_table_insert(slot_numbers,
                GUINT_TO_POINTER(slot->config.slot),
                GUINT_TO_POINTER(slot->config.slot));
        }

        slot->plugin = plugin;
        slot->watch = ofono_watch_new(slot->path);
        slot->watch_event_id[WATCH_EVENT_MODEM] =
            ofono_watch_add_modem_changed_handler(slot->watch,
                binder_plugin_slot_modem_changed, slot);
        slot->sim_settings = binder_sim_settings_new(slot->path,
            slot->config.techs);

        /* Start timeout for this slot */
        slot->start_timeout_id = g_timeout_add(slot->start_timeout_ms,
            binder_plugin_slot_start_timeout, slot);
    }

    g_hash_table_unref(slot_paths);
    g_hash_table_unref(slot_numbers);
    g_free(config_file);
    return plugin;
}

static
void
binder_plugin_slot_check_plugin_flags_cb(
    BinderSlot* slot,
    void* param)
{
    BinderPlugin* plugin = param;

    if (slot->version >= RADIO_INTERFACE_1_2) {
        plugin->flags |= BINDER_PLUGIN_NEED_CONFIG_SERVICE;
    }
}

static
guint
binder_plugin_slot_driver_start(
    BinderPlugin* plugin)
{
    BinderPluginSettings* ps = &plugin->settings;
    guint start_timeout, shortest_timeout = 0;

    DBG("");

    /* Switch the user to the one expected by the radio subsystem */
    binder_plugin_switch_identity(&ps->identity);

    /* Pick the shortest timeout */
    binder_plugin_foreach_slot_param(plugin,
        binder_plugin_slot_pick_shortest_timeout_cb, &shortest_timeout);

    /* Timeout remains zero if there are no slots */
    start_timeout = shortest_timeout ?
        (shortest_timeout + BINDER_SLOT_REGISTRATION_TIMEOUT_MS) : 0;
    GASSERT(!plugin->start_timeout_id);
    plugin->start_timeout_id = g_timeout_add_full(G_PRIORITY_DEFAULT,
        start_timeout, binder_plugin_manager_start_timeout,
        plugin, binder_plugin_manager_start_done);

    DBG("start timeout %u ms id %u", start_timeout, plugin->start_timeout_id);

    /*
     * Keep and eye on IRadioConfig service. Note that we're watching
     * IRadioConfig 1.0 even though it's of no use to us.
     *
     * But if IRadioConfig 1.0 is there and IRadioConfig 1.1 isn't,
     * then there's no point in waiting for IRadioConfig 1.1.
     *
     * At startup, assume that we need IRadioConfig for IRadio 1.2
     * and higher.
     */
    binder_plugin_foreach_slot_param(plugin,
        binder_plugin_slot_check_plugin_flags_cb, plugin);
    plugin->radio_config_watch_id =
        gbinder_servicemanager_add_registration_handler(plugin->svcmgr,
            RADIO_CONFIG_1_0, binder_plugin_service_registration_proc, plugin);
    binder_plugin_service_check(plugin);

    /* And per-slot IRadio services too */
    binder_plugin_foreach_slot(plugin, binder_logger_slot_start);

    /* Return the timeout id that can be used for cancelling the startup */
    return plugin->start_timeout_id;
}

static
void
binder_plugin_slot_driver_cancel(
    BinderPlugin* plugin,
    guint id)
{
    DBG("%u", id);
    GASSERT(plugin->start_timeout_id == id);
    plugin->start_timeout_id = 0;
    g_source_remove(id);
}

static
void
binder_plugin_slot_driver_cleanup(
    BinderPlugin* plugin)
{
    if (plugin) {
        guint i;

        for (i = 0; i < G_N_ELEMENTS(binder_plugin_modules); i++) {
            binder_plugin_modules[i].cleanup();
        }
        GASSERT(!plugin->slots);
        if (plugin->system_bus) {
            g_object_unref(plugin->system_bus);
        }
        binder_plugin_drop_radio_config(plugin);
        gbinder_servicemanager_cancel(plugin->svcmgr, plugin->list_call_id);
        gbinder_servicemanager_remove_handler(plugin->svcmgr,
            plugin->radio_config_watch_id);
        gbinder_servicemanager_unref(plugin->svcmgr);
        binder_data_manager_unref(plugin->data_manager);
        binder_radio_caps_manager_remove_handler(plugin->caps_manager,
            plugin->caps_manager_event_id);
        binder_radio_caps_manager_unref(plugin->caps_manager);
        g_free(plugin);
    }
}

void
binder_plugin_mce_log_notify(
    struct ofono_debug_desc* desc)
{
    mce_log.level = (desc->flags & OFONO_DEBUG_FLAG_PRINT) ?
        GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

/* Global part (that requires access to these global variables) */

static struct ofono_slot_driver_reg* binder_driver_reg = NULL;

static
void
binder_logger_trace_notify(
    struct ofono_debug_desc* desc)
{
    BinderPlugin* plugin = ofono_slot_driver_get_data(binder_driver_reg);

    binder_logger_module.level = (desc->flags & OFONO_DEBUG_FLAG_PRINT) ?
        GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
    binder_radio_config_trace_update(plugin);
    binder_plugin_foreach_slot(plugin, binder_logger_trace_update_slot);
}

static
void
binder_logger_dump_notify(
    struct ofono_debug_desc* desc)
{
    BinderPlugin* plugin = ofono_slot_driver_get_data(binder_driver_reg);

    binder_plugin_foreach_slot(plugin, binder_logger_dump_update_slot);
    binder_radio_config_dump_update(plugin);
}

static
void
binder_plugin_manager_started(
    BinderPlugin* plugin)
{
    binder_plugin_drop_orphan_slots(plugin);
    binder_plugin_check_data_manager(plugin);
    binder_data_manager_check_data(plugin->data_manager);
    ofono_slot_driver_started(binder_driver_reg);
}

static
int
binder_plugin_init(void)
{
    static const struct ofono_slot_driver binder_slot_driver = {
        .name = BINDER_DRIVER,
        .api_version = OFONO_SLOT_API_VERSION,
        .init = binder_plugin_slot_driver_init,
        .start = binder_plugin_slot_driver_start,
        .cancel = binder_plugin_slot_driver_cancel,
        .cleanup = binder_plugin_slot_driver_cleanup,
    };

   static struct ofono_debug_desc mce_debug OFONO_DEBUG_ATTR = {
        .name = "mce",
        .flags = OFONO_DEBUG_FLAG_DEFAULT,
        .notify = binder_plugin_mce_log_notify
    };

    DBG("");

    /*
     * Log categories (accessible via D-Bus) are generated from
     * ofono_debug_desc structures, while libglibutil based log
     * functions receive the log module name. Those should match.
     */
    mce_log.name = mce_debug.name;

    /* Register the slot driver */
    binder_driver_reg = ofono_slot_driver_register(&binder_slot_driver);
    return 0;
}

static
void
binder_plugin_exit(void)
{
    DBG("");
    binder_plugin_foreach_slot(ofono_slot_driver_get_data(binder_driver_reg),
        binder_plugin_slot_free);
    ofono_slot_driver_unregister(binder_driver_reg);
    binder_driver_reg = NULL;
}

OFONO_PLUGIN_DEFINE(binder, "Binder adaptation plugin", OFONO_VERSION,
    OFONO_PLUGIN_PRIORITY_DEFAULT, binder_plugin_init, binder_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
