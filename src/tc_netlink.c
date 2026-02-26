/*
 * tc_netlink.c  –  CAKE bandwidth control via raw NETLINK_ROUTE
 *
 * Why raw netlink instead of libnl-tiny?
 *   libnl-tiny on OpenWrt provides only the core netlink socket helpers;
 *   it does NOT include libnl-route (qdisc/class/filter objects).  To use
 *   the higher-level route API you would need the full libnl3, which is far
 *   too heavy for a router daemon.  Raw netlink with kernel headers is
 *   self-contained, zero extra dependencies, and produces a message that is
 *   byte-for-byte identical to what iproute2 tc sends.
 *
 * Message layout (64 bytes total for a typical rate-change):
 *
 *   [nlmsghdr 16 B]
 *   [tcmsg    20 B]
 *   [nlattr TCA_KIND    12 B]  "cake\0" + 3 pad
 *   [nlattr TCA_OPTIONS  4 B + nested...]
 *     [nlattr TCA_CAKE_BASE_RATE64  12 B]  u64 rate_bps
 *
 * Reference: iproute2 tc/q_cake.c  (cake_parse_opt / addattr_nest /
 *            addattr_l(…, TCA_CAKE_BASE_RATE64, &bandwidth, 8))
 *            iproute2 tc/tc_qdisc.c (qdisc_modify, NLM_F_* flags)
 *            Linux kernel net/sched/sch_cake.c (cake_change)
 */

#include "tc_netlink.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>

#include <net/if.h>                /* if_nametoindex           */
#include <sys/socket.h>
#include <linux/netlink.h>         /* NETLINK_ROUTE, nlmsghdr  */
#include <linux/rtnetlink.h>       /* RTM_NEWQDISC, tcmsg, TCA_* */
#include <linux/pkt_sched.h>       /* TC_H_ROOT, TCA_CAKE_*    */

/* ── TCA_CAKE_BASE_RATE64 guard ──────────────────────────────────────────
 *
 * Introduced in kernel 4.19 (upstream CAKE merge).  The Filogic 820 SDK
 * (kernel 5.15 / 6.1) defines it in linux/pkt_sched.h, but older SDK
 * snapshot header sets may not have it.  Value = 2 is stable in the UAPI.
 */
#ifndef TCA_CAKE_BASE_RATE64
#define TCA_CAKE_BASE_RATE64  2
#endif

/* ── Internal state ──────────────────────────────────────────────────── */

struct tc_nl_ctx {
    int      fd;
    uint32_t pid;    /* kernel-assigned nl_pid after bind() */
    uint32_t seq;    /* monotone sequence counter           */
};

/* ── Netlink message buffer ──────────────────────────────────────────── */

/*
 * Worst-case message size (header + tcmsg + KIND + OPTIONS + RATE64):
 * 16 + 20 + 12 + 4 + 12 = 64 bytes.  256 is very conservative.
 */
#define NL_BUF_SIZE   256
#define NL_RECV_SIZE  256

/* ── Inline nlattr helpers ───────────────────────────────────────────── */

/* Append a raw-data nlattr to buf[*pos]; advance *pos. */
static void nl_put_attr(char *buf, int *pos, uint16_t type,
                        const void *data, uint16_t dlen)
{
    struct nlattr *nla = (struct nlattr *)(buf + *pos);
    nla->nla_type = type;
    nla->nla_len  = (uint16_t)(NLA_HDRLEN + dlen);
    if (dlen && data)
        memcpy((char *)nla + NLA_HDRLEN, data, dlen);
    /* zero the alignment pad */
    int pad = (int)NLA_ALIGN(nla->nla_len) - nla->nla_len;
    if (pad > 0)
        memset((char *)nla + nla->nla_len, 0, pad);
    *pos += (int)NLA_ALIGN(nla->nla_len);
}

static void nl_put_u64(char *buf, int *pos, uint16_t type, uint64_t val)
{
    nl_put_attr(buf, pos, type, &val, sizeof(val));
}

static void nl_put_str(char *buf, int *pos, uint16_t type, const char *s)
{
    nl_put_attr(buf, pos, type, s, (uint16_t)(strlen(s) + 1));
}

/* Begin a nested attribute; returns offset of its header for nest_end(). */
static int nl_nest_start(char *buf, int *pos, uint16_t type)
{
    int off = *pos;
    struct nlattr *nla = (struct nlattr *)(buf + off);
    nla->nla_type = type;    /* kernel CAKE code uses nla_parse_nested  */
    nla->nla_len  = NLA_HDRLEN; /* will be patched by nl_nest_end()     */
    *pos += NLA_HDRLEN;
    return off;
}

/* Patch the nested attribute's length to cover everything written so far. */
static void nl_nest_end(char *buf, int *pos, int nest_off)
{
    struct nlattr *nla = (struct nlattr *)(buf + nest_off);
    nla->nla_len = (uint16_t)(*pos - nest_off);
}

/* ── Socket management ───────────────────────────────────────────────── */

