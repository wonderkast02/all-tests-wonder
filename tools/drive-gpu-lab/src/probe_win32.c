// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <winver.h>

#include "common.h"
#include "dgl_protocol.h"
#include "dgl_ring.h"
#include "sha256.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")

#define DGL_HUD_CLASS "DriveGpuLabHud"
#define DGL_HOTKEY_TOGGLE 1001
#define DGL_HOTKEY_MARK   1002
#define DGL_HOTKEY_DEEP   1003
#define DGL_HOTKEY_SHOT   1004
#define DGL_FRAME_SAMPLES 8192
#define DGL_PATH_CAP 4096
#define DGL_MAX_TAILS 48
#define DGL_TAIL_TAG_CAP 32
#define DGL_TAIL_LINE_CAP 8192
#define DGL_TEXT_CAP 512
#define DGL_GAME_KEY_CAP 192
#define DGL_PROCESS_SCAN_NS 500000000ull
#define DGL_MODULE_SCAN_NS 10000000000ull
#define DGL_TAIL_SCAN_NS 1000000000ull
#define DGL_SAMPLE_NS 500000000ull

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

typedef struct frame_stats {
    double ms[DGL_FRAME_SAMPLES];
    size_t head;
    size_t count;
    uint64_t last_qpc_ns;
    uint64_t first_qpc_ns;
    uint64_t frames;
    double current_ms;
    double fps_window;
    double avg_fps;
    double p95_ms;
    double p99_ms;
    double p999_ms;
} frame_stats;

typedef struct log_tail {
    char tag[DGL_TAIL_TAG_CAP];
    char path[DGL_PATH_CAP];
    char raw_path[DGL_PATH_CAP];
    FILE *src;
    FILE *raw;
    int64_t position;
    uint64_t lines;
    bool announced;
} log_tail;

typedef struct file_identity {
    char product_name[DGL_TEXT_CAP];
    char product_version[DGL_TEXT_CAP];
    char file_description[DGL_TEXT_CAP];
    char file_version[DGL_TEXT_CAP];
} file_identity;

typedef struct probe_state {
    /* CLI / lifetime. */
    char requested_process[MAX_PATH];
    DWORD requested_pid;
    bool auto_mode;
    bool stay_alive;
    bool one_session_only;
    bool launch_mode;
    char launch_exe[DGL_PATH_CAP];
    char launch_cmdline[DGL_PATH_CAP * 2];
    char lab_root[DGL_PATH_CAP];
    unsigned data_port;
    unsigned control_port;

    /* Target. */
    DWORD pid;
    HANDLE process;
    bool target_ever_attached;
    uint64_t target_exit_qpc_ns;
    char process_name[MAX_PATH];
    char process_path[DGL_PATH_CAP];
    file_identity game_identity;
    char game_key[DGL_GAME_KEY_CAP];
    char exe_sha256[65];

    /* Stable game tree + one session. */
    bool session_open;
    char games_root[DGL_PATH_CAP];
    char game_dir[DGL_PATH_CAP];
    char sessions_dir[DGL_PATH_CAP];
    char session_id[128];
    char session_dir[DGL_PATH_CAP];
    char raw_dir[DGL_PATH_CAP];
    char events_dir[DGL_PATH_CAP];
    char perf_dir[DGL_PATH_CAP];
    char screenshot_dir[DGL_PATH_CAP];
    FILE *master;
    FILE *timeline;
    FILE *telemetry;
    FILE *markers;
    FILE *anomalies;
    FILE *host_raw;
    FILE *vk_raw;

    /* Networking / HUD. */
    SOCKET udp;
    struct sockaddr_in host_control_addr;
    HWND hud;
    HFONT hud_font;
    bool hud_visible;
    bool deep_mode;

    /* Time / sampling. */
    uint64_t process_start_qpc_ns;
    uint64_t session_start_qpc_ns;
    uint64_t last_sample_qpc_ns;
    uint64_t last_process_scan_qpc_ns;
    uint64_t last_module_scan_qpc_ns;
    uint64_t last_tail_scan_qpc_ns;
    uint64_t last_tail_poll_qpc_ns;
    uint64_t prev_proc_100ns;
    uint64_t prev_cpu_sample_qpc_ns;
    unsigned cpu_count;

    /* Counters. */
    uint64_t event_seq;
    uint64_t udp_events;
    uint64_t udp_dropped;
    uint64_t markers_count;
    uint64_t anomalies_count;
    uint64_t device_lost_count;
    uint64_t external_lines;
    uint64_t last_anomaly_ns;
    uint64_t last_frame_rx_ns;
    uint64_t sessions_completed;

    /* Logs / metrics. */
    log_tail tails[DGL_MAX_TAILS];
    size_t tail_count;
    dgl_metrics m;
    frame_stats frames;
    dgl_ring ring;

    /* Runtime identification. Never invent unavailable values. */
    char gpu_name[DGL_TEXT_CAP];
    char driver_name[DGL_TEXT_CAP];
    char driver_info[DGL_TEXT_CAP];
    char vulkan_api[DGL_TEXT_CAP];
    char vulkan_driver_version[DGL_TEXT_CAP];
    char wine_version[DGL_TEXT_CAP];
    char proton_version[DGL_TEXT_CAP];
    char translator[DGL_TEXT_CAP];
    char wrapper_identity[DGL_TEXT_CAP];
    char graphics_stack[DGL_TEXT_CAP];
    char dxvk_identity[DGL_TEXT_CAP];
    char vkd3d_identity[DGL_TEXT_CAP];
    char mesa_identity[DGL_TEXT_CAP];
    char panvk_identity[DGL_TEXT_CAP];
    char host_identity[DGL_TEXT_CAP];
    char host_kbase[DGL_TEXT_CAP];
    char host_android[DGL_TEXT_CAP];
    char host_governor[DGL_TEXT_CAP];
} probe_state;

static probe_state *g_state = NULL;

static uint64_t filetime_u64(FILETIME ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static uint64_t qpc_ns(void) {
    return dgl_monotonic_ns();
}

static void utc_stamp(char *out, size_t cap) {
    char tmp[32];
    dgl_iso8601_utc(tmp);
    snprintf(out, cap, "%s", tmp);
}

static void session_stamp(char *out, size_t cap) {
    char tmp[32];
    dgl_compact_local_stamp(tmp);
    snprintf(out, cap, "%s", tmp);
}

static int mkdir_tree(const char *path) {
    return dgl_mkdir_p(path);
}

static void path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t n = a ? strlen(a) : 0;
    snprintf(out, cap, "%s%s%s", a ? a : "",
             (n && a[n - 1] != '\\' && a[n - 1] != '/') ? "\\" : "",
             b ? b : "");
}

static const char *base_name(const char *path) {
    const char *a, *b, *q;
    if (!path) return "";
    a = strrchr(path, '\\');
    b = strrchr(path, '/');
    q = a ? (b && b > a ? b : a) : b;
    return q ? q + 1 : path;
}

static void dirname_copy(const char *path, char *out, size_t cap) {
    char *a, *b, *q;
    snprintf(out, cap, "%s", path ? path : "");
    a = strrchr(out, '\\');
    b = strrchr(out, '/');
    q = a ? (b && b > a ? b : a) : b;
    if (q) *q = '\0';
    else snprintf(out, cap, ".");
}

static int file_exists(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int64_t file_size_or_zero(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER u;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    return (int64_t)u.QuadPart;
}

static void json_write_string(FILE *f, const char *key, const char *value, bool comma) {
    char escaped[DGL_PATH_CAP * 2];
    dgl_json_escape(value ? value : "", escaped, sizeof(escaped));
    fprintf(f, "  \"%s\": \"%s\"%s\n", key, escaped, comma ? "," : "");
}

static void master_log(probe_state *s, const char *tag, const char *message) {
    char utc[64];
    uint64_t now = qpc_ns();
    char ringline[DGL_RING_LINE_BYTES];
    if (!s || !s->session_open || !message) return;
    utc_stamp(utc, sizeof(utc));
    if (s->master) {
        fprintf(s->master, "%s rx_qpc_ns=%llu +%.3fms [%s] %s\n", utc,
                (unsigned long long)now,
                (double)(now - s->session_start_qpc_ns) / 1000000.0,
                tag ? tag : "PROBE", message);
        fflush(s->master);
    }
    snprintf(ringline, sizeof(ringline), "+%.3fms [%s] %s",
             (double)(now - s->session_start_qpc_ns) / 1000000.0,
             tag ? tag : "PROBE", message);
    dgl_ring_push(&s->ring, ++s->event_seq, ringline);
}

static void timeline_raw_event(probe_state *s, const char *raw_json) {
    uint64_t now = qpc_ns();
    if (!s || !s->timeline || !raw_json) return;
    if (raw_json[0] == '{') {
        fprintf(s->timeline, "{\"rx_qpc_ns\":%llu,\"event\":%s}\n",
                (unsigned long long)now, raw_json);
    } else {
        fprintf(s->timeline,
                "{\"rx_qpc_ns\":%llu,\"event\":{\"source\":\"unknown\",\"raw\":\"invalid-json\"}}\n",
                (unsigned long long)now);
    }
    fflush(s->timeline);
}

static void timeline_text_event(probe_state *s, const char *source, const char *text) {
    char escaped[DGL_TAIL_LINE_CAP * 2];
    uint64_t now = qpc_ns();
    if (!s || !s->timeline || !text) return;
    dgl_json_escape(text, escaped, sizeof(escaped));
    fprintf(s->timeline,
            "{\"rx_qpc_ns\":%llu,\"event\":{\"v\":%d,\"source\":\"%s\","
            "\"type\":\"text_log\",\"text\":\"%s\"}}\n",
            (unsigned long long)now, DGL_PROTOCOL_VERSION,
            source ? source : "external", escaped);
    fflush(s->timeline);
}

static const char *json_find_value(const char *json, const char *key) {
    static char needle[128];
    const char *p;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(json, needle);
    return p ? p + strlen(needle) : NULL;
}

static double json_double(const char *json, const char *key, double fallback) {
    const char *p = json_find_value(json, key);
    char *end = NULL;
    double v;
    if (!p || !strncmp(p, "null", 4)) return fallback;
    v = strtod(p, &end);
    return end == p ? fallback : v;
}

static uint64_t json_u64(const char *json, const char *key, uint64_t fallback) {
    const char *p = json_find_value(json, key);
    char *end = NULL;
    unsigned long long v;
    if (!p || !strncmp(p, "null", 4)) return fallback;
    v = strtoull(p, &end, 10);
    return end == p ? fallback : (uint64_t)v;
}

static int64_t json_i64(const char *json, const char *key, int64_t fallback) {
    const char *p = json_find_value(json, key);
    char *end = NULL;
    long long v;
    if (!p || !strncmp(p, "null", 4)) return fallback;
    v = strtoll(p, &end, 10);
    return end == p ? fallback : (int64_t)v;
}

static void json_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json_find_value(json, key);
    size_t n = 0;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!p) return;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p++ != '"') return;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case 'n': out[n++] = '\n'; ++p; continue;
                case 'r': out[n++] = '\r'; ++p; continue;
                case 't': out[n++] = '\t'; ++p; continue;
                default: break;
            }
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
}

