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

#include "binder_cell_info.h"
#include "binder_sim_card.h"
#include "binder_radio.h"
#include "binder_util.h"
#include "binder_log.h"

#include <radio_client.h>
#include <radio_request.h>
#include <radio_util.h>

#include <gbinder_reader.h>
#include <gbinder_writer.h>

#include <gutil_idlepool.h>
#include <gutil_macros.h>
#include <gutil_misc.h>

#define DEFAULT_UPDATE_RATE_MS  (10000) /* 10 sec */
#define MAX_RETRIES             (5)

enum binder_cell_info_event {
    CELL_INFO_EVENT_1_0,
    CELL_INFO_EVENT_1_2,
    CELL_INFO_EVENT_1_4,
    CELL_INFO_EVENT_1_5,
    CELL_INFO_EVENT_COUNT
};

typedef GObjectClass BinderCellInfoClass;
typedef struct binder_cell_info {
    GObject object;
    struct ofono_cell_info info;
    struct ofono_cell **cells;
    RadioClient* client;
    BinderRadio* radio;
    BinderSimCard* sim_card;
    gulong radio_state_event_id;
    gulong sim_status_event_id;
    gboolean sim_card_ready;
    int update_rate_ms;
    char* log_prefix;
    gulong event_id[CELL_INFO_EVENT_COUNT];
    RadioRequest* query_req;
    RadioRequest* set_rate_req;
    gboolean enabled;
} BinderCellInfo;

enum binder_cell_info_signal {
    SIGNAL_CELLS_CHANGED,
    SIGNAL_COUNT
};

#define SIGNAL_CELLS_CHANGED_NAME   "binder-cell-info-cells-changed"

static GUtilIdlePool* binder_cell_info_pool = NULL;
static guint binder_cell_info_signals[SIGNAL_COUNT] = { 0 };

G_DEFINE_TYPE(BinderCellInfo, binder_cell_info, G_TYPE_OBJECT)
#define THIS_TYPE (binder_cell_info_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, BinderCellInfo))
#define PARENT_CLASS binder_cell_info_parent_class

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

/*
 * binder_cell_info_list_equal() assumes that zero-initialized
 * struct ofono_cell gets allocated regardless of the cell type,
 * even if a part of the structure remains unused.
 */

#define binder_cell_new() g_new0(struct ofono_cell, 1)

static
const char*
binder_cell_info_int_format(
    int value,
    const char* format)
{
    if (value == OFONO_CELL_INVALID_VALUE) {
        return "";
    } else {
        GUtilIdlePool* pool = gutil_idle_pool_get(&binder_cell_info_pool);
        char* str = g_strdup_printf(format, value);

        gutil_idle_pool_add(pool, str, g_free);
        return str;
    }
}

static
const char*
binder_cell_info_int64_format(
    guint64 value,
    const char* format)
{
    if (value == OFONO_CELL_INVALID_VALUE_INT64) {
        return "";
    } else {
        GUtilIdlePool* pool = gutil_idle_pool_get(&binder_cell_info_pool);
        char* str = g_strdup_printf(format, value);

        gutil_idle_pool_add(pool, str, g_free);
        return str;
    }
}

static
gint
binder_cell_info_list_compare(
    gconstpointer a,
    gconstpointer b)
{
    return ofono_cell_compare_location(*(struct ofono_cell**)a,
        *(struct ofono_cell**)b);
}

static
gboolean
binder_cell_info_list_equal(
    const ofono_cell_ptr* l1,
    const ofono_cell_ptr* l2)
{
    if (l1 && l2) {
        while (*l1 && *l2) {
            if (memcmp(*l1, *l2, sizeof(struct ofono_cell))) {
                return FALSE;
            }
            l1++;
            l2++;
        }
        return !*l1 && !*l2;
    } else {
        return (!l1 || !*l1) && (!l2 || !*l2);
    }
}

static
void
binder_cell_info_clear(
    BinderCellInfo* self)
{
    if (self->cells && self->cells[0]) {
        gutil_ptrv_free((void**)self->cells);
        self->info.cells = self->cells = g_new0(struct ofono_cell*, 1);
        g_signal_emit(self, binder_cell_info_signals[SIGNAL_CELLS_CHANGED], 0);
    }
}

