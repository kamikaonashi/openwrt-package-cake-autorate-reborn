#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <uci.h>

/* ── helpers ─────────────────────────────────────────────────── */

static const char *uci_get(struct uci_context *ctx,
                            struct uci_section *s, const char *opt)
{
    struct uci_option *o = uci_lookup_option(ctx, s, opt);
    if (o && o->type == UCI_TYPE_STRING)
        return o->v.string;
    return NULL;
}

/*
 * parse_fixed – convert a decimal string to integer with a given scale.
 *
 *   parse_fixed("0.3",  1000000) == 300000   (0.3 s > µs)
 *   parse_fixed("0.001",1000000) == 1000     (α = 0.001 in _fp)
 *   parse_fixed("10",   1000000) == 10000000 (10 s > µs)
 *   parse_fixed("1.04", 1000000) == 1040000  (rate multiplier)
 *
 * No floating-point instructions are generated.
 */
static int64_t parse_fixed(const char *s, int64_t scale)
{
    if (!s || !*s) return 0;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }

    int64_t intpart  = 0;
    int64_t fracpart = 0;
    int64_t fracdiv  = 1;

    while (*s >= '0' && *s <= '9')
        intpart = intpart * 10 + (*s++ - '0');

    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            fracpart = fracpart * 10 + (*s++ - '0');
            fracdiv *= 10;
        }
    }

    int64_t result = intpart * scale + fracpart * scale / fracdiv;
    return neg ? -result : result;
}

/*
 * Conversion macros
 *   UCI_STR   – string field
 *   UCI_INT   – int field, UCI value is a plain integer
 *   UCI_U32   – uint32_t field
 *   UCI_MS_US – int64_t field, UCI value in milliseconds > stored as µs
 *   UCI_S_US  – int64_t field, UCI value in seconds      > stored as µs
 *   UCI_FP    – int64_t _fp field, UCI value is a ratio (e.g. 0.99)
 *               stored as value * 1,000,000
 */
#define UCI_STR(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) snprintf(cfg->field, sizeof(cfg->field), "%s", _v); } while(0)

#define UCI_INT(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = (int)strtol(_v, NULL, 10); } while(0)

#define UCI_U32(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = (uint32_t)strtoul(_v, NULL, 10); } while(0)

#define UCI_MS_US(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = parse_fixed(_v, 1000LL); } while(0)

#define UCI_S_US(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = parse_fixed(_v, 1000000LL); } while(0)

#define UCI_FP(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = parse_fixed(_v, 1000000LL); } while(0)

/* ── defaults ────────────────────────────────────────────────── */

