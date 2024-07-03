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

#include "binder_logger.h"
#include "binder_util.h"

#include <radio_config.h>
#include <radio_instance.h>
#include <radio_util.h>

#include <gbinder_local_request.h>
#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_log.h>

enum binder_logger_events {
    EVENT_REQ,
    EVENT_RESP,
    EVENT_IND,
    EVENT_ACK,
    EVENT_COUNT
};

typedef struct binder_logger_callbacks {
    const char* (*req_name)(gpointer object, guint32 code);
    const char* (*resp_name)(gpointer object, guint32 code);
    const char* (*ind_name)(gpointer object, guint32 code);
    gsize (*rpc_header_size)(gpointer object, guint32 code);
    void (*drop_object)(BinderLogger* logger);
} BinderLoggerCallbacks;

struct binder_logger {
    const BinderLoggerCallbacks* cb;
    gpointer object;
    gulong event_id[EVENT_COUNT];
    char* prefix;
};

#define CONFIG_PREFIX "config"

GLogModule binder_logger_module = {
    .max_level = GLOG_LEVEL_VERBOSE,
    .level = GLOG_LEVEL_VERBOSE,
    .flags = GLOG_FLAG_HIDE_NAME
};

static GLogModule binder_logger_dump_module = {
    .parent = &binder_logger_module,
    .max_level = GLOG_LEVEL_VERBOSE,
    .level = GLOG_LEVEL_INHERIT,
    .flags = GLOG_FLAG_HIDE_NAME
};

static
void
binder_logger_trace_req(
    BinderLogger* logger,
    guint code,
    GBinderLocalRequest* args)
{
    const BinderLoggerCallbacks* cb = logger->cb;
    static const GLogModule* log = &binder_logger_module;
    const gsize header_size = cb->rpc_header_size(logger->object, code);
    const char* name = cb->req_name(logger->object, code);
    GBinderWriter writer;
    const guint8* data;
    guint32 serial;
    gsize size;

    /* Use writer API to fetch the raw data and extract the serial */
    gbinder_local_request_init_writer(args, &writer);
    data = gbinder_writer_get_data(&writer, &size);
    serial = (size >= header_size + 4) ? *(guint32*)(data + header_size) : 0;

    if (serial) {
        gutil_log(log, GLOG_LEVEL_VERBOSE, "%s< [%08x] %u %s",
            logger->prefix, serial, code, name ? name : "");
    } else {
        gutil_log(log, GLOG_LEVEL_VERBOSE, "%s< %u %s",
            logger->prefix, code, name ? name : "");
    }
}

static
void
binder_logger_trace_resp(
    BinderLogger* logger,
    guint code,
    const RadioResponseInfo* info,
    const GBinderReader* args)
{
    static const GLogModule* log = &binder_logger_module;
    const BinderLoggerCallbacks* cb = logger->cb;
    const char* name = cb->resp_name(logger->object, code);
    const char* error = (info->error == RADIO_ERROR_NONE) ? NULL :
        binder_radio_error_string(info->error);
    const char* arg1 = name ? name : error;
    const char* arg2 = name ? error : NULL;

    if (arg2) {
        gutil_log(log, GLOG_LEVEL_VERBOSE, "%s> [%08x] %u %s %s",
            logger->prefix, info->serial, code, arg1, arg2);
    } else if (arg1) {
        gutil_log(log, GLOG_LEVEL_VERBOSE, "%s> [%08x] %u %s",
            logger->prefix, info->serial, code, arg1);
    } else {
        gutil_log(log, GLOG_LEVEL_VERBOSE, "%s> [%08x] %u",
            logger->prefix, info->serial, code);
    }
}

static
void
binder_logger_trace_ind(
    BinderLogger* logger,
    RADIO_IND code,
    const GBinderReader* args)
{
    const BinderLoggerCallbacks* cb = logger->cb;
    const char* name = cb->ind_name(logger->object, code);
    static const GLogModule* log = &binder_logger_module;

    gutil_log(log, GLOG_LEVEL_VERBOSE, "%s> %u %s",
        logger->prefix, code, name ? name : "");
}

static
void
binder_logger_dump_req(
    GBinderLocalRequest* args)
{
    GBinderWriter writer;
    const guint8* data;
    gsize size;

    /* Use writer API to fetch the raw data */
    gbinder_local_request_init_writer(args, &writer);
    data = gbinder_writer_get_data(&writer, &size);
    gutil_log_dump(&binder_logger_dump_module, GLOG_LEVEL_VERBOSE, "  ",
        data, size);
}

static
void
binder_logger_dump_reader(
    const GBinderReader* reader)
{
    gsize size;
    const guint8* data = gbinder_reader_get_data(reader, &size);

    gutil_log_dump(&binder_logger_dump_module, GLOG_LEVEL_VERBOSE, "  ",
        data, size);
}

