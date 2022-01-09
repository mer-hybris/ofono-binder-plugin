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

#ifndef BINDER_SIM_CARD_H
#define BINDER_SIM_CARD_H

#include "binder_types.h"

#include <glib-object.h>

typedef struct binder_sim_card_app {
    RADIO_APP_TYPE app_type;
    RADIO_APP_STATE app_state;
    RADIO_PERSO_SUBSTATE perso_substate;
    char* aid;
    char* label;
    guint pin_replaced;
    RADIO_PIN_STATE pin1_state;
    RADIO_PIN_STATE pin2_state;
} BinderSimCardApp;

typedef struct binder_sim_card_status {
    RADIO_CARD_STATE card_state;
    RADIO_PIN_STATE pin_state;
    int gsm_umts_index;
    int ims_index;
    guint num_apps;
    BinderSimCardApp* apps;
} BinderSimCardStatus;

struct binder_sim_card {
    GObject object;
    BinderSimCardStatus* status;
    const BinderSimCardApp* app;
    gboolean sim_io_active;
    guint slot;
};

typedef
void
(*BinderSimCardFunc)(
    BinderSimCard* card,
    void* user_data);

BinderSimCard*
binder_sim_card_new(
    RadioClient* client,
    guint slot)
    BINDER_INTERNAL;

BinderSimCard*
binder_sim_card_ref(
    BinderSimCard* card)
    BINDER_INTERNAL;

void
binder_sim_card_unref(
    BinderSimCard* card)
    BINDER_INTERNAL;

void
binder_sim_card_reset(
    BinderSimCard* card)
    BINDER_INTERNAL;

void
binder_sim_card_request_status(
    BinderSimCard* card)
    BINDER_INTERNAL;

void
binder_sim_card_sim_io_started(
    BinderSimCard* card,
    gpointer key)
    BINDER_INTERNAL;

void
binder_sim_card_sim_io_finished(
    BinderSimCard* card,
    gpointer key)
    BINDER_INTERNAL;

gboolean
binder_sim_card_ready(
    BinderSimCard* card)
    BINDER_INTERNAL;

gulong
binder_sim_card_add_status_received_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
    BINDER_INTERNAL;

gulong
binder_sim_card_add_status_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
    BINDER_INTERNAL;

gulong
binder_sim_card_add_state_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
    BINDER_INTERNAL;

gulong
binder_sim_card_add_app_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
    BINDER_INTERNAL;

gulong
binder_sim_card_add_sim_io_active_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
    BINDER_INTERNAL;

void
binder_sim_card_remove_handler(
    BinderSimCard* card,
    gulong id)
    BINDER_INTERNAL;

void
binder_sim_card_remove_handlers(
    BinderSimCard* card,
    gulong* ids,
    int n)
    BINDER_INTERNAL;

/* Inline wrappers */

static inline RADIO_APP_TYPE binder_sim_card_app_type(BinderSimCard* card)
    { return (card && card->app) ? card->app->app_type :
        RADIO_APP_TYPE_UNKNOWN; }

static inline const char* binder_sim_card_app_aid(BinderSimCard* card)
    { return (card && card->app) ? card->app->aid : NULL; }

#define binder_sim_card_remove_all_handlers(net, ids) \
	binder_sim_card_remove_handlers(net, ids, G_N_ELEMENTS(ids))

#endif /* BINDER_SIM_CARD_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
