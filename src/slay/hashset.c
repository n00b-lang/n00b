// hashset.c - Pointer-identity hash set using n00b allocator.
#include "internal/slay/hashset.h"
#include "core/alloc.h"
#include <string.h>

#define HS_TOMBSTONE ((void *)(uintptr_t)1)

static uint32_t
ptr_hash(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = ((v >> 16) ^ v) * 0x45d9f3b;
    v = ((v >> 16) ^ v) * 0x45d9f3b;
    v = (v >> 16) ^ v;
    return (uint32_t)v;
}

n00b_hashset_t *
n00b_hashset_new(int32_t cap)
{
    if (cap < 8) {
        cap = 8;
    }

    // Round up to power of 2.
    int32_t actual = 8;
    while (actual < cap) {
        actual *= 2;
    }

    n00b_hashset_t *s = n00b_alloc(n00b_hashset_t);
    s->buckets        = n00b_alloc_array(void *, actual);
    s->cap            = actual;
    s->len            = 0;
    s->shared         = false;

    return s;
}

static void
hashset_grow(n00b_hashset_t *s)
{
    int32_t  new_cap     = s->cap * 2;
    void   **new_buckets = n00b_alloc_array(void *, new_cap);
    int32_t  mask        = new_cap - 1;

    for (int32_t i = 0; i < s->cap; i++) {
        void *item = s->buckets[i];

        if (item && item != HS_TOMBSTONE) {
            uint32_t h   = ptr_hash(item);
            int32_t  idx = (int32_t)(h & (uint32_t)mask);

            while (new_buckets[idx]) {
                idx = (idx + 1) & mask;
            }

            new_buckets[idx] = item;
        }
    }

    n00b_free(s->buckets);
    s->buckets = new_buckets;
    s->cap     = new_cap;
}

static void
ensure_writable(n00b_hashset_t *s)
{
    if (s->shared) {
        void  **old = s->buckets;
        int32_t cap = s->cap;

        s->buckets = n00b_alloc_array(void *, cap);
        memcpy(s->buckets, old, (size_t)cap * sizeof(void *));
        s->shared = false;
    }
}

bool
n00b_hashset_add(n00b_hashset_t *s, void *item)
{
    if (!item || item == HS_TOMBSTONE) {
        return false;
    }

    if (s->len * 4 >= s->cap * 3) {
        ensure_writable(s);
        hashset_grow(s);
    }
    else {
        ensure_writable(s);
    }

    int32_t  mask = s->cap - 1;
    uint32_t h    = ptr_hash(item);
    int32_t  idx  = (int32_t)(h & (uint32_t)mask);

    while (s->buckets[idx]) {
        if (s->buckets[idx] == item) {
            return false;
        }
        if (s->buckets[idx] == HS_TOMBSTONE) {
            break;
        }
        idx = (idx + 1) & mask;
    }

    s->buckets[idx] = item;
    s->len++;
    return true;
}

void
n00b_hashset_put(n00b_hashset_t *s, void *item)
{
    n00b_hashset_add(s, item);
}

bool
n00b_hashset_contains(n00b_hashset_t *s, void *item)
{
    if (!s || !item || item == HS_TOMBSTONE) {
        return false;
    }

    int32_t  mask = s->cap - 1;
    uint32_t h    = ptr_hash(item);
    int32_t  idx  = (int32_t)(h & (uint32_t)mask);

    while (s->buckets[idx]) {
        if (s->buckets[idx] == item) {
            return true;
        }
        idx = (idx + 1) & mask;
    }

    return false;
}

n00b_hashset_t *
n00b_hashset_copy(n00b_hashset_t *s)
{
    n00b_hashset_t *c = n00b_alloc(n00b_hashset_t);

    c->cap    = s->cap;
    c->len    = s->len;
    c->shared = false;
    c->buckets = n00b_alloc_array(void *, s->cap);
    memcpy(c->buckets, s->buckets, (size_t)s->cap * sizeof(void *));

    return c;
}

n00b_hashset_t *
n00b_hashset_share(n00b_hashset_t *s)
{
    if (!s) {
        return n00b_hashset_new(8);
    }

    n00b_hashset_t *c = n00b_alloc(n00b_hashset_t);

    c->cap     = s->cap;
    c->len     = s->len;
    c->shared  = true;
    c->buckets = s->buckets;
    s->shared  = true;

    return c;
}

n00b_hashset_t *
n00b_hashset_union(n00b_hashset_t *a, n00b_hashset_t *b)
{
    n00b_hashset_t *result = n00b_hashset_copy(a);

    if (!b) {
        return result;
    }

    for (int32_t i = 0; i < b->cap; i++) {
        void *item = b->buckets[i];

        if (item && item != HS_TOMBSTONE) {
            n00b_hashset_add(result, item);
        }
    }

    return result;
}

void
n00b_hashset_free(n00b_hashset_t *s)
{
    if (!s) {
        return;
    }

    if (!s->shared && s->buckets) {
        n00b_free(s->buckets);
    }

    n00b_free(s);
}
