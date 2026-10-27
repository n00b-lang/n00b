#pragma once

#include "lib/module.h"
#include "lib/buffer.h"

typedef struct core_source_snapshot_t core_source_snapshot_t;
typedef struct core_snapshot_inventory_t core_snapshot_inventory_t;
typedef struct {
    size_t package_cache_hits;
    size_t package_scans;
    size_t source_reads;
} core_snapshot_inventory_stats_t;
typedef struct {
    core_string_t path;
    bool enters_fileless_directories;
} core_snapshot_root_t;

typedef bool (*core_snapshot_inventory_accept_fn)(
    void *context, core_string_t relative_path);
typedef bool (*core_snapshot_inventory_visit_fn)(
    void *context, size_t root_index, core_string_t relative_path,
    const core_source_snapshot_t *snapshot);

core_snapshot_inventory_t *core_snapshot_inventory_new(
    const core_snapshot_root_t *roots, size_t root_count,
    core_snapshot_inventory_accept_fn accept, void *context);
void core_snapshot_inventory_free(core_snapshot_inventory_t *inventory);
bool core_snapshot_inventory_enable_cache(
    core_snapshot_inventory_t *inventory, core_string_t cache_root,
    uint64_t filter_revision);

bool core_snapshot_inventory_refresh(core_snapshot_inventory_t *inventory);
uint64_t core_snapshot_inventory_generation(
    const core_snapshot_inventory_t *inventory);
size_t core_snapshot_inventory_count(const core_snapshot_inventory_t *inventory);
core_snapshot_inventory_stats_t core_snapshot_inventory_stats(
    const core_snapshot_inventory_t *inventory);
bool core_snapshot_inventory_product_read(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path, core_string_t kind, uint64_t profile,
    core_buffer_t *out);
bool core_snapshot_inventory_product_write(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path, core_string_t kind, uint64_t profile,
    const void *data, size_t size);
const core_source_snapshot_t *core_snapshot_inventory_find(
    const core_snapshot_inventory_t *inventory, size_t root_index,
    core_string_t relative_path);
bool core_snapshot_inventory_visit(
    const core_snapshot_inventory_t *inventory,
    core_snapshot_inventory_visit_fn visit, void *context);

core_source_snapshot_t *core_source_snapshot_retain(
    const core_source_snapshot_t *snapshot);
void core_source_snapshot_release(core_source_snapshot_t *snapshot);
core_string_t core_source_snapshot_path(const core_source_snapshot_t *snapshot);
core_string_t core_source_snapshot_contents(
    const core_source_snapshot_t *snapshot);
size_t core_source_snapshot_size(const core_source_snapshot_t *snapshot);
uint64_t core_source_snapshot_revision(
    const core_source_snapshot_t *snapshot);