/* NULL-terminates and takes ownership of GPtrArray */
static
void
binder_cell_info_update_cells(
    BinderCellInfo* self,
    GPtrArray* l)
{
    if (l) {
        g_ptr_array_sort(l, binder_cell_info_list_compare);
        g_ptr_array_add(l, NULL);

        DBG_(self, "%d cell(s)", (int)(l->len - 1));
        if (!binder_cell_info_list_equal(self->cells,
           (struct ofono_cell**)l->pdata)) {
            gutil_ptrv_free((void**)self->cells);
            self->info.cells = self->cells = (struct ofono_cell **)
                g_ptr_array_free(l, FALSE);
            g_signal_emit(self, binder_cell_info_signals
                [SIGNAL_CELLS_CHANGED], 0);
        } else {
            g_ptr_array_set_free_func(l, g_free);
            g_ptr_array_free(l, TRUE);
        }
    }
}

static
void
binder_cell_info_invalidate(
    void* info,
    gsize size)
{
    const int n = size/sizeof(int);
    int* value = info;
    int i;

    for (i = 0; i < n; i++) {
        *value++ = OFONO_CELL_INVALID_VALUE;
    }
}

static
void
binder_cell_info_invalidate_nr(
    struct ofono_cell_info_nr* nr)
{
    nr->mcc = OFONO_CELL_INVALID_VALUE;
    nr->mnc = OFONO_CELL_INVALID_VALUE;
    nr->nci = OFONO_CELL_INVALID_VALUE_INT64;
    nr->pci = OFONO_CELL_INVALID_VALUE;
    nr->tac = OFONO_CELL_INVALID_VALUE;
    nr->nrarfcn = OFONO_CELL_INVALID_VALUE;
    nr->ssRsrp = OFONO_CELL_INVALID_VALUE;
    nr->ssRsrq = OFONO_CELL_INVALID_VALUE;
    nr->ssSinr = OFONO_CELL_INVALID_VALUE;
    nr->csiRsrp = OFONO_CELL_INVALID_VALUE;
    nr->csiRsrq = OFONO_CELL_INVALID_VALUE;
    nr->csiSinr = OFONO_CELL_INVALID_VALUE;
}

static
struct ofono_cell*
binder_cell_info_new_cell_gsm(
    gboolean registered,
    const RadioCellIdentityGsm* id,
    const RadioSignalStrengthGsm* ss)
{
    struct ofono_cell* cell = binder_cell_new();
    struct ofono_cell_info_gsm* gsm = &cell->info.gsm;

    cell->type = OFONO_CELL_TYPE_GSM;
    cell->registered = registered;

    binder_cell_info_invalidate(gsm, sizeof(*gsm));
    gutil_parse_int(id->mcc.data.str, 10, &gsm->mcc);
    gutil_parse_int(id->mnc.data.str, 10, &gsm->mnc);
    gsm->lac = id->lac;
    gsm->cid = id->cid;
    gsm->arfcn = id->arfcn;
    gsm->bsic = id->bsic;
    gsm->signalStrength = ss->signalStrength;
    gsm->bitErrorRate = ss->bitErrorRate;
    gsm->timingAdvance = ss->timingAdvance;
    DBG("[gsm] reg=%d%s%s%s%s%s%s%s%s%s", registered,
        binder_cell_info_int_format(gsm->mcc, ",mcc=%d"),
        binder_cell_info_int_format(gsm->mnc, ",mnc=%d"),
        binder_cell_info_int_format(gsm->lac, ",lac=%d"),
        binder_cell_info_int_format(gsm->cid, ",cid=%d"),
        binder_cell_info_int_format(gsm->arfcn, ",arfcn=%d"),
        binder_cell_info_int_format(gsm->bsic, ",bsic=%d"),
        binder_cell_info_int_format(gsm->signalStrength, ",strength=%d"),
        binder_cell_info_int_format(gsm->bitErrorRate, ",err=%d"),
        binder_cell_info_int_format(gsm->timingAdvance, ",t=%d"));
    return cell;
}

