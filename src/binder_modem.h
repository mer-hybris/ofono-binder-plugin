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

#ifndef BINDER_MODEM_H
#define BINDER_MODEM_H

#include "binder_types.h"
#include "binder_ext_types.h"

#include <ofono/modem.h>

struct binder_modem {
    RadioInstance* instance;
    RadioClient* client;
    RadioClient* data_client;
    RadioClient* network_client;
    RadioClient* sim_client;
    const char* path;
    const char* log_prefix;
    const char* imei;
    const char* imeisv;
    struct ofono_modem* ofono;
    struct ofono_cell_info* cell_info;
    struct ofono_watch* watch;
    BinderExtSlot* ext;
    BinderData* data;
    BinderImsReg* ims;
    BinderNetwork* network;
    BinderRadio* radio;
    BinderSimCard* sim_card;
    BinderSimSettings* sim_settings;
    BinderSlotConfig config;
};

#define binder_modem_get_path(modem) ofono_modem_get_path((modem)->ofono)
#define binder_modem_get_data(modem) ((BinderModem*)ofono_modem_get_data(modem))

void
binder_modem_init(void)
    BINDER_INTERNAL;

void
binder_modem_cleanup(void)
    BINDER_INTERNAL;

BinderModem*
binder_modem_create(
    RadioInstance* instance,
    RadioClient* client,
    RadioClient* data_client,
    RadioClient* network_client,
    RadioClient* sim_client,
    const char* name,
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
    BINDER_INTERNAL;

#endif /* BINDER_MODEM_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
