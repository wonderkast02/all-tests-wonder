<!-- SPDX-License-Identifier: MIT -->
# Qualification plan

A build is not called qualified merely because it compiles.

## CI gates

1. Linux host daemon and common library compile with warnings-as-errors.
2. Ring/common/SHA-256 unit tests pass.
3. `g720-hostd --once --stdout` emits valid JSON even when GPU sysfs is absent.
4. Android `arm64-v8a` host daemon cross-build succeeds using the pinned NDK.
5. Windows x64 `G720Probe.exe` and `G720VkLayer.dll` compile with MSVC warnings-as-errors.
6. Windows x86 probe/layer also compile; architecture-specific packages are separate.
7. Windows unit tests pass in both architectures where runnable by the hosted runner.
8. Every packaged artifact contains a SHA-256 manifest.

## G720 real-device gates

1. Probe starts in automatic mode with no game and remains stable.
2. One game creates one stable game root; second launch reuses that root and adds only another session.
3. `game.json`, `history.jsonl`, `latest.txt`, `manifest.json`, `modules.tsv`, `timeline.jsonl`, `MASTER.log`, raw streams and telemetry are internally consistent.
4. Android host packets reach the probe through loopback and host identity refreshes after session hello.
5. Vulkan layer loads under the exact Winlator Vulkan loader for the matching process architecture.
6. `device`, `swapchain`, `frame` and `memory` messages are valid JSON and appear in raw + unified logs.
7. A controlled device-lost/error path, when reproducible, creates an anomaly without killing the probe.
8. F9 creates a marker + ring context; F11 additionally creates a valid screenshot.
9. External DXVK/VKD3D/Wine-style logs are tailed from the current EOF and do not import stale lines.
10. Game exit finalizes summary/manifest; probe remains alive in stay-alive/automatic mode.
11. A 30-minute normal capture has no unexplained receive/drop increments.
12. A/B measurements quantify overhead with layer disabled vs enabled before broad community rollout.

## Performance rules

- normal-capture and deep-debug runs are never mixed in the same performance baseline;
- compare repeated runs, not one lucky sample;
- report frametime-tail metrics together with average FPS;
- a metric marked unavailable must not be coerced to zero during analysis.

## Release rule

CI-green is necessary but insufficient. Public/community release status is assigned only after real-device qualification and an explicit project release decision.
