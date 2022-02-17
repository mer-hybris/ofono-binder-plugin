/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2022 Jolla Ltd.
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

#include "binder_base.h"
#include "binder_ims_reg.h"
#include "binder_log.h"
#include "binder_util.h"

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

enum binder_ims_events {
    EVENT_IMS_NETWORK_STATE_CHANGED,
    EVENT_COUNT
};

typedef struct binder_ims_reg_object {
    BinderBase base;
    BinderImsReg pub;
    RadioRequestGroup* g;
    char* log_prefix;
    gulong event_id[EVENT_COUNT];
} BinderImsRegObject;

typedef BinderBaseClass BinderImsRegObjectClass;
GType binder_ims_reg_object_get_type() BINDER_INTERNAL;
G_DEFINE_TYPE(BinderImsRegObject, binder_ims_reg_object, BINDER_TYPE_BASE)
#define PARENT_CLASS binder_ims_reg_object_parent_class
#define THIS_TYPE binder_ims_reg_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj,THIS_TYPE,BinderImsRegObject)

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

/* Assumptions */
BINDER_BASE_ASSERT_COUNT(BINDER_IMS_REG_PROPERTY_COUNT);

static inline BinderImsRegObject* binder_ims_reg_cast(BinderImsReg* ims)
    { return ims ? THIS(G_CAST(ims, BinderImsRegObject, pub)) : NULL; }
static inline void binder_ims_reg_object_ref(BinderImsRegObject* self)
    { g_object_ref(self); }
static inline void binder_ims_reg_object_unref(BinderImsRegObject* self)
    { g_object_unref(self); }

static
void
binder_ims_reg_query_done(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderImsRegObject* self = THIS(user_data);
    BinderBase* base = &self->base;
    BinderImsReg* ims = &self->pub;
    gboolean registered = FALSE;
    gint32 rat = RADIO_TECH_FAMILY_3GPP;

    if (status != RADIO_TX_STATUS_OK) {
        ofono_error("getImsRegistrationState failed");
    } else if (resp != RADIO_RESP_GET_IMS_REGISTRATION_STATE) {
        ofono_error("Unexpected getImsRegistrationState response %d", resp);
    } else if (error != RADIO_ERROR_NONE) {
        DBG_(self, "%s", binder_radio_error_string(error));
    } else {
        GBinderReader reader;

        /*
         * getImsRegistrationStateResponse(RadioResponseInfo info,
         * bool isRegistered, RadioTechnologyFamily ratFamily)
         */
        gbinder_reader_copy(&reader, args);
        if (gbinder_reader_read_bool(&reader, &registered) &&
            gbinder_reader_read_int32(&reader, &rat)) {
            DBG_(self, "registered: %d, rat: %d", registered, rat);
        } else {
            ofono_error("Failed to parse getImsRegistrationState response");
        }
    }

    /* Any error is treated as an unregistered state */
    if (ims->registered != registered) {
        ims->registered = registered;
        binder_base_queue_property_change(base,
            BINDER_IMS_REG_PROPERTY_REGISTERED);
    }
    if (ims->tech_family != rat) {
        ims->tech_family = rat;
        binder_base_queue_property_change(base,
            BINDER_IMS_REG_PROPERTY_TECH_FAMILY);
    }
    binder_base_emit_queued_signals(base);
}

static
void
binder_ims_reg_query(
    BinderImsRegObject* self)
{
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_GET_IMS_REGISTRATION_STATE, NULL,
        binder_ims_reg_query_done, NULL, self);

    radio_request_submit(req);
    radio_request_unref(req);
}

static
void
binder_ims_reg_state_changed(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderImsRegObject* self = THIS(user_data);

    DBG_(self, "");
    binder_ims_reg_query(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderImsReg*
binder_ims_reg_new(
    RadioClient* client,
    const char* log_prefix)
{
    BinderImsReg* ims = NULL;

    if (client) {
        BinderImsRegObject* self = g_object_new(THIS_TYPE, NULL);

        ims = &self->pub;
        self->log_prefix = binder_dup_prefix(log_prefix);
        self->g = radio_request_group_new(client); /* Keeps ref to client */
        DBG_(self, "");

        /* Register event handlers */
        self->event_id[EVENT_IMS_NETWORK_STATE_CHANGED] =
        radio_client_add_indication_handler(client,
            RADIO_IND_IMS_NETWORK_STATE_CHANGED,
            binder_ims_reg_state_changed, self);

        /* Query the initial state */
        binder_ims_reg_query(self);
    }
    return ims;
}

BinderImsReg*
binder_ims_reg_ref(
    BinderImsReg* ims)
{
    BinderImsRegObject* self = binder_ims_reg_cast(ims);

    if (G_LIKELY(self)) {
        binder_ims_reg_object_ref(self);
        return ims;
    } else {
        return NULL;
    }
}

void
binder_ims_reg_unref(
    BinderImsReg* ims)
{
    BinderImsRegObject* self = binder_ims_reg_cast(ims);

    if (G_LIKELY(self)) {
        binder_ims_reg_object_unref(self);
    }
}

gulong
binder_ims_reg_add_property_handler(
    BinderImsReg* ims,
    BINDER_IMS_REG_PROPERTY property,
    BinderImsRegPropertyFunc callback,
    void* user_data)
{
    BinderImsRegObject* self = binder_ims_reg_cast(ims);

    return G_LIKELY(self) ? binder_base_add_property_handler(&self->base,
        property, G_CALLBACK(callback), user_data) : 0;
}

void
binder_ims_reg_remove_handler(
    BinderImsReg* ims,
    gulong id)
{
    if (G_LIKELY(id)) {
        BinderImsRegObject* self = binder_ims_reg_cast(ims);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
binder_ims_reg_remove_handlers(
    BinderImsReg* ims,
    gulong* ids,
    int count)
{
    gutil_disconnect_handlers(binder_ims_reg_cast(ims), ids, count);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_ims_reg_object_init(
    BinderImsRegObject* self)
{
}

static
void
binder_ims_reg_object_finalize(
    GObject* object)
{
    BinderImsRegObject* self = THIS(object);
    RadioRequestGroup* g = self->g;

    radio_client_remove_all_handlers(g->client, self->event_id);
    radio_request_group_cancel(g);
    radio_request_group_unref(g);
    g_free(self->log_prefix);

    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_ims_reg_object_class_init(
    BinderImsRegObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_ims_reg_object_finalize;
    BINDER_BASE_CLASS(klass)->public_offset =
        G_STRUCT_OFFSET(BinderImsRegObject, pub);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
