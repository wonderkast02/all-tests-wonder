<!-- SPDX-License-Identifier: MIT -->
# Architecture

```text
 Windows guest / Wine                                 Android host

 game.exe
   |
 DXVK / VKD3D-Proton / native Vulkan
   |
 G720VkLayer.dll  --------------------\
   |                                  \
   | UDP :47520                        >  G720Probe.exe
   |                                  /      |
 external text logs -----------------/       +-- stable per-game session store
                                                |-- timeline.jsonl
 g720-hostd -------------------------/          |-- MASTER.log
   ^                                            |-- raw/
   |                                            `-- telemetry.csv
 /proc + /sys + Kbase/devfreq/thermal
```

## Separation of responsibilities

`G720Probe.exe` owns session lifecycle and files. The game never owns the session directory, so a game crash cannot erase the collector's already-written evidence.

`G720VkLayer.dll` is deliberately narrow. It instruments Vulkan facts that are difficult to recover correctly from an external process: physical-device identity, swapchain creation, queue submissions, present timing/results and Vulkan device-memory allocation/free totals.

`g720-hostd` is the only component allowed to interpret Android/Linux host telemetry. It emits `null` for inaccessible metrics.

## Clock model

Producers may expose their own monotonic time, but producer epochs are not assumed equal. Every received event receives the probe's local `QueryPerformanceCounter`-derived nanosecond timestamp. Cross-source ordering uses that receive clock, with transport latency treated as measurement uncertainty.

## Normal hot path

- queue submissions increment an atomic counter only;
- present emits one compact JSON datagram with frame interval and accumulated submits;
- allocation/free emits current + peak Vulkan allocation totals;
- host and process sampling runs at a low rate;
- external logs are tailed asynchronously by the probe.

This is instrumentation, not full Vulkan API tracing.

## Ring buffer

The human-event ring is heap allocated so the probe does not consume a multi-megabyte thread stack. It stores the newest `DGL_RING_CAPACITY` events. Markers persist a snapshot; overwrites are counted in the final manifest/summary.

## Per-game storage

Only session directories are unique per run. The game root is deterministic, so repeated runs of the same executable identity accumulate under one folder instead of polluting the lab root with near-duplicates.