static
struct
ofono_cell*
binder_cell_info_new_cell_wcdma(
    gboolean registered,
    const RadioCellIdentityWcdma* id,
    const RadioSignalStrengthWcdma* ss)
{
    struct ofono_cell* cell = binder_cell_new();
    struct ofono_cell_info_wcdma* wcdma = &cell->info.wcdma;

    cell->type = OFONO_CELL_TYPE_WCDMA;
    cell->registered = registered;

    binder_cell_info_invalidate(wcdma, sizeof(*wcdma));
    gutil_parse_int(id->mcc.data.str, 10, &wcdma->mcc);
    gutil_parse_int(id->mnc.data.str, 10, &wcdma->mnc);
    wcdma->lac = id->lac;
    wcdma->cid = id->cid;
    wcdma->psc = id->psc;
    wcdma->uarfcn = id->uarfcn;
    wcdma->signalStrength = ss->signalStrength;
    wcdma->bitErrorRate = ss->bitErrorRate;
    DBG("[wcdma] reg=%d%s%s%s%s%s%s%s", registered,
        binder_cell_info_int_format(wcdma->mcc, ",mcc=%d"),
        binder_cell_info_int_format(wcdma->mnc, ",mnc=%d"),
        binder_cell_info_int_format(wcdma->lac, ",lac=%d"),
        binder_cell_info_int_format(wcdma->cid, ",cid=%d"),
        binder_cell_info_int_format(wcdma->psc, ",psc=%d"),
        binder_cell_info_int_format(wcdma->signalStrength, ",strength=%d"),
        binder_cell_info_int_format(wcdma->bitErrorRate, ",err=%d"));
    return cell;
}

static
struct ofono_cell*
binder_cell_info_new_cell_lte(
    gboolean registered,
    const RadioCellIdentityLte* id,
    const RadioSignalStrengthLte* ss)
{
    struct ofono_cell* cell = binder_cell_new();
    struct ofono_cell_info_lte* lte = &cell->info.lte;

    cell->type = OFONO_CELL_TYPE_LTE;
    cell->registered = registered;

    binder_cell_info_invalidate(lte, sizeof(*lte));
    gutil_parse_int(id->mcc.data.str, 10, &lte->mcc);
    gutil_parse_int(id->mnc.data.str, 10, &lte->mnc);
    lte->ci = id->ci;
    lte->pci = id->pci;
    lte->tac = id->tac;
    lte->earfcn = id->earfcn;
    lte->signalStrength = ss->signalStrength;
    lte->rsrp = ss->rsrp;
    lte->rsrq = ss->rsrq;
    lte->rssnr = ss->rssnr;
    lte->cqi = ss->cqi;
    lte->timingAdvance = ss->timingAdvance;
    DBG("[lte] reg=%d%s%s%s%s%s%s%s%s%s%s%s", registered,
        binder_cell_info_int_format(lte->mcc, ",mcc=%d"),
        binder_cell_info_int_format(lte->mnc, ",mnc=%d"),
        binder_cell_info_int_format(lte->ci, ",ci=%d"),
        binder_cell_info_int_format(lte->pci, ",pci=%d"),
        binder_cell_info_int_format(lte->tac, ",tac=%d"),
        binder_cell_info_int_format(lte->signalStrength, ",strength=%d"),
        binder_cell_info_int_format(lte->rsrp, ",rsrp=%d"),
        binder_cell_info_int_format(lte->rsrq, ",rsrq=%d"),
        binder_cell_info_int_format(lte->rssnr, ",rssnr=%d"),
        binder_cell_info_int_format(lte->cqi, ",cqi=%d"),
        binder_cell_info_int_format(lte->timingAdvance, ",t=%d"));
    return cell;
}
static
struct ofono_cell*
binder_cell_info_new_cell_nr(
    gboolean registered,
    const RadioCellIdentityNr* id,
    const RadioSignalStrengthNr* ss)
{
    struct ofono_cell* cell = binder_cell_new();
    struct ofono_cell_info_nr* nr = &cell->info.nr;

    cell->type = OFONO_CELL_TYPE_NR;
    cell->registered = registered;

    binder_cell_info_invalidate_nr(nr);
    gutil_parse_int(id->mcc.data.str, 10, &nr->mcc);
    gutil_parse_int(id->mnc.data.str, 10, &nr->mnc);
    nr->nci = id->nci;
    nr->pci = id->pci;
    nr->tac = id->tac;
    nr->nrarfcn = id->nrarfcn;
    nr->ssRsrp = ss->ssRsrp;
    nr->ssRsrq = ss->ssRsrq;
    nr->ssSinr = ss->ssSinr;
    nr->csiRsrp = ss->csiRsrp;
    nr->csiRsrq = ss->csiRsrq;
    nr->csiSinr = ss->csiSinr;
    DBG("[nr] reg=%d%s%s%s%s%s%s%s%s%s%s%s", registered,
        binder_cell_info_int_format(nr->mcc, ",mcc=%d"),
        binder_cell_info_int_format(nr->mnc, ",mnc=%d"),
        binder_cell_info_int64_format(nr->nci, ",nci=%" G_GINT64_FORMAT),
        binder_cell_info_int_format(nr->pci, ",pci=%d"),
        binder_cell_info_int_format(nr->tac, ",tac=%d"),
        binder_cell_info_int_format(nr->ssRsrp, ",ssRsrp=%d"),
        binder_cell_info_int_format(nr->ssRsrq, ",ssRsrq=%d"),
        binder_cell_info_int_format(nr->ssSinr, ",ssSinr=%d"),
        binder_cell_info_int_format(nr->csiRsrp, ",csiRsrp=%d"),
        binder_cell_info_int_format(nr->csiRsrq, ",csiRsrq=%d"),
        binder_cell_info_int_format(nr->csiSinr, ",csiSinr=%d"));
    return cell;
}

