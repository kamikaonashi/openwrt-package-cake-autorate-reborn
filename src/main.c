/*
 * cake-autorate – C rewrite for OpenWrt
 *
 * Core algorithm mirrors cake-autorate.sh:
 *   • ICMP ping via custom pinger
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
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

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
    uint32_t addr_be;
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

    /* Integrated ICMP pinger (IPv4 raw socket) */
    int               icmp_sock;
    struct uloop_fd   icmp_ufd;
    struct uloop_timeout ping_timer;
    uint16_t          ping_id;
    uint16_t          ping_seq;
    int               ping_rr_idx;  /* round-robin reflector index */

    /* Timers */
    struct uloop_timeout rate_timer;
    struct uloop_timeout health_timer;

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

    /*
     * Asymmetric EWMA baseline update.
     * alpha fields are stored *1,000,000 (_fp), so no float conversion needed.
     */
    int64_t dl_alpha = (dl_owd_us > r->dl_owd_baseline_us)
        ? c->alpha_baseline_increase
        : c->alpha_baseline_decrease;
    int64_t ul_alpha = (ul_owd_us > r->ul_owd_baseline_us)
        ? c->alpha_baseline_increase
        : c->alpha_baseline_decrease;

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
        /* alpha_delta_ewma is stored _fp (*1,000,000) */
        int64_t ae = c->alpha_delta_ewma;
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

    /* high_load_thr is _fp (*1,000,000): use integer multiply then divide */
    uint32_t high_thr_dl = (uint32_t)((uint64_t)ar->shaper_rate_kbps[DIR_DL]
                                      * (uint64_t)c->high_load_thr / 1000000ULL);
    uint32_t high_thr_ul = (uint32_t)((uint64_t)ar->shaper_rate_kbps[DIR_UL]
                                      * (uint64_t)c->high_load_thr / 1000000ULL);

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
/*  Integrated ICMPv4 pinger (raw socket)                          */
/* ────────────────────────────────────────────────────────────── */

#define PING_PAYLOAD_MAGIC 0xCACEB00Bu

static uint16_t csum16(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint16_t)(p[0] << 8);

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static void write_be64(uint8_t out[8], uint64_t v)
{
    out[0] = (uint8_t)(v >> 56);
    out[1] = (uint8_t)(v >> 48);
    out[2] = (uint8_t)(v >> 40);
    out[3] = (uint8_t)(v >> 32);
    out[4] = (uint8_t)(v >> 24);
    out[5] = (uint8_t)(v >> 16);
    out[6] = (uint8_t)(v >> 8);
    out[7] = (uint8_t)(v);
}

static uint64_t read_be64(const uint8_t in[8])
{
    return ((uint64_t)in[0] << 56) |
           ((uint64_t)in[1] << 48) |
           ((uint64_t)in[2] << 40) |
           ((uint64_t)in[3] << 32) |
           ((uint64_t)in[4] << 24) |
           ((uint64_t)in[5] << 16) |
           ((uint64_t)in[6] << 8)  |
           ((uint64_t)in[7]);
}

struct ping_payload {
    uint32_t magic_be;
    uint16_t ridx_be;     /* reflector index (debug/sanity), optional */
    uint16_t reserved_be;
    uint8_t  t_sent_be64[8]; /* int64_t timestamp in us, big-endian */
} __attribute__((packed));

static void icmp_reply_cb(struct uloop_fd *ufd, unsigned int events)
{
    (void)events;
    autorate_t *ar = container_of(ufd, autorate_t, icmp_ufd);

    for (;;) {
        uint8_t buf[512];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);

        ssize_t n = recvfrom(ufd->fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            return;
        }
        if ((size_t)n < sizeof(struct iphdr))
            continue;

        struct iphdr *iph = (struct iphdr *)buf;
        int ip_hlen = iph->ihl * 4;
        if (ip_hlen < 20 || (size_t)n < (size_t)ip_hlen + sizeof(struct icmphdr))
            continue;

        struct icmphdr *icmph = (struct icmphdr *)(buf + ip_hlen);

        if (icmph->type != ICMP_ECHOREPLY)
            continue;

        if (ntohs(icmph->un.echo.id) != ar->ping_id)
            continue;

        const uint8_t *payload = (const uint8_t *)(icmph + 1);
        size_t payload_len = (size_t)n - (size_t)ip_hlen - sizeof(struct icmphdr);
        if (payload_len < sizeof(struct ping_payload))
            continue;

        const struct ping_payload *pl = (const struct ping_payload *)payload;
        if (pl->magic_be != htonl(PING_PAYLOAD_MAGIC))
            continue;

        uint32_t src_be = src.sin_addr.s_addr;

        int ridx = -1;
        for (int i = 0; i < ar->no_active_reflectors; i++) {
            if (ar->reflectors[i].addr_be == src_be) {
                ridx = i;
                break;
            }
        }
        if (ridx < 0)
            continue;

        int64_t t_now_us = now_us();
        int64_t t_sent_us = (int64_t)read_be64(pl->t_sent_be64);
        int64_t rtt_us = t_now_us - t_sent_us;
        if (rtt_us <= 0)
            continue;

        int64_t owd_us = rtt_us / 2;
        process_owd(ar, ridx, owd_us, owd_us, t_now_us);
    }
}

