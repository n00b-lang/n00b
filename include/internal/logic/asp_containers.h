/**
 * @file asp_containers.h
 * @brief Self-contained container types for the Datalog engine.
 *
 * @internal
 *
 * Replaces slop's macro-generated typed containers with minimal
 * open-addressing hash tables and dynamic arrays.  All allocations
 * go through n00b_alloc / n00b_alloc_array / n00b_free.
 */
#pragma once

#include "logic/asp_types.h"
#include "core/alloc.h"
#include "core/hash.h"
#include "core/string.h"
#include "core/list.h"
#include "strings/string_ops.h"

#include <string.h>

// ============================================================================
// Dynamic arrays
// ============================================================================

n00b_list_decl(n00b_dl_rule_t);
typedef n00b_list_t(n00b_dl_rule_t) n00b_dl_rule_list_t;

typedef struct {
    size_t *data;
    int32_t len;
    int32_t cap;
} n00b_dl_offset_list_t;

typedef struct {
    int32_t target;
    bool    negated;
} n00b_dl_dep_edge_t;

typedef struct {
    n00b_dl_dep_edge_t *data;
    int32_t             len;
    int32_t             cap;
} n00b_dl_dep_edge_list_t;

// ============================================================================
// n00b_string_t -> int64 hash table (for intern name_to_id)
// ============================================================================

typedef struct {
    n00b_string_t key;
    int64_t       value;
    bool          occupied;
    bool          deleted;
} n00b_dl_str_i64_entry_t;

typedef struct {
    n00b_dl_str_i64_entry_t *entries;
    int32_t                  capacity;
    int32_t                  count;
} n00b_dl_str_i64_map_t;

static inline uint64_t
_n00b_dl_str_hash(n00b_string_t s)
{
    return (uint64_t)n00b_string_hash(s);
}

static inline void
n00b_dl_str_i64_map_init(n00b_dl_str_i64_map_t *m)
{
    m->capacity = 64;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_str_i64_entry_t, m->capacity);
}

static inline void
n00b_dl_str_i64_map_free(n00b_dl_str_i64_map_t *m)
{
    n00b_free(m->entries);
    m->entries  = nullptr;
    m->capacity = 0;
    m->count    = 0;
}

static inline void n00b_dl_str_i64_map_put(n00b_dl_str_i64_map_t *m,
                                             n00b_string_t           key,
                                             int64_t                 value);

static inline void
n00b_dl_str_i64_map_grow(n00b_dl_str_i64_map_t *m)
{
    int32_t                  old_cap     = m->capacity;
    n00b_dl_str_i64_entry_t *old_entries = m->entries;

    m->capacity *= 2;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_str_i64_entry_t, m->capacity);

    for (int32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied && !old_entries[i].deleted) {
            n00b_dl_str_i64_map_put(m, old_entries[i].key,
                                     old_entries[i].value);
        }
    }
    n00b_free(old_entries);
}

static inline void
n00b_dl_str_i64_map_put(n00b_dl_str_i64_map_t *m, n00b_string_t key,
                          int64_t value)
{
    if (m->count * 2 >= m->capacity) {
        n00b_dl_str_i64_map_grow(m);
    }

    uint64_t h   = _n00b_dl_str_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_str_i64_entry_t *e = &m->entries[idx];
        if (!e->occupied || e->deleted) {
            e->key      = key;
            e->value    = value;
            e->occupied = true;
            e->deleted  = false;
            m->count++;
            return;
        }
        if (n00b_unicode_str_eq(e->key, key)) {
            e->value = value;
            return;
        }
        idx = (idx + 1) % m->capacity;
    }
}

static inline int64_t *
n00b_dl_str_i64_map_get(n00b_dl_str_i64_map_t *m, n00b_string_t key)
{
    uint64_t h   = _n00b_dl_str_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_str_i64_entry_t *e = &m->entries[idx];
        if (!e->occupied) {
            return nullptr;
        }
        if (!e->deleted && n00b_unicode_str_eq(e->key, key)) {
            return &e->value;
        }
        idx = (idx + 1) % m->capacity;
    }
}

