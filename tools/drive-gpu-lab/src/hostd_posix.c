// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "dgl_protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DGL_HOSTD_TEXT 256
#define DGL_SESSION_TEXT 192
#define DGL_JSON_NUM 64

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void sleep_ms(unsigned ms) {
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR && !g_stop) {}
}

static int join_path(char *out, size_t cap, const char *base, const char *leaf) {
    size_t a, b;
    if (!out || cap == 0 || !base || !leaf) return -1;
    a = strlen(base);
    b = strlen(leaf);
    if (a + 1u + b + 1u > cap) return -1;
    memcpy(out, base, a);
    out[a] = '/';
    memcpy(out + a + 1u, leaf, b + 1u);
    return 0;
}

static int read_trimmed(const char *path, char *buf, size_t cap) {
    int n = dgl_read_small_file(path, buf, cap);
    if (n < 0) return -1;
    while (n > 0 && isspace((unsigned char)buf[n - 1])) buf[--n] = '\0';
    return n > 0 ? 0 : -1;
}

static int read_u64(const char *path, uint64_t *value) {
    char buf[128];
    char *end = NULL;
    unsigned long long parsed;
    if (!value || read_trimmed(path, buf, sizeof(buf)) != 0) return -1;
    errno = 0;
    parsed = strtoull(buf, &end, 10);
    if (errno != 0 || end == buf) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int path_u64(const char *base, const char *leaf, uint64_t *value) {
    char path[PATH_MAX];
    if (!base || !base[0] || join_path(path, sizeof(path), base, leaf) != 0) return -1;
    return read_u64(path, value);
}

static int path_text(const char *base, const char *leaf, char *buf, size_t cap) {
    char path[PATH_MAX];
    if (!base || !base[0] || join_path(path, sizeof(path), base, leaf) != 0) return -1;
    return read_trimmed(path, buf, cap);
}

static int looks_like_gpu(const char *text) {
    return dgl_contains_ci(text, "mali") || dgl_contains_ci(text, "gpu") ||
           dgl_contains_ci(text, "g3d") || dgl_contains_ci(text, "panfrost");
}

static int find_gpu_devfreq(char *out, size_t cap) {
    static const char base[] = "/sys/class/devfreq";
    DIR *dir = opendir(base);
    struct dirent *entry;
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!dir) return -1;
    while ((entry = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        char name_path[PATH_MAX];
        char name[DGL_HOSTD_TEXT];
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (join_path(path, sizeof(path), base, entry->d_name) != 0) continue;
        if (looks_like_gpu(entry->d_name)) {
            if (strlen(path) + 1u <= cap) memcpy(out, path, strlen(path) + 1u);
            closedir(dir);
            return out[0] ? 0 : -1;
        }
        if (join_path(name_path, sizeof(name_path), path, "name") != 0) continue;
        if (read_trimmed(name_path, name, sizeof(name)) == 0 && looks_like_gpu(name)) {
            if (strlen(path) + 1u <= cap) memcpy(out, path, strlen(path) + 1u);
            closedir(dir);
            return out[0] ? 0 : -1;
        }
    }
    closedir(dir);
    return -1;
}

static int meminfo_value(const char *key, uint64_t *value) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    char found_key[96];
    unsigned long long v;
    if (!f || !key || !value) {
        if (f) fclose(f);
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%95[^:]: %llu kB", found_key, &v) == 2 && !strcmp(found_key, key)) {
            fclose(f);
            *value = (uint64_t)v;
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int read_gpu_temp_mc(int64_t *value) {
    int i, found = 0;
    int64_t best = -1;
    for (i = 0; i < 256; ++i) {
        char type_path[PATH_MAX];
        char temp_path[PATH_MAX];
        char type[256];
        uint64_t temp;
        if (snprintf(type_path, sizeof(type_path), "/sys/class/thermal/thermal_zone%d/type", i) >= (int)sizeof(type_path)) continue;
        if (read_trimmed(type_path, type, sizeof(type)) != 0 || !looks_like_gpu(type)) continue;
        if (snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/thermal_zone%d/temp", i) >= (int)sizeof(temp_path)) continue;
        if (read_u64(temp_path, &temp) == 0 && temp < 250000ull) {
            if (!found || (int64_t)temp > best) best = (int64_t)temp;
            found = 1;
        }
    }
    if (!found || !value) return -1;
    *value = best;
    return 0;
}

static int read_soc_temp_mc(int64_t *value) {
    int i, found = 0;
    int64_t best = -1;
    for (i = 0; i < 256; ++i) {
        char temp_path[PATH_MAX];
        uint64_t temp;
        if (snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/thermal_zone%d/temp", i) >= (int)sizeof(temp_path)) continue;
        if (read_u64(temp_path, &temp) == 0 && temp < 250000ull) {
            if (!found || (int64_t)temp > best) best = (int64_t)temp;
            found = 1;
        }
    }
    if (!found || !value) return -1;
    *value = best;
    return 0;
}

typedef struct cpu_sample {
    uint64_t busy;
    uint64_t total;
    int valid;
} cpu_sample;

static cpu_sample read_cpu_sample(void) {
    FILE *f = fopen("/proc/stat", "r");
    cpu_sample s = {0, 0, 0};
    unsigned long long user, nicev, systemv, idle, iowait, irq, softirq, steal;
    if (!f) return s;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nicev, &systemv, &idle, &iowait, &irq, &softirq, &steal) == 8) {
        uint64_t idle_all = (uint64_t)idle + (uint64_t)iowait;
        s.total = (uint64_t)user + (uint64_t)nicev + (uint64_t)systemv + idle_all +
                  (uint64_t)irq + (uint64_t)softirq + (uint64_t)steal;
        s.busy = s.total - idle_all;
        s.valid = 1;
    }
    fclose(f);
    return s;
}

static int run_first_line(const char *command, char *out, size_t cap) {
    FILE *pipe;
    size_t n;
    if (!command || !out || cap < 2u) return -1;
    out[0] = '\0';
    pipe = popen(command, "r");
    if (!pipe) return -1;
    if (!fgets(out, (int)cap, pipe)) {
        (void)pclose(pipe);
        return -1;
    }
    (void)pclose(pipe);
    n = strlen(out);
    while (n && isspace((unsigned char)out[n - 1])) out[--n] = '\0';
    return n ? 0 : -1;
}

static void read_kbase_version(char *out, size_t cap) {
    static const char *paths[] = {
        "/sys/module/mali_kbase/version",
        "/sys/module/mali/version",
        "/sys/module/panfrost/version",
        NULL
    };
    int i;
    if (!out || cap == 0) return;
    out[0] = '\0';
    for (i = 0; paths[i]; ++i) if (read_trimmed(paths[i], out, cap) == 0) return;
}

static const char *json_find_value(const char *json, const char *key) {
    char needle[128];
    const char *p;
    if (!json || !key || snprintf(needle, sizeof(needle), "\"%s\":", key) >= (int)sizeof(needle)) return NULL;
    p = strstr(json, needle);
    return p ? p + strlen(needle) : NULL;
}

static void json_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json_find_value(json, key);
    size_t n = 0;
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!p) return;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p++ != '"') return;
    while (*p && *p != '"' && n + 1u < cap) {
        if (*p == '\\' && p[1]) ++p;
        out[n++] = *p++;
    }
    out[n] = '\0';
}