static void ping_timer_cb(struct uloop_timeout *t)
{
    autorate_t *ar = container_of(t, autorate_t, ping_timer);
    cake_config_t *c = &ar->cfg;

    if (ar->no_active_reflectors <= 0 || ar->icmp_sock < 0) {
        uloop_timeout_set(&ar->ping_timer, 1000);
        return;
    }

    if (ar->ping_rr_idx >= ar->no_active_reflectors)
        ar->ping_rr_idx = 0;

    int ridx = ar->ping_rr_idx++;
    reflector_t *r = &ar->reflectors[ridx];

    if (r->addr_be == 0) {
        /* skip invalid address */
        goto out;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = r->addr_be;

    uint8_t pkt[sizeof(struct icmphdr) + sizeof(struct ping_payload)];
    memset(pkt, 0, sizeof(pkt));

    struct icmphdr *icmph = (struct icmphdr *)pkt;
    struct ping_payload *pl = (struct ping_payload *)(icmph + 1);

    icmph->type = ICMP_ECHO;
    icmph->code = 0;
    icmph->un.echo.id = htons(ar->ping_id);
    icmph->un.echo.sequence = htons(++ar->ping_seq);

    pl->magic_be = htonl(PING_PAYLOAD_MAGIC);
    pl->ridx_be  = htons((uint16_t)ridx);
    pl->reserved_be = 0;

    int64_t t_sent_us = now_us();
    write_be64(pl->t_sent_be64, (uint64_t)t_sent_us);

    icmph->checksum = 0;
    icmph->checksum = csum16(pkt, sizeof(pkt));

    (void)sendto(ar->icmp_sock, pkt, sizeof(pkt), 0,
                 (struct sockaddr *)&dst, sizeof(dst));

out:
    /* reflector_ping_interval_us is already in µs; integer division only */
    int interval_ms = (int)((c->reflector_ping_interval_us / 1000)
                            / ar->no_active_reflectors);
    if (interval_ms < 10) interval_ms = 10; /* minimum 10 ms */
    uloop_timeout_set(&ar->ping_timer, interval_ms);
}

static void refresh_reflector_addrs(autorate_t *ar)
{
    for (int i = 0; i < ar->no_active_reflectors; i++) {
        struct in_addr a;
        if (inet_pton(AF_INET, ar->reflectors[i].addr, &a) == 1)
            ar->reflectors[i].addr_be = a.s_addr;
        else
            ar->reflectors[i].addr_be = 0;
    }
}

static int start_pinger(autorate_t *ar)
{
    ar->icmp_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (ar->icmp_sock < 0)
        return -1;

    set_nonblocking(ar->icmp_sock);

    ar->icmp_ufd.fd = ar->icmp_sock;
    ar->icmp_ufd.cb = icmp_reply_cb;
    uloop_fd_add(&ar->icmp_ufd, ULOOP_READ | ULOOP_EDGE_TRIGGER);

    ar->ping_id = (uint16_t)(getpid() ^ ((uint16_t)time(NULL)));
    ar->ping_seq = 0;
    ar->ping_rr_idx = 0;

    refresh_reflector_addrs(ar);

    ar->ping_timer.cb = ping_timer_cb;
    uloop_timeout_set(&ar->ping_timer, 1);
    return 0;
}

static void stop_pinger(autorate_t *ar)
{
    uloop_timeout_cancel(&ar->ping_timer);

    if (ar->icmp_ufd.registered)
        uloop_fd_delete(&ar->icmp_ufd);

    if (ar->icmp_sock >= 0) {
        close(ar->icmp_sock);
        ar->icmp_sock = -1;
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
        refresh_reflector_addrs(ar);
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

    /* reflector_ping_interval_us is already in µs */
    ar.ping_response_interval_us =
        ar.cfg.reflector_ping_interval_us
        / ar.no_active_reflectors;

    /* Open persistent NETLINK_ROUTE socket for CAKE bandwidth control */
    ar.tc_nl = tc_nl_open();
    if (!ar.tc_nl) {
        syslog(LOG_ERR, "tc_netlink: failed to open netlink socket");
        rate_monitor_cleanup(&ar.rm);
        return 1;
    }

    /* Apply initial CAKE rates */
    set_shaper_rate(&ar, DIR_DL);
    set_shaper_rate(&ar, DIR_UL);

    /* Initialise rate monitor */
    rate_monitor_init(&ar.rm, ar.cfg.dl_if, ar.cfg.ul_if);

    /* Startup wait */
    if (ar.cfg.startup_wait_us > 0)
        usleep((unsigned int)ar.cfg.startup_wait_us);

    /* uloop */
    uloop_init();
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Start integrated pinger */
    if (start_pinger(&ar) < 0) {
        syslog(LOG_ERR, "failed to start integrated pinger (raw ICMP)");
        rate_monitor_cleanup(&ar.rm);
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

    syslog(LOG_INFO, "started instance '%s' dl=%s ul=%s",
           section, ar.cfg.dl_if, ar.cfg.ul_if);

    uloop_run();
    uloop_done();

    /* ── Graceful shutdown ─────────────────────────────────── */
    syslog(LOG_INFO, "shutting down instance '%s'", section);

    uloop_timeout_cancel(&ar.rate_timer);
    uloop_timeout_cancel(&ar.health_timer);

    stop_pinger(&ar);

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
