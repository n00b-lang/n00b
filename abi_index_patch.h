#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lib/indexed_table.h"
#include "n00b-abi-v1.h"

#define N00BB_ABI_INDEX_PATCH_BYTES 32u

typedef enum {
    N00BB_ABI_PATCH_INDEX = 1,
    N00BB_ABI_PATCH_BOX_TYPE = 2,
} n00bb_abi_patch_kind_t;

typedef struct {
    uint32_t target_section_index;
    uint32_t referenced_table_kind;
    uint64_t target_byte_offset;
    uint32_t local_index;
    uint32_t patch_kind;
    uint32_t reserved_zero;
    uint32_t reserved_zero_2;
} n00bb_abi_index_patch_t;

typedef struct {
    uint32_t native_index;
    uint8_t *bytes;
    size_t byte_length;
} n00bb_abi_patch_section_t;

/* Supply only fixed-record output tables; omit streams and link-only tables.
 * All local rows, including reserved row zero, need an assigned remap. */
typedef struct {
    uint32_t kind;
    const core_index_remap_t *remap;
    uint32_t final_row_count;
} n00bb_abi_patch_table_t;

typedef enum {
    N00BB_ABI_PATCH_OK = 0,
    N00BB_ABI_PATCH_INVALID_INPUT,
    N00BB_ABI_PATCH_INVALID_RECORD,
    N00BB_ABI_PATCH_UNKNOWN_SECTION,
    N00BB_ABI_PATCH_UNKNOWN_TABLE,
    N00BB_ABI_PATCH_INVALID_INDEX,
    N00BB_ABI_PATCH_INVALID_TARGET,
    N00BB_ABI_PATCH_TARGET_MISMATCH,
    N00BB_ABI_PATCH_DUPLICATE_TARGET,
} n00bb_abi_patch_status_t;

void n00bb_abi_index_patch_encode(
    uint8_t out[N00BB_ABI_INDEX_PATCH_BYTES],
    const n00bb_abi_index_patch_t *patch);
void n00bb_abi_index_patch_decode(
    const uint8_t in[N00BB_ABI_INDEX_PATCH_BYTES],
    n00bb_abi_index_patch_t *patch);

/* Applies serialized local-index patches only after all records validate.
 * On failure, section bytes are unchanged and failed_patch receives an input
 * record ordinal when one is available. Remaps and sections must stay stable
 * for the duration of this call. */
n00bb_abi_patch_status_t n00bb_abi_index_patches_apply(
    const uint8_t *patch_bytes, size_t patch_byte_count,
    const n00bb_abi_patch_section_t *sections, size_t section_count,
    const n00bb_abi_patch_table_t *tables, size_t table_count,
    size_t *failed_patch);
