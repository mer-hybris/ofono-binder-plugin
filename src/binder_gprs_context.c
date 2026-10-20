/*
 *  oFono - Open Source Telephony - binder based adaptation
 *
 *  Copyright (C) 2021-2022 Jolla Ltd.
 *  Copyright (C) 2025 Slava Monich <slava@monich.com>
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

#include "binder_data.h"
#include "binder_gprs_context.h"
#include "binder_log.h"
#include "binder_modem.h"
#include "binder_network.h"
#include "binder_netreg.h"
#include "binder_util.h"

#include "binder_ext_ims.h"
#include "binder_ext_slot.h"

#include <ofono/gprs.h>
#include <ofono/log.h>
#include <ofono/mtu-limit.h>
#include <ofono/watch.h>

#include <gutil_strv.h>
#include <gutil_misc.h>

#include <arpa/inet.h>
#include <linux/fib_rules.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define CTX_ID_NONE ((unsigned int)(-1))

#define MAX_MMS_MTU 1280

#define IMS_ROUTE_TABLE_BASE 3000
#define IMS_ROUTE_RULE_PRIORITY 990

typedef struct binder_gprs_context_call {
    BinderDataRequest* req;
    ofono_gprs_context_cb_t cb;
    gpointer data;
    unsigned int cid;
} BinderGprsContextCall;

typedef struct binder_gprs_context {
    struct ofono_gprs_context* gc;
    struct ofono_watch* watch;
    struct ofono_mtu_limit* mtu_limit;
    BinderNetwork* network;
    BinderData* data;
    char* log_prefix;
    guint active_ctx_cid;
    gulong calls_changed_id;
    BinderDataCall* active_call;
    BinderGprsContextCall activate;
    BinderGprsContextCall deactivate;
    BinderExtIms* ims;
    gulong ims_state_id;
} BinderGprsContext;

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->log_prefix, ##args)

static inline BinderGprsContext*
binder_gprs_context_get_data(struct ofono_gprs_context *gprs)
    {  return ofono_gprs_context_get_data(gprs); }

static
char*
binder_gprs_context_netmask(
    const char* bits)
{
    guint nbits;

    if (gutil_parse_uint(bits, 0, &nbits) && nbits < 33) {
        const char* str;
        struct in_addr in;

        in.s_addr = htonl((nbits == 32) ? 0xffffffff :
            ((1u << nbits)-1) << (32-nbits));
        str = inet_ntoa(in);
        if (str) {
            return g_strdup(str);
        }
    }
    return NULL;
}

static
int
binder_gprs_context_address_family(
    const char* addr)
{
    if (strchr(addr, ':')) {
        return AF_INET6;
    } else if (strchr(addr, '.')) {
        return AF_INET;
    } else {
        return AF_UNSPEC;
    }
}

static
void
binder_gprs_context_nl_attr(
    struct nlmsghdr* msg,
    int type,
    const void* data,
    int len)
{
    struct rtattr* rta = (struct rtattr*)((char*)msg +
        NLMSG_ALIGN(msg->nlmsg_len));

    rta->rta_type = type;
    rta->rta_len = RTA_LENGTH(len);
    memcpy(RTA_DATA(rta), data, len);
    msg->nlmsg_len = NLMSG_ALIGN(msg->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

static
void
binder_gprs_context_nl_request(
    int fd,
    struct nlmsghdr* msg,
    int ignored_error)
{
    struct {
        struct nlmsghdr hdr;
        struct nlmsgerr err;
    } ack;
    int error;

    if (send(fd, msg, msg->nlmsg_len, 0) < 0 ||
        recv(fd, &ack, sizeof(ack), 0) < (ssize_t)sizeof(ack)) {
        error = errno;
    } else {
        error = -ack.err.error;
    }
    if (error && error != ignored_error) {
        ofono_warn("IMS routing request %u failed: %s", msg->nlmsg_type,
            strerror(error));
    }
}

/*
 * The IMS stack binds its sockets to the IMS interface. While the internet
 * PDN is up, the main table has no default route via the IMS interface and
 * the IPv6 reverse path filter drops the replies from the P-CSCF. Lookups
 * bound to the IMS interface get a default route via it from this table.
 */
