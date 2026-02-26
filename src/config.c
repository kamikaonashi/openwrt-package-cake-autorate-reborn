#include "config.h"
#include <string.h>
#include <stdlib.h>
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
 * Conversion macros
 *   UCI_STR   – string field
 *   UCI_INT   – int field, UCI value is a plain integer
 *   UCI_U32   – uint32_t field
 *   UCI_DBL   – double field (value stored as-is)
 *   UCI_MS_US – int64_t field, UCI value is in milliseconds, stored as µs
 *   UCI_S_US  – int64_t field, UCI value is in seconds,      stored as µs
 *   UCI_MULT  – int64_t field, UCI value is a multiplier (e.g. 0.99),
 *               stored as value * 1e6 (fixed-point *1e6)
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

#define UCI_DBL(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = strtod(_v, NULL); } while(0)

#define UCI_MS_US(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = (int64_t)(strtod(_v, NULL) * 1000.0); } while(0)

#define UCI_S_US(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = (int64_t)(strtod(_v, NULL) * 1000000.0); } while(0)

#define UCI_MULT(field, name) \
    do { const char *_v = uci_get(ctx, sec, name); \
         if (_v) cfg->field = (int64_t)(strtod(_v, NULL) * 1e6); } while(0)

/* ── defaults ────────────────────────────────────────────────── */

