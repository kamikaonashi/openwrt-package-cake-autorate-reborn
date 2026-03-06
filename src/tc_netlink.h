/*
 * tc_netlink.h  –  Standalone CAKE qdisc management via raw NETLINK_ROUTE
 *
 * Provides two layers of functionality:
 *
 *  1. RUNTIME rate control  – tc_cake_set_bandwidth()
 *     Fast in-place bandwidth adjustment; called thousands of times per day.
 *     Uses a single persistent socket, no fork/exec.
 *
 *  2. LIFECYCLE setup/teardown  – tc_dl_setup() / tc_ul_setup() / …
 *     Creates and destroys the full CAKE + IFB plumbing at daemon
 *     start/stop, replacing what SQM scripts previously did:
 *
 *       DL path:
 *         ip link add ifb-wan type ifb
 *         ip link set ifb-wan up
 *         tc qdisc add dev ifb-wan  root cake bandwidth <X>kbit [opts]
 *         tc qdisc add dev wan      ingress
 *         tc filter add dev wan     parent ffff: matchall \
 *                                     action mirred egress redirect dev ifb-wan
 *
 *       UL path:
 *         tc qdisc add dev wan root cake bandwidth <X>kbit [opts]
 *
 * Dependencies: standard C library + Linux kernel headers only.
 * No libnl, no libnl-tiny, no tc binary at runtime, no sqm-scripts.
 */

#ifndef TC_NETLINK_H
#define TC_NETLINK_H

#include <stdint.h>

/* ── CAKE mode constants ────────────────────────────────────────
 *
 * These are defined as enum values in linux/pkt_sched.h (kernel 4.19+).
 * Do NOT redefine them here with #define – the compiler rejects a macro
 * redefinition of an enum member name ("expected identifier before
 * numeric constant").
 *
 * Values for reference (pass as plain integers to cake_qdisc_opts_t):
 *
 *   Diffserv / tin structure (cake_diffserv):
 *     CAKE_DIFFSERV_DIFFSERV3  = 0  (deprecated 3-tin)
 *     CAKE_DIFFSERV_DIFFSERV4  = 1  (bulk/streaming/video/voice) ← default
 *     CAKE_DIFFSERV_DIFFSERV8  = 2  (8-tin CS0–CS7)
 *     CAKE_DIFFSERV_BESTEFFORT = 3  (single tin)
 *     CAKE_DIFFSERV_PRECEDENCE = 4  (legacy IP precedence)
 *
 *   Flow isolation (cake_flow_mode):
 *     CAKE_FLOW_NONE     = 0
 *     CAKE_FLOW_SRC_IP   = 1
 *     CAKE_FLOW_DST_IP   = 2
 *     CAKE_FLOW_HOSTS    = 3
 *     CAKE_FLOW_FLOWS    = 4
 *     CAKE_FLOW_DUAL_SRC = 5
 *     CAKE_FLOW_DUAL_DST = 6
 *     CAKE_FLOW_TRIPLE   = 7  ← default (src+dst host + 5-tuple)
 *
 *   ATM/PTM compensation (cake_atm):
 *     CAKE_ATM_NONE = 0  ← default
 *     CAKE_ATM_ATM  = 1  (48-byte cell rounding, ADSL)
 *     CAKE_ATM_PTM  = 2  (64-byte expansion, VDSL2)
 *
 *   ACK filter (cake_ack_filter):
 *     CAKE_ACK_NONE       = 0  ← default
 *     CAKE_ACK_FILTER     = 1
 *     CAKE_ACK_AGGRESSIVE = 2
 */

/* ── CAKE qdisc creation options ────────────────────────────── */

/*
 * cake_qdisc_opts_t – options for tc_dl_setup / tc_ul_setup.
 *
 * These mirror the flags accepted by `tc qdisc add … cake …`.
 * All fields use CAKE's own defaults when left at 0 / INT32_MIN.
 */
typedef struct {
    /*
     * overhead – per-packet byte overhead added before rate calculation.
     *   Positive: ADSL/VDSL framing overhead (e.g. 8 for PPPoE over PTM,
     *             18 for ATM LLC).
     *   Negative: unusual (strip headers).
     *   INT32_MIN: omit attribute entirely → CAKE default (0).
     */
    int32_t  overhead;

    /*
     * mpu – minimum packet unit in bytes; packets shorter than this are
     * padded to mpu before rate accounting.  0 = CAKE default.
     */
    uint32_t mpu;

    /* nat – enable NAT-aware flow hashing (peek inside masquerade).
     * Strongly recommended for home routers doing SNAT. */
    uint32_t nat;

    /* wash – strip DSCP markings on egress so ISPs see unmarked traffic.
     * Set on the UL (WAN-facing) qdisc; typically off on the DL (IFB). */
    uint32_t wash;

    /* ingress – tell CAKE this qdisc is on an IFB / ingress path.
     * Must be set on the IFB CAKE qdisc for correct RTT estimation. */
    uint32_t ingress;

    /* ack_filter – suppress pure TCP ACKs to reclaim capacity.
     * CAKE_ACK_NONE / CAKE_ACK_FILTER / CAKE_ACK_AGGRESSIVE */
    uint32_t ack_filter;

    /* diffserv – tin structure: CAKE_DIFFSERV_* */
    uint32_t diffserv;

    /* flow_mode – isolation: CAKE_FLOW_* */
    uint32_t flow_mode;

    /* atm – cell-size compensation: CAKE_ATM_* */
    uint32_t atm;

    /* rtt_us – target RTT for AQM in microseconds.
     * 0 = CAKE default (100 ms = 100000 µs). */
    uint32_t rtt_us;

    /* split_gso – split GSO super-packets before scheduling (recommended). */
    uint32_t split_gso;

    /*
     * use_cake_mq – use the "cake-mq" multi-queue qdisc instead of "cake".
     *
     * cake-mq is a multi-queue variant available in OpenWrt 25.12+.  It
     * uses per-CPU TX queues and improves throughput on multi-core routers
     * (e.g. Mediatek Filogic) while preserving all CAKE AQM behaviour.
     *
     * Requires kernel module:  kmod-sched-cake-mq  (OpenWrt 25.12+).
     * Falls back silently to regular CAKE if the module is absent at
     * setup time (ENOENT from the kernel).
     *
     *   0 = use "cake"    (default; works on all kernels)
     *   1 = use "cake-mq" (OpenWrt 25.12+, multi-core benefit)
     */
    uint32_t use_cake_mq;
} cake_qdisc_opts_t;

