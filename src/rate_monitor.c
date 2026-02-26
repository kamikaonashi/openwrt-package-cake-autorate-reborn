/*
 * rate_monitor.c
 *
 * Reads TX/RX byte counters from /sys/class/net/<if>/statistics/
 * and computes achieved rate in kbps via a non-blocking, poll-based approach.
 *
 * Interface conventions:
 *   IFB / veth  – ingress is redirected to the TX queue, so we read tx_bytes
 *                 for the DL (download) rate.
 *   Normal WAN  – upload is genuine egress, so we read tx_bytes for UL.
 *                 Download on a plain WAN port uses rx_bytes, but that case
 *                 is uncommon; the typical OpenWrt setup uses an IFB for DL.
 */

#include "rate_monitor.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── internal helpers ─────────────────────────────────────────── */

static uint64_t read_bytes(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    fscanf(f, "%llu", &v);
    fclose(f);
    return (uint64_t)v;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

/* ── public API ───────────────────────────────────────────────── */

void rate_monitor_init(rate_monitor_t *rm,
                       const char *dl_if, const char *ul_if)
{
    memset(rm, 0, sizeof(*rm));

    /*
     * DL interface: IFB and veth redirect ingress traffic through their
     * TX queue, so tx_bytes is the correct counter.  All other interfaces
     * (rare for DL) use rx_bytes.
     */
    if (strncmp(dl_if, "ifb",  3) == 0 ||
        strncmp(dl_if, "veth", 4) == 0)
        snprintf(rm->rx_path, sizeof(rm->rx_path),
                 "/sys/class/net/%s/statistics/tx_bytes", dl_if);
    else
        snprintf(rm->rx_path, sizeof(rm->rx_path),
                 "/sys/class/net/%s/statistics/rx_bytes", dl_if);

    /* UL interface: upload is always genuine egress → tx_bytes. */
    snprintf(rm->tx_path, sizeof(rm->tx_path),
             "/sys/class/net/%s/statistics/tx_bytes", ul_if);

    rm->prev_rx_bytes = read_bytes(rm->rx_path);
    rm->prev_tx_bytes = read_bytes(rm->tx_path);
    rm->t_prev_us     = now_us();
}

int64_t rate_monitor_update(rate_monitor_t *rm,
                             uint32_t *out_dl_kbps,
                             uint32_t *out_ul_kbps)
{
    int64_t t_now   = now_us();
    int64_t elapsed = t_now - rm->t_prev_us;
    if (elapsed <= 0) elapsed = 1; /* guard against divide-by-zero */

    uint64_t rx = read_bytes(rm->rx_path);
    uint64_t tx = read_bytes(rm->tx_path);

    /* Guard against 32-bit counter wraparound */
    uint64_t drx = (rx >= rm->prev_rx_bytes) ? (rx - rm->prev_rx_bytes) : 0;
    uint64_t dtx = (tx >= rm->prev_tx_bytes) ? (tx - rm->prev_tx_bytes) : 0;

    /* kbps = bytes * 8 / elapsed_s = bytes * 8 000 000 / elapsed_us */
    *out_dl_kbps = (uint32_t)((drx * 8000000ULL) / (uint64_t)elapsed);
    *out_ul_kbps = (uint32_t)((dtx * 8000000ULL) / (uint64_t)elapsed);

    rm->prev_rx_bytes = rx;
    rm->prev_tx_bytes = tx;
    rm->t_prev_us     = t_now;
    return elapsed;
}
