/*
 * cake-autorate – C rewrite for OpenWrt
 *
 * Core algorithm mirrors cake-autorate.sh:
 *   • ICMP ping via fping child process (non-blocking read on pipe)
 *   • Rate monitor via /sys/class/net polling
 *   • tc qdisc change for CAKE bandwidth adjustment (fork+exec, no shell)
 *   • uloop for the main event loop, timers, and fd watching
 *   • Reflector health monitoring with automatic replacement
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <math.h>
#include <sys/wait.h>

#include <libubox/uloop.h>

#include "config.h"
#include "rate_monitor.h"
#include "tc_netlink.h"

/* ────────────────────────────────────────────────────────────── */
/*  Constants                                                     */
/* ────────────────────────────────────────────────────────────── */
#define DIR_DL 0
#define DIR_UL 1

#define LOAD_IDLE 0
#define LOAD_LOW  1
#define LOAD_HIGH 2
#define LOAD_BB   3  /* bufferbloat */

#define STATE_RUNNING 0
#define STATE_IDLE    1
#define STATE_STALL   2

/* Hard cap on the misbehaving-detection window (offences[] array size). */
#define MAX_OFFENCE_WINDOW 64

/* ────────────────────────────────────────────────────────────── */
/*  Per-reflector runtime state                                   */
/* ────────────────────────────────────────────────────────────── */
typedef struct {
    char    addr[64];
    int64_t dl_owd_baseline_us;
    int64_t ul_owd_baseline_us;
    int64_t dl_owd_delta_ewma_us;
    int64_t ul_owd_delta_ewma_us;
    int64_t last_response_us;        /* 0 = no response yet */
    int     offences[MAX_OFFENCE_WINDOW];
    int     offences_idx;
    int     sum_offences;
} reflector_t;

/* ────────────────────────────────────────────────────────────── */
/*  Global application state                                      */
/* ────────────────────────────────────────────────────────────── */
typedef struct {
    cake_config_t   cfg;
    rate_monitor_t  rm;
    tc_nl_ctx_t    *tc_nl;

    /* Shaper rates (kbps) */
    uint32_t shaper_rate_kbps[2];
    uint32_t last_shaper_rate_kbps[2];

    /* Achieved rates */
    uint32_t achieved_rate_kbps[2];
    int      achieved_rate_updated[2];

    /* Load classification */
    int      load_condition[2];
    int      bufferbloat_detected[2];
    int64_t  t_last_bufferbloat_us[2];
    int64_t  t_last_decay_us[2];

    /* OWD sliding window (allocated to cfg.bufferbloat_detection_window) */
    int64_t *dl_delays;
    int64_t *ul_delays;
    int64_t *dl_owd_deltas_us;
    int64_t *ul_owd_deltas_us;
    int      delays_idx;
    int64_t  sum_dl_delays;
    int64_t  sum_ul_delays;
    int64_t  sum_dl_owd_deltas_us;
    int64_t  sum_ul_owd_deltas_us;
    int64_t  avg_owd_delta_us[2];

    /* Reflectors */
    reflector_t reflectors[MAX_REFLECTORS];
    int         no_active_reflectors;  /* = min(cfg.no_pingers, cfg.no_reflectors) */
    int         spare_idx;             /* next unused spare in cfg.reflectors[] */
    int64_t     t_last_reflector_health_us;
    int64_t     global_last_response_us;

    /* Pingers (fping child) */
    pid_t             pinger_pid;
    int               pinger_fd;
    struct uloop_fd   pinger_ufd;

    /* Line reassembly buffer (was global static – now per-instance) */
    char line_buf[512];
    int  line_len;

    /* Timers */
    struct uloop_timeout rate_timer;
    struct uloop_timeout health_timer;
    struct uloop_timeout restart_timer;  /* delayed fping restart */

    /* State machine */
    int main_state;

    /* Ping response interval (µs): ping_interval / no_pingers */
    int64_t ping_response_interval_us;
} autorate_t;

/* ────────────────────────────────────────────────────────────── */
/*  Helpers                                                       */
/* ────────────────────────────────────────────────────────────── */
static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ────────────────────────────────────────────────────────────── */
/*  CAKE rate control                                             */
/* ────────────────────────────────────────────────────────────── */
static void clamp_shaper_rate(autorate_t *ar, int dir)
{
    uint32_t mn = (dir == DIR_DL) ? ar->cfg.min_dl_shaper_rate_kbps
                                  : ar->cfg.min_ul_shaper_rate_kbps;
    uint32_t mx = (dir == DIR_DL) ? ar->cfg.max_dl_shaper_rate_kbps
                                  : ar->cfg.max_ul_shaper_rate_kbps;
    if (ar->shaper_rate_kbps[dir] < mn) ar->shaper_rate_kbps[dir] = mn;
    if (ar->shaper_rate_kbps[dir] > mx) ar->shaper_rate_kbps[dir] = mx;
}

