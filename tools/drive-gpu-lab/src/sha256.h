/* SPDX-License-Identifier: MIT */
#ifndef DGL_SHA256_H
#define DGL_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint8_t block[64];
    uint32_t state[8];
    uint64_t bitlen;
    size_t used;
} dgl_sha256_ctx;
void dgl_sha256_init(dgl_sha256_ctx *ctx);
void dgl_sha256_update(dgl_sha256_ctx *ctx, const uint8_t *data, size_t len);
void dgl_sha256_final(dgl_sha256_ctx *ctx, uint8_t out[32]);
int dgl_sha256_file(const char *path, char hex[65]);
#endif
