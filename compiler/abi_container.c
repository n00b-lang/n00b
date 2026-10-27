#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define XXH_INLINE_ALL
#include <xxhash.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "abi_container.h"

#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"

#define N00BB_ABI_CONTAINER_VERSION 1u

bool
n00bb_abi_image_key_for_target(const core_target_desc_t *target,
                               n00bb_abi_image_key_t *out)
{
    if (!target || !out || !core_target_validate(target)
        || target->endian != CORE_TARGET_LITTLE_ENDIAN
        || target->pointer_bits != 64
        || (target->enabled_features
            & ~CORE_TARGET_FEATURE_NATIVE_ATOMIC128)) return false;
    uint32_t id;
    if (target->object_format == CORE_TARGET_OBJECT_ELF
        && target->call_abi == CORE_TARGET_CALL_SYSV_AMD64)
        id = N00BB_ABI_TARGET_X86_64_LINUX;
    else if (target->object_format == CORE_TARGET_OBJECT_ELF
             && target->call_abi == CORE_TARGET_CALL_AAPCS64)
        id = N00BB_ABI_TARGET_ARM64_LINUX;
    else if (target->object_format == CORE_TARGET_OBJECT_MACH_O
             && target->call_abi == CORE_TARGET_CALL_DARWIN_AARCH64)
        id = N00BB_ABI_TARGET_ARM64_MACOS;
    else if (target->object_format == CORE_TARGET_OBJECT_COFF
             && target->call_abi == CORE_TARGET_CALL_WINDOWS_X64)
        id = N00BB_ABI_TARGET_X86_64_WINDOWS;
    else return false;
    *out = (n00bb_abi_image_key_t){
        .target_id = id,
        .abi_profile_id = N00BB_ABI_PROFILE_ID,
        .cpu_profile = target->enabled_features
            & CORE_TARGET_FEATURE_NATIVE_ATOMIC128,
        .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
    };
    return true;
}

static bool
valid_key(const n00bb_abi_image_key_t *key)
{
    return key
        && (key->target_id == N00BB_ABI_TARGET_X86_64_LINUX
            || key->target_id == N00BB_ABI_TARGET_ARM64_MACOS
            || key->target_id == N00BB_ABI_TARGET_X86_64_WINDOWS
            || key->target_id == N00BB_ABI_TARGET_ARM64_LINUX)
        && key->abi_profile_id == N00BB_ABI_PROFILE_ID
        && !key->feature_bits
        && !(key->cpu_profile & ~N00BB_ABI_CPU_NATIVE_ATOMIC128)
        && key->data_layout_profile == N00BB_ABI_DATA_LAYOUT_PROFILE;
}

static bool
valid_header(const n00bb_abi_image_header_t *header, size_t image_bytes)
{
    return header && valid_key(&header->key)
        && !(header->artifact_capabilities & ~N00BB_ABI_CAPABILITY_MASK)
        && header->required_alignment
        && !(header->required_alignment & (header->required_alignment - 1))
        && header->total_extent >= N00BB_ABI_HEADER_BYTES
        && header->total_extent <= image_bytes
        && header->directory_offset >= N00BB_ABI_HEADER_BYTES;
}

static void
encode_header(uint8_t out[N00BB_ABI_HEADER_BYTES],
              const n00bb_abi_image_header_t *header)
{
    memcpy(out, "N00BMETA", 8);
    core_le_store_u32(out + 8, N00BB_ABI_CONTAINER_VERSION);
    core_le_store_u32(out + 12, N00BB_ABI_HEADER_BYTES);
    core_le_store_u32(out + 16, header->key.target_id);
    core_le_store_u32(out + 20, header->key.abi_profile_id);
    core_le_store_u64(out + 24, header->key.feature_bits);
    core_le_store_u64(out + 32, header->artifact_capabilities);
    core_le_store_u64(out + 40, header->key.cpu_profile);
    core_le_store_u64(out + 48, header->key.data_layout_profile);
    core_le_store_u32(out + 56, header->required_alignment);
    core_le_store_u32(out + 60, header->directory_count);
    core_le_store_u64(out + 64, header->directory_offset);
    core_le_store_u64(out + 72, header->total_extent);
    core_le_store_u64(out + 80, header->payload_checksum);
}