static int dblcmp(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void frame_recalc(frame_stats *f, uint64_t now_ns) {
    double tmp[DGL_FRAME_SAMPLES];
    size_t i, n = f->count;
    if (!n) return;
    for (i = 0; i < n; ++i) tmp[i] = f->ms[i];
    qsort(tmp, n, sizeof(double), dblcmp);
    f->p95_ms = tmp[(size_t)floor((double)(n - 1) * 0.95)];
    f->p99_ms = tmp[(size_t)floor((double)(n - 1) * 0.99)];
    f->p999_ms = tmp[(size_t)floor((double)(n - 1) * 0.999)];
    if (f->first_qpc_ns && now_ns > f->first_qpc_ns)
        f->avg_fps = (double)f->frames * 1e9 / (double)(now_ns - f->first_qpc_ns);
    {
        size_t samples = 0;
        double total = 0.0;
        for (i = 0; i < n; ++i) {
            size_t slot = (f->head + DGL_FRAME_SAMPLES - 1 - i) % DGL_FRAME_SAMPLES;
            total += f->ms[slot];
            ++samples;
            if (total >= 1000.0) break;
        }
        if (total > 0.0) f->fps_window = 1000.0 * (double)samples / total;
    }
}

static void frame_push(probe_state *s, uint64_t now_ns, uint64_t source_frame_ns, uint64_t submits) {
    frame_stats *f = &s->frames;
    double ms = 0.0;
    if (!f->first_qpc_ns) f->first_qpc_ns = now_ns;
    if (source_frame_ns > 0 && source_frame_ns < 10000000000ull)
        ms = (double)source_frame_ns / 1000000.0;
    else if (f->last_qpc_ns && now_ns > f->last_qpc_ns)
        ms = (double)(now_ns - f->last_qpc_ns) / 1000000.0;
    if (ms > 0.0) {
        f->current_ms = ms;
        f->ms[f->head] = ms;
        f->head = (f->head + 1) % DGL_FRAME_SAMPLES;
        if (f->count < DGL_FRAME_SAMPLES) ++f->count;
        if (ms > 50.0 && now_ns - s->last_anomaly_ns > 1000000000ull) {
            char line[256];
            snprintf(line, sizeof(line), "frametime_spike frame_ms=%.3f submits=%llu",
                     ms, (unsigned long long)submits);
            master_log(s, "ANOMALY", line);
            if (s->anomalies) {
                fprintf(s->anomalies,
                        "{\"qpc_ns\":%llu,\"type\":\"frametime_spike\","
                        "\"frame_ms\":%.3f,\"submits\":%llu}\n",
                        (unsigned long long)now_ns, ms, (unsigned long long)submits);
                fflush(s->anomalies);
            }
            ++s->anomalies_count;
            s->last_anomaly_ns = now_ns;
        }
    }
    f->last_qpc_ns = now_ns;
    ++f->frames;
    s->last_frame_rx_ns = now_ns;
    s->m.frames_seen = f->frames;
    s->m.submits_since_present = submits;
    if ((f->frames & 31u) == 0) frame_recalc(f, now_ns);
    s->m.fps = f->fps_window;
    s->m.frame_ms = f->current_ms;
    s->m.p95_ms = f->p95_ms;
    s->m.p99_ms = f->p99_ms;
}

static int read_version_string(const char *path, const char *field, char *out, size_t cap) {
    DWORD dummy = 0;
    DWORD size;
    BYTE *data;
    struct lang_cp { WORD language; WORD codepage; } *translate = NULL;
    UINT translate_len = 0;
    char query[128];
    char *value = NULL;
    UINT value_len = 0;
    int ok = 0;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    size = GetFileVersionInfoSizeA(path, &dummy);
    if (!size) return 0;
    data = (BYTE *)malloc(size);
    if (!data) return 0;
    if (!GetFileVersionInfoA(path, 0, size, data)) goto done;
    if (!VerQueryValueA(data, "\\VarFileInfo\\Translation", (LPVOID *)&translate, &translate_len) ||
        translate_len < sizeof(*translate)) {
        snprintf(query, sizeof(query), "\\StringFileInfo\\040904E4\\%s", field);
    } else {
        snprintf(query, sizeof(query), "\\StringFileInfo\\%04x%04x\\%s",
                 translate[0].language, translate[0].codepage, field);
    }
    if (VerQueryValueA(data, query, (LPVOID *)&value, &value_len) && value && value_len) {
        snprintf(out, cap, "%s", value);
        ok = 1;
    }
done:
    free(data);
    return ok;
}

static void read_file_identity(const char *path, file_identity *id) {
    memset(id, 0, sizeof(*id));
    (void)read_version_string(path, "ProductName", id->product_name, sizeof(id->product_name));
    (void)read_version_string(path, "ProductVersion", id->product_version, sizeof(id->product_version));
    (void)read_version_string(path, "FileDescription", id->file_description, sizeof(id->file_description));
    (void)read_version_string(path, "FileVersion", id->file_version, sizeof(id->file_version));
}

static void derive_game_key(probe_state *s) {
    char product[DGL_GAME_KEY_CAP];
    char exe[DGL_GAME_KEY_CAP];
    dgl_sanitize_key(s->game_identity.product_name, product, sizeof(product));
    dgl_sanitize_key(s->process_name, exe, sizeof(exe));
    if (!s->game_identity.product_name[0] || !strcmp(product, "unknown-game")) {
        snprintf(s->game_key, sizeof(s->game_key), "%s", exe);
    } else if (!strcmp(product, exe) || dgl_contains_ci(product, exe) || dgl_contains_ci(exe, product)) {
        snprintf(s->game_key, sizeof(s->game_key), "%s", product);
    } else {
        snprintf(s->game_key, sizeof(s->game_key), "%s--%s", product, exe);
    }
}

static int process_image_path(DWORD pid, char *out, size_t cap) {
    HANDLE h;
    DWORD n;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    n = (DWORD)cap;
    if (!QueryFullProcessImageNameA(h, 0, out, &n)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return 1;
}

static bool ignored_process_name(const char *name) {
    static const char *ignored[] = {
        "explorer.exe", "services.exe", "winedevice.exe", "plugplay.exe", "rpcss.exe",
        "svchost.exe", "winemenubuilder.exe", "wineboot.exe", "start.exe", "cmd.exe",
        "conhost.exe", "rundll32.exe", "regedit.exe", "taskmgr.exe", "msiexec.exe",
        "G720Probe.exe", "G720Probe-x64.exe", "G720Probe-x86.exe", NULL
    };
    int i;
    for (i = 0; ignored[i]; ++i) if (!_stricmp(name, ignored[i])) return true;
    return false;
}

typedef struct window_score_ctx {
    DWORD pid;
    uint64_t area;
} window_score_ctx;

static BOOL CALLBACK score_window_proc(HWND hwnd, LPARAM param) {
    window_score_ctx *ctx = (window_score_ctx *)param;
    DWORD pid = 0;
    RECT r;
    LONG_PTR style;
    uint64_t area;
    if (!IsWindowVisible(hwnd)) return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    if (!(style & WS_VISIBLE)) return TRUE;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    area = (uint64_t)(r.right - r.left) * (uint64_t)(r.bottom - r.top);
    if (area > ctx->area) ctx->area = area;
    return TRUE;
}

static DWORD find_process_by_name(const char *name) {
    HANDLE snap;
    PROCESSENTRY32A pe;
    DWORD pid = 0;
    if (!name || !name[0]) return 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = (DWORD)sizeof(pe);
    if (Process32FirstA(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextA(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static DWORD find_auto_game(char *name, size_t name_cap, char *path, size_t path_cap) {
    HANDLE snap;
    PROCESSENTRY32A pe;
    DWORD best_pid = 0;
    uint64_t best_score = 0;
    DWORD self = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = (DWORD)sizeof(pe);
    if (Process32FirstA(snap, &pe)) {
        do {
            char image[DGL_PATH_CAP];
            HANDLE h;
            PROCESS_MEMORY_COUNTERS_EX pmc;
            window_score_ctx wc;
            uint64_t score;
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == self || ignored_process_name(pe.szExeFile)) continue;
            if (!process_image_path(pe.th32ProcessID, image, sizeof(image))) continue;
            h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
            if (!h) continue;
            memset(&pmc, 0, sizeof(pmc));
            pmc.cb = (DWORD)sizeof(pmc);
            if (!GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, (DWORD)sizeof(pmc))) {
                CloseHandle(h);
                continue;
            }
            CloseHandle(h);
            memset(&wc, 0, sizeof(wc));
            wc.pid = pe.th32ProcessID;
            EnumWindows(score_window_proc, (LPARAM)&wc);
            if (!wc.area) continue;
            score = wc.area / 1024ull + (uint64_t)pmc.WorkingSetSize / (1024ull * 1024ull);
            if (score > best_score) {
                best_score = score;
                best_pid = pe.th32ProcessID;
                snprintf(name, name_cap, "%s", pe.szExeFile);
                snprintf(path, path_cap, "%s", image);
            }
        } while (Process32NextA(snap, &pe));
    }
    CloseHandle(snap);
    return best_pid;
}

static int env_value(const char *name, char *out, size_t cap) {
    DWORD n;
    if (!name || !out || cap == 0) return 0;
    out[0] = '\0';
    n = GetEnvironmentVariableA(name, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static int env_first(const char *const *names, char *out, size_t cap) {
    int i;
    for (i = 0; names && names[i]; ++i) {
        if (env_value(names[i], out, cap)) return 1;
    }
    if (out && cap) out[0] = '\0';
    return 0;
}

static void detect_probe_runtime(probe_state *s) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    typedef const char *(__cdecl *wine_get_version_fn)(void);
    wine_get_version_fn wine_get_version = NULL;
    char value[DGL_TEXT_CAP];
    char box_version[DGL_TEXT_CAP] = "";
    char fex_version[DGL_TEXT_CAP] = "";
    char proton_hint[DGL_TEXT_CAP] = "";
    char wrapper_hint[DGL_TEXT_CAP] = "";
    static const char *const proton_keys[] = {
        "PROTON_VERSION", "PROTON_BUILD", "PROTONPATH", "STEAM_COMPAT_TOOL_PATHS", NULL
    };
    static const char *const wrapper_keys[] = {
        "WINLATOR_WRAPPER", "WINLATOR_VERSION", "WRAPPER_NAME", "WRAPPER", NULL
    };
    bool box = false, fex = false;
    if (ntdll) {
        FARPROC proc = GetProcAddress(ntdll, "wine_get_version");
        if (proc) memcpy(&wine_get_version, &proc, sizeof(wine_get_version));
    }
    if (wine_get_version) {
        const char *v = wine_get_version();
        if (v) snprintf(s->wine_version, sizeof(s->wine_version), "%s", v);
    }
    if (env_first(proton_keys, proton_hint, sizeof(proton_hint))) {
        const char *hint = base_name(proton_hint);
        snprintf(s->proton_version, sizeof(s->proton_version), "%s", hint && hint[0] ? hint : proton_hint);
    } else if (GetEnvironmentVariableA("STEAM_COMPAT_DATA_PATH", value, (DWORD)sizeof(value)) > 0 ||
               GetEnvironmentVariableA("PROTON_LOG", value, (DWORD)sizeof(value)) > 0 ||
               GetEnvironmentVariableA("PROTON_VERB", value, (DWORD)sizeof(value)) > 0) {
        snprintf(s->proton_version, sizeof(s->proton_version), "detected (version unavailable)");
    }
    (void)env_value("BOX64_VERSION", box_version, sizeof(box_version));
    (void)env_value("FEX_VERSION", fex_version, sizeof(fex_version));
    if (env_first(wrapper_keys, wrapper_hint, sizeof(wrapper_hint)))
        snprintf(s->wrapper_identity, sizeof(s->wrapper_identity), "%s", base_name(wrapper_hint));
    if (GetEnvironmentVariableA("BOX64_DYNAREC", value, (DWORD)sizeof(value)) > 0 ||
        GetEnvironmentVariableA("BOX64_PATH", value, (DWORD)sizeof(value)) > 0 ||
        GetEnvironmentVariableA("BOX64_LD_LIBRARY_PATH", value, (DWORD)sizeof(value)) > 0) box = true;
    if (GetEnvironmentVariableA("FEX_ROOTFS", value, (DWORD)sizeof(value)) > 0 ||
        GetEnvironmentVariableA("FEX_APP_CONFIG_LOCATION", value, (DWORD)sizeof(value)) > 0 ||
        GetEnvironmentVariableA("FEX_OUTPUTLOG", value, (DWORD)sizeof(value)) > 0) fex = true;
    if (box && fex) {
        if (box_version[0] || fex_version[0])
            snprintf(s->translator, sizeof(s->translator), "Box64%s%s + FEX%s%s",
                     box_version[0] ? " " : "", box_version, fex_version[0] ? " " : "", fex_version);
        else snprintf(s->translator, sizeof(s->translator), "Box64 + FEX indicators");
    } else if (box) {
        snprintf(s->translator, sizeof(s->translator), "Box64%s%s", box_version[0] ? " " : "", box_version);
    } else if (fex) {
        snprintf(s->translator, sizeof(s->translator), "FEX%s%s", fex_version[0] ? " " : "", fex_version);
    } else snprintf(s->translator, sizeof(s->translator), "unknown");
}

static bool relevant_env_name(const char *name) {
    static const char *prefixes[] = {
        "DXVK_", "VKD3D_", "WINE", "BOX64_", "FEX_", "PROTON_", "STEAM_COMPAT_",
        "VK_", "MESA_", "PAN_", "GALLIUM_", "ZINK_", "WINLATOR_", NULL
    };
    int i;
    for (i = 0; prefixes[i]; ++i) {
        if (!_strnicmp(name, prefixes[i], strlen(prefixes[i]))) return true;
    }
    return false;
}

static void write_environment_snapshot(probe_state *s) {
    LPCH env;
    LPCH p;
    char path[DGL_PATH_CAP];
    FILE *f;
    path_join(path, sizeof(path), s->session_dir, "environment.json");
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"schema\": 1,\n  \"variables\": {\n");
    env = GetEnvironmentStringsA();
    if (env) {
        bool first = true;
        for (p = env; *p; p += strlen(p) + 1) {
            const char *eq = strchr(p, '=');
            char name[256];
            char escaped[DGL_PATH_CAP * 2];
            size_t n;
            if (!eq || eq == p) continue;
            n = (size_t)(eq - p);
            if (n >= sizeof(name)) continue;
            memcpy(name, p, n);
            name[n] = '\0';
            if (!relevant_env_name(name)) continue;
            dgl_json_escape(eq + 1, escaped, sizeof(escaped));
            fprintf(f, "%s    \"%s\": \"%s\"", first ? "" : ",\n", name, escaped);
            first = false;
        }
        FreeEnvironmentStringsA(env);
    }
    fprintf(f, "\n  }\n}\n");
    fclose(f);
}

static bool module_relevant(const char *module, const char *path) {
    static const char *keys[] = {
        "dxgi", "d3d9", "d3d10", "d3d11", "d3d12", "dxvk", "vkd3d",
        "vulkan", "winevulkan", "mesa", "panvk", "wrapper", "zink", NULL
    };
    int i;
    for (i = 0; keys[i]; ++i) {
        if (dgl_contains_ci(module, keys[i]) || dgl_contains_ci(path, keys[i])) return true;
    }
    return false;
}

static void update_stack_summary(probe_state *s, bool seen_dxvk, bool seen_vkd3d,
                                 bool seen_winevulkan, bool seen_vulkan) {
    if (seen_dxvk && seen_vkd3d) {
        snprintf(s->graphics_stack, sizeof(s->graphics_stack), "DXVK + VKD3D-Proton + Vulkan");
        if (!s->wrapper_identity[0]) snprintf(s->wrapper_identity, sizeof(s->wrapper_identity), "DXVK + VKD3D-Proton");
    } else if (seen_vkd3d) {
        snprintf(s->graphics_stack, sizeof(s->graphics_stack), "VKD3D-Proton + Vulkan");
        if (!s->wrapper_identity[0]) snprintf(s->wrapper_identity, sizeof(s->wrapper_identity), "VKD3D-Proton");
    } else if (seen_dxvk) {
        snprintf(s->graphics_stack, sizeof(s->graphics_stack), "DXVK + Vulkan");
        if (!s->wrapper_identity[0]) snprintf(s->wrapper_identity, sizeof(s->wrapper_identity), "DXVK");
    } else if (seen_winevulkan || seen_vulkan) {
        snprintf(s->graphics_stack, sizeof(s->graphics_stack), "Vulkan path (D3D wrapper not identified)");
    } else {
        snprintf(s->graphics_stack, sizeof(s->graphics_stack), "not identified yet");
    }
}

static void scan_modules(probe_state *s) {
    HANDLE snap;
    MODULEENTRY32A me;
    char out_path[DGL_PATH_CAP];
    FILE *f;
    bool seen_dxvk = false, seen_vkd3d = false, seen_winevulkan = false, seen_vulkan = false;
    if (!s->pid || !s->session_open) return;
    path_join(out_path, sizeof(out_path), s->session_dir, "modules.tsv");
    f = fopen(out_path, "w");
    if (!f) return;
    fprintf(f, "module\tpath\tsha256\tproduct_name\tproduct_version\tfile_version\tidentity\n");
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, s->pid);
    if (snap == INVALID_HANDLE_VALUE) {
        fprintf(f, "<unavailable>\terror=%lu\t\t\n", (unsigned long)GetLastError());
        fclose(f);
        return;
    }
    memset(&me, 0, sizeof(me));
    me.dwSize = (DWORD)sizeof(me);
    if (Module32FirstA(snap, &me)) {
        do {
            char hash[65] = "";
            char identity[DGL_TEXT_CAP] = "";
            file_identity module_identity;
            const char *needles[] = {"DXVK v", "DXVK", "vkd3d-proton", "vkd3d", "Mesa ", "PanVK", NULL};
            int i;
            memset(&module_identity, 0, sizeof(module_identity));
            if (!module_relevant(me.szModule, me.szExePath)) continue;
            if (dgl_sha256_file(me.szExePath, hash) != 0) snprintf(hash, sizeof(hash), "unavailable");
            read_file_identity(me.szExePath, &module_identity);
            for (i = 0; needles[i]; ++i) {
                if (dgl_find_ascii_token_in_file(me.szExePath, needles[i], identity, sizeof(identity)) == 0)
                    break;
            }
            {
                bool is_dxvk = dgl_contains_ci(identity, "DXVK") ||
                               dgl_contains_ci(me.szExePath, "dxvk") ||
                               dgl_contains_ci(module_identity.product_name, "DXVK");
                bool is_vkd3d = dgl_contains_ci(identity, "vkd3d-proton") ||
                                dgl_contains_ci(me.szExePath, "vkd3d-proton") ||
                                dgl_contains_ci(module_identity.product_name, "vkd3d-proton");
                if (is_dxvk) {
                    seen_dxvk = true;
                    if (!s->dxvk_identity[0]) {
                        if (identity[0]) snprintf(s->dxvk_identity, sizeof(s->dxvk_identity), "%s", identity);
                        else if (module_identity.product_version[0])
                            snprintf(s->dxvk_identity, sizeof(s->dxvk_identity), "DXVK %s", module_identity.product_version);
                        else snprintf(s->dxvk_identity, sizeof(s->dxvk_identity), "DXVK loaded; version not embedded");
                    }
                }
                if (is_vkd3d) {
                    seen_vkd3d = true;
                    if (!s->vkd3d_identity[0]) {
                        if (identity[0]) snprintf(s->vkd3d_identity, sizeof(s->vkd3d_identity), "%s", identity);
                        else if (module_identity.product_version[0])
                            snprintf(s->vkd3d_identity, sizeof(s->vkd3d_identity), "VKD3D-Proton %s", module_identity.product_version);
                        else snprintf(s->vkd3d_identity, sizeof(s->vkd3d_identity), "vkd3d-proton loaded; version not embedded");
                    }
                }
            }
            if (dgl_contains_ci(me.szModule, "winevulkan")) seen_winevulkan = true;
            if (dgl_contains_ci(me.szModule, "vulkan") || dgl_contains_ci(me.szExePath, "vulkan")) seen_vulkan = true;
            if (dgl_contains_ci(identity, "Mesa") && !s->mesa_identity[0])
                snprintf(s->mesa_identity, sizeof(s->mesa_identity), "%s", identity);
            if (dgl_contains_ci(identity, "PanVK") && !s->panvk_identity[0])
                snprintf(s->panvk_identity, sizeof(s->panvk_identity), "%s", identity);
            fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                    me.szModule, me.szExePath, hash,
                    module_identity.product_name[0] ? module_identity.product_name : "",
                    module_identity.product_version[0] ? module_identity.product_version : "",
                    module_identity.file_version[0] ? module_identity.file_version : "",
                    identity[0] ? identity : "not-embedded");
        } while (Module32NextA(snap, &me));
    }
    CloseHandle(snap);
    fclose(f);
    update_stack_summary(s, seen_dxvk, seen_vkd3d, seen_winevulkan, seen_vulkan);
}

static const char *tail_tag_for_name(const char *name) {
    if (dgl_contains_ci(name, "vkd3d")) return "VKD3D";
    if (dgl_contains_ci(name, "dxvk") || dgl_contains_ci(name, "dxgi") ||
        dgl_contains_ci(name, "d3d9") || dgl_contains_ci(name, "d3d10") ||
        dgl_contains_ci(name, "d3d11")) return "DXVK";
    if (dgl_contains_ci(name, "wine")) return "WINE";
    if (dgl_contains_ci(name, "box64")) return "BOX64";
    if (dgl_contains_ci(name, "fex")) return "FEX";
    if (dgl_contains_ci(name, "panvk") || dgl_contains_ci(name, "mesa")) return "PANVK";
    if (dgl_contains_ci(name, "vulkan")) return "VULKAN";
    if (dgl_contains_ci(name, "proton")) return "PROTON";
    return NULL;
}

static void safe_filename(const char *input, char *output, size_t cap) {
    size_t i, j = 0;
    if (!cap) return;
    for (i = 0; input && input[i] && j + 1 < cap; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (isalnum(c) || c == '.' || c == '-' || c == '_') output[j++] = (char)c;
        else output[j++] = '_';
    }
    output[j] = '\0';
}

static int add_tail_path(probe_state *s, const char *path, const char *tag, bool from_beginning) {
    size_t i;
    log_tail *t;
    char filename[MAX_PATH];
    if (!path || !path[0] || !tag || !tag[0] || s->tail_count >= DGL_MAX_TAILS) return -1;
    for (i = 0; i < s->tail_count; ++i) if (!_stricmp(s->tails[i].path, path)) return 0;
    t = &s->tails[s->tail_count++];
    memset(t, 0, sizeof(*t));
    snprintf(t->tag, sizeof(t->tag), "%s", tag);
    snprintf(t->path, sizeof(t->path), "%s", path);
    safe_filename(base_name(path), filename, sizeof(filename));
    snprintf(t->raw_path, sizeof(t->raw_path), "%s\\external-%02llu-%s-%s",
             s->raw_dir, (unsigned long long)s->tail_count, tag, filename);
    t->position = from_beginning ? 0 : file_size_or_zero(path);
    return 0;
}

static void discover_logs_in_dir(probe_state *s, const char *dir) {
    char pattern[DGL_PATH_CAP];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!dir || !dir[0]) return;
    snprintf(pattern, sizeof(pattern), "%s\\*.log", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char *tag;
        char path[DGL_PATH_CAP];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        tag = tail_tag_for_name(fd.cFileName);
        if (!tag) continue;
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        (void)add_tail_path(s, path, tag, false);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void discover_external_logs(probe_state *s) {
    char dir[DGL_PATH_CAP];
    char env[DGL_PATH_CAP];
    char cwd[DGL_PATH_CAP];
    char explicit_file[DGL_PATH_CAP];
    const char *tag;
    if (!s->session_open) return;
    dirname_copy(s->process_path, dir, sizeof(dir));
    discover_logs_in_dir(s, dir);
    if (GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd)) discover_logs_in_dir(s, cwd);
    if (GetEnvironmentVariableA("DXVK_LOG_PATH", env, (DWORD)sizeof(env)) > 0) discover_logs_in_dir(s, env);
    if (GetEnvironmentVariableA("PROTON_LOG_DIR", env, (DWORD)sizeof(env)) > 0) discover_logs_in_dir(s, env);
    if (GetEnvironmentVariableA("VKD3D_LOG_FILE", explicit_file, (DWORD)sizeof(explicit_file)) > 0) {
        tag = "VKD3D";
        (void)add_tail_path(s, explicit_file, tag, false);
    }
}

static void poll_external_tails(probe_state *s) {
    size_t i;
    for (i = 0; i < s->tail_count; ++i) {
        log_tail *t = &s->tails[i];
        char line[DGL_TAIL_LINE_CAP];
        unsigned processed = 0;
        if (!t->src) {
            if (!file_exists(t->path)) continue;
            t->src = fopen(t->path, "rb");
            if (!t->src) continue;
            if (t->position > 0) _fseeki64(t->src, t->position, SEEK_SET);
            t->raw = fopen(t->raw_path, "ab");
            if (!t->announced) {
                char msg[DGL_PATH_CAP + 128];
                snprintf(msg, sizeof(msg), "tail attached source=%s path=%s", t->tag, t->path);
                master_log(s, "PROBE", msg);
                t->announced = true;
            }
        }
        while (processed < 512 && fgets(line, sizeof(line), t->src)) {
            size_t n = strlen(line);
            char display[DGL_TAIL_LINE_CAP];
            if (t->raw) { fwrite(line, 1, n, t->raw); fflush(t->raw); }
            snprintf(display, sizeof(display), "%s", line);
            while (n && (display[n - 1] == '\n' || display[n - 1] == '\r')) display[--n] = '\0';
            if (n) {
                master_log(s, t->tag, display);
                timeline_text_event(s, t->tag, display);
                ++t->lines;
                ++s->external_lines;
            }
            ++processed;
        }
        if (t->src) {
            t->position = _ftelli64(t->src);
            if (feof(t->src)) clearerr(t->src);
        }
    }
}

static void write_game_identity(probe_state *s) {
    char path[DGL_PATH_CAP];
    char utc[64];
    FILE *f;
    path_join(path, sizeof(path), s->game_dir, "game.json");
    f = fopen(path, "w");
    if (!f) return;
    utc_stamp(utc, sizeof(utc));
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 1,\n");
    json_write_string(f, "game_key", s->game_key, true);
    json_write_string(f, "last_seen_utc", utc, true);
    json_write_string(f, "executable_name", s->process_name, true);
    json_write_string(f, "product_name", s->game_identity.product_name, true);
    json_write_string(f, "product_version", s->game_identity.product_version, true);
    json_write_string(f, "file_description", s->game_identity.file_description, true);
    json_write_string(f, "file_version", s->game_identity.file_version, true);
    json_write_string(f, "last_executable_path", s->process_path, true);
    json_write_string(f, "last_executable_sha256", s->exe_sha256, false);
    fprintf(f, "}\n");
    fclose(f);
}

static void append_game_history(probe_state *s, const char *event) {
    char path[DGL_PATH_CAP];
    char utc[64], eevent[128], esession[256], epath[DGL_PATH_CAP * 2];
    FILE *f;
    path_join(path, sizeof(path), s->game_dir, "history.jsonl");
    f = fopen(path, "a");
    if (!f) return;
    utc_stamp(utc, sizeof(utc));
    dgl_json_escape(event ? event : "event", eevent, sizeof(eevent));
    dgl_json_escape(s->session_id, esession, sizeof(esession));
    dgl_json_escape(s->session_dir, epath, sizeof(epath));
    fprintf(f,
            "{\"utc\":\"%s\",\"event\":\"%s\",\"session_id\":\"%s\","
            "\"session_dir\":\"%s\",\"pid\":%lu}\n",
            utc, eevent, esession, epath, (unsigned long)s->pid);
    fclose(f);
}

static void write_manifest(probe_state *s, bool final) {
    char path[DGL_PATH_CAP];
    char utc[64];
    FILE *f;
    if (!s->session_open) return;
    path_join(path, sizeof(path), s->session_dir, "manifest.json");
    f = fopen(path, "w");
    if (!f) return;
    utc_stamp(utc, sizeof(utc));
    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 2,\n");
    json_write_string(f, "tool", "Drive GPU Lab", true);
    json_write_string(f, "version", DGL_VERSION_STRING, true);
    json_write_string(f, "state", final ? "final" : "capturing", true);
    json_write_string(f, final ? "updated_utc" : "start_utc", utc, true);
    json_write_string(f, "session_id", s->session_id, true);
    json_write_string(f, "game_key", s->game_key, true);
    json_write_string(f, "target_process", s->process_name, true);
    json_write_string(f, "target_path", s->process_path, true);
    json_write_string(f, "target_sha256", s->exe_sha256, true);
    fprintf(f, "  \"pid\": %lu,\n", (unsigned long)s->pid);
    fprintf(f, "  \"capture\": {\"auto_mode\":%s,\"launch_mode\":%s,\"deep_mode\":%s},\n",
            s->auto_mode ? "true" : "false", s->launch_mode ? "true" : "false",
            s->deep_mode ? "true" : "false");
    fprintf(f, "  \"runtime\": {\n");
    {
        char e1[DGL_TEXT_CAP * 2], e2[DGL_TEXT_CAP * 2], e3[DGL_TEXT_CAP * 2];
        char e4[DGL_TEXT_CAP * 2], e5[DGL_TEXT_CAP * 2], e6[DGL_TEXT_CAP * 2];
        char e7[DGL_TEXT_CAP * 2], e8[DGL_TEXT_CAP * 2], e9[DGL_TEXT_CAP * 2];
        dgl_json_escape(s->wine_version, e1, sizeof(e1));
        dgl_json_escape(s->proton_version, e2, sizeof(e2));
        dgl_json_escape(s->translator, e3, sizeof(e3));
        dgl_json_escape(s->graphics_stack, e4, sizeof(e4));
        dgl_json_escape(s->dxvk_identity, e5, sizeof(e5));
        dgl_json_escape(s->vkd3d_identity, e6, sizeof(e6));
        dgl_json_escape(s->mesa_identity, e7, sizeof(e7));
        dgl_json_escape(s->panvk_identity, e8, sizeof(e8));
        dgl_json_escape(s->wrapper_identity, e9, sizeof(e9));
        fprintf(f,
                "    \"wine\":\"%s\",\"proton\":\"%s\",\"translator\":\"%s\",\n"
                "    \"wrapper\":\"%s\",\"graphics_stack\":\"%s\",\"dxvk\":\"%s\",\"vkd3d\":\"%s\",\n"
                "    \"mesa\":\"%s\",\"panvk\":\"%s\"\n",
                e1, e2, e3, e9, e4, e5, e6, e7, e8);
    }
    fprintf(f, "  },\n");
    fprintf(f, "  \"vulkan\": {\n");
    {
        char egpu[DGL_TEXT_CAP * 2], edrv[DGL_TEXT_CAP * 2], einfo[DGL_TEXT_CAP * 2];
        char eapi[DGL_TEXT_CAP * 2], ever[DGL_TEXT_CAP * 2];
        dgl_json_escape(s->gpu_name, egpu, sizeof(egpu));
        dgl_json_escape(s->driver_name, edrv, sizeof(edrv));
        dgl_json_escape(s->driver_info, einfo, sizeof(einfo));
        dgl_json_escape(s->vulkan_api, eapi, sizeof(eapi));
        dgl_json_escape(s->vulkan_driver_version, ever, sizeof(ever));
        fprintf(f,
                "    \"gpu_name\":\"%s\",\"driver_name\":\"%s\",\"driver_info\":\"%s\",\n"
                "    \"api_version\":\"%s\",\"driver_version\":\"%s\"\n",
                egpu, edrv, einfo, eapi, ever);
    }
    fprintf(f, "  },\n");
    fprintf(f,
            "  \"counters\": {\"frames\":%llu,\"udp_events\":%llu,\"udp_dropped\":%llu,"
            "\"external_log_lines\":%llu,\"markers\":%llu,\"anomalies\":%llu,"
            "\"device_lost\":%llu,\"ring_overwrites\":%llu}\n",
            (unsigned long long)s->frames.frames,
            (unsigned long long)s->udp_events,
            (unsigned long long)s->udp_dropped,
            (unsigned long long)s->external_lines,
            (unsigned long long)s->markers_count,
            (unsigned long long)s->anomalies_count,
            (unsigned long long)s->device_lost_count,
            (unsigned long long)s->ring.overwritten);
    fprintf(f, "}\n");
    fclose(f);
}

static void send_host_session_hello(probe_state *s) {
    char e_session[256], e_game[DGL_GAME_KEY_CAP * 2], json[1024];
    if (s->udp == INVALID_SOCKET || !s->session_open) return;
    dgl_json_escape(s->session_id, e_session, sizeof(e_session));
    dgl_json_escape(s->game_key, e_game, sizeof(e_game));
    snprintf(json, sizeof(json),
             "{\"v\":%d,\"source\":\"probe\",\"type\":\"session_hello\","
             "\"session_id\":\"%s\",\"game_key\":\"%s\",\"pid\":%lu,\"deep\":%s}",
             DGL_PROTOCOL_VERSION, e_session, e_game, (unsigned long)s->pid,
             s->deep_mode ? "true" : "false");
    (void)sendto(s->udp, json, (int)strlen(json), 0,
                 (struct sockaddr *)&s->host_control_addr, sizeof(s->host_control_addr));
}

static void reset_session_runtime(probe_state *s) {
    /* Preserve CLI/lifetime settings and deep_mode, reset every per-session fact. */
    s->event_seq = 0;
    s->udp_events = 0;
    s->udp_dropped = 0;
    s->markers_count = 0;
    s->anomalies_count = 0;
    s->device_lost_count = 0;
    s->external_lines = 0;
    s->last_anomaly_ns = 0;
    s->last_frame_rx_ns = 0;
    s->tail_count = 0;
    s->last_module_scan_qpc_ns = 0;
    s->last_tail_scan_qpc_ns = 0;
    s->last_tail_poll_qpc_ns = 0;
    s->prev_proc_100ns = 0;
    s->prev_cpu_sample_qpc_ns = 0;
    memset(&s->frames, 0, sizeof(s->frames));
    memset(&s->m, 0, sizeof(s->m));
    s->m.gpu_temp_mc = -1;
    if (!s->ring.lines) (void)dgl_ring_init(&s->ring);
    else dgl_ring_reset(&s->ring);
    s->gpu_name[0] = '\0';
    s->driver_name[0] = '\0';
    s->driver_info[0] = '\0';
    s->vulkan_api[0] = '\0';
    s->vulkan_driver_version[0] = '\0';
    s->wine_version[0] = '\0';
    s->proton_version[0] = '\0';
    s->translator[0] = '\0';
    s->wrapper_identity[0] = '\0';
    s->graphics_stack[0] = '\0';
    s->dxvk_identity[0] = '\0';
    s->vkd3d_identity[0] = '\0';
    s->mesa_identity[0] = '\0';
    s->panvk_identity[0] = '\0';
    s->host_identity[0] = '\0';
    s->host_kbase[0] = '\0';
    s->host_android[0] = '\0';
    s->host_governor[0] = '\0';
}

static int open_session(probe_state *s) {
    char stamp[64], path[DGL_PATH_CAP], latest[DGL_PATH_CAP], utc[64];
    FILE *f;
    if (!s->process || !s->pid) return -1;
    reset_session_runtime(s);
    read_file_identity(s->process_path, &s->game_identity);
    derive_game_key(s);
    if (dgl_sha256_file(s->process_path, s->exe_sha256) != 0)
        snprintf(s->exe_sha256, sizeof(s->exe_sha256), "unavailable");

    path_join(s->games_root, sizeof(s->games_root), s->lab_root, "games");
    if (mkdir_tree(s->games_root) != 0) return -1;
    path_join(s->game_dir, sizeof(s->game_dir), s->games_root, s->game_key);
    if (mkdir_tree(s->game_dir) != 0) return -1;
    path_join(s->sessions_dir, sizeof(s->sessions_dir), s->game_dir, "sessions");
    if (mkdir_tree(s->sessions_dir) != 0) return -1;

    session_stamp(stamp, sizeof(stamp));
    snprintf(s->session_id, sizeof(s->session_id), "%s-p%lu-q%016llx", stamp,
             (unsigned long)s->pid, (unsigned long long)qpc_ns());
    path_join(s->session_dir, sizeof(s->session_dir), s->sessions_dir, s->session_id);
    if (mkdir_tree(s->session_dir) != 0) return -1;
    path_join(s->raw_dir, sizeof(s->raw_dir), s->session_dir, "raw"); mkdir_tree(s->raw_dir);
    path_join(s->events_dir, sizeof(s->events_dir), s->session_dir, "events"); mkdir_tree(s->events_dir);
    path_join(s->perf_dir, sizeof(s->perf_dir), s->session_dir, "performance"); mkdir_tree(s->perf_dir);
    path_join(s->screenshot_dir, sizeof(s->screenshot_dir), s->session_dir, "screenshots"); mkdir_tree(s->screenshot_dir);

    path_join(path, sizeof(path), s->session_dir, "MASTER.log"); s->master = fopen(path, "w");
    path_join(path, sizeof(path), s->session_dir, "timeline.jsonl"); s->timeline = fopen(path, "w");
    path_join(path, sizeof(path), s->perf_dir, "telemetry.csv"); s->telemetry = fopen(path, "w");
    path_join(path, sizeof(path), s->events_dir, "markers.jsonl"); s->markers = fopen(path, "w");
    path_join(path, sizeof(path), s->events_dir, "anomalies.jsonl"); s->anomalies = fopen(path, "w");
    path_join(path, sizeof(path), s->raw_dir, "hostd.jsonl"); s->host_raw = fopen(path, "w");
    path_join(path, sizeof(path), s->raw_dir, "vklayer.jsonl"); s->vk_raw = fopen(path, "w");
    if (!s->master || !s->timeline || !s->telemetry) return -1;

    fprintf(s->telemetry,
            "qpc_ns,pid,cpu_pct,working_set_mb,fps,frame_ms,p95_ms,p99_ms,p999_ms,"
            "gpu_busy_pct,gpu_freq_hz,gpu_min_freq_hz,gpu_max_freq_hz,gpu_temp_mC,"
            "mem_total_kb,mem_available_kb,swap_free_kb,vk_allocated_bytes,vk_peak_allocated_bytes,"
            "frames,submits,deep_mode\n");

    s->session_open = true;
    s->session_start_qpc_ns = qpc_ns();
    detect_probe_runtime(s);

    write_game_identity(s);
    append_game_history(s, "session_start");
    path_join(latest, sizeof(latest), s->game_dir, "latest.txt");
    f = fopen(latest, "w");
    if (f) { fprintf(f, "%s\n%s\n", s->session_id, s->session_dir); fclose(f); }
    write_environment_snapshot(s);
    scan_modules(s);
    write_manifest(s, false);

    utc_stamp(utc, sizeof(utc));
    {
        char msg[DGL_PATH_CAP + 256];
        snprintf(msg, sizeof(msg),
                 "capture started utc=%s game_key=%s pid=%lu exe=%s",
                 utc, s->game_key, (unsigned long)s->pid, s->process_path);
        master_log(s, "PROBE", msg);
    }
    discover_external_logs(s);
    send_host_session_hello(s);
    return 0;
}

static void write_summary(probe_state *s) {
    char path[DGL_PATH_CAP], utc[64];
    FILE *f;
    uint64_t now = qpc_ns();
    double one_low = 0.0, point_one_low = 0.0;
    if (!s->session_open) return;
    frame_recalc(&s->frames, now);
    if (s->frames.p99_ms > 0.0) one_low = 1000.0 / s->frames.p99_ms;
    if (s->frames.p999_ms > 0.0) point_one_low = 1000.0 / s->frames.p999_ms;
    path_join(path, sizeof(path), s->session_dir, "summary.txt");
    f = fopen(path, "w");
    if (!f) return;
    utc_stamp(utc, sizeof(utc));
    fprintf(f,
            "Drive GPU Lab session summary\n"
            "=============================\n"
            "Version: %s\n"
            "End UTC: %s\n"
            "Game key: %s\n"
            "Target: %s\n"
            "Target path: %s\n"
            "Executable SHA-256: %s\n"
            "PID: %lu\n"
            "Wine: %s\n"
            "Proton: %s\n"
            "Translator: %s\n"
            "Wrapper: %s\n"
            "Graphics stack: %s\n"
            "DXVK: %s\n"
            "VKD3D: %s\n"
            "Mesa: %s\n"
            "PanVK: %s\n"
            "GPU: %s\n"
            "Vulkan driver: %s\n"
            "Vulkan driver info: %s\n"
            "Vulkan API: %s\n"
            "Vulkan driver version: %s\n"
            "Android host: %s\n"
            "Kbase: %s\n"
            "GPU governor: %s\n"
            "Frames observed: %llu\n"
            "Average FPS: %.3f\n"
            "1%% low estimate (P99 frame): %.3f\n"
            "0.1%% low estimate (P99.9 frame): %.3f\n"
            "Last frame: %.3f ms\n"
            "P95 frametime: %.3f ms\n"
            "P99 frametime: %.3f ms\n"
            "P99.9 frametime: %.3f ms\n"
            "Last process CPU: %.3f %%\n"
            "Last process RAM: %.3f MB\n"
            "Last GPU busy: %.3f %%\n"
            "Last GPU clock: %.3f MHz\n"
            "Last GPU temp: %.3f C\n"
            "Vulkan allocated: %.3f MB\n"
            "Vulkan peak allocated: %.3f MB\n"
            "UDP events: %llu\n"
            "UDP receive/drop errors: %llu\n"
            "External log lines merged: %llu\n"
            "Markers: %llu\n"
            "Anomalies: %llu\n"
            "Device lost events: %llu\n"
            "Ring overwrites: %llu\n",
            DGL_VERSION_STRING, utc, s->game_key, s->process_name, s->process_path,
            s->exe_sha256, (unsigned long)s->pid,
            s->wine_version[0] ? s->wine_version : "unknown",
            s->proton_version[0] ? s->proton_version : "not detected",
            s->translator[0] ? s->translator : "unknown",
            s->wrapper_identity[0] ? s->wrapper_identity : "not identified",
            s->graphics_stack[0] ? s->graphics_stack : "unknown",
            s->dxvk_identity[0] ? s->dxvk_identity : "not identified",
            s->vkd3d_identity[0] ? s->vkd3d_identity : "not identified",
            s->mesa_identity[0] ? s->mesa_identity : "not identified",
            s->panvk_identity[0] ? s->panvk_identity : "not identified",
            s->gpu_name[0] ? s->gpu_name : "unknown",
            s->driver_name[0] ? s->driver_name : "unknown",
            s->driver_info[0] ? s->driver_info : "unknown",
            s->vulkan_api[0] ? s->vulkan_api : "unknown",
            s->vulkan_driver_version[0] ? s->vulkan_driver_version : "unknown",
            s->host_identity[0] ? s->host_identity : "unavailable",
            s->host_kbase[0] ? s->host_kbase : "unavailable",
            s->host_governor[0] ? s->host_governor : "unavailable",
            (unsigned long long)s->frames.frames, s->frames.avg_fps, one_low, point_one_low,
            s->frames.current_ms, s->frames.p95_ms, s->frames.p99_ms, s->frames.p999_ms,
            s->m.process_cpu_pct, s->m.process_mem_mb, s->m.gpu_busy_pct,
            (double)s->m.gpu_freq_hz / 1000000.0,
            s->m.gpu_temp_mc > 0 ? (double)s->m.gpu_temp_mc / 1000.0 : 0.0,
            (double)s->m.vk_allocated_bytes / (1024.0 * 1024.0),
            (double)s->m.vk_peak_allocated_bytes / (1024.0 * 1024.0),
            (unsigned long long)s->udp_events, (unsigned long long)s->udp_dropped,
            (unsigned long long)s->external_lines, (unsigned long long)s->markers_count,
            (unsigned long long)s->anomalies_count, (unsigned long long)s->device_lost_count,
            (unsigned long long)s->ring.overwritten);
    fclose(f);
}

static void close_tail_sources(probe_state *s) {
    size_t i;
    for (i = 0; i < s->tail_count; ++i) {
        if (s->tails[i].src) fclose(s->tails[i].src);
        if (s->tails[i].raw) fclose(s->tails[i].raw);
        s->tails[i].src = NULL;
        s->tails[i].raw = NULL;
    }
    s->tail_count = 0;
}

static void close_session_files(probe_state *s) {
    if (s->master) { fclose(s->master); s->master = NULL; }
    if (s->timeline) { fclose(s->timeline); s->timeline = NULL; }
    if (s->telemetry) { fclose(s->telemetry); s->telemetry = NULL; }
    if (s->markers) { fclose(s->markers); s->markers = NULL; }
    if (s->anomalies) { fclose(s->anomalies); s->anomalies = NULL; }
    if (s->host_raw) { fclose(s->host_raw); s->host_raw = NULL; }
    if (s->vk_raw) { fclose(s->vk_raw); s->vk_raw = NULL; }
}

static void finalize_session(probe_state *s, const char *reason) {
    char msg[256];
    if (!s->session_open) return;
    snprintf(msg, sizeof(msg), "capture stopping reason=%s", reason ? reason : "unknown");
    master_log(s, "PROBE", msg);
    scan_modules(s);
    write_summary(s);
    write_manifest(s, true);
    append_game_history(s, reason ? reason : "session_end");
    close_tail_sources(s);
    close_session_files(s);
    s->session_open = false;
    ++s->sessions_completed;
}

static void detach_target(probe_state *s, const char *why) {
    if (s->process) {
        CloseHandle(s->process);
        s->process = NULL;
    }
    if (s->session_open) finalize_session(s, why ? why : "detach");
    s->pid = 0;
    s->prev_proc_100ns = 0;
    s->prev_cpu_sample_qpc_ns = 0;
    s->target_exit_qpc_ns = qpc_ns();
    s->process_name[0] = '\0';
    s->process_path[0] = '\0';
}

static int attach_target(probe_state *s, DWORD pid, const char *known_name, const char *known_path) {
    char image[DGL_PATH_CAP];
    HANDLE process;
    if (!pid || s->process) return -1;
    image[0] = '\0';
    if (known_path && known_path[0]) snprintf(image, sizeof(image), "%s", known_path);
    else if (!process_image_path(pid, image, sizeof(image))) return -1;
    process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process) return -1;
    s->process = process;
    s->pid = pid;
    s->target_ever_attached = true;
    s->process_start_qpc_ns = qpc_ns();
    snprintf(s->process_path, sizeof(s->process_path), "%s", image);
    snprintf(s->process_name, sizeof(s->process_name), "%s",
             known_name && known_name[0] ? known_name : base_name(image));
    if (open_session(s) != 0) {
        CloseHandle(s->process);
        s->process = NULL;
        s->pid = 0;
        return -1;
    }
    return 0;
}

static void try_attach_target(probe_state *s) {
    DWORD pid = 0;
    char name[MAX_PATH] = "";
    char path[DGL_PATH_CAP] = "";
    uint64_t now = qpc_ns();
    if (s->process || now - s->last_process_scan_qpc_ns < DGL_PROCESS_SCAN_NS) return;
    s->last_process_scan_qpc_ns = now;
    if (s->requested_pid) {
        pid = s->requested_pid;
        if (!process_image_path(pid, path, sizeof(path))) return;
        snprintf(name, sizeof(name), "%s", base_name(path));
    } else if (s->requested_process[0]) {
        pid = find_process_by_name(s->requested_process);
        if (!pid || !process_image_path(pid, path, sizeof(path))) return;
        snprintf(name, sizeof(name), "%s", s->requested_process);
    } else if (s->auto_mode) {
        pid = find_auto_game(name, sizeof(name), path, sizeof(path));
    }
    if (pid) (void)attach_target(s, pid, name, path);
}

static void sample_process(probe_state *s, uint64_t now_ns) {
    FILETIME ct, et, kt, ut;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    uint64_t proc100, delta100, wall100;
    if (!s->process || !s->session_open) return;
    if (WaitForSingleObject(s->process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        char msg[128];
        GetExitCodeProcess(s->process, &code);
        snprintf(msg, sizeof(msg), "target exited code=%lu", (unsigned long)code);
        master_log(s, "PROCESS", msg);
        detach_target(s, code == 0 ? "target_exit_clean" : "target_exit_nonzero");
        return;
    }
    if (GetProcessTimes(s->process, &ct, &et, &kt, &ut)) {
        proc100 = filetime_u64(kt) + filetime_u64(ut);
        if (s->prev_proc_100ns && now_ns > s->prev_cpu_sample_qpc_ns) {
            delta100 = proc100 - s->prev_proc_100ns;
            wall100 = (now_ns - s->prev_cpu_sample_qpc_ns) / 100ull;
            if (wall100) s->m.process_cpu_pct = 100.0 * (double)delta100 / (double)wall100;
        }
        s->prev_proc_100ns = proc100;
        s->prev_cpu_sample_qpc_ns = now_ns;
    }
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = (DWORD)sizeof(pmc);
    if (GetProcessMemoryInfo(s->process, (PROCESS_MEMORY_COUNTERS *)&pmc, (DWORD)sizeof(pmc)))
        s->m.process_mem_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
}

static void write_telemetry(probe_state *s, uint64_t now_ns) {
    if (!s->telemetry) return;
    frame_recalc(&s->frames, now_ns);
    s->m.fps = s->frames.fps_window;
    s->m.p95_ms = s->frames.p95_ms;
    s->m.p99_ms = s->frames.p99_ms;
    fprintf(s->telemetry,
            "%llu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%lld,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d\n",
            (unsigned long long)now_ns, (unsigned long)s->pid,
            s->m.process_cpu_pct, s->m.process_mem_mb,
            s->m.fps, s->m.frame_ms, s->m.p95_ms, s->m.p99_ms, s->frames.p999_ms,
            s->m.gpu_busy_pct, (unsigned long long)s->m.gpu_freq_hz,
            (unsigned long long)s->m.gpu_min_freq_hz, (unsigned long long)s->m.gpu_max_freq_hz,
            (long long)s->m.gpu_temp_mc,
            (unsigned long long)s->m.mem_total_kb, (unsigned long long)s->m.mem_available_kb,
            (unsigned long long)s->m.swap_free_kb,
            (unsigned long long)s->m.vk_allocated_bytes,
            (unsigned long long)s->m.vk_peak_allocated_bytes,
            (unsigned long long)s->m.frames_seen,
            (unsigned long long)s->m.submits_since_present, s->deep_mode ? 1 : 0);
    fflush(s->telemetry);
}

static void process_datagram(probe_state *s, char *buf, int len) {
    char source[64], type[64];
    uint64_t now = qpc_ns();
    uint64_t submits;
    if (len <= 0) return;
    if (!s->session_open) return;
    if (len >= DGL_MAX_EVENT_BYTES) { ++s->udp_dropped; return; }
    buf[len] = '\0';
    ++s->udp_events;
    timeline_raw_event(s, buf);
    json_string(buf, "source", source, sizeof(source));
    json_string(buf, "type", type, sizeof(type));
    if (!source[0]) snprintf(source, sizeof(source), "unknown");
    if (!type[0]) snprintf(type, sizeof(type), "event");
    master_log(s, source, buf);

    if (!_stricmp(source, "hostd")) {
        if (s->host_raw) { fprintf(s->host_raw, "%s\n", buf); fflush(s->host_raw); }
        if (!_stricmp(type, "telemetry")) {
            double busy = json_double(buf, "gpu_busy_pct", -1.0);
            uint64_t freq = json_u64(buf, "gpu_freq_hz", 0);
            uint64_t minf = json_u64(buf, "gpu_min_freq_hz", 0);
            uint64_t maxf = json_u64(buf, "gpu_max_freq_hz", 0);
            uint64_t mem_total = json_u64(buf, "mem_total_kb", 0);
            uint64_t mem_avail = json_u64(buf, "mem_available_kb", 0);
            uint64_t swap_free = json_u64(buf, "swap_free_kb", 0);
            int64_t temp = json_i64(buf, "gpu_temp_mC", -1);
            if (busy >= 0.0) s->m.gpu_busy_pct = busy;
            if (freq) s->m.gpu_freq_hz = freq;
            if (minf) s->m.gpu_min_freq_hz = minf;
            if (maxf) s->m.gpu_max_freq_hz = maxf;
            if (mem_total) s->m.mem_total_kb = mem_total;
            if (mem_avail) s->m.mem_available_kb = mem_avail;
            if (swap_free) s->m.swap_free_kb = swap_free;
            if (temp >= 0) s->m.gpu_temp_mc = temp;
            json_string(buf, "gpu_governor", s->host_governor, sizeof(s->host_governor));
        } else if (!_stricmp(type, "identity")) {
            char model[256], android[128], kbase[256];
            json_string(buf, "android_model", model, sizeof(model));
            json_string(buf, "android_release", android, sizeof(android));
            json_string(buf, "kbase_version", kbase, sizeof(kbase));
            snprintf(s->host_identity, sizeof(s->host_identity), "%s / Android %s",
                     model[0] ? model : "Android host", android[0] ? android : "unknown");
            snprintf(s->host_android, sizeof(s->host_android), "%s", android);
            snprintf(s->host_kbase, sizeof(s->host_kbase), "%s", kbase);
        }
    } else if (!_stricmp(source, "vklayer")) {
        if (s->vk_raw) { fprintf(s->vk_raw, "%s\n", buf); fflush(s->vk_raw); }
        if (!_stricmp(type, "frame")) {
            uint64_t source_frame_ns = json_u64(buf, "frame_ns", 0);
            submits = json_u64(buf, "submits", 0);
            frame_push(s, now, source_frame_ns, submits);
        } else if (!_stricmp(type, "device")) {
            json_string(buf, "gpu_name", s->gpu_name, sizeof(s->gpu_name));
            json_string(buf, "driver_name", s->driver_name, sizeof(s->driver_name));
            json_string(buf, "driver_info", s->driver_info, sizeof(s->driver_info));
            json_string(buf, "api_version_string", s->vulkan_api, sizeof(s->vulkan_api));
            json_string(buf, "driver_version_string", s->vulkan_driver_version, sizeof(s->vulkan_driver_version));
            write_manifest(s, false);
        } else if (!_stricmp(type, "memory")) {
            s->m.vk_allocated_bytes = json_u64(buf, "allocated_bytes", s->m.vk_allocated_bytes);
            s->m.vk_peak_allocated_bytes = json_u64(buf, "peak_allocated_bytes", s->m.vk_peak_allocated_bytes);
        }
    }

    if (!_stricmp(type, "device_lost") || strstr(buf, "VK_ERROR_DEVICE_LOST")) {
        ++s->anomalies_count;
        ++s->device_lost_count;
        if (s->anomalies) {
            fprintf(s->anomalies,
                    "{\"qpc_ns\":%llu,\"type\":\"device_lost\",\"source\":\"%s\"}\n",
                    (unsigned long long)now, source);
            fflush(s->anomalies);
        }
    }
}

static void drain_udp(probe_state *s) {
    for (;;) {
        char buf[DGL_MAX_EVENT_BYTES + 1];
        struct sockaddr_in from;
        int fromlen = (int)sizeof(from);
        int n = recvfrom(s->udp, buf, DGL_MAX_EVENT_BYTES, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK) ++s->udp_dropped;
            break;
        }
        process_datagram(s, buf, n);
    }
}

static int capture_screen_bmp(const char *path) {
    HDC screen = GetDC(NULL), mem = NULL;
    HBITMAP bmp = NULL, old = NULL;
    BITMAPINFOHEADER bih;
    BITMAPFILEHEADER bfh;
    HANDLE f = INVALID_HANDLE_VALUE;
    BYTE *pixels = NULL;
    DWORD wrote;
    int w, h, stride;
    int rc = -1;
    if (!screen) return -1;
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    if (w <= 0 || h <= 0) goto done;
    mem = CreateCompatibleDC(screen); if (!mem) goto done;
    bmp = CreateCompatibleBitmap(screen, w, h); if (!bmp) goto done;
    old = (HBITMAP)SelectObject(mem, bmp);
    if (!BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY | CAPTUREBLT)) goto done;
    memset(&bih, 0, sizeof(bih));
    bih.biSize = (DWORD)sizeof(bih); bih.biWidth = w; bih.biHeight = h; bih.biPlanes = 1;
    bih.biBitCount = 24; bih.biCompression = BI_RGB;
    stride = ((w * 3 + 3) & ~3);
    pixels = (BYTE *)malloc((size_t)stride * (size_t)h); if (!pixels) goto done;
    if (!GetDIBits(mem, bmp, 0, (UINT)h, pixels, (BITMAPINFO *)&bih, DIB_RGB_COLORS)) goto done;
    memset(&bfh, 0, sizeof(bfh));
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = (DWORD)(sizeof(bfh) + sizeof(bih));
    bfh.bfSize = bfh.bfOffBits + (DWORD)((size_t)stride * (size_t)h);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) goto done;
    if (!WriteFile(f, &bfh, (DWORD)sizeof(bfh), &wrote, NULL)) goto done;
    if (!WriteFile(f, &bih, (DWORD)sizeof(bih), &wrote, NULL)) goto done;
    if (!WriteFile(f, pixels, (DWORD)((size_t)stride * (size_t)h), &wrote, NULL)) goto done;
    rc = 0;
