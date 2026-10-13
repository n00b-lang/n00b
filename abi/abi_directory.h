#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define N00BB_ABI_DIRECTORY_BYTES 32u

typedef struct {
    uint32_t kind;
    uint32_t schema_version;
    uint32_t record_bytes;
    uint32_t record_count;
    uint64_t file_offset;
    uint64_t byte_length;
} n00bb_abi_directory_entry_t;

typedef struct {
    uint32_t kind;
    uint32_t schema_version;
    uint32_t record_bytes;
    uint32_t alignment_bytes;
} n00bb_abi_table_schema_t;

/* Record widths and alignments from the reviewed v1 C ABI. */
const n00bb_abi_table_schema_t *n00bb_abi_v1_table_schemas(size_t *count);

void n00bb_abi_directory_encode_entry(
    uint8_t out[N00BB_ABI_DIRECTORY_BYTES],
    const n00bb_abi_directory_entry_t *entry);
void n00bb_abi_directory_decode_entry(
    const uint8_t in[N00BB_ABI_DIRECTORY_BYTES],
    n00bb_abi_directory_entry_t *entry);

/* Validates the directory and its table extents. Record contents and
 * cross-table references are validated by the table-specific reader. */
bool n00bb_abi_directory_validate(
    const uint8_t *image, size_t image_bytes, uint64_t header_bytes,
    uint64_t directory_offset, uint32_t directory_count,
    uint64_t total_extent, const n00bb_abi_table_schema_t *schemas,
    size_t schema_count, bool allow_unknown,
    n00bb_abi_directory_entry_t *entries);