static uint64_t json_u64(const char *json, const char *key, uint64_t fallback) {
    const char *p = json_find_value(json, key);
    char *end = NULL;
    unsigned long long v;
    if (!p) return fallback;
    while (*p == ' ' || *p == '\t') ++p;
    if (!strncmp(p, "null", 4) || *p == '-') return fallback;
    errno = 0;
    v = strtoull(p, &end, 10);
    if (end == p || errno == ERANGE) return fallback;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end && *end != ',' && *end != '}' && *end != ']') return fallback;
    return (uint64_t)v;
}

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void format_u64_json(char out[DGL_JSON_NUM], int valid, uint64_t value) {
    if (!valid) (void)snprintf(out, DGL_JSON_NUM, "null");
    else (void)snprintf(out, DGL_JSON_NUM, "%llu", (unsigned long long)value);
}

static void format_i64_json(char out[DGL_JSON_NUM], int valid, int64_t value) {
    if (!valid) (void)snprintf(out, DGL_JSON_NUM, "null");
    else (void)snprintf(out, DGL_JSON_NUM, "%lld", (long long)value);
}

static void format_double_json(char out[DGL_JSON_NUM], int valid, double value) {
    if (!valid) (void)snprintf(out, DGL_JSON_NUM, "null");
    else (void)snprintf(out, DGL_JSON_NUM, "%.3f", value);
}

