#include "logic/asp_relation.h"

#include <string.h>

#define REL_INIT_CAP 64

uint64_t
n00b_dl_tuple_hash(const n00b_dl_sym_t *tuple, int32_t arity)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int32_t i = 0; i < arity; i++) {
        uint64_t v = (uint64_t)tuple[i];
        h ^= v;
        h *= 0x100000001b3ULL;
        h ^= v >> 32;
        h *= 0x100000001b3ULL;
    }
    return h;
}

void
n00b_dl_relation_init(n00b_dl_relation_t *rel, n00b_dl_rel_id_t id,
                        n00b_string_t name, int32_t arity)
{
    rel->id    = id;
    rel->arity = arity;
    rel->name  = name;

    rel->stable_data  = n00b_alloc_array(n00b_dl_sym_t, REL_INIT_CAP * arity);
    rel->stable_count = 0;
    rel->stable_cap   = REL_INIT_CAP;

    rel->recent_data  = n00b_alloc_array(n00b_dl_sym_t, REL_INIT_CAP * arity);
    rel->recent_count = 0;
    rel->recent_cap   = REL_INIT_CAP;

    rel->to_add_data  = n00b_alloc_array(n00b_dl_sym_t, REL_INIT_CAP * arity);
    rel->to_add_count = 0;
    rel->to_add_cap   = REL_INIT_CAP;

    n00b_dl_u64_set_init(&rel->seen);

    rel->col_index = n00b_alloc_array(n00b_dl_i64_offsets_map_t, arity);
    for (int32_t i = 0; i < arity; i++) {
        n00b_dl_i64_offsets_map_init(&rel->col_index[i]);
    }
    rel->index_dirty = true;
}

void
n00b_dl_relation_free(n00b_dl_relation_t *rel)
{
    n00b_free(rel->stable_data);
    n00b_free(rel->recent_data);
    n00b_free(rel->to_add_data);
    n00b_dl_u64_set_free(&rel->seen);

    for (int32_t i = 0; i < rel->arity; i++) {
        n00b_dl_i64_offsets_map_free(&rel->col_index[i]);
    }
    n00b_free(rel->col_index);
}

static void
ensure_cap(n00b_dl_sym_t **data, size_t *cap, size_t needed, int32_t arity)
{
    if (needed <= *cap) return;
    size_t         old_cap  = *cap;
    size_t         new_cap  = old_cap * 2;
    while (new_cap < needed) new_cap *= 2;

    n00b_dl_sym_t *new_data = n00b_alloc_array(n00b_dl_sym_t,
                                                 new_cap * arity);
    memcpy(new_data, *data, old_cap * sizeof(n00b_dl_sym_t) * (size_t)arity);
    n00b_free(*data);
    *data = new_data;
    *cap  = new_cap;
}

// ---------------------------------------------------------------------------
// Get a tuple pointer from the combined stable+recent+to_add storage.
// Used during insert to verify hash collisions.
// ---------------------------------------------------------------------------
static const n00b_dl_sym_t *
get_any_tuple(n00b_dl_relation_t *rel, size_t idx)
{
    if (idx < rel->stable_count) {
        return rel->stable_data + idx * rel->arity;
    }
    idx -= rel->stable_count;
    if (idx < rel->recent_count) {
        return rel->recent_data + idx * rel->arity;
    }
    idx -= rel->recent_count;
    return rel->to_add_data + idx * rel->arity;
}

