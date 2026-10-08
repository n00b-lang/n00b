#include "abi_index_patch.h"

#include <stdio.h>
#include <string.h>

#include "lib/byteorder.h"

static bool
expect(const char *label, bool condition)
{
    if (!condition) fprintf(stderr, "%s failed\n", label);
    return condition;
}

static void
encode_pair(uint8_t bytes[2 * N00BB_ABI_INDEX_PATCH_BYTES],
            n00bb_abi_index_patch_t first,
            n00bb_abi_index_patch_t second)
{
    n00bb_abi_index_patch_encode(bytes, &first);
    n00bb_abi_index_patch_encode(bytes + N00BB_ABI_INDEX_PATCH_BYTES,
                                  &second);
}

int
main(void)
{
    bool ok = true;
    core_index_remap_t *type_remap = core_index_remap_new(4, 3);
    const uint32_t final_indexes[] = {0, 2, 3, 1};
    for (uint32_t i = 0; i < 4; i++)
        ok &= expect("assign remap", core_index_remap_set(
            type_remap, i, final_indexes[i]));
    const n00bb_abi_patch_table_t table = {
        .kind = N00B_ABI_TABLE_TYPE,
        .remap = type_remap,
        .final_row_count = 4,
    };
    uint8_t section_bytes[64] = {0};
    const n00bb_abi_patch_section_t section = {
        .native_index = 7,
        .bytes = section_bytes,
        .byte_length = sizeof(section_bytes),
    };
    n00bb_abi_index_patch_t first = {
        .target_section_index = 7,
        .referenced_table_kind = N00B_ABI_TABLE_TYPE,
        .target_byte_offset = 0x20,
        .local_index = 1,
        .patch_kind = N00BB_ABI_PATCH_INDEX,
    };
    n00bb_abi_index_patch_t second = first;
    second.target_byte_offset = 0x28;
    second.patch_kind = N00BB_ABI_PATCH_BOX_TYPE;
    uint8_t patches[2 * N00BB_ABI_INDEX_PATCH_BYTES];
    encode_pair(patches, first, second);
    const uint8_t first_vector[N00BB_ABI_INDEX_PATCH_BYTES] = {
        7, 0, 0, 0, 4, 0, 0, 0, 0x20, 0, 0, 0, 0, 0, 0, 0,
        1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    ok &= expect("patch record matches ABI byte vector",
                 !memcmp(patches, first_vector, sizeof(first_vector)));
    n00bb_abi_index_patch_t decoded = {0};
    n00bb_abi_index_patch_decode(patches, &decoded);
    ok &= expect("patch record decoded", decoded.target_section_index == 7
        && decoded.referenced_table_kind == N00B_ABI_TABLE_TYPE
        && decoded.target_byte_offset == 0x20
        && decoded.local_index == 1 && decoded.patch_kind == 1);

    core_le_store_u32(section_bytes + 0x20, 1);
    core_le_store_u32(section_bytes + 0x28, UINT32_C(0x80000001));
    size_t failed = 0;
    ok &= expect("nonidentity type and box patch",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed) == N00BB_ABI_PATCH_OK
        && failed == SIZE_MAX
        && core_le_load_u32(section_bytes + 0x20) == 2
        && core_le_load_u32(section_bytes + 0x28) == UINT32_C(0x80000002));

    core_le_store_u32(section_bytes + 0x20, 1);
    core_le_store_u32(section_bytes + 0x28, UINT32_C(0x80000001));
    second.target_byte_offset = first.target_byte_offset;
    encode_pair(patches, first, second);
    ok &= expect("duplicate target rejected atomically",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_DUPLICATE_TARGET
        && core_le_load_u32(section_bytes + 0x20) == 1);

    second.target_byte_offset = 0x28;
    second.local_index = 3;
    encode_pair(patches, first, second);
    ok &= expect("stale local index rejected atomically",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_TARGET_MISMATCH
        && failed == 1 && core_le_load_u32(section_bytes + 0x20) == 1);
    second.local_index = 1;

    second.target_byte_offset = sizeof(section_bytes) - 3;
    encode_pair(patches, first, second);
    ok &= expect("misaligned/out-of-bounds target rejected atomically",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_INVALID_TARGET
        && core_le_load_u32(section_bytes + 0x20) == 1);
    second.target_byte_offset = 0x28;

    second.reserved_zero = 1;
    encode_pair(patches, first, second);
    ok &= expect("nonzero reserved field rejected atomically",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_INVALID_RECORD
        && core_le_load_u32(section_bytes + 0x20) == 1);
    second.reserved_zero = 0;

    second.patch_kind = N00BB_ABI_PATCH_INDEX;
    second.referenced_table_kind = 32;
    encode_pair(patches, first, second);
    ok &= expect("stream table cannot be patched",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_UNKNOWN_TABLE
        && core_le_load_u32(section_bytes + 0x20) == 1);
    second.referenced_table_kind = N00B_ABI_TABLE_TYPE;
    second.patch_kind = N00BB_ABI_PATCH_BOX_TYPE;

    second.local_index = 4;
    encode_pair(patches, first, second);
    ok &= expect("missing local row rejected",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_INVALID_INDEX
        && core_le_load_u32(section_bytes + 0x20) == 1);
    second.local_index = 1;

    core_index_remap_t *module_remap = core_index_remap_new(2, 1);
    ok &= expect("assign module remap",
        core_index_remap_set(module_remap, 0, 0)
        && core_index_remap_set(module_remap, 1, 1));
    const n00bb_abi_patch_table_t tables[] = {
        table,
        {.kind = N00B_ABI_TABLE_MODULE, .remap = module_remap,
         .final_row_count = 2},
    };
    second.patch_kind = N00BB_ABI_PATCH_INDEX;
    second.referenced_table_kind = N00B_ABI_TABLE_MODULE;
    second.local_index = 0;
    core_le_store_u32(section_bytes + 0x28, 0);
    encode_pair(patches, first, second);
    ok &= expect("non-type row zero rejected atomically",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, tables, 2, &failed)
                == N00BB_ABI_PATCH_INVALID_INDEX
        && failed == 1 && core_le_load_u32(section_bytes + 0x20) == 1);
    second.patch_kind = N00BB_ABI_PATCH_BOX_TYPE;
    second.referenced_table_kind = N00B_ABI_TABLE_TYPE;
    second.local_index = 1;
    core_le_store_u32(section_bytes + 0x28, UINT32_C(0x80000001));
    core_index_remap_t *nil_remap = core_index_remap_new(2, 3);
    ok &= expect("assign invalid nil remap",
        core_index_remap_set(nil_remap, 0, 0)
        && core_index_remap_set(nil_remap, 1, 0));
    const n00bb_abi_patch_table_t nil_table = {
        .kind = N00B_ABI_TABLE_TYPE, .remap = nil_remap,
        .final_row_count = 4,
    };
    encode_pair(patches, first, second);
    ok &= expect("nonzero type cannot remap to nil",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &nil_table, 1, &failed)
                == N00BB_ABI_PATCH_INVALID_INDEX
        && failed == 0 && core_le_load_u32(section_bytes + 0x20) == 1);
    core_index_remap_free(nil_remap);
    core_index_remap_free(module_remap);

    first.local_index = 0;
    core_le_store_u32(section_bytes + 0x20, 0);
    n00bb_abi_index_patch_encode(patches, &first);
    ok &= expect("type row zero remains valid",
        n00bb_abi_index_patches_apply(patches, N00BB_ABI_INDEX_PATCH_BYTES,
            &section, 1, &table, 1, &failed) == N00BB_ABI_PATCH_OK
        && core_le_load_u32(section_bytes + 0x20) == 0);
    first.local_index = 1;
    core_le_store_u32(section_bytes + 0x20, 1);

    second.target_section_index = 8;
    encode_pair(patches, first, second);
    ok &= expect("unknown native section rejected",
        n00bb_abi_index_patches_apply(patches, sizeof(patches),
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_UNKNOWN_SECTION
        && core_le_load_u32(section_bytes + 0x20) == 1);
    second.target_section_index = 7;

    encode_pair(patches, first, second);
    ok &= expect("truncated patch table rejected",
        n00bb_abi_index_patches_apply(patches, sizeof(patches) - 1,
            &section, 1, &table, 1, &failed)
                == N00BB_ABI_PATCH_INVALID_INPUT
        && core_le_load_u32(section_bytes + 0x20) == 1);
    core_index_remap_free(type_remap);
    return ok ? 0 : 1;
}