void config_set_defaults(cake_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    snprintf(cfg->dl_if,  sizeof(cfg->dl_if),  "ifb-wan");
    snprintf(cfg->ul_if,  sizeof(cfg->ul_if),  "wan");
    snprintf(cfg->pinger_method, sizeof(cfg->pinger_method), "fping");

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

    cfg->no_pingers              = 6;
    cfg->reflector_ping_interval_s = 0.3;

    /* Built-in reflectors */
    static const char *def_refs[] = {
        "1.1.1.1", "1.0.0.1",
        "8.8.8.8", "8.8.4.4",
        "9.9.9.9", "9.9.9.10",
        "94.140.14.15", "94.140.14.140",
        "208.67.220.220", "208.67.222.222",
        NULL
    };
    cfg->no_reflectors = 0;
    for (int i = 0; def_refs[i] && i < MAX_REFLECTORS; i++) {
        snprintf(cfg->reflectors[i], 64, "%s", def_refs[i]);
        cfg->no_reflectors++;
    }

    /* OWD thresholds (stored in µs) */
    cfg->dl_avg_owd_delta_max_adjust_up_thr_us   = 10000;
    cfg->ul_avg_owd_delta_max_adjust_up_thr_us   = 10000;
    cfg->dl_owd_delta_delay_thr_us               = 30000;
    cfg->ul_owd_delta_delay_thr_us               = 30000;
    cfg->dl_avg_owd_delta_max_adjust_down_thr_us = 60000;
    cfg->ul_avg_owd_delta_max_adjust_down_thr_us = 60000;

    /* EWMA */
    cfg->alpha_baseline_increase = 0.001;
    cfg->alpha_baseline_decrease = 0.9;
    cfg->alpha_delta_ewma        = 0.095;

    /* Rate multipliers (*1e6 fixed-point) */
    cfg->shaper_rate_min_adjust_down_bufferbloat = (int64_t)(0.99 * 1e6);
    cfg->shaper_rate_max_adjust_down_bufferbloat = (int64_t)(0.75 * 1e6);
    cfg->shaper_rate_min_adjust_up_load_high     = (int64_t)(1.00 * 1e6);
    cfg->shaper_rate_max_adjust_up_load_high     = (int64_t)(1.04 * 1e6);
    cfg->shaper_rate_adjust_down_load_low        = (int64_t)(0.99 * 1e6);
    cfg->shaper_rate_adjust_up_load_low          = (int64_t)(1.01 * 1e6);

    /* Detection */
    cfg->bufferbloat_detection_window = 6;
    cfg->bufferbloat_detection_thr    = 3;
    cfg->high_load_thr                = 0.75;

    /* Refractory periods (µs) */
    cfg->bufferbloat_refractory_period_us = 300000;    /* 300 ms */
    cfg->decay_refractory_period_us       = 1000000;   /* 1 s   */

    /* Reflector health */
    cfg->reflector_health_check_interval_us     = 1000000;         /* 1 s  */
    cfg->reflector_response_deadline_us         = 1000000;         /* 1 s  */
    cfg->reflector_misbehaving_detection_window = 60;
    cfg->reflector_misbehaving_detection_thr    = 3;
    cfg->reflector_replacement_interval_us      = 3600LL * 1000000LL; /* 1 hr */
    cfg->reflector_comparison_interval_us       = 60LL  * 1000000LL; /* 1 min */
    cfg->reflector_sum_owd_baselines_delta_thr_ms = 20.0;
    cfg->reflector_owd_delta_ewma_delta_thr_ms    = 10.0;

    /* Stall */
    cfg->stall_detection_thr        = 5;
    cfg->connection_stall_thr_kbps  = 10;
    cfg->global_ping_response_timeout_s = 10.0;

    /* Misc */
    cfg->sustained_idle_sleep_thr_s         = 60.0;
    cfg->min_shaper_rates_enforcement       = 0;
    cfg->enable_sleep_function              = 1;
    cfg->startup_wait_s                     = 0.0;
    cfg->monitor_achieved_rates_interval_us = 200000; /* 200 ms */
    cfg->if_up_check_interval_s             = 10.0;
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

    /* Pinger */
    UCI_STR(pinger_method,              "pinger_method");
    UCI_INT(no_pingers,                 "no_pingers");
    UCI_DBL(reflector_ping_interval_s,  "reflector_ping_interval_s");

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

    /* OWD thresholds: UCI stores in ms, we store µs */
    UCI_MS_US(dl_avg_owd_delta_max_adjust_up_thr_us,   "dl_avg_owd_delta_max_adjust_up_thr_ms");
    UCI_MS_US(ul_avg_owd_delta_max_adjust_up_thr_us,   "ul_avg_owd_delta_max_adjust_up_thr_ms");
    UCI_MS_US(dl_owd_delta_delay_thr_us,               "dl_owd_delta_delay_thr_ms");
    UCI_MS_US(ul_owd_delta_delay_thr_us,               "ul_owd_delta_delay_thr_ms");
    UCI_MS_US(dl_avg_owd_delta_max_adjust_down_thr_us, "dl_avg_owd_delta_max_adjust_down_thr_ms");
    UCI_MS_US(ul_avg_owd_delta_max_adjust_down_thr_us, "ul_avg_owd_delta_max_adjust_down_thr_ms");

    /* EWMA */
    UCI_DBL(alpha_baseline_increase, "alpha_baseline_increase");
    UCI_DBL(alpha_baseline_decrease, "alpha_baseline_decrease");
    UCI_DBL(alpha_delta_ewma,        "alpha_delta_ewma");

    /* Rate multipliers: UCI stores as float (e.g. 0.99), we store *1e6 */
    UCI_MULT(shaper_rate_min_adjust_down_bufferbloat, "shaper_rate_min_adjust_down_bufferbloat");
    UCI_MULT(shaper_rate_max_adjust_down_bufferbloat, "shaper_rate_max_adjust_down_bufferbloat");
    UCI_MULT(shaper_rate_min_adjust_up_load_high,     "shaper_rate_min_adjust_up_load_high");
    UCI_MULT(shaper_rate_max_adjust_up_load_high,     "shaper_rate_max_adjust_up_load_high");
    UCI_MULT(shaper_rate_adjust_down_load_low,        "shaper_rate_adjust_down_load_low");
    UCI_MULT(shaper_rate_adjust_up_load_low,          "shaper_rate_adjust_up_load_low");

    /* Detection */
    UCI_INT(bufferbloat_detection_window, "bufferbloat_detection_window");
    UCI_INT(bufferbloat_detection_thr,    "bufferbloat_detection_thr");
    UCI_DBL(high_load_thr,                "high_load_thr");

    /* Refractory periods: UCI in ms, stored as µs */
    UCI_MS_US(bufferbloat_refractory_period_us, "bufferbloat_refractory_period_ms");
    UCI_MS_US(decay_refractory_period_us,       "decay_refractory_period_ms");

    /* Reflector health: UCI in seconds, stored as µs */
    UCI_S_US(reflector_health_check_interval_us,     "reflector_health_check_interval_s");
    UCI_S_US(reflector_response_deadline_us,         "reflector_response_deadline_s");
    UCI_INT (reflector_misbehaving_detection_window, "reflector_misbehaving_detection_window");
    UCI_INT (reflector_misbehaving_detection_thr,    "reflector_misbehaving_detection_thr");
    UCI_S_US(reflector_replacement_interval_us,      "reflector_replacement_interval_s");
    UCI_S_US(reflector_comparison_interval_us,       "reflector_comparison_interval_s");
    UCI_DBL (reflector_sum_owd_baselines_delta_thr_ms, "reflector_sum_owd_baselines_delta_thr_ms");
    UCI_DBL (reflector_owd_delta_ewma_delta_thr_ms,    "reflector_owd_delta_ewma_delta_thr_ms");

    /* Stall */
    UCI_INT(stall_detection_thr,           "stall_detection_thr");
    UCI_U32(connection_stall_thr_kbps,     "connection_stall_thr_kbps");
    UCI_DBL(global_ping_response_timeout_s,"global_ping_response_timeout_s");

    /* Misc */
    UCI_DBL(sustained_idle_sleep_thr_s,         "sustained_idle_sleep_thr_s");
    UCI_INT(min_shaper_rates_enforcement,        "min_shaper_rates_enforcement");
    UCI_INT(enable_sleep_function,               "enable_sleep_function");
    UCI_DBL(startup_wait_s,                      "startup_wait_s");
    UCI_MS_US(monitor_achieved_rates_interval_us,"monitor_achieved_rates_interval_ms");
    UCI_DBL(if_up_check_interval_s,              "if_up_check_interval_s");

    uci_unload(ctx, pkg);
    uci_free_context(ctx);
    return 0;
}
