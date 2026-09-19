// SPDX-License-Identifier: MIT
#include "dgl_ring.h"

#include <stdlib.h>
#include <string.h>

int dgl_ring_init(dgl_ring *r) {
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    r->lines = calloc(DGL_RING_CAPACITY, sizeof(*r->lines));
    r->seq = calloc(DGL_RING_CAPACITY, sizeof(*r->seq));
    if (!r->lines || !r->seq) {
        free(r->lines);
        free(r->seq);
        memset(r, 0, sizeof(*r));
        return -1;
    }
    r->capacity = DGL_RING_CAPACITY;
    return 0;
}

void dgl_ring_reset(dgl_ring *r) {
    if (!r || !r->lines || !r->seq || !r->capacity) return;
    r->head = 0;
    r->count = 0;
    r->pushed = 0;
    r->overwritten = 0;
}

void dgl_ring_destroy(dgl_ring *r) {
    if (!r) return;
    free(r->lines);
    free(r->seq);
    memset(r, 0, sizeof(*r));
}

void dgl_ring_push(dgl_ring *r, uint64_t seq, const char *line) {
    size_t slot;
    if (!r || !r->lines || !r->seq || !r->capacity || !line) return;
    slot = r->head;
    strncpy(r->lines[slot], line, DGL_RING_LINE_BYTES - 1u);
    r->lines[slot][DGL_RING_LINE_BYTES - 1u] = '\0';
    r->seq[slot] = seq;
    r->head = (r->head + 1u) % r->capacity;
    ++r->pushed;
    if (r->count < r->capacity) ++r->count;
    else ++r->overwritten;
}

size_t dgl_ring_count(const dgl_ring *r) {
    return r ? r->count : 0;
}

const char *dgl_ring_get_oldest(const dgl_ring *r, size_t index, uint64_t *seq_out) {
    size_t start, slot;
    if (!r || !r->lines || !r->seq || !r->capacity || index >= r->count) return NULL;
    start = (r->head + r->capacity - r->count) % r->capacity;
    slot = (start + index) % r->capacity;
    if (seq_out) *seq_out = r->seq[slot];
    return r->lines[slot];
}
