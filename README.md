# cake-autorate-reborn

A lightweight, CPU-efficient C rewrite of [cake-autorate](https://github.com/lynxthecat/cake-autorate) for OpenWrt. Designed for routers where maximizing CPU efficiency is a priority.

## Background

The original [cake-autorate](https://github.com/lynxthecat/cake-autorate) created by [@lynxthecat](https://github.com/lynxthecat) is a highly effective bash script that dynamically adjusts CAKE bandwidth based on real-time One-Way Delay (OWD) measurements.

While the algorithm is excellent at mitigating bufferbloat, running a complex bash script that continuously spawns new processes and subshells can consume a significant amount of CPU on lower-end routers.

**cake-autorate-reborn** resolves this by reimplementing the exact same algorithm as a native C binary and OpenWrt procd service. It drastically reduces CPU overhead while maintaining identical adaptive traffic-shaping behavior.

*All credit for the original algorithm, math, and concept goes to [@lynxthecat](https://github.com/lynxthecat) and the contributors of the original repository.*

---

## Features

- **Native C Implementation:** No bash scripts, no subshells, and minimal CPU footprint.
- **Event-Driven Architecture:** Utilizes `libubox/uloop` to eliminate polling busy-loops.
- **Efficient System I/O:** Reads network statistics directly from `/sys/class/net/.../statistics` rather than invoking shell commands.
- **Direct Traffic Control:** Applies CAKE bandwidth changes directly via `tc qdisc change` without spawning an interactive shell.
- **Asynchronous Pinging:** Uses a background `fping` child process with non-blocking pipe I/O.
- **Dynamic Reflector Health:** Automatically monitors and replaces unresponsive ping targets.
- **Flash Storage Safe:** All state data is kept in RAM. Zero flash writes occur during runtime.
- **LuCI Web Interface:** Includes a fully integrated UI for configuring settings and controlling the service state.

---

## How It Works

The service continuously measures round-trip time via `fping`, utilizing `RTT/2` as a proxy for One-Way Delay (OWD). It maintains an asymmetric Exponentially Weighted Moving Average (EWMA) baseline for each reflector and classifies network load into four states:

| State | Condition | Action |
| :--- | :--- | :--- |
| **BUFFERBLOAT** | Delay spike detected above configured threshold | Reduces shaper rate aggressively |
| **HIGH** | Achieved rate > (high_load_thr × shaper rate) | Increases shaper rate |
| **LOW** | Achieved rate > connection_active_thr | Decays shaper rate toward base rate |
| **IDLE** | Minimal to no traffic detected | Decays shaper rate toward base rate |

---

## Installation

### Prerequisites
The service requires the following standard OpenWrt packages:

    apk update
    apk add fping libubox libuci

Installing from Release

Download the appropriate .apk file for your architecture from the Releases page.
Upload the file to your router and install:

    apk add --allow-untrusted cake-autorate-reborn_*.apk

Enable and start the service:

    /etc/init.d/cake-autorate-reborn enable
    /etc/init.d/cake-autorate-reborn start

Compiling from Source

If you are building your own OpenWrt firmware, you can compile it via the SDK:

    make package/cake-autorate-reborn/compile V=s

Configuration

The recommended way to configure the service is through the OpenWrt web interface by navigating to Services → CAKE Autorate Reborn. Clicking "Save & Apply" will automatically reload the daemon.

Alternatively, you can edit the configuration file via SSH:

    vi /etc/config/cake_autorate_reborn

Important: Ensure that the dl_if and ul_if options match your router's actual interfaces. For typical OpenWrt SQM setups, download is handled by an IFB interface (e.g., ifb4wan) and upload is handled by the physical WAN interface (e.g., wan).

    config cake_autorate_reborn 'primary'
        option enabled                  '1'
        option dl_if                    'ifb4wan'
        option ul_if                    'wan'
        option base_dl_shaper_rate_kbps '50000'
        option base_ul_shaper_rate_kbps '20000'
        option max_dl_shaper_rate_kbps  '100000'
        option max_ul_shaper_rate_kbps  '35000'

Verification and Logging

To confirm the service is registered and running under procd:

    ubus call service list '{"name":"cake-autorate-reborn"}'

To monitor the daemon adjusting bandwidth or responding to connection stalls in real-time:

    logread -f -e cake-autorate

Credits

Original Algorithm & Concept: @lynxthecat — cake-autorate

C Rewrite & OpenWrt Integration: kamikaonashi

License

This project is released under the MIT License.

The original cake-autorate project is licensed under its own terms. Please see the upstream repository for details.


Screenshots
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-41" src="https://github.com/user-attachments/assets/2dde3238-859f-4f90-86a1-b57384f253cf" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-46" src="https://github.com/user-attachments/assets/d9149822-3a21-4f3c-ab15-c09b0a83507b" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-54" src="https://github.com/user-attachments/assets/132e7d98-eaa3-4a2b-aaf8-70e37d1d5bab" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-26-59" src="https://github.com/user-attachments/assets/626c132d-e15a-48b2-850f-38aadf523412" />
<img width="3343" height="1318" alt="Screenshot From 2026-02-26 17-27-04" src="https://github.com/user-attachments/assets/59adcf63-c6e1-4805-975c-619120fd4bf1" />