static
void
binder_gprs_context_ims_route(
    const char* ifname,
    gboolean add)
{
    const guint32 ifindex = ifname ? if_nametoindex(ifname) : 0;
    const guint32 table = IMS_ROUTE_TABLE_BASE + ifindex;
    const guint32 priority = IMS_ROUTE_RULE_PRIORITY;
    struct {
        struct nlmsghdr hdr;
        struct rtmsg rtm;
        char attrs[64];
    } route;
    struct {
        struct nlmsghdr hdr;
        struct fib_rule_hdr frh;
        char attrs[64];
    } rule;
    int fd;

    if (!ifindex) {
        ofono_warn("IMS interface %s not found", ifname);
        return;
    }

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        ofono_error("Failed to open netlink socket: %s", strerror(errno));
        return;
    }

    memset(&route, 0, sizeof(route));
    route.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(route.rtm));
    route.hdr.nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
    route.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
        (add ? (NLM_F_CREATE | NLM_F_REPLACE) : 0);
    route.rtm.rtm_family = AF_INET6;
    route.rtm.rtm_protocol = RTPROT_STATIC;
    route.rtm.rtm_type = RTN_UNICAST;
    binder_gprs_context_nl_attr(&route.hdr, RTA_TABLE, &table, sizeof(table));
    binder_gprs_context_nl_attr(&route.hdr, RTA_OIF, &ifindex,
        sizeof(ifindex));

    memset(&rule, 0, sizeof(rule));
    rule.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(rule.frh));
    rule.hdr.nlmsg_type = add ? RTM_NEWRULE : RTM_DELRULE;
    rule.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
        (add ? (NLM_F_CREATE | NLM_F_EXCL) : 0);
    rule.frh.family = AF_INET6;
    rule.frh.action = FR_ACT_TO_TBL;
    binder_gprs_context_nl_attr(&rule.hdr, FRA_TABLE, &table, sizeof(table));
    binder_gprs_context_nl_attr(&rule.hdr, FRA_PRIORITY, &priority,
        sizeof(priority));
    binder_gprs_context_nl_attr(&rule.hdr, FRA_OIFNAME, ifname,
        strlen(ifname) + 1);

    /* The kernel drops the route when the interface goes down */
    if (add) {
        binder_gprs_context_nl_request(fd, &route.hdr, 0);
        binder_gprs_context_nl_request(fd, &rule.hdr, EEXIST);
    } else {
        binder_gprs_context_nl_request(fd, &rule.hdr, ENOENT);
        binder_gprs_context_nl_request(fd, &route.hdr, ESRCH);
    }
    close(fd);
}

static
void
binder_gprs_context_free_active_call(
    BinderGprsContext* self)
{
    if (self->active_call) {
        if (ofono_gprs_context_get_type(self->gc) ==
            OFONO_GPRS_CONTEXT_TYPE_IMS) {
            binder_gprs_context_ims_route(self->active_call->ifname, FALSE);
        }
        binder_data_call_release(self->data, self->active_call->cid, self);
        binder_data_call_free(self->active_call);
        self->active_call = NULL;
    }
    if (self->calls_changed_id) {
        binder_data_remove_handler(self->data, self->calls_changed_id);
        self->calls_changed_id = 0;
    }
    if (self->mtu_limit) {
        ofono_mtu_limit_free(self->mtu_limit);
        self->mtu_limit = NULL;
    }
}

static
void
binder_gprs_context_set_active_call(
    BinderGprsContext* self,
    const BinderDataCall* call)
{
    if (call) {
        binder_data_call_free(self->active_call);
        self->active_call = binder_data_call_dup(call);
        if (ofono_gprs_context_get_type(self->gc) ==
            OFONO_GPRS_CONTEXT_TYPE_MMS) {
            /*
             * Some MMS providers have a problem with MTU greater than 1280.
             * Let's be safe.
             */
            if (!self->mtu_limit) {
                self->mtu_limit = ofono_mtu_limit_new(MAX_MMS_MTU);
            }
        }
        ofono_mtu_limit_set_ifname(self->mtu_limit, call->ifname);
        binder_data_call_grab(self->data, call->cid, self);
    } else {
        binder_gprs_context_free_active_call(self);
    }
}

