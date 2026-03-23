#include "hist_bptree.h"
#include "history.h"
#include "kernel/mm/slab.h"
#include "lib/string.h"

#define BP_MAX 3

struct hist_bp_node {
    bool                is_leaf;
    uint8_t             nkeys;
    uint64_t            keys[BP_MAX];
    uint32_t            vals[BP_MAX];
    struct hist_bp_node *parent;
    struct hist_bp_node *children[BP_MAX + 1];
    struct hist_bp_node *next;
};

static hist_bp_node_t *node_alloc(bool leaf)
{
    hist_bp_node_t *n = (hist_bp_node_t *)kcalloc(1, sizeof(hist_bp_node_t));
    if (n)
        n->is_leaf = leaf;
    return n;
}

static void node_free_recursive(hist_bp_node_t *n)
{
    if (!n)
        return;
    if (!n->is_leaf) {
        for (uint32_t i = 0; i <= (uint32_t)n->nkeys; i++)
            node_free_recursive(n->children[i]);
    }
    kfree(n);
}

void hist_bpt_init(hist_bptree_t *t)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
}

void hist_bpt_destroy(hist_bptree_t *t)
{
    if (!t)
        return;
    node_free_recursive(t->root);
    t->root = NULL;
    t->height = 0;
}

void hist_bpt_clear(hist_bptree_t *t)
{
    hist_bpt_destroy(t);
}

static hist_bp_node_t *leaf_search(hist_bp_node_t *root, uint64_t key)
{
    hist_bp_node_t *x = root;
    while (x && !x->is_leaf) {
        int i = 0;
        while (i < (int)x->nkeys && key >= x->keys[i])
            i++;
        x = x->children[i];
    }
    return x;
}

static void leaf_insert_sorted(hist_bp_node_t *leaf, uint64_t key, uint32_t val)
{
    int i = (int)leaf->nkeys - 1;
    while (i >= 0 && leaf->keys[i] > key) {
        leaf->keys[i + 1]  = leaf->keys[i];
        leaf->vals[i + 1]  = leaf->vals[i];
        i--;
    }
    leaf->keys[i + 1] = key;
    leaf->vals[i + 1] = val;
    leaf->nkeys++;
}

static void internal_insert(hist_bp_node_t *in, int pos, uint64_t key,
                            hist_bp_node_t *new_child)
{
    for (int i = (int)in->nkeys; i > pos; i--) {
        in->keys[i]         = in->keys[i - 1];
        in->children[i + 1] = in->children[i];
    }
    in->keys[pos]           = key;
    in->children[pos + 1]   = new_child;
    new_child->parent       = in;
    in->nkeys++;
}

/* Split full leaf (nkeys == BP_MAX before insert — caller inserts first so nkeys==BP_MAX+1) */
static hist_bp_node_t *split_leaf_node(hist_bp_node_t *leaf)
{
    hist_bp_node_t *R = node_alloc(true);
    if (!R)
        return NULL;

    uint32_t total = (uint32_t)leaf->nkeys;
    uint32_t mid   = total / 2;
    R->nkeys = (uint8_t)(total - mid);
    for (uint32_t i = 0; i < R->nkeys; i++) {
        R->keys[i] = leaf->keys[mid + i];
        R->vals[i] = leaf->vals[mid + i];
    }
    leaf->nkeys = (uint8_t)mid;

    R->next      = leaf->next;
    leaf->next   = R;
    R->parent    = leaf->parent;
    return R;
}

/* Split internal with nkeys == BP_MAX after insert → BP_MAX+1 keys, BP_MAX+2 children */
static void split_internal_node(hist_bp_node_t *in, uint64_t *out_promo,
                                hist_bp_node_t **out_right)
{
    uint32_t nk   = (uint32_t)in->nkeys;
    uint32_t midk = nk / 2;
    *out_promo    = in->keys[midk];

    hist_bp_node_t *R = node_alloc(false);
    if (!R) {
        *out_right = NULL;
        return;
    }

    uint32_t rn = nk - midk - 1;
    R->nkeys    = (uint8_t)rn;
    uint32_t j  = 0;
    for (uint32_t i = midk + 1; i < nk; i++) {
        R->keys[j]         = in->keys[i];
        R->children[j]     = in->children[i];
        R->children[j]->parent = R;
        j++;
    }
    R->children[j]     = in->children[nk];
    R->children[j]->parent = R;
    R->parent          = in->parent;

    in->nkeys = (uint8_t)midk;
    *out_right = R;
}

