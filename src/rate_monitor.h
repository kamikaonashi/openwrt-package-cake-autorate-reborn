#ifndef RATE_MONITOR_H
#define RATE_MONITOR_H

#include <stdint.h>

typedef struct {
    char     rx_path[128];   /* DL byte-counter sysfs path */
    char     tx_path[128];   /* UL byte-counter sysfs path */

    int      rx_fd;          /* Cached FD for DL */
    int      tx_fd;          /* Cached FD for UL */

    uint64_t prev_rx_bytes;
    uint64_t prev_tx_bytes;
    int64_t  t_prev_us;
} rate_monitor_t;

void rate_monitor_init(rate_monitor_t *rm,
                          const char *dl_if, const char *ul_if);

void rate_monitor_cleanup(rate_monitor_t *rm);

/*
 * Call periodically.  Writes current dl/ul rates into *out_dl_kbps /
 * *out_ul_kbps.  Returns the microsecond interval actually elapsed
 * since the last call (useful for drift compensation).
 */
int64_t rate_monitor_update(rate_monitor_t *rm,
                             uint32_t *out_dl_kbps,
                             uint32_t *out_ul_kbps);

#endif /* RATE_MONITOR_H */