/*==========================================================================*
 * RadioInstance implementation
 *==========================================================================*/

static
void
binder_logger_radio_trace_req_cb(
    RadioInstance* radio,
    RADIO_REQ code,
    GBinderLocalRequest* args,
    gpointer user_data)
{
    binder_logger_trace_req((BinderLogger*)user_data, code, args);
}

static
void
binder_logger_radio_trace_resp_cb(
    RadioInstance* radio,
    RADIO_RESP code,
    const RadioResponseInfo* info,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_trace_resp((BinderLogger*)user_data, code, info, args);
}

static
void
binder_logger_radio_trace_ind_cb(
    RadioInstance* radio,
    RADIO_IND code,
    RADIO_IND_TYPE type,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_trace_ind((BinderLogger*)user_data, code, args);
}

static
void
binder_logger_radio_trace_ack_cb(
    RadioInstance* radio,
    guint32 serial,
    gpointer user_data)
{
    BinderLogger* logger = user_data;

    gutil_log(&binder_logger_module, GLOG_LEVEL_VERBOSE, "%s> [%08x] "
        "acknowledgeRequest", logger->prefix, serial);
}

static
void
binder_logger_radio_dump_req_cb(
    RadioInstance* radio,
    RADIO_REQ code,
    GBinderLocalRequest* args,
    gpointer user_data)
{
    binder_logger_dump_req(args);
}

static
void
binder_logger_radio_dump_resp_cb(
    RadioInstance* radio,
    RADIO_RESP code,
    const RadioResponseInfo* info,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_dump_reader(args);
}

static
void
binder_logger_radio_dump_ind_cb(
    RadioInstance* radio,
    RADIO_IND code,
    RADIO_IND_TYPE type,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_dump_reader(args);
}

static
const char*
binder_logger_radio_req_name(
    gpointer object,
    guint32 code)
{
    return radio_req_name2(object, code);
}

static
const char*
binder_logger_radio_resp_name(
    gpointer object,
    guint32 code)
{
    return radio_resp_name2(object, code);
}

static
const char*
binder_logger_radio_ind_name(
    gpointer object,
    guint32 code)
{
    return radio_ind_name2(object, code);
}

static
gsize
binder_logger_radio_rpc_header_size(
    gpointer object,
    guint32 code)
{
    return radio_instance_rpc_header_size(object, code);
}

static
void
binder_logger_radio_drop_object(
    BinderLogger* logger)
{
    radio_instance_remove_all_handlers(logger->object, logger->event_id);
    radio_instance_unref(logger->object);
}

static
BinderLogger*
binder_logger_radio_new(
    RadioInstance* radio,
    const char* prefix,
    RADIO_INSTANCE_PRIORITY pri,
    RadioRequestObserverFunc req_cb,
    RadioResponseObserverFunc resp_cb,
    RadioIndicationObserverFunc ind_cb,
    RadioAckFunc ack_cb)
{
    static BinderLoggerCallbacks binder_logger_radio_callbacks = {
        .req_name = binder_logger_radio_req_name,
        .resp_name = binder_logger_radio_resp_name,
        .ind_name = binder_logger_radio_ind_name,
        .rpc_header_size = binder_logger_radio_rpc_header_size,
        .drop_object = binder_logger_radio_drop_object
    };

    if (radio) {
        BinderLogger* logger = g_new0(BinderLogger, 1);

        logger->cb = &binder_logger_radio_callbacks;
        logger->prefix = binder_dup_prefix(prefix);
        logger->object = radio_instance_ref(radio);
        logger->event_id[EVENT_REQ] =
            radio_instance_add_request_observer_with_priority(radio, pri,
                RADIO_REQ_ANY, req_cb, logger);
        logger->event_id[EVENT_RESP] =
            radio_instance_add_response_observer_with_priority(radio, pri,
                RADIO_RESP_ANY, resp_cb, logger);
        logger->event_id[EVENT_IND] =
            radio_instance_add_indication_observer_with_priority(radio, pri,
                RADIO_IND_ANY, ind_cb, logger);
        logger->event_id[EVENT_ACK] = radio_instance_add_ack_handler(radio,
            ack_cb, logger);
        return logger;
    } else {
        return NULL;
    }
}

/*==========================================================================*
 * RadioConfig implementation
 *==========================================================================*/

static
void
binder_logger_config_trace_req_cb(
    RadioConfig* config,
    RADIO_CONFIG_REQ code,
    GBinderLocalRequest* args,
    gpointer user_data)
{
    binder_logger_trace_req((BinderLogger*)user_data, code, args);
}

static
void
binder_logger_config_trace_resp_cb(
    RadioConfig* config,
    RADIO_CONFIG_RESP code,
    const RadioResponseInfo* info,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_trace_resp((BinderLogger*)user_data, code, info, args);
}