void config_set_defaults(cake_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    snprintf(cfg->dl_if, sizeof(cfg->dl_if), "ifb-wan");
    snprintf(cfg->ul_if, sizeof(cfg->ul_if), "wan");

    cfg->enabled                 = 0;
    cfg->adjust_dl_shaper_rate   = 1;
    cfg->adjust_ul_shaper_rate   = 1;
    cfg->min_dl_shaper_rate_kbps  = 5000;
    cfg->base_dl_shaper_rate_kbps = 20000;
    cfg->max_dl_shaper_rate_kbps  = 80000;
    cfg->min_ul_shaper_rate_kbps  = 5000;
    cfg->base_ul_shaper_rate_kbps = 20000;
    cfg->max_ul_shaper_rate_kbps  = 35000;
    cfg->connection_active_thr_kbps = 2000;

    cfg->no_pingers                 = 6;
    cfg->reflector_ping_interval_us = 300000;   /* 0.3 s */

    /* Built-in reflectors */
    static const char *def_refs[] = {
        "1.1.1.1", "1.0.0.1",
        "8.8.8.8", "8.8.4.4",
        "9.9.9.9", "9.9.9.10",
        "94.140.14.15",  "94.140.14.140",
        "208.67.220.220","208.67.222.222",
        NULL
    };
    cfg->no_reflectors = 0;
    for (int i = 0; def_refs[i] && i < MAX_REFLECTORS; i++) {
        snprintf(cfg->reflectors[i], 64, "%s", def_refs[i]);
        cfg->no_reflectors++;
    }

    /* OWD thresholds (µs) */
    cfg->dl_avg_owd_delta_max_adjust_up_thr_us   = 10000;
    cfg->ul_avg_owd_delta_max_adjust_up_thr_us   = 10000;
    cfg->dl_owd_delta_delay_thr_us               = 30000;
    cfg->ul_owd_delta_delay_thr_us               = 30000;
    cfg->dl_avg_owd_delta_max_adjust_down_thr_us = 60000;
    cfg->ul_avg_owd_delta_max_adjust_down_thr_us = 60000;

    /* EWMA alphas (_fp = value * 1,000,000) */
    cfg->alpha_baseline_increase =    1000;   /* 0.001 */
    cfg->alpha_baseline_decrease = 900000;    /* 0.9   */
    cfg->alpha_delta_ewma        =  95000;    /* 0.095 */

    /* Rate multipliers (_fp) */
    cfg->shaper_rate_min_adjust_down_bufferbloat = 990000;   /* 0.99 */
    cfg->shaper_rate_max_adjust_down_bufferbloat = 750000;   /* 0.75 */
    cfg->shaper_rate_min_adjust_up_load_high     = 1000000;  /* 1.00 */
    cfg->shaper_rate_max_adjust_up_load_high     = 1040000;  /* 1.04 */
    cfg->shaper_rate_adjust_down_load_low        = 990000;   /* 0.99 */
    cfg->shaper_rate_adjust_up_load_low          = 1010000;  /* 1.01 */

    /* Detection */
    cfg->bufferbloat_detection_window = 6;
    cfg->bufferbloat_detection_thr    = 3;
    cfg->high_load_thr                = 750000;  /* 0.75 _fp */

    /* Refractory periods (µs) */
    cfg->bufferbloat_refractory_period_us = 300000;          /* 300 ms */
    cfg->decay_refractory_period_us       = 1000000;         /* 1 s    */

    /* Reflector health */
    cfg->reflector_health_check_interval_us     = 1000000;            /* 1 s   */
    cfg->reflector_response_deadline_us         = 1000000;            /* 1 s   */
    cfg->reflector_misbehaving_detection_window = 60;
    cfg->reflector_misbehaving_detection_thr    = 3;
    cfg->reflector_replacement_interval_us      = 3600LL * 1000000LL; /* 1 hr  */
    cfg->reflector_comparison_interval_us       =   60LL * 1000000LL; /* 1 min */
    cfg->reflector_sum_owd_baselines_delta_thr_us = 20000;  /* 20 ms */
    cfg->reflector_owd_delta_ewma_delta_thr_us    = 10000;  /* 10 ms */

    /* Stall */
    cfg->stall_detection_thr             = 5;
    cfg->connection_stall_thr_kbps       = 10;
    cfg->global_ping_response_timeout_us = 10000000;  /* 10 s */

    /* Misc */
    cfg->sustained_idle_sleep_thr_us         = 60000000;  /* 60 s   */
    cfg->min_shaper_rates_enforcement        = 0;
    cfg->enable_sleep_function               = 1;
    cfg->startup_wait_us                     = 0;
    cfg->monitor_achieved_rates_interval_us  = 200000;   /* 200 ms */
    cfg->if_up_check_interval_us             = 10000000; /* 10 s   */

    /* ── CAKE qdisc options – "piece of cake" defaults ──────────
     *
     * These produce a well-behaved, general-purpose home-router CAKE
     * setup without any SQM script involvement.
     *
     * Reasoning:
     *   diffserv4   – 4-tin structure suits most home traffic mixes.
     *   triple      – triple isolation prevents any one host/flow from
     *                 starving others; best for shared connections.
     *   nat         – mandatory for NAT routers; corrects flow IDs
     *                 distorted by SNAT/masquerade.
     *   wash        – strip DSCP on UL so the ISP cannot prioritise
     *                 traffic unfairly. Applied to UL only (DL carries
     *                 received markings).
     *   overhead 0  – no extra framing overhead by default. Set to
     *                 8 (PPPoE/PTM) or 18 (PPPoE/ATM) as appropriate.
     *   split_gso 1 – essential for accurate per-packet scheduling;
     *                 almost always desired.
     */
    cfg->cake_overhead   = 0;              /* no overhead compensation */
    cfg->cake_mpu        = 0;              /* CAKE default             */
    cfg->cake_nat        = 1;              /* NAT-aware hashing ON     */
    cfg->cake_wash       = 1;             /* DSCP wash ON (UL)        */
    cfg->cake_ack_filter = 0;   /* CAKE_ACK_NONE       – off; safest default */
    cfg->cake_diffserv   = 1;   /* CAKE_DIFFSERV_DIFFSERV4 – 4-tin           */
    cfg->cake_flow_mode  = 7;   /* CAKE_FLOW_TRIPLE    – triple isolation     */
    cfg->cake_atm        = 0;   /* CAKE_ATM_NONE       – no ATM cells         */
    cfg->cake_rtt_us     = 0;              /* CAKE default 100 ms      */
    cfg->cake_split_gso  = 1;             /* split GSO ON             */
    cfg->cake_mq         = 0;             /* standard CAKE (safe default) */

    /* Pinger mode */
    cfg->ping_type = 0;                   /* ICMP echo (works everywhere) */
    cfg->reflectors_file[0] = '\0';       /* disabled; use UCI list       */
}

