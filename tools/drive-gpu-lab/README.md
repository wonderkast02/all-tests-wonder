<!-- SPDX-License-Identifier: MIT -->
# Drive GPU Lab — Winlator / PanVK Telemetry

**Drive GPU Lab** is the diagnostic, telemetry and regression toolkit for the Drive G720 / PanVK workstream. It is designed to keep evidence from the whole graphics path synchronized enough to answer practical questions such as:

- did a stutter begin in the game, DXVK/VKD3D-Proton, Vulkan/PanVK, the host GPU or thermal/memory pressure?
- which exact Wine/Proton/translator/wrapper/driver binaries were active?
- did a candidate driver improve average performance while worsening tail frametimes?
- was a crash preceded by `VK_ERROR_DEVICE_LOST`, memory pressure, a GPU frequency drop or an external log error?

> **Status:** `0.1.0-dev.1`. This is developer instrumentation, not a Vulkan-conformance or universal-compatibility claim. CI qualification and real-device qualification are separate gates.

## Components

| Component | Runs in | Role |
|---|---|---|
| `G720Probe.exe` | Winlator/Wine guest | Background supervisor, per-game sessions, HUD, process telemetry, stack/module identification, log ingestion, markers, screenshots, summary |
| `G720VkLayer.dll` | Game Vulkan process | Present/frametime, queue submits, swapchain/device metadata, Vulkan allocation accounting, device-lost events |
| `g720-hostd` | Android/Termux host | GPU devfreq/load/clock/thermal, host memory/CPU, Kbase/device identity when accessible |

All normal transport is loopback UDP. No component uploads data automatically.

## One permanent folder per game

The probe does **not** create a new top-level game folder every launch. It derives a stable key from executable metadata and the executable name, reuses that game directory, and creates only a new session beneath it.

```text
DriveGpuLab/
└── games/
    └── grand-theft-auto-v/
        ├── game.json
        ├── history.jsonl
        ├── latest.txt
        └── sessions/
            ├── 20260919-135205-p18432/
            ├── 20260919-171144-p22108/
            └── 20260920-003107-p11704/
```

A session contains:

```text
<session>/
├── manifest.json               # exact game/runtime/driver identity
├── environment.json            # relevant DXVK/VKD3D/Wine/Box64/FEX/Proton/Mesa/etc env
├── modules.tsv                 # graphics DLL/module path + SHA-256 + embedded identity
├── summary.txt                 # human summary
├── MASTER.log                  # merged chronological human log
├── timeline.jsonl              # machine-readable source of truth
├── raw/
│   ├── hostd.jsonl
│   ├── vklayer.jsonl
│   └── external-*.log          # DXVK/VKD3D/Wine/Box64/FEX/Proton/etc tails
├── performance/
│   └── telemetry.csv
├── events/
│   ├── markers.jsonl
│   ├── anomalies.jsonl
│   └── marker-*-context.log
└── screenshots/
    └── marker-*.bmp
```

`timeline.jsonl` is the structured evidence stream. `MASTER.log` is the easy-to-read merged view. Producer logs remain separate under `raw/` for provenance.

## Automatic runtime identification

The probe records what it can prove and leaves unavailable facts unknown instead of guessing. Detection includes:

- target executable ProductName/ProductVersion/FileVersion and SHA-256;
- Wine runtime version through Wine's exported runtime API when available;
- Proton indicators/version hints plus all relevant `PROTON_*` / `STEAM_COMPAT_*` environment values;
- Box64 and FEX indicators/version hints plus all relevant environment configuration;
- explicit wrapper identity when Winlator/runtime metadata exposes it, otherwise wrapper classification from loaded DXVK/VKD3D-Proton modules;
- loaded DXVK / VKD3D-Proton / Vulkan / winevulkan / Mesa / PanVK / Zink-related modules;
- SHA-256 for relevant loaded binaries;
- embedded `DXVK`, `vkd3d-proton`, `Mesa` and `PanVK` identity strings when discoverable;
- Vulkan GPU name, vendor/device ID and API/driver version from the telemetry layer;
- Android model/release/build fingerprint, kernel/Kbase information and `/dev/mali0` accessibility from the host daemon;
- GPU devfreq path, governor, available/min/max/current frequencies, utilization and thermal values when the Android security context exposes them.

The exact relevant environment snapshot includes prefixes such as `DXVK_`, `VKD3D_`, `WINE*`, `BOX64_`, `FEX_`, `PROTON_`, `STEAM_COMPAT_`, `VK_`, `MESA_`, `PAN_`, `GALLIUM_`, `ZINK_` and `WINLATOR_`.