// ============================================================================
// int64 -> n00b_string_t hash table (for intern var_id_to_name)
// ============================================================================

typedef struct {
    int64_t       key;
    n00b_string_t value;
    bool          occupied;
    bool          deleted;
} n00b_dl_i64_str_entry_t;

typedef struct {
    n00b_dl_i64_str_entry_t *entries;
    int32_t                  capacity;
    int32_t                  count;
} n00b_dl_i64_str_map_t;

static inline uint64_t
_n00b_dl_i64_hash(int64_t v)
{
    uint64_t h = (uint64_t)v;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

static inline void
n00b_dl_i64_str_map_init(n00b_dl_i64_str_map_t *m)
{
    m->capacity = 64;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_i64_str_entry_t, m->capacity);
}

static inline void
n00b_dl_i64_str_map_free(n00b_dl_i64_str_map_t *m)
{
    n00b_free(m->entries);
    m->entries  = nullptr;
    m->capacity = 0;
    m->count    = 0;
}

static inline void n00b_dl_i64_str_map_put(n00b_dl_i64_str_map_t *m,
                                             int64_t                 key,
                                             n00b_string_t           value);

static inline void
n00b_dl_i64_str_map_grow(n00b_dl_i64_str_map_t *m)
{
    int32_t                  old_cap     = m->capacity;
    n00b_dl_i64_str_entry_t *old_entries = m->entries;

    m->capacity *= 2;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_i64_str_entry_t, m->capacity);

    for (int32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied && !old_entries[i].deleted) {
            n00b_dl_i64_str_map_put(m, old_entries[i].key,
                                     old_entries[i].value);
        }
    }
    n00b_free(old_entries);
}

static inline void
n00b_dl_i64_str_map_put(n00b_dl_i64_str_map_t *m, int64_t key,
                          n00b_string_t value)
{
    if (m->count * 2 >= m->capacity) {
        n00b_dl_i64_str_map_grow(m);
    }

    uint64_t h   = _n00b_dl_i64_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_i64_str_entry_t *e = &m->entries[idx];
        if (!e->occupied || e->deleted) {
            e->key      = key;
            e->value    = value;
            e->occupied = true;
            e->deleted  = false;
            m->count++;
            return;
        }
        if (e->key == key) {
            e->value = value;
            return;
        }
        idx = (idx + 1) % m->capacity;
    }
}

static inline n00b_string_t *
n00b_dl_i64_str_map_get(n00b_dl_i64_str_map_t *m, int64_t key)
{
    uint64_t h   = _n00b_dl_i64_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_i64_str_entry_t *e = &m->entries[idx];
        if (!e->occupied) {
            return nullptr;
        }
        if (!e->deleted && e->key == key) {
            return &e->value;
        }
        idx = (idx + 1) % m->capacity;
    }
}

// ============================================================================
// uint64 -> bool hash set (for relation dedup)
// ============================================================================

typedef struct {
    uint64_t key;
    bool     occupied;
} n00b_dl_u64_set_entry_t;

typedef struct {
    n00b_dl_u64_set_entry_t *entries;
    int32_t                  capacity;
    int32_t                  count;
} n00b_dl_u64_set_t;

static inline void
n00b_dl_u64_set_init(n00b_dl_u64_set_t *s)
{
    s->capacity = 128;
    s->count    = 0;
    s->entries  = n00b_alloc_array(n00b_dl_u64_set_entry_t, s->capacity);
}

static inline void
n00b_dl_u64_set_free(n00b_dl_u64_set_t *s)
{
    n00b_free(s->entries);
    s->entries  = nullptr;
    s->capacity = 0;
    s->count    = 0;
}

static inline bool n00b_dl_u64_set_insert(n00b_dl_u64_set_t *s, uint64_t key);