done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(pixels);
    if (old && mem) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(NULL, screen);
    return rc;
}

static void marker_capture(probe_state *s, bool screenshot) {
    char ctx[DGL_PATH_CAP], shot[DGL_PATH_CAP], line[256];
    FILE *f;
    size_t i, count;
    uint64_t seq;
    const char *text;
    uint64_t id;
    uint64_t now;
    if (!s->session_open) return;
    id = ++s->markers_count;
    now = qpc_ns();
    snprintf(line, sizeof(line), "BUG_MARK id=%llu screenshot=%d",
             (unsigned long long)id, screenshot ? 1 : 0);
    master_log(s, "MARK", line);
    if (s->markers) {
        fprintf(s->markers,
                "{\"qpc_ns\":%llu,\"id\":%llu,\"pid\":%lu,\"fps\":%.3f,"
                "\"frame_ms\":%.3f,\"gpu_busy_pct\":%.3f,\"gpu_freq_hz\":%llu}\n",
                (unsigned long long)now, (unsigned long long)id, (unsigned long)s->pid,
                s->m.fps, s->m.frame_ms, s->m.gpu_busy_pct,
                (unsigned long long)s->m.gpu_freq_hz);
        fflush(s->markers);
    }
    snprintf(ctx, sizeof(ctx), "%s\\marker-%03llu-context.log", s->events_dir, (unsigned long long)id);
    f = fopen(ctx, "w");
    if (f) {
        count = dgl_ring_count(&s->ring);
        for (i = 0; i < count; ++i) {
            text = dgl_ring_get_oldest(&s->ring, i, &seq);
            if (text) fprintf(f, "%llu %s\n", (unsigned long long)seq, text);
        }
        fclose(f);
    }
    if (screenshot) {
        snprintf(shot, sizeof(shot), "%s\\marker-%03llu.bmp", s->screenshot_dir, (unsigned long long)id);
        if (capture_screen_bmp(shot) != 0) master_log(s, "WARN", "screenshot capture failed");
    }
}

