#include "abi_index_patch.h"

#include <stdlib.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"

typedef struct {
    uint32_t section_index;
    uint64_t offset;
    uint8_t *address;
    uint32_t final_value;
    size_t ordinal;
} resolved_patch_t;

void
n00bb_abi_index_patch_encode(uint8_t out[N00BB_ABI_INDEX_PATCH_BYTES],
                              const n00bb_abi_index_patch_t *patch)
{
    core_le_store_u32(out, patch->target_section_index);
    core_le_store_u32(out + 4, patch->referenced_table_kind);
    core_le_store_u64(out + 8, patch->target_byte_offset);
    core_le_store_u32(out + 16, patch->local_index);
    core_le_store_u32(out + 20, patch->patch_kind);
    core_le_store_u32(out + 24, patch->reserved_zero);
    core_le_store_u32(out + 28, patch->reserved_zero_2);
}

void
n00bb_abi_index_patch_decode(const uint8_t in[N00BB_ABI_INDEX_PATCH_BYTES],
                              n00bb_abi_index_patch_t *patch)
{
    *patch = (n00bb_abi_index_patch_t){
        .target_section_index = core_le_load_u32(in),
        .referenced_table_kind = core_le_load_u32(in + 4),
        .target_byte_offset = core_le_load_u64(in + 8),
        .local_index = core_le_load_u32(in + 16),
        .patch_kind = core_le_load_u32(in + 20),
        .reserved_zero = core_le_load_u32(in + 24),
        .reserved_zero_2 = core_le_load_u32(in + 28),
    };
}

static int
compare_target(const void *left, const void *right)
{
    const resolved_patch_t *a = left;
    const resolved_patch_t *b = right;
    if (a->section_index != b->section_index)
        return a->section_index < b->section_index ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}

static n00bb_abi_patch_status_t
fail(n00bb_abi_patch_status_t status, size_t ordinal, size_t *failed_patch)
{
    if (failed_patch) *failed_patch = ordinal;
    return status;
}

n00bb_abi_patch_status_t
n00bb_abi_index_patches_apply(
    const uint8_t *patch_bytes, size_t patch_byte_count,
    const n00bb_abi_patch_section_t *sections, size_t section_count,
    const n00bb_abi_patch_table_t *tables, size_t table_count,
    size_t *failed_patch)
{
    if (failed_patch) *failed_patch = SIZE_MAX;
    if (patch_byte_count % N00BB_ABI_INDEX_PATCH_BYTES
        || (patch_byte_count && !patch_bytes)
        || (section_count && !sections) || (table_count && !tables))
        return N00BB_ABI_PATCH_INVALID_INPUT;
    size_t count = patch_byte_count / N00BB_ABI_INDEX_PATCH_BYTES;
    if (count > SIZE_MAX / sizeof(resolved_patch_t))
        return N00BB_ABI_PATCH_INVALID_INPUT;
    resolved_patch_t *resolved = core_alloc_array(resolved_patch_t, count);
    n00bb_abi_patch_status_t status = N00BB_ABI_PATCH_OK;
    for (size_t i = 0; i < count; i++) {
        n00bb_abi_index_patch_t patch;
        n00bb_abi_index_patch_decode(
            patch_bytes + i * N00BB_ABI_INDEX_PATCH_BYTES, &patch);
        if (!patch.target_section_index || !patch.referenced_table_kind
            || patch.reserved_zero || patch.reserved_zero_2
            || (patch.patch_kind != N00BB_ABI_PATCH_INDEX
                && patch.patch_kind != N00BB_ABI_PATCH_BOX_TYPE)
            || (patch.patch_kind == N00BB_ABI_PATCH_BOX_TYPE
                && patch.referenced_table_kind != N00B_ABI_TABLE_TYPE)) {
            status = fail(N00BB_ABI_PATCH_INVALID_RECORD, i, failed_patch);
            break;
        }
        const n00bb_abi_patch_section_t *section = nullptr;
        bool duplicate_section = false;
        for (size_t j = 0; j < section_count; j++) {
            if (sections[j].native_index != patch.target_section_index)
                continue;
            if (section) {
                duplicate_section = true;
                break;
            }
            section = &sections[j];
        }
        if (!section || duplicate_section) {
            status = fail(N00BB_ABI_PATCH_UNKNOWN_SECTION, i, failed_patch);
            break;
        }
        const n00bb_abi_patch_table_t *table = nullptr;
        bool duplicate_table = false;
        for (size_t j = 0; j < table_count; j++) {
            if (tables[j].kind != patch.referenced_table_kind) continue;
            if (table) {
                duplicate_table = true;
                break;
            }
            table = &tables[j];
        }
        if (!table || duplicate_table || !table->remap) {
            status = fail(N00BB_ABI_PATCH_UNKNOWN_TABLE, i, failed_patch);
            break;
        }
        uint32_t final_index;
        if (!core_index_remap_get(table->remap, patch.local_index,
                                  &final_index)
            || final_index >= table->final_row_count
            || ((patch.local_index == 0) != (final_index == 0))
            || (patch.referenced_table_kind != N00B_ABI_TABLE_TYPE
                && !patch.local_index)
            || (patch.patch_kind == N00BB_ABI_PATCH_BOX_TYPE
                && (patch.local_index > INT32_MAX
                    || final_index > INT32_MAX))) {
            status = fail(N00BB_ABI_PATCH_INVALID_INDEX, i, failed_patch);
            break;
        }
        if (!section->bytes || patch.target_byte_offset % sizeof(uint32_t)
            || patch.target_byte_offset > section->byte_length
            || sizeof(uint32_t) > section->byte_length
            || patch.target_byte_offset
                > section->byte_length - sizeof(uint32_t)) {
            status = fail(N00BB_ABI_PATCH_INVALID_TARGET, i, failed_patch);
            break;
        }
        uint8_t *address = section->bytes + (size_t)patch.target_byte_offset;
        uint32_t local_value = core_le_load_u32(address);
        if (patch.patch_kind == N00BB_ABI_PATCH_BOX_TYPE) {
            if ((local_value & UINT32_C(0x7fffffff)) != patch.local_index) {
                status = fail(N00BB_ABI_PATCH_TARGET_MISMATCH,
                              i, failed_patch);
                break;
            }
            final_index |= local_value & UINT32_C(0x80000000);
        } else if (local_value != patch.local_index) {
            status = fail(N00BB_ABI_PATCH_TARGET_MISMATCH, i, failed_patch);
            break;
        }
        resolved[i] = (resolved_patch_t){
            .section_index = patch.target_section_index,
            .offset = patch.target_byte_offset,
            .address = address,
            .final_value = final_index,
            .ordinal = i,
        };
    }
    if (status == N00BB_ABI_PATCH_OK) {
        qsort(resolved, count, sizeof(*resolved), compare_target);
        for (size_t i = 1; i < count; i++) {
            if (compare_target(&resolved[i - 1], &resolved[i]) != 0) continue;
            status = fail(N00BB_ABI_PATCH_DUPLICATE_TARGET,
                          resolved[i].ordinal, failed_patch);
            break;
        }
        if (status == N00BB_ABI_PATCH_OK)
            for (size_t i = 0; i < count; i++)
                core_le_store_u32(resolved[i].address, resolved[i].final_value);
    }
    core_free(resolved);
    return status;
}
