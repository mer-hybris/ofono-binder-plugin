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

#include "binder_log.h"
#include "binder_modem.h"
#include "binder_sim.h"
#include "binder_sim_card.h"
#include "binder_util.h"

#include <ofono/log.h>
#include <ofono/misc.h>
#include <ofono/sim.h>
#include <ofono/watch.h>

#include <radio_client.h>
#include <radio_request.h>
#include <radio_request_group.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_macros.h>
#include <gutil_misc.h>

#define SIM_STATE_CHANGE_TIMEOUT_SECS (5)
#define FAC_LOCK_QUERY_TIMEOUT_SECS   (10)
#define FAC_LOCK_QUERY_RETRIES        (1)
#define SIM_IO_TIMEOUT_SECS           (20)

#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

/* Commands defined for TS 27.007 +CRSM */
#define CMD_READ_BINARY   176 /* 0xB0   */
#define CMD_READ_RECORD   178 /* 0xB2   */
#define CMD_GET_RESPONSE  192 /* 0xC0   */
#define CMD_UPDATE_BINARY 214 /* 0xD6   */
#define CMD_UPDATE_RECORD 220 /* 0xDC   */
#define CMD_STATUS        242 /* 0xF2   */
#define CMD_RETRIEVE_DATA 203 /* 0xCB   */
#define CMD_SET_DATA      219 /* 0xDB   */

/* FID/path of SIM/USIM root directory */
static const char ROOTMF[] = "3F00";

/* P2 coding (modes) for READ RECORD and UPDATE RECORD (see TS 102.221) */
#define MODE_SELECTED (0x00) /* Currently selected EF */
#define MODE_CURRENT  (0x04) /* P1='00' denotes the current record */
#define MODE_ABSOLUTE (0x04) /* The record number is given in P1 */
#define MODE_NEXT     (0x02) /* Next record */
#define MODE_PREVIOUS (0x03) /* Previous record */

enum binder_sim_card_event {
    SIM_CARD_STATUS_EVENT,
    SIM_CARD_APP_EVENT,
    SIM_CARD_EVENT_COUNT
};

enum binder_sim_io_event {
    IO_EVENT_SIM_REFRESH,
    IO_EVENT_COUNT
};

typedef struct binder_sim {
    struct ofono_sim* sim;
    struct ofono_watch* watch;
    enum ofono_sim_password_type ofono_passwd_state;
    BinderSimCard* card;
    RadioRequestGroup* g;
    RadioRequest* query_pin_retries_req;
    GList* pin_cbd_list;
    int retries[OFONO_SIM_PASSWORD_INVALID];
    gboolean empty_pin_query_allowed;
    gboolean inserted;
    guint idle_id; /* Used by register and SIM reset callbacks */
    guint list_apps_id;
    gulong card_event_id[SIM_CARD_EVENT_COUNT];
    gulong io_event_id[IO_EVENT_COUNT];
    gulong sim_state_watch_id;
    char *log_prefix;

    /* query_passwd_state context */
    ofono_sim_passwd_cb_t query_passwd_state_cb;
    void* query_passwd_state_cb_data;
    guint query_passwd_state_timeout_id;
    gulong query_passwd_state_sim_status_refresh_id;
} BinderSim;

typedef struct binder_sim_io_response {
    guint sw1, sw2;
    guint8* data;
    guint data_len;
} BinderSimIoResponse;

typedef struct binder_sim_cbd_io {
    BinderSim* self;
    BinderSimCard* card;
    union _ofono_sim_cb {
        ofono_sim_file_info_cb_t file_info;
        ofono_sim_read_cb_t read;
        ofono_sim_write_cb_t write;
        ofono_sim_imsi_cb_t imsi;
        ofono_query_facility_lock_cb_t query_facility_lock;
        ofono_sim_open_channel_cb_t open_channel;
        ofono_sim_close_channel_cb_t close_channel;
        BinderCallback ptr;
    } cb;
    gpointer data;
    gpointer req_id; /* Actually RadioRequest pointer (but not a ref) */
} BinderSimCbdIo;

typedef struct binder_sim_session_cbd {
    BinderSim* self;
    BinderSimCard* card;
    ofono_sim_logical_access_cb_t cb;
    gpointer data;
    int ref_count;
    int channel;
    int cla;
    gpointer req_id; /* Actually RadioRequest pointer (but not a ref) */
} BinderSimSessionCbData;

typedef struct binder_sim_pin_cbd {
    BinderSim* self;
    ofono_sim_lock_unlock_cb_t cb;
    gpointer data;
    BinderSimCard* card;
    enum ofono_sim_password_type passwd_type;
    RADIO_ERROR status;
    guint state_event_count;
    guint timeout_id;
    gulong card_status_id;
} BinderSimPinCbData;

typedef struct binder_sim_retry_query_cbd {
    BinderSim* self;
    ofono_sim_pin_retries_cb_t cb;
    void* data;
    guint query_index;
} BinderSimRetryQueryCbData;

typedef struct binder_sim_retry_query {
    const char* name;
    enum ofono_sim_password_type passwd_type;
    RADIO_REQ code;
    RadioRequest* (*new_req)(BinderSim* self, RADIO_REQ code,
        RadioRequestCompleteFunc complete, GDestroyNotify destroy,
        void* user_data);
} BinderSimRetryQuery;

/* TS 102.221 */
#define APP_TEMPLATE_TAG 0x61
#define APP_ID_TAG 0x4F

typedef struct binder_sim_list_apps {
    BinderSim* self;
    ofono_sim_list_apps_cb_t cb;
    void* data;
} BinderSimListApps;

static
void
binder_sim_query_retry_count_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data);

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderSim* binder_sim_get_data(struct ofono_sim* sim)
    { return ofono_sim_get_data(sim); }

static
BinderSimCbdIo*
binder_sim_cbd_io_new(
    BinderSim* self,
    BinderCallback cb,
    void* data)
{
    BinderSimCbdIo* cbd = g_slice_new0(BinderSimCbdIo);

    cbd->self = self;
    cbd->cb.ptr = cb;
    cbd->data = data;
    cbd->card = binder_sim_card_ref(self->card);
    return cbd;
}

static
void
binder_sim_cbd_io_free(
    gpointer data)
{
    BinderSimCbdIo* cbd = data;

    binder_sim_card_sim_io_finished(cbd->card, cbd->req_id);
    binder_sim_card_unref(cbd->card);
    gutil_slice_free(cbd);
}

static
gboolean
binder_sim_cbd_io_start(
    BinderSimCbdIo* cbd,
    RadioRequest* req)
{
    if (radio_request_submit(req)) {
        binder_sim_card_sim_io_started(cbd->card, cbd->req_id = req);
        return TRUE;
    }
    return FALSE;
}