/*
 * set_shaper_rate – apply the current shaper rate to the kernel.
 * Includes optimization to prevent thrashing on micro-adjustments.
 */
static void set_shaper_rate(autorate_t *ar, int dir)
{
    uint32_t rate     = ar->shaper_rate_kbps[dir];
    uint32_t old_rate = ar->last_shaper_rate_kbps[dir];

    if (rate == old_rate)
        return;

    /*
     * HYSTERESIS OPTIMIZATION:
     * Don't fork 'tc' for insignificant changes (< 0.5%) unless we are
     * hitting the exact min/max limits. This prevents CPU churn.
     */
    uint32_t min_limit = (dir == DIR_DL) ? ar->cfg.min_dl_shaper_rate_kbps 
                                         : ar->cfg.min_ul_shaper_rate_kbps;
    uint32_t max_limit = (dir == DIR_DL) ? ar->cfg.max_dl_shaper_rate_kbps 
                                         : ar->cfg.max_ul_shaper_rate_kbps;

    // Calculate absolute difference
    uint32_t diff = (rate > old_rate) ? (rate - old_rate) : (old_rate - rate);
    
    // Threshold is 0.5% of current rate (e.g., 100kbps at 20Mbps, 1.5Mbps at 300Mbps)
    uint32_t base = old_rate ? old_rate : rate;

    uint32_t threshold = base / 200;            /* 0.5% */
    uint32_t floor_val = (base < 5000) ? (base / 100) : 50; /* 1% below 5 Mbps */
    if (threshold < floor_val) threshold = floor_val;

    // If change is small AND we are not snapping to a rail (min/max), skip update
    if (diff < threshold && rate != min_limit && rate != max_limit) {
        return;
    }

    const char *iface  = (dir == DIR_DL) ? ar->cfg.dl_if : ar->cfg.ul_if;
    int         adjust = (dir == DIR_DL) ? ar->cfg.adjust_dl_shaper_rate
                                         : ar->cfg.adjust_ul_shaper_rate;

    if (adjust && iface[0] != '\0') {
        /* tc_cake_set_bandwidth() tries RTM_NEWQDISC/change first,
         * then falls back to RTM_NEWQDISC/add on ENOENT – identical
         * semantics to the old two-command approach, but in-process. */
        tc_cake_set_bandwidth(ar->tc_nl, iface, rate);
    }

    ar->last_shaper_rate_kbps[dir] = rate;
}

/*
 * reset_shaper_rate – restore unlimited bandwidth on shutdown so the
 * interface is not left permanently throttled.
 */
static void reset_shaper_rate(autorate_t *ar, const char *iface)
{
    /* rate_kbps == 0  →  CAKE rate_bps == 0  →  unlimited */
    tc_cake_set_bandwidth(ar->tc_nl, iface, 0);
}

