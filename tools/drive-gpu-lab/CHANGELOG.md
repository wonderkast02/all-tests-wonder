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
### Qualification hardening

Before the first public dev.1 prerelease, CI qualification was hardened to:

- preserve `/W4 /WX` on MSVC while explicitly allowing only portable ISO C CRT interfaces;
- avoid Vulkan loader symbol-linkage collisions and export stable custom layer entry points;
- provide architecture-specific `.def` aliases so Win32 and Win64 expose the exact manifest names;
- validate Vulkan layer manifest JSON and exported PE symbols in CI;
- pin hosted runner families and Node 24-based GitHub Actions by immutable commit SHA;
- use the runner-provided Android SDK directly and install the pinned NDK without the deprecated `tools` package path;
- harden Win32 size conversions under `/W4 /WX` and make per-run session IDs collision-resistant while keeping the per-game root stable.
- correct Tool Help process/module enumeration to the SDK's unsuffixed ANSI entry points used by `PROCESSENTRY32` and `MODULEENTRY32`.
- resolve MSVC `dumpbin.exe` through Visual Studio `vswhere` in Windows CI instead of assuming it is present in the default PowerShell `PATH`.
- redesenha o HUD Win32 em cartões compactos semitransparentes, com hierarquia visual, cores por métrica e fundo click-through para preservar a visibilidade do jogo.
- endurece a descoberta automática: exige evidência da Vulkan layer por PID antes do attach e ignora o Wine File Manager, evitando capturas acidentais de `wfm.exe`.
- mantém `VK MEM` como indisponível até existir telemetria de memória Vulkan real, sem inferir validade a partir do evento de criação do dispositivo.
