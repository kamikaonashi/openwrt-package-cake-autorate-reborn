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
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

/* ── internal helpers ─────────────────────────────────────────── */

static uint64_t read_bytes_fd(int *fd_ptr, const char *path)
{
    char buf[64];

    /* Reopen if interface was recreated */
    if (*fd_ptr < 0) {
        *fd_ptr = open(path, O_RDONLY);
        if (*fd_ptr < 0)
            return 0;
    }

    if (lseek(*fd_ptr, 0, SEEK_SET) == (off_t)-1) {
        close(*fd_ptr);
        *fd_ptr = -1;
        return 0;
    }

    ssize_t n = read(*fd_ptr, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(*fd_ptr);
        *fd_ptr = -1;
        return 0;
    }

    buf[n] = '\0';
    return (uint64_t)strtoull(buf, NULL, 10);
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

    rm->rx_fd = -1;
    rm->tx_fd = -1;

    /*
     * DL interface: IFB/veth use tx_bytes, others use rx_bytes
     */
    if (strncmp(dl_if, "ifb",  3) == 0 ||
        strncmp(dl_if, "veth", 4) == 0)
        snprintf(rm->rx_path, sizeof(rm->rx_path),
                 "/sys/class/net/%s/statistics/tx_bytes", dl_if);
    else
        snprintf(rm->rx_path, sizeof(rm->rx_path),
                 "/sys/class/net/%s/statistics/rx_bytes", dl_if);

    /* UL interface: always tx_bytes */
    snprintf(rm->tx_path, sizeof(rm->tx_path),
             "/sys/class/net/%s/statistics/tx_bytes", ul_if);

    /* Try opening immediately */
    rm->rx_fd = open(rm->rx_path, O_RDONLY);
    rm->tx_fd = open(rm->tx_path, O_RDONLY);

    rm->prev_rx_bytes = read_bytes_fd(&rm->rx_fd, rm->rx_path);
    rm->prev_tx_bytes = read_bytes_fd(&rm->tx_fd, rm->tx_path);
    rm->t_prev_us     = now_us();
}

void rate_monitor_cleanup(rate_monitor_t *rm)
{
    if (rm->rx_fd >= 0)
        close(rm->rx_fd);

    if (rm->tx_fd >= 0)
        close(rm->tx_fd);

    rm->rx_fd = -1;
    rm->tx_fd = -1;
}

int64_t rate_monitor_update(rate_monitor_t *rm,
                             uint32_t *out_dl_kbps,
                             uint32_t *out_ul_kbps)
{
    int64_t t_now   = now_us();
    int64_t elapsed = t_now - rm->t_prev_us;
    if (elapsed <= 0)
        elapsed = 1;

    uint64_t rx = read_bytes_fd(&rm->rx_fd, rm->rx_path);
    uint64_t tx = read_bytes_fd(&rm->tx_fd, rm->tx_path);

    uint64_t drx = (rx >= rm->prev_rx_bytes)
                   ? (rx - rm->prev_rx_bytes)
                   : 0;

    uint64_t dtx = (tx >= rm->prev_tx_bytes)
                   ? (tx - rm->prev_tx_bytes)
                   : 0;

    *out_dl_kbps = (uint32_t)((drx * 8000000ULL) / (uint64_t)elapsed);
    *out_ul_kbps = (uint32_t)((dtx * 8000000ULL) / (uint64_t)elapsed);

    rm->prev_rx_bytes = rx;
    rm->prev_tx_bytes = tx;
    rm->t_prev_us     = t_now;
    return elapsed;
}
