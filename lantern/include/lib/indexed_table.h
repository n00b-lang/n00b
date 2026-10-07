#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/dict.h"

typedef struct core_indexed_table_t core_indexed_table_t;
typedef struct core_index_remap_t core_index_remap_t;

typedef struct {
    const void *key;
    void *value;
    uint32_t index;
} core_indexed_entry_t;

/* Keys and values are borrowed. Keys must remain immutable and live until the
 * table is freed. Rows [0, reserved_count) start empty and can only be filled
 * by reserve(); later rows are appended by intern() in first-seen order. */
core_indexed_table_t *core_indexed_table_new(size_t reserved_count,
                                             uint32_t max_index,
                                             core_hash_fn hash_fn,
                                             core_dict_key_eq_fn key_eq);
void core_indexed_table_free(core_indexed_table_t *table);
size_t core_indexed_table_len(const core_indexed_table_t *table);
const core_indexed_entry_t *core_indexed_table_at(
    const core_indexed_table_t *table, uint32_t index);
const core_indexed_entry_t *core_indexed_table_find(
    const core_indexed_table_t *table, const void *key);

/* A reserved row may be filled once. A null key is invalid; unfilled reserved
 * rows remain empty. Duplicate keys are rejected, including across rows. */
bool core_indexed_table_reserve(core_indexed_table_t *table, uint32_t index,
                                const void *key, void *value);

/* Returns the existing entry on a duplicate without replacing its value.
 * Returns null on index overflow or an invalid key. `inserted` is optional. */
const core_indexed_entry_t *core_indexed_table_intern(
    core_indexed_table_t *table, const void *key, void *value, bool *inserted);

/* A local index can be assigned exactly once. Unassigned and out-of-range
 * indexes fail lookup, including when the final index could legally be zero. */
core_index_remap_t *core_index_remap_new(size_t local_count,
                                         uint32_t max_final_index);
void core_index_remap_free(core_index_remap_t *remap);
size_t core_index_remap_len(const core_index_remap_t *remap);
bool core_index_remap_set(core_index_remap_t *remap, uint32_t local_index,
                          uint32_t final_index);
bool core_index_remap_get(const core_index_remap_t *remap,
                          uint32_t local_index, uint32_t *final_index);
