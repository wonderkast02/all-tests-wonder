<!-- SPDX-License-Identifier: MIT -->
# Winlator installation and capture

This document describes the expected `0.1.0-dev.1` laboratory setup. A Winlator build is considered supported only after the on-device qualification gates pass.

## 1. Windows package

Keep these files together inside a directory visible to the Wine container:

```text
G720Probe.exe
G720VkLayer.dll
VK_LAYER_DRIVE_G720_telemetry.json
config/
```

For 64-bit games use the x64 artifact. For a 32-bit Vulkan process use the x86 artifact. A 64-bit layer DLL cannot be loaded into a 32-bit process and vice versa.

## 2. Normal container environment

Set the layer directory and telemetry port:

```text
VK_LAYER_PATH=<directory containing G720VkLayer.dll and the JSON manifest>
VK_INSTANCE_LAYERS=VK_LAYER_DRIVE_G720_telemetry
DRIVE_G720_PROBE_PORT=47520
DXVK_LOG_LEVEL=info
VKD3D_DEBUG=warn
```

Do not enable maximum tracing for performance comparisons.

## 3. Android host daemon

Use the prebuilt `drive-gpu-lab-android-arm64` artifact in Termux:

```sh
chmod 700 g720-hostd
./g720-hostd
```

It sends telemetry to `127.0.0.1:47520` and listens for low-rate session metadata on `127.0.0.1:47521`.

No root requirement is imposed by the program. Android/SELinux may still hide specific Kbase/devfreq/thermal nodes; those values remain `null`.

## 4. Start the probe

The simplest mode is:

```text
G720Probe.exe
```

With no arguments it stays resident, waits for a visible game process, creates/reuses that game's permanent folder, starts a new session under `sessions/`, captures until that process exits, then waits for the next game.

Explicit modes:

```text
G720Probe.exe --process GAME.exe --stay-alive
G720Probe.exe --pid 12345 --once
G720Probe.exe --root Z:\DriveGpuLab
```

## 5. During the bug

- `F8`: hide/show HUD.
- `F9`: create a bug marker and persist the current ring context.
- `F10`: toggle deep-capture intent.
- `F11`: marker + screenshot.

Use `F9` as close as possible to the visible failure. The marker carries the same session timeline and receive-clock domain as the rest of the capture.

## 6. After the game exits

Open:

```text
DriveGpuLab\games\<game-key>\latest.txt
```

It points to the most recent session for that game. Preserve the entire session directory when reporting a bug; do not send only `MASTER.log` when deeper analysis may be required.

## Deep debug profile

`config/winlator-env-deep-debug.txt` is intentionally separate. Heavy DXVK/VKD3D/Wine/Mesa/PanVK logging can materially change frametime, scheduling and I/O behavior. A deep-debug result is diagnostic evidence, not a clean benchmark.