static
BinderSimSessionCbData*
binder_sim_session_cbd_new(
    BinderSim* self,
    int channel,
    int cla,
    ofono_sim_logical_access_cb_t cb,
    void* data)
{
    BinderSimSessionCbData* cbd = g_slice_new0(BinderSimSessionCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    cbd->card = binder_sim_card_ref(self->card);
    cbd->channel = channel;
    cbd->cla = cla;
    cbd->ref_count = 1;
    return cbd;
}

static
void
binder_sim_session_cbd_unref(
    gpointer data)
{
    BinderSimSessionCbData* cbd = data;

    if (--(cbd->ref_count) < 1) {
        binder_sim_card_sim_io_finished(cbd->card, cbd->req_id);
        binder_sim_card_unref(cbd->card);
        gutil_slice_free(cbd);
    }
}

static
gboolean
binder_sim_session_cbd_start(
    BinderSimSessionCbData* cbd,
    RadioRequest* req)
{
    const gpointer finished_req = cbd->req_id;
    gboolean ok;

    cbd->ref_count++;
    ok = radio_request_submit(req);
    if (ok) {
        binder_sim_card_sim_io_started(cbd->card, cbd->req_id = req);
    } else {
        cbd->req_id = 0;
    }
    binder_sim_card_sim_io_finished(cbd->card, finished_req);
    radio_request_unref(req);
    return ok;
}

static
void
binder_sim_pin_cbd_state_event_count_cb(
    BinderSimCard* sc,
    void* user_data)
{
    BinderSimPinCbData* cbd = user_data;

    /*
     * Cound the SIM status events received while request is pending
     * so that binder_sim_pin_change_state_cb can decide whether to wait
     * for the next event or not
     */
    cbd->state_event_count++;
}

static
BinderSimPinCbData*
binder_sim_pin_cbd_new(
    BinderSim* self,
    enum ofono_sim_password_type passwd_type,
    gboolean state_change_expected,
    ofono_sim_lock_unlock_cb_t cb,
    void* data)
{
    BinderSimPinCbData* cbd = g_slice_new0(BinderSimPinCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    cbd->passwd_type = passwd_type;
    cbd->card = binder_sim_card_ref(self->card);
    if (state_change_expected) {
        cbd->card_status_id =
            binder_sim_card_add_status_received_handler(cbd->card,
                binder_sim_pin_cbd_state_event_count_cb, cbd);
    }
    return cbd;
}

static
void
binder_sim_pin_cbd_free(
    BinderSimPinCbData* cbd)
{
    if (cbd->timeout_id) {
        g_source_remove(cbd->timeout_id);
    }

    binder_sim_card_remove_handler(cbd->card, cbd->card_status_id);
    binder_sim_card_unref(cbd->card);
    gutil_slice_free(cbd);
}

static
void
binder_sim_pin_req_done(
    gpointer ptr)
{
    BinderSimPinCbData* cbd = ptr;

    /* Only free if callback isn't waiting for something else to happen */
    if (!cbd->timeout_id) {
        GASSERT(!cbd->card_status_id);
        binder_sim_pin_cbd_free(cbd);
    }
}

static
const char*
binder_sim_append_path(
    BinderSim* self,
    GBinderWriter* writer,
    int fileid,
    const guchar* path,
    guint path_len)
{
    const RADIO_APP_TYPE app_type = binder_sim_card_app_type(self->card);
    guchar db_path[OFONO_EF_PATH_BUFFER_SIZE] = { 0x00 };
    char* hex_path = NULL;
    int len;

    if (path_len > 0 && path_len < 7) {
        memcpy(db_path, path, path_len);
        len = path_len;
    } else if (app_type == RADIO_APP_TYPE_USIM) {
        len = ofono_get_ef_path_3g(fileid, db_path);
    } else if (app_type == RADIO_APP_TYPE_SIM) {
        len = ofono_get_ef_path_2g(fileid, db_path);
    } else {
        ofono_error("Unsupported app type %d", app_type);
        len = 0;
    }

    if (len > 0) {
        hex_path = binder_encode_hex(db_path, len);
        gbinder_writer_add_cleanup(writer, g_free, hex_path);
        DBG_(self, "%s", hex_path);
        return hex_path;
    } else {
        /*
         * Catch-all for EF_ICCID, EF_PL and other files absent
         * from ef_db table in src/simutil.c, hard-code ROOTMF.
         */
        DBG_(self, "%s (default)", ROOTMF);
        return ROOTMF;
    }
}

static
BinderSimIoResponse*
binder_sim_io_response_new(
    const GBinderReader* args)
{
    const RadioIccIoResult* result;
    GBinderReader reader;

    gbinder_reader_copy(&reader, args);
    result = gbinder_reader_read_hidl_struct(&reader, RadioIccIoResult);
    if (result) {
        BinderSimIoResponse* resp = g_slice_new0(BinderSimIoResponse);
        const char* hex = result->response.data.str;

        DBG("sw1=0x%02X,sw2=0x%02X,%s", result->sw1, result->sw2, hex);
        resp->sw1 = result->sw1;
        resp->sw2 = result->sw2;
        resp->data = binder_decode_hex(hex, -1, &resp->data_len);
        return resp;
    }
    return NULL;
}

static
void
binder_sim_io_response_free(
    BinderSimIoResponse* res)
{
    if (res) {
        g_free(res->data);
        g_slice_free(BinderSimIoResponse, res);
    }
}

static
gboolean
binder_sim_io_response_ok(
    const BinderSimIoResponse* res)
{
    if (res) {
        static const struct binder_sim_io_error {
            gint32 sw;
            const char* msg;
        } errmsg [] = {
            /* TS 102.221 */
            { 0x6a80, "Incorrect parameters in the data field" },
            { 0x6a81, "Function not supported" },
            { 0x6a82, "File not found" },
            { 0x6a83, "Record not found" },
            { 0x6a84, "Not enough memory space" },
            { 0x6a86, "Incorrect parameters P1 to P2" },
            { 0x6a87, "Lc inconsistent with P1 to P2" },
            { 0x6a88, "Referenced data not found" },
            /* TS 51.011 */
            { 0x9240, "Memory problem" },
            { 0x9400, "No EF selected" },
            { 0x9402, "Out of range (invalid address)" },
            { 0x9404, "File id/pattern not found" },
            { 0x9408, "File is inconsistent with the command" }
        };

        gint32 low, high, sw;

        switch (res->sw1) {
        case 0x90:
            /* '90 00' is the normal completion */
            if (res->sw2 != 0x00) {
                break;
            }
            /* fall through */
        case 0x91:
        case 0x9e:
        case 0x9f:
            return TRUE;
        case 0x92:
            if (res->sw2 != 0x40) {
                /* '92 40' is "memory problem" */
                return TRUE;
            }
            break;
        default:
            break;
        }

        /* Find the error message */
        low = 0;
        high = G_N_ELEMENTS(errmsg)-1;
        sw = (res->sw1 << 8) | res->sw2;

        while (low <= high) {
            const int mid = (low + high)/2;
            const int val = errmsg[mid].sw;

            if (val < sw) {
                low = mid + 1;
            } else if (val > sw) {
                high = mid - 1;
            } else {
                /* Message found */
                DBG("error: %s", errmsg[mid].msg);
                return FALSE;
            }
        }

        /* No message */
        DBG("error %02x %02x", res->sw1, res->sw2);
    }
    return FALSE;
}

static
void
binder_sim_file_info_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    BinderSim* self = cbd->self;
    ofono_sim_file_info_cb_t cb = cbd->cb.file_info;
    struct ofono_error err;

    DBG_(self, "");

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_ICC_IO_FOR_APP) {
            BinderSimIoResponse* res = binder_sim_io_response_new(args);

            if (!self->inserted) {
                DBG_(self, "No SIM card");
            } else if (binder_sim_io_response_ok(res) &&
                error == RADIO_ERROR_NONE) {
                gboolean ok = FALSE;
                guchar faccess[3] = { 0x00, 0x00, 0x00 };
                guchar fstatus = EF_STATUS_VALID;
                unsigned int flen = 0, rlen = 0, str = 0;

                if (res->data_len) {
                    if (res->data[0] == 0x62) {
                        ok = ofono_parse_get_response_3g(res->data,
                            res->data_len, &flen, &rlen, &str, faccess,
                            NULL);
                    } else {
                        ok = ofono_parse_get_response_2g(res->data,
                            res->data_len, &flen, &rlen, &str, faccess,
                            &fstatus);
                    }
                }

                if (ok) {
                    /* Success */
                    cb(binder_error_ok(&err), flen, str, rlen, faccess,
                       fstatus, cbd->data);
                    binder_sim_io_response_free(res);
                    return;
                } else {
                    ofono_error("file info parse error");
                }
            } else if (res) {
                binder_error_init_sim_error(&err, res->sw1, res->sw2);
            } else if (error != RADIO_ERROR_NONE) {
                ofono_error("SIM I/O error: %s",
                    binder_radio_error_string(error));
            } else {
                ofono_error("Failed to parse iccIOForApp response");
            }
            binder_sim_io_response_free(res);
        } else {
            ofono_error("Unexpected iccIOForApp response %d", resp);
        }
    }
    /* Error path */
    cb(&err, -1, -1, -1, NULL, EF_STATUS_INVALIDATED, cbd->data);
}

static
gboolean
binder_sim_request_io(
    BinderSim* self,
    guint cmd,
    int fid,
    guint p1,
    guint p2,
    guint p3,
    const char* hex_data,
    const guchar* path,
    guint path_len,
    RadioRequestCompleteFunc complete,
    BinderCallback cb,
    void* data)
{
    static const char empty[] = "";
    const char* aid = binder_sim_card_app_aid(self->card);
    BinderSimCbdIo* cbd = binder_sim_cbd_io_new(self, cb, data);
    guint parent;
    gboolean ok;

    /* iccIOForApp(int32 serial, IccIo iccIo); */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g, RADIO_REQ_ICC_IO_FOR_APP,
        &writer, complete, binder_sim_cbd_io_free, cbd);
    RadioIccIo* io = gbinder_writer_new0(&writer, RadioIccIo);

    DBG_(self, "cmd=0x%.2X,fid=0x%.4X,%d,%d,%d,%s,pin2=(null),aid=%s",
        cmd, fid, p1, p2, p3, hex_data, aid);

    io->command = cmd;
    io->fileId = fid;
    io->path.data.str = binder_sim_append_path(self, &writer, fid, path,
        path_len);
    io->path.len = strlen(io->path.data.str);
    io->p1 = p1;
    io->p2 = p2;
    io->p3 = p3;
    binder_copy_hidl_string(&writer, &io->data, hex_data);
    io->pin2.data.str = empty;
    binder_copy_hidl_string(&writer, &io->aid, aid);

    /* Write the parent structure */
    parent = gbinder_writer_append_buffer_object(&writer, io, sizeof(*io));

    /* Write the string data in the right order */
    binder_append_hidl_string_data(&writer, io, path, parent);
    binder_append_hidl_string_data(&writer, io, data, parent);
    binder_append_hidl_string_data(&writer, io, pin2, parent);
    binder_append_hidl_string_data(&writer, io, aid, parent);

    radio_request_set_blocking(req, TRUE);
    radio_request_set_timeout(req, SIM_IO_TIMEOUT_SECS * 1000);
    ok = binder_sim_cbd_io_start(cbd, req);
    radio_request_unref(req);
    return ok;
}

