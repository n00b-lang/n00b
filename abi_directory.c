#include "abi_directory.h"

#include "lib/byteorder.h"

void
n00bb_abi_directory_encode_entry(uint8_t out[N00BB_ABI_DIRECTORY_BYTES],
                                 const n00bb_abi_directory_entry_t *entry)
{
    core_le_store_u32(out, entry->kind);
    core_le_store_u32(out + 4, entry->schema_version);
    core_le_store_u32(out + 8, entry->record_bytes);
    core_le_store_u32(out + 12, entry->record_count);
    core_le_store_u64(out + 16, entry->file_offset);
    core_le_store_u64(out + 24, entry->byte_length);
}

void
n00bb_abi_directory_decode_entry(const uint8_t in[N00BB_ABI_DIRECTORY_BYTES],
                                 n00bb_abi_directory_entry_t *entry)
{
    *entry = (n00bb_abi_directory_entry_t){
        .kind = core_le_load_u32(in),
        .schema_version = core_le_load_u32(in + 4),
        .record_bytes = core_le_load_u32(in + 8),
        .record_count = core_le_load_u32(in + 12),
        .file_offset = core_le_load_u64(in + 16),
        .byte_length = core_le_load_u64(in + 24),
    };
}

static bool
overlaps(uint64_t left, uint64_t left_bytes,
         uint64_t right, uint64_t right_bytes)
{
    return left_bytes && right_bytes
        && left < right + right_bytes
        && right < left + left_bytes;
}

bool
n00bb_abi_directory_validate(
    const uint8_t *image, size_t image_bytes, uint64_t header_bytes,
    uint64_t directory_offset, uint32_t directory_count,
    uint64_t total_extent, const n00bb_abi_table_schema_t *schemas,
    size_t schema_count, bool allow_unknown,
    n00bb_abi_directory_entry_t *entries)
{
    if (!image || (directory_count && !entries)
        || (schema_count && !schemas)
        || header_bytes > total_extent || total_extent > image_bytes
        || directory_offset < header_bytes
        || directory_offset > total_extent
        || directory_count > (total_extent - directory_offset)
                             / N00BB_ABI_DIRECTORY_BYTES)
        return false;
    uint64_t directory_bytes = (uint64_t)directory_count
        * N00BB_ABI_DIRECTORY_BYTES;
    for (uint32_t i = 0; i < directory_count; i++) {
        n00bb_abi_directory_entry_t entry;
        n00bb_abi_directory_decode_entry(image + directory_offset
            + (uint64_t)i * N00BB_ABI_DIRECTORY_BYTES, &entry);
        if (!entry.kind || !entry.schema_version || !entry.record_bytes
            || entry.byte_length != (uint64_t)entry.record_bytes
                                    * entry.record_count
            || entry.file_offset > total_extent
            || entry.byte_length > total_extent - entry.file_offset
            || overlaps(entry.file_offset, entry.byte_length, 0,
                        header_bytes)
            || overlaps(entry.file_offset, entry.byte_length,
                        directory_offset, directory_bytes)) return false;
        const n00bb_abi_table_schema_t *schema = nullptr;
        for (size_t s = 0; s < schema_count; s++)
            if (schemas[s].kind == entry.kind) {
                schema = &schemas[s];
                break;
            }
        if (!schema && !allow_unknown) return false;
        if (schema && (entry.schema_version != schema->schema_version
                       || entry.record_bytes != schema->record_bytes
                       || !schema->alignment_bytes
                       || entry.file_offset % schema->alignment_bytes))
            return false;
        for (uint32_t j = 0; j < i; j++)
            if (entries[j].kind == entry.kind
                || overlaps(entries[j].file_offset,
                    entries[j].byte_length,
                    entry.file_offset, entry.byte_length)) return false;
        entries[i] = entry;
    }
    return true;
}
