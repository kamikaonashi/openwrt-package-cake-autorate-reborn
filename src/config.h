#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

#define MAX_REFLECTORS 64
#define MAX_IF_NAME    32

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

    /* Pinger */
    char     pinger_method[16];   /* "fping" only for now */
    int      no_pingers;
    double   reflector_ping_interval_s;

    /* Reflectors (all; first no_pingers are active, rest are spares) */
    char     reflectors[MAX_REFLECTORS][64];
    int      no_reflectors;

    /* OWD thresholds (UCI: ms → stored as us) */
    int64_t  dl_avg_owd_delta_max_adjust_up_thr_us;
    int64_t  ul_avg_owd_delta_max_adjust_up_thr_us;
    int64_t  dl_owd_delta_delay_thr_us;
    int64_t  ul_owd_delta_delay_thr_us;
    int64_t  dl_avg_owd_delta_max_adjust_down_thr_us;
    int64_t  ul_avg_owd_delta_max_adjust_down_thr_us;

    /* EWMA / baseline */
    double   alpha_baseline_increase;
    double   alpha_baseline_decrease;
    double   alpha_delta_ewma;

    /* Rate adjustment multipliers (stored *1e6 as int64) */
    int64_t  shaper_rate_min_adjust_down_bufferbloat;
    int64_t  shaper_rate_max_adjust_down_bufferbloat;
    int64_t  shaper_rate_min_adjust_up_load_high;
    int64_t  shaper_rate_max_adjust_up_load_high;
    int64_t  shaper_rate_adjust_down_load_low;
    int64_t  shaper_rate_adjust_up_load_low;

    /* Detection */
    int      bufferbloat_detection_window;
    int      bufferbloat_detection_thr;
    double   high_load_thr;

    /* Refractory periods (UCI: ms → stored as us) */
    int64_t  bufferbloat_refractory_period_us;
    int64_t  decay_refractory_period_us;

    /* Reflector health (UCI: s → stored as us unless noted) */
    int64_t  reflector_health_check_interval_us;
    int64_t  reflector_response_deadline_us;
    int      reflector_misbehaving_detection_window;
    int      reflector_misbehaving_detection_thr;
    int64_t  reflector_replacement_interval_us;
    int64_t  reflector_comparison_interval_us;
    double   reflector_sum_owd_baselines_delta_thr_ms;
    double   reflector_owd_delta_ewma_delta_thr_ms;

    /* Stall */
    int      stall_detection_thr;
    uint32_t connection_stall_thr_kbps;
    double   global_ping_response_timeout_s;

    /* Misc */
    double   sustained_idle_sleep_thr_s;
    int      min_shaper_rates_enforcement;
    int      enable_sleep_function;
    double   startup_wait_s;
    int64_t  monitor_achieved_rates_interval_us;
    double   if_up_check_interval_s;
} cake_config_t;

int  config_load(const char *uci_section, cake_config_t *cfg);
void config_set_defaults(cake_config_t *cfg);

#endif /* CONFIG_H */
