// SPDX-License-Identifier: MIT
#ifndef DGL_PROTOCOL_H
#define DGL_PROTOCOL_H

#include <stdint.h>

#define DGL_VERSION_STRING "0.1.0-dev.1"
#define DGL_PROTOCOL_VERSION 1
#define DGL_DATA_PORT 47520
#define DGL_CONTROL_PORT 47521
#define DGL_DEFAULT_UDP_PORT DGL_DATA_PORT
#define DGL_MAX_EVENT_BYTES 8192
#define DGL_RING_CAPACITY 8192
#define DGL_RING_LINE_BYTES 1536

/*
 * Drive GPU Lab protocol v1
 * -------------------------
 * Data producers send exactly one UTF-8 JSON object per UDP datagram to
 * 127.0.0.1:DGL_DATA_PORT. The probe is the only consumer.
 *
 * The host daemon additionally listens on DGL_CONTROL_PORT for low-rate
 * session-control JSON from the probe (session_hello / capture_mode).
 *
 * Common producer fields:
 *   v        protocol version
 *   source   hostd, vklayer, probe, dxvk, vkd3d, wine, box64, fex, ...
 *   type     telemetry, identity, frame, device, swapchain, error, ...
 *   seq      producer-local monotonic sequence
 *   mono_ns  producer-local monotonic timestamp when available
 *
 * The probe adds rx_qpc_ns when persisting every event. Cross-producer
 * ordering therefore uses receiver timestamps first and never wall time alone.
 */

typedef struct dgl_metrics {
    double fps;
    double frame_ms;
    double p95_ms;
    double p99_ms;
    double process_cpu_pct;
    double process_mem_mb;
    double gpu_busy_pct;
    uint64_t gpu_freq_hz;
    uint64_t gpu_min_freq_hz;
    uint64_t gpu_max_freq_hz;
    int64_t gpu_temp_mc;
    uint64_t mem_available_kb;
    uint64_t mem_total_kb;
    uint64_t swap_free_kb;
    uint64_t frames_seen;
    uint64_t submits_since_present;
    uint64_t vk_allocated_bytes;
    uint64_t vk_peak_allocated_bytes;
} dgl_metrics;

#endif
