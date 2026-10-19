#include "llvm_stack_maps.h"

static uint16_t
read16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t
read32(const uint8_t *bytes)
{
    return (uint32_t)read16(bytes) | ((uint32_t)read16(bytes + 2) << 16);
}

static uint64_t
read64(const uint8_t *bytes)
{
    return (uint64_t)read32(bytes) | ((uint64_t)read32(bytes + 4) << 32);
}

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
function_name(const void *object, size_t object_bytes,
              core_target_object_format_t format,
              const core_object_section_t *section, size_t offset,
              core_object_symbol_name_t *out)
{
    for (uint32_t i = 0; i < section->relocation_count; i++) {
        core_object_absolute64_relocation_t relocation;
        if (!core_object_absolute64_relocation_at(section, format, i,
                                                  &relocation)) return false;
        if (relocation.section_offset != offset) continue;
        if (relocation.target_is_section) return false;
        return core_object_symbol_name_at(object, object_bytes, format,
            section, relocation.native_symbol_index, out);
    }
    return false;
}

bool
n00bb_llvm_stack_maps_visit(const void *object, size_t object_bytes,
    core_target_object_format_t format, n00bb_llvm_stack_location_fn visit,
    void *context, bool *found)
{
    if (!object || !visit || !found) return false;
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
    if (length < 16 || bytes[0] != 3 || bytes[1] || read16(bytes + 2))
        return false;
    uint32_t function_count = read32(bytes + 4);
    uint32_t constant_count = read32(bytes + 8);
    uint32_t record_count = read32(bytes + 12);
    size_t cursor = 16, at;
    if (function_count > (length - cursor) / 24) return false;
    size_t function_base = cursor;
    cursor += (size_t)function_count * 24;
    if (constant_count > (length - cursor) / 8) return false;
    cursor += (size_t)constant_count * 8;
    uint64_t visited_records = 0;
    for (uint32_t f = 0; f < function_count; f++) {
        size_t row = function_base + (size_t)f * 24;
        core_object_symbol_name_t symbol;
        if (!function_name(object, object_bytes, format, &section, row,
                           &symbol)) return false;
        uint64_t stack_bytes = read64(bytes + row + 8);
        uint64_t records = read64(bytes + row + 16);
        if (records > record_count - visited_records) return false;
        for (uint64_t r = 0; r < records; r++) {
            if (!take(&cursor, 16, length, &at)) return false;
            uint64_t id = read64(bytes + at);
            uint32_t instruction_offset = read32(bytes + at + 8);
            uint16_t locations = read16(bytes + at + 14);
            if (locations > (length - cursor) / 12) return false;
            for (uint16_t i = 0; i < locations; i++) {
                if (!take(&cursor, 12, length, &at)) return false;
                n00bb_llvm_stack_location_t location = {
                    .function = symbol,
                    .id = id,
                    .stack_bytes = stack_bytes,
                    .instruction_offset = instruction_offset,
                    .kind = bytes[at],
                    .dwarf_register = read16(bytes + at + 4),
                    .size_bytes = read16(bytes + at + 2),
                    .frame_offset = (int32_t)read32(bytes + at + 8),
                };
                if (!visit(context, &location)) return false;
            }
            if (!align8(&cursor, length) || !take(&cursor, 4, length, &at))
                return false;
            uint16_t live_outs = read16(bytes + at);
            if (live_outs > (length - cursor) / 4) return false;
            cursor += (size_t)live_outs * 4;
            if (!align8(&cursor, length)) return false;
        }
        visited_records += records;
    }
    return visited_records == record_count && cursor == length;
}