static
GPtrArray*
binder_cell_info_array_new_1_0(
    const RadioCellInfo* cells,
    gsize count)
{
    gsize i;
    GPtrArray* l = g_ptr_array_sized_new(count + 1);

    for (i = 0; i < count; i++) {
        const RadioCellInfo* cell = cells + i;
        const gboolean reg = cell->registered;
        const RadioCellInfoGsm* gsm;
        const RadioCellInfoLte* lte;
        const RadioCellInfoWcdma* wcdma;
        guint j;

        switch (cell->cellInfoType) {
        case RADIO_CELL_INFO_GSM:
            gsm = cell->gsm.data.ptr;
            for (j = 0; j < cell->gsm.count; j++) {
                g_ptr_array_add(l, binder_cell_info_new_cell_gsm(reg,
                    &gsm[j].cellIdentityGsm,
                    &gsm[j].signalStrengthGsm));
            }
            continue;
        case RADIO_CELL_INFO_LTE:
            lte = cell->lte.data.ptr;
            for (j = 0; j < cell->lte.count; j++) {
                g_ptr_array_add(l, binder_cell_info_new_cell_lte(reg,
                    &lte[j].cellIdentityLte,
                    &lte[j].signalStrengthLte));
            }
            continue;
        case RADIO_CELL_INFO_WCDMA:
            wcdma = cell->wcdma.data.ptr;
            for (j = 0; j < cell->wcdma.count; j++) {
                g_ptr_array_add(l, binder_cell_info_new_cell_wcdma(reg,
                    &wcdma[j].cellIdentityWcdma,
                    &wcdma[j].signalStrengthWcdma));
            }
            continue;
        case RADIO_CELL_INFO_CDMA:
        case RADIO_CELL_INFO_TD_SCDMA:
            break;
        }
        DBG("unsupported cell type %d", cell->cellInfoType);
    }
    return l;
}

static
GPtrArray*
binder_cell_info_array_new_1_2(
    const RadioCellInfo_1_2* cells,
    gsize count)
{
    gsize i;
    GPtrArray* l = g_ptr_array_sized_new(count + 1);

    for (i = 0; i < count; i++) {
        const RadioCellInfo_1_2* cell = cells + i;
        const gboolean registered = cell->registered;
        const RadioCellInfoGsm_1_2* gsm;
        const RadioCellInfoLte_1_2* lte;
        const RadioCellInfoWcdma_1_2* wcdma;
        guint j;

        switch (cell->cellInfoType) {
        case RADIO_CELL_INFO_GSM:
            gsm = cell->gsm.data.ptr;
            for (j = 0; j < cell->gsm.count; j++) {
                g_ptr_array_add(l, binder_cell_info_new_cell_gsm(registered,
                    &gsm[j].cellIdentityGsm.base,
                    &gsm[j].signalStrengthGsm));
            }
            continue;
        case RADIO_CELL_INFO_LTE:
            lte = cell->lte.data.ptr;
            for (j = 0; j < cell->lte.count; j++) {
                g_ptr_array_add(l, binder_cell_info_new_cell_lte(registered,
                    &lte[j].cellIdentityLte.base,
                    &lte[j].signalStrengthLte));
            }
            continue;
        case RADIO_CELL_INFO_WCDMA:
            wcdma = cell->wcdma.data.ptr;
            for (j = 0; j < cell->wcdma.count; j++) {
                g_ptr_array_add(l, binder_cell_info_new_cell_wcdma(registered,
                    &wcdma[j].cellIdentityWcdma.base,
                    &wcdma[j].signalStrengthWcdma.base));
            }
            continue;
        case RADIO_CELL_INFO_CDMA:
        case RADIO_CELL_INFO_TD_SCDMA:
            break;
        }
        DBG("unsupported cell type %d", cell->cellInfoType);
    }
    return l;
}

