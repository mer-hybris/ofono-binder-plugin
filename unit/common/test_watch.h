/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2017-2021 Jolla Ltd.
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

#ifndef TEST_WATCH_H
#define TEST_WATCH_H

#include <ofono/watch.h>

typedef enum test_watch_signal {
    TEST_WATCH_SIGNAL_MODEM_CHANGED,
    TEST_WATCH_SIGNAL_ONLINE_CHANGED,
    TEST_WATCH_SIGNAL_SIM_CHANGED,
    TEST_WATCH_SIGNAL_SIM_STATE_CHANGED,
    TEST_WATCH_SIGNAL_ICCID_CHANGED,
    TEST_WATCH_SIGNAL_IMSI_CHANGED,
    TEST_WATCH_SIGNAL_SPN_CHANGED,
    TEST_WATCH_SIGNAL_NETREG_CHANGED,
    TEST_WATCH_SIGNAL_COUNT
} TEST_WATCH_SIGNAL;

typedef struct ofono_watch OfonoWatch;

void
test_watch_signal_queue(
    OfonoWatch* watch,
    TEST_WATCH_SIGNAL id);

void
test_watch_emit_queued_signals(
    OfonoWatch* watch);

void
test_watch_set_ofono_sim(
    OfonoWatch* watch,
    struct ofono_sim* sim);

void
test_watch_set_ofono_iccid(
    OfonoWatch* watch,
    const char* iccid);

void
test_watch_set_ofono_imsi(
    OfonoWatch* watch,
    const char* imsi);

void
test_watch_set_ofono_spn(
    OfonoWatch* watch,
    const char* spn);

void
test_watch_set_ofono_netreg(
    OfonoWatch* watch,
    struct ofono_netreg* netreg);

#endif /* TEST_WATCH_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