bool
n00b_dl_relation_insert(n00b_dl_relation_t *rel, const n00b_dl_sym_t *tuple)
{
    uint64_t h = n00b_dl_tuple_hash(tuple, rel->arity);

    // After a hash match, verify actual tuple equality to avoid false
    // dedup on hash collision.  If the hash is new, the tuple is new.
    if (n00b_dl_u64_set_contains(&rel->seen, h)) {
        size_t total = rel->stable_count + rel->recent_count + rel->to_add_count;
        for (size_t i = 0; i < total; i++) {
            const n00b_dl_sym_t *existing = get_any_tuple(rel, i);
            if (memcmp(existing, tuple,
                       sizeof(n00b_dl_sym_t) * (size_t)rel->arity) == 0) {
                return false;
            }
        }
        // Hash collision but not a true duplicate — fall through to insert
    } else {
        n00b_dl_u64_set_insert(&rel->seen, h);
    }

    ensure_cap(&rel->to_add_data, &rel->to_add_cap,
               rel->to_add_count + 1, rel->arity);

    memcpy(rel->to_add_data + rel->to_add_count * rel->arity,
           tuple, sizeof(n00b_dl_sym_t) * (size_t)rel->arity);
    rel->to_add_count++;
    return true;
}

bool
n00b_dl_relation_swap(n00b_dl_relation_t *rel)
{
    // Merge old recent into stable
    if (rel->recent_count > 0) {
        ensure_cap(&rel->stable_data, &rel->stable_cap,
                   rel->stable_count + rel->recent_count, rel->arity);
        memcpy(rel->stable_data + rel->stable_count * rel->arity,
               rel->recent_data,
               sizeof(n00b_dl_sym_t) * (size_t)rel->arity * rel->recent_count);
        rel->stable_count += rel->recent_count;
    }

    // Swap to_add -> recent
    n00b_dl_sym_t *tmp_data = rel->recent_data;
    size_t         tmp_cap  = rel->recent_cap;

    rel->recent_data  = rel->to_add_data;
    rel->recent_count = rel->to_add_count;
    rel->recent_cap   = rel->to_add_cap;

    rel->to_add_data  = tmp_data;
    rel->to_add_count = 0;
    rel->to_add_cap   = tmp_cap;

    if (rel->recent_count > 0) {
        rel->index_dirty = true;
    }

    return rel->recent_count > 0;
}

void
n00b_dl_relation_rebuild_index(n00b_dl_relation_t *rel)
{
    if (!rel->index_dirty) return;

    // Clear and reinitialize indexes
    for (int32_t col = 0; col < rel->arity; col++) {
        n00b_dl_i64_offsets_map_free(&rel->col_index[col]);
        n00b_dl_i64_offsets_map_init(&rel->col_index[col]);
    }

    // Index stable + recent
    size_t total = rel->stable_count + rel->recent_count;
    for (size_t i = 0; i < total; i++) {
        const n00b_dl_sym_t *tuple;
        if (i < rel->stable_count) {
            tuple = rel->stable_data + i * rel->arity;
        } else {
            tuple = rel->recent_data + (i - rel->stable_count) * rel->arity;
        }

        for (int32_t col = 0; col < rel->arity; col++) {
            int64_t                 val  = (int64_t)tuple[col];
            n00b_dl_offset_list_t *list =
                n00b_dl_i64_offsets_map_get(&rel->col_index[col], val);

            if (list) {
                if (list->len >= list->cap) {
                    int32_t  old_cap  = list->cap;
                    int32_t  new_cap  = old_cap ? old_cap * 2 : 4;
                    size_t  *new_data = n00b_alloc_array(size_t, new_cap);
                    memcpy(new_data, list->data, old_cap * sizeof(size_t));
                    n00b_free(list->data);
                    list->data = new_data;
                    list->cap  = new_cap;
                }
                list->data[list->len++] = i;
            } else {
                n00b_dl_offset_list_t new_list = {};
                new_list.data    = n00b_alloc_array(size_t, 4);
                new_list.cap     = 4;
                new_list.len     = 1;
                new_list.data[0] = i;
                n00b_dl_i64_offsets_map_put(&rel->col_index[col], val,
                                             new_list);
            }
        }
    }

    rel->index_dirty = false;
}

size_t
n00b_dl_relation_count(n00b_dl_relation_t *rel)
{
    return rel->stable_count + rel->recent_count;
}