/* ── Opaque handle ──────────────────────────────────────────── */

/* Owns a cached NETLINK_ROUTE socket. */
typedef struct tc_nl_ctx tc_nl_ctx_t;

/* ── Socket lifecycle ───────────────────────────────────────── */

/*
 * tc_nl_open  –  open and bind a NETLINK_ROUTE socket.
 * Returns a heap-allocated handle, or NULL on failure.
 * Call once at startup.
 */
tc_nl_ctx_t *tc_nl_open(void);

/*
 * tc_nl_close  –  close the socket and free the handle.
 * Safe to call with NULL.
 */
void tc_nl_close(tc_nl_ctx_t *ctx);

/* ── Runtime rate control ───────────────────────────────────── */

/*
 * tc_cake_set_bandwidth  –  update the CAKE root qdisc bandwidth on <iface>.
 *
 *   rate_kbps == 0  →  unlimited (CAKE rate_bps = 0 disables shaping)
 *
 * Strategy:
 *   1. RTM_NEWQDISC (change)  →  modifies existing root CAKE qdisc in-place.
 *   2. Falls back to RTM_NEWQDISC (add/create) only if step 1 returns ENOENT.
 *
 * All existing CAKE options are preserved; only TCA_CAKE_BASE_RATE64 changes.
 *
 * Returns 0 on success, -1 on error (errno set).
 */
int tc_cake_set_bandwidth(tc_nl_ctx_t *ctx,
                          const char  *iface,
                          uint32_t     rate_kbps);

/* ── Lifecycle: download path setup/teardown ────────────────── */

/*
 * tc_dl_setup  –  build the full download shaping path from scratch.
 *
 * Equivalent to:
 *   ip  link add  <ifb_if> type ifb
 *   ip  link set  <ifb_if> up
 *   tc  qdisc add dev <ifb_if>  root     cake bandwidth <rate>kbit [opts_dl]
 *   tc  qdisc add dev <wan_if>  ingress
 *   tc  filter add dev <wan_if> parent ffff: protocol all u32 match u32 0 0 \
 *                               action mirred egress redirect dev <ifb_if>
 *
 * opts_dl should have .ingress = 1 set (IFB carries ingress traffic).
 *
 * Returns 0 on success, -1 on error.
 * Idempotent: silently ignores EEXIST for the IFB interface and qdiscs.
 */
int tc_dl_setup(tc_nl_ctx_t            *ctx,
                const char             *wan_if,
                const char             *ifb_if,
                uint32_t                rate_kbps,
                const cake_qdisc_opts_t *opts_dl);

/*
 * tc_dl_teardown  –  remove the download shaping path.
 *
 * Equivalent to:
 *   tc   qdisc del dev <wan_if>  ingress
 *   tc   qdisc del dev <ifb_if>  root
 *   ip   link   del <ifb_if>
 *
 * Safe to call even if only some of the pieces exist.
 */
void tc_dl_teardown(tc_nl_ctx_t *ctx,
                    const char  *wan_if,
                    const char  *ifb_if);

/* ── Lifecycle: upload path setup/teardown ──────────────────── */

/*
 * tc_ul_setup  –  attach a CAKE root qdisc to <wan_if>.
 *
 * Equivalent to:
 *   tc qdisc add dev <wan_if> root cake bandwidth <rate>kbit [opts_ul]
 *
 * Returns 0 on success, -1 on error.
 * Idempotent: replaces an existing root qdisc (EEXIST → delete + re-add).
 */
int tc_ul_setup(tc_nl_ctx_t            *ctx,
                const char             *wan_if,
                uint32_t                rate_kbps,
                const cake_qdisc_opts_t *opts_ul);

/*
 * tc_ul_teardown  –  remove the CAKE root qdisc from <wan_if>.
 *
 * Equivalent to:
 *   tc qdisc del dev <wan_if> root
 */
void tc_ul_teardown(tc_nl_ctx_t *ctx, const char *wan_if);

#endif /* TC_NETLINK_H */