static inline void
n00b_dl_u64_set_grow(n00b_dl_u64_set_t *s)
{
    int32_t                  old_cap     = s->capacity;
    n00b_dl_u64_set_entry_t *old_entries = s->entries;

    s->capacity *= 2;
    s->count    = 0;
    s->entries  = n00b_alloc_array(n00b_dl_u64_set_entry_t, s->capacity);

    for (int32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied) {
            n00b_dl_u64_set_insert(s, old_entries[i].key);
        }
    }
    n00b_free(old_entries);
}

static inline bool
n00b_dl_u64_set_insert(n00b_dl_u64_set_t *s, uint64_t key)
{
    if (s->count * 2 >= s->capacity) {
        n00b_dl_u64_set_grow(s);
    }

    uint64_t h   = _n00b_dl_i64_hash((int64_t)key);
    int32_t  idx = (int32_t)(h % (uint64_t)s->capacity);

    for (;;) {
        n00b_dl_u64_set_entry_t *e = &s->entries[idx];
        if (!e->occupied) {
            e->key      = key;
            e->occupied = true;
            s->count++;
            return true;
        }
        if (e->key == key) {
            return false;
        }
        idx = (idx + 1) % s->capacity;
    }
}

static inline bool
n00b_dl_u64_set_contains(n00b_dl_u64_set_t *s, uint64_t key)
{
    uint64_t h   = _n00b_dl_i64_hash((int64_t)key);
    int32_t  idx = (int32_t)(h % (uint64_t)s->capacity);

    for (;;) {
        n00b_dl_u64_set_entry_t *e = &s->entries[idx];
        if (!e->occupied) {
            return false;
        }
        if (e->key == key) {
            return true;
        }
        idx = (idx + 1) % s->capacity;
    }
}

// ============================================================================
// int64 -> n00b_dl_offset_list_t hash table (for column indexes)
// ============================================================================

typedef struct {
    int64_t               key;
    n00b_dl_offset_list_t value;
    bool                  occupied;
    bool                  deleted;
} n00b_dl_i64_offsets_entry_t;

typedef struct {
    n00b_dl_i64_offsets_entry_t *entries;
    int32_t                      capacity;
    int32_t                      count;
} n00b_dl_i64_offsets_map_t;

static inline void
n00b_dl_i64_offsets_map_init(n00b_dl_i64_offsets_map_t *m)
{
    m->capacity = 64;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_i64_offsets_entry_t, m->capacity);
}

static inline void
n00b_dl_i64_offsets_map_free_entries(n00b_dl_i64_offsets_map_t *m)
{
    for (int32_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].occupied && !m->entries[i].deleted) {
            n00b_free(m->entries[i].value.data);
        }
    }
}

static inline void
n00b_dl_i64_offsets_map_free(n00b_dl_i64_offsets_map_t *m)
{
    n00b_dl_i64_offsets_map_free_entries(m);
    n00b_free(m->entries);
    m->entries  = nullptr;
    m->capacity = 0;
    m->count    = 0;
}

static inline void n00b_dl_i64_offsets_map_put(n00b_dl_i64_offsets_map_t *m,
                                                 int64_t                key,
                                                 n00b_dl_offset_list_t value);

static inline void
n00b_dl_i64_offsets_map_grow(n00b_dl_i64_offsets_map_t *m)
{
    int32_t                      old_cap     = m->capacity;
    n00b_dl_i64_offsets_entry_t *old_entries = m->entries;

    m->capacity *= 2;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_i64_offsets_entry_t, m->capacity);

    for (int32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied && !old_entries[i].deleted) {
            n00b_dl_i64_offsets_map_put(m, old_entries[i].key,
                                         old_entries[i].value);
        }
    }
    n00b_free(old_entries);
}