static
void
binder_gprs_context_set_disconnected(
    BinderGprsContext* self)
{
    if (self->active_call) {
        binder_gprs_context_free_active_call(self);
        if (self->deactivate.req) {
            BinderGprsContextCall deact = self->deactivate;

            memset(&self->deactivate, 0, sizeof(self->deactivate));
            binder_data_request_cancel(deact.req);
            if (deact.cb) {
                struct ofono_error err;

                ofono_info("Deactivated data call");
                deact.cb(binder_error_ok(&err), deact.data);
            }
        }
    }

    if (self->active_ctx_cid != CTX_ID_NONE) {
        const guint id = self->active_ctx_cid;

        self->active_ctx_cid = CTX_ID_NONE;
        DBG_(self, "ofono context %u deactivated", id);
        ofono_gprs_context_deactivated(self->gc, id);
    }
}

static
void
binder_gprs_context_set_address(
    struct ofono_gprs_context *gc,
    const BinderDataCall* call)
{
    const char* ip_addr = NULL;
    char* ip_mask = NULL;
    const char* ipv6_addr = NULL;
    unsigned char ipv6_prefix_length = 0;
    char* tmp_ip_addr = NULL;
    char* tmp_ipv6_addr = NULL;
    char* const* list = call->addresses;
    const int n = gutil_strv_length(list);
    int i;

    for (i = 0; i < n && (!ipv6_addr || !ip_addr); i++) {
        const char* addr = list[i];

        switch (binder_gprs_context_address_family(addr)) {
        case AF_INET:
            if (!ip_addr) {
                const char* s = strchr(addr, '/');

                if (s) {
                    const gsize len = s - addr;
                    tmp_ip_addr = g_strndup(addr, len);
                    ip_addr = tmp_ip_addr;
                    ip_mask = binder_gprs_context_netmask(s+1);
                } else {
                    ip_addr = addr;
                }
                if (!ip_mask) {
                    ip_mask = g_strdup("255.255.255.0");
                }
            }
            break;
        case AF_INET6:
            if (!ipv6_addr) {
                const char* s = strchr(addr, '/');

                if (s) {
                    const gsize len = s - addr;
                    const int prefix = atoi(s + 1);

                    tmp_ipv6_addr = g_strndup(addr, len);
                    ipv6_addr = tmp_ipv6_addr;
                    if (prefix >= 0 && prefix <= 128) {
                        ipv6_prefix_length = prefix;
                    }
                } else {
                    ipv6_addr = addr;
                }
            }
        }
    }

    ofono_gprs_context_set_ipv4_address(gc, ip_addr, TRUE);
    ofono_gprs_context_set_ipv4_netmask(gc, ip_mask);
    ofono_gprs_context_set_ipv6_address(gc, ipv6_addr);
    ofono_gprs_context_set_ipv6_prefix_length(gc, ipv6_prefix_length);

    if (!ip_addr && !ipv6_addr) {
        ofono_error("GPRS context: No IP address");
    }

    /* Allocate temporary strings */
    g_free(ip_mask);
    g_free(tmp_ip_addr);
    g_free(tmp_ipv6_addr);
}

static
void
binder_gprs_context_set_gateway(
    struct ofono_gprs_context *gc,
    const BinderDataCall* call)
{
    const char* ip_gw = NULL;
    const char* ipv6_gw = NULL;
    char* const* list = call->gateways;
    const int n = gutil_strv_length(list);
    int i;

    /* Pick 1 gw for each protocol*/
    for (i = 0; i < n && (!ipv6_gw || !ip_gw); i++) {
        const char* addr = list[i];
        switch (binder_gprs_context_address_family(addr)) {
        case AF_INET:
            if (!ip_gw) ip_gw = addr;
            break;
        case AF_INET6:
            if (!ipv6_gw) ipv6_gw = addr;
            break;
        }
    }

    ofono_gprs_context_set_ipv4_gateway(gc, ip_gw);
    ofono_gprs_context_set_ipv6_gateway(gc, ipv6_gw);
}

