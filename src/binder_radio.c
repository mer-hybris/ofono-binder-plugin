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

#include "binder_base.h"
#include "binder_log.h"
#include "binder_radio.h"
#include "binder_util.h"

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

/*
 * Object states:
 *
 * 1. Idle (!pending && !retry)
 * 2. Power on/off request pending (pending)
 * 3. Power on retry has been scheduled (retry)
 */
typedef struct binder_radio_object {
    BinderBase base;
    BinderRadio pub;
    RadioClient* client;
    RadioRequestGroup* g;
    gulong state_event_id;
    char* log_prefix;
    GHashTable* req_table;
    RadioRequest* pending_req;
    guint retry_id;
    guint state_changed_while_request_pending;
    RADIO_STATE last_known_state;
    gboolean power_cycle;
    gboolean next_state_valid;
    gboolean next_state;
} BinderRadioObject;

#define POWER_RETRY_SECS (1)

typedef BinderBaseClass BinderRadioObjectClass;
GType binder_radio_object_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderRadioObject, binder_radio_object, BINDER_TYPE_BASE)
#define PARENT_CLASS binder_radio_object_parent_class
#define THIS_TYPE binder_radio_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, BinderRadioObject)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

/* Assumptions */
BINDER_BASE_ASSERT_COUNT(BINDER_RADIO_PROPERTY_COUNT);

static
void
binder_radio_submit_power_request(
    BinderRadioObject* self,
    gboolean on);

static inline BinderRadioObject* binder_radio_object_cast(BinderRadio* net)
    { return net ? THIS(G_CAST(net, BinderRadioObject, pub)) : NULL; }
static inline void binder_radio_object_ref(BinderRadioObject* self)
    { g_object_ref(self); }
static inline void binder_radio_object_unref(BinderRadioObject* self)
    { g_object_unref(self); }
static inline gboolean binder_radio_state_off(RADIO_STATE state)
    { return state == RADIO_STATE_OFF; }
static inline gboolean binder_radio_state_on(RADIO_STATE state)
    { return !binder_radio_state_off(state); }

static
gboolean
binder_radio_power_should_be_on(
    BinderRadioObject* self)
{
    BinderRadio* radio = &self->pub;

    return (radio->online || g_hash_table_size(self->req_table) > 0) &&
        !self->power_cycle;
}

static
gboolean
binder_radio_power_request_retry_cb(
    gpointer user_data)
{
    BinderRadioObject* self = THIS(user_data);

    DBG_(self, "");
    GASSERT(self->retry_id);
    self->retry_id = 0;
    binder_radio_submit_power_request(self,
        binder_radio_power_should_be_on(self));

    return G_SOURCE_REMOVE;
}

static
void
binder_radio_cancel_retry(
    BinderRadioObject* self)
{
    if (self->retry_id) {
        DBG_(self, "retry cancelled");
        g_source_remove(self->retry_id);
        self->retry_id = 0;
    }
}

static
void
binder_radio_check_state(
    BinderRadioObject* self)
{
    BinderRadio* radio = &self->pub;

    if (!self->pending_req) {
        const gboolean should_be_on = binder_radio_power_should_be_on(self);

        if (binder_radio_state_on(self->last_known_state) == should_be_on) {
            /* All is good, cancel pending retry if there is one */
            binder_radio_cancel_retry(self);
        } else if (self->state_changed_while_request_pending) {
            /* Hmm... BINDER's reaction was inadequate, repeat */
            binder_radio_submit_power_request(self, should_be_on);
        } else if (!self->retry_id) {
            /* There has been no reaction so far, wait a bit */
            DBG_(self, "retry scheduled");
            self->retry_id = g_timeout_add_seconds(POWER_RETRY_SECS,
                binder_radio_power_request_retry_cb, self);
        }
    }

    /* Don't update public state while something is pending */
    if (!self->pending_req && !self->retry_id &&
        radio->state != self->last_known_state) {
        DBG_(self, "%s -> %s", binder_radio_state_string(radio->state),
             binder_radio_state_string(self->last_known_state));
        radio->state = self->last_known_state;
        binder_base_emit_property_change(&self->base,
            BINDER_RADIO_PROPERTY_STATE);
    }
}

static
void
binder_radio_power_request_done(
    BinderRadioObject* self)
{
    GASSERT(!self->pending_req);
    if (self->next_state_valid) {
        binder_radio_submit_power_request(self, self->next_state);
    } else {
        binder_radio_check_state(self);
    }
}