## HUD

The transparent topmost HUD shows available live data, including:

- game / PID;
- GPU and Vulkan driver/API;
- detected graphics stack and translator;
- FPS, average FPS, 1% low estimate and 0.1% low estimate;
- current/P95/P99/P99.9 frametime;
- process CPU and RAM;
- host GPU utilization, frequency and temperature;
- Vulkan allocated/peak device memory observed through `vkAllocateMemory`;
- frames/submissions, events and capture status.

Hotkeys:

- `F8` — show/hide HUD;
- `F9` — mark a bug and persist the current ring context;
- `F10` — toggle deep-capture intent and notify the host side;
- `F11` — mark a bug + save a screenshot.

## Automatic external log ingestion

During an active session the probe looks for relevant `.log` sources in the game/current/log directories and known paths such as `DXVK_LOG_PATH`, `PROTON_LOG_DIR` and `VKD3D_LOG_FILE`. Recognized sources include DXVK, VKD3D, Wine, Box64, FEX, Proton, Vulkan, Mesa and PanVK.

When attaching to a pre-existing log it starts at that file's current end, so an old run is not silently mixed into the new session.

## Normal Winlator use

For the exact installation steps see [`docs/INSTALL-WINLATOR.md`](docs/INSTALL-WINLATOR.md). The intended normal path is:

1. place the prebuilt Windows package where Winlator can access it;
2. expose the Vulkan layer directory with `VK_LAYER_PATH` and enable `VK_LAYER_DRIVE_G720_telemetry`;
3. run the prebuilt Android arm64 `g720-hostd` in Termux;
4. start `G720Probe.exe` with no arguments — it stays resident and automatically selects the largest visible game process;
5. launch/play the game normally;
6. press `F9`/`F11` when a bug is visible;
7. collect that game's session directory.

You can also target a process explicitly:

```text
G720Probe.exe --process GTA5.exe --stay-alive
G720Probe.exe --pid 18432 --once
G720Probe.exe --root Z:\DriveGpuLab
```

`--launch GAME.exe [arguments...]` can launch a target with light DXVK/VKD3D log routing prepared by the probe. Put `--launch` last because the remaining arguments are passed to the target.

## Termux is intentionally minimal

Normal users do **not** compile on the phone. GitHub Actions builds the Android `arm64-v8a` daemon. After extracting the Android artifact, Termux only needs:

```sh
chmod 700 g720-hostd
./g720-hostd
```

Optional diagnostic mirror:

```sh
./g720-hostd --stdout
```

If the device/SELinux policy hides a sysfs metric, `hostd` emits `null`; it never substitutes a made-up value.

## Build and CI

Local Linux validation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDGL_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/g720-hostd --once --stdout
```

Repository CI builds and packages:

- Linux x86-64 host daemon + tests;
- Android arm64-v8a host daemon with a pinned NDK;
- Windows x64 probe + Vulkan layer + tests;
- Windows x86 probe + Vulkan layer + tests;
- SHA-256 manifests for release artifacts.

`Vulkan-Headers` is pinned to immutable commit `e3b1eec08173d6b825cd3ac88c885a63b621504a` (the commit behind `v1.4.357`) for deterministic layer-interface headers.

## Measurement rules

- **Receiver-monotonic ordering:** producer clocks are not falsely assumed to share an epoch. The probe adds its own receive timestamp to every event.
- **Raw + normalized:** evidence is never discarded just because it was normalized into `MASTER.log`.
- **Unknown means unknown:** inaccessible telemetry remains unavailable, not zero or inferred.
- **Low overhead by default:** submit calls are counted atomically and emitted with the next present; host/process sampling is low-rate.
- **Independent supervisor:** game termination cannot erase the session already owned by the external probe.
- **Hashes matter:** relevant binaries and the game executable are SHA-256 identified to prevent ambiguous “same version” reports.
- **Performance and deep debug are different experiments:** heavy logging is never considered directly comparable to a normal benchmark.

## Qualification

Compilation is only CI qualification. Real Winlator/PanVK readiness requires the on-device gates in [`docs/TESTPLAN.md`](docs/TESTPLAN.md), including layer discovery, loopback transport, event validity, clean shutdown, marker/screenshot behavior and measured capture overhead.

## License

Files authored specifically for Drive GPU Lab in this directory use the MIT license and carry SPDX identifiers where applicable. They do **not** relicense Mesa/PanVK or any other component in the parent repository. Parent-repository licensing remains governed per file/component by the repository's `LICENSING.md`.