static
GPtrArray*
binder_cell_info_array_new_1_4(
    const RadioCellInfo_1_4* cells,
    gsize count)
{
    gsize i;
    GPtrArray* l = g_ptr_array_sized_new(count + 1);

    for (i = 0; i < count; i++) {
        const RadioCellInfo_1_4* cell = cells + i;
        const gboolean registered = cell->registered;

        switch ((RADIO_CELL_INFO_TYPE_1_4)cell->cellInfoType) {
        case RADIO_CELL_INFO_1_4_GSM:
            g_ptr_array_add(l, binder_cell_info_new_cell_gsm(registered,
                &cell->info.gsm.cellIdentityGsm.base,
                &cell->info.gsm.signalStrengthGsm));
            continue;
        case RADIO_CELL_INFO_1_4_LTE:
            g_ptr_array_add(l, binder_cell_info_new_cell_lte(registered,
                &cell->info.lte.base.cellIdentityLte.base,
                &cell->info.lte.base.signalStrengthLte));
            continue;
        case RADIO_CELL_INFO_1_4_WCDMA:
            g_ptr_array_add(l, binder_cell_info_new_cell_wcdma(registered,
                &cell->info.wcdma.cellIdentityWcdma.base,
                &cell->info.wcdma.signalStrengthWcdma.base));
            continue;
        case RADIO_CELL_INFO_1_4_NR:
            g_ptr_array_add(l, binder_cell_info_new_cell_nr(registered,
                &cell->info.nr.cellIdentity,
                &cell->info.nr.signalStrength));
            continue;
        case RADIO_CELL_INFO_1_4_TD_SCDMA:
        case RADIO_CELL_INFO_1_4_CDMA:
            break;
        }
        DBG("unsupported cell type %d", cell->cellInfoType);
    }
    return l;
}

static
GPtrArray*
binder_cell_info_array_new_1_5(
    const RadioCellInfo_1_5* cells,
    gsize count)
{
    gsize i;
    GPtrArray* l = g_ptr_array_sized_new(count + 1);

    for (i = 0; i < count; i++) {
        const RadioCellInfo_1_5* cell = cells + i;
        const gboolean registered = cell->registered;

        switch ((RADIO_CELL_INFO_TYPE_1_5)cell->cellInfoType) {
        case RADIO_CELL_INFO_1_5_GSM:
            g_ptr_array_add(l, binder_cell_info_new_cell_gsm(registered,
                &cell->info.gsm.cellIdentityGsm.base.base,
                &cell->info.gsm.signalStrengthGsm));
            continue;
        case RADIO_CELL_INFO_1_5_LTE:
            g_ptr_array_add(l, binder_cell_info_new_cell_lte(registered,
                &cell->info.lte.cellIdentityLte.base.base,
                &cell->info.lte.signalStrengthLte));
            continue;
        case RADIO_CELL_INFO_1_5_WCDMA:
            g_ptr_array_add(l, binder_cell_info_new_cell_wcdma(registered,
                &cell->info.wcdma.cellIdentityWcdma.base.base,
                &cell->info.wcdma.signalStrengthWcdma.base));
            continue;
        case RADIO_CELL_INFO_1_5_NR:
            g_ptr_array_add(l, binder_cell_info_new_cell_nr(registered,
                &cell->info.nr.cellIdentityNr.base,
                &cell->info.nr.signalStrengthNr));
            continue;
        case RADIO_CELL_INFO_1_5_TD_SCDMA:
        case RADIO_CELL_INFO_1_5_CDMA:
            break;
        }
        DBG("unsupported cell type %d", cell->cellInfoType);
    }
    return l;
}

static
void
binder_cell_info_list_1_0(
    BinderCellInfo* self,
    GBinderReader* reader)
{
    gsize count;
    const RadioCellInfo* cells = gbinder_reader_read_hidl_type_vec(reader,
        RadioCellInfo, &count);

    if (cells) {
        binder_cell_info_update_cells(self,
            binder_cell_info_array_new_1_0(cells, count));
    } else {
        ofono_warn("Failed to parse cellInfoList payload");
    }
}

static
void
binder_cell_info_list_1_2(
    BinderCellInfo* self,
    GBinderReader* reader)
{
    gsize count;
    const RadioCellInfo_1_2* cells = gbinder_reader_read_hidl_type_vec(reader,
        RadioCellInfo_1_2, &count);

    if (cells) {
        binder_cell_info_update_cells(self,
            binder_cell_info_array_new_1_2(cells, count));
    } else {
        ofono_warn("Failed to parse cellInfoList_1_2 payload");
    }
}

