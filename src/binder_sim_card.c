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

#include "binder_sim_card.h"
#include "binder_radio.h"
#include "binder_util.h"
#include "binder_log.h"

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_misc.h>

/*
 * First we wait for USIM app to get activated by itself. If that
 * doesn't happen within UICC_SUBSCRIPTION_START_MS we poke the SIM
 * with SET_UICC_SUBSCRIPTION request, resubmitting it if it times out.
 * If nothing happens within UICC_SUBSCRIPTION_TIMEOUT_MS we give up.
 *
 * Submitting SET_UICC_SUBSCRIPTION request when modem doesn't expect
 * it sometimes breaks pretty much everything. Unfortunately, there no
 * reliable way to find out when modem expects it and when it doesn't :/
 */
#define UICC_SUBSCRIPTION_RETRY_MS     (500)
#define UICC_SUBSCRIPTION_START_MS    (5000)
#define UICC_SUBSCRIPTION_TIMEOUT_MS (30000)

/* SIM I/O idle timeout is measured in the number of idle loops.
 * When active SIM I/O is going on, the idle loop count very rarely
 * exceeds 1 between the requests, so 10 is more than enough. Idle
 * loop is actually more accurate criteria than a timeout because
 * it doesn't depend that much on the system load. */
#define SIM_IO_IDLE_LOOPS (10)

enum binder_sim_card_event {
    EVENT_SIM_STATUS_CHANGED,
    EVENT_UICC_SUBSCRIPTION_STATUS_CHANGED,
    EVENT_COUNT
};

typedef struct binder_sim_card_object {
    BinderSimCard card;
    RadioRequest* status_req;
    RadioRequest* sub_req;
    RadioRequestGroup* g;
    guint sub_start_timer;
    gulong event_id[EVENT_COUNT];
    guint sim_io_idle_id;
    guint sim_io_idle_count;
    GHashTable* sim_io_pending;
} BinderSimCardObject;

enum binder_sim_card_signal {
    SIGNAL_STATUS_RECEIVED,
    SIGNAL_STATUS_CHANGED,
    SIGNAL_STATE_CHANGED,
    SIGNAL_APP_CHANGED,
    SIGNAL_SIM_IO_ACTIVE_CHANGED,
    SIGNAL_COUNT
};

#define SIGNAL_STATUS_RECEIVED_NAME       "binder-simcard-status-received"
#define SIGNAL_STATUS_CHANGED_NAME        "binder-simcard-status-changed"
#define SIGNAL_STATE_CHANGED_NAME         "binder-simcard-state-changed"
#define SIGNAL_APP_CHANGED_NAME           "binder-simcard-app-changed"
#define SIGNAL_SIM_IO_ACTIVE_CHANGED_NAME "binder-simcard-sim-io-active-changed"

static guint binder_sim_card_signals[SIGNAL_COUNT] = { 0 };

typedef GObjectClass BinderSimCardObjectClass;
GType binder_sim_card_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderSimCardObject, binder_sim_card, G_TYPE_OBJECT)
#define PARENT_CLASS binder_sim_card_parent_class
#define THIS_TYPE binder_sim_card_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    BinderSimCardObject)