/* ────────────────────────────────────────────────────────────── */
/*  Rate adjustment                                               */
/* ────────────────────────────────────────────────────────────── */
static void adjust_shaper_rate(autorate_t *ar, int dir, int64_t t_now_us)
{
    cake_config_t *c    = &ar->cfg;
    uint32_t base       = (dir == DIR_DL) ? c->base_dl_shaper_rate_kbps
                                           : c->base_ul_shaper_rate_kbps;
    int64_t delay_thr   = (dir == DIR_DL) ? c->dl_owd_delta_delay_thr_us
                                           : c->ul_owd_delta_delay_thr_us;
    int64_t max_up_thr  = (dir == DIR_DL) ? c->dl_avg_owd_delta_max_adjust_up_thr_us
                                           : c->ul_avg_owd_delta_max_adjust_up_thr_us;
    int64_t max_down_thr = (dir == DIR_DL) ? c->dl_avg_owd_delta_max_adjust_down_thr_us
                                            : c->ul_avg_owd_delta_max_adjust_down_thr_us;

    switch (ar->load_condition[dir]) {

    case LOAD_BB: {
        if (t_now_us - ar->t_last_bufferbloat_us[dir] <
                c->bufferbloat_refractory_period_us)
            break;

        int64_t avg = ar->avg_owd_delta_us[dir];
        int64_t factor;
        if (max_down_thr <= delay_thr) {
            factor = c->shaper_rate_max_adjust_down_bufferbloat;
        } else if (avg > delay_thr) {
            factor = c->shaper_rate_min_adjust_down_bufferbloat
                + (c->shaper_rate_max_adjust_down_bufferbloat
                   - c->shaper_rate_min_adjust_down_bufferbloat)
                * (avg - delay_thr)
                / (max_down_thr - delay_thr);
        } else {
            factor = c->shaper_rate_min_adjust_down_bufferbloat;
        }

        ar->shaper_rate_kbps[dir] =
            (uint32_t)((uint64_t)ar->shaper_rate_kbps[dir]
                       * (uint64_t)factor / 1000000ULL);
        ar->t_last_bufferbloat_us[dir] = t_now_us;
        ar->t_last_decay_us[dir]       = t_now_us;
        break;
    }

    case LOAD_HIGH: {
        if (!ar->achieved_rate_updated[dir])
            break;
        if (t_now_us - ar->t_last_bufferbloat_us[dir] <
                c->bufferbloat_refractory_period_us)
            break;

        int64_t avg = ar->avg_owd_delta_us[dir];
        int64_t factor;
        if (avg <= delay_thr) {
            factor = c->shaper_rate_max_adjust_up_load_high;
        } else if (avg < max_up_thr) {
            factor = c->shaper_rate_max_adjust_up_load_high
                - (c->shaper_rate_max_adjust_up_load_high
                   - c->shaper_rate_min_adjust_up_load_high)
                * (avg - delay_thr)
                / (max_up_thr - delay_thr);
        } else {
            factor = c->shaper_rate_min_adjust_up_load_high;
        }

        ar->shaper_rate_kbps[dir] =
            (uint32_t)((uint64_t)ar->shaper_rate_kbps[dir]
                       * (uint64_t)factor / 1000000ULL);
        ar->achieved_rate_updated[dir] = 0;
        ar->t_last_decay_us[dir]       = t_now_us;
        break;
    }

    case LOAD_LOW:
    case LOAD_IDLE: {
        if (t_now_us - ar->t_last_decay_us[dir] < c->decay_refractory_period_us)
            break;

        uint32_t rate = ar->shaper_rate_kbps[dir];
        if (rate > base) {
            int64_t f = c->shaper_rate_adjust_down_load_low;
            rate = (uint32_t)((uint64_t)rate * (uint64_t)f / 1000000ULL);
            ar->shaper_rate_kbps[dir] = (rate < base) ? base : rate;
        } else if (rate < base) {
            int64_t f = c->shaper_rate_adjust_up_load_low;
            rate = (uint32_t)((uint64_t)rate * (uint64_t)f / 1000000ULL);
            ar->shaper_rate_kbps[dir] = (rate > base) ? base : rate;
        }
        ar->t_last_decay_us[dir] = t_now_us;
        break;
    }
    }

    clamp_shaper_rate(ar, dir);
    set_shaper_rate(ar, dir);
}

