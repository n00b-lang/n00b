#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "abi_emit_strings.h"
#include "abi_string.h"
#include "lib/alloc.h"

#define ARRAY_STRING(a) core_string_borrow((a), sizeof(a) - 1)

int
main(void)
{
    static const char first[] = "example.module.LongTypeName";
    static const char second[] = "another.package.LongSymbolName";
    static const char third[] = "third.long.metadata.identifier";
    n00bb_abi_name_pool_t *pool = n00bb_abi_name_pool_new();
    assert(pool);
    n00b_abi_type_v1 types[2] = {0};
    n00b_abi_symbol_v1 symbols[2] = {0};
    assert(n00bb_abi_name_pool_encode(pool, ARRAY_STRING(first),
        N00B_ABI_TABLE_TYPE, 0,
        offsetof(n00b_abi_type_v1, canonical_name),
        &types[0].canonical_name));
    assert(n00bb_abi_name_pool_encode(pool, ARRAY_STRING(first),
        N00B_ABI_TABLE_TYPE, 1,
        offsetof(n00b_abi_type_v1, canonical_name),
        &types[1].canonical_name));
    assert(n00bb_abi_name_pool_encode(pool, ARRAY_STRING(second),
        N00B_ABI_TABLE_SYMBOL, 1,
        offsetof(n00b_abi_symbol_v1, name), &symbols[1].name));
    assert(n00bb_abi_name_pool_encode(pool, ARRAY_STRING(third),
        N00B_ABI_TABLE_SYMBOL, 0,
        offsetof(n00b_abi_symbol_v1, name), &symbols[0].name));
    assert(n00bb_abi_name_pool_size(pool) == sizeof(first) - 1
        + sizeof(second) - 1 + sizeof(third) - 1);
    assert(!memcmp(n00bb_abi_name_pool_bytes(pool), first, sizeof(first) - 1));
    n00b_abi_string_v1 value = {0};
    size_t pool_offset = SIZE_MAX;
    assert(n00bb_abi_name_pool_value(pool, ARRAY_STRING(first),
        N00B_ABI_STRING_ASCII, &value, &pool_offset));
    assert(pool_offset == 0 && value.long_value.selector
        == ((uint64_t)(sizeof(first) - 1)
            | ((uint64_t)N00B_ABI_STRING_ASCII
                << N00B_ABI_STRING_ENCODING_SHIFT)
            | ((uint64_t)N00B_ABI_STRING_LONG
                << N00B_ABI_STRING_TAG_SHIFT)));
    assert(n00bb_abi_name_pool_value(pool, CORE_STRING_STATIC("abc"),
        N00B_ABI_STRING_ASCII, &value, &pool_offset));
    assert(value.short_value.selector
        == (3 | (N00B_ABI_STRING_ASCII << 4)) && pool_offset == 0);
    assert(n00bb_abi_name_pool_size(pool) == sizeof(first) - 1
        + sizeof(second) - 1 + sizeof(third) - 1);
    n00bb_abi_string_header_t header;
    assert(n00bb_abi_string_decode(
        (const uint8_t *)&types[0].canonical_name,
        sizeof(types[0].canonical_name), &header));
    assert(header.tag == N00BB_ABI_STRING_LONG
        && header.length == sizeof(first) - 1);

    n00bb_abi_directory_entry_t entries[] = {
        {.kind = N00B_ABI_TABLE_TYPE,
         .record_bytes = sizeof(n00b_abi_type_v1), .record_count = 2,
         .file_offset = 64},
        {.kind = N00B_ABI_TABLE_SYMBOL,
         .record_bytes = sizeof(n00b_abi_symbol_v1), .record_count = 2,
         .file_offset = 64 + sizeof(types)},
    };
    core_object_emit_absolute64_t *relocations = nullptr;
    size_t count = 0;
    assert(n00bb_abi_name_pool_relocations(pool, entries, 2,
                                           &relocations, &count));
    assert(count == 4);
    assert(relocations[0].section_index == 1
        && relocations[0].symbol_index == 0
        && relocations[0].section_offset == 64
            + offsetof(n00b_abi_type_v1, canonical_name)
        && relocations[0].addend_bits == 0);
    assert(relocations[1].section_offset == 64 + sizeof(n00b_abi_type_v1)
        + offsetof(n00b_abi_type_v1, canonical_name)
        && relocations[1].addend_bits == 0);
    assert(relocations[2].section_offset == entries[1].file_offset
        + sizeof(n00b_abi_symbol_v1)
        + offsetof(n00b_abi_symbol_v1, name)
        && relocations[2].addend_bits == sizeof(first) - 1);
    assert(relocations[3].section_offset == entries[1].file_offset
        + offsetof(n00b_abi_symbol_v1, name)
        && relocations[3].addend_bits == sizeof(first) - 1
            + sizeof(second) - 1);
    core_free(relocations);

    entries[0].record_count = 0;
    relocations = nullptr;
    count = 0;
    assert(!n00bb_abi_name_pool_relocations(pool, entries, 2,
                                            &relocations, &count));
    assert(!relocations && !count);
    entries[0].record_count = 2;
    entries[0].file_offset = UINT64_MAX - 1;
    assert(!n00bb_abi_name_pool_relocations(pool, entries, 2,
                                            &relocations, &count));
    assert(!relocations && !count);
    n00bb_abi_name_pool_t *binary_pool = n00bb_abi_name_pool_new();
    static const char binary[] = "\0binary_payload";
    n00b_abi_static_string_v1 binary_row = {0};
    assert(binary_pool && n00bb_abi_name_pool_encode_with_encoding(
        binary_pool, core_string_borrow(binary, sizeof(binary) - 1),
        N00B_ABI_STRING_BINARY, N00B_ABI_TABLE_STATIC_STRING, 1,
        offsetof(n00b_abi_static_string_v1, value), &binary_row.value));
    assert(n00bb_abi_string_decode((const uint8_t *)&binary_row.value,
        sizeof(binary_row.value), &header)
        && header.tag == N00BB_ABI_STRING_LONG
        && header.encoding == N00BB_ABI_STRING_BINARY
        && header.length == sizeof(binary) - 1
        && n00bb_abi_name_pool_size(binary_pool) == sizeof(binary) - 1
        && !memcmp(n00bb_abi_name_pool_bytes(binary_pool), binary,
                   sizeof(binary) - 1));
    n00bb_abi_directory_entry_t binary_entry = {
        .kind = N00B_ABI_TABLE_STATIC_STRING,
        .record_bytes = sizeof(n00b_abi_static_string_v1),
        .record_count = 2, .file_offset = 32,
    };
    assert(n00bb_abi_name_pool_relocations(binary_pool, &binary_entry, 1,
        &relocations, &count) && count == 1
        && relocations[0].section_offset
            == 32 + sizeof(n00b_abi_static_string_v1));
    core_free(relocations);
    n00bb_abi_name_pool_free(binary_pool);
    n00bb_abi_name_pool_t *span_pool = n00bb_abi_name_pool_new();
    n00b_abi_static_string_v1 span_row = {0};
    assert(span_pool && n00bb_abi_name_pool_encode_tagged(span_pool,
        CORE_STRING_STATIC("abc"), N00B_ABI_STRING_ASCII,
        N00B_ABI_STRING_SPAN, N00B_ABI_TABLE_STATIC_STRING, 1,
        offsetof(n00b_abi_static_string_v1, value), &span_row.value));
    assert(n00bb_abi_string_decode((const uint8_t *)&span_row.value,
        sizeof(span_row.value), &header)
        && header.tag == N00BB_ABI_STRING_SPAN
        && header.encoding == N00BB_ABI_STRING_ASCII
        && header.length == 3
        && n00bb_abi_name_pool_size(span_pool) == 3);
    assert(n00bb_abi_name_pool_relocations(span_pool, &binary_entry, 1,
        &relocations, &count) && count == 1);
    core_free(relocations);
    n00b_abi_string_v1 empty_span = {0};
    assert(n00bb_abi_name_pool_value_tagged(span_pool,
        CORE_STRING_STATIC(""), N00B_ABI_STRING_ASCII,
        N00B_ABI_STRING_SPAN, &empty_span, &pool_offset)
        && pool_offset == 3 && n00bb_abi_name_pool_size(span_pool) == 4);
    n00bb_abi_name_pool_free(span_pool);
    n00bb_abi_name_pool_t *tagged_pool = n00bb_abi_name_pool_new();
    n00b_abi_static_string_v1 tagged_rows[3] = {0};
    assert(tagged_pool && n00bb_abi_name_pool_encode_tagged(tagged_pool,
        CORE_STRING_STATIC("abc"), N00B_ABI_STRING_ASCII,
        N00B_ABI_STRING_LONG, N00B_ABI_TABLE_STATIC_STRING, 0,
        offsetof(n00b_abi_static_string_v1, value), &tagged_rows[0].value));
    assert(n00bb_abi_name_pool_encode_tagged(tagged_pool,
        CORE_STRING_STATIC(""), N00B_ABI_STRING_ASCII,
        N00B_ABI_STRING_LONG, N00B_ABI_TABLE_STATIC_STRING, 1,
        offsetof(n00b_abi_static_string_v1, value), &tagged_rows[1].value));
    assert(n00bb_abi_name_pool_encode_tagged(tagged_pool,
        CORE_STRING_STATIC("abc"), N00B_ABI_STRING_ASCII,
        N00B_ABI_STRING_SHORT, N00B_ABI_TABLE_STATIC_STRING, 2,
        offsetof(n00b_abi_static_string_v1, value), &tagged_rows[2].value));
    assert(!n00bb_abi_name_pool_value_tagged(tagged_pool,
        ARRAY_STRING(first), N00B_ABI_STRING_ASCII,
        N00B_ABI_STRING_SHORT, &value, &pool_offset));
    assert(n00bb_abi_string_decode((const uint8_t *)&tagged_rows[0].value,
        sizeof(tagged_rows[0].value), &header)
        && header.tag == N00BB_ABI_STRING_LONG && header.length == 3);
    assert(n00bb_abi_string_decode((const uint8_t *)&tagged_rows[1].value,
        sizeof(tagged_rows[1].value), &header)
        && header.tag == N00BB_ABI_STRING_LONG && header.length == 0);
    assert(n00bb_abi_string_decode((const uint8_t *)&tagged_rows[2].value,
        sizeof(tagged_rows[2].value), &header)
        && header.tag == N00BB_ABI_STRING_SHORT && header.length == 3);
    assert(n00bb_abi_name_pool_size(tagged_pool) == 4);
    binary_entry.record_count = 3;
    assert(n00bb_abi_name_pool_relocations(tagged_pool, &binary_entry, 1,
        &relocations, &count) && count == 2
        && relocations[0].addend_bits == 0
        && relocations[1].addend_bits == 3);
    core_free(relocations);
    n00bb_abi_name_pool_free(tagged_pool);
    n00bb_abi_name_pool_free(pool);
    return 0;
}
