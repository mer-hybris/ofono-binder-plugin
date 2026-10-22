/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2026 Jolla Mobile Ltd
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

#include "binder_oplist.h"

#include <ofono/netreg.h>

#define OP(name,mcc,mnc,tech) \
    { name, mcc, mnc, OFONO_OPERATOR_STATUS_UNKNOWN, tech }
#define NR OFONO_ACCESS_TECHNOLOGY_NR_5GCN
#define LTE OFONO_ACCESS_TECHNOLOGY_EUTRAN
#define GSM OFONO_ACCESS_TECHNOLOGY_GSM

static
void
test_assert_list(
    const BinderOpList* list,
    const struct ofono_network_operator* expected,
    guint count)
{
    guint i;

    g_assert(list);
    g_assert_cmpuint(list->count, == ,count);
    for (i = 0; i < count; i++) {
        const struct ofono_network_operator* op = list->op + i;

        g_assert_cmpstr(op->name, == ,expected[i].name);
        g_assert_cmpstr(op->mcc, == ,expected[i].mcc);
        g_assert_cmpstr(op->mnc, == ,expected[i].mnc);
        g_assert_cmpint(op->status, == ,expected[i].status);
        g_assert_cmpint(op->tech, == ,expected[i].tech);
    }
}

/* Incremental NR, LTE, then GSM results observed on Deck in Spain. */
static
void
test_merge_technologies(
    void)
{
    struct ofono_network_operator ops[] = {
        OP("Movistar", "214", "07", NR),
        OP("Orange", "214", "03", NR),
        OP("Movistar", "214", "07", LTE),
        OP("21401", "214", "01", LTE),
        OP("Orange", "214", "03", LTE),
        OP("21401", "214", "01", GSM),
        OP("Orange", "214", "03", GSM),
        OP("Movistar", "214", "07", GSM)
    };
    BinderOpList results = { ops, 2 };
    BinderOpList* list = binder_oplist_merge(NULL, &results);

    test_assert_list(list, ops, 2);

    /* The modem repeats earlier results in subsequent notifications. */
    results.count = 5;
    list = binder_oplist_merge(list, &results);
    test_assert_list(list, ops, 5);

    results.count = G_N_ELEMENTS(ops);
    list = binder_oplist_merge(list, &results);
    test_assert_list(list, ops, G_N_ELEMENTS(ops));

    /* A repeated final notification must not add duplicates. */
    list = binder_oplist_merge(list, &results);
    test_assert_list(list, ops, G_N_ELEMENTS(ops));
    binder_oplist_free(list);
}

static
void
test_merge_update(
    void)
{
    struct ofono_network_operator ops[] = {
        OP("21401", "214", "01", LTE),
        OP("21401", "214", "01", GSM),
        OP("Other MNC", "214", "07", LTE),
        OP("Other MCC", "208", "01", LTE),
        OP("Unknown technology", "214", "01", -1)
    };
    struct ofono_network_operator update =
        OP("Vodafone", "214", "01", LTE);
    BinderOpList results = { ops, G_N_ELEMENTS(ops) };
    BinderOpList updates = { &update, 1 };
    BinderOpList* list = binder_oplist_merge(NULL, &results);

    test_assert_list(list, ops, G_N_ELEMENTS(ops));

    update.status = OFONO_OPERATOR_STATUS_AVAILABLE;
    list = binder_oplist_merge(list, &updates);
    g_assert_cmpstr(ops[0].name, == ,"21401"); /* Source was not modified. */
    ops[0] = update;
    test_assert_list(list, ops, G_N_ELEMENTS(ops));
    binder_oplist_free(list);
}

static
void
test_merge_empty(
    void)
{
    BinderOpList* empty = binder_oplist_new();
    BinderOpList* list = binder_oplist_new();

    g_assert(!binder_oplist_merge(NULL, NULL));
    g_assert(!binder_oplist_merge(NULL, empty));
    g_assert(binder_oplist_merge(list, NULL) == list);
    g_assert(binder_oplist_merge(list, empty) == list);
    g_assert_cmpuint(list->count, == ,0);
    binder_oplist_free(list);
    binder_oplist_free(empty);
}

int main(int argc, char* argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/oplist/merge/technologies", test_merge_technologies);
    g_test_add_func("/oplist/merge/update", test_merge_update);
    g_test_add_func("/oplist/merge/empty", test_merge_empty);
    return g_test_run();
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