#define NEW_SIGNAL(klass,name) NEW_SIGNAL_(klass,name##_CHANGED)
#define NEW_SIGNAL_(klass,name) \
    binder_sim_card_signals[SIGNAL_##name] =       \
        g_signal_new(SIGNAL_##name##_NAME, \
            G_OBJECT_CLASS_TYPE(klass), G_SIGNAL_RUN_FIRST, \
            0, NULL, NULL, NULL, G_TYPE_NONE, 0)

#define BINDER_SIMCARD_STATE_CHANGED  (0x01)
#define BINDER_SIMCARD_STATUS_CHANGED (0x02)

static inline BinderSimCardObject* binder_sim_card_cast(BinderSimCard* card)
    { return G_LIKELY(card) ? THIS(card) : NULL; }

static
gboolean
binder_sim_card_app_equal(
    const BinderSimCardApp* a1,
    const BinderSimCardApp* a2)
{
    if (a1 == a2) {
        return TRUE;
    } else if (!a1 || !a2) {
        return FALSE;
    } else {
        return a1->app_type == a2->app_type &&
            a1->app_state == a2->app_state &&
            a1->perso_substate == a2->perso_substate &&
            a1->pin_replaced == a2->pin_replaced &&
            a1->pin1_state == a2->pin1_state &&
            a1->pin2_state == a2->pin2_state &&
            !g_strcmp0(a1->aid, a2->aid) &&
            !g_strcmp0(a1->label, a2->label);
    }
}

static
int
binder_sim_card_status_compare(
    const BinderSimCardStatus* s1,
    const BinderSimCardStatus* s2)
{
    if (s1 == s2) {
        return 0;
    } else if (!s1 || !s2) {
        return BINDER_SIMCARD_STATE_CHANGED | BINDER_SIMCARD_STATUS_CHANGED;
    } else {
        int diff = 0;

        if (s1->card_state != s2->card_state) {
            diff |= BINDER_SIMCARD_STATE_CHANGED;
        }

        if (s1->pin_state != s2->pin_state ||
            s1->gsm_umts_index != s2->gsm_umts_index ||
            s1->ims_index != s2->ims_index ||
            s1->num_apps != s2->num_apps) {
            diff |= BINDER_SIMCARD_STATUS_CHANGED;
        } else {
            int i;

            for (i = 0; i < s1->num_apps; i++) {
                if (!binder_sim_card_app_equal(s1->apps + i, s2->apps + i)) {
                    diff |= BINDER_SIMCARD_STATUS_CHANGED;
                    break;
                }
            }
        }

        return diff;
    }
}

static
void
binder_sim_card_status_free(
    BinderSimCardStatus* status)
{
    if (status) {
        if (status->apps) {
            int i;

            for (i = 0; i < status->num_apps; i++) {
                g_free(status->apps[i].aid);
                g_free(status->apps[i].label);
            }
        }
        /* status->apps is allocated from the same memory block */
        g_free(status);
    }
}

static
void
binder_sim_card_tx_start(
    BinderSimCardObject* self)
{
    RADIO_BLOCK block = radio_request_group_block_status(self->g);

    if (block == RADIO_BLOCK_NONE) {
        block = radio_request_group_block(self->g);
        DBG("status tx for slot %u %s", self->card.slot,
            (block == RADIO_BLOCK_ACQUIRED) ? "started" : "starting");
    }
}

static
void
binder_sim_card_tx_check(
    BinderSimCardObject* self)
{
    if (radio_request_group_block_status(self->g) != RADIO_BLOCK_NONE) {
        BinderSimCard* card = &self->card;
        const BinderSimCardStatus* status = card->status;

        if (status && status->card_state == RADIO_CARD_STATE_PRESENT) {
            /*
             * Transaction (if there is any) is finished when
             * both GET_SIM_STATUS and SET_UICC_SUBSCRIPTION
             * complete or get dropped.
             */
            if (!self->status_req && !self->sub_req &&
                status->gsm_umts_index >= 0 &&
                status->gsm_umts_index < status->num_apps) {
                DBG("status tx for slot %u finished", card->slot);
                radio_request_group_unblock(self->g);
            }
        } else {
            DBG("status tx for slot %u cancelled", card->slot);
            radio_request_group_unblock(self->g);
        }
    }
}

static
void
binder_sim_card_subscription_done(
    BinderSimCardObject* self)
{
    if (self->sub_start_timer) {
        /* Don't need this timer anymore */
        g_source_remove(self->sub_start_timer);
        self->sub_start_timer = 0;
    }
    if (self->sub_req) {
        radio_request_drop(self->sub_req);
        self->sub_req = NULL;
    }
    binder_sim_card_tx_check(self);
}

static
void
binder_sim_card_subscribe_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCardObject* self = THIS(user_data);

    GASSERT(resp == RADIO_RESP_SET_UICC_SUBSCRIPTION);
    GASSERT(status == RADIO_TX_STATUS_OK);
    GASSERT(error == RADIO_ERROR_NONE);
    GASSERT(self->sub_req == req);

    radio_request_unref(self->sub_req);
    self->sub_req = NULL;
    DBG("UICC subscription OK for slot %u", self->card.slot);
    binder_sim_card_subscription_done(self);
}

