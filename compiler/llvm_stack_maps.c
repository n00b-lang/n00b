#include "llvm_stack_maps.h"

#include "lib/byteorder.h"

static bool
take(size_t *cursor, size_t count, size_t total, size_t *at)
{
    if (count > total || *cursor > total - count) return false;
    *at = *cursor;
    *cursor += count;
    return true;
}

static bool
align8(size_t *cursor, size_t total)
{
    size_t padding = (8 - (*cursor & 7)) & 7;
    size_t ignored;
    return take(cursor, padding, total, &ignored);
}

static bool
function_symbol(const void *object, size_t object_bytes,
                core_target_object_format_t format,
                const core_object_section_t *section, size_t offset,
                n00bb_llvm_stack_function_t *out)
{
    for (uint32_t i = 0; i < section->relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        if (!core_object_absolute64_relocation_at(section, format, i,
                                                  &relocation)) return false;
        if (relocation.section_offset != offset) continue;
        core_object_symbol_t symbol;
        if (relocation.target_is_section
            || !core_object_symbol_at(object, object_bytes, format, section,
                   relocation.native_symbol_index, &symbol)
            || !core_object_symbol_extent(object, object_bytes, format,
                   section, relocation.native_symbol_index, &out->code_bytes))
            return false;
        out->name = symbol.name;
        out->section = symbol.native_section_index;
        out->offset = symbol.section_offset;
        return true;
    }
    return false;
}

bool
n00bb_llvm_stack_maps_visit(const void *object, size_t object_bytes,
    core_target_object_format_t format,
    const n00bb_llvm_stack_visitor_t *visitor, bool *found)
{
    if (!object || !visitor || !visitor->location || !found) return false;
    *found = false;
    core_object_section_t section;
    core_string_t segment = format == CORE_TARGET_OBJECT_MACH_O
        ? CORE_STRING_STATIC("__LLVM_STACKMAPS") : (core_string_t){};
    core_string_t name = format == CORE_TARGET_OBJECT_MACH_O
        ? CORE_STRING_STATIC("__llvm_stackmaps")
        : CORE_STRING_STATIC(".llvm_stackmaps");
    if (!core_object_section_find(object, object_bytes, format, segment,
                                  name, &section)) return true;
    *found = true;
    const uint8_t *bytes = section.bytes;
    size_t length = section.size;
    if (length < 16 || bytes[0] != 3 || bytes[1] || core_le_load_u16(bytes + 2))
        return false;
    uint32_t function_count = core_le_load_u32(bytes + 4);
    uint32_t constant_count = core_le_load_u32(bytes + 8);
    uint32_t record_count = core_le_load_u32(bytes + 12);
    size_t cursor = 16, at;
    if (function_count > (length - cursor) / 24) return false;
    size_t function_base = cursor;
    cursor += (size_t)function_count * 24;
    if (constant_count > (length - cursor) / 8) return false;
    cursor += (size_t)constant_count * 8;
    uint64_t visited_records = 0;
    for (uint32_t f = 0; f < function_count; f++) {
        size_t row = function_base + (size_t)f * 24;
        n00bb_llvm_stack_function_t function = {
            .stack_bytes = core_le_load_u64(bytes + row + 8),
        };
        if (!function_symbol(object, object_bytes, format, &section, row,
                             &function)
            || (visitor->function
                && !visitor->function(visitor->context, &function)))
            return false;
        uint64_t records = core_le_load_u64(bytes + row + 16);
        if (records > record_count - visited_records) return false;
        for (uint64_t r = 0; r < records; r++) {
            if (!take(&cursor, 16, length, &at)) return false;
            uint64_t id = core_le_load_u64(bytes + at);
            uint32_t instruction_offset = core_le_load_u32(bytes + at + 8);
            uint16_t locations = core_le_load_u16(bytes + at + 14);
            if (locations > (length - cursor) / 12) return false;
            for (uint16_t i = 0; i < locations; i++) {
                if (!take(&cursor, 12, length, &at)) return false;
                n00bb_llvm_stack_location_t location = {
                    .function = &function,
                    .id = id,
                    .instruction_offset = instruction_offset,
                    .kind = bytes[at],
                    .dwarf_register = core_le_load_u16(bytes + at + 4),
                    .size_bytes = core_le_load_u16(bytes + at + 2),
                    .frame_offset = (int32_t)core_le_load_u32(bytes + at + 8),
                };
                if (!visitor->location(visitor->context, &location))
                    return false;
            }
            if (!align8(&cursor, length) || !take(&cursor, 4, length, &at))
                return false;
            uint16_t live_outs = core_le_load_u16(bytes + at);
            if (live_outs > (length - cursor) / 4) return false;
            cursor += (size_t)live_outs * 4;
            if (!align8(&cursor, length)) return false;
        }
        visited_records += records;
    }
    return visited_records == record_count && cursor == length;
}
