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

bool core_indexed_table_reserve(core_indexed_table_t *table, uint32_t index,
                                const void *key, void *value);

const core_indexed_entry_t *core_indexed_table_intern(
    core_indexed_table_t *table, const void *key, void *value, bool *inserted);

core_index_remap_t *core_index_remap_new(size_t local_count,
                                         uint32_t max_final_index);
void core_index_remap_free(core_index_remap_t *remap);
size_t core_index_remap_len(const core_index_remap_t *remap);
bool core_index_remap_set(core_index_remap_t *remap, uint32_t local_index,
                          uint32_t final_index);
bool core_index_remap_get(const core_index_remap_t *remap,
                          uint32_t local_index, uint32_t *final_index);