static LRESULT CALLBACK hud_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    probe_state *s = g_state;
    (void)lp;
    if (msg == WM_HOTKEY && s) {
        if (wp == DGL_HOTKEY_TOGGLE) {
            s->hud_visible = !s->hud_visible;
            ShowWindow(hwnd, s->hud_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        } else if (wp == DGL_HOTKEY_MARK) {
            marker_capture(s, false);
        } else if (wp == DGL_HOTKEY_DEEP) {
            s->deep_mode = !s->deep_mode;
            if (s->session_open) {
                master_log(s, "PROBE", s->deep_mode ? "deep capture marker enabled" : "deep capture marker disabled");
                send_host_session_hello(s);
                write_manifest(s, false);
            }
        } else if (wp == DGL_HOTKEY_SHOT) {
            marker_capture(s, true);
        }
        return 0;
    }
    if (msg == WM_PAINT && s) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        HBRUSH brush;
        char text[8192];
        const char *game = s->session_open ? s->process_name : "waiting for game";
        const char *gpu = s->gpu_name[0] ? s->gpu_name : "N/A";
        const char *driver = s->driver_name[0] ? s->driver_name : "N/A";
        const char *stack = s->graphics_stack[0] ? s->graphics_stack : "N/A";
        const char *wine = s->wine_version[0] ? s->wine_version : "N/A";
        const char *translator = s->translator[0] ? s->translator : "N/A";
        double one_low = s->frames.p99_ms > 0 ? 1000.0 / s->frames.p99_ms : 0.0;
        double point_low = s->frames.p999_ms > 0 ? 1000.0 / s->frames.p999_ms : 0.0;
        GetClientRect(hwnd, &r);
        brush = CreateSolidBrush(RGB(7, 7, 7));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 235, 235));
        SelectObject(dc, s->hud_font);
        snprintf(text, sizeof(text),
                 "DRIVE GPU LAB  %s  %s\r\n"
                 "GAME       %-34s PID %-8lu\r\n"
                 "GPU        %-34s\r\n"
                 "DRIVER     %-34s\r\n"
                 "STACK      %-34s\r\n"
                 "WINE       %-20s  TRANSLATOR %-18s\r\n"
                 "FPS        %7.2f   AVG %7.2f   1%%L %7.2f   0.1%%L %7.2f\r\n"
                 "FRAME      %7.2fms P95 %7.2f   P99 %7.2f   P99.9 %7.2f\r\n"
                 "CPU GAME   %7.2f%%  RAM %8.1f MB\r\n"
                 "GPU LOAD   %7.2f%%  CLOCK %8.1f MHz  TEMP %6.1f C\r\n"
                 "VK MEM     %8.1f MB  PEAK %8.1f MB\r\n"
                 "FRAMES     %-10llu SUBMITS %-8llu EVENTS %-10llu DROP %-6llu\r\n"
                 "MODE       %-10s F8 HUD  F9 MARK  F10 DEEP  F11 MARK+SHOT",
                 DGL_VERSION_STRING, s->session_open ? "REC *" : "IDLE",
                 game, (unsigned long)s->pid, gpu, driver, stack, wine, translator,
                 s->m.fps, s->frames.avg_fps, one_low, point_low,
                 s->m.frame_ms, s->frames.p95_ms, s->frames.p99_ms, s->frames.p999_ms,
                 s->m.process_cpu_pct, s->m.process_mem_mb,
                 s->m.gpu_busy_pct, (double)s->m.gpu_freq_hz / 1000000.0,
                 s->m.gpu_temp_mc > 0 ? (double)s->m.gpu_temp_mc / 1000.0 : 0.0,
                 (double)s->m.vk_allocated_bytes / (1024.0 * 1024.0),
                 (double)s->m.vk_peak_allocated_bytes / (1024.0 * 1024.0),
                 (unsigned long long)s->m.frames_seen,
                 (unsigned long long)s->m.submits_since_present,
                 (unsigned long long)s->udp_events,
                 (unsigned long long)s->udp_dropped,
                 s->deep_mode ? "DEEP" : "NORMAL");
        r.left += 12; r.top += 10;
        DrawTextA(dc, text, -1, &r, DT_LEFT | DT_TOP | DT_NOPREFIX);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int create_hud(probe_state *s) {
    WNDCLASSEXA wc;
    HINSTANCE inst = GetModuleHandleA(NULL);
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = (UINT)sizeof(wc);
    wc.lpfnWndProc = hud_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = DGL_HUD_CLASS;
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
    s->hud = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                             DGL_HUD_CLASS, "Drive GPU Lab", WS_POPUP,
                             18, 18, 960, 320, NULL, NULL, inst, NULL);
    if (!s->hud) return -1;
    SetLayeredWindowAttributes(s->hud, 0, 220, LWA_ALPHA);
    s->hud_font = CreateFontA(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    if (!s->hud_font) return -1;
    RegisterHotKey(s->hud, DGL_HOTKEY_TOGGLE, 0, VK_F8);
    RegisterHotKey(s->hud, DGL_HOTKEY_MARK, 0, VK_F9);
    RegisterHotKey(s->hud, DGL_HOTKEY_DEEP, 0, VK_F10);
    RegisterHotKey(s->hud, DGL_HOTKEY_SHOT, 0, VK_F11);
    s->hud_visible = true;
    ShowWindow(s->hud, SW_SHOWNOACTIVATE);
    UpdateWindow(s->hud);
    return 0;
}

