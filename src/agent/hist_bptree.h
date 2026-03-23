#pragma once

#include <aevos/types.h>

struct history;

typedef struct hist_bp_node hist_bp_node_t;

typedef struct hist_bptree {
    hist_bp_node_t *root;
    uint32_t        height;
} hist_bptree_t;

void hist_bpt_init(hist_bptree_t *t);
void hist_bpt_destroy(hist_bptree_t *t);
void hist_bpt_clear(hist_bptree_t *t);

int  hist_bpt_insert(hist_bptree_t *t, uint64_t seq, uint32_t ring_idx);
void hist_bpt_rebuild(hist_bptree_t *t, struct history *h);

uint32_t hist_bpt_range_from_seq(hist_bptree_t *t, uint64_t min_seq,
                                 uint32_t *out_ring_idx, uint32_t max_out);
