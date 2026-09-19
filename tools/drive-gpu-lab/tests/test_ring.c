// SPDX-License-Identifier: MIT
#include "dgl_ring.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    dgl_ring r;
    uint64_t seq = 0;
    const char *s;
    if (dgl_ring_init(&r) != 0) return 10;
    dgl_ring_push(&r, 11, "alpha");
    dgl_ring_push(&r, 12, "beta");
    if (dgl_ring_count(&r) != 2) { dgl_ring_destroy(&r); return 1; }
    s = dgl_ring_get_oldest(&r, 0, &seq);
    if (!s || strcmp(s, "alpha") || seq != 11) { dgl_ring_destroy(&r); return 2; }
    s = dgl_ring_get_oldest(&r, 1, &seq);
    if (!s || strcmp(s, "beta") || seq != 12) { dgl_ring_destroy(&r); return 3; }
    dgl_ring_reset(&r);
    if (dgl_ring_count(&r) != 0) { dgl_ring_destroy(&r); return 4; }
    dgl_ring_destroy(&r);
    puts("test_ring: PASS");
    return 0;
}