static
void
binder_sim_card_subscribe(
    BinderSimCardObject* self,
    int app_index)
{
    BinderSimCard* card = &self->card;
    GBinderWriter args;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_SET_UICC_SUBSCRIPTION, &args,
        binder_sim_card_subscribe_cb, NULL, self);
    RadioSelectUiccSub* sub = gbinder_writer_new0(&args, RadioSelectUiccSub);

    /* setUiccSubscription(serial, SelectUiccSub uiccSub) */
    DBG("%u,%d", card->slot, app_index);
    sub->slot = card->slot;
    sub->appIndex = app_index;
    sub->actStatus = RADIO_UICC_SUB_ACTIVATE;
    gbinder_writer_append_buffer_object(&args, sub, sizeof(*sub));

    radio_request_set_retry(req, UICC_SUBSCRIPTION_RETRY_MS, -1);
    radio_request_set_timeout(req, UICC_SUBSCRIPTION_TIMEOUT_MS);

    /* N.B. Some adaptations never reply to SET_UICC_SUBSCRIPTION request */
    radio_request_drop(self->sub_req);
    self->sub_req = req;

    /*
     * Don't allow any requests other that GET_SIM_STATUS until
     * we are done with the subscription.
     */
    binder_sim_card_tx_start(self);
    radio_request_submit(self->sub_req);
}

static
int
binder_sim_card_select_app(
    const BinderSimCardStatus *status)
{
    int i, selected_app = -1;

    for (i = 0; i < status->num_apps; i++) {
        const int type = status->apps[i].app_type;

        if (type == RADIO_APP_TYPE_USIM || type == RADIO_APP_TYPE_RUIM) {
            selected_app = i;
            break;
        } else if (type != RADIO_APP_TYPE_UNKNOWN && selected_app == -1) {
            selected_app = i;
        }
    }

    DBG("%d", selected_app);
    return selected_app;
}

static
void
binder_sim_card_update_app(
    BinderSimCardObject* self)
{
    BinderSimCard* card = &self->card;
    const BinderSimCardApp* old_app = card->app;
    const BinderSimCardStatus* status = card->status;
    int app_index;

    if (status->card_state == RADIO_CARD_STATE_PRESENT) {
        if (status->gsm_umts_index >= 0 &&
            status->gsm_umts_index < status->num_apps) {
            app_index = status->gsm_umts_index;
            binder_sim_card_subscription_done(self);
        } else {
            app_index = binder_sim_card_select_app(status);
            if (app_index >= 0 && !self->sub_start_timer) {
                binder_sim_card_subscribe(self, app_index);
            }
        }
    } else {
        app_index = -1;
        binder_sim_card_subscription_done(self);
    }

    if (app_index >= 0 &&
        status->apps[app_index].app_type != RADIO_APP_TYPE_UNKNOWN) {
        card->app = status->apps + app_index;
    } else {
        card->app = NULL;
    }

    if (!binder_sim_card_app_equal(old_app, card->app)) {
        g_signal_emit(self, binder_sim_card_signals[SIGNAL_APP_CHANGED], 0);
    }
}

static
gboolean
binder_sim_card_sub_start_timeout(
    gpointer user_data)
{
    BinderSimCardObject* self = THIS(user_data);

    DBG("%u", self->card.slot);
    GASSERT(self->sub_start_timer);
    self->sub_start_timer = 0;
    binder_sim_card_update_app(self);
    return G_SOURCE_REMOVE;
}

static
void
binder_sim_card_update_status(
    BinderSimCardObject* self,
    BinderSimCardStatus* status)
{
    BinderSimCard* card = &self->card;
    const int diff = binder_sim_card_status_compare(card->status, status);

    if (diff) {
        BinderSimCardStatus* old_status = card->status;

        card->status = status;
        if (diff & BINDER_SIMCARD_STATE_CHANGED &&
            status->card_state == RADIO_CARD_STATE_PRESENT) {

            /*
             * SIM card has just appeared, give it some time to
             * activate the USIM app
             */
            if (self->sub_start_timer) {
                g_source_remove(self->sub_start_timer);
            }
            DBG("started subscription timeout for slot %u", card->slot);
            self->sub_start_timer = g_timeout_add(UICC_SUBSCRIPTION_START_MS,
                binder_sim_card_sub_start_timeout, self);
        }
        binder_sim_card_update_app(self);

        g_signal_emit(self, binder_sim_card_signals
            [SIGNAL_STATUS_RECEIVED], 0);

        if (diff & BINDER_SIMCARD_STATUS_CHANGED) {
            DBG("status changed");
            g_signal_emit(self, binder_sim_card_signals
                [SIGNAL_STATUS_CHANGED], 0);
        }
        if (diff & BINDER_SIMCARD_STATE_CHANGED) {
            DBG("state changed");
            g_signal_emit(self, binder_sim_card_signals
                [SIGNAL_STATE_CHANGED], 0);
        }
        binder_sim_card_status_free(old_status);
    } else {
        binder_sim_card_update_app(self);
        binder_sim_card_status_free(status);
        g_signal_emit(self, binder_sim_card_signals
            [SIGNAL_STATUS_RECEIVED], 0);
    }
}

