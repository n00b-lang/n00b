#include "lib/indexed_table.h"

#include "lib/alloc.h"
#include "lib/list.h"

typedef core_indexed_entry_t *core_indexed_entry_ptr_t;
core_list_decl(core_indexed_entry_ptr_t);

struct core_indexed_table_t {
    core_dict_t by_key;
    core_list_t(core_indexed_entry_ptr_t) rows;
    size_t reserved_count;
    uint32_t max_index;
};

struct core_index_remap_t {
    uint32_t *targets;
    uint8_t *assigned;
    size_t len;
    uint32_t max_final_index;
};

core_indexed_table_t *
core_indexed_table_new(size_t reserved_count, uint32_t max_index,
                       core_hash_fn hash_fn, core_dict_key_eq_fn key_eq)
{
    if (reserved_count > (uint64_t)max_index + 1) {
        return nullptr;
    }
    core_indexed_table_t *table = core_alloc(core_indexed_table_t);
    core_dict_init(&table->by_key, hash_fn, key_eq);
    table->rows = core_list_new_cap(core_indexed_entry_ptr_t,
                                    reserved_count);
    for (size_t i = 0; i < reserved_count; i++) {
        core_list_push(table->rows, nullptr);
    }
    table->reserved_count = reserved_count;
    table->max_index = max_index;
    return table;
}

void
core_indexed_table_free(core_indexed_table_t *table)
{
    if (!table) {
        return;
    }
    for (size_t i = 0; i < table->rows.len; i++) {
        core_free(table->rows.data[i]);
    }
    core_list_free(table->rows);
    core_dict_free(&table->by_key);
    core_free(table);
}

size_t
core_indexed_table_len(const core_indexed_table_t *table)
{
    return table ? table->rows.len : 0;
}

const core_indexed_entry_t *
core_indexed_table_at(const core_indexed_table_t *table, uint32_t index)
{
    return table && index < table->rows.len ? table->rows.data[index] : nullptr;
}

const core_indexed_entry_t *
core_indexed_table_find(const core_indexed_table_t *table, const void *key)
{
    if (!table || !key) {
        return nullptr;
    }
    bool found = false;
    core_indexed_entry_t *entry = core_dict_get(
        (core_dict_t *)&table->by_key, key, &found);
    return found ? entry : nullptr;
}

bool
core_indexed_table_reserve(core_indexed_table_t *table, uint32_t index,
                           const void *key, void *value)
{
    if (!table || !key || index >= table->reserved_count
        || table->rows.data[index] || core_indexed_table_find(table, key)) {
        return false;
    }
    core_indexed_entry_t *entry = core_alloc(core_indexed_entry_t);
    *entry = (core_indexed_entry_t){.key = key, .value = value, .index = index};
    table->rows.data[index] = entry;
    core_dict_add(&table->by_key, key, entry);
    return true;
}

const core_indexed_entry_t *
core_indexed_table_intern(core_indexed_table_t *table, const void *key,
                          void *value, bool *inserted)
{
    if (inserted) {
        *inserted = false;
    }
    if (!table || !key) {
        return nullptr;
    }
    const core_indexed_entry_t *existing = core_indexed_table_find(table, key);
    if (existing) {
        return existing;
    }
    if (table->rows.len > table->max_index) {
        return nullptr;
    }
    core_indexed_entry_t *entry = core_alloc(core_indexed_entry_t);
    *entry = (core_indexed_entry_t){.key = key, .value = value,
                                     .index = (uint32_t)table->rows.len};
    core_list_push(table->rows, entry);
    core_dict_add(&table->by_key, key, entry);
    if (inserted) {
        *inserted = true;
    }
    return entry;
}

core_index_remap_t *
core_index_remap_new(size_t local_count, uint32_t max_final_index)
{
    if (local_count > (uint64_t)UINT32_MAX + 1) {
        return nullptr;
    }
    core_index_remap_t *remap = core_alloc(core_index_remap_t);
    remap->targets = core_alloc_array(uint32_t, local_count);
    remap->assigned = core_alloc_array(uint8_t, local_count);
    remap->len = local_count;
    remap->max_final_index = max_final_index;
    return remap;
}

void
core_index_remap_free(core_index_remap_t *remap)
{
    if (!remap) {
        return;
    }
    core_free(remap->targets);
    core_free(remap->assigned);
    core_free(remap);
}

size_t
core_index_remap_len(const core_index_remap_t *remap)
{
    return remap ? remap->len : 0;
}

bool
core_index_remap_set(core_index_remap_t *remap, uint32_t local_index,
                     uint32_t final_index)
{
    if (!remap || local_index >= remap->len || remap->assigned[local_index]
        || final_index > remap->max_final_index) {
        return false;
    }
    remap->targets[local_index] = final_index;
    remap->assigned[local_index] = 1;
    return true;
}

bool
core_index_remap_get(const core_index_remap_t *remap,
                     uint32_t local_index, uint32_t *final_index)
{
    if (!remap || !final_index || local_index >= remap->len
        || !remap->assigned[local_index]) {
        return false;
    }
    *final_index = remap->targets[local_index];
    return true;
}