static void pump_messages(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            if (g_state) g_state->one_session_only = true;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static int init_udp(probe_state *s) {
    WSADATA wd;
    struct sockaddr_in addr;
    u_long nb = 1;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
    s->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s->udp == INVALID_SOCKET) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)s->data_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s->udp, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
    ioctlsocket(s->udp, FIONBIO, &nb);
    memset(&s->host_control_addr, 0, sizeof(s->host_control_addr));
    s->host_control_addr.sin_family = AF_INET;
    s->host_control_addr.sin_port = htons((u_short)s->control_port);
    s->host_control_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return 0;
}

static void close_network_hud(probe_state *s) {
    if (s->udp != INVALID_SOCKET) closesocket(s->udp);
    s->udp = INVALID_SOCKET;
    if (s->hud) {
        UnregisterHotKey(s->hud, DGL_HOTKEY_TOGGLE);
        UnregisterHotKey(s->hud, DGL_HOTKEY_MARK);
        UnregisterHotKey(s->hud, DGL_HOTKEY_DEEP);
        UnregisterHotKey(s->hud, DGL_HOTKEY_SHOT);
        DestroyWindow(s->hud);
        s->hud = NULL;
    }
    if (s->hud_font) { DeleteObject(s->hud_font); s->hud_font = NULL; }
    WSACleanup();
}