static
void
binder_sim_ofono_read_file_info(
    struct ofono_sim* sim,
    int fileid,
    const unsigned char* path,
    unsigned int len,
    ofono_sim_file_info_cb_t cb,
    void* data)
{
    if (!binder_sim_request_io(binder_sim_get_data(sim), CMD_GET_RESPONSE,
        fileid, 0, 0, 15, NULL, path, len, binder_sim_file_info_cb,
        BINDER_CB(cb), data)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), -1, -1, -1, NULL,
            EF_STATUS_INVALIDATED, data);
    }
}

static
void
binder_sim_read_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    BinderSim* self = cbd->self;
    ofono_sim_read_cb_t cb = cbd->cb.read;
    struct ofono_error err;

    DBG_(self, "");

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_ICC_IO_FOR_APP) {
            BinderSimIoResponse* res = binder_sim_io_response_new(args);

            if (!self->inserted) {
                DBG_(self, "No SIM card");
            } else if (binder_sim_io_response_ok(res) &&
                error == RADIO_ERROR_NONE) {
                /* Success */
                cb(binder_error_ok(&err), res->data, res->data_len, cbd->data);
                binder_sim_io_response_free(res);
                return;
            } else if (res) {
                binder_error_init_sim_error(&err, res->sw1, res->sw2);
            } else if (error != RADIO_ERROR_NONE) {
                ofono_error("SIM read error: %s",
                    binder_radio_error_string(error));
            } else {
                ofono_error("Failed to parse iccIOForApp response");
            }
            binder_sim_io_response_free(res);
        }
    }
    /* Error */
    cb(&err, NULL, 0, cbd->data);
}

static
void
binder_sim_read(
    struct ofono_sim* sim,
    guint cmd,
    int fileid,
    guint p1,
    guint p2,
    guint p3,
    const guchar* path,
    guint path_len,
    ofono_sim_read_cb_t cb,
    void* data)
{
    if (!binder_sim_request_io(binder_sim_get_data(sim), cmd, fileid, p1, p2,
        p3, NULL, path, path_len, binder_sim_read_cb, BINDER_CB(cb), data)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), NULL, 0, data);
    }
}

static
void
binder_sim_ofono_read_file_transparent(
    struct ofono_sim* sim,
    int fileid,
    int start,
    int length,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_read_cb_t cb,
    void* data)
{
    binder_sim_read(sim, CMD_READ_BINARY, fileid, (start >> 8), (start & 0xff),
        length, path, path_len, cb, data);
}

static
void
binder_sim_ofono_read_file_linear(
    struct ofono_sim* sim,
    int fileid,
    int record,
    int length,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_read_cb_t cb,
    void *data)
{
    binder_sim_read(sim, CMD_READ_RECORD, fileid, record, MODE_ABSOLUTE,
        length, path, path_len, cb, data);
}

static
void
binder_sim_ofono_read_file_cyclic(
    struct ofono_sim* sim,
    int fileid,
    int record,
    int length,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_read_cb_t cb,
    void* data)
{
    binder_sim_read(sim, CMD_READ_RECORD, fileid, record, MODE_ABSOLUTE,
        length, path, path_len, cb, data);
}

static
void binder_sim_write_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    BinderSim* self = cbd->self;
    ofono_sim_write_cb_t cb = cbd->cb.write;
    struct ofono_error err;

    DBG_(self, "");

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_ICC_IO_FOR_APP) {
            BinderSimIoResponse* res = binder_sim_io_response_new(args);

            if (!self->inserted) {
                DBG_(self, "No SIM card");
            } else if (binder_sim_io_response_ok(res) &&
                error == RADIO_ERROR_NONE) {
                /* Success */
                cb(binder_error_ok(&err), cbd->data);
                return;
            } else if (res) {
                binder_error_init_sim_error(&err, res->sw1, res->sw2);
            } else if (error != RADIO_ERROR_NONE) {
                ofono_error("SIM write error: %s",
                    binder_radio_error_string(error));
            } else {
                ofono_error("Failed to parse iccIOForApp response");
            }
            binder_sim_io_response_free(res);
        }
    }
    /* Error */
    cb(&err, cbd->data);
}

static
void
binder_sim_write(
    struct ofono_sim* sim,
    guint cmd,
    int fileid,
    guint p1,
    guint p2,
    guint length,
    const void* value,
    const guchar* path,
    guint path_len,
    ofono_sim_write_cb_t cb,
    void* data)
{
    char* hex_data = binder_encode_hex(value, length);

    if (!binder_sim_request_io(binder_sim_get_data(sim), cmd, fileid, p1, p2,
        length, hex_data, path, path_len, binder_sim_write_cb,
        BINDER_CB(cb), data)) {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }
    g_free(hex_data);
}

static
void
binder_sim_write_file_transparent(
    struct ofono_sim* sim,
    int fileid,
    int start,
    int length,
    const unsigned char* value,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_write_cb_t cb,
    void* data)
{
    binder_sim_write(sim, CMD_UPDATE_BINARY, fileid, (start >> 8),
        (start & 0xff), length, value, path, path_len, cb, data);
}

static
void
binder_sim_write_file_linear(
    struct ofono_sim* sim,
    int fileid,
    int record,
    int length,
    const unsigned char* value,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_write_cb_t cb,
    void* data)
{
    binder_sim_write(sim, CMD_UPDATE_RECORD, fileid, record, MODE_ABSOLUTE,
        length, value, path, path_len, cb, data);
}

static
void
binder_sim_write_file_cyclic(
    struct ofono_sim* sim,
    int fileid,
    int length,
    const unsigned char* value,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_write_cb_t cb,
    void* data)
{
    binder_sim_write(sim, CMD_UPDATE_RECORD, fileid, 0, MODE_PREVIOUS,
        length, value, path, path_len, cb, data);
}

static
void
binder_sim_get_imsi_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    ofono_sim_imsi_cb_t cb = cbd->cb.imsi;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        /* getIMSIForAppResponse(RadioResponseInfo, string imsi); */
        if (resp == RADIO_RESP_GET_IMSI_FOR_APP) {
            if (error == RADIO_ERROR_NONE) {
                const char* imsi = binder_read_hidl_string(args);

                DBG_(cbd->self, "%s", imsi);
                if (imsi) {
                    /* Success */
                    GASSERT(strlen(imsi) == 15);
                    cb(binder_error_ok(&err), imsi, cbd->data);
                    return;
                }
            } else {
                ofono_warn("Failed to query IMSI, error %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getIMSIForApp response %d", resp);
        }
    }

    /* Error */
    cb(binder_error_failure(&err), NULL, cbd->data);
}

static
void
binder_sim_read_imsi(
    struct ofono_sim* sim,
    ofono_sim_imsi_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    BinderSimCbdIo* cbd = binder_sim_cbd_io_new(self, BINDER_CB(cb), data);
    const char* aid = binder_sim_card_app_aid(self->card);
    gboolean ok;

    /* getImsiForApp(int32 serial, string aid); */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_GET_IMSI_FOR_APP, &writer,
        binder_sim_get_imsi_cb, binder_sim_cbd_io_free, cbd);

    DBG_(self, "%s", aid);
    binder_append_hidl_string(&writer, aid);

    /*
     * If we fail the .read_imsi call, ofono gets into "Unable to
     * read IMSI, emergency calls only" state. Retry the request
     * on failure.
     */
    radio_request_set_retry(req, BINDER_RETRY_MS, -1);
    radio_request_set_blocking(req, TRUE);
    ok = binder_sim_cbd_io_start(cbd, req);
    radio_request_unref(req);

    if (!ok) {
        struct ofono_error err;

        cb(binder_error_failure(&err), NULL, cbd->data);
    }
}

static
enum ofono_sim_password_type
binder_sim_passwd_state(
    BinderSim* self)
{
    const BinderSimCardApp *app = self->card->app;

    if (app) {
        switch (app->app_state) {
        case RADIO_APP_STATE_PIN:
            return OFONO_SIM_PASSWORD_SIM_PIN;
        case RADIO_APP_STATE_PUK:
            return OFONO_SIM_PASSWORD_SIM_PUK;
        case RADIO_APP_STATE_READY:
            return OFONO_SIM_PASSWORD_NONE;
        case RADIO_APP_STATE_SUBSCRIPTION_PERSO:
            switch (app->perso_substate) {
            case RADIO_PERSO_SUBSTATE_READY:
                return OFONO_SIM_PASSWORD_NONE;
            case RADIO_PERSO_SUBSTATE_SIM_NETWORK:
                return OFONO_SIM_PASSWORD_PHNET_PIN;
            case RADIO_PERSO_SUBSTATE_SIM_NETWORK_SUBSET:
                return OFONO_SIM_PASSWORD_PHNETSUB_PIN;
            case RADIO_PERSO_SUBSTATE_SIM_CORPORATE:
                return OFONO_SIM_PASSWORD_PHCORP_PIN;
            case RADIO_PERSO_SUBSTATE_SIM_SERVICE_PROVIDER:
                return OFONO_SIM_PASSWORD_PHSP_PIN;
            case RADIO_PERSO_SUBSTATE_SIM_SIM:
                return OFONO_SIM_PASSWORD_PHSIM_PIN;
            case RADIO_PERSO_SUBSTATE_SIM_NETWORK_PUK:
                return OFONO_SIM_PASSWORD_PHNET_PUK;
            case RADIO_PERSO_SUBSTATE_SIM_NETWORK_SUBSET_PUK:
                return OFONO_SIM_PASSWORD_PHNETSUB_PUK;
            case RADIO_PERSO_SUBSTATE_SIM_CORPORATE_PUK:
                return OFONO_SIM_PASSWORD_PHCORP_PUK;
            case RADIO_PERSO_SUBSTATE_SIM_SERVICE_PROVIDER_PUK:
                return OFONO_SIM_PASSWORD_PHSP_PUK;
            case RADIO_PERSO_SUBSTATE_SIM_SIM_PUK:
                return OFONO_SIM_PASSWORD_PHFSIM_PUK;
            default:
                break;
            }
        default:
            break;
        }
    }
    return OFONO_SIM_PASSWORD_INVALID;
}

