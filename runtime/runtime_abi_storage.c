#include "builtin_abi.h"

#include <stdint.h>

typedef struct {
    uint64_t count;
    const uint8_t *bases[];
} section_bases_t;

extern const section_bases_t n00b_abi_section_bases;
extern const uint8_t *n00bb_abi_tls_address_current_thread(uint64_t key);

static const uint8_t *
section_address(uint32_t section, uint64_t offset)
{
    if (section >= n00b_abi_section_bases.count) return NULL;
    const uint8_t *base = n00b_abi_section_bases.bases[section];
    uintptr_t address = (uintptr_t)base;
    if (!base || offset > UINTPTR_MAX - address) return NULL;
    return (const uint8_t *)(address + offset);
}

const uint8_t *
n00b_builtin_abi_baked_node_address(uint32_t index)
{
    const n00b_abi_baked_node_v1 *node = n00b_builtin_abi_baked_node(index);
    return node ? section_address(node->segment_index, node->byte_offset)
                : NULL;
}

static const uint8_t *
thread_storage_address(const n00b_abi_storage_v1 *storage)
{
    const n00b_abi_snapshot_v1 *snapshot = n00b_abi_v1_current_snapshot();
    const n00b_abi_symbol_v1 *symbol = n00b_builtin_abi_symbol(
        storage->symbol_index);
    const n00b_abi_module_v1 *module = n00b_builtin_abi_module(
        storage->module_index);
    const uint8_t *module_name = NULL, *name = NULL;
    uint64_t module_bytes = 0, name_bytes = 0;
    if (!symbol || !module
        || n00b_abi_v1_string_bytes(snapshot, &module->full_path,
               &module_name, &module_bytes) != 1
        || n00b_abi_v1_string_bytes(snapshot, &symbol->name, &name,
               &name_bytes) != 1)
        return NULL;
    return n00bb_abi_tls_address_current_thread(n00b_abi_storage_key(
        module_name, module_bytes, name, name_bytes));
}

const uint8_t *
n00b_builtin_abi_storage_address(uint32_t index)
{
    const n00b_abi_storage_v1 *storage = n00b_builtin_abi_storage(index);
    if (!storage || !storage->symbol_index)
        return NULL;
    if (storage->segment_kind == N00B_ABI_STORAGE_TLS)
        return thread_storage_address(storage);
    if (storage->segment_kind > N00B_ABI_STORAGE_TLS) return NULL;
    return section_address(storage->segment_index, storage->byte_offset);
}