static void default_lab_root(probe_state *s) {
    char profile[DGL_PATH_CAP];
    DWORD n = GetEnvironmentVariableA("USERPROFILE", profile, (DWORD)sizeof(profile));
    if (n > 0 && n < sizeof(profile))
        snprintf(s->lab_root, sizeof(s->lab_root), "%s\\Documents\\DriveGpuLab", profile);
    else
        snprintf(s->lab_root, sizeof(s->lab_root), ".\\DriveGpuLab");
}

static int prepare_launch_environment(probe_state *s) {
    char temp_root[DGL_PATH_CAP];
    char vkd3d[DGL_PATH_CAP];
    if (!s->launch_exe[0]) return -1;
    path_join(temp_root, sizeof(temp_root), s->lab_root, "launcher-logs");
    if (mkdir_tree(temp_root) != 0) return -1;
    snprintf(vkd3d, sizeof(vkd3d), "%s\\vkd3d-live.log", temp_root);
    SetEnvironmentVariableA("DXVK_LOG_PATH", temp_root);
    if (!GetEnvironmentVariableA("DXVK_LOG_LEVEL", vkd3d, (DWORD)sizeof(vkd3d)))
        SetEnvironmentVariableA("DXVK_LOG_LEVEL", "info");
    snprintf(vkd3d, sizeof(vkd3d), "%s\\vkd3d-live.log", temp_root);
    SetEnvironmentVariableA("VKD3D_LOG_FILE", vkd3d);
    if (!GetEnvironmentVariableA("VKD3D_DEBUG", temp_root, (DWORD)sizeof(temp_root)))
        SetEnvironmentVariableA("VKD3D_DEBUG", "warn");
    return 0;
}

