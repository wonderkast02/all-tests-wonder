// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static int dgl_strnicmp_local(const char *a, const char *b, size_t n) {
#ifdef _WIN32
    return _strnicmp(a, b, n);
#else
    return strncasecmp(a, b, n);
#endif
}

int dgl_contains_ci(const char *haystack, const char *needle) {
    size_t hay_len, needle_len, i;
    if (!haystack || !needle) return 0;
    hay_len = strlen(haystack);
    needle_len = strlen(needle);
    if (!needle_len || needle_len > hay_len) return 0;
    for (i = 0; i + needle_len <= hay_len; ++i) {
        if (dgl_strnicmp_local(haystack + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

void dgl_sanitize_key(const char *input, char *output, size_t capacity) {
    size_t i, j = 0;
    int last_sep = 0;
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!input) input = "";

    for (i = 0; input[i] && j + 1 < capacity; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (input[i] == '.' && dgl_strnicmp_local(input + i, ".exe", 4) == 0 &&
            input[i + 4] == '\0') {
            break;
        }
        if (isalnum(c)) {
            output[j++] = (char)tolower(c);
            last_sep = 0;
        } else if (!last_sep && j > 0) {
            output[j++] = '-';
            last_sep = 1;
        }
    }
    while (j > 0 && output[j - 1] == '-') --j;
    if (j == 0) {
        const char fallback[] = "unknown-game";
        snprintf(output, capacity, "%s", fallback);
    } else {
        output[j] = '\0';
    }
}

void dgl_iso8601_utc(char output[32]) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(output, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

void dgl_compact_local_stamp(char output[32]) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(output, 32, "%Y%m%d-%H%M%S", &tmv);
}

uint64_t dgl_monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
        return 0;
    }
    return (uint64_t)((long double)counter.QuadPart * 1000000000.0L /
                      (long double)freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static int dgl_mkdir_one(const char *path) {
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    return -1;
#endif
}

int dgl_mkdir_p(const char *path) {
    char tmp[4096];
    size_t i, n;
    if (!path || !path[0]) return -1;
    n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);

    for (i = 1; i < n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char keep = tmp[i];
#ifdef _WIN32
            if (i == 2 && tmp[1] == ':') continue;
#endif
            tmp[i] = '\0';
            if (tmp[0] && dgl_mkdir_one(tmp) != 0) return -1;
            tmp[i] = keep;
        }
    }
    return dgl_mkdir_one(tmp);
}

void dgl_json_escape(const char *input, char *output, size_t capacity) {
    size_t i, j = 0;
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!input) input = "";
    for (i = 0; input[i] && j + 1 < capacity; ++i) {
        unsigned char c = (unsigned char)input[i];
        const char *replacement = NULL;
        char unicode_escape[7];
        switch (c) {
            case '"': replacement = "\\\""; break;
            case '\\': replacement = "\\\\"; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default:
                if (c < 0x20) {
                    snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", c);
                    replacement = unicode_escape;
                }
                break;
        }
        if (replacement) {
            size_t rn = strlen(replacement);
            if (j + rn >= capacity) break;
            memcpy(output + j, replacement, rn);
            j += rn;
        } else {
            output[j++] = (char)c;
        }
    }
    output[j] = '\0';
}

int dgl_read_small_file(const char *path, char *output, size_t capacity) {
    FILE *f;
    size_t n;
    if (!path || !output || capacity < 2) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    n = fread(output, 1, capacity - 1, f);
    if (ferror(f)) {
        fclose(f);
        output[0] = '\0';
        return -2;
    }
    fclose(f);
    output[n] = '\0';
    return (int)n;
}

int dgl_find_ascii_token_in_file(const char *path, const char *needle,
                                 char *output, size_t capacity) {
    FILE *f;
    unsigned char buffer[65536 + 512];
    size_t n, carry = 0, needle_len;
    if (!path || !needle || !needle[0] || !output || capacity < 2) return -1;
    output[0] = '\0';
    needle_len = strlen(needle);
    f = fopen(path, "rb");
    if (!f) return -1;

    while ((n = fread(buffer + carry, 1, 65536, f)) > 0) {
        size_t total = carry + n;
        size_t i;
        for (i = 0; i + needle_len <= total; ++i) {
            if (memcmp(buffer + i, needle, needle_len) == 0) {
                size_t j = 0;
                while (i + j < total && j + 1 < capacity) {
                    unsigned char c = buffer[i + j];
                    if (c < 32 || c > 126) break;
                    output[j++] = (char)c;
                }
                output[j] = '\0';
                fclose(f);
                return 0;
            }
        }
        carry = total < 511 ? total : 511;
        memmove(buffer, buffer + total - carry, carry);
    }
    fclose(f);
    return -2;
}
