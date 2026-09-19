<!-- SPDX-License-Identifier: MIT -->
# Session schema

## Stable game identity

A sanitized game key is derived from executable ProductName plus executable name where available. Product/version changes do not themselves create a new session root. Every run gets a new session directory while the same game root is reused.

`game.json` records the current known stable identity. `history.jsonl` is append-only session history. `latest.txt` points to the most recent session.

## `manifest.json`

The manifest is the session inventory and contains:

- Drive GPU Lab version and capture mode;
- session ID, game key, PID and exact executable path;
- target SHA-256;
- Wine/Proton/translator/graphics-stack detection;
- DXVK/VKD3D/Mesa/PanVK identity when observed;
- Vulkan GPU/API/driver values from the layer;
- final counters including frames, UDP receive/drop errors, external lines, markers, anomalies, device-lost events and ring overwrites.

A blank/unknown field means no trustworthy observation was available.

## `timeline.jsonl`

One JSON object per line. It is the primary machine-readable chronological stream. The probe adds a local receive timestamp to producer events so cross-source ordering does not depend on pretending Android and guest clocks share an epoch.

## `MASTER.log`

Human-readable merged timeline. It is derived evidence, not a replacement for producer-specific logs.

## `modules.tsv`

Inventory of graphics-related target modules, including path, SHA-256 and discovered embedded identity strings. This is used to distinguish builds that share the same marketing/version label.

## `performance/telemetry.csv`

Periodic process/host/frame summary samples. Current fields include CPU, working set, FPS/frametime percentiles, GPU utilization/clock/thermal, host memory and Vulkan allocation totals.

## `raw/`

Raw streams and external log tails. The probe starts newly discovered pre-existing logs at their current EOF so stale sessions are not silently concatenated into a new capture.
