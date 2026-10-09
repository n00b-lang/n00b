#include "builtin_abi.h"

#include <stdint.h>

typedef struct {
    uint64_t count;
    const uint8_t *bases[];
} section_bases_t;

extern const section_bases_t n00b_abi_section_bases;
extern const uint8_t *n00bb_abi_tls_address_current_thread(uint32_t index);

const uint8_t *
n00b_builtin_abi_storage_address(uint32_t index)
{
    const n00b_abi_storage_v1 *storage = n00b_builtin_abi_storage(index);
    if (!storage || !storage->symbol_index)
        return NULL;
    if (storage->segment_kind == N00B_ABI_STORAGE_TLS)
        return n00bb_abi_tls_address_current_thread(index);
    if (storage->segment_kind > N00B_ABI_STORAGE_TLS
        || storage->segment_index >= n00b_abi_section_bases.count)
        return NULL;
    const uint8_t *base = n00b_abi_section_bases.bases[storage->segment_index];
    uintptr_t address = (uintptr_t)base;
    if (!base || storage->byte_offset > UINTPTR_MAX - address) return NULL;
    return (const uint8_t *)(address + storage->byte_offset);
}
