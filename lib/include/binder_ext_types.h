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

#ifndef BINDER_EXT_TYPES_H
#define BINDER_EXT_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct binder_ext_call BinderExtCall;
typedef struct binder_ext_plugin BinderExtPlugin;
typedef struct binder_ext_slot BinderExtSlot;
typedef struct binder_ext_sms BinderExtSms;

/* Type of address, 3GPP TS 24.008 subclause 10.5.4.7 */
typedef enum binder_ext_toa {
    BINDER_EXT_TOA_LOCAL = 129,
    BINDER_EXT_TOA_INTERNATIONAL = 145,
    BINDER_EXT_TOA_UNKNOWN = 0x7fffffff
} BINDER_EXT_TOA;

G_END_DECLS

#endif /* BINDER_EXT_TYPES_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
