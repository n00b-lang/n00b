#pragma once

#include "abi_directory.h"
#include "lib/target.h"

#define N00BB_ABI_HEADER_BYTES 88u
#define N00BB_ABI_PROFILE_ID 1u
#define N00BB_ABI_DATA_LAYOUT_PROFILE 1u
#define N00BB_ABI_CPU_NATIVE_ATOMIC128 (UINT64_C(1) << 0)
#define N00BB_ABI_CAP_WRAPPING (UINT64_C(1) << 0)
#define N00BB_ABI_CAP_DEBUG (UINT64_C(1) << 1)
#define N00BB_ABI_CAP_OPTIMIZED (UINT64_C(1) << 2)
#define N00BB_ABI_CAP_SANITIZED (UINT64_C(1) << 3)
#define N00BB_ABI_CAPABILITY_MASK (N00BB_ABI_CAP_WRAPPING \
    | N00BB_ABI_CAP_DEBUG | N00BB_ABI_CAP_OPTIMIZED \
    | N00BB_ABI_CAP_SANITIZED)

typedef enum {
    N00BB_ABI_TARGET_X86_64_LINUX = 1,
    N00BB_ABI_TARGET_ARM64_MACOS = 2,
    N00BB_ABI_TARGET_X86_64_WINDOWS = 3,
    N00BB_ABI_TARGET_ARM64_LINUX = 4,
} n00bb_abi_target_id_t;

typedef struct {
    uint32_t target_id;
    uint32_t abi_profile_id;
    uint64_t feature_bits;
    uint64_t cpu_profile;
    uint64_t data_layout_profile;
} n00bb_abi_image_key_t;

typedef struct {
    n00bb_abi_image_key_t key;
    uint64_t artifact_capabilities;
    uint32_t required_alignment;
    uint32_t directory_count;
    uint64_t directory_offset;
    uint64_t total_extent;
    uint64_t payload_checksum;
} n00bb_abi_image_header_t;

bool n00bb_abi_image_key_for_target(const core_target_desc_t *target,
                                    n00bb_abi_image_key_t *out);

/* Table bytes must already be present. The checksum covers the current
 * serialized bytes; final images call this after static-link rewrites and
 * before loader relocations. */
bool n00bb_abi_container_write(
    uint8_t *image, size_t image_bytes,
    const n00bb_abi_image_header_t *header,
    const n00bb_abi_directory_entry_t *entries,
    const n00bb_abi_table_schema_t *schemas, size_t schema_count,
    bool allow_unknown);

/* Never follows a directory entry until header, key, and checksum pass. */
bool n00bb_abi_container_read(
    const uint8_t *image, size_t image_bytes,
    const n00bb_abi_image_key_t *expected_key,
    const n00bb_abi_table_schema_t *schemas, size_t schema_count,
    bool allow_unknown, n00bb_abi_image_header_t *header,
    n00bb_abi_directory_entry_t *entries, size_t entry_capacity);