tc_nl_ctx_t *tc_nl_open(void)
{
    tc_nl_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (ctx->fd < 0) {
        syslog(LOG_ERR, "tc_netlink: socket(): %m");
        free(ctx);
        return NULL;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (bind(ctx->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        syslog(LOG_ERR, "tc_netlink: bind(): %m");
        close(ctx->fd);
        free(ctx);
        return NULL;
    }

    /* Retrieve the kernel-assigned nl_pid (used to match ACKs). */
    socklen_t slen = sizeof(sa);
    if (getsockname(ctx->fd, (struct sockaddr *)&sa, &slen) == 0)
        ctx->pid = sa.nl_pid;

    ctx->seq = 1;
    return ctx;
}

void tc_nl_close(tc_nl_ctx_t *ctx)
{
    if (!ctx)
        return;
    close(ctx->fd);
    free(ctx);
}

/* ── Core: build and send one RTM_NEWQDISC message, wait for ACK ─────── */

/*
 * Returns  0    success
 *         -ENOENT   qdisc not found (caller may retry with NLM_F_CREATE)
 *         -errno    other kernel error
 *         -1        local send/recv error
 */
static int tc_send_newqdisc(tc_nl_ctx_t *ctx,
                            unsigned int ifindex,
                            uint64_t     rate_bps,
                            uint16_t     nl_extra_flags) /* 0 or NLM_F_CREATE|NLM_F_EXCL */
{
    char buf[NL_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    uint32_t seq = ctx->seq++;

    /* ── 1. nlmsghdr ──────────────────────────────────────────────── */
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_type  = RTM_NEWQDISC;
    nlh->nlmsg_flags = (uint16_t)(NLM_F_REQUEST | NLM_F_ACK | nl_extra_flags);
    nlh->nlmsg_seq   = seq;
    nlh->nlmsg_pid   = 0;   /* kernel fills this in on receipt */

    int pos = NLMSG_HDRLEN;

    /* ── 2. tcmsg ─────────────────────────────────────────────────── */
    struct tcmsg *tc = (struct tcmsg *)(buf + pos);
    tc->tcm_family  = AF_UNSPEC;
    tc->tcm_ifindex = (int)ifindex;
    tc->tcm_handle  = 0;          /* kernel matches by (ifindex, parent) */
    tc->tcm_parent  = TC_H_ROOT;
    tc->tcm_info    = 0;
    pos += (int)NLMSG_ALIGN(sizeof(struct tcmsg));

    /* ── 3. TCA_KIND = "cake" ─────────────────────────────────────── */
    nl_put_str(buf, &pos, TCA_KIND, "cake");

    /* ── 4. TCA_OPTIONS { TCA_CAKE_BASE_RATE64 = rate_bps } ──────── */
    int opts_off = nl_nest_start(buf, &pos, TCA_OPTIONS);
    nl_put_u64(buf, &pos, TCA_CAKE_BASE_RATE64, rate_bps);
    nl_nest_end(buf, &pos, opts_off);

    /* Patch total message length. */
    nlh->nlmsg_len = (uint32_t)pos;

    /* ── 5. Send ─────────────────────────────────────────────────── */
    struct sockaddr_nl dst;
    memset(&dst, 0, sizeof(dst));
    dst.nl_family = AF_NETLINK;

    if (sendto(ctx->fd, buf, (size_t)pos, 0,
               (struct sockaddr *)&dst, sizeof(dst)) < 0)
        return -1;   /* errno set */

    /* ── 6. Receive ACK ──────────────────────────────────────────── */
    char rbuf[NL_RECV_SIZE];
    ssize_t n = recv(ctx->fd, rbuf, sizeof(rbuf), 0);
    if (n < 0)
        return -1;

    struct nlmsghdr *ack = (struct nlmsghdr *)rbuf;
    if ((size_t)n < NLMSG_HDRLEN || ack->nlmsg_type != NLMSG_ERROR)
        return -1;

    struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(ack);
    /* err->error is a negative errno on failure, 0 on ACK/success */
    if (err->error != 0) {
        errno = -err->error;
        return err->error;   /* negative errno */
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int tc_cake_set_bandwidth(tc_nl_ctx_t *ctx,
                          const char  *iface,
                          uint32_t     rate_kbps)
{
    if (!ctx || !iface || !iface[0]) {
        errno = EINVAL;
        return -1;
    }

    unsigned int ifindex = if_nametoindex(iface);
    if (!ifindex) {
        syslog(LOG_WARNING, "tc_netlink: unknown interface '%s'", iface);
        errno = ENODEV;
        return -1;
    }

    /*
     * Convert kbps → bps.
     * rate_kbps == 0 → rate_bps == 0 → CAKE unlimited (no shaping).
     * This is how iproute2 encodes "bandwidth unlimited".
     */
    uint64_t rate_bps = (uint64_t)rate_kbps * 1000ULL;

    /* ── Step 1: tc qdisc change (modify existing root CAKE qdisc) ─ */
    int ret = tc_send_newqdisc(ctx, ifindex, rate_bps, 0);
    if (ret == 0)
        return 0;

    /*
     * Step 2: if the qdisc was not found (ENOENT), fall back to
     * tc qdisc add (NLM_F_CREATE | NLM_F_EXCL) to create a minimal
     * CAKE qdisc.  This matches the existing two-command fallback in
     * set_shaper_rate().
     */
    if (errno == ENOENT) {
        ret = tc_send_newqdisc(ctx, ifindex, rate_bps,
                               NLM_F_CREATE | NLM_F_EXCL);
        if (ret == 0)
            return 0;
    }

    syslog(LOG_WARNING,
           "tc_netlink: failed to set CAKE bandwidth on %s to %ukbps: %m",
           iface, rate_kbps);
    return -1;
}