static
gboolean
binder_sim_app_in_transient_state(
    BinderSim* self)
{
    const BinderSimCardApp* app = self->card->app;

    if (app) {
        switch (app->app_state) {
        case RADIO_APP_STATE_DETECTED:
            return TRUE;
        case RADIO_APP_STATE_SUBSCRIPTION_PERSO:
            switch (app->perso_substate) {
            case RADIO_PERSO_SUBSTATE_UNKNOWN:
            case RADIO_PERSO_SUBSTATE_IN_PROGRESS:
                return TRUE;
            default:
                break;
            }
        default:
            break;
        }
    }
    return FALSE;
}

static
void
binder_sim_finish_passwd_state_query(
    BinderSim* self,
    enum ofono_sim_password_type state)
{
    if (self->query_passwd_state_timeout_id) {
        g_source_remove(self->query_passwd_state_timeout_id);
        self->query_passwd_state_timeout_id = 0;
    }

    if (self->query_passwd_state_sim_status_refresh_id) {
        binder_sim_card_remove_handler(self->card,
            self->query_passwd_state_sim_status_refresh_id);
        self->query_passwd_state_sim_status_refresh_id = 0;
    }

    if (self->query_passwd_state_cb) {
        ofono_sim_passwd_cb_t cb = self->query_passwd_state_cb;
        void* data = self->query_passwd_state_cb_data;
        struct ofono_error err;

        self->query_passwd_state_cb = NULL;
        self->query_passwd_state_cb_data = NULL;
        self->ofono_passwd_state = state;

        if (state == OFONO_SIM_PASSWORD_INVALID) {
            cb(binder_error_failure(&err), state, data);
        } else {
            cb(binder_error_ok(&err), state, data);
        }
    }
}

static
void
binder_sim_check_perm_lock(
    BinderSim* self)
{
    BinderSimCard* sim = self->card;

    /*
     * Zero number of retries in the PUK state indicates to the ofono
     * client that the card is permanently locked. This is different
     * from the case when the number of retries is negative (which
     * means that PUK is required but the number of remaining attempts
     * is not available).
     */
    if (sim->app &&
        sim->app->app_state == RADIO_APP_STATE_PUK &&
        sim->app->pin1_state == RADIO_PIN_STATE_ENABLED_PERM_BLOCKED) {

        /*
         * It makes no sense to return non-zero number of remaining
         * attempts in PERM_LOCKED state. So when we get here, the
         * number of retries has to be negative (unknown) or zero.
         * Otherwise, something must be broken.
         */
        GASSERT(self->retries[OFONO_SIM_PASSWORD_SIM_PUK] <= 0);
        if (self->retries[OFONO_SIM_PASSWORD_SIM_PUK] < 0) {
            self->retries[OFONO_SIM_PASSWORD_SIM_PUK] = 0;
            DBG_(self, "SIM card is locked");
        }
    }
}

static
void
binder_sim_invalidate_passwd_state(
    BinderSim* self)
{
    guint i;

    self->ofono_passwd_state = OFONO_SIM_PASSWORD_INVALID;
    for (i = 0; i < OFONO_SIM_PASSWORD_INVALID; i++) {
        self->retries[i] = -1;
    }

    binder_sim_check_perm_lock(self);
    binder_sim_finish_passwd_state_query(self, OFONO_SIM_PASSWORD_INVALID);
}

static
void
binder_sim_app_changed_cb(
    BinderSimCard* sim,
    void *user_data)
{
    binder_sim_check_perm_lock((BinderSim*)user_data);
}

static
void
binder_sim_status_changed_cb(
    BinderSimCard* sim,
    void* user_data)
{
    BinderSim* self = user_data;

    GASSERT(self->card == sim);
    if (sim->status && sim->status->card_state == RADIO_CARD_STATE_PRESENT) {
        if (sim->app) {
            enum ofono_sim_password_type ps;

            binder_sim_check_perm_lock(self);
            if (!self->inserted) {
                self->inserted = TRUE;
                ofono_info("SIM card OK");
                ofono_sim_inserted_notify(self->sim, TRUE);
            }

            ps = binder_sim_passwd_state(self);
            if (ps != OFONO_SIM_PASSWORD_INVALID) {
                binder_sim_finish_passwd_state_query(self, ps);
            }
        } else {
            binder_sim_invalidate_passwd_state(self);
        }
    } else {
        binder_sim_invalidate_passwd_state(self);
        if (self->inserted) {
            self->inserted = FALSE;
            ofono_info("No SIM card");
            ofono_sim_inserted_notify(self->sim, FALSE);
        }
    }
}

static
void
binder_sim_state_changed_cb(
    struct ofono_watch* watch,
    void* data)
{
    BinderSim* self = data;
    const enum ofono_sim_state state = ofono_sim_get_state(watch->sim);

    DBG_(self, "%d %d", state, self->inserted);
    if (state == OFONO_SIM_STATE_RESETTING && self->inserted) {
        /* That will simulate SIM card removal: */
        binder_sim_card_reset(self->card);
    }
}

static
RadioRequest*
binder_sim_enter_sim_pin_req(
    BinderSim* self,
    RADIO_REQ code,
    const char* pin,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    /*
     * supplyIccPinForApp(int32 serial, string pin, string aid);
     * supplyIccPin2ForApp(int32_t serial, string pin2, string aid);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g, code, &writer,
        complete, destroy, user_data);

    binder_append_hidl_string(&writer, pin);
    binder_append_hidl_string(&writer,
        binder_sim_card_app_aid(self->card));

    radio_request_set_blocking(req, TRUE);
    return req;
}

static
RadioRequest*
binder_sim_enter_sim_puk_req(
    BinderSim* self,
    RADIO_REQ code,
    const char* puk,
    const char* pin,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    /*
     * supplyIccPukForApp(int32 serial, string puk, string pin, string aid);
     * supplyIccPuk2ForApp(int32 serial, string puk2, string pin2, string aid);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g, code, &writer,
        complete, destroy, user_data);

    binder_append_hidl_string(&writer, puk);
    binder_append_hidl_string(&writer, pin);
    binder_append_hidl_string(&writer,
        binder_sim_card_app_aid(self->card));

    radio_request_set_blocking(req, TRUE);
    return req;
}

/*
 * Some IRadio implementations allow to query the retry count
 * by sending the empty pin in any state.
 */

static
RadioRequest*
binder_sim_empty_sim_pin_req(
    BinderSim* self,
    RADIO_REQ code,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return binder_sim_enter_sim_pin_req(self, code, "",
        complete, destroy, user_data);
}

static
RadioRequest*
binder_sim_empty_sim_puk_req(
    BinderSim* self,
    RADIO_REQ code,
    RadioRequestCompleteFunc complete,
    GDestroyNotify destroy,
    void* user_data)
{
    return binder_sim_enter_sim_puk_req(self, code, "", "",
        complete, destroy, user_data);
}

static const BinderSimRetryQuery binder_sim_retry_query_types[] = {
    {
        "pin",
        OFONO_SIM_PASSWORD_SIM_PIN,
        RADIO_REQ_SUPPLY_ICC_PIN_FOR_APP,
        binder_sim_empty_sim_pin_req
    },{
        "pin2",
        OFONO_SIM_PASSWORD_SIM_PIN2,
        RADIO_REQ_SUPPLY_ICC_PIN2_FOR_APP,
        binder_sim_empty_sim_pin_req
    },{
        "puk",
        OFONO_SIM_PASSWORD_SIM_PUK,
        RADIO_REQ_SUPPLY_ICC_PUK_FOR_APP,
        binder_sim_empty_sim_puk_req
    },{
        "puk2",
        OFONO_SIM_PASSWORD_SIM_PUK2,
        RADIO_REQ_SUPPLY_ICC_PUK2_FOR_APP,
        binder_sim_empty_sim_puk_req
    }
};

static
BinderSimRetryQueryCbData*
binder_sim_retry_query_cbd_new(
    BinderSim* self,
    guint query_index,
    ofono_sim_pin_retries_cb_t cb,
    void* data)
{
    BinderSimRetryQueryCbData* cbd = g_slice_new(BinderSimRetryQueryCbData);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    cbd->query_index = query_index;
    return cbd;
}

static
void
binder_sim_retry_query_cbd_free(
    gpointer cbd)
{
    g_slice_free(BinderSimRetryQueryCbData, cbd);
}

static
RadioRequest*
binder_sim_query_retry_count(
    BinderSim* self,
    guint start_index,
    ofono_sim_pin_retries_cb_t cb,
    void* data)
{
    if (self->empty_pin_query_allowed) {
        guint i = start_index;

        /* Find the first unknown retry count that we can query. */
        while (i < G_N_ELEMENTS(binder_sim_retry_query_types)) {
            const BinderSimRetryQuery* query =
                binder_sim_retry_query_types + i;

            if (self->retries[query->passwd_type] < 0) {
                RadioRequest* req = query->new_req(self, query->code,
                    binder_sim_query_retry_count_cb,
                    binder_sim_retry_query_cbd_free,
                    binder_sim_retry_query_cbd_new(self, i, cb, data));

                DBG_(self, "querying %s retry count...", query->name);
                if (radio_request_submit(req)) {
                    return req;
                } else {
                    radio_request_unref(req);
                }
                break;
            }
            i++;
        }
    }
    return NULL;
}

static
void
binder_sim_query_retry_count_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimRetryQueryCbData* cbd = user_data;
    BinderSim* self = cbd->self;
    struct ofono_error err;

    GASSERT(self->query_pin_retries_req);
    radio_request_unref(self->query_pin_retries_req);
    self->query_pin_retries_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            gint32 retry_count;

            if (binder_read_int32(args, &retry_count)) {
                const BinderSimRetryQuery* query =
                    binder_sim_retry_query_types + cbd->query_index;

                DBG_(self, "%s retry count=%d", query->name, retry_count);
                self->retries[query->passwd_type] = retry_count;

                /* Submit the next request */
                if ((self->query_pin_retries_req =
                    binder_sim_query_retry_count(self, cbd->query_index + 1,
                    cbd->cb, cbd->data)) != NULL) {
                    /* The next request is pending */
                    return;
                }
            } else {
                ofono_error("pin retry query error %s",
                   binder_radio_error_string(error));
                self->empty_pin_query_allowed = FALSE;
            }
        }
    }

    cbd->cb(binder_error_ok(&err), self->retries, cbd->data);
}

