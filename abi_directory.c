#include "abi_directory.h"
#include "n00b-abi-v1.h"

#include "lib/byteorder.h"

#define SCHEMA(name, type) {N00B_ABI_TABLE_##name, \
    N00B_ABI_V1_TABLE_SCHEMA_VERSION, sizeof(type), _Alignof(type)}

static const n00bb_abi_table_schema_t v1_schemas[] = {
    SCHEMA(PACKAGE, n00b_abi_package_v1),
    SCHEMA(MODULE, n00b_abi_module_v1),
    SCHEMA(SCC, n00b_abi_scc_v1),
    SCHEMA(TYPE, n00b_abi_type_v1),
    SCHEMA(LAYOUT, n00b_abi_layout_v1),
    SCHEMA(FIELD, n00b_abi_field_v1),
    SCHEMA(ARM, n00b_abi_arm_v1),
    SCHEMA(PACKED_RUN, n00b_abi_packed_run_v1),
    SCHEMA(PHYSICAL_RUN, n00b_abi_physical_run_v1),
    SCHEMA(TRACE, n00b_abi_trace_v1),
    SCHEMA(TRACE_CHILD, n00b_abi_trace_child_v1),
    SCHEMA(TRACE_ARM, n00b_abi_trace_arm_v1),
    SCHEMA(TAG_EXTRACTOR, n00b_abi_tag_extractor_v1),
    SCHEMA(FUNCTION, n00b_abi_function_v1),
    SCHEMA(PARAMETER, n00b_abi_parameter_v1),
    SCHEMA(CODE_RANGE, n00b_abi_code_range_v1),
    SCHEMA(SCOPE, n00b_abi_scope_v1),
    SCHEMA(SYMBOL, n00b_abi_symbol_v1),
    SCHEMA(NATIVE_BINDING, n00b_abi_native_binding_v1),
    SCHEMA(TRAIT_METHOD, n00b_abi_trait_method_v1),
    SCHEMA(DISPATCH, n00b_abi_dispatch_v1),
    SCHEMA(DISPATCH_ENTRY, n00b_abi_dispatch_entry_v1),
    SCHEMA(FRAME_LAYOUT, n00b_abi_frame_layout_v1),
    SCHEMA(SOURCE_FILE, n00b_abi_source_file_v1),
    SCHEMA(SOURCE_LOCATION, n00b_abi_source_location_v1),
    SCHEMA(STORAGE, n00b_abi_storage_v1),
    SCHEMA(TLS_RELOCATION, n00b_abi_tls_relocation_v1),
    SCHEMA(STATIC_STRING, n00b_abi_static_string_v1),
    SCHEMA(NAME_SLOT, n00b_abi_name_slot_v1),
    SCHEMA(REQUIREMENT, n00b_abi_requirement_v1),
    SCHEMA(INDEX_PATCH, n00b_abi_index_patch_v1),
    SCHEMA(U32_STREAM, uint32_t),
    SCHEMA(BYTE_STREAM, uint8_t),
    SCHEMA(U64_STREAM, uint64_t),
    SCHEMA(CONSTRAINT, n00b_abi_constraint_v1),
    SCHEMA(SHAPE, n00b_abi_shape_v1),
    SCHEMA(ENUM_ITEM, n00b_abi_enum_item_v1),
    SCHEMA(RUNTIME_CONFIG, n00b_abi_runtime_config_v1),
    SCHEMA(CALL_SIGNATURE, n00b_abi_call_signature_v1),
    SCHEMA(SPECIALIZATION_LINK, n00b_abi_specialization_link_v1),
};

const n00bb_abi_table_schema_t *
n00bb_abi_v1_table_schemas(size_t *count)
{
    if (count) *count = sizeof(v1_schemas) / sizeof(*v1_schemas);
    return v1_schemas;
}

#undef SCHEMA

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
