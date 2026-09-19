// SPDX-License-Identifier: MIT
#ifndef DGL_COMMON_H
#define DGL_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "dgl_protocol.h"

void dgl_sanitize_key(const char *input, char *output, size_t capacity);
void dgl_iso8601_utc(char output[32]);
void dgl_compact_local_stamp(char output[32]);
uint64_t dgl_monotonic_ns(void);
int dgl_mkdir_p(const char *path);
void dgl_json_escape(const char *input, char *output, size_t capacity);
int dgl_read_small_file(const char *path, char *output, size_t capacity);
int dgl_find_ascii_token_in_file(const char *path, const char *needle,
                                 char *output, size_t capacity);
int dgl_contains_ci(const char *haystack, const char *needle);

#endif