static
void
binder_sim_query_pin_retries(
    struct ofono_sim* sim,
    ofono_sim_pin_retries_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);

    DBG_(self, "");
    radio_request_drop(self->query_pin_retries_req);
    if (!(self->query_pin_retries_req =
          binder_sim_query_retry_count(self, 0, cb, data))) {
        struct ofono_error err;

        /* Nothing to wait for */
        cb(binder_error_ok(&err), self->retries, data);
    }
}

static
void
binder_sim_query_passwd_state_complete_cb(
    BinderSimCard* sim,
    void* user_data)
{
    BinderSim* self = user_data;

    GASSERT(self->query_passwd_state_sim_status_refresh_id);
    binder_sim_finish_passwd_state_query(self, binder_sim_passwd_state(self));
}

static
gboolean
binder_sim_query_passwd_state_timeout_cb(
    gpointer user_data)
{
    BinderSim* self = user_data;

    GASSERT(self->query_passwd_state_cb);
    self->query_passwd_state_timeout_id = 0;
    binder_sim_finish_passwd_state_query(self, OFONO_SIM_PASSWORD_INVALID);

    return G_SOURCE_REMOVE;
}

static
void
binder_sim_query_passwd_state(
    struct ofono_sim* sim,
    ofono_sim_passwd_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);

    if (self->query_passwd_state_timeout_id) {
        g_source_remove(self->query_passwd_state_timeout_id);
        self->query_passwd_state_timeout_id = 0;
    }

    if (!self->query_passwd_state_sim_status_refresh_id) {
        binder_sim_card_remove_handler(self->card,
            self->query_passwd_state_sim_status_refresh_id);
        self->query_passwd_state_sim_status_refresh_id = 0;
    }

    /* Always request fresh status, just in case. */
    binder_sim_card_request_status(self->card);
    self->query_passwd_state_cb = cb;
    self->query_passwd_state_cb_data = data;

    if (binder_sim_passwd_state(self) != OFONO_SIM_PASSWORD_INVALID) {
        /* Just wait for GET_SIM_STATUS completion */
        DBG_(self, "waiting for SIM status query to complete");
        self->query_passwd_state_sim_status_refresh_id =
            binder_sim_card_add_status_received_handler(self->card,
                binder_sim_query_passwd_state_complete_cb, self);
    } else {
        /* Wait for the state to change */
        DBG_(self, "waiting for the SIM state to change");
    }

    /*
     * We still need to complete the request somehow, even if
     * GET_STATUS never completes or SIM status never changes.
     */
    self->query_passwd_state_timeout_id =
        g_timeout_add_seconds(SIM_STATE_CHANGE_TIMEOUT_SECS,
            binder_sim_query_passwd_state_timeout_cb, self);
}

static
gboolean
binder_sim_pin_change_state_timeout_cb(
    gpointer user_data)
{
    BinderSimPinCbData* cbd = user_data;
    BinderSim* self = cbd->self;
    struct ofono_error err;

    DBG_(self, "oops...");
    cbd->timeout_id = 0;
    self->pin_cbd_list = g_list_remove(self->pin_cbd_list, cbd);
    cbd->cb(binder_error_failure(&err), cbd->data);
    binder_sim_pin_cbd_free(cbd);

    return G_SOURCE_REMOVE;
}

static
void
binder_sim_pin_change_state_status_cb(
    BinderSimCard* sim,
    void* user_data)
{
    BinderSimPinCbData* cbd = user_data;
    BinderSim* self = cbd->self;

    if (!binder_sim_app_in_transient_state(self)) {
        enum ofono_sim_password_type ps = binder_sim_passwd_state(self);
        struct ofono_error err;

        if (ps == OFONO_SIM_PASSWORD_INVALID ||
            cbd->status != RADIO_ERROR_NONE) {
            DBG_(self, "failure");
            cbd->cb(binder_error_failure(&err), cbd->data);
        } else {
            DBG_(self, "success, passwd_state=%d", ps);
            cbd->cb(binder_error_ok(&err), cbd->data);
        }

        ofono_sim_initialized_notify(self->sim);
        self->pin_cbd_list = g_list_remove(self->pin_cbd_list, cbd);
        binder_sim_pin_cbd_free(cbd);
    } else {
        DBG_(self, "will keep waiting");
    }
}

static
void
binder_sim_pin_change_state_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimPinCbData* cbd = user_data;
    BinderSim* self = cbd->self;
    enum ofono_sim_password_type type = cbd->passwd_type;
    gint32 retry_count = 0;

    if (status == RADIO_TX_STATUS_OK) {
        if (!binder_read_int32(args, &retry_count)) {
            ofono_error("Failed to parse PIN/PUK response %d", resp);
            error = RADIO_ERROR_GENERIC_FAILURE;
        }
    } else {
        error = RADIO_ERROR_GENERIC_FAILURE;
    }

    DBG_(self, "result=%d type=%d retry_count=%d", error, type, retry_count);
    if (error == RADIO_ERROR_NONE && retry_count == 0) {
        enum ofono_sim_password_type pin_type = ofono_sim_puk2pin(type);
        /*
         * If PIN/PUK request has succeeded, zero retry count
         * makes no sense, we have to assume that it's unknown.
         * If it can be queried, it will be queried later. If
         * it can't be queried it will remain unknown.
         */
        self->retries[type] = -1;
        if (pin_type != OFONO_SIM_PASSWORD_INVALID) {
            /* Successful PUK requests affect PIN retry count */
            self->retries[pin_type] = -1;
        }
    } else {
        self->retries[type] = retry_count;
    }

    binder_sim_check_perm_lock(self);
    cbd->status = error;

    /*
     * RADIO_ERROR_PASSWORD_INCORRECT is the final result,
     * no need to wait.
     */
    if (error != RADIO_ERROR_PASSWORD_INCORRECT && cbd->card_status_id &&
        (!cbd->state_event_count || binder_sim_app_in_transient_state(self))) {
        GASSERT(!g_list_find(self->pin_cbd_list, cbd));
        GASSERT(!cbd->timeout_id);

        /* Wait for the SIM to change state */
        DBG_(self, "waiting for SIM state change");
        self->pin_cbd_list = g_list_append(self->pin_cbd_list, cbd);
        cbd->timeout_id = g_timeout_add_seconds(SIM_STATE_CHANGE_TIMEOUT_SECS,
            binder_sim_pin_change_state_timeout_cb, cbd);

        /*
         * We no longer need to maintain state_event_count,
         * replace the SIM state event handler
         */
        binder_sim_card_remove_handler(cbd->card, cbd->card_status_id);
        cbd->card_status_id =
            binder_sim_card_add_status_received_handler(self->card,
                binder_sim_pin_change_state_status_cb, cbd);
    } else {
        struct ofono_error err;

        /* It's either already changed or not expected at all */
        if (error == RADIO_ERROR_NONE) {
            cbd->cb(binder_error_ok(&err), cbd->data);
        } else {
            cbd->cb(binder_error_failure(&err), cbd->data);
        }

        /* To avoid assert in binder_sim_pin_req_done: */
        if (cbd->card_status_id) {
            binder_sim_card_remove_handler(cbd->card, cbd->card_status_id);
            cbd->card_status_id = 0;
        }

        /* Tell the core that we are ready to accept more requests */
        ofono_sim_initialized_notify(self->sim);
    }
}