int hist_bpt_insert(hist_bptree_t *t, uint64_t seq, uint32_t ring_idx)
{
    if (!t)
        return -EINVAL;

    if (!t->root) {
        hist_bp_node_t *L = node_alloc(true);
        if (!L)
            return -ENOMEM;
        L->keys[0] = seq;
        L->vals[0] = ring_idx;
        L->nkeys   = 1;
        t->root    = L;
        t->height  = 1;
        return 0;
    }

    hist_bp_node_t *leaf = leaf_search(t->root, seq);
    if (!leaf)
        return -EINVAL;

    for (uint32_t i = 0; i < (uint32_t)leaf->nkeys; i++) {
        if (leaf->keys[i] == seq) {
            leaf->vals[i] = ring_idx;
            return 0;
        }
    }

    if (leaf->nkeys < BP_MAX) {
        leaf_insert_sorted(leaf, seq, ring_idx);
        return 0;
    }

    leaf_insert_sorted(leaf, seq, ring_idx);
    hist_bp_node_t *newR = split_leaf_node(leaf);
    if (!newR)
        return -ENOMEM;
    uint64_t sep = newR->keys[0];

    hist_bp_node_t *left = leaf;
    hist_bp_node_t *parent = leaf->parent;
    while (parent) {
        int pos = 0;
        while (pos < (int)parent->nkeys && parent->keys[pos] <= sep)
            pos++;

        if (parent->nkeys < BP_MAX) {
            internal_insert(parent, pos, sep, newR);
            return 0;
        }

        internal_insert(parent, pos, sep, newR);

        uint64_t        promo;
        hist_bp_node_t *newIR;
        split_internal_node(parent, &promo, &newIR);
        if (!newIR)
            return -ENOMEM;
        left   = parent;
        sep    = promo;
        newR   = newIR;
        parent = parent->parent;
    }

    hist_bp_node_t *nr = node_alloc(false);
    if (!nr)
        return -ENOMEM;
    nr->keys[0]     = sep;
    nr->children[0] = left;
    nr->children[1] = newR;
    nr->nkeys       = 1;
    left->parent    = nr;
    newR->parent    = nr;
    t->root         = nr;
    t->height++;
    return 0;
}

void hist_bpt_rebuild(hist_bptree_t *t, struct history *h)
{
    if (!t || !h)
        return;
    hist_bpt_clear(t);
    uint32_t total = h->head - h->tail;
    for (uint32_t i = 0; i < total; i++) {
        uint32_t abs_idx = (h->tail + i) % HIST_RING_SIZE;
        hist_entry_t *e  = &h->ring[abs_idx];
        if (e->data)
            hist_bpt_insert(t, e->seq, abs_idx);
    }
}

static hist_bp_node_t *leftmost_leaf(hist_bp_node_t *r)
{
    while (r && !r->is_leaf)
        r = r->children[0];
    return r;
}

uint32_t hist_bpt_range_from_seq(hist_bptree_t *t, uint64_t min_seq,
                                 uint32_t *out_ring_idx, uint32_t max_out)
{
    if (!t || !t->root || !out_ring_idx || max_out == 0)
        return 0;

    hist_bp_node_t *L = leftmost_leaf(t->root);
    uint32_t        w = 0;
    while (L) {
        for (uint32_t i = 0; i < (uint32_t)L->nkeys; i++) {
            if (L->keys[i] >= min_seq && w < max_out)
                out_ring_idx[w++] = L->vals[i];
        }
        L = L->next;
    }
    return w;
}