static
void
binder_cell_info_list_1_4(
    BinderCellInfo* self,
    GBinderReader* reader)
{
    gsize count;
    const RadioCellInfo_1_4* cells = gbinder_reader_read_hidl_type_vec(reader,
        RadioCellInfo_1_4, &count);

    if (cells) {
        binder_cell_info_update_cells(self,
            binder_cell_info_array_new_1_4(cells, count));
    } else {
        ofono_warn("Failed to parse cellInfoList_1_4 payload");
    }
}

static
void
binder_cell_info_list_1_5(
    BinderCellInfo* self,
    GBinderReader* reader)
{
    gsize count;
    const RadioCellInfo_1_5* cells = gbinder_reader_read_hidl_type_vec(reader,
        RadioCellInfo_1_5, &count);

    if (cells) {
        binder_cell_info_update_cells(self,
            binder_cell_info_array_new_1_5(cells, count));
    } else {
        ofono_warn("Failed to parse cellInfoList_1_5 payload");
    }
}

static
void
binder_cell_info_list_changed_1_0(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCellInfo* self = THIS(user_data);

    GASSERT(code == RADIO_IND_CELL_INFO_LIST);
    if (self->enabled) {
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        binder_cell_info_list_1_0(self, &reader);
    }
}

static
void
binder_cell_info_list_changed_1_2(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCellInfo* self = THIS(user_data);

    GASSERT(code == RADIO_IND_CELL_INFO_LIST_1_2);
    if (self->enabled) {
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        binder_cell_info_list_1_2(self, &reader);
    }
}

static
void
binder_cell_info_list_changed_1_4(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCellInfo* self = THIS(user_data);

    GASSERT(code == RADIO_IND_CELL_INFO_LIST_1_4);
    if (self->enabled) {
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        binder_cell_info_list_1_4(self, &reader);
    }
}

static
void
binder_cell_info_list_changed_1_5(
    RadioClient* client,
    RADIO_IND code,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCellInfo* self = THIS(user_data);

    GASSERT(code == RADIO_IND_CELL_INFO_LIST_1_5);
    if (self->enabled) {
        GBinderReader reader;

        gbinder_reader_copy(&reader, args);
        binder_cell_info_list_1_5(self, &reader);
    }
}

static
void
binder_cell_info_list_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCellInfo* self = THIS(user_data);

    GASSERT(self->query_req == req);
    radio_request_drop(self->query_req);
    self->query_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (error == RADIO_ERROR_NONE) {
            if (self->enabled) {
                GBinderReader reader;

                gbinder_reader_copy(&reader, args);
                switch (resp) {
                case RADIO_RESP_GET_CELL_INFO_LIST:
                    binder_cell_info_list_1_0(self, &reader);
                    break;
                case RADIO_RESP_GET_CELL_INFO_LIST_1_2:
                    binder_cell_info_list_1_2(self, &reader);
                    break;
                case RADIO_RESP_GET_CELL_INFO_LIST_1_4:
                    binder_cell_info_list_1_4(self, &reader);
                    break;
                case RADIO_RESP_GET_CELL_INFO_LIST_1_5:
                    binder_cell_info_list_1_5(self, &reader);
                    break;
                default:
                    ofono_warn("Unexpected getCellInfoList response %d", resp);
                    break;
                }
            }
        } else {
            DBG_(self, "%s error %d", radio_resp_name(resp), error);
        }
    }
}

static
void
binder_cell_info_set_rate_cb(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    gpointer user_data)
{
    BinderCellInfo* self = THIS(user_data);

    DBG_(self, "");
    GASSERT(self->set_rate_req == req);
    radio_request_drop(self->set_rate_req);
    self->set_rate_req = NULL;

    if (status == RADIO_TX_STATUS_OK) {
        if (resp == RADIO_RESP_SET_CELL_INFO_LIST_RATE) {
            if (error != RADIO_ERROR_NONE) {
                DBG_(self, "Failed to set cell info rate, error %d", error);
            }
        } else {
            ofono_error("Unexpected setCellInfoListRate response %d", resp);
        }
    }
}

static
gboolean
binder_cell_info_retry(
    RadioRequest* req,
    RADIO_TX_STATUS status,
    RADIO_RESP resp,
    RADIO_ERROR error,
    const GBinderReader* args,
    void* user_data)
{
    BinderCellInfo* self = THIS(user_data);

    switch (error) {
    case RADIO_ERROR_NONE:
    case RADIO_ERROR_RADIO_NOT_AVAILABLE:
        return FALSE;
    default:
        return self->enabled;
    }
}