static
void
binder_radio_power_request_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    BinderRadioObject* self = THIS(user_data);
    const RADIO_INTERFACE iface = radio_client_interface(self->client);
    const RADIO_RESP code = (iface >= RADIO_INTERFACE_1_5) ?
                            RADIO_RESP_SET_RADIO_POWER_1_5 :
                            RADIO_RESP_SET_RADIO_POWER;

    GASSERT(self->pending_req == req);
    radio_request_unref(self->pending_req);
    self->pending_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp != code) {
            ofono_error("Unexpected setRadioPower response %d", resp);
        } else if (error != RADIO_ERROR_NONE) {
            ofono_error("Power request failed: %s",
                binder_radio_error_string(error));
        }
    } else {
        ofono_error("Power request failed");
    }

    binder_radio_power_request_done(self);
}

static
void
binder_radio_submit_power_request(
    BinderRadioObject* self,
    gboolean on)
{
    /* setRadioPower(int32 serial, bool on)
     * setRadioPower_1_5(int32 serial, bool on, bool forEmergencyCall,
     *     bool preferredForEmergencyCall)
     */
    GBinderWriter writer;
    const RADIO_INTERFACE iface = radio_client_interface(self->client);
    const RADIO_REQ code = (iface >= RADIO_INTERFACE_1_5) ?
                           RADIO_REQ_SET_RADIO_POWER_1_5 :
                           RADIO_REQ_SET_RADIO_POWER;
    RadioRequest* req = radio_request_new(self->client,
        code, &writer,
        binder_radio_power_request_cb, NULL, self);

    gbinder_writer_append_bool(&writer, on);
    if (iface >= RADIO_INTERFACE_1_5) {
        gbinder_writer_append_bool(&writer, FALSE);
        gbinder_writer_append_bool(&writer, FALSE);
    }

    self->next_state_valid = FALSE;
    self->next_state = on;
    self->state_changed_while_request_pending = 0;
    binder_radio_cancel_retry(self);

    GASSERT(!self->pending_req);
    radio_request_set_blocking(req, TRUE);
    if (radio_request_submit(req)) {
        self->pending_req = req; /* Keep the ref */
    } else {
        radio_request_unref(req);
    }
}

static
void
binder_radio_power_request(
    BinderRadioObject* self,
    gboolean on,
    gboolean allow_repeat)
{
    const char* on_off = on ? "on" : "off";

    if (self->pending_req) {
        if (allow_repeat || self->next_state != on) {
            /* Wait for the pending request to complete */
            self->next_state_valid = TRUE;
            self->next_state = on;
            DBG_(self, "%s (queued)", on_off);
        } else {
            DBG_(self, "%s (ignored)", on_off);
        }
    } else {
        if (binder_radio_state_on(self->last_known_state) == on) {
            DBG_(self, "%s (already)", on_off);
            binder_radio_check_state(self);
        } else {
            DBG_(self, "%s", on_off);
            binder_radio_submit_power_request(self, on);
        }
    }
}

static
void
binder_radio_state_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderRadioObject* self = THIS(user_data);
    RADIO_STATE radio_state = RADIO_STATE_UNAVAILABLE;
    GBinderReader reader;
    gint32 tmp;

    /* radioStateChanged(RadioIndicationType, RadioState radioState); */
   gbinder_reader_copy(&reader, args);
   if (gbinder_reader_read_int32(&reader, &tmp)) {
       radio_state = tmp;
   } else {
       ofono_error("Failed to parse radioStateChanged payload");
   }

   if (radio_state != RADIO_STATE_UNAVAILABLE) {
       DBG_(self, "%s", binder_radio_state_string(radio_state));

       GASSERT(!self->pending_req || !self->retry_id);
       if (self->power_cycle && binder_radio_state_off(radio_state)) {
           DBG_(self, "switched off for power cycle");
           self->power_cycle = FALSE;
       }

       self->last_known_state = radio_state;

       if (self->pending_req) {
           if (binder_radio_state_on(radio_state) ==
               binder_radio_power_should_be_on(self)) {
               DBG_(self, "dropping pending request");
               /*
                * All right, the modem has switched to the
                * desired state, drop the request.
                */
               radio_request_drop(self->pending_req);
               self->pending_req = NULL;
               binder_radio_power_request_done(self);

               /* We are done */
               return;
           } else {
               /* Something weird is going on */
               self->state_changed_while_request_pending++;
           }
       }

       binder_radio_check_state(self);
   }
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderRadio*
binder_radio_new(
    RadioClient* client,
    const char* log_prefix)
{
    BinderRadioObject* self = g_object_new(THIS_TYPE, NULL);
    BinderRadio* radio = &self->pub;

    self->client = radio_client_ref(client);
    self->g = radio_request_group_new(client);
    self->log_prefix = binder_dup_prefix(log_prefix);
    DBG_(self, "");

    self->state_event_id = radio_client_add_indication_handler(client,
        RADIO_IND_RADIO_STATE_CHANGED, binder_radio_state_changed, self);

    /*
     * Some modem adaptations like to receive power off request at startup
     * even if radio is already off. Make those happy.
     */
    binder_radio_submit_power_request(self, FALSE);
    return radio;
}