/* ────────────────────────────────────────────────────────────── */
/*  OWD processing (called for each received ping response)       */
/* ────────────────────────────────────────────────────────────── */
static void process_owd(autorate_t *ar,
                        int reflector_idx,
                        int64_t dl_owd_us,
                        int64_t ul_owd_us,
                        int64_t t_now_us)
{
    cake_config_t *c = &ar->cfg;
    reflector_t   *r = &ar->reflectors[reflector_idx];

    /*
     * Bootstrap: first sample for this reflector (or after replacement).
     * Set baseline to the observed OWD so the first delta is zero rather
     * than triggering a spurious bufferbloat event.
     */
    if (r->dl_owd_baseline_us == 0) {
        r->dl_owd_baseline_us = dl_owd_us;
        r->ul_owd_baseline_us = ul_owd_us;
        r->last_response_us   = t_now_us;
        return;
    }

    /*
     * Sanity check: a very large negative delta means the path changed
     * drastically (e.g. route flip).  Reset baselines.
     */
    if (dl_owd_us - r->dl_owd_baseline_us < -3000000LL ||
        ul_owd_us - r->ul_owd_baseline_us < -3000000LL) {
        r->dl_owd_baseline_us = dl_owd_us;
        r->ul_owd_baseline_us = ul_owd_us;
        r->last_response_us   = t_now_us;
        return;
    }

    /* Asymmetric EWMA baseline update */
    int64_t dl_alpha = (dl_owd_us > r->dl_owd_baseline_us)
        ? (int64_t)(c->alpha_baseline_increase * 1e6)
        : (int64_t)(c->alpha_baseline_decrease * 1e6);
    int64_t ul_alpha = (ul_owd_us > r->ul_owd_baseline_us)
        ? (int64_t)(c->alpha_baseline_increase * 1e6)
        : (int64_t)(c->alpha_baseline_decrease * 1e6);

    r->dl_owd_baseline_us =
          dl_alpha * dl_owd_us           / 1000000LL
        + (1000000LL - dl_alpha) * r->dl_owd_baseline_us / 1000000LL;
    r->ul_owd_baseline_us =
          ul_alpha * ul_owd_us           / 1000000LL
        + (1000000LL - ul_alpha) * r->ul_owd_baseline_us / 1000000LL;

    int64_t dl_delta = dl_owd_us - r->dl_owd_baseline_us;
    int64_t ul_delta = ul_owd_us - r->ul_owd_baseline_us;

    /* EWMA of delta – only while under high load */
    if (ar->load_condition[DIR_DL] == LOAD_HIGH ||
        ar->load_condition[DIR_UL] == LOAD_HIGH) {
        int64_t ae = (int64_t)(c->alpha_delta_ewma * 1e6);
        r->dl_owd_delta_ewma_us =
              ae * dl_delta             / 1000000LL
            + (1000000LL - ae) * r->dl_owd_delta_ewma_us / 1000000LL;
        r->ul_owd_delta_ewma_us =
              ae * ul_delta             / 1000000LL
            + (1000000LL - ae) * r->ul_owd_delta_ewma_us / 1000000LL;
    }

    /* Sliding window for bufferbloat detection */
    int bdw = c->bufferbloat_detection_window;
    int idx = ar->delays_idx;

    ar->sum_dl_delays -= ar->dl_delays[idx];
    ar->dl_delays[idx] = (dl_delta > c->dl_owd_delta_delay_thr_us) ? 1 : 0;
    ar->sum_dl_delays += ar->dl_delays[idx];

    ar->sum_ul_delays -= ar->ul_delays[idx];
    ar->ul_delays[idx] = (ul_delta > c->ul_owd_delta_delay_thr_us) ? 1 : 0;
    ar->sum_ul_delays += ar->ul_delays[idx];

    ar->sum_dl_owd_deltas_us -= ar->dl_owd_deltas_us[idx];
    ar->dl_owd_deltas_us[idx] = dl_delta;
    ar->sum_dl_owd_deltas_us += dl_delta;

    ar->sum_ul_owd_deltas_us -= ar->ul_owd_deltas_us[idx];
    ar->ul_owd_deltas_us[idx] = ul_delta;
    ar->sum_ul_owd_deltas_us += ul_delta;

    ar->delays_idx = (idx + 1) % bdw;

    ar->avg_owd_delta_us[DIR_DL] = ar->sum_dl_owd_deltas_us / bdw;
    ar->avg_owd_delta_us[DIR_UL] = ar->sum_ul_owd_deltas_us / bdw;

    ar->bufferbloat_detected[DIR_DL] =
        (ar->sum_dl_delays >= c->bufferbloat_detection_thr);
    ar->bufferbloat_detected[DIR_UL] =
        (ar->sum_ul_delays >= c->bufferbloat_detection_thr);

    /* Load classification */
    uint32_t high_thr_dl = (uint32_t)(c->high_load_thr * ar->shaper_rate_kbps[DIR_DL]);
    uint32_t high_thr_ul = (uint32_t)(c->high_load_thr * ar->shaper_rate_kbps[DIR_UL]);

    for (int d = 0; d < 2; d++) {
        uint32_t ach = ar->achieved_rate_kbps[d];
        uint32_t thr = (d == DIR_DL) ? high_thr_dl : high_thr_ul;
        if (ar->bufferbloat_detected[d])
            ar->load_condition[d] = LOAD_BB;
        else if (ach >= thr)
            ar->load_condition[d] = LOAD_HIGH;
        else if (ach >= c->connection_active_thr_kbps)
            ar->load_condition[d] = LOAD_LOW;
        else
            ar->load_condition[d] = LOAD_IDLE;
    }

    r->last_response_us            = t_now_us;
    ar->global_last_response_us    = t_now_us;

    adjust_shaper_rate(ar, DIR_DL, t_now_us);
    adjust_shaper_rate(ar, DIR_UL, t_now_us);
}