typedef
void
(*ofono_gprs_context_list_setter_t)(
    struct ofono_gprs_context* gc,
    const char** list);

static
void
binder_gprs_context_set_servers(
    struct ofono_gprs_context* gc,
    char* const* list,
    ofono_gprs_context_list_setter_t set_ipv4,
    ofono_gprs_context_list_setter_t set_ipv6)
{
    int i;
    const char** ip_list = NULL, ** ip_ptr = NULL;
    const char** ipv6_list = NULL, ** ipv6_ptr = NULL;
    const int n = gutil_strv_length(list);

    for (i = 0; i < n; i++) {
        const char *addr = list[i];

        switch (binder_gprs_context_address_family(addr)) {
        case AF_INET:
            if (!ip_ptr) {
                ip_list = g_new0(const char *, n - i + 1);
                ip_ptr = ip_list;
            }
            *ip_ptr++ = addr;
            break;
        case AF_INET6:
            if (!ipv6_ptr) {
                ipv6_list = g_new0(const char *, n - i + 1);
                ipv6_ptr = ipv6_list;
            }
            *ipv6_ptr++ = addr;
            break;
        }
    }

    set_ipv4(gc, ip_list);
    set_ipv6(gc, ipv6_list);

    g_free(ip_list);
    g_free(ipv6_list);
}

static
void
binder_gprs_context_set_dns_servers(
    struct ofono_gprs_context* gc,
    const BinderDataCall* call)
{
    binder_gprs_context_set_servers(gc, call->dnses,
        ofono_gprs_context_set_ipv4_dns_servers,
        ofono_gprs_context_set_ipv6_dns_servers);
}

static
void
binder_gprs_context_set_proxy_cscf(
    struct ofono_gprs_context* gc,
    const BinderDataCall* call)
{
    binder_gprs_context_set_servers(gc, call->pcscf,
        ofono_gprs_context_set_ipv4_proxy_cscf,
        ofono_gprs_context_set_ipv6_proxy_cscf);
}

/* Only compares the stuff that's important to us */

#define DATA_CALL_IFNAME_CHANGED    (0x01)
#define DATA_CALL_ADDRESS_CHANGED   (0x02)
#define DATA_CALL_GATEWAY_CHANGED   (0x04)
#define DATA_CALL_DNS_CHANGED       (0x08)
#define DATA_CALL_PCSCF_CHANGED     (0x10)
#define DATA_CALL_ALL_CHANGED       (0x1f)

static
int
binder_gprs_context_data_call_change(
    const BinderDataCall* c1,
    const BinderDataCall* c2)
{
    if (c1 == c2) {
        return 0;
    } else if (c1 && c2) {
        int changes = 0;

        if (g_strcmp0(c1->ifname, c2->ifname)) {
            changes |= DATA_CALL_IFNAME_CHANGED;
        }

        if (!gutil_strv_equal(c1->addresses, c2->addresses)) {
            changes |= DATA_CALL_ADDRESS_CHANGED;
        }

        if (!gutil_strv_equal(c1->gateways, c2->gateways)) {
            changes |= DATA_CALL_GATEWAY_CHANGED;
        }

        if (!gutil_strv_equal(c1->dnses, c2->dnses)) {
            changes |= DATA_CALL_DNS_CHANGED;
        }

        if (!gutil_strv_equal(c1->pcscf, c2->pcscf)) {
            changes |= DATA_CALL_PCSCF_CHANGED;
        }

        return changes;
    } else {
        return DATA_CALL_ALL_CHANGED;
    }
}