BinderRadio*
binder_radio_ref(
    BinderRadio* radio)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self)) {
        binder_radio_object_ref(self);
        return radio;
    } else {
        return NULL;
    }
}

void
binder_radio_unref(
    BinderRadio* radio)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self)) {
        binder_radio_object_unref(self);
    }
}

void
binder_radio_set_online(
    BinderRadio* radio,
    gboolean online)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self) && radio->online != online) {
        gboolean on, was_on = binder_radio_power_should_be_on(self);

        radio->online = online;
        on = binder_radio_power_should_be_on(self);
        if (was_on != on) {
            binder_radio_power_request(self, on, FALSE);
        }
        binder_base_emit_property_change(&self->base,
            BINDER_RADIO_PROPERTY_ONLINE);
    }
}

void
binder_radio_confirm_power_on(
    BinderRadio* radio)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self) && binder_radio_power_should_be_on(self)) {
        if (self->pending_req) {
            if (!self->next_state) {
                /* Wait for the pending request to complete */
                self->next_state_valid = TRUE;
                self->next_state = TRUE;
                DBG_(self, "on (queued)");
            }
        } else {
            DBG_(self, "on");
            binder_radio_submit_power_request(self, TRUE);
        }
    }
}

void
binder_radio_power_cycle(
    BinderRadio* radio)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self)) {
        if (binder_radio_state_off(self->last_known_state)) {
            DBG_(self, "power is already off");
            GASSERT(!self->power_cycle);
        } else if (self->power_cycle) {
            DBG_(self, "already in progress");
        } else {
            DBG_(self, "initiated");
            self->power_cycle = TRUE;
            if (!self->pending_req) {
                binder_radio_submit_power_request(self, FALSE);
            }
        }
    }
}

void
binder_radio_power_on(
    BinderRadio* radio,
    gpointer tag)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self)) {
        if (!g_hash_table_contains(self->req_table, tag)) {
            const gboolean was_on = binder_radio_power_should_be_on(self);

            DBG_(self, "%p", tag);
            g_hash_table_insert(self->req_table, tag, tag);
            if (!was_on && binder_radio_power_should_be_on(self)) {
                binder_radio_power_request(self, TRUE, FALSE);
            }
        }
    }
}

void
binder_radio_power_off(
    BinderRadio* radio,
    gpointer tag)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    if (G_LIKELY(self)) {
        if (g_hash_table_remove(self->req_table, tag)) {
            DBG_(self, "%p", tag);
            if (!binder_radio_power_should_be_on(self)) {
                /* The last one turns the lights off */
                binder_radio_power_request(self, FALSE, FALSE);
            }
        }
    }
}

gulong
binder_radio_add_property_handler(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    BinderRadioPropertyFunc callback,
    void* user_data)
{
    BinderRadioObject* self = binder_radio_object_cast(radio);

    return G_LIKELY(self) ? binder_base_add_property_handler(&self->base,
        property, G_CALLBACK(callback), user_data) : 0;
}

void
binder_radio_remove_handler(
    BinderRadio* radio,
    gulong id)
{
    if (G_LIKELY(id)) {
        BinderRadioObject* self = binder_radio_object_cast(radio);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
binder_radio_remove_handlers(
    BinderRadio* radio,
    gulong* ids,
    int count)
{
    gutil_disconnect_handlers(binder_radio_object_cast(radio), ids, count);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_radio_object_init(
    BinderRadioObject* self)
{
    self->req_table = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static
void
binder_radio_object_finalize(
    GObject* object)
{
    BinderRadioObject* self = THIS(object);

    DBG_(self, "");
    binder_radio_cancel_retry(self);

    radio_request_drop(self->pending_req);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);
    radio_client_remove_handler(self->client, self->state_event_id);
    radio_client_unref(self->client);

    g_hash_table_unref(self->req_table);
    g_free(self->log_prefix);

    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_radio_object_class_init(
    BinderRadioObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_radio_object_finalize;
    BINDER_BASE_CLASS(klass)->public_offset =
        G_STRUCT_OFFSET(BinderRadioObject, pub);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