static int launch_target(probe_state *s) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[DGL_PATH_CAP * 2];
    if (!s->launch_mode || !s->launch_exe[0]) return 0;
    (void)prepare_launch_environment(s);
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = (DWORD)sizeof(si);
    if (s->launch_cmdline[0]) snprintf(cmd, sizeof(cmd), "%s", s->launch_cmdline);
    else snprintf(cmd, sizeof(cmd), "\"%s\"", s->launch_exe);
    if (!CreateProcessA(s->launch_exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    s->requested_pid = pi.dwProcessId;
    return 1;
}

static void usage(void) {
    printf(
        "Drive GPU Lab / G720Probe %s\n"
        "Usage:\n"
        "  G720Probe.exe                         auto-detect games, stay resident\n"
        "  G720Probe.exe --process GAME.exe     attach to one named game\n"
        "  G720Probe.exe --pid N                attach to one PID\n"
        "  G720Probe.exe --launch PATH [args]   launch and capture\n"
        "Options:\n"
        "  --root PATH          stable DriveGpuLab data root\n"
        "  --stay-alive         return to auto wait after a captured game exits\n"
        "  --once               stop after one completed session\n"
        "  --data-port N        telemetry UDP port (default %d)\n"
        "  --control-port N     hostd control UDP port (default %d)\n"
        "Hotkeys: F8 HUD, F9 mark bug, F10 deep marker, F11 mark + screenshot\n",
        DGL_VERSION_STRING, DGL_DATA_PORT, DGL_CONTROL_PORT);
}

static void parse_args(probe_state *s, int argc, char **argv) {
    int i;
    if (argc == 1) {
        s->auto_mode = true;
        s->stay_alive = true;
        return;
    }
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--process") && i + 1 < argc) {
            snprintf(s->requested_process, sizeof(s->requested_process), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            s->requested_pid = (DWORD)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--auto")) {
            s->auto_mode = true;
        } else if (!strcmp(argv[i], "--stay-alive")) {
            s->stay_alive = true;
        } else if (!strcmp(argv[i], "--once")) {
            s->one_session_only = true;
        } else if (!strcmp(argv[i], "--root") && i + 1 < argc) {
            snprintf(s->lab_root, sizeof(s->lab_root), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--data-port") && i + 1 < argc) {
            s->data_port = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--control-port") && i + 1 < argc) {
            s->control_port = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--launch") && i + 1 < argc) {
            size_t used;
            s->launch_mode = true;
            snprintf(s->launch_exe, sizeof(s->launch_exe), "%s", argv[++i]);
            snprintf(s->launch_cmdline, sizeof(s->launch_cmdline), "\"%s\"", s->launch_exe);
            used = strlen(s->launch_cmdline);
            while (i + 1 < argc && used + strlen(argv[i + 1]) + 4 < sizeof(s->launch_cmdline)) {
                ++i;
                s->launch_cmdline[used++] = ' ';
                s->launch_cmdline[used] = '\0';
                strncat(s->launch_cmdline, argv[i], sizeof(s->launch_cmdline) - used - 1);
                used = strlen(s->launch_cmdline);
            }
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            ExitProcess(0);
        }
    }
    if (!s->requested_process[0] && !s->requested_pid && !s->launch_mode) s->auto_mode = true;
}

static int dgl_main(int argc, char **argv) {
    probe_state state;
    SYSTEM_INFO si;
    uint64_t now;
    bool quit = false;
    memset(&state, 0, sizeof(state));
    state.udp = INVALID_SOCKET;
    state.data_port = DGL_DATA_PORT;
    state.control_port = DGL_CONTROL_PORT;
    default_lab_root(&state);
    parse_args(&state, argc, argv);
    if (!state.lab_root[0]) default_lab_root(&state);
    if (state.data_port == 0 || state.data_port > 65535 ||
        state.control_port == 0 || state.control_port > 65535) {
        fprintf(stderr, "Invalid UDP port.\n");
        return 2;
    }
    if (mkdir_tree(state.lab_root) != 0) {
        fprintf(stderr, "Cannot create lab root: %s\n", state.lab_root);
        return 3;
    }
    GetSystemInfo(&si);
    state.cpu_count = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    state.m.gpu_temp_mc = -1;
    state.process_start_qpc_ns = qpc_ns();
    g_state = &state;

    if (init_udp(&state) != 0) {
        fprintf(stderr, "Cannot bind telemetry UDP port %u.\n", state.data_port);
        close_network_hud(&state);
        return 4;
    }
    if (create_hud(&state) != 0) {
        fprintf(stderr, "warning: HUD initialization failed; background capture remains available.\n");
    }
    if (state.launch_mode && launch_target(&state) < 0) {
        fprintf(stderr, "Failed to launch target.\n");
    }

    while (!quit) {
        now = qpc_ns();
        pump_messages();
        drain_udp(&state);
        if (!state.process) try_attach_target(&state);

        if (state.session_open) {
            if (now - state.last_tail_scan_qpc_ns >= DGL_TAIL_SCAN_NS) {
                discover_external_logs(&state);
                state.last_tail_scan_qpc_ns = now;
            }
            if (now - state.last_tail_poll_qpc_ns >= 100000000ull) {
                poll_external_tails(&state);
                state.last_tail_poll_qpc_ns = now;
            }
            if (now - state.last_module_scan_qpc_ns >= DGL_MODULE_SCAN_NS) {
                scan_modules(&state);
                write_manifest(&state, false);
                state.last_module_scan_qpc_ns = now;
            }
            if (now - state.last_sample_qpc_ns >= DGL_SAMPLE_NS) {
                sample_process(&state, now);
                if (state.session_open) write_telemetry(&state, now);
                if (state.hud) InvalidateRect(state.hud, NULL, FALSE);
                state.last_sample_qpc_ns = now;
            }
        } else {
            if (state.hud) InvalidateRect(state.hud, NULL, FALSE);
            if (state.sessions_completed > 0) {
                if (state.one_session_only || (!state.stay_alive && !state.auto_mode)) quit = true;
            }
        }

        if (state.one_session_only && state.sessions_completed > 0 && !state.session_open) quit = true;
        Sleep(5);
    }

    if (state.session_open) finalize_session(&state, "probe_exit");
    if (state.process) CloseHandle(state.process);
    close_network_hud(&state);
    dgl_ring_destroy(&state.ring);
    return 0;
}


int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR cmd_line, int show_cmd) {
    LPWSTR *wide_argv;
    int argc = 0, i, rc;
    char **argv;
    (void)instance;
    (void)prev_instance;
    (void)cmd_line;
    (void)show_cmd;

    wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv || argc <= 0) return 2;
    argv = (char **)calloc((size_t)argc + 1u, sizeof(*argv));
    if (!argv) {
        LocalFree(wide_argv);
        return 2;
    }
    for (i = 0; i < argc; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0) {
            argv[i] = (char *)calloc(1u, 1u);
            continue;
        }
        argv[i] = (char *)calloc((size_t)bytes, 1u);
        if (!argv[i] || WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, argv[i], bytes, NULL, NULL) <= 0) {
            free(argv[i]);
            argv[i] = (char *)calloc(1u, 1u);
        }
    }
    rc = dgl_main(argc, argv);
    for (i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    LocalFree(wide_argv);
    return rc;
}
