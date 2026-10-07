#include "abi_link.h"

#include "abi_string.h"
#include "lib/alloc.h"
#include "lib/byteorder.h"
#include <string.h>

bool
n00bb_abi_object_open(const core_target_desc_t *target,
                      const uint8_t *object, size_t object_bytes,
                      n00bb_abi_object_view_t *out)
{
    n00bb_abi_image_key_t key;
    if (!out || !object || !object_bytes
        || !n00bb_abi_image_key_for_target(target, &key)) return false;
    const char *segment = nullptr, *name = nullptr;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        name = ".data.rel.ro.n00b_meta";
        break;
    case CORE_TARGET_OBJECT_MACH_O:
        segment = "__DATA_CONST";
        name = "__n00b_meta";
        break;
    case CORE_TARGET_OBJECT_COFF:
        name = ".n00b$M";
        break;
    default:
        return false;
    }
    core_object_section_t metadata = {0};
    if (!core_object_section_find(object, object_bytes,
            target->object_format, segment, name, &metadata)
        || metadata.size < N00BB_ABI_HEADER_BYTES) return false;
    size_t schema_count = 0;
    const n00bb_abi_table_schema_t *schemas =
        n00bb_abi_v1_table_schemas(&schema_count);
    uint32_t count = core_le_load_u32(metadata.bytes + 60);
    if (!schemas || count > schema_count
        || count > (metadata.size - N00BB_ABI_HEADER_BYTES)
                     / N00BB_ABI_DIRECTORY_BYTES) return false;
    n00bb_abi_directory_entry_t *directory = core_alloc_array(
        n00bb_abi_directory_entry_t, count);
    n00bb_abi_image_header_t header;
    if (!n00bb_abi_container_read(metadata.bytes, metadata.size, &key,
            schemas, schema_count, false, &header, directory, count)) {
        core_free(directory);
        return false;
    }
    *out = (n00bb_abi_object_view_t){
        .object = object, .object_bytes = object_bytes,
        .metadata = metadata, .format = target->object_format,
        .header = header,
        .directory = directory, .directory_count = count,
    };
    return true;
}

void
n00bb_abi_object_close(n00bb_abi_object_view_t *view)
{
    if (!view) return;
    core_free(view->directory);
    *view = (n00bb_abi_object_view_t){0};
}

const n00bb_abi_directory_entry_t *
n00bb_abi_object_table(const n00bb_abi_object_view_t *view, uint32_t kind)
{
    if (!view || !view->directory) return nullptr;
    for (size_t i = 0; i < view->directory_count; i++)
        if (view->directory[i].kind == kind) return &view->directory[i];
    return nullptr;
}

const void *
n00bb_abi_object_row(const n00bb_abi_object_view_t *view,
                     uint32_t kind, uint32_t index)
{
    const n00bb_abi_directory_entry_t *table =
        n00bb_abi_object_table(view, kind);
    if (!table || index >= table->record_count) return nullptr;
    return view->metadata.bytes + table->file_offset
        + (size_t)index * table->record_bytes;
}

bool
n00bb_abi_object_string(const n00bb_abi_object_view_t *view,
                        const n00b_abi_string_v1 *value,
                        n00bb_abi_string_view_t *out)
{
    if (!view || !view->directory || !value || !out) return false;
    uintptr_t base = (uintptr_t)view->metadata.bytes;
    uintptr_t address = (uintptr_t)value;
    if (address < base || address - base > view->metadata.size
        || sizeof(*value) > view->metadata.size - (address - base))
        return false;

    n00bb_abi_string_header_t header;
    if (!n00bb_abi_string_decode((const uint8_t *)value, sizeof(*value),
                                 &header)) return false;
    if (header.tag == N00BB_ABI_STRING_SHORT) {
        *out = (n00bb_abi_string_view_t){
            .bytes = value->short_value.bytes,
            .length = (size_t)header.length,
        };
        return true;
    }
    if (header.tag != N00BB_ABI_STRING_LONG) return false;

    const char *segment = view->format == CORE_TARGET_OBJECT_MACH_O
        ? "__TEXT" : nullptr;
    const char *name = view->format == CORE_TARGET_OBJECT_MACH_O
        ? "__n00b_strings" : view->format == CORE_TARGET_OBJECT_COFF
            ? ".rdata$S" : ".rodata.n00b_strings";
    core_object_section_t pool;
    if (!core_object_section_find(view->object, view->object_bytes,
            view->format, segment, name, &pool)) return false;

    bool found = false;
    uint64_t pool_offset = 0;
    uint64_t field_offset = address - base;
    for (uint32_t i = 0; i < view->metadata.relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        if (!core_object_absolute64_relocation_at(&view->metadata,
                view->format, i, &relocation)) return false;
        if (relocation.section_offset != field_offset) continue;
        if (found) return false;
        if (view->format == CORE_TARGET_OBJECT_MACH_O) {
            if (!relocation.target_is_section
                || relocation.native_symbol_index != pool.native_index
                || relocation.addend_bits < pool.virtual_address)
                return false;
            pool_offset = relocation.addend_bits - pool.virtual_address;
        }
        else {
            core_object_symbol_name_t symbol;
            if (relocation.target_is_section
                || !core_object_symbol_name_at(view->object,
                    view->object_bytes, view->format, &view->metadata,
                    relocation.native_symbol_index, &symbol)
                || symbol.length != sizeof("n00b_string_pool") - 1
                || memcmp(symbol.bytes, "n00b_string_pool",
                          symbol.length)) return false;
            pool_offset = relocation.addend_bits;
        }
        found = true;
    }
    if (!found || pool_offset > pool.size
        || header.length > pool.size - pool_offset) return false;
    *out = (n00bb_abi_string_view_t){
        .bytes = pool.bytes + pool_offset,
        .length = (size_t)header.length,
    };
    return true;
}
