#include "abi_directory.h"
#include "n00b-abi-v1.h"
#include "lib/string.h"

#include <stdio.h>
#include <string.h>

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
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *official =
        n00bb_abi_v1_table_schemas(&schema_count);
    bool ok = EXPECT("every v1 table kind has one schema",
        schema_count == N00B_ABI_TABLE_LAST);
    for (uint32_t kind = N00B_ABI_TABLE_PACKAGE;
         kind <= N00B_ABI_TABLE_LAST; kind++) {
        size_t matches = 0;
        for (size_t i = 0; i < schema_count; i++) {
            if (official[i].kind != kind) continue;
            matches++;
            ok &= EXPECT("v1 schema has a valid width and alignment",
                official[i].schema_version == N00B_ABI_V1_TABLE_SCHEMA_VERSION
                && official[i].record_bytes
                && official[i].alignment_bytes
                && !(official[i].alignment_bytes
                     & (official[i].alignment_bytes - 1)));
        }
        ok &= EXPECT("v1 table kind appears exactly once", matches == 1);
    }
    ok &= EXPECT("type and stream schemas use reviewed C widths",
        official[N00B_ABI_TABLE_TYPE - 1].record_bytes
            == sizeof(n00b_abi_type_v1)
        && official[N00B_ABI_TABLE_INDEX_PATCH - 1].record_bytes
            == sizeof(n00b_abi_index_patch_v1)
        && official[N00B_ABI_TABLE_U32_STREAM - 1].record_bytes == 4
        && official[N00B_ABI_TABLE_BYTE_STREAM - 1].record_bytes == 1
        && official[N00B_ABI_TABLE_U64_STREAM - 1].record_bytes == 8);
    uint8_t image[192] = {0};
    n00bb_abi_directory_entry_t first = {
        .kind = 4, .schema_version = 1, .record_bytes = 8,
        .record_count = 1, .file_offset = 152, .byte_length = 8,
    };
    n00bb_abi_directory_entry_t second = {
        .kind = 5, .schema_version = 1, .record_bytes = 8,
        .record_count = 1, .file_offset = 160, .byte_length = 8,
    };
    const n00bb_abi_table_schema_t schemas[] = {
        {.kind = 4, .schema_version = 1, .record_bytes = 8,
         .alignment_bytes = 8},
        {.kind = 5, .schema_version = 1, .record_bytes = 8,
         .alignment_bytes = 8},
    };
    const uint8_t first_bytes[N00BB_ABI_DIRECTORY_BYTES] = {
        4, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0,
        152, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0,
    };
    n00bb_abi_directory_encode_entry(image + 88, &first);
    n00bb_abi_directory_encode_entry(image + 120, &second);
    n00bb_abi_directory_entry_t decoded[2] = {0};
    ok &= EXPECT("byte-exact directory record",
                 !memcmp(image + 88, first_bytes, sizeof(first_bytes)));
    n00bb_abi_directory_decode_entry(image + 88, &decoded[0]);
    ok &= EXPECT("decode all directory fields",
        decoded[0].kind == first.kind
        && decoded[0].schema_version == first.schema_version
        && decoded[0].record_bytes == first.record_bytes
        && decoded[0].record_count == first.record_count
        && decoded[0].file_offset == first.file_offset
        && decoded[0].byte_length == first.byte_length);
    n00bb_abi_directory_entry_t wide = first;
    wide.file_offset = UINT64_C(0x1122334455667788);
    wide.byte_length = UINT64_C(0x99aabbccddeeff00);
    uint8_t wide_bytes[N00BB_ABI_DIRECTORY_BYTES];
    n00bb_abi_directory_encode_entry(wide_bytes, &wide);
    n00bb_abi_directory_decode_entry(wide_bytes, &decoded[0]);
    ok &= EXPECT("directory offsets and extents retain all 64 bits",
        decoded[0].file_offset == wide.file_offset
        && decoded[0].byte_length == wide.byte_length);
    ok &= EXPECT("valid adjacent table extents",
        n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                      168, schemas, 2, false, decoded));
    ok &= EXPECT("directory count cannot exceed image",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88,
                                       UINT32_MAX, 168, schemas, 2,
                                       false, decoded));
    ok &= EXPECT("truncated image rejected",
        !n00bb_abi_directory_validate(image, 167, 88, 88, 2,
                                       168, schemas, 2, false, decoded));

    second.kind = first.kind;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("duplicate table kind rejected",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, false, decoded));
    second.kind = 5;
    second.file_offset = 152;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("overlapping table extents rejected",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, false, decoded));
    second.file_offset = 160;
    second.byte_length = 9;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("record count and length must agree",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, false, decoded));
    second.byte_length = 8;
    second.file_offset = 161;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("table alignment enforced",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       169, schemas, 2, false, decoded));
    second.file_offset = 160;
    second.schema_version = 2;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("known schema version enforced",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, false, decoded));
    second.schema_version = 1;
    second.record_bytes = 4;
    second.record_count = 2;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("known record width enforced",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, false, decoded));
    second.record_bytes = 8;
    second.record_count = 1;
    second.kind = 99;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("unknown table requires caller permission",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, false, decoded)
        && n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                         168, schemas, 2, true, decoded));
    second.file_offset = 120;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("unknown table still cannot overlap directory",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, true, decoded));
    second.file_offset = 0;
    n00bb_abi_directory_encode_entry(image + 120, &second);
    ok &= EXPECT("unknown table still cannot overlap header",
        !n00bb_abi_directory_validate(image, sizeof(image), 88, 88, 2,
                                       168, schemas, 2, true, decoded));
    return ok ? 0 : 1;
}
