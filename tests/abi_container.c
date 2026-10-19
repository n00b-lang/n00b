#include "abi_container.h"

#include <stdio.h>
#include <string.h>

#include "lib/byteorder.h"

#define EXPECT(lit, condition) expect(CORE_STRING_STATIC(lit), (condition))

static bool
expect(core_string_t label, bool condition)
{
    if (!condition)
        fprintf(stderr, "%.*s failed\n", (int)label.u8_bytes, label.data);
    return condition;
}

int main(void)
{
    uint8_t image[160] = {0};
    const n00bb_abi_table_schema_t schema = {
        .kind = 4, .schema_version = 1,
        .record_bytes = 8, .alignment_bytes = 8,
    };
    const n00bb_abi_directory_entry_t entry = {
        .kind = 4, .schema_version = 1, .record_bytes = 8,
        .record_count = 1, .file_offset = 120, .byte_length = 8,
    };
    const n00bb_abi_image_header_t authored = {
        .key = {
            .target_id = N00BB_ABI_TARGET_ARM64_MACOS,
            .abi_profile_id = N00BB_ABI_PROFILE_ID,
            .feature_bits = 0, .cpu_profile = 0,
            .data_layout_profile = N00BB_ABI_DATA_LAYOUT_PROFILE,
        },
        .artifact_capabilities = N00BB_ABI_CAP_DEBUG,
        .required_alignment = 8,
        .directory_count = 1,
        .directory_offset = 88,
        .total_extent = 128,
    };
    const uint8_t prefix[24] = {
        'N', '0', '0', 'B', 'M', 'E', 'T', 'A',
        1, 0, 0, 0, 88, 0, 0, 0,
        2, 0, 0, 0, 1, 0, 0, 0,
    };
    for (size_t i = 0; i < 8; i++) image[120 + i] = (uint8_t)(i + 1);
    bool ok = true;
    ok &= EXPECT("container writes complete payload and directory",
        n00bb_abi_container_write(image, sizeof(image), &authored,
                                  &entry, &schema, 1, false));
    ok &= EXPECT("header prefix is byte exact",
        !memcmp(image, prefix, sizeof(prefix))
        && core_le_load_u64(image + 32) == 2
        && core_le_load_u32(image + 56) == 8
        && core_le_load_u32(image + 60) == 1
        && core_le_load_u64(image + 64) == 88
        && core_le_load_u64(image + 72) == 128);
    n00bb_abi_image_header_t decoded = {0};
    n00bb_abi_directory_entry_t decoded_entry = {0};
    ok &= EXPECT("valid container read with exact key",
        n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1)
        && decoded.payload_checksum == core_le_load_u64(image + 80)
        && decoded.artifact_capabilities == N00BB_ABI_CAP_DEBUG
        && decoded_entry.file_offset == 120);
    ok &= EXPECT("byte-exact payload checksum vector",
        core_le_load_u64(image + 80) == UINT64_C(0xc1fa374c74473860));
    n00bb_abi_image_key_t wrong_key = authored.key;
    wrong_key.target_id = N00BB_ABI_TARGET_X86_64_LINUX;
    ok &= EXPECT("target key mismatch rejected",
        !n00bb_abi_container_read(image, sizeof(image), &wrong_key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    wrong_key = authored.key;
    wrong_key.cpu_profile = N00BB_ABI_CPU_NATIVE_ATOMIC128;
    ok &= EXPECT("CPU guarantee is part of exact key",
        !n00bb_abi_container_read(image, sizeof(image), &wrong_key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    ok &= EXPECT("reader capacity checked before directory write",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 0));
    image[120] ^= 1;
    ok &= EXPECT("payload corruption rejected by checksum",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    uint64_t before_relink = core_le_load_u64(image + 80);
    ok &= EXPECT("producer finalizes changed payload with a new checksum",
        n00bb_abi_container_write(image, sizeof(image), &authored,
            &entry, &schema, 1, false)
        && core_le_load_u64(image + 80) != before_relink
        && n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    image[120] ^= 1;
    ok &= EXPECT("finalizing restored payload restores its checksum",
        n00bb_abi_container_write(image, sizeof(image), &authored,
            &entry, &schema, 1, false)
        && core_le_load_u64(image + 80) == before_relink);
    image[0] ^= 1;
    ok &= EXPECT("magic rejected before directory access",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    image[0] ^= 1;
    core_le_store_u32(image + 12, 87);
    ok &= EXPECT("header width mismatch rejected",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    core_le_store_u32(image + 12, 88);
    core_le_store_u64(image + 24, 1);
    ok &= EXPECT("unknown public feature rejected",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    core_le_store_u64(image + 24, 0);
    core_le_store_u64(image + 32, N00BB_ABI_CAPABILITY_MASK + 1);
    ok &= EXPECT("unknown capability rejected",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    core_le_store_u64(image + 32, N00BB_ABI_CAP_DEBUG);
    core_le_store_u32(image + 56, 3);
    ok &= EXPECT("section alignment must be a power of two",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));
    core_le_store_u32(image + 56, 8);
    core_le_store_u64(image + 72, UINT64_MAX);
    ok &= EXPECT("total extent checked before checksum",
        !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1));

    n00bb_abi_image_header_t invalid = authored;
    invalid.directory_count = UINT32_MAX;
    ok &= EXPECT("writer rejects directory outside image",
        !n00bb_abi_container_write(image, sizeof(image), &invalid,
                                   &entry, &schema, 1, false));
    invalid = authored;
    invalid.key.target_id = N00BB_ABI_TARGET_X86_64_WINDOWS + 1;
    ok &= EXPECT("writer rejects unsupported target",
        !n00bb_abi_container_write(image, sizeof(image), &invalid,
                                   &entry, &schema, 1, false));

    uint8_t empty_image[N00BB_ABI_HEADER_BYTES] = {0};
    n00bb_abi_image_header_t empty_header = authored;
    empty_header.directory_count = 0;
    empty_header.directory_offset = N00BB_ABI_HEADER_BYTES;
    empty_header.total_extent = N00BB_ABI_HEADER_BYTES;
    ok &= EXPECT("empty container has a valid zero-length payload",
        n00bb_abi_container_write(empty_image, sizeof(empty_image),
            &empty_header, nullptr, &schema, 1, false)
        && n00bb_abi_container_read(empty_image, sizeof(empty_image),
            &empty_header.key, &schema, 1, false, &decoded, nullptr, 0)
        && !decoded.directory_count);

    memset(image, 0, sizeof(image));
    n00bb_abi_directory_entry_t unknown = entry;
    unknown.kind = 99;
    ok &= EXPECT("unknown table is optional under exact key",
        n00bb_abi_container_write(image, sizeof(image), &authored,
            &unknown, &schema, 1, true)
        && !n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, false, &decoded, &decoded_entry, 1)
        && n00bb_abi_container_read(image, sizeof(image), &authored.key,
            &schema, 1, true, &decoded, &decoded_entry, 1)
        && decoded_entry.kind == unknown.kind);
    return ok ? 0 : 1;
}