/* ────────────────────────────────────────────────────────────── */
/*  fping output parser                                           */
/*                                                                */
/*  fping --timestamp --loop output (modern, default):            */
/*    [epoch.frac] addr : [seq], N bytes, rtt ms (avg ms, loss%)  */
/*                                                                */
/*  Older / minimal builds omit the "N bytes" field:              */
/*    [epoch.frac] addr : [seq] rtt ms                            */
/*                                                                */
/*  Timeout lines (skipped):                                      */
/*    [epoch.frac] addr : [seq], timed out                        */
/*                                                                */
/*  We use RTT/2 as a proxy for both DL and UL OWD since fping    */
/*  only gives round-trip time.                                   */
/* ────────────────────────────────────────────────────────────── */
static void parse_fping_line(autorate_t *ar, const char *line)
{
    double ts, rtt_ms;
    char   addr[64];
    int    seq;

    /*
     * Try modern format first: "[ts] addr : [seq], N bytes, rtt ms ..."
     * The %*d skips byte count, %*s skips the word "bytes,".
     */
    if (sscanf(line, "[%lf] %63s : [%d], %*d %*s %lf ms",
               &ts, addr, &seq, &rtt_ms) != 4) {
        /* Fallback: "[ts] addr : [seq] rtt ms" (no bytes field) */
        if (sscanf(line, "[%lf] %63s : [%d] %lf ms",
                   &ts, addr, &seq, &rtt_ms) != 4)
            return; /* timeout or unrecognised line – skip silently */
    }

    /* Locate the reflector in our active set */
    int ridx = -1;
    for (int i = 0; i < ar->no_active_reflectors; i++) {
        if (strcmp(ar->reflectors[i].addr, addr) == 0) {
            ridx = i;
            break;
        }
    }
    if (ridx < 0) return;

    int64_t t_now  = now_us();
    int64_t owd_us = (int64_t)(rtt_ms * 500.0); /* RTT/2 → µs */
    process_owd(ar, ridx, owd_us, owd_us, t_now);
}

/* ────────────────────────────────────────────────────────────── */
/*  fping lifecycle                                               */
/* ────────────────────────────────────────────────────────────── */
static void pinger_cb(struct uloop_fd *ufd, unsigned int events);
static void restart_fping_cb(struct uloop_timeout *t);

static int start_fping(autorate_t *ar)
{
    cake_config_t *c = &ar->cfg;
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* child: wire pipe write-end to both stdout and stderr */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /*
         * fping -D --loop --period <ms_total> --interval <ms_each>
         *       --timeout 10000 <reflectors…>
         *
         * period = interval * no_pingers  → each host gets pinged
         *   every reflector_ping_interval_s seconds.
         */
        int interval_ms = (int)(c->reflector_ping_interval_s * 1000.0);
        int period_ms   = interval_ms * c->no_pingers;
        if (period_ms < interval_ms) period_ms = interval_ms;

        char period_s[16], interval_s[16];
        snprintf(period_s,   sizeof(period_s),   "%d", period_ms);
        snprintf(interval_s, sizeof(interval_s), "%d", interval_ms);

        /* argv: fixed args + active reflectors + NULL */
        int n_fixed = 9;
        const char **argv = calloc((size_t)(n_fixed + ar->no_active_reflectors + 1),
                                   sizeof(char *));
        if (!argv) _exit(1);

        int a = 0;
        argv[a++] = "/usr/bin/fping";
        argv[a++] = "--timestamp";
        argv[a++] = "--loop";
        argv[a++] = "--period";    argv[a++] = period_s;
        argv[a++] = "--interval";  argv[a++] = interval_s;
        argv[a++] = "--timeout";   argv[a++] = "10000";
        for (int i = 0; i < ar->no_active_reflectors; i++)
            argv[a++] = ar->reflectors[i].addr;
        argv[a] = NULL;

        execv(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);
    set_nonblocking(pipefd[0]);

    ar->pinger_pid        = pid;
    ar->pinger_fd         = pipefd[0];
    ar->line_len          = 0;   /* reset line reassembly state */
    ar->pinger_ufd.fd     = pipefd[0];
    ar->pinger_ufd.cb     = pinger_cb;
    uloop_fd_add(&ar->pinger_ufd, ULOOP_READ | ULOOP_EDGE_TRIGGER);
    return 0;
}