/* ── UCI load ────────────────────────────────────────────────── */

int config_load(const char *section_name, cake_config_t *cfg)
{
    struct uci_context *ctx;
    struct uci_package *pkg;
    struct uci_section *sec;

    config_set_defaults(cfg);

    ctx = uci_alloc_context();
    if (!ctx) return -1;

    if (uci_load(ctx, "cake_autorate_reborn", &pkg) != UCI_OK) {
        uci_free_context(ctx);
        return -1;
    }

    sec = uci_lookup_section(ctx, pkg, section_name);
    if (!sec) {
        uci_unload(ctx, pkg);
        uci_free_context(ctx);
        return -1;
    }

    snprintf(cfg->instance_id, sizeof(cfg->instance_id), "%s", section_name);

    /* Basic */
    UCI_INT(enabled,                    "enabled");
    UCI_STR(dl_if,                      "dl_if");
    UCI_STR(ul_if,                      "ul_if");
    UCI_INT(adjust_dl_shaper_rate,      "adjust_dl_shaper_rate");
    UCI_INT(adjust_ul_shaper_rate,      "adjust_ul_shaper_rate");
    UCI_U32(min_dl_shaper_rate_kbps,    "min_dl_shaper_rate_kbps");
    UCI_U32(base_dl_shaper_rate_kbps,   "base_dl_shaper_rate_kbps");
    UCI_U32(max_dl_shaper_rate_kbps,    "max_dl_shaper_rate_kbps");
    UCI_U32(min_ul_shaper_rate_kbps,    "min_ul_shaper_rate_kbps");
    UCI_U32(base_ul_shaper_rate_kbps,   "base_ul_shaper_rate_kbps");
    UCI_U32(max_ul_shaper_rate_kbps,    "max_ul_shaper_rate_kbps");
    UCI_U32(connection_active_thr_kbps, "connection_active_thr_kbps");

    /* Pinger – UCI value is in seconds */
    UCI_INT(no_pingers,                  "no_pingers");
    UCI_S_US(reflector_ping_interval_us, "reflector_ping_interval_s");

    /* Reflectors list */
    struct uci_option *refl_opt = uci_lookup_option(ctx, sec, "reflectors");
    if (refl_opt && refl_opt->type == UCI_TYPE_LIST) {
        int idx = 0;
        struct uci_element *e;
        uci_foreach_element(&refl_opt->v.list, e) {
            if (idx >= MAX_REFLECTORS) break;
            snprintf(cfg->reflectors[idx++], 64, "%s", e->name);
        }
        cfg->no_reflectors = idx;
    }

    /* OWD thresholds: UCI in ms > µs */
    UCI_MS_US(dl_avg_owd_delta_max_adjust_up_thr_us,   "dl_avg_owd_delta_max_adjust_up_thr_ms");
    UCI_MS_US(ul_avg_owd_delta_max_adjust_up_thr_us,   "ul_avg_owd_delta_max_adjust_up_thr_ms");
    UCI_MS_US(dl_owd_delta_delay_thr_us,               "dl_owd_delta_delay_thr_ms");
    UCI_MS_US(ul_owd_delta_delay_thr_us,               "ul_owd_delta_delay_thr_ms");
    UCI_MS_US(dl_avg_owd_delta_max_adjust_down_thr_us, "dl_avg_owd_delta_max_adjust_down_thr_ms");
    UCI_MS_US(ul_avg_owd_delta_max_adjust_down_thr_us, "ul_avg_owd_delta_max_adjust_down_thr_ms");

    /* EWMA alphas: UCI is a plain ratio (e.g. "0.001") > _fp */
    UCI_FP(alpha_baseline_increase, "alpha_baseline_increase");
    UCI_FP(alpha_baseline_decrease, "alpha_baseline_decrease");
    UCI_FP(alpha_delta_ewma,        "alpha_delta_ewma");

    /* Rate multipliers: UCI is a plain ratio (e.g. "0.99") > _fp */
    UCI_FP(shaper_rate_min_adjust_down_bufferbloat, "shaper_rate_min_adjust_down_bufferbloat");
    UCI_FP(shaper_rate_max_adjust_down_bufferbloat, "shaper_rate_max_adjust_down_bufferbloat");
    UCI_FP(shaper_rate_min_adjust_up_load_high,     "shaper_rate_min_adjust_up_load_high");
    UCI_FP(shaper_rate_max_adjust_up_load_high,     "shaper_rate_max_adjust_up_load_high");
    UCI_FP(shaper_rate_adjust_down_load_low,        "shaper_rate_adjust_down_load_low");
    UCI_FP(shaper_rate_adjust_up_load_low,          "shaper_rate_adjust_up_load_low");

    /* Detection */
    UCI_INT(bufferbloat_detection_window, "bufferbloat_detection_window");
    UCI_INT(bufferbloat_detection_thr,    "bufferbloat_detection_thr");
    UCI_FP (high_load_thr,                "high_load_thr");

    /* Refractory periods: UCI in ms > µs */
    UCI_MS_US(bufferbloat_refractory_period_us, "bufferbloat_refractory_period_ms");
    UCI_MS_US(decay_refractory_period_us,       "decay_refractory_period_ms");

    /* Reflector health: UCI in seconds > µs */
    UCI_S_US(reflector_health_check_interval_us,     "reflector_health_check_interval_s");
    UCI_S_US(reflector_response_deadline_us,         "reflector_response_deadline_s");
    UCI_INT (reflector_misbehaving_detection_window, "reflector_misbehaving_detection_window");
    UCI_INT (reflector_misbehaving_detection_thr,    "reflector_misbehaving_detection_thr");
    UCI_S_US(reflector_replacement_interval_us,      "reflector_replacement_interval_s");
    UCI_S_US(reflector_comparison_interval_us,       "reflector_comparison_interval_s");
    UCI_MS_US(reflector_sum_owd_baselines_delta_thr_us, "reflector_sum_owd_baselines_delta_thr_ms");
    UCI_MS_US(reflector_owd_delta_ewma_delta_thr_us,    "reflector_owd_delta_ewma_delta_thr_ms");

    /* Stall */
    UCI_INT  (stall_detection_thr,             "stall_detection_thr");
    UCI_U32  (connection_stall_thr_kbps,       "connection_stall_thr_kbps");
    UCI_S_US (global_ping_response_timeout_us, "global_ping_response_timeout_s");

    /* Misc */
    UCI_S_US (sustained_idle_sleep_thr_us,         "sustained_idle_sleep_thr_s");
    UCI_INT  (min_shaper_rates_enforcement,         "min_shaper_rates_enforcement");
    UCI_INT  (enable_sleep_function,                "enable_sleep_function");
    UCI_S_US (startup_wait_us,                      "startup_wait_s");
    UCI_MS_US(monitor_achieved_rates_interval_us,   "monitor_achieved_rates_interval_ms");
    UCI_S_US (if_up_check_interval_us,              "if_up_check_interval_s");

    /* ── CAKE qdisc options ─────────────────────────────────── */

    /*
     * cake_overhead: signed integer (bytes), stored directly.
     * Sentinel INT32_MIN means "omit the attribute" (keep CAKE's default).
     * UCI value "auto" or omission > leaves the default from set_defaults().
     */
    {
        const char *_v = uci_get(ctx, sec, "cake_overhead");
        if (_v)
            cfg->cake_overhead = (int32_t)strtol(_v, NULL, 10);
    }

    UCI_U32(cake_mpu,        "cake_mpu");
    UCI_INT(cake_nat,        "cake_nat");
    UCI_INT(cake_wash,       "cake_wash");
    UCI_INT(cake_ack_filter, "cake_ack_filter");
    UCI_INT(cake_diffserv,   "cake_diffserv");
    UCI_INT(cake_flow_mode,  "cake_flow_mode");
    UCI_INT(cake_atm,        "cake_atm");
    UCI_INT(cake_split_gso,  "cake_split_gso");
    UCI_INT(cake_mq,         "cake_mq");

    /* cake_rtt_ms: UCI in ms > stored as µs (uint32_t) */
    {
        const char *_v = uci_get(ctx, sec, "cake_rtt_ms");
        if (_v)
            cfg->cake_rtt_us = (uint32_t)(strtoul(_v, NULL, 10) * 1000UL);
    }

    /* Pinger mode */
    UCI_INT(ping_type,       "ping_type");
    UCI_STR(reflectors_file, "reflectors_file");

    uci_unload(ctx, pkg);
    uci_free_context(ctx);

    /*
     * Reflectors file override: if reflectors_file is set and readable,
     * load IPs from it (one per line, '#' comments ignored), shuffle them
     * with Fisher-Yates, and replace the UCI reflectors list.
     *
     * Done after UCI unload since it is pure file I/O; the shuffle gives
     * reflector diversity across restarts which is important when pulling
     * from a large list (e.g. https://github.com/tievolu/timestamp-reflectors).
     */
    if (cfg->reflectors_file[0] != '\0') {
        FILE *fp = fopen(cfg->reflectors_file, "r");
        if (fp) {
            char line[64];
            int  count = 0;
            while (fgets(line, sizeof(line), fp) && count < MAX_REFLECTORS) {
                /* Strip trailing newline / whitespace */
                int len = (int)strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                                   line[len-1] == ' '  || line[len-1] == '\t'))
                    line[--len] = '\0';
                if (len == 0 || line[0] == '#')
                    continue;
                snprintf(cfg->reflectors[count++], 64, "%s", line);
            }
            fclose(fp);

            /* Fisher-Yates shuffle so each daemon restart picks a different
             * active set from the large pool. */
            if (count > 1) {
                srand((unsigned int)time(NULL));
                for (int i = count - 1; i > 0; i--) {
                    int j = rand() % (i + 1);
                    char tmp[64];
                    memcpy(tmp,                cfg->reflectors[i], 64);
                    memcpy(cfg->reflectors[i], cfg->reflectors[j], 64);
                    memcpy(cfg->reflectors[j], tmp,                64);
                }
            }
            cfg->no_reflectors = count;
        }
        /* If the file cannot be opened, keep the UCI list as fallback. */
    }

    return 0;
}