static
void
binder_sim_pin_send(
    struct ofono_sim* sim,
    const char* passwd,
    ofono_sim_lock_unlock_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    RadioRequest* req = binder_sim_enter_sim_pin_req(self,
        RADIO_REQ_SUPPLY_ICC_PIN_FOR_APP, passwd,
        binder_sim_pin_change_state_cb, binder_sim_pin_req_done,
        binder_sim_pin_cbd_new(self, OFONO_SIM_PASSWORD_SIM_PIN, TRUE,
        cb, data));

    if (radio_request_submit(req)) {
        DBG_(self, "%s,aid=%s", passwd, binder_sim_card_app_aid(self->card));
    } else {
        struct ofono_error err;

        DBG_(self, "sorry");
        cb(binder_error_failure(&err), data);
    }
    radio_request_unref(req);
}

static
gboolean
binder_perso_change_state(
    struct ofono_sim* sim,
    enum ofono_sim_password_type passwd_type,
    int enable,
    const char* passwd,
    ofono_sim_lock_unlock_cb_t cb,
    void *data)
{
    BinderSim* self = binder_sim_get_data(sim);
    RADIO_REQ code = RADIO_REQ_NONE;
    gboolean ok = FALSE;

    switch (passwd_type) {
    case OFONO_SIM_PASSWORD_PHNET_PIN:
        if (!enable) {
            code = RADIO_REQ_SUPPLY_NETWORK_DEPERSONALIZATION;
        } else {
            DBG_(self, "Not supported, enable=%d", enable);
        }
        break;
    default:
        DBG_(self, "Not supported, type=%d", passwd_type);
        break;
    }

    if (code) {
        /* supplyNetworkDepersonalization(int32 serial, string netPin); */
        GBinderWriter writer;
        RadioRequest* req = radio_request_new2(self->g, code, &writer,
            binder_sim_pin_change_state_cb, binder_sim_pin_req_done,
            binder_sim_pin_cbd_new(self, passwd_type, FALSE, cb, data));

        binder_append_hidl_string(&writer, passwd);
        ok = radio_request_submit(req);
        radio_request_unref(req);
    }

    return ok;
}

static
const char*
binder_sim_facility_code(
    enum ofono_sim_password_type type)
{
    switch (type) {
    case OFONO_SIM_PASSWORD_SIM_PIN:
        return "SC";
    case OFONO_SIM_PASSWORD_SIM_PIN2:
        return "P2";
    case OFONO_SIM_PASSWORD_PHSIM_PIN:
        return "PS";
    case OFONO_SIM_PASSWORD_PHFSIM_PIN:
        return "PF";
    case OFONO_SIM_PASSWORD_PHNET_PIN:
        return "PN";
    case OFONO_SIM_PASSWORD_PHNETSUB_PIN:
        return "PU";
    case OFONO_SIM_PASSWORD_PHSP_PIN:
        return "PP";
    case OFONO_SIM_PASSWORD_PHCORP_PIN:
        return "PC";
    default:
        return NULL;
    }
}

static
void
binder_sim_pin_change_state(
    struct ofono_sim* sim,
    enum ofono_sim_password_type pwtype,
    int enable,
    const char* passwd,
    ofono_sim_lock_unlock_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    const char* aid = binder_sim_card_app_aid(self->card);
    const char* fac = binder_sim_facility_code(pwtype);
    gboolean ok = FALSE;

    DBG_(self, "%d,%s,%d,%s,0,aid=%s", pwtype, fac, enable, passwd, aid);

    if (pwtype == OFONO_SIM_PASSWORD_PHNET_PIN) {
        ok = binder_perso_change_state(sim, pwtype, enable, passwd, cb, data);
    } else if (fac) {
        /*
         * setFacilityLockForApp(int32 serial, string facility, bool lockState,
         * string password, int32 serviceClass, string appId);
         */
        GBinderWriter writer;
        RadioRequest* req = radio_request_new2(self->g,
            RADIO_REQ_SET_FACILITY_LOCK_FOR_APP, &writer,
            binder_sim_pin_change_state_cb, binder_sim_pin_req_done,
            binder_sim_pin_cbd_new(self, pwtype, FALSE, cb, data));

        gbinder_writer_append_hidl_string(&writer, fac); /* facility */
        gbinder_writer_append_bool(&writer, enable);     /* lockState */
        binder_append_hidl_string(&writer, passwd);      /* password */
        gbinder_writer_append_int32(&writer,             /* serviceClass */
            RADIO_SERVICE_CLASS_NONE);
        binder_append_hidl_string(&writer, aid);         /* appId */

        radio_request_set_blocking(req, TRUE);
        ok = radio_request_submit(req);
        radio_request_unref(req);
    }

    if (!ok) {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_sim_pin_send_puk(
    struct ofono_sim* sim,
    const char* puk,
    const char* pin,
    ofono_sim_lock_unlock_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    RadioRequest* req = binder_sim_enter_sim_puk_req(self,
        RADIO_REQ_SUPPLY_ICC_PUK_FOR_APP, puk, pin,
        binder_sim_pin_change_state_cb, binder_sim_pin_req_done,
        binder_sim_pin_cbd_new(self, OFONO_SIM_PASSWORD_SIM_PUK, TRUE,
        cb, data));

    if (radio_request_submit(req)) {
        DBG_(self, "puk=%s,pin=%s,aid=%s", puk, pin,
            binder_sim_card_app_aid(self->card));
    } else {
        struct ofono_error err;

        DBG_(self, "sorry");
        cb(binder_error_failure(&err), data);
    }
    radio_request_unref(req);
}

static
void
binder_sim_change_passwd(
    struct ofono_sim* sim,
    enum ofono_sim_password_type passwd_type,
    const char* old_passwd,
    const char* new_passwd,
    ofono_sim_lock_unlock_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    RADIO_REQ code = RADIO_REQ_NONE;
    gboolean ok = FALSE;

    switch (passwd_type) {
    case OFONO_SIM_PASSWORD_SIM_PIN:
        code = RADIO_REQ_CHANGE_ICC_PIN_FOR_APP;
        break;
    case OFONO_SIM_PASSWORD_SIM_PIN2:
        code = RADIO_REQ_CHANGE_ICC_PIN2_FOR_APP;
        break;
    default:
        break;
    }

    if (code) {
        /*
         * changeIccPinForApp(int32 serial, string oldPin, string newPin,
         *   string aid);
         * changeIccPin2ForApp(int32 serial, string oldPin2, string newPin2,
         *   string aid);
         */
        GBinderWriter writer;
        RadioRequest* req = radio_request_new2(self->g, code, &writer,
            binder_sim_pin_change_state_cb, binder_sim_pin_req_done,
            binder_sim_pin_cbd_new(self, passwd_type, FALSE, cb, data));
        const char* aid = binder_sim_card_app_aid(self->card);

        DBG_(self, "old=%s,new=%s,aid=%s", old_passwd, new_passwd, aid);
        binder_append_hidl_string(&writer, old_passwd);
        binder_append_hidl_string(&writer, new_passwd);
        binder_append_hidl_string(&writer, aid);
        radio_request_set_blocking(req, TRUE);
        ok = radio_request_submit(req);
        radio_request_unref(req);
    }

    if (!ok) {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_sim_query_facility_lock_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    ofono_query_facility_lock_cb_t cb = cbd->cb.query_facility_lock;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        /* getFacilityLockForAppResponse(RadioResponseInfo, int32 response); */
        if (resp == RADIO_RESP_GET_FACILITY_LOCK_FOR_APP) {
            if (error == RADIO_ERROR_NONE) {
                gint32 locked;

                if (binder_read_int32(args, &locked)) {
                    DBG_(cbd->self, "%d", locked);
                    cb(binder_error_ok(&err), locked != 0, cbd->data);
                    return;
                } else {
                    ofono_error("Broken getFacilityLockForApp response?");
                }
            } else {
                ofono_error("Facility lock query error: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected getFacilityLockForApp response %d", resp);
        }
    }
    /* Error */
    cb(binder_error_failure(&err), FALSE, cbd->data);
}

static
gboolean
binder_sim_query_facility_lock_retry(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    return (status == RADIO_TX_STATUS_TIMEOUT);
}

static
void
binder_sim_query_facility_lock(
    struct ofono_sim* sim,
    enum ofono_sim_password_type type,
    ofono_query_facility_lock_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    const char* fac = binder_sim_facility_code(type);
    BinderSimCbdIo* cbd = binder_sim_cbd_io_new(self, BINDER_CB(cb), data);
    gboolean ok;

    /*
     * getFacilityLockForApp(int32 serial, string facility, string password,
     *   int32 serviceClass, string appId);
     */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_GET_FACILITY_LOCK_FOR_APP, &writer,
        binder_sim_query_facility_lock_cb, binder_sim_cbd_io_free, cbd);

    binder_append_hidl_string(&writer, fac); /* facility */
    binder_append_hidl_string(&writer, "");  /* password */
    gbinder_writer_append_int32(&writer,     /* serviceClass */
        RADIO_SERVICE_CLASS_NONE);
    binder_append_hidl_string(&writer,       /* appId */
        binder_sim_card_app_aid(self->card));

    /* Make sure that this request gets completed sooner or later */
    radio_request_set_timeout(req, FAC_LOCK_QUERY_TIMEOUT_SECS * 1000);
    radio_request_set_retry(req, BINDER_RETRY_MS, FAC_LOCK_QUERY_RETRIES);
    radio_request_set_retry_func(req, binder_sim_query_facility_lock_retry);

    DBG_(self, "%s", fac);
    ok = binder_sim_cbd_io_start(cbd, req);
    radio_request_unref(req);

    if (!ok) {
        struct ofono_error err;

        cb(binder_error_failure(&err), FALSE, data);
    }
}

static
gboolean
binder_sim_list_apps_cb(
    gpointer data)
{
    BinderSimListApps* cbd = data;
    BinderSim* self = cbd->self;
    const BinderSimCardStatus* status = self->card->status;
    struct ofono_error err;

    GASSERT(self->list_apps_id);
    self->list_apps_id = 0;

    if (status) {
        int i, n = status->num_apps;
        GByteArray* tlv = g_byte_array_sized_new(n * 20);

        /* Reconstruct EFdir contents */
        for (i = 0; i < n; i++) {
            const char* hex = status->apps[i].aid;
            gsize hex_len = hex ? strlen(hex) : 0;
            guint8 aid[16];

            if (hex_len >= 2 && hex_len <= 2 * sizeof(aid) &&
                gutil_hex2bin(hex, hex_len, aid)) {
                const guint8 aid_size = (guint8)hex_len/2;
                guint8 buf[4];

                /*
                 * TS 102.221
                 * 13 Application independent files
                 * 13.1 EFdir
                 *
                 * Application template TLV object.
                 */
                buf[0] = APP_TEMPLATE_TAG;
                buf[1] = aid_size + 2;
                buf[2] = APP_ID_TAG;
                buf[3] = aid_size;
                g_byte_array_append(tlv, buf, sizeof(buf));
                g_byte_array_append(tlv, aid, aid_size);
            }
        }
        DBG_(self, "reporting %u apps %u bytes", n, tlv->len);
        cbd->cb(binder_error_ok(&err), tlv->data, tlv->len, cbd->data);
        g_byte_array_unref(tlv);
    } else {
        DBG_(self, "no SIM card, no apps");
        cbd->cb(binder_error_failure(&err), NULL, 0, cbd->data);
    }

    return G_SOURCE_REMOVE;
}

static
void
binder_sim_list_apps(
    struct ofono_sim* sim,
    ofono_sim_list_apps_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    BinderSimListApps* cbd = g_new(BinderSimListApps, 1);

    cbd->self = self;
    cbd->cb = cb;
    cbd->data = data;
    if (self->list_apps_id) {
        g_source_remove(self->list_apps_id);
    }
    self->list_apps_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
        binder_sim_list_apps_cb, cbd, g_free);
}

static
void
binder_sim_open_channel_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    ofono_sim_open_channel_cb_t cb = cbd->cb.open_channel;
    struct ofono_error err;

    if (status == RADIO_TX_STATUS_OK) {
        /*
         * iccOpenLogicalChannelResponse(RadioResponseInfo info,
         *   int32 channelId, vec<int8_t> selectResponse);
         */
        if (resp == RADIO_RESP_ICC_OPEN_LOGICAL_CHANNEL) {
            if (error == RADIO_ERROR_NONE) {
                gint32 channel;

                /* Ignore selectResponse */
                if (binder_read_int32(args, &channel)) {
                    /* Success */
                    DBG_(cbd->self, "%u", channel);
                    cb(binder_error_ok(&err), channel, cbd->data);
                    return;
                } else {
                    ofono_error("Broken iccOpenLogicalChannel response?");
                }
            } else {
                ofono_error("Open logical channel failure: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected iccOpenLogicalChannel response %d", resp);
        }
    }
    /* Error */
    cb(binder_error_failure(&err), 0, cbd->data);
}

static
void
binder_sim_open_channel(
    struct ofono_sim* sim,
    const unsigned char* aid,
    unsigned int len,
    ofono_sim_open_channel_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    BinderSimCbdIo* cbd = binder_sim_cbd_io_new(self, BINDER_CB(cb), data);
    gboolean ok;

    /* iccOpenLogicalChannel(int32 serial, string aid, int32 p2); */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_ICC_OPEN_LOGICAL_CHANNEL, &writer,
        binder_sim_open_channel_cb, binder_sim_cbd_io_free, cbd);
    char *aid_hex = binder_encode_hex(aid, len);

    DBG_(self, "%s", aid_hex);
    gbinder_writer_add_cleanup(&writer, g_free, aid_hex);
    gbinder_writer_append_hidl_string(&writer, aid_hex); /* aid */
    gbinder_writer_append_int32(&writer, 0);             /* p2 */
    radio_request_set_timeout(req, SIM_IO_TIMEOUT_SECS * 1000);
    ok = binder_sim_cbd_io_start(cbd, req);
    radio_request_unref(req);

    if (!ok) {
        struct ofono_error err;

        cb(binder_error_failure(&err), 0, data);
    }
}