static
BinderSimCardStatus*
binder_sim_card_status_new(
    const RadioCardStatus* radio_status)
{
    const guint num_apps = radio_status->apps.count;
    BinderSimCardStatus* status = g_malloc0(sizeof(BinderSimCardStatus) +
        num_apps * sizeof(BinderSimCardApp));

    DBG("card_state=%d, universal_pin_state=%d, gsm_umts_index=%d, "
        "ims_index=%d, num_apps=%d", radio_status->cardState,
        radio_status->universalPinState,
        radio_status->gsmUmtsSubscriptionAppIndex,
        radio_status->imsSubscriptionAppIndex, num_apps);

    status->card_state = radio_status->cardState;
    status->pin_state = radio_status->universalPinState;
    status->gsm_umts_index = radio_status->gsmUmtsSubscriptionAppIndex;
    status->ims_index = radio_status->imsSubscriptionAppIndex;

    if ((status->num_apps = num_apps) > 0) {
        const RadioAppStatus* radio_apps = radio_status->apps.data.ptr;
        guint i;

        status->apps = (BinderSimCardApp*)(status + 1);
        for (i = 0; i < num_apps; i++) {
            const RadioAppStatus* radio_app = radio_apps + i;
            BinderSimCardApp* app = status->apps + i;

            app->app_type = radio_app->appType;
            app->app_state = radio_app->appState;
            app->perso_substate = radio_app->persoSubstate;
            app->pin_replaced = radio_app->pinReplaced;
            app->pin1_state = radio_app->pin1;
            app->pin2_state = radio_app->pin2;
            app->aid = g_strdup(radio_app->aid.data.str);
            app->label = g_strdup(radio_app->label.data.str);

            DBG("app[%d]: type=%d, state=%d, perso_substate=%d, aid_ptr=%s, "
                "label=%s, pin1_replaced=%d, pin1=%d, pin2=%d", i,
                app->app_type, app->app_state, app->perso_substate,
                app->aid, app->label, app->pin_replaced, app->pin1_state,
                app->pin2_state);
        }
    }

    return status;
}

static
void
binder_sim_card_status_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCardObject* self = THIS(user_data);

    GASSERT(self->status_req);
    radio_request_unref(self->status_req);
    self->status_req = NULL;

    if (status == RADIO_TX_STATUS_OK && error == RADIO_ERROR_NONE) {
        const RadioCardStatus* status_1_0;
        const RadioCardStatus_1_2* status_1_2;
        const RadioCardStatus_1_4* status_1_4;
        BinderSimCardStatus* status = NULL;
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        switch (resp) {
        case RADIO_RESP_GET_ICC_CARD_STATUS:
            status_1_0 = gbinder_reader_read_hidl_struct(&reader,
                RadioCardStatus);
            if (status_1_0) {
                status = binder_sim_card_status_new(status_1_0);
            }
            break;
        case RADIO_RESP_GET_ICC_CARD_STATUS_1_2:
            status_1_2 = gbinder_reader_read_hidl_struct(&reader,
                RadioCardStatus_1_2);
            if (status_1_2) {
                status = binder_sim_card_status_new(&status_1_2->base);
            }
            break;
        case RADIO_RESP_GET_ICC_CARD_STATUS_RESPONSE_1_4:
            status_1_4 = gbinder_reader_read_hidl_struct(&reader,
                RadioCardStatus_1_4);
            if (status_1_4) {
                status = binder_sim_card_status_new(&status_1_4->base);
            }
            break;
        default:
            ofono_warn("Unexpected getIccCardStatus response %u", resp);
        }

        if (status) {
            binder_sim_card_update_status(self, status);
        }
    }
    binder_sim_card_tx_check(self);
}

