// SPDX-License-Identifier: MIT
#ifndef DGL_RING_H
#define DGL_RING_H

#include <stddef.h>
#include <stdint.h>
#include "dgl_protocol.h"

typedef struct dgl_ring {
    char (*lines)[DGL_RING_LINE_BYTES];
    uint64_t *seq;
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t pushed;
    uint64_t overwritten;
} dgl_ring;

int dgl_ring_init(dgl_ring *r);
void dgl_ring_reset(dgl_ring *r);
void dgl_ring_destroy(dgl_ring *r);
void dgl_ring_push(dgl_ring *r, uint64_t seq, const char *line);
size_t dgl_ring_count(const dgl_ring *r);
const char *dgl_ring_get_oldest(const dgl_ring *r, size_t index, uint64_t *seq_out);

#endif