static
void
binder_logger_config_trace_ind_cb(
    RadioConfig* config,
    RADIO_CONFIG_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_trace_ind((BinderLogger*)user_data, code, args);
}

static
void
binder_logger_config_dump_req_cb(
    RadioConfig* config,
    RADIO_CONFIG_REQ code,
    GBinderLocalRequest* args,
    gpointer user_data)
{
    binder_logger_dump_req(args);
}

static
void
binder_logger_config_dump_resp_cb(
    RadioConfig* config,
    RADIO_CONFIG_RESP code,
    const RadioResponseInfo* info,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_dump_reader(args);
}

static
void
binder_logger_config_dump_ind_cb(
    RadioConfig* config,
    RADIO_CONFIG_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    binder_logger_dump_reader(args);
}

static
const char*
binder_logger_config_req_name(
    gpointer object,
    guint32 code)
{
    return radio_config_req_name(object, code);
}

static
const char*
binder_logger_config_resp_name(
    gpointer object,
    guint32 code)
{
    return radio_config_resp_name(object, code);
}

static
const char*
binder_logger_config_ind_name(
    gpointer object,
    guint32 code)
{
    return radio_config_ind_name(object, code);
}

static
gsize
binder_logger_config_rpc_header_size(
    gpointer object,
    guint32 code)
{
    return radio_config_rpc_header_size(object, code);
}

static
void
binder_logger_config_drop_object(
    BinderLogger* logger)
{
    radio_config_remove_all_handlers(logger->object, logger->event_id);
    radio_config_unref(logger->object);
}

static
BinderLogger*
binder_logger_config_new(
    RadioConfig* config,
    const char* prefix,
    RADIO_INSTANCE_PRIORITY pri,
    RadioConfigRequestObserverFunc req_cb,
    RadioConfigResponseObserverFunc resp_cb,
    RadioConfigIndicationObserverFunc ind_cb)
{
    static BinderLoggerCallbacks binder_logger_config_callbacks = {
        .req_name = binder_logger_config_req_name,
        .resp_name = binder_logger_config_resp_name,
        .ind_name = binder_logger_config_ind_name,
        .rpc_header_size = binder_logger_config_rpc_header_size,
        .drop_object = binder_logger_config_drop_object
    };

    if (config) {
        BinderLogger* logger = g_new0(BinderLogger, 1);

        logger->cb = &binder_logger_config_callbacks;
        logger->prefix = binder_dup_prefix(prefix);
        logger->object = radio_config_ref(config);
        logger->event_id[EVENT_REQ] =
            radio_config_add_request_observer_with_priority(config, pri,
                RADIO_REQ_ANY, req_cb, logger);
        logger->event_id[EVENT_RESP] =
            radio_config_add_response_observer_with_priority(config, pri,
                RADIO_RESP_ANY, resp_cb, logger);
        logger->event_id[EVENT_IND] =
            radio_config_add_indication_observer_with_priority(config, pri,
                RADIO_IND_ANY, ind_cb, logger);
        return logger;
    } else {
        return NULL;
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

BinderLogger*
binder_logger_new_radio_trace(
    RadioInstance* radio,
    const char* prefix)
{
    return binder_logger_radio_new(radio, prefix,
        RADIO_INSTANCE_PRIORITY_HIGHEST, binder_logger_radio_trace_req_cb,
        binder_logger_radio_trace_resp_cb, binder_logger_radio_trace_ind_cb,
        binder_logger_radio_trace_ack_cb);
}

BinderLogger*
binder_logger_new_radio_dump(
    RadioInstance* radio,
    const char* prefix)
{
    return binder_logger_radio_new(radio, prefix,
        RADIO_INSTANCE_PRIORITY_HIGHEST - 1, binder_logger_radio_dump_req_cb,
        binder_logger_radio_dump_resp_cb, binder_logger_radio_dump_ind_cb,
        NULL);
}

BinderLogger*
binder_logger_new_config_trace(
    RadioConfig* config)
{
    return binder_logger_config_new(config, CONFIG_PREFIX,
        RADIO_INSTANCE_PRIORITY_HIGHEST, binder_logger_config_trace_req_cb,
        binder_logger_config_trace_resp_cb, binder_logger_config_trace_ind_cb);
}

BinderLogger*
binder_logger_new_config_dump(
    RadioConfig* config)
{
    return binder_logger_config_new(config, CONFIG_PREFIX,
        RADIO_INSTANCE_PRIORITY_HIGHEST - 1, binder_logger_config_dump_req_cb,
        binder_logger_config_dump_resp_cb, binder_logger_config_dump_ind_cb);
}

void
binder_logger_free(
    BinderLogger* logger)
{
    if (logger) {
        logger->cb->drop_object(logger);
        g_free(logger->prefix);
        g_free(logger);
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
