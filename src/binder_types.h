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

#ifndef BINDER_TYPES_H
#define BINDER_TYPES_H

#include <radio_types.h>
#include <radio_config_types.h>
#include <ofono/radio-settings.h>

typedef struct binder_data BinderData;
typedef struct binder_data_manager BinderDataManager;
typedef struct binder_devmon BinderDevmon;
typedef struct binder_ims_reg BinderImsReg;
typedef struct binder_logger BinderLogger;
typedef struct binder_modem BinderModem;
typedef struct binder_network BinderNetwork;
typedef struct binder_radio_caps BinderRadioCaps;
typedef struct binder_radio_caps_manager BinderRadioCapsManager;
typedef struct binder_radio_caps_request BinderRadioCapsRequest;
typedef struct binder_radio BinderRadio;
typedef struct binder_sim_card BinderSimCard;
typedef struct binder_sim_settings BinderSimSettings;

typedef enum binder_feature_mask {
    BINDER_FEATURE_NONE           = 0,
    BINDER_FEATURE_CBS            = 0x0001, /* cbs */
    BINDER_FEATURE_DATA           = 0x0002, /* data */
    BINDER_FEATURE_NETREG         = 0x0004, /* netreg */
    BINDER_FEATURE_PHONEBOOK      = 0x0008, /* pb */
    BINDER_FEATURE_RADIO_SETTINGS = 0x0010, /* rat */
    BINDER_FEATURE_SIM_AUTH       = 0x0020, /* auth */
    BINDER_FEATURE_SMS            = 0x0040, /* sms */
    BINDER_FEATURE_STK            = 0x0080, /* stk */
    BINDER_FEATURE_USSD           = 0x0100, /* ussd */
    BINDER_FEATURE_VOICE          = 0x0200, /* voice */
    BINDER_FEATURE_IMS            = 0x0400, /* ims */
    BINDER_FEATURE_ALL            = 0x07ff  /* all */
} BINDER_FEATURE_MASK;

typedef struct binder_data_profile_config {
    gboolean use_data_profiles;
    guint default_profile_id;
    guint mms_profile_id;
} BinderDataProfileConfig;

typedef struct binder_slot_config {
    guint slot;
    int cell_info_interval_short_ms;
    int cell_info_interval_long_ms;
    int network_mode_timeout_ms;
    int network_selection_timeout_ms;
    int signal_strength_dbm_weak;
    int signal_strength_dbm_strong;
    enum ofono_radio_access_mode techs;
    RADIO_PREF_NET_TYPE lte_network_mode;
    RADIO_PREF_NET_TYPE umts_network_mode;
    BINDER_FEATURE_MASK features;
    gboolean query_available_band_mode;
    gboolean empty_pin_query;
    gboolean radio_power_cycle;
    gboolean confirm_radio_power_on;
    gboolean use_network_scan;
    gboolean replace_strange_oper;
    gboolean force_gsm_when_radio_off;
    BinderDataProfileConfig data_profile_config;
    GUtilInts* local_hangup_reasons;
    GUtilInts* remote_hangup_reasons;
} BinderSlotConfig;

#define BINDER_DRIVER "binder"
#define BINDER_INTERNAL G_GNUC_INTERNAL

#define BINDER_RETRY_SECS (2)
#define BINDER_RETRY_MS   (BINDER_RETRY_SECS * 1000)

typedef void (*BinderCallback)(void);
#define BINDER_CB(f) ((BinderCallback)(f))

#define OFONO_RADIO_ACCESS_MODE_COUNT (4)

#define OFONO_RADIO_ACCESS_MODE_ALL (\
    OFONO_RADIO_ACCESS_MODE_GSM  |\
    OFONO_RADIO_ACCESS_MODE_UMTS |\
    OFONO_RADIO_ACCESS_MODE_LTE |\
    OFONO_RADIO_ACCESS_MODE_NR)

#define OFONO_RADIO_ACCESS_MODE_NONE \
    ((enum ofono_radio_access_mode) 0)

/* Masks */
#define OFONO_RADIO_ACCESS_GSM_MASK OFONO_RADIO_ACCESS_MODE_GSM
#define OFONO_RADIO_ACCESS_UMTS_MASK \
    (OFONO_RADIO_ACCESS_MODE_UMTS | (OFONO_RADIO_ACCESS_MODE_UMTS - 1))
#define OFONO_RADIO_ACCESS_LTE_MASK \
    (OFONO_RADIO_ACCESS_MODE_LTE | (OFONO_RADIO_ACCESS_MODE_LTE - 1))
#define OFONO_RADIO_ACCESS_NR_MASK \
    (OFONO_RADIO_ACCESS_MODE_NR | (OFONO_RADIO_ACCESS_MODE_NR - 1))

#endif /* BINDER_TYPES_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