static bool
decode_header(const uint8_t *image, size_t image_bytes,
              n00bb_abi_image_header_t *header)
{
    if (!image || image_bytes < N00BB_ABI_HEADER_BYTES
        || memcmp(image, "N00BMETA", 8)
        || core_le_load_u32(image + 8) != N00BB_ABI_CONTAINER_VERSION
        || core_le_load_u32(image + 12) != N00BB_ABI_HEADER_BYTES)
        return false;
    *header = (n00bb_abi_image_header_t){
        .key = {
            .target_id = core_le_load_u32(image + 16),
            .abi_profile_id = core_le_load_u32(image + 20),
            .feature_bits = core_le_load_u64(image + 24),
            .cpu_profile = core_le_load_u64(image + 40),
            .data_layout_profile = core_le_load_u64(image + 48),
        },
        .artifact_capabilities = core_le_load_u64(image + 32),
        .required_alignment = core_le_load_u32(image + 56),
        .directory_count = core_le_load_u32(image + 60),
        .directory_offset = core_le_load_u64(image + 64),
        .total_extent = core_le_load_u64(image + 72),
        .payload_checksum = core_le_load_u64(image + 80),
    };
    return valid_header(header, image_bytes);
}

static uint64_t
payload_checksum(const uint8_t *image, uint64_t total_extent)
{
    return XXH3_64bits(image + N00BB_ABI_HEADER_BYTES,
                       (size_t)(total_extent - N00BB_ABI_HEADER_BYTES));
}

bool
n00bb_abi_container_write(
    uint8_t *image, size_t image_bytes,
    const n00bb_abi_image_header_t *header,
    const n00bb_abi_directory_entry_t *entries,
    const n00bb_abi_table_schema_t *schemas, size_t schema_count,
    bool allow_unknown)
{
    if (!image || !valid_header(header, image_bytes)
        || (header->directory_count && !entries)
        || header->directory_offset > header->total_extent
        || header->directory_count >
            (header->total_extent - header->directory_offset)
                / N00BB_ABI_DIRECTORY_BYTES)
        return false;
    for (uint32_t i = 0; i < header->directory_count; i++)
        n00bb_abi_directory_encode_entry(image + header->directory_offset
            + (uint64_t)i * N00BB_ABI_DIRECTORY_BYTES, &entries[i]);
    n00bb_abi_directory_entry_t *decoded = core_alloc_array(
        n00bb_abi_directory_entry_t, header->directory_count);
    bool valid = n00bb_abi_directory_validate(image, image_bytes,
        N00BB_ABI_HEADER_BYTES, header->directory_offset,
        header->directory_count, header->total_extent,
        schemas, schema_count, allow_unknown, decoded);
    core_free(decoded);
    if (!valid) return false;
    n00bb_abi_image_header_t completed = *header;
    completed.payload_checksum = payload_checksum(image, header->total_extent);
    encode_header(image, &completed);
    return true;
}

bool
n00bb_abi_container_read(
    const uint8_t *image, size_t image_bytes,
    const n00bb_abi_image_key_t *expected_key,
    const n00bb_abi_table_schema_t *schemas, size_t schema_count,
    bool allow_unknown, n00bb_abi_image_header_t *header,
    n00bb_abi_directory_entry_t *entries, size_t entry_capacity)
{
    n00bb_abi_image_header_t decoded;
    if (!expected_key || !header || !decode_header(image, image_bytes,
                                                  &decoded)
        || decoded.key.target_id != expected_key->target_id
        || decoded.key.abi_profile_id != expected_key->abi_profile_id
        || decoded.key.feature_bits != expected_key->feature_bits
        || decoded.key.cpu_profile != expected_key->cpu_profile
        || decoded.key.data_layout_profile
            != expected_key->data_layout_profile
        || decoded.directory_count > entry_capacity
        || decoded.payload_checksum != payload_checksum(image,
                                                        decoded.total_extent)
        || !n00bb_abi_directory_validate(image, image_bytes,
            N00BB_ABI_HEADER_BYTES, decoded.directory_offset,
            decoded.directory_count, decoded.total_extent,
            schemas, schema_count, allow_unknown, entries)) return false;
    *header = decoded;
    return true;
}
