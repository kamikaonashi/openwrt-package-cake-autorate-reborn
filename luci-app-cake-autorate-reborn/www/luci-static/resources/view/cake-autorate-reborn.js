'use strict';
'require view';
'require form';
'require uci';
'require rpc';
'require ui';

var callInitAction = rpc.declare({
    object: 'luci',
    method: 'setInitAction',
    params: [ 'name', 'action' ],
    expect: { result: false }
});

/*
 * callFsStat – probe whether a sysfs/filesystem path exists.
 * Used to check for /sys/module/sch_cake_mq (present iff module is loaded).
 */
var callFsStat = rpc.declare({
    object: 'file',
    method: 'stat',
    params: [ 'path' ],
    expect: { type: '' }
});

/*
 * makeServiceButton – returns a <button> DOM node that calls callInitAction,
 * disables itself while pending, and shows a success/error notification.
 */
function makeServiceButton(label, style, action) {
    return E('button', {
        'class': 'btn cbi-button cbi-button-' + style,
        'click': function(ev) {
            var btn = ev.currentTarget;
            btn.disabled = true;
            return callInitAction('cake-autorate-reborn', action)
                .then(function(res) {
                    btn.disabled = false;
                    ui.addNotification(null,
                        E('p', res
                            ? _('Service %s successful.').format(action)
                            : _('Service %s failed.').format(action)),
                        res ? 'info' : 'error');
                })
                .catch(function(err) {
                    btn.disabled = false;
                    ui.addNotification(null,
                        E('p', _('RPC error: %s').format(err.message || String(err))),
                        'error');
                });
        }
    }, label);
}