static
void
binder_cell_info_query(
    BinderCellInfo* self)
{
    radio_request_drop(self->query_req);
    self->query_req = radio_request_new(self->client,
        RADIO_REQ_GET_CELL_INFO_LIST, NULL,
        binder_cell_info_list_cb, NULL, self);
    radio_request_set_retry(self->query_req, BINDER_RETRY_MS, MAX_RETRIES);
    radio_request_set_retry_func(self->query_req, binder_cell_info_retry);
    radio_request_submit(self->query_req);
}

static
void
binder_cell_info_set_rate(
    BinderCellInfo* self)
{
    GBinderWriter writer;

    radio_request_drop(self->set_rate_req);
    self->set_rate_req = radio_request_new(self->client,
        RADIO_REQ_SET_CELL_INFO_LIST_RATE, &writer,
        binder_cell_info_set_rate_cb, NULL, self);

    gbinder_writer_append_int32(&writer,
        (self->update_rate_ms >= 0 && self->enabled) ?
            self->update_rate_ms : INT_MAX);

    radio_request_set_retry(self->set_rate_req, BINDER_RETRY_MS, MAX_RETRIES);
    radio_request_set_retry_func(self->set_rate_req, binder_cell_info_retry);
    radio_request_submit(self->set_rate_req);
}

static
void
binder_cell_info_refresh(
    BinderCellInfo* self)
{
    /* getCellInfoList fails without SIM card */
    if (self->enabled &&
        self->radio->state == RADIO_STATE_ON &&
        self->sim_card_ready) {
        binder_cell_info_query(self);
    } else {
        binder_cell_info_clear(self);
    }
}

static
void
binder_cell_info_radio_state_cb(
    BinderRadio* radio,
    BINDER_RADIO_PROPERTY property,
    void* user_data)
{
    BinderCellInfo* self = THIS(user_data);

    DBG_(self, "%s", binder_radio_state_string(radio->state));
    binder_cell_info_refresh(self);
}

static
void
binder_cell_info_sim_status_cb(
    BinderSimCard* sim,
    void* user_data)
{
    BinderCellInfo* self = THIS(user_data);

    self->sim_card_ready = binder_sim_card_ready(sim);
    DBG_(self, "%sready", self->sim_card_ready ? "" : "not ");
    binder_cell_info_refresh(self);
    if (self->sim_card_ready) {
        binder_cell_info_set_rate(self);
    }
}

/*==========================================================================*
 * ofono_cell_info interface
 *==========================================================================*/

typedef struct binder_cell_info_closure {
    GCClosure cclosure;
    ofono_cell_info_cb_t cb;
    void* user_data;
} BinderCellInfoClosure;

static inline BinderCellInfo* binder_cell_info_cast(struct ofono_cell_info* info)
    { return G_CAST(info, BinderCellInfo, info); }

static
void
binder_cell_info_ref_proc(
    struct ofono_cell_info* info)
{
    g_object_ref(binder_cell_info_cast(info));
}

static
void
binder_cell_info_unref_proc(
    struct ofono_cell_info* info)
{
    g_object_unref(binder_cell_info_cast(info));
}

static
void
binder_cell_info_cells_changed_cb(
    BinderCellInfo* self,
    BinderCellInfoClosure* closure)
{
    closure->cb(&self->info, closure->user_data);
}

static
gulong
binder_cell_info_add_cells_changed_handler_proc(
    struct ofono_cell_info* info,
    ofono_cell_info_cb_t cb,
    void* user_data)
{
    if (cb) {
        BinderCellInfoClosure* closure = (BinderCellInfoClosure *)
            g_closure_new_simple(sizeof(BinderCellInfoClosure), NULL);
        GCClosure* cc = &closure->cclosure;

        cc->closure.data = closure;
        cc->callback = G_CALLBACK(binder_cell_info_cells_changed_cb);
        closure->cb = cb;
        closure->user_data = user_data;
        return g_signal_connect_closure_by_id(binder_cell_info_cast(info),
            binder_cell_info_signals[SIGNAL_CELLS_CHANGED], 0,
            &cc->closure, FALSE);
    } else {
        return 0;
    }
}

static
void
binder_cell_info_remove_handler_proc(
    struct ofono_cell_info* info,
    gulong id)
{
    if (G_LIKELY(id)) {
        g_signal_handler_disconnect(binder_cell_info_cast(info), id);
    }
}

