<!-- SPDX-License-Identifier: MIT -->
# Wire protocol v1

Default data transport: one UTF-8 JSON object per UDP datagram to `127.0.0.1:47520`.

Host control: low-rate JSON from the probe to `127.0.0.1:47521` (`session_hello` / `capture_mode`).

Common producer fields:

```json
{"v":1,"source":"hostd","type":"telemetry","seq":42,"mono_ns":123456789}
```

Additive keys are forward-compatible. Consumers ignore unknown keys. Breaking semantic changes require a new protocol version.

## `hostd / identity`

Includes observable Android/kernel/Kbase/device/devfreq identity plus session/game metadata echoed from probe control packets.

## `hostd / telemetry`

Representative fields:

- `gpu_freq_hz`, `gpu_min_freq_hz`, `gpu_max_freq_hz`;
- `gpu_busy_pct`;
- `gpu_temp_mC`, `soc_temp_mC`;
- `mem_total_kb`, `mem_available_kb`, `swap_free_kb`;
- `cpu_busy_pct`;
- `gpu_governor`.

Unavailable numeric values are JSON `null`.

## `vklayer / device`

- `gpu_name`;
- `vendor_id`, `device_id`;
- `api_version`, `api_version_string`;
- `driver_version`, `driver_version_string`.

## `vklayer / swapchain`

- `result`;
- width/height/min image count;
- format;
- present mode.

## `vklayer / frame`

- layer-local `frame` counter;
- `frame_ns` between observed presents;
- queue `submits` accumulated since previous present;
- `present_result`.

## `vklayer / memory`

- `allocated_bytes`: tracked live Vulkan device-memory allocation total;
- `peak_allocated_bytes`: peak observed allocation total.

This is Vulkan allocation accounting, not a claim that the number equals physical VRAM residency.

## `vklayer / device_lost`

Emitted when an intercepted queue submit, present or memory allocation returns `VK_ERROR_DEVICE_LOST`, with the operation name and raw `VkResult`.