static void emit_json(int data_sock, const struct sockaddr_in *dst, FILE *logf,
                      int mirror_stdout, const char *json) {
    if (!json) return;
    if (data_sock >= 0 && dst) {
        (void)sendto(data_sock, json, strlen(json), 0,
                     (const struct sockaddr *)dst, sizeof(*dst));
    }
    if (mirror_stdout) {
        puts(json);
        fflush(stdout);
    }
    if (logf) {
        fprintf(logf, "%s\n", json);
        fflush(logf);
    }
}

static void emit_identity(int data_sock, const struct sockaddr_in *dst, FILE *logf,
                          int mirror_stdout, uint64_t *seq, const char *gpu_path,
                          const char *session_id, const char *game_key, uint64_t target_pid) {
    struct utsname un;
    char model[DGL_HOSTD_TEXT] = "";
    char android[DGL_HOSTD_TEXT] = "";
    char fingerprint[DGL_HOSTD_TEXT] = "";
    char kbase[DGL_HOSTD_TEXT] = "";
    char governor[DGL_HOSTD_TEXT] = "";
    char available[DGL_HOSTD_TEXT] = "";
    char e_model[DGL_HOSTD_TEXT * 2], e_android[DGL_HOSTD_TEXT * 2];
    char e_fp[DGL_HOSTD_TEXT * 2], e_kbase[DGL_HOSTD_TEXT * 2];
    char e_gpu[1024], e_gov[DGL_HOSTD_TEXT * 2], e_avail[DGL_HOSTD_TEXT * 2];
    char e_session[DGL_SESSION_TEXT * 2], e_game[DGL_SESSION_TEXT * 2];
    char minf_json[DGL_JSON_NUM], maxf_json[DGL_JSON_NUM];
    char json[DGL_MAX_EVENT_BYTES];
    uint64_t minf = 0, maxf = 0;
    int have_min, have_max;

    memset(&un, 0, sizeof(un));
    (void)uname(&un);
    (void)run_first_line("getprop ro.product.model 2>/dev/null", model, sizeof(model));
    (void)run_first_line("getprop ro.build.version.release 2>/dev/null", android, sizeof(android));
    (void)run_first_line("getprop ro.build.fingerprint 2>/dev/null", fingerprint, sizeof(fingerprint));
    read_kbase_version(kbase, sizeof(kbase));
    (void)path_text(gpu_path, "governor", governor, sizeof(governor));
    (void)path_text(gpu_path, "available_frequencies", available, sizeof(available));
    have_min = path_u64(gpu_path, "min_freq", &minf) == 0;
    have_max = path_u64(gpu_path, "max_freq", &maxf) == 0;
    format_u64_json(minf_json, have_min, minf);
    format_u64_json(maxf_json, have_max, maxf);

    dgl_json_escape(model, e_model, sizeof(e_model));
    dgl_json_escape(android, e_android, sizeof(e_android));
    dgl_json_escape(fingerprint, e_fp, sizeof(e_fp));
    dgl_json_escape(kbase, e_kbase, sizeof(e_kbase));
    dgl_json_escape(gpu_path ? gpu_path : "", e_gpu, sizeof(e_gpu));
    dgl_json_escape(governor, e_gov, sizeof(e_gov));
    dgl_json_escape(available, e_avail, sizeof(e_avail));
    dgl_json_escape(session_id ? session_id : "", e_session, sizeof(e_session));
    dgl_json_escape(game_key ? game_key : "", e_game, sizeof(e_game));

    (void)snprintf(json, sizeof(json),
             "{\"v\":%d,\"source\":\"hostd\",\"type\":\"identity\",\"seq\":%llu,"
             "\"mono_ns\":%llu,\"hostd_version\":\"%s\",\"session_id\":\"%s\","
             "\"game_key\":\"%s\",\"target_pid\":%llu,\"kernel_sysname\":\"%s\","
             "\"kernel_release\":\"%s\",\"machine\":\"%s\",\"android_model\":\"%s\","
             "\"android_release\":\"%s\",\"android_fingerprint\":\"%s\","
             "\"kbase_version\":\"%s\",\"mali0_exists\":%s,\"mali0_readable\":%s,"
             "\"mali0_writable\":%s,\"gpu_devfreq_path\":\"%s\",\"gpu_governor\":\"%s\","
             "\"gpu_available_frequencies\":\"%s\",\"gpu_min_freq_hz\":%s,"
             "\"gpu_max_freq_hz\":%s}",
             DGL_PROTOCOL_VERSION, (unsigned long long)++(*seq),
             (unsigned long long)dgl_monotonic_ns(), DGL_VERSION_STRING, e_session, e_game,
             (unsigned long long)target_pid, un.sysname, un.release, un.machine, e_model, e_android,
             e_fp, e_kbase,
             access("/dev/mali0", F_OK) == 0 ? "true" : "false",
             access("/dev/mali0", R_OK) == 0 ? "true" : "false",
             access("/dev/mali0", W_OK) == 0 ? "true" : "false",
             e_gpu, e_gov, e_avail, minf_json, maxf_json);
    emit_json(data_sock, dst, logf, mirror_stdout, json);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Drive GPU Lab host daemon %s\n"
            "Usage: %s [--data-port N] [--control-port N] [--interval-ms N]\n"
            "          [--once] [--stdout] [--log FILE] [--no-control]\n",
            DGL_VERSION_STRING, argv0);
}