static
void
binder_cell_info_set_update_interval_proc(
    struct ofono_cell_info* info,
    int ms)
{
    BinderCellInfo* self = binder_cell_info_cast(info);

    if (self->update_rate_ms != ms) {
        self->update_rate_ms = ms;
        DBG_(self, "%d ms", ms);
        if (self->enabled && self->sim_card_ready) {
            binder_cell_info_set_rate(self);
        }
    }
}

static
void
binder_cell_info_set_enabled_proc(
    struct ofono_cell_info* info,
    gboolean enabled)
{
    BinderCellInfo* self = binder_cell_info_cast(info);

    if (self->enabled != enabled) {
        self->enabled = enabled;
        DBG_(self, "%d", enabled);
        binder_cell_info_refresh(self);
        if (self->sim_card_ready) {
            binder_cell_info_set_rate(self);
        }
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

struct ofono_cell_info*
binder_cell_info_new(
    RadioClient* client,
    const char* log_prefix,
    BinderRadio* radio,
    BinderSimCard* sim)
{
    BinderCellInfo* self = g_object_new(THIS_TYPE, 0);

    self->client = radio_client_ref(client);
    self->radio = binder_radio_ref(radio);
    self->sim_card = binder_sim_card_ref(sim);
    self->log_prefix = binder_dup_prefix(log_prefix);

    DBG_(self, "");
    self->event_id[CELL_INFO_EVENT_1_0] =
        radio_client_add_indication_handler(client,
            RADIO_IND_CELL_INFO_LIST,
            binder_cell_info_list_changed_1_0, self);
    self->event_id[CELL_INFO_EVENT_1_2] =
        radio_client_add_indication_handler(client,
            RADIO_IND_CELL_INFO_LIST_1_2,
            binder_cell_info_list_changed_1_2, self);
    self->event_id[CELL_INFO_EVENT_1_4] =
        radio_client_add_indication_handler(client,
            RADIO_IND_CELL_INFO_LIST_1_4,
            binder_cell_info_list_changed_1_4, self);
    self->event_id[CELL_INFO_EVENT_1_5] =
        radio_client_add_indication_handler(client,
            RADIO_IND_CELL_INFO_LIST_1_5,
            binder_cell_info_list_changed_1_5, self);
    self->radio_state_event_id =
        binder_radio_add_property_handler(radio,
            BINDER_RADIO_PROPERTY_STATE,
            binder_cell_info_radio_state_cb, self);
    self->sim_status_event_id =
        binder_sim_card_add_status_changed_handler(sim,
            binder_cell_info_sim_status_cb, self);
    self->sim_card_ready = binder_sim_card_ready(sim);
    binder_cell_info_refresh(self);

    /* Disable updates by default */
    self->enabled = FALSE;
    if (self->sim_card_ready) {
        binder_cell_info_set_rate(self);
    }
    return &self->info;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
binder_cell_info_init(
    BinderCellInfo* self)
{
    static const struct ofono_cell_info_proc binder_cell_info_proc = {
        binder_cell_info_ref_proc,
        binder_cell_info_unref_proc,
        binder_cell_info_add_cells_changed_handler_proc,
        binder_cell_info_remove_handler_proc,
        binder_cell_info_set_update_interval_proc,
        binder_cell_info_set_enabled_proc
    };

    self->update_rate_ms = DEFAULT_UPDATE_RATE_MS;
    self->info.cells = self->cells = g_new0(struct ofono_cell*, 1);
    self->info.proc = &binder_cell_info_proc;
}

static
void
binder_cell_info_finalize(
    GObject* object)
{
    BinderCellInfo* self = THIS(object);

    DBG_(self, "");
    radio_request_drop(self->query_req);
    radio_request_drop(self->set_rate_req);
    radio_client_remove_all_handlers(self->client, self->event_id);
    radio_client_unref(self->client);
    binder_radio_remove_handler(self->radio, self->radio_state_event_id);
    binder_radio_unref(self->radio);
    binder_sim_card_remove_handler(self->sim_card, self->sim_status_event_id);
    binder_sim_card_unref(self->sim_card);
    gutil_ptrv_free((void**)self->cells);
    g_free(self->log_prefix);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
binder_cell_info_class_init(
    BinderCellInfoClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = binder_cell_info_finalize;
    binder_cell_info_signals[SIGNAL_CELLS_CHANGED] =
        g_signal_new(SIGNAL_CELLS_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