static
void
binder_gprs_context_call_list_changed(
    BinderData* data,
    BINDER_DATA_PROPERTY property,
    void* arg)
{
    BinderGprsContext* self = arg;
    struct ofono_gprs_context* gc = self->gc;

    /*
     * self->active_call can't be NULL here because this callback
     * is only registered when we have the active call and released
     * when active call is dropped.
     */
    BinderDataCall* prev_call = self->active_call;
    const BinderDataCall* call = binder_data_call_find(data->calls,
        prev_call->cid);
    int change = 0;

    if (call && call->active != RADIO_DATA_CALL_INACTIVE) {
        /* Compare it against the last known state */
        change = binder_gprs_context_data_call_change(call, prev_call);
    } else {
        ofono_error("Clearing active context");
        binder_gprs_context_set_disconnected(self);
        call = NULL;
    }

    if (!call) {
        /* We are not interested */
        return;
    } else if (!change) {
        DBG_(self, "call %u didn't change", call->cid);
        return;
    } else {
        DBG_(self, "call %u changed", call->cid);
    }

    /*
     * prev_call points to the previous active call, and it will
     * be deallocated at the end of the this function. Clear the
     * self->active_call pointer so that we don't deallocate it twice.
     */
    self->active_call = NULL;
    binder_gprs_context_set_active_call(self, call);

    if (call->status != RADIO_DATA_CALL_FAIL_NONE) {
        ofono_info("data call status: %d", call->status);
    }

    if (change & DATA_CALL_IFNAME_CHANGED) {
        DBG_(self, "interface changed");
        ofono_gprs_context_set_interface(gc, call->ifname);
    }

    if (change & DATA_CALL_ADDRESS_CHANGED) {
        DBG_(self, "address changed");
        binder_gprs_context_set_address(gc, call);
    }

    if (change & DATA_CALL_GATEWAY_CHANGED) {
        DBG_(self, "gateway changed");
        binder_gprs_context_set_gateway(gc, call);
    }

    if (change & DATA_CALL_DNS_CHANGED) {
        DBG_(self, "name server(s) changed");
        binder_gprs_context_set_dns_servers(gc, call);
    }

    if (change & DATA_CALL_PCSCF_CHANGED) {
        DBG_(self, "P-CSCF changed");
        binder_gprs_context_set_proxy_cscf(gc, call);
    }

    ofono_gprs_context_signal_change(gc, self->active_ctx_cid);
    binder_data_call_free(prev_call);
}

static
void
binder_gprs_context_activate_primary_cb(
    BinderData* data,
    RADIO_ERROR radio_error,
    const BinderDataCall* call,
    void* user_data)
{
    BinderGprsContext* self = user_data;
    struct ofono_gprs_context* gc = self->gc;
    struct ofono_error error;
    ofono_gprs_context_cb_t cb;
    gpointer cb_data;

    binder_error_init_failure(&error);
    if (radio_error != RADIO_ERROR_NONE) {
        ofono_error("GPRS context: Reply failure: %s",
            binder_radio_error_string(radio_error));
    } else if (!call) {
        ofono_error("Unexpected data call failure");
    } else if (call->status != RADIO_DATA_CALL_FAIL_NONE) {
        ofono_error("Unexpected data call status %d", call->status);
        error.type = OFONO_ERROR_TYPE_CMS;
        error.error = call->status;
    } else if (!call->ifname) {
        /* Must have interface */
        ofono_error("GPRS context: No interface");
    } else {
        ofono_info("setting up data call");

        GASSERT(!self->calls_changed_id);
        binder_data_remove_handler(self->data, self->calls_changed_id);
        self->calls_changed_id = binder_data_add_property_handler(self->data,
            BINDER_DATA_PROPERTY_CALLS, binder_gprs_context_call_list_changed,
            self);

        self->active_ctx_cid = self->activate.cid;
        binder_gprs_context_set_active_call(self, call);
        if (ofono_gprs_context_get_type(gc) == OFONO_GPRS_CONTEXT_TYPE_IMS) {
            binder_gprs_context_ims_route(call->ifname, TRUE);
        }
        ofono_gprs_context_set_interface(gc, call->ifname);
        binder_gprs_context_set_address(gc, call);
        binder_gprs_context_set_gateway(gc, call);
        binder_gprs_context_set_dns_servers(gc, call);
        binder_gprs_context_set_proxy_cscf(gc, call);
        binder_error_init_ok(&error);
    }

    cb = self->activate.cb;
    cb_data = self->activate.data;
    GASSERT(self->activate.req);
    memset(&self->activate, 0, sizeof(self->activate));

    if (cb) {
        cb(&error, cb_data);
    }
}