static void stop_fping(autorate_t *ar)
{
    if (ar->pinger_ufd.registered)
        uloop_fd_delete(&ar->pinger_ufd);
    if (ar->pinger_fd >= 0) {
        close(ar->pinger_fd);
        ar->pinger_fd = -1;
    }
    if (ar->pinger_pid > 0) {
        kill(ar->pinger_pid, SIGTERM);
        /* Reap with a brief spin so we don't block the event loop. */
        for (int i = 0; i < 50; i++) {
            if (waitpid(ar->pinger_pid, NULL, WNOHANG) > 0) break;
            usleep(10000); /* 10 ms */
        }
        /* If still alive after 500 ms, SIGKILL and blocking wait. */
        if (waitpid(ar->pinger_pid, NULL, WNOHANG) == 0) {
            kill(ar->pinger_pid, SIGKILL);
            waitpid(ar->pinger_pid, NULL, 0);
        }
        ar->pinger_pid = 0;
    }
}

/* Delayed restart callback – fires 1 s after an unexpected fping exit. */
static void restart_fping_cb(struct uloop_timeout *t)
{
    autorate_t *ar = container_of(t, autorate_t, restart_timer);
    syslog(LOG_INFO, "restarting fping");
    if (start_fping(ar) < 0) {
        syslog(LOG_ERR, "failed to restart fping, retrying in 5 s");
        uloop_timeout_set(t, 5000);
    }
}

/* uloop fd callback: data available from fping pipe */
static void pinger_cb(struct uloop_fd *ufd, unsigned int events)
{
    autorate_t *ar = container_of(ufd, autorate_t, pinger_ufd);
    char buf[512];
    ssize_t n;

    while ((n = read(ufd->fd, buf, sizeof(buf) - 1)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                ar->line_buf[ar->line_len] = '\0';
                parse_fping_line(ar, ar->line_buf);
                ar->line_len = 0;
            } else if (ar->line_len < (int)sizeof(ar->line_buf) - 1) {
                ar->line_buf[ar->line_len++] = buf[i];
            }
        }
    }

    if (ufd->eof) {
        /* fping exited unexpectedly – clean up and schedule a restart. */
        uloop_fd_delete(ufd);
        close(ufd->fd);
        ar->pinger_fd = -1;

        /* Reap zombie without blocking. */
        if (ar->pinger_pid > 0) {
            waitpid(ar->pinger_pid, NULL, WNOHANG);
            ar->pinger_pid = 0;
        }

        syslog(LOG_WARNING, "fping exited unexpectedly, restarting in 1 s");
        uloop_timeout_set(&ar->restart_timer, 1000);
    }
}

/* ────────────────────────────────────────────────────────────── */
/*  Reflector health check timer                                  */
/* ────────────────────────────────────────────────────────────── */
static void health_timer_cb(struct uloop_timeout *t)
{
    autorate_t    *ar  = container_of(t, autorate_t, health_timer);
    cake_config_t *c   = &ar->cfg;
    int64_t        now = now_us();
    int            win = c->reflector_misbehaving_detection_window;
    int            replaced = 0;

    /* Clamp window to the allocated offences[] array. */
    if (win <= 0 || win > MAX_OFFENCE_WINDOW)
        win = MAX_OFFENCE_WINDOW;

    for (int i = 0; i < ar->no_active_reflectors; i++) {
        reflector_t *r = &ar->reflectors[i];

        /* Skip reflectors that haven't received even one response yet. */
        if (r->last_response_us == 0)
            continue;

        int offence = (now - r->last_response_us >
                       c->reflector_response_deadline_us) ? 1 : 0;

        int widx = r->offences_idx;
        r->sum_offences      -= r->offences[widx];
        r->offences[widx]     = offence;
        r->sum_offences      += offence;
        r->offences_idx       = (widx + 1) % win;

        if (r->sum_offences >= c->reflector_misbehaving_detection_thr &&
            ar->spare_idx   <  c->no_reflectors) {

            syslog(LOG_WARNING,
                   "replacing misbehaving reflector %s with %s "
                   "(%d/%d misses in window)",
                   r->addr,
                   c->reflectors[ar->spare_idx],
                   r->sum_offences, win);

            snprintf(r->addr, sizeof(r->addr),
                     "%s", c->reflectors[ar->spare_idx++]);

            /* Reset all state for the new reflector. */
            r->dl_owd_baseline_us   = 0;
            r->ul_owd_baseline_us   = 0;
            r->dl_owd_delta_ewma_us = 0;
            r->ul_owd_delta_ewma_us = 0;
            r->last_response_us     = 0;
            memset(r->offences, 0, sizeof(r->offences));
            r->sum_offences = 0;
            r->offences_idx = 0;
            replaced = 1;
        }
    }

    if (replaced) {
        /* Restart fping so it pings the updated reflector set. */
        stop_fping(ar);
        if (start_fping(ar) < 0) {
            syslog(LOG_ERR, "failed to restart fping after reflector replacement");
            uloop_timeout_set(&ar->restart_timer, 1000);
        }
    }

    uloop_timeout_set(t, (int)(c->reflector_health_check_interval_us / 1000));
}