static
void
binder_sim_close_channel_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimCbdIo* cbd = user_data;
    struct ofono_error err;

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_ICC_CLOSE_LOGICAL_CHANNEL) {
            if (error == RADIO_ERROR_NONE) {
                binder_error_init_ok(&err);
            } else {
                ofono_error("Close logical channel failure: %s",
                    binder_radio_error_string(error));
            }
        } else {
            ofono_error("Unexpected iccCloseLogicalChannel response %d", resp);
        }
    }
    cbd->cb.close_channel(&err, cbd->data);
}

static
void
binder_sim_close_channel(
    struct ofono_sim* sim,
    int channel,
    ofono_sim_close_channel_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    BinderSimCbdIo* cbd = binder_sim_cbd_io_new(self, BINDER_CB(cb), data);
    gboolean ok;

    /* iccCloseLogicalChannel(int32 serial, int32 channelId); */
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_ICC_CLOSE_LOGICAL_CHANNEL, &writer,
        binder_sim_close_channel_cb, binder_sim_cbd_io_free, cbd);

    DBG_(self, "%u", channel);
    gbinder_writer_append_int32(&writer, channel);  /* channelId */
    radio_request_set_timeout(req, SIM_IO_TIMEOUT_SECS * 1000);
    ok = binder_sim_cbd_io_start(cbd, req);
    radio_request_unref(req);

    if (!ok) {
        struct ofono_error err;

        cb(binder_error_failure(&err), data);
    }
}

static
void
binder_sim_logical_access_get_results_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimSessionCbData* cbd = user_data;
    ofono_sim_logical_access_cb_t cb = cbd->cb;
    struct ofono_error err;

    binder_error_init_failure(&err);
    if (status == RADIO_TX_STATUS_OK) {
        /*
         * iccTransmitApduLogicalChannelResponse(RadioResponseInfo,
         *   IccIoResult result);
         */
        if (resp == RADIO_RESP_ICC_TRANSMIT_APDU_LOGICAL_CHANNEL) {
            BinderSimIoResponse* res = binder_sim_io_response_new(args);

            if (binder_sim_io_response_ok(res) && error == RADIO_ERROR_NONE) {
                cb(binder_error_ok(&err), res->data, res->data_len, cbd->data);
                binder_sim_io_response_free(res);
                return;
            } else if (res) {
                binder_error_init_sim_error(&err, res->sw1, res->sw2);
            }
            binder_sim_io_response_free(res);
        } else {
            ofono_error("Unexpected iccTransmitApduLogicalChannel response %d",
                resp);
        }
    }
    cb(&err, NULL, 0, cbd->data);
}

static
gboolean
binder_sim_logical_access_transmit(
    BinderSimSessionCbData* cbd,
    int ins,
    int p1,
    int p2,
    int p3,
    const char* hex_data,
    RadioRequestCompleteFunc cb)
{
    /* iccTransmitApduLogicalChannel(int32 serial, SimApdu message); */
    BinderSim* self = cbd->self;
    GBinderWriter writer;
    RadioRequest* req = radio_request_new2(self->g,
        RADIO_REQ_ICC_TRANSMIT_APDU_LOGICAL_CHANNEL, &writer,
        cb, binder_sim_session_cbd_unref, cbd);
    RadioSimApdu* apdu = gbinder_writer_new0(&writer, RadioSimApdu);
    gboolean ok;
    guint parent;

    DBG_(self, "session=%u,cmd=%02X,%02X,%02X,%02X,%02X,%s", cbd->channel,
        cbd->cla, ins, p1, p2, p3, hex_data ? hex_data : "");

    apdu->sessionId = cbd->channel;
    apdu->cla = cbd->cla;
    apdu->instruction = ins;
    apdu->p1 = p1;
    apdu->p2 = p2;
    apdu->p3 = p3;

    binder_copy_hidl_string(&writer, &apdu->data, hex_data);
    parent = gbinder_writer_append_buffer_object(&writer, apdu, sizeof(*apdu));
    binder_append_hidl_string_data(&writer, apdu, data, parent);

    radio_request_set_timeout(req, SIM_IO_TIMEOUT_SECS * 1000);
    ok = binder_sim_session_cbd_start(cbd, req);
    radio_request_unref(req);
    return ok;
}