static
void
binder_gprs_context_activate_primary(
    struct ofono_gprs_context* gc,
    const struct ofono_gprs_primary_context* ctx,
    ofono_gprs_context_cb_t cb,
    void* data)
{
    BinderGprsContext* self = binder_gprs_context_get_data(gc);
    struct ofono_watch* watch = self->watch;
    struct ofono_netreg* netreg = watch->netreg;
    const enum ofono_netreg_status rs = ofono_netreg_get_status(netreg);

    /* Let's make sure that we aren't connecting when roaming not allowed */
    if (rs == OFONO_NETREG_STATUS_ROAMING) {
        struct ofono_gprs* gprs = watch->gprs;

        if (!ofono_gprs_get_roaming_allowed(gprs) &&
            binder_netreg_check_if_really_roaming(netreg, rs) ==
            OFONO_NETREG_STATUS_ROAMING) {
            struct ofono_error error;

            ofono_info("Can't activate context %u (roaming)", ctx->cid);
            cb(binder_error_failure(&error), data);
            return;
        }
    }

    ofono_info("Activating context: %u", ctx->cid);
    GASSERT(!self->activate.req);
    GASSERT(ctx->cid != CTX_ID_NONE);

    self->activate.cb = cb;
    self->activate.data = data;
    self->activate.cid = ctx->cid;
    self->activate.req = binder_data_call_setup(self->data, ctx,
        (ofono_gprs_context_get_type(gc) == OFONO_GPRS_CONTEXT_TYPE_IMS) ?
        OFONO_GPRS_CONTEXT_TYPE_IMS : ofono_gprs_context_get_assigned_type(gc),
        binder_gprs_context_activate_primary_cb, self);
}

static
void
binder_gprs_context_deactivate_primary_cb(
    BinderData* data,
    RADIO_ERROR radio_error,
    void* user_data)
{
    BinderGprsContext* gcd = user_data;

    /*
     * Data call list may change before the completion of the deactivate
     * request, in that case binder_gprs_context_set_disconnected will be
     * invoked and gcd->deactivate.req will be NULL.
     */
    if (gcd->deactivate.req) {
        ofono_gprs_context_cb_t cb = gcd->deactivate.cb;
        gpointer cb_data = gcd->deactivate.data;

        if (radio_error == RADIO_ERROR_NONE) {
            GASSERT(gcd->active_call);
            ofono_info("Deactivated data call");
        } else {
            ofono_error("Deactivate failure: %s",
                binder_radio_error_string(radio_error));
        }

        memset(&gcd->deactivate, 0, sizeof(gcd->deactivate));
        if (cb) {
            struct ofono_error error;

            binder_gprs_context_free_active_call(gcd);
            cb(binder_error_ok(&error), cb_data);
            return;
        }
    }

    /* Make sure we are in the disconnected state */
    binder_gprs_context_set_disconnected(gcd);
}

static
void
binder_gprs_context_deactivate_primary(
    struct ofono_gprs_context* gc,
    unsigned int id,
    ofono_gprs_context_cb_t cb,
    void* data)
{
    BinderGprsContext* self = binder_gprs_context_get_data(gc);

    GASSERT(self->active_ctx_cid == id);
    ofono_info("Deactivating context: %u", id);

    if (self->active_call && self->active_ctx_cid == id) {
        self->deactivate.cb = cb;
        self->deactivate.data = data;
        self->deactivate.req = binder_data_call_deactivate(self->data,
            self->active_call->cid, binder_gprs_context_deactivate_primary_cb,
            self);
    } else if (cb) {
        struct ofono_error err;

        cb(binder_error_ok(&err), data);
    }
}

static
void
binder_gprs_context_detach_shutdown(
    struct ofono_gprs_context* gc,
    unsigned int id)
{
    binder_gprs_context_deactivate_primary(gc, id, NULL, NULL);
}

static
void
binder_gprs_context_ims_finished(
    const struct ofono_error* error,
    void* data)
{
    if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
        ofono_error("IMS data call request failed");
    }
}