/* ────────────────────────────────────────────────────────────── */
/*  Rate-monitor timer callback (~200 ms)                         */
/* ────────────────────────────────────────────────────────────── */
static void rate_timer_cb(struct uloop_timeout *t)
{
    autorate_t    *ar  = container_of(t, autorate_t, rate_timer);
    cake_config_t *c   = &ar->cfg;

    int64_t elapsed = rate_monitor_update(&ar->rm,
                                          &ar->achieved_rate_kbps[DIR_DL],
                                          &ar->achieved_rate_kbps[DIR_UL]);

    ar->achieved_rate_updated[DIR_DL] = 1;
    ar->achieved_rate_updated[DIR_UL] = 1;

    /*
     * Drift compensation: if the timer fired late (elapsed > target),
     * subtract the excess from the next interval so we stay on schedule
     * on average.  Clamp to the nominal interval so we never schedule
     * an instant (0 ms) or negative timer.
     */
    int64_t target = c->monitor_achieved_rates_interval_us;
    int64_t next   = target - (elapsed - target);
    if (next < target) next = target;

    /* Stall detection */
    int64_t now = now_us();
    if (ar->global_last_response_us > 0) {
        int64_t stall_thr_us =
            (int64_t)c->stall_detection_thr * ar->ping_response_interval_us;
        if (now - ar->global_last_response_us > stall_thr_us &&
            ar->achieved_rate_kbps[DIR_DL] < c->connection_stall_thr_kbps &&
            ar->achieved_rate_kbps[DIR_UL] < c->connection_stall_thr_kbps) {
            if (ar->main_state != STATE_STALL) {
                ar->main_state = STATE_STALL;
                syslog(LOG_WARNING, "connection stall detected");
            }
        } else if (ar->main_state == STATE_STALL) {
            ar->main_state = STATE_RUNNING;
            syslog(LOG_INFO, "connection recovered from stall");
        }
    }

    int ms = (int)((next / 1000) & 0x7FFFFFFF);
    uloop_timeout_set(t, ms);
}

