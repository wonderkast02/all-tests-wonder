<!-- SPDX-License-Identifier: MIT -->
# Changelog

## 0.1.0-dev.1 — 2026-09-19

Initial Drive GPU Lab developer instrumentation candidate:

- persistent Win32/Wine supervisor with automatic game-process discovery;
- deterministic per-game roots with append-only per-run session directories;
- HUD with FPS/frametime tails, CPU/RAM, GPU host telemetry and Vulkan allocation totals;
- F8/F9/F10/F11 HUD/marker/deep-intent/screenshot workflow;
- exact target executable SHA-256 and relevant graphics-module hashes;
- automatic Wine/Proton/Box64/FEX/DXVK/VKD3D/Mesa/PanVK evidence capture when observable;
- external DXVK/VKD3D/Wine/Box64/FEX/Proton/Vulkan/PanVK log tail ingestion;
- merged `MASTER.log` plus machine `timeline.jsonl` and preserved raw sources;
- explicit Vulkan layer for device/swapchain/present/submission/memory/device-lost telemetry;
- Android/Termux host daemon for devfreq, utilization, thermal, memory, CPU and Kbase/device identity;
- loopback protocol v1 with separate data/control ports;
- heap-backed event ring with observable overwrite counters;
- CI packaging plan for Linux x86-64, Android arm64-v8a, Windows x64 and Windows x86;
- qualification, schema, detection, installation and privacy documentation.