static
void
binder_gprs_context_ims_state_changed(
    BinderExtIms* ims,
    void* user_data)
{
    BinderGprsContext* self = user_data;
    struct ofono_gprs_context* gc = self->gc;

    if (ofono_gprs_context_get_type(gc) != OFONO_GPRS_CONTEXT_TYPE_IMS) {
        return;
    }

    /* The modem requests the IMS PDN, the core never activates it */
    if (binder_ext_ims_get_bearer_active(ims)) {
        const struct ofono_gprs_primary_context* ctx =
            ofono_gprs_context_settings_by_type(self->watch->gprs,
                OFONO_GPRS_CONTEXT_TYPE_IMS);

        if (ctx && ctx->apn[0] && !self->active_call && !self->activate.req) {
            binder_gprs_context_activate_primary(gc, ctx,
                binder_gprs_context_ims_finished, self);
        }
    } else if (self->active_call && !self->deactivate.req) {
        binder_gprs_context_deactivate_primary(gc, self->active_ctx_cid,
            binder_gprs_context_ims_finished, self);
    }
}

static
int
binder_gprs_context_probe(
    struct ofono_gprs_context* gc,
    unsigned int vendor,
    void* data)
{
    BinderModem* modem = binder_modem_get_data(data);
    BinderGprsContext* self = g_new0(BinderGprsContext, 1);

    self->log_prefix = binder_dup_prefix(modem->log_prefix);
    DBG_(self, "");

    self->gc = gc;
    self->watch = ofono_watch_new(binder_modem_get_path(modem));
    self->network = binder_network_ref(modem->network);
    self->data = binder_data_ref(modem->data);
    self->active_ctx_cid = CTX_ID_NONE;
    self->ims = binder_ext_ims_ref(binder_ext_slot_get_interface(modem->ext,
        BINDER_EXT_TYPE_IMS));
    self->ims_state_id = binder_ext_ims_add_state_handler(self->ims,
        binder_gprs_context_ims_state_changed, self);

    ofono_gprs_context_set_data(gc, self);
    return 0;
}

static
void
binder_gprs_context_remove(
    struct ofono_gprs_context* gc)
{
    BinderGprsContext* self = binder_gprs_context_get_data(gc);

    DBG_(self, "");
    if (self->activate.req) {
        /*
         * The core has already completed its pending D-Bus
         * request, invoking the completion callback will
         * cause libdbus to panic.
         */
        binder_data_request_detach(self->activate.req);
        binder_data_request_cancel(self->activate.req);
    }

    if (self->deactivate.req) {
        /* Let it complete but we won't be around to be notified. */
        binder_data_request_detach(self->deactivate.req);
    } else if (self->active_call) {
        binder_data_call_deactivate(self->data, self->active_call->cid,
            NULL, NULL);
    }

    binder_ext_ims_remove_handler(self->ims, self->ims_state_id);
    binder_ext_ims_unref(self->ims);
    binder_data_remove_handler(self->data, self->calls_changed_id);
    binder_data_unref(self->data);
    binder_network_unref(self->network);
    binder_data_call_free(self->active_call);
    ofono_mtu_limit_free(self->mtu_limit);
    ofono_watch_unref(self->watch);

    g_free(self->log_prefix);
    g_free(self);

    ofono_gprs_context_set_data(gc, NULL);
}

/*==========================================================================*
 * API
 *==========================================================================*/

static const struct ofono_gprs_context_driver binder_gprs_context_driver = {
    .name                   = BINDER_DRIVER,
    .probe                  = binder_gprs_context_probe,
    .remove                 = binder_gprs_context_remove,
    .activate_primary       = binder_gprs_context_activate_primary,
    .deactivate_primary     = binder_gprs_context_deactivate_primary,
    .detach_shutdown        = binder_gprs_context_detach_shutdown
};

void
binder_gprs_context_init()
{
    ofono_gprs_context_driver_register(&binder_gprs_context_driver);
}

void
binder_gprs_context_cleanup()
{
    ofono_gprs_context_driver_unregister(&binder_gprs_context_driver);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
