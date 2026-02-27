#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_REFLECTORS 64
#define MAX_IF_NAME    32

/*
 * Fixed-point encoding conventions used throughout this struct:
 *
 *   _us   – value stored in microseconds             (int64_t)
 *   _fp   – value stored as integer * 1,000,000      (int64_t)
 *           e.g. 0.75 > 750000, 1.04 > 1040000
 *           Multiply by this, then divide by 1000000.
 *
 * All fields that were previously `double` have been converted to one of
 * these two encodings so the daemon runs without any floating-point
 * instructions.  This is critical for MIPS/ARM cores that lack an FPU and
 * would otherwise pay a heavy software-emulation penalty on every EWMA
 * update.
 */

typedef struct {
    /* Instance */
    char     instance_id[32];
    int      enabled;

    /* Interfaces */
    char     dl_if[MAX_IF_NAME];
    char     ul_if[MAX_IF_NAME];

    /* Adjust flags */
    int      adjust_dl_shaper_rate;
    int      adjust_ul_shaper_rate;

    /* Shaper rates (kbps) */
    uint32_t min_dl_shaper_rate_kbps;
    uint32_t base_dl_shaper_rate_kbps;
    uint32_t max_dl_shaper_rate_kbps;
    uint32_t min_ul_shaper_rate_kbps;
    uint32_t base_ul_shaper_rate_kbps;
    uint32_t max_ul_shaper_rate_kbps;

    /* Connection activity */
    uint32_t connection_active_thr_kbps;

    /* Pinger – interval stored as µs (UCI option is in seconds) */
    int      no_pingers;
    int64_t  reflector_ping_interval_us;  /* was: double reflector_ping_interval_s */

    /* Reflectors (all; first no_pingers are active, rest are spares) */
    char     reflectors[MAX_REFLECTORS][64];
    int      no_reflectors;

    /* OWD thresholds (UCI: ms → stored as µs) */
    int64_t  dl_avg_owd_delta_max_adjust_up_thr_us;
    int64_t  ul_avg_owd_delta_max_adjust_up_thr_us;
    int64_t  dl_owd_delta_delay_thr_us;
    int64_t  ul_owd_delta_delay_thr_us;
    int64_t  dl_avg_owd_delta_max_adjust_down_thr_us;
    int64_t  ul_avg_owd_delta_max_adjust_down_thr_us;

    /*
     * EWMA / baseline – stored *1,000,000 (_fp encoding).
     * e.g. alpha_baseline_increase = 1000  means α = 0.001
     *      alpha_baseline_decrease = 900000 means α = 0.9
     * was: double alpha_baseline_increase / decrease / alpha_delta_ewma
     */
    int64_t  alpha_baseline_increase;   /* _fp */
    int64_t  alpha_baseline_decrease;   /* _fp */
    int64_t  alpha_delta_ewma;          /* _fp */

    /* Rate adjustment multipliers – already _fp (*1,000,000) */
    int64_t  shaper_rate_min_adjust_down_bufferbloat;
    int64_t  shaper_rate_max_adjust_down_bufferbloat;
    int64_t  shaper_rate_min_adjust_up_load_high;
    int64_t  shaper_rate_max_adjust_up_load_high;
    int64_t  shaper_rate_adjust_down_load_low;
    int64_t  shaper_rate_adjust_up_load_low;

    /* Detection */
    int      bufferbloat_detection_window;
    int      bufferbloat_detection_thr;
    int64_t  high_load_thr;             /* _fp; was: double high_load_thr */

    /* Refractory periods (UCI: ms → stored as µs) */
    int64_t  bufferbloat_refractory_period_us;
    int64_t  decay_refractory_period_us;

    /* Reflector health (UCI: s → stored as µs unless noted) */
    int64_t  reflector_health_check_interval_us;
    int64_t  reflector_response_deadline_us;
    int      reflector_misbehaving_detection_window;
    int      reflector_misbehaving_detection_thr;
    int64_t  reflector_replacement_interval_us;
    int64_t  reflector_comparison_interval_us;
    /* OWD delta thresholds for comparison-based replacement (UCI: ms → µs) */
    int64_t  reflector_sum_owd_baselines_delta_thr_us;  /* was: double …_ms */
    int64_t  reflector_owd_delta_ewma_delta_thr_us;     /* was: double …_ms */

    /* Stall */
    int      stall_detection_thr;
    uint32_t connection_stall_thr_kbps;
    int64_t  global_ping_response_timeout_us;  /* was: double …_s */

    /* Misc */
    int64_t  sustained_idle_sleep_thr_us;       /* was: double …_s */
    int      min_shaper_rates_enforcement;
    int      enable_sleep_function;
    int64_t  startup_wait_us;                   /* was: double startup_wait_s */
    int64_t  monitor_achieved_rates_interval_us;
    int64_t  if_up_check_interval_us;           /* was: double …_s */
} cake_config_t;

int  config_load(const char *uci_section, cake_config_t *cfg);
void config_set_defaults(cake_config_t *cfg);

#endif /* CONFIG_H */
