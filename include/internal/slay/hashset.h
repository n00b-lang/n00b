#pragma once

/**
 * @file hashset.h
 * @internal
 * @brief Open-addressing pointer-identity hash set.
 *
 * GC-managed via n00b_alloc() — no arena parameter needed.
 */

#include "slay/types.h"

struct n00b_hashset_t {
    void   **buckets;
    int32_t  cap;
    int32_t  len;
    bool     shared;
};

n00b_hashset_t *n00b_hashset_new(int32_t cap);
bool            n00b_hashset_add(n00b_hashset_t *s, void *item);
void            n00b_hashset_put(n00b_hashset_t *s, void *item);
bool            n00b_hashset_contains(n00b_hashset_t *s, void *item);
n00b_hashset_t *n00b_hashset_copy(n00b_hashset_t *s);
n00b_hashset_t *n00b_hashset_share(n00b_hashset_t *s);
n00b_hashset_t *n00b_hashset_union(n00b_hashset_t *a, n00b_hashset_t *b);
void            n00b_hashset_free(n00b_hashset_t *s);