static
void
binder_sim_card_get_status(
    BinderSimCardObject* self)
{
    if (self->status_req) {
        /* Retry right away, don't wait for retry timeout to expire */
        radio_request_retry(self->status_req);
    } else {
        self->status_req = radio_request_new2(self->g,
            RADIO_REQ_GET_ICC_CARD_STATUS, NULL,
            binder_sim_card_status_cb, NULL, self);

        /*
         * Start the transaction to not allow any other requests to
         * interfere with SIM status query.
         */
        binder_sim_card_tx_start(self);
        radio_request_set_retry(self->status_req, BINDER_RETRY_MS, -1);
        radio_request_submit(self->status_req);
    }
}

static
void
binder_sim_card_update_sim_io_active(
    BinderSimCardObject* self)
{
    /* SIM I/O is considered active for certain period of time after
     * the last request has completed. That's because SIM_IO requests
     * are usually submitted in large quantities and quick succession.
     * Some modems don't like being bothered while they are doing SIM I/O
     * and some time after that too. That sucks but what else can we
     * do about it? */
    BinderSimCard* card = &self->card;
    const gboolean active = self->sim_io_idle_id ||
        g_hash_table_size(self->sim_io_pending);

    if (card->sim_io_active != active) {
        card->sim_io_active = active;
        DBG("SIM I/O for slot %u is %sactive", card->slot, active ? "" : "in");
        g_signal_emit(self, binder_sim_card_signals
            [SIGNAL_SIM_IO_ACTIVE_CHANGED], 0);
    }
}

static
gboolean
binder_sim_card_sim_io_idle_cb(
    gpointer user_data)
{
    BinderSimCardObject* self = THIS(user_data);

    if (++(self->sim_io_idle_count) >= SIM_IO_IDLE_LOOPS) {
        self->sim_io_idle_id = 0;
        self->sim_io_idle_count = 0;
        binder_sim_card_update_sim_io_active(self);
        return G_SOURCE_REMOVE;
    } else {
        return G_SOURCE_CONTINUE;
    }
}

static
void
binder_sim_card_status_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_sim_card_get_status(THIS(user_data));
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderSimCard*
binder_sim_card_new(
    RadioClient* client,
    guint slot)
{
    BinderSimCardObject* self = g_object_new(THIS_TYPE, NULL);
    BinderSimCard *card = &self->card;

    DBG("%u", slot);
    card->slot = slot;
    self->g = radio_request_group_new(client); /* Keeps ref to client */

    self->event_id[EVENT_SIM_STATUS_CHANGED] =
        radio_client_add_indication_handler(client,
            RADIO_IND_SIM_STATUS_CHANGED,
            binder_sim_card_status_changed, self);
    self->event_id[EVENT_UICC_SUBSCRIPTION_STATUS_CHANGED] =
        radio_client_add_indication_handler(client,
            RADIO_IND_SUBSCRIPTION_STATUS_CHANGED,
            binder_sim_card_status_changed, self);
    binder_sim_card_get_status(self);
    return card;
}

BinderSimCard*
binder_sim_card_ref(
    BinderSimCard* card)
{
    if (G_LIKELY(card)) {
        g_object_ref(THIS(card));
    }
    return card;
}

void
binder_sim_card_unref(
    BinderSimCard* card)
{
    if (G_LIKELY(card)) {
        g_object_unref(THIS(card));
    }
}

void
binder_sim_card_reset(
    BinderSimCard* card)
{
    if (G_LIKELY(card)) {
        BinderSimCardObject* self = binder_sim_card_cast(card);
        BinderSimCardStatus* status = g_new0(BinderSimCardStatus, 1);

        /* Simulate removal and re-submit the SIM status query */
        status->card_state = RADIO_CARD_STATE_ABSENT;
        status->gsm_umts_index = -1;
        status->ims_index = -1;
        binder_sim_card_update_status(self, status);
        binder_sim_card_get_status(self);
    }
}