int main(int argc, char **argv) {
    unsigned data_port = DGL_DATA_PORT;
    unsigned control_port = DGL_CONTROL_PORT;
    unsigned interval_ms = 500;
    int once = 0, mirror_stdout = 0, no_control = 0;
    const char *log_path = NULL;
    FILE *logf = NULL;
    int data_sock = -1, control_sock = -1;
    struct sockaddr_in data_dst;
    struct sockaddr_in control_addr;
    char gpu_path[PATH_MAX] = {0};
    char session_id[DGL_SESSION_TEXT] = "";
    char game_key[DGL_SESSION_TEXT] = "";
    uint64_t target_pid = 0;
    uint64_t seq = 0;
    uint64_t prev_gpu_busy = 0, prev_gpu_total = 0;
    int have_prev_gpu_busy = 0;
    cpu_sample prev_cpu = {0, 0, 0};
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data-port") && i + 1 < argc)
            data_port = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--control-port") && i + 1 < argc)
            control_port = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--interval-ms") && i + 1 < argc)
            interval_ms = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--stdout")) mirror_stdout = 1;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(argv[i], "--no-control")) no_control = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!data_port || data_port > 65535 || !control_port || control_port > 65535 ||
        interval_ms < 50 || interval_ms > 60000) {
        fprintf(stderr, "Invalid port or interval.\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (log_path) {
        logf = fopen(log_path, "a");
        if (!logf) fprintf(stderr, "warning: cannot open log file: %s\n", log_path);
    }

    data_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (data_sock < 0) {
        perror("data socket");
        if (logf) fclose(logf);
        return 3;
    }
    memset(&data_dst, 0, sizeof(data_dst));
    data_dst.sin_family = AF_INET;
    data_dst.sin_port = htons((uint16_t)data_port);
    data_dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (!no_control) {
        int yes = 1;
        control_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (control_sock >= 0) {
            (void)setsockopt(control_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            memset(&control_addr, 0, sizeof(control_addr));
            control_addr.sin_family = AF_INET;
            control_addr.sin_port = htons((uint16_t)control_port);
            control_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(control_sock, (struct sockaddr *)&control_addr, sizeof(control_addr)) != 0 ||
                make_nonblocking(control_sock) != 0) {
                close(control_sock);
                control_sock = -1;
            }
        }
    }

    (void)find_gpu_devfreq(gpu_path, sizeof(gpu_path));
    emit_identity(data_sock, &data_dst, logf, mirror_stdout, &seq, gpu_path,
                  session_id, game_key, target_pid);

    do {
        uint64_t now = dgl_monotonic_ns();
        uint64_t freq = 0, minf = 0, maxf = 0, busy = 0, total = 0, load = 0;
        uint64_t mem_total = 0, mem_avail = 0, swap_free = 0;
        int64_t gpu_temp = -1, soc_temp = -1;
        int have_freq, have_min, have_max, have_gpu_temp, have_soc_temp;
        int have_mem_total, have_mem_avail, have_swap_free, have_busy = 0;
        double busy_pct = -1.0, cpu_pct = -1.0;
        cpu_sample cpu = read_cpu_sample();
        char governor[DGL_HOSTD_TEXT] = "";
        char escaped_governor[DGL_HOSTD_TEXT * 2];
        char json[DGL_MAX_EVENT_BYTES];
        char freq_json[DGL_JSON_NUM], minf_json[DGL_JSON_NUM], maxf_json[DGL_JSON_NUM];
        char busy_json[DGL_JSON_NUM], gpu_temp_json[DGL_JSON_NUM], soc_temp_json[DGL_JSON_NUM];
        char mem_total_json[DGL_JSON_NUM], mem_avail_json[DGL_JSON_NUM], swap_free_json[DGL_JSON_NUM];
        char cpu_json[DGL_JSON_NUM];

        if (!gpu_path[0]) (void)find_gpu_devfreq(gpu_path, sizeof(gpu_path));
        have_freq = path_u64(gpu_path, "cur_freq", &freq) == 0;
        have_min = path_u64(gpu_path, "min_freq", &minf) == 0;
        have_max = path_u64(gpu_path, "max_freq", &maxf) == 0;
        (void)path_text(gpu_path, "governor", governor, sizeof(governor));
        dgl_json_escape(governor, escaped_governor, sizeof(escaped_governor));
        have_gpu_temp = read_gpu_temp_mc(&gpu_temp) == 0;
        have_soc_temp = read_soc_temp_mc(&soc_temp) == 0;
        have_mem_total = meminfo_value("MemTotal", &mem_total) == 0;
        have_mem_avail = meminfo_value("MemAvailable", &mem_avail) == 0;
        have_swap_free = meminfo_value("SwapFree", &swap_free) == 0;

        if (path_u64(gpu_path, "load", &load) == 0 && load <= 100) {
            busy_pct = (double)load;
            have_busy = 1;
        } else if (path_u64(gpu_path, "utilization", &load) == 0 && load <= 100) {
            busy_pct = (double)load;
            have_busy = 1;
        } else if (path_u64(gpu_path, "busy_time", &busy) == 0 &&
                   path_u64(gpu_path, "total_time", &total) == 0) {
            if (have_prev_gpu_busy && total > prev_gpu_total && busy >= prev_gpu_busy) {
                busy_pct = 100.0 * (double)(busy - prev_gpu_busy) / (double)(total - prev_gpu_total);
                if (busy_pct < 0.0) busy_pct = 0.0;
                if (busy_pct > 100.0) busy_pct = 100.0;
                have_busy = 1;
            }
            prev_gpu_busy = busy;
            prev_gpu_total = total;
            have_prev_gpu_busy = 1;
        }

        if (cpu.valid && prev_cpu.valid && cpu.total > prev_cpu.total && cpu.busy >= prev_cpu.busy) {
            cpu_pct = 100.0 * (double)(cpu.busy - prev_cpu.busy) /
                      (double)(cpu.total - prev_cpu.total);
        }
        prev_cpu = cpu;

        format_u64_json(freq_json, have_freq, freq);
        format_u64_json(minf_json, have_min, minf);
        format_u64_json(maxf_json, have_max, maxf);
        format_double_json(busy_json, have_busy, busy_pct);
        format_i64_json(gpu_temp_json, have_gpu_temp, gpu_temp);
        format_i64_json(soc_temp_json, have_soc_temp, soc_temp);
        format_u64_json(mem_total_json, have_mem_total, mem_total);
        format_u64_json(mem_avail_json, have_mem_avail, mem_avail);
        format_u64_json(swap_free_json, have_swap_free, swap_free);
        format_double_json(cpu_json, cpu_pct >= 0.0, cpu_pct);

        (void)snprintf(json, sizeof(json),
                 "{\"v\":%d,\"source\":\"hostd\",\"type\":\"telemetry\",\"seq\":%llu,"
                 "\"mono_ns\":%llu,\"gpu_freq_hz\":%s,\"gpu_min_freq_hz\":%s,"
                 "\"gpu_max_freq_hz\":%s,\"gpu_busy_pct\":%s,\"gpu_temp_mC\":%s,"
                 "\"soc_temp_mC\":%s,\"mem_total_kb\":%s,\"mem_available_kb\":%s,"
                 "\"swap_free_kb\":%s,\"cpu_busy_pct\":%s,\"gpu_governor\":\"%s\"}",
                 DGL_PROTOCOL_VERSION, (unsigned long long)++seq, (unsigned long long)now,
                 freq_json, minf_json, maxf_json, busy_json, gpu_temp_json, soc_temp_json,
                 mem_total_json, mem_avail_json, swap_free_json, cpu_json, escaped_governor);
        emit_json(data_sock, &data_dst, logf, mirror_stdout, json);

        if (control_sock >= 0) {
            for (;;) {
                char ctrl[DGL_MAX_EVENT_BYTES + 1];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                ssize_t n = recvfrom(control_sock, ctrl, DGL_MAX_EVENT_BYTES, 0,
                                     (struct sockaddr *)&from, &from_len);
                (void)from;
                if (n < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        /* Control failure is non-fatal; telemetry keeps running. */
                    }
                    break;
                }
                ctrl[n] = '\0';
                {
                    char type[64];
                    char new_session[DGL_SESSION_TEXT];
                    char new_game[DGL_SESSION_TEXT];
                    json_string(ctrl, "type", type, sizeof(type));
                    if (!strcmp(type, "session_hello") || !strcmp(type, "capture_mode")) {
                        json_string(ctrl, "session_id", new_session, sizeof(new_session));
                        json_string(ctrl, "game_key", new_game, sizeof(new_game));
                        if (new_session[0]) (void)snprintf(session_id, sizeof(session_id), "%s", new_session);
                        if (new_game[0]) (void)snprintf(game_key, sizeof(game_key), "%s", new_game);
                        target_pid = json_u64(ctrl, "pid", target_pid);
                        emit_identity(data_sock, &data_dst, logf, mirror_stdout, &seq, gpu_path,
                                      session_id, game_key, target_pid);
                    }
                }
            }
        }

        if (once) break;
        sleep_ms(interval_ms);
    } while (!g_stop);

    if (control_sock >= 0) close(control_sock);
    if (data_sock >= 0) close(data_sock);
    if (logf) fclose(logf);
    return 0;
}