static
void
binder_sim_logical_access_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderSimSessionCbData* cbd = user_data;
    ofono_sim_logical_access_cb_t cb = cbd->cb;
    struct ofono_error err;

    DBG_(cbd->self, "");
    cbd->req_id = 0;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_ICC_TRANSMIT_APDU_LOGICAL_CHANNEL) {
            BinderSimIoResponse* res = binder_sim_io_response_new(args);

            if (res && error == RADIO_ERROR_NONE) {
                /*
                 * TS 102 221
                 * 7.3.1.1.5.2 Case 4 commands
                 *
                 * If the UICC receives a case 4 command, after processing
                 * the data sent with the C-APDU, it shall return:
                 *
                 * a) procedure bytes '61 xx' instructing the transport
                 * layer of the terminal to issue a GET RESPONSE command
                 * with a maximum length of 'xx'; or
                 * b) status indicating a warning or error condition (but
                 * not SW1 SW2 = '90 00').
                 *
                 * The GET RESPONSE command so issued is then treated as
                 * described for case 2 commands.
                 */
                if (res->sw1 == 0x61) {
                    binder_sim_logical_access_transmit(cbd,
                        CMD_GET_RESPONSE, 0, 0, res->sw2, NULL,
                        binder_sim_logical_access_get_results_cb);
                } else if (binder_sim_io_response_ok(res)) {
                    cb(binder_error_ok(&err), res->data, res->data_len,
                        cbd->data);
                } else {
                    cb(binder_error_sim(&err, res->sw1, res->sw2), NULL, 0,
                        cbd->data);
                }
                binder_sim_io_response_free(res);
                return;
            }
            binder_sim_io_response_free(res);
        } else {
            ofono_error("Unexpected iccTransmitApduLogicalChannel response %d",
                resp);
        }
    }
    cb(binder_error_failure(&err), NULL, 0, cbd->data);
}


static
void
binder_sim_logical_access(
    struct ofono_sim* sim,
    int channel,
    const unsigned char* pdu,
    unsigned int len,
    ofono_sim_logical_access_cb_t cb,
    void* data)
{
    BinderSim* self = binder_sim_get_data(sim);
    const char* hex_data;
    char* tmp;
    /* SIM Command APDU: CLA INS P1 P2 P3 Data */
    BinderSimSessionCbData* cbd = binder_sim_session_cbd_new(self, channel,
        pdu[0], cb, data);

    GASSERT(len >= 5);
    if (len > 5) {
        hex_data = tmp = binder_encode_hex(pdu + 5, len - 5);
    } else {
        tmp = NULL;
        hex_data = "";
    }

    binder_sim_logical_access_transmit(cbd, pdu[1], pdu[2], pdu[3], pdu[4],
        hex_data, binder_sim_logical_access_cb);
    binder_sim_session_cbd_unref(cbd);
    g_free(tmp);
}

static
void
binder_sim_session_read_binary(
    struct ofono_sim* sim,
    int session,
    int fileid,
    int start,
    int length,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_read_cb_t cb,
    void* data)
{
    struct ofono_error error;

    ofono_error("session_read_binary not implemented");
    cb(binder_error_failure(&error), NULL, 0, data);
}

static
void
binder_sim_session_read_record(
    struct ofono_sim* sim,
    int channel,
    int fileid,
    int record,
    int length,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_read_cb_t cb,
    void* data)
{
    struct ofono_error error;

    ofono_error("session_read_record not implemented");
    cb(binder_error_failure(&error), NULL, 0, data);
}

static
void
binder_sim_session_read_info(
    struct ofono_sim* sim,
    int channel,
    int fileid,
    const unsigned char* path,
    unsigned int path_len,
    ofono_sim_file_info_cb_t cb,
    void* data)
{
    struct ofono_error error;

    ofono_error("session_read_info not implemented");
    cb(binder_error_failure(&error), -1, -1, -1, NULL, 0, data);
}

static
void
binder_sim_refresh_cb(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* reader,
    gpointer user_data)
{
    BinderSim* self = user_data;

    /*
     * BINDER_UNSOL_SIM_REFRESH may contain the EFID of the updated file,
     * so we could be more descrete here. However I have't actually
     * seen that in real life, let's just refresh everything for now.
     */
    ofono_sim_refresh_full(self->sim);
}

static
gboolean
binder_sim_register(
    gpointer user)
{
    BinderSim* self = user;
    RadioClient* client = self->g->client;

    DBG_(self, "");
    GASSERT(self->idle_id);
    self->idle_id = 0;

    ofono_sim_register(self->sim);

    /* Register for change notifications */
    self->card_event_id[SIM_CARD_STATUS_EVENT] =
        binder_sim_card_add_status_changed_handler(self->card,
            binder_sim_status_changed_cb, self);
    self->card_event_id[SIM_CARD_APP_EVENT] =
        binder_sim_card_add_app_changed_handler(self->card,
            binder_sim_app_changed_cb, self);
    self->sim_state_watch_id =
        ofono_watch_add_sim_state_changed_handler(self->watch,
            binder_sim_state_changed_cb, self);

    /* And IRadio events */
    self->io_event_id[IO_EVENT_SIM_REFRESH] =
        radio_client_add_indication_handler(client,
            RADIO_IND_SIM_REFRESH,
            binder_sim_refresh_cb, self);

    /* Check the current state */
    binder_sim_status_changed_cb(self->card, self);
    return G_SOURCE_REMOVE;
}

static
int
binder_sim_probe(
    struct ofono_sim* sim,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderSim* self = g_new0(BinderSim, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    self->empty_pin_query_allowed = modem->config.empty_pin_query;
    self->card = binder_sim_card_ref(modem->sim_card);
    self->g = radio_request_group_new(modem->client); /* Keeps ref to client */
    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->sim = sim;

    DBG_(self, "");
    binder_sim_invalidate_passwd_state(self);
    self->idle_id = g_idle_add(binder_sim_register, self);
    ofono_sim_set_data(sim, self);
    return 0;
}

static void binder_sim_remove(struct ofono_sim *sim)
{
    BinderSim* self = binder_sim_get_data(sim);

    DBG_(self, "");

    g_list_free_full(self->pin_cbd_list, (GDestroyNotify)
        binder_sim_pin_cbd_free);

    radio_client_remove_all_handlers(self->g->client, self->io_event_id);
    radio_request_drop(self->query_pin_retries_req);
    radio_request_group_cancel(self->g);
    radio_request_group_unref(self->g);

    if (self->list_apps_id) {
        g_source_remove(self->list_apps_id);
    }

    if (self->idle_id) {
        g_source_remove(self->idle_id);
    }

    if (self->query_passwd_state_timeout_id) {
        g_source_remove(self->query_passwd_state_timeout_id);
    }

    if (self->query_passwd_state_sim_status_refresh_id) {
        binder_sim_card_remove_handler(self->card,
            self->query_passwd_state_sim_status_refresh_id);
    }

    ofono_watch_remove_handler(self->watch, self->sim_state_watch_id);
    ofono_watch_unref(self->watch);

    binder_sim_card_remove_all_handlers(self->card, self->card_event_id);
    binder_sim_card_unref(self->card);

    g_free(self->log_prefix);
    g_free(self);

    ofono_sim_set_data(sim, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_sim_driver binder_sim_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_sim_probe,
    .remove                 = binder_sim_remove,
    .read_file_info         = binder_sim_ofono_read_file_info,
    .read_file_transparent  = binder_sim_ofono_read_file_transparent,
    .read_file_linear       = binder_sim_ofono_read_file_linear,
    .read_file_cyclic       = binder_sim_ofono_read_file_cyclic,
    .write_file_transparent = binder_sim_write_file_transparent,
    .write_file_linear      = binder_sim_write_file_linear,
    .write_file_cyclic      = binder_sim_write_file_cyclic,
    .read_imsi              = binder_sim_read_imsi,
    .query_passwd_state     = binder_sim_query_passwd_state,
    .send_passwd            = binder_sim_pin_send,
    .lock                   = binder_sim_pin_change_state,
    .reset_passwd           = binder_sim_pin_send_puk,
    .change_passwd          = binder_sim_change_passwd,
    .query_pin_retries      = binder_sim_query_pin_retries,
    .query_facility_lock    = binder_sim_query_facility_lock,
    .list_apps              = binder_sim_list_apps,
    .open_channel2          = binder_sim_open_channel,
    .close_channel          = binder_sim_close_channel,
    .session_read_binary    = binder_sim_session_read_binary,
    .session_read_record    = binder_sim_session_read_record,
    .session_read_info      = binder_sim_session_read_info,
    .logical_access         = binder_sim_logical_access
};

void
binder_sim_init()
{
    ofono_sim_driver_register(&binder_sim_driver);
}

void
binder_sim_cleanup()
{
    ofono_sim_driver_unregister(&binder_sim_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