return view.extend({
    /*
     * load() – runs before render().
     * We load UCI config AND check whether sch_cake_mq is loaded in the
     * kernel so render() can decide whether to show the cake-mq option.
     *
     * /sys/module/sch_cake_mq is created by the kernel when the module is
     * loaded; fs.stat returns a non-empty type string when it exists.
     */
    load: function() {
        return Promise.all([
            uci.load('cake_autorate_reborn'),
            callFsStat('/sys/module/sch_cake_mq').catch(function() { return ''; })
        ]);
    },

    /*
     * handleSaveApply – override the default Save & Apply so that after
     * UCI changes are committed we always restart the daemon.
     *
     * procd's reload-trigger mechanism only fires on already-registered
     * (running) service instances.  If the service was disabled (no running
     * instance) and the user enables it and hits Save & Apply, procd has
     * nothing to trigger and the daemon never starts.  Explicitly calling
     * restart here covers both the cold-start (was disabled) and the
     * already-running (settings change) cases.
     */
    handleSaveApply: function(ev, mode) {
        return this.handleSave(ev).then(function() {
            return ui.changes.apply(mode).then(function() {
                return callInitAction('cake-autorate-reborn', 'restart')
                    .catch(function() { /* ignore if service not present yet */ });
            });
        });
    },

    render: function(data) {
        var cakeMqAvailable = (data[1] !== '');
        var m, s, o;

        m = new form.Map('cake_autorate_reborn', _('CAKE Autorate Reborn'),
            _('Adaptive CAKE shaper – automatically adjusts download and upload ' +
              'bandwidth based on measured one-way delay (OWD). ' +
              'Standalone operation: no sqm-scripts required.'));

        /* ── Per-instance configuration sections ───────────── */
        uci.sections('cake_autorate_reborn', 'cake_autorate_reborn').forEach(function(section) {
            var sid   = section['.name'];
            var title = sid.charAt(0).toUpperCase() + sid.slice(1) + ' ' + _('Instance');

            s = m.section(form.NamedSection, sid, 'cake_autorate_reborn', title);
            s.addremove = false;
            s.anonymous = false;

            s.tab('general',  _('General'));
            s.tab('qdisc',    _('CAKE Qdisc'));
            s.tab('advanced', _('Advanced'));
            s.tab('health',   _('Reflector Health'));

            /* ════════════════════════════════════════════════
             * General tab
             * ════════════════════════════════════════════════ */
            o = s.taboption('general', form.Flag, 'enabled', _('Enable'));
            o.default  = '0';
            o.rmempty  = false;

            o = s.taboption('general', form.Value, 'dl_if',
                _('Download Interface'),
                _('IFB interface that carries shaped ingress traffic (e.g. <code>ifb-wan</code>). ' +
                  'Created automatically at startup if it does not exist.'));
            o.rmempty = false;

            o = s.taboption('general', form.Value, 'ul_if',
                _('Upload Interface'),
                _('WAN-facing interface for egress shaping (e.g. <code>wan</code>).'));
            o.rmempty = false;

            o = s.taboption('general', form.Flag, 'adjust_dl_shaper_rate',
                _('Adjust Download Shaper'));
            o.default = '1';

            o = s.taboption('general', form.Flag, 'adjust_ul_shaper_rate',
                _('Adjust Upload Shaper'));
            o.default = '1';

            function rateOption(tab, name, label, def) {
                var opt = s.taboption(tab, form.Value, name, label);
                opt.datatype = 'uinteger';
                opt.default  = String(def);
                return opt;
            }

            rateOption('general', 'min_dl_shaper_rate_kbps',   _('Min Download Rate (kbps)'),   5000);
            rateOption('general', 'base_dl_shaper_rate_kbps',  _('Base Download Rate (kbps)'), 20000);
            rateOption('general', 'max_dl_shaper_rate_kbps',   _('Max Download Rate (kbps)'),  80000);
            rateOption('general', 'min_ul_shaper_rate_kbps',   _('Min Upload Rate (kbps)'),     5000);
            rateOption('general', 'base_ul_shaper_rate_kbps',  _('Base Upload Rate (kbps)'),  20000);
            rateOption('general', 'max_ul_shaper_rate_kbps',   _('Max Upload Rate (kbps)'),   35000);
            rateOption('general', 'connection_active_thr_kbps',
                _('Connection Active Threshold (kbps)'), 2000);

            /* ════════════════════════════════════════════════
             * CAKE Qdisc tab
             * ════════════════════════════════════════════════ */

            /* cake-mq first – it's the headline new feature */
            o = s.taboption('qdisc', form.Flag, 'cake_mq',
                _('Use cake-mq (multi-queue)'),
                cakeMqAvailable
                    ? _('<strong>OpenWrt 25.12+ only.</strong> ' +
                        'Uses the <code>cake-mq</code> qdisc instead of <code>cake</code>. ' +
                        'Distributes CAKE scheduling across per-CPU TX queues for significantly ' +
                        'lower CPU overhead at high throughput on supported multi-core routers ')
                    : _('<span style="color:#c00">⚠ The <code>sch_cake_mq</code> kernel module ' +
                        'is not loaded on this device.</span> ' +
                        'Install <code>kmod-sched-cake-mq</code> (OpenWrt 25.12+) and reboot ' +
                        'before enabling this option. Enabling it without the module has no ' +
                        'effect — the daemon falls back to standard CAKE.'));
            o.default = '0';
            o.readonly = !cakeMqAvailable;

            o = s.taboption('qdisc', form.ListValue, 'cake_diffserv',
                _('Traffic Classification'),
                _('Determines how packets are sorted into CAKE tins for prioritisation.'));
            o.default = '1';
            o.value('1', _('diffserv4 – 4 tins: Bulk / Best-Effort / Streaming / Voice (recommended)'));
            o.value('2', _('diffserv8 – 8 tins mapped to DSCP CS0–CS7'));
            o.value('3', _('besteffort – single tin, no prioritisation'));
            o.value('4', _('precedence – legacy IP Precedence field'));

            o = s.taboption('qdisc', form.ListValue, 'cake_flow_mode',
                _('Flow Isolation'),
                _('Controls how CAKE identifies flows for fair-queuing.'));
            o.default = '7';
            o.value('7', _('triple – src host + dst host + 5-tuple flow (recommended)'));
            o.value('5', _('dual-srchost – per src host + flow'));
            o.value('6', _('dual-dsthost – per dst host + flow'));
            o.value('3', _('hosts – per src+dst host pair'));
            o.value('4', _('flows – per 5-tuple flow only'));
            o.value('0', _('none – no isolation'));

            o = s.taboption('qdisc', form.Flag, 'cake_nat',
                _('NAT-Aware Flow Hashing'),
                _('Peek inside NAT/masquerade to correctly identify flows by their real ' +
                  'source address. Strongly recommended for home routers doing SNAT.'));
            o.default = '1';

            o = s.taboption('qdisc', form.Flag, 'cake_wash',
                _('DSCP Wash (Upload)'),
                _('Strip DSCP markings on egress (upload to ISP) so the carrier cannot ' +
                  'exploit your internal QoS markings. Applied to the WAN qdisc only; ' +
                  'download markings are preserved for local use.'));
            o.default = '1';

            o = s.taboption('qdisc', form.Value, 'cake_overhead',
                _('Per-Packet Overhead (bytes)'),
                _('Framing overhead added to each packet before rate accounting. ' +
                  'Common values: <code>0</code> = Ethernet/plain, ' +
                  '<code>8</code> = PPPoE over PTM (VDSL2), ' +
                  '<code>18</code> = PPPoE over ATM (ADSL LLC/SNAP).'));
            o.datatype = 'integer';
            o.default  = '0';
            o.placeholder = '0';

            o = s.taboption('qdisc', form.ListValue, 'cake_atm',
                _('ATM/PTM Cell Compensation'),
                _('Hardware-layer cell-size rounding. Use instead of (or in addition to) overhead ' +
                  'for DSL links with cell-based framing.'));
            o.default = '0';
            o.value('0', _('None (default – Ethernet / PTM with overhead)'));
            o.value('1', _('ATM – 48-byte cell rounding (ADSL)'));
            o.value('2', _('PTM – 64-byte expansion (VDSL2 alternative)'));

            o = s.taboption('qdisc', form.Value, 'cake_mpu',
                _('Minimum Packet Unit (bytes)'),
                _('Packets shorter than this are padded before rate accounting. ' +
                  '<code>0</code> = disabled (CAKE default).'));
            o.datatype = 'uinteger';
            o.default  = '0';
            o.placeholder = '0';

            o = s.taboption('qdisc', form.ListValue, 'cake_ack_filter',
                _('ACK Filter'),
                _('Suppress redundant TCP ACKs on the upload path to recover capacity ' +
                  'on asymmetric connections. Off by default (safest).'));
            o.default = '0';
            o.value('0', _('Off (default)'));
            o.value('1', _('Moderate filtering'));
            o.value('2', _('Aggressive'));

            o = s.taboption('qdisc', form.Value, 'cake_rtt_ms',
                _('Target RTT (ms)'),
                _("CAKE's internal AQM target RTT. " +
                  '<code>0</code> = use CAKE default (100 ms). ' +
                  'Lower values improve latency on very fast links; ' +
                  'higher values suit high-latency paths.'));
            o.datatype = 'uinteger';
            o.default  = '0';
            o.placeholder = '0 (CAKE default = 100 ms)';

            o = s.taboption('qdisc', form.Flag, 'cake_split_gso',
                _('Split GSO Super-Packets'),
                _('Break large GSO/GRO segment groups into individual packets before scheduling. ' +
                  'Strongly recommended – improves per-packet latency on high-throughput links.'));
            o.default = '1';

            /* ════════════════════════════════════════════════
             * Advanced tab
             * ════════════════════════════════════════════════ */

            /* ── Pinger mode ──────────────────────────────── */
            o = s.taboption('advanced', form.ListValue, 'ping_type',
                _('Ping Type'),
                _('ICMP packet type used for OWD measurement.<br>' +
                  '<strong>ICMP Echo (type 8)</strong>: measures RTT; OWD = RTT/2. ' +
                  'Works against any host. Assumes symmetric path — inaccurate ' +
                  'for asymmetric connections (DSL, cable, DOCSIS).<br>' +
                  '<strong>ICMP Timestamp (type 13)</strong>: reflector returns its own ' +
                  'receive/transmit timestamps giving true per-direction OWD. ' +
                  'Recommended for asymmetric broadband. Requires reflectors that ' +
                  'support timestamp replies — see ' +
                  '<a href="https://github.com/tievolu/timestamp-reflectors" target="_blank">' +
                  'timestamp-reflectors</a>.'));
            o.default = '0';
            o.value('0', _('ICMP Echo (type 8) – RTT/2, symmetric estimate'));
            o.value('1', _('ICMP Timestamp (type 13) – true per-direction OWD'));

            o = s.taboption('advanced', form.Value, 'reflectors_file',
                _('Reflectors File'),
                _('Path to a plain-text file of reflector IP addresses, one per line ' +
                  '(lines starting with <code>#</code> are ignored). ' +
                  'When set, overrides the Reflectors list above. ' +
                  'The daemon shuffles the list on each startup for reflector diversity.<br>' +
                  'Intended for use with large timestamp-reflector lists from ' +
                  '<a href="https://github.com/tievolu/timestamp-reflectors" target="_blank">' +
                  'github.com/tievolu/timestamp-reflectors</a>.<br>' +
                  'Leave blank to use the Reflectors list.'));
            o.placeholder = '/etc/cake-autorate/timestamp-reflectors.txt';

            /* ── Pinger tuning ────────────────────────────── */
            o = s.taboption('advanced', form.Value, 'no_pingers',
                _('Number of Active Pingers'),
                _('How many reflectors to ping concurrently.'));
            o.datatype = 'range(1,20)';
            o.default  = '6';

            o = s.taboption('advanced', form.Value, 'reflector_ping_interval_s',
                _('Ping Interval (s)'),
                _('Time between pings to each individual reflector.'));
            o.datatype = 'float';
            o.default  = '0.3';

            o = s.taboption('advanced', form.DynamicList, 'reflectors',
                _('Reflectors'),
                _('ICMP ping targets. The first <em>N</em> (= Number of Active Pingers) ' +
                  'are active; the rest are spares for automatic replacement.'));
            o.datatype = 'or(ipaddr, hostname)';

            o = s.taboption('advanced', form.Value, 'dl_owd_delta_delay_thr_ms',
                _('DL Delay Threshold (ms)'),
                _('OWD delta above this counts as a bufferbloat event for download.'));
            o.datatype = 'float'; o.default = '30.0';

            o = s.taboption('advanced', form.Value, 'ul_owd_delta_delay_thr_ms',
                _('UL Delay Threshold (ms)'));
            o.datatype = 'float'; o.default = '30.0';

            o = s.taboption('advanced', form.Value, 'dl_avg_owd_delta_max_adjust_up_thr_ms',
                _('DL Avg OWD Max Adjust-Up Threshold (ms)'),
                _('At or below this average OWD delta, rate increases at maximum speed.'));
            o.datatype = 'float'; o.default = '10.0';

            o = s.taboption('advanced', form.Value, 'ul_avg_owd_delta_max_adjust_up_thr_ms',
                _('UL Avg OWD Max Adjust-Up Threshold (ms)'));
            o.datatype = 'float'; o.default = '10.0';

            o = s.taboption('advanced', form.Value, 'dl_avg_owd_delta_max_adjust_down_thr_ms',
                _('DL Avg OWD Max Adjust-Down Threshold (ms)'),
                _('At or above this average OWD delta, rate decreases at maximum speed.'));
            o.datatype = 'float'; o.default = '60.0';

            o = s.taboption('advanced', form.Value, 'ul_avg_owd_delta_max_adjust_down_thr_ms',
                _('UL Avg OWD Max Adjust-Down Threshold (ms)'));
            o.datatype = 'float'; o.default = '60.0';

            o = s.taboption('advanced', form.Value, 'alpha_baseline_increase',
                _('Baseline EWMA α (increase)'),
                _('Small value → slow upward tracking (0.001 = very slow).'));
            o.datatype = 'float'; o.default = '0.001';

            o = s.taboption('advanced', form.Value, 'alpha_baseline_decrease',
                _('Baseline EWMA α (decrease)'),
                _('Large value → fast relaxation when OWD drops (0.9 = fast).'));
            o.datatype = 'float'; o.default = '0.9';

            o = s.taboption('advanced', form.Value, 'alpha_delta_ewma',
                _('Delta EWMA α'));
            o.datatype = 'float'; o.default = '0.095';

            o = s.taboption('advanced', form.Value, 'shaper_rate_min_adjust_down_bufferbloat',
                _('Min Rate Down Factor (bufferbloat)'),
                _('Minimum multiplier on bufferbloat (e.g. 0.99 = −1%).'));
            o.datatype = 'float'; o.default = '0.99';

            o = s.taboption('advanced', form.Value, 'shaper_rate_max_adjust_down_bufferbloat',
                _('Max Rate Down Factor (bufferbloat)'),
                _('Maximum multiplier on severe bufferbloat (e.g. 0.75 = −25%).'));
            o.datatype = 'float'; o.default = '0.75';

            o = s.taboption('advanced', form.Value, 'shaper_rate_min_adjust_up_load_high',
                _('Min Rate Up Factor (high load)'));
            o.datatype = 'float'; o.default = '1.0';

            o = s.taboption('advanced', form.Value, 'shaper_rate_max_adjust_up_load_high',
                _('Max Rate Up Factor (high load)'));
            o.datatype = 'float'; o.default = '1.04';

            o = s.taboption('advanced', form.Value, 'shaper_rate_adjust_down_load_low',
                _('Rate Down Factor (low load)'));
            o.datatype = 'float'; o.default = '0.99';

            o = s.taboption('advanced', form.Value, 'shaper_rate_adjust_up_load_low',
                _('Rate Up Factor (low load)'));
            o.datatype = 'float'; o.default = '1.01';

            o = s.taboption('advanced', form.Value, 'bufferbloat_detection_window',
                _('Bufferbloat Detection Window'),
                _('Number of consecutive ping samples examined for delay.'));
            o.datatype = 'uinteger'; o.default = '6';

            o = s.taboption('advanced', form.Value, 'bufferbloat_detection_thr',
                _('Bufferbloat Detection Threshold'),
                _('How many samples in the window must show delay before bufferbloat is declared.'));
            o.datatype = 'uinteger'; o.default = '3';

            o = s.taboption('advanced', form.Value, 'high_load_thr',
                _('High Load Threshold (fraction)'),
                _('Fraction of current shaper rate above which load is "high" (e.g. 0.75).'));
            o.datatype = 'float'; o.default = '0.75';

            o = s.taboption('advanced', form.Value, 'bufferbloat_refractory_period_ms',
                _('Bufferbloat Refractory Period (ms)'),
                _('Minimum time between consecutive rate-down adjustments.'));
            o.datatype = 'uinteger'; o.default = '300';

            o = s.taboption('advanced', form.Value, 'decay_refractory_period_ms',
                _('Decay Refractory Period (ms)'),
                _('Minimum time between idle/low-load rate adjustments.'));
            o.datatype = 'uinteger'; o.default = '1000';

            o = s.taboption('advanced', form.Flag, 'enable_sleep_function',
                _('Enable Sleep on Sustained Idle'));
            o.default = '1';

            o = s.taboption('advanced', form.Value, 'sustained_idle_sleep_thr_s',
                _('Idle Sleep Threshold (s)'));
            o.datatype = 'float'; o.default = '60.0';

            o = s.taboption('advanced', form.Flag, 'min_shaper_rates_enforcement',
                _('Enforce Min Rates on Idle / Stall'),
                _('When enabled, the shaper will not drop below the configured minimum rates.'));
            o.default = '0';

            o = s.taboption('advanced', form.Value, 'stall_detection_thr',
                _('Stall Detection Threshold (missed pings)'));
            o.datatype = 'uinteger'; o.default = '5';

            o = s.taboption('advanced', form.Value, 'connection_stall_thr_kbps',
                _('Connection Stall Rate Threshold (kbps)'),
                _('If both DL and UL are below this while pings time out, declare a stall.'));
            o.datatype = 'uinteger'; o.default = '10';

            o = s.taboption('advanced', form.Value, 'global_ping_response_timeout_s',
                _('Global Ping Response Timeout (s)'));
            o.datatype = 'float'; o.default = '10.0';

            o = s.taboption('advanced', form.Value, 'startup_wait_s',
                _('Startup Wait (s)'),
                _('Seconds to pause after start before adjusting rates.'));
            o.datatype = 'float'; o.default = '0.0';

            o = s.taboption('advanced', form.Value, 'monitor_achieved_rates_interval_ms',
                _('Rate Monitor Interval (ms)'),
                _('How often achieved DL/UL rates are sampled from sysfs.'));
            o.datatype = 'uinteger'; o.default = '200';

            o = s.taboption('advanced', form.Value, 'if_up_check_interval_s',
                _('Interface Up-Check Interval (s)'));
            o.datatype = 'float'; o.default = '10.0';

            /* ════════════════════════════════════════════════
             * Reflector Health tab
             * ════════════════════════════════════════════════ */
            o = s.taboption('health', form.Value, 'reflector_health_check_interval_s',
                _('Health Check Interval (s)'),
                _('How often each reflector is checked for missed responses.'));
            o.datatype = 'float'; o.default = '1';

            o = s.taboption('health', form.Value, 'reflector_response_deadline_s',
                _('Response Deadline (s)'),
                _('A reflector is counted as non-responsive if it has not replied within this time.'));
            o.datatype = 'float'; o.default = '1';

            o = s.taboption('health', form.Value, 'reflector_misbehaving_detection_window',
                _('Misbehaving Detection Window (checks)'),
                _('Sliding window size for counting missed-response events per reflector.'));
            o.datatype = 'uinteger'; o.default = '60';

            o = s.taboption('health', form.Value, 'reflector_misbehaving_detection_thr',
                _('Misbehaving Detection Threshold'),
                _('Missed responses within the window required to trigger automatic replacement.'));
            o.datatype = 'uinteger'; o.default = '3';

            o = s.taboption('health', form.Value, 'reflector_replacement_interval_s',
                _('Reflector Replacement Interval (s)'),
                _('Minimum time between automatic replacements. Default: 3600 (1 hour).'));
            o.datatype = 'uinteger'; o.default = '3600';

            o = s.taboption('health', form.Value, 'reflector_comparison_interval_s',
                _('Reflector Comparison Interval (s)'),
                _('How often active reflectors are compared against spare candidates.'));
            o.datatype = 'uinteger'; o.default = '60';

            o = s.taboption('health', form.Value, 'reflector_sum_owd_baselines_delta_thr_ms',
                _('OWD Baselines Sum Delta Threshold (ms)'),
                _('Replace a reflector if its OWD baseline sum exceeds a spare\'s by more than this.'));
            o.datatype = 'float'; o.default = '20.0';

            o = s.taboption('health', form.Value, 'reflector_owd_delta_ewma_delta_thr_ms',
                _('OWD EWMA Delta Threshold (ms)'),
                _('Replacement threshold based on the OWD-delta EWMA.'));
            o.datatype = 'float'; o.default = '10.0';
        });

        /*
         * Inject the Start/Stop/Restart buttons directly into the DOM above
         * the settings form.
         */
        return m.render().then(function(formNode) {
            var bar = E('div', { 'class': 'cbi-section' }, [
                E('h3', {}, _('Service Control')),
                E('div', { 'class': 'cbi-value' }, [
                    E('div', { 'class': 'cbi-value-title' }, ''),
                    E('div', {
                        'class': 'cbi-value-field',
                        'style': 'display:flex; gap:0.5em'
                    }, [
                        makeServiceButton(_('Start'),   'apply',  'start'),
                        makeServiceButton(_('Stop'),    'reset',  'stop'),
                        makeServiceButton(_('Restart'), 'reload', 'restart')
                    ])
                ])
            ]);

            return E('div', {}, [ bar, formNode ]);
        });
    }
});