void
binder_sim_card_request_status(
    BinderSimCard* card)
{
    if (G_LIKELY(card)) {
        binder_sim_card_get_status(binder_sim_card_cast(card));
    }
}

void
binder_sim_card_sim_io_started(
    BinderSimCard* card,
    gpointer key)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    if (G_LIKELY(self) && G_LIKELY(key)) {
        g_hash_table_insert(self->sim_io_pending, key, key);
        if (self->sim_io_idle_id) {
            g_source_remove(self->sim_io_idle_id);
            self->sim_io_idle_id = 0;
            self->sim_io_idle_count = 0;
        }
        binder_sim_card_update_sim_io_active(self);
    }
}

void
binder_sim_card_sim_io_finished(
    BinderSimCard* card,
    gpointer key)
{
    if (G_LIKELY(card) && G_LIKELY(key)) {
        BinderSimCardObject* self = binder_sim_card_cast(card);

        if (g_hash_table_remove(self->sim_io_pending, key) &&
            g_hash_table_size(self->sim_io_pending) == 0) {
            /* Reset the idle loop count */
            if (self->sim_io_idle_id) {
                g_source_remove(self->sim_io_idle_id);
                self->sim_io_idle_count = 0;
            }
            self->sim_io_idle_id = g_idle_add(binder_sim_card_sim_io_idle_cb,
                self);
        }
        binder_sim_card_update_sim_io_active(self);
    }
}

gboolean
binder_sim_card_ready(
    BinderSimCard* card)
{
    return card && card->app &&
        ((card->app->app_state == RADIO_APP_STATE_READY) ||
         (card->app->app_state == RADIO_APP_STATE_SUBSCRIPTION_PERSO &&
          card->app->perso_substate == RADIO_PERSO_SUBSTATE_READY));
}

gulong
binder_sim_card_add_status_received_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_STATUS_RECEIVED_NAME, G_CALLBACK(fn), user_data) : 0;
}

gulong
binder_sim_card_add_status_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
       SIGNAL_STATUS_CHANGED_NAME, G_CALLBACK(fn), user_data) : 0;
}

gulong
binder_sim_card_add_state_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_STATE_CHANGED_NAME, G_CALLBACK(fn), user_data) : 0;
}

gulong
binder_sim_card_add_app_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_APP_CHANGED_NAME, G_CALLBACK(fn), user_data) : 0;
}

gulong
binder_sim_card_add_sim_io_active_changed_handler(
    BinderSimCard* card,
    BinderSimCardFunc fn,
    void* user_data)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    return (G_LIKELY(self) && G_LIKELY(fn)) ? g_signal_connect(self,
        SIGNAL_SIM_IO_ACTIVE_CHANGED_NAME, G_CALLBACK(fn), user_data) : 0;
}

void
binder_sim_card_remove_handler(
    BinderSimCard* card,
    gulong id)
{
    BinderSimCardObject* self = binder_sim_card_cast(card);

    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_signal_handler_disconnect(self, id);
    }
}

void
binder_sim_card_remove_handlers(
    BinderSimCard* card,
    gulong* ids,
    int n)
{
    gutil_disconnect_handlers(binder_sim_card_cast(card), ids, n);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_sim_card_init(
    BinderSimCardObject* self)
{
    self->sim_io_pending = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static
void
binder_sim_card_finalize(
    GObject* object)
{
    BinderSimCardObject* self = THIS(object);
    BinderSimCard* card = &self->card;

    if (self->sim_io_idle_id) {
        g_source_remove(self->sim_io_idle_id);
    }
    if (self->sub_start_timer) {
        g_source_remove(self->sub_start_timer);
    }
    g_hash_table_destroy(self->sim_io_pending);

    radio_request_drop(self->status_req);
    radio_request_drop(self->sub_req);

    radio_client_remove_all_handlers(self->g->client, self->event_id);
    radio_request_group_unblock(self->g);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    binder_sim_card_status_free(card->status);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_sim_card_class_init(
    BinderSimCardObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_sim_card_finalize;
    NEW_SIGNAL_(klass,STATUS_RECEIVED);
    NEW_SIGNAL(klass,STATUS);
    NEW_SIGNAL(klass,STATE);
    NEW_SIGNAL(klass,APP);
    NEW_SIGNAL(klass,SIM_IO_ACTIVE);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