static inline void
n00b_dl_i64_offsets_map_put(n00b_dl_i64_offsets_map_t *m, int64_t key,
                              n00b_dl_offset_list_t value)
{
    if (m->count * 2 >= m->capacity) {
        n00b_dl_i64_offsets_map_grow(m);
    }

    uint64_t h   = _n00b_dl_i64_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_i64_offsets_entry_t *e = &m->entries[idx];
        if (!e->occupied || e->deleted) {
            e->key      = key;
            e->value    = value;
            e->occupied = true;
            e->deleted  = false;
            m->count++;
            return;
        }
        if (e->key == key) {
            e->value = value;
            return;
        }
        idx = (idx + 1) % m->capacity;
    }
}

static inline n00b_dl_offset_list_t *
n00b_dl_i64_offsets_map_get(n00b_dl_i64_offsets_map_t *m, int64_t key)
{
    uint64_t h   = _n00b_dl_i64_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_i64_offsets_entry_t *e = &m->entries[idx];
        if (!e->occupied) {
            return nullptr;
        }
        if (!e->deleted && e->key == key) {
            return &e->value;
        }
        idx = (idx + 1) % m->capacity;
    }
}

// ============================================================================
// int32 -> n00b_dl_dep_edge_list_t hash table (for stratification)
// ============================================================================

typedef struct {
    int32_t                 key;
    n00b_dl_dep_edge_list_t value;
    bool                    occupied;
    bool                    deleted;
} n00b_dl_i32_edges_entry_t;

typedef struct {
    n00b_dl_i32_edges_entry_t *entries;
    int32_t                    capacity;
    int32_t                    count;
} n00b_dl_i32_edges_map_t;

static inline void
n00b_dl_i32_edges_map_init(n00b_dl_i32_edges_map_t *m)
{
    m->capacity = 64;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_i32_edges_entry_t, m->capacity);
}

static inline void n00b_dl_i32_edges_map_put(n00b_dl_i32_edges_map_t *m,
                                               int32_t                  key,
                                               n00b_dl_dep_edge_list_t  value);

static inline void
n00b_dl_i32_edges_map_grow(n00b_dl_i32_edges_map_t *m)
{
    int32_t                    old_cap     = m->capacity;
    n00b_dl_i32_edges_entry_t *old_entries = m->entries;

    m->capacity *= 2;
    m->count    = 0;
    m->entries  = n00b_alloc_array(n00b_dl_i32_edges_entry_t, m->capacity);

    for (int32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].occupied && !old_entries[i].deleted) {
            n00b_dl_i32_edges_map_put(m, old_entries[i].key,
                                       old_entries[i].value);
        }
    }
    n00b_free(old_entries);
}

static inline void
n00b_dl_i32_edges_map_put(n00b_dl_i32_edges_map_t *m, int32_t key,
                            n00b_dl_dep_edge_list_t value)
{
    if (m->count * 2 >= m->capacity) {
        n00b_dl_i32_edges_map_grow(m);
    }

    uint64_t h   = _n00b_dl_i64_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_i32_edges_entry_t *e = &m->entries[idx];
        if (!e->occupied || e->deleted) {
            e->key      = key;
            e->value    = value;
            e->occupied = true;
            e->deleted  = false;
            m->count++;
            return;
        }
        if (e->key == key) {
            e->value = value;
            return;
        }
        idx = (idx + 1) % m->capacity;
    }
}

static inline n00b_dl_dep_edge_list_t *
n00b_dl_i32_edges_map_get(n00b_dl_i32_edges_map_t *m, int32_t key)
{
    uint64_t h   = _n00b_dl_i64_hash(key);
    int32_t  idx = (int32_t)(h % (uint64_t)m->capacity);

    for (;;) {
        n00b_dl_i32_edges_entry_t *e = &m->entries[idx];
        if (!e->occupied) {
            return nullptr;
        }
        if (!e->deleted && e->key == key) {
            return &e->value;
        }
        idx = (idx + 1) % m->capacity;
    }
}

