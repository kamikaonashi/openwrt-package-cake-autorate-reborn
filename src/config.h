#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <limits.h>   /* INT32_MIN */

#define MAX_REFLECTORS 64
#define MAX_IF_NAME    32

/*
 * Fixed-point encoding conventions used throughout this struct:
 *
 *   _us   – value stored in microseconds             (int64_t)
 *   _fp   – value stored as integer * 1,000,000      (int64_t)
 *           e.g. 0.75 → 750000, 1.04 → 1040000
 *           Multiply by this, then divide by 1,000,000.
 *
 * All fields that were previously `double` have been converted to one of
 * these two encodings so the daemon runs without any floating-point
 * instructions.
 */

typedef struct {
    /* Instance */
    char     instance_id[32];
    int      enabled;

    /* Interfaces */
    char     dl_if[MAX_IF_NAME];   /* IFB interface for DL shaping   */
    char     ul_if[MAX_IF_NAME];   /* WAN interface for UL shaping    */

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
    int64_t  reflector_ping_interval_us;

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

    /* EWMA / baseline – stored *1,000,000 (_fp encoding) */
    int64_t  alpha_baseline_increase;   /* _fp */
    int64_t  alpha_baseline_decrease;   /* _fp */
    int64_t  alpha_delta_ewma;          /* _fp */

    /* Rate adjustment multipliers – _fp (*1,000,000) */
    int64_t  shaper_rate_min_adjust_down_bufferbloat;
    int64_t  shaper_rate_max_adjust_down_bufferbloat;
    int64_t  shaper_rate_min_adjust_up_load_high;
    int64_t  shaper_rate_max_adjust_up_load_high;
    int64_t  shaper_rate_adjust_down_load_low;
    int64_t  shaper_rate_adjust_up_load_low;

    /* Detection */
    int      bufferbloat_detection_window;
    int      bufferbloat_detection_thr;
    int64_t  high_load_thr;             /* _fp */

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
    int64_t  reflector_sum_owd_baselines_delta_thr_us;
    int64_t  reflector_owd_delta_ewma_delta_thr_us;

    /* Stall */
    int      stall_detection_thr;
    uint32_t connection_stall_thr_kbps;
    int64_t  global_ping_response_timeout_us;

    /* Misc */
    int64_t  sustained_idle_sleep_thr_us;
    int      min_shaper_rates_enforcement;
    int      enable_sleep_function;
    int64_t  startup_wait_us;
    int64_t  monitor_achieved_rates_interval_us;
    int64_t  if_up_check_interval_us;

    /* ── Standalone CAKE qdisc setup options ──────────────────────────
     *
     * These control how the daemon creates the CAKE qdiscs at startup.
     * They replace what SQM scripts previously configured.
     *
     * DL options apply to the IFB interface (ingress shaping).
     * UL options apply to the WAN interface (egress shaping).
     *
     * See tc_netlink.h for the full value reference table.
     * Pass integer literals directly (the enum values are defined in
     * linux/pkt_sched.h and must not be redefined as macros).
     */

    /*
     * cake_overhead – per-packet byte overhead for rate accounting.
     *   0        = no overhead compensation (default; good for Ethernet/WiFi)
     *   8        = PPPoE over PTM (VDSL2 with Ethernet framing)
     *  18        = PPPoE over ATM (ADSL with LLC/SNAP)
     *  INT32_MIN = omit; let CAKE use its compiled default (0)
     *
     * UCI option: cake_overhead (signed integer)
     */
    int32_t  cake_overhead;

    /*
     * cake_mpu – minimum packet unit in bytes.
     * Packets shorter than this are padded before rate accounting.
     *   0 = CAKE default (no padding).
     * UCI option: cake_mpu
     */
    uint32_t cake_mpu;

    /*
     * cake_nat – enable NAT-aware flow hashing.
     * Strongly recommended for home routers performing SNAT/masquerade.
     *   1 = enabled (default), 0 = disabled.
     * UCI option: cake_nat
     */
    int      cake_nat;

    /*
     * cake_wash – strip DSCP/ECN markings on egress.
     * Set on the UL qdisc so the ISP sees unmarked traffic.
     * Typically off on the IFB/DL qdisc.
     *   1 = enabled (default for UL), 0 = disabled.
     * UCI option: cake_wash (applied to UL; DL always 0)
     */
    int      cake_wash;

    /*
     * cake_ack_filter – suppress redundant TCP ACKs.
     *   CAKE_ACK_NONE (0)       = off (default; safest)
     *   CAKE_ACK_FILTER (1)     = moderate filtering
     *   CAKE_ACK_AGGRESSIVE (2) = aggressive (may hurt bidirectional flows)
     * UCI option: cake_ack_filter
     */
    int      cake_ack_filter;

    /*
     * cake_diffserv – tin / diffserv structure.
     *   CAKE_DIFFSERV_DIFFSERV4  (1) = 4 tins: bulk/streaming/video/voice (default)
     *   CAKE_DIFFSERV_DIFFSERV8  (2) = 8 tins (CS0–CS7)
     *   CAKE_DIFFSERV_BESTEFFORT (3) = single best-effort tin
     *   CAKE_DIFFSERV_PRECEDENCE (4) = legacy IP precedence
     * UCI option: cake_diffserv
     */
    int      cake_diffserv;

    /*
     * cake_flow_mode – per-flow / per-host isolation.
     *   CAKE_FLOW_TRIPLE (7) = triple isolation: src-host + dst-host + flow (default)
     *   CAKE_FLOW_HOSTS  (3) = per src+dst host pair
     *   CAKE_FLOW_FLOWS  (4) = per 5-tuple flow only
     *   CAKE_FLOW_DUAL_SRC (5) / CAKE_FLOW_DUAL_DST (6) = asymmetric
     * UCI option: cake_flow_mode
     */
    int      cake_flow_mode;

    /*
     * cake_atm – ATM/PTM cell-size compensation.
     *   CAKE_ATM_NONE (0) = no compensation (default; Ethernet/PPPoE-PTM handled by overhead)
     *   CAKE_ATM_ATM  (1) = 48-byte ATM cell rounding (ADSL)
     *   CAKE_ATM_PTM  (2) = PTM 64-byte expansion (VDSL2; alternative to overhead 8)
     * UCI option: cake_atm
     */
    int      cake_atm;

    /*
     * cake_rtt_us – target RTT for CAKE's AQM in microseconds.
     *   0 = use CAKE default (100 ms = 100000 µs).
     * UCI option: cake_rtt_ms (converted to µs on load)
     */
    uint32_t cake_rtt_us;

    /*
     * cake_split_gso – split GSO super-packets before scheduling.
     * Recommended: 1 (enabled by default).
     * UCI option: cake_split_gso
     */
    int      cake_split_gso;

    /*
     * cake_mq – use the "cake-mq" multi-queue qdisc (OpenWrt 25.12+).
     *
     * cake-mq distributes CAKE scheduling across per-CPU TX queues, which
     * significantly reduces CPU bottlenecks on multi-core SoCs (Filogic,
     * IPQ807x, etc.) at high throughput while preserving all CAKE AQM logic.
     *
     * Requirements:
     *   - OpenWrt 25.12 or later
     *   - kmod-sched-cake-mq package installed
     *   - Multi-core router (no benefit on single-core)
     *
     *   0 = use standard CAKE (default; safe on all hardware)
     *   1 = use cake-mq      (OpenWrt 25.12+)
     *
     * UCI option: cake_mq
     */
    int      cake_mq;

    /* ── Pinger mode ──────────────────────────────────────────────────
     *
     * ping_type – selects the ICMP packet type used for OWD measurement.
     *
     *   0 = ICMP Echo (type 8/0)
     *       Measures RTT only; OWD estimated as RTT/2.
     *       Works against any host that responds to ping.
     *       Assumes symmetric path – incorrect for asymmetric broadband.
     *
     *   1 = ICMP Timestamp (type 13/14)
     *       Reflector returns its own receive and transmit timestamps,
     *       giving true per-direction OWD:
     *         UL OWD = reflector_receive_ms  − originate_ms
     *         DL OWD = local_receive_ms      − reflector_transmit_ms
     *       Clock offset is absorbed by the EWMA baseline (constant offset
     *       cancels when computing deltas from baseline).
     *       Requires reflectors that support ICMP timestamp replies.
     *       Recommended list: https://github.com/tievolu/timestamp-reflectors
     *
     * UCI option: ping_type
     */
    int      ping_type;

    /*
     * reflectors_file – path to a plain-text file of reflector IP addresses,
     * one per line. Lines starting with '#' and blank lines are ignored.
     *
     * When set, overrides the UCI 'reflectors' list.  Intended for use with
     * the large timestamp-reflector lists from:
     *   https://github.com/tievolu/timestamp-reflectors
     *
     * The daemon loads all IPs, shuffles them (Fisher-Yates), and uses the
     * first no_reflectors entries (first no_pingers active, rest spares).
     *
     * UCI option: reflectors_file
     * Default: "" (disabled; use UCI list)
     */
    char     reflectors_file[256];

} cake_config_t;

int  config_load(const char *uci_section, cake_config_t *cfg);
void config_set_defaults(cake_config_t *cfg);

#endif /* CONFIG_H */
