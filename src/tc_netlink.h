/*
 * tc_netlink.h  –  CAKE bandwidth control via raw NETLINK_ROUTE
 *
 * Replaces posix_spawnp("tc …") with a single persistent kernel socket.
 * One RTM_NEWQDISC message per rate change; no fork, no exec, no waitpid.
 *
 * Dependencies: none beyond the standard C library and Linux kernel headers.
 * No libnl, no libnl-tiny, no tc binary at runtime.
 */

#ifndef TC_NETLINK_H
#define TC_NETLINK_H

#include <stdint.h>

/* Opaque handle that owns the cached NETLINK_ROUTE socket. */
typedef struct tc_nl_ctx tc_nl_ctx_t;

/*
 * tc_nl_open  –  open and bind a NETLINK_ROUTE socket.
 * Returns a heap-allocated handle, or NULL on failure.
 * Call once at startup and store in autorate_t.
 */
tc_nl_ctx_t *tc_nl_open(void);

/*
 * tc_nl_close  –  close the socket and free the handle.
 * Safe to call with NULL.
 */
void tc_nl_close(tc_nl_ctx_t *ctx);

/*
 * tc_cake_set_bandwidth  –  change the CAKE bandwidth on the root qdisc
 *                            of <iface> to <rate_kbps> kbps.
 *
 *   rate_kbps == 0  → unlimited (CAKE internal: rate_bps == 0 disables shaping)
 *
 * Behaviour mirrors the two-step tc approach:
 *   1. RTM_NEWQDISC / NLM_F_REQUEST  (tc qdisc change)
 *   2. Falls back to RTM_NEWQDISC / NLM_F_CREATE | NLM_F_EXCL  (tc qdisc add)
 *      only if step 1 returns ENOENT (qdisc not yet present).
 *
 * All existing CAKE options (diffserv, nat, wash, overhead, …) are
 * preserved because we only send TCA_CAKE_BASE_RATE64; the kernel's
 * cake_change() only touches parameters that are present in the message.
 *
 * Returns 0 on success, -1 on error (errno set).
 */
int tc_cake_set_bandwidth(tc_nl_ctx_t *ctx,
                          const char  *iface,
                          uint32_t     rate_kbps);

#endif /* TC_NETLINK_H */