/* ────────────────────────────────────────────────────────────── */
/*  Sliding-window allocation                                     */
/* ────────────────────────────────────────────────────────────── */
static int init_windows(autorate_t *ar)
{
    int w = ar->cfg.bufferbloat_detection_window;
    ar->dl_delays        = calloc((size_t)w, sizeof(int64_t));
    ar->ul_delays        = calloc((size_t)w, sizeof(int64_t));
    ar->dl_owd_deltas_us = calloc((size_t)w, sizeof(int64_t));
    ar->ul_owd_deltas_us = calloc((size_t)w, sizeof(int64_t));
    return (ar->dl_delays && ar->ul_delays &&
            ar->dl_owd_deltas_us && ar->ul_owd_deltas_us) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────── */
/*  Signal handler                                                */
/* ────────────────────────────────────────────────────────────── */
static void handle_signal(int sig)
{
    (void)sig;
    uloop_end();
}

/* ────────────────────────────────────────────────────────────── */
/*  main                                                          */
/* ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    const char *section = (argc > 1) ? argv[1] : "primary";

    openlog("cake-autorate", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    autorate_t ar;
    memset(&ar, 0, sizeof(ar));
    ar.pinger_fd = -1;

    if (config_load(section, &ar.cfg) < 0) {
        fprintf(stderr, "cake-autorate: failed to load UCI config '%s'\n",
                section);
        syslog(LOG_ERR, "failed to load UCI config section '%s'", section);
        return 1;
    }

    if (!ar.cfg.enabled) {
        syslog(LOG_INFO, "instance '%s' disabled, exiting", section);
        return 0;
    }

    if (ar.cfg.no_pingers < 1) {
        syslog(LOG_ERR, "no_pingers must be >= 1");
        return 1;
    }

    if (init_windows(&ar) < 0) {
        syslog(LOG_ERR, "out of memory allocating OWD windows");
        return 1;
    }

    /*
     * Active reflectors = first min(no_pingers, no_reflectors) entries.
     * Spare reflectors start at index spare_idx.
     */
    ar.no_active_reflectors =
        (ar.cfg.no_pingers < ar.cfg.no_reflectors)
        ? ar.cfg.no_pingers
        : ar.cfg.no_reflectors;
    ar.spare_idx = ar.no_active_reflectors;

    for (int i = 0; i < ar.no_active_reflectors; i++)
        snprintf(ar.reflectors[i].addr, 64, "%s", ar.cfg.reflectors[i]);

    /* Initial shaper rates */
    ar.shaper_rate_kbps[DIR_DL] = ar.cfg.base_dl_shaper_rate_kbps;
    ar.shaper_rate_kbps[DIR_UL] = ar.cfg.base_ul_shaper_rate_kbps;

    /* Force the initial tc call by making last != current */
    ar.last_shaper_rate_kbps[DIR_DL] = 0;
    ar.last_shaper_rate_kbps[DIR_UL] = 0;

    ar.ping_response_interval_us =
        (int64_t)(ar.cfg.reflector_ping_interval_s * 1e6)
        / ar.no_active_reflectors;

    /* Apply initial CAKE rates */
    set_shaper_rate(&ar, DIR_DL);
    set_shaper_rate(&ar, DIR_UL);

    /* Initialise rate monitor */
    rate_monitor_init(&ar.rm, ar.cfg.dl_if, ar.cfg.ul_if);

    /* Open persistent NETLINK_ROUTE socket for CAKE bandwidth control */
    ar.tc_nl = tc_nl_open();
    if (!ar.tc_nl) {
        syslog(LOG_ERR, "tc_netlink: failed to open netlink socket");
        rate_monitor_cleanup(&ar.rm); 
        return 1;
    }

    /* Startup wait (before entering the event loop) */
    if (ar.cfg.startup_wait_s > 0.0)
        usleep((unsigned int)(ar.cfg.startup_wait_s * 1e6));

    /* uloop */
    uloop_init();
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGCHLD, SIG_DFL); /* ensure waitpid works normally */

    /* Start fping */
    if (start_fping(&ar) < 0) {
        syslog(LOG_ERR, "failed to start fping");
        rate_monitor_cleanup(&ar.rm);
        tc_nl_close(ar.tc_nl); 
        return 1;
    }

    /* Rate-monitor timer */
    ar.rate_timer.cb = rate_timer_cb;
    uloop_timeout_set(&ar.rate_timer,
        (int)(ar.cfg.monitor_achieved_rates_interval_us / 1000));

    /* Reflector health timer */
    ar.health_timer.cb = health_timer_cb;
    uloop_timeout_set(&ar.health_timer,
        (int)(ar.cfg.reflector_health_check_interval_us / 1000));

    /* Restart timer (armed on demand, not here) */
    ar.restart_timer.cb = restart_fping_cb;

    syslog(LOG_INFO, "started instance '%s' dl=%s ul=%s",
           section, ar.cfg.dl_if, ar.cfg.ul_if);

    uloop_run();
    uloop_done();

    /* ── Graceful shutdown ─────────────────────────────────── */
    syslog(LOG_INFO, "shutting down instance '%s'", section);

    uloop_timeout_cancel(&ar.rate_timer);
    uloop_timeout_cancel(&ar.health_timer);
    uloop_timeout_cancel(&ar.restart_timer);

    stop_fping(&ar);

    /* Restore unlimited bandwidth so the interface is not throttled
     * after we exit. */
    if (ar.cfg.adjust_dl_shaper_rate && ar.cfg.dl_if[0])
        reset_shaper_rate(&ar, ar.cfg.dl_if);
    if (ar.cfg.adjust_ul_shaper_rate && ar.cfg.ul_if[0])
        reset_shaper_rate(&ar, ar.cfg.ul_if);

    tc_nl_close(ar.tc_nl);
    ar.tc_nl = NULL;

    rate_monitor_cleanup(&ar.rm);

    free(ar.dl_delays);
    free(ar.ul_delays);
    free(ar.dl_owd_deltas_us);
    free(ar.ul_owd_deltas_us);

    closelog();
    return 0;
}
