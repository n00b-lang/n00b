#include "lib/object_section.h"

#include <string.h>

#include "lib/byteorder.h"

static bool span(size_t total, uint64_t offset, uint64_t length)
{
    return offset <= total && length <= total - offset;
}

static bool table_span(size_t total, uint64_t offset, uint64_t count,
                       uint64_t width)
{
    return width && count <= UINT64_MAX / width &&
           span(total, offset, count * width);
}

static bool bounded_name(const uint8_t *base, size_t size, uint64_t offset,
                         const char *name)
{
    size_t length = strlen(name);
    return span(size, offset, length + 1) &&
           !memcmp(base + offset, name, length) && !base[offset + length];
}

static bool fixed_name(const uint8_t *base, size_t width, const char *name)
{
    size_t length = strlen(name);
    if (length > width || memcmp(base, name, length)) return false;
    for (size_t i = length; i < width; i++) {
        if (base[i]) return false;
    }
    return true;
}

static bool find_elf(const uint8_t *image, size_t size, const char *name,
                     core_object_section_t *out)
{
    if (!span(size, 0, 64) || memcmp(image, "\x7f" "ELF", 4) ||
        image[4] != 2 || image[5] != 1 || image[6] != 1 ||
        core_le_load_u16(image + 16) != 1) return false;

    uint64_t headers = core_le_load_u64(image + 40);
    uint16_t width = core_le_load_u16(image + 58);
    uint64_t count = core_le_load_u16(image + 60);
    uint64_t names_index = core_le_load_u16(image + 62);
    if (width < 64 || !span(size, headers, width)) return false;
    const uint8_t *first = image + headers;
    if (!count) count = core_le_load_u64(first + 32);
    if (names_index == 0xffff) names_index = core_le_load_u32(first + 40);
    if (!count || names_index >= count ||
        !table_span(size, headers, count, width)) return false;

    const uint8_t *names_header = image + headers + names_index * width;
    uint64_t names_offset = core_le_load_u64(names_header + 24);
    uint64_t names_size = core_le_load_u64(names_header + 32);
    if (core_le_load_u32(names_header + 4) != 3 ||
        !span(size, names_offset, names_size)) return false;

    for (uint64_t i = 1; i < count; i++) {
        const uint8_t *header = image + headers + i * width;
        uint32_t name_offset = core_le_load_u32(header);
        if (name_offset >= names_size ||
            !bounded_name(image + names_offset, (size_t)names_size,
                          name_offset, name)) continue;
        uint32_t type = core_le_load_u32(header + 4);
        uint64_t offset = core_le_load_u64(header + 24);
        uint64_t length = core_le_load_u64(header + 32);
        if (type == 8 || !span(size, offset, length) || i > UINT32_MAX)
            return false;
        core_object_section_t result = {
            .bytes = image + offset,
            .size = (size_t)length,
            .native_index = (uint32_t)i,
            .alignment = core_le_load_u64(header + 48),
            .flags = core_le_load_u64(header + 8),
        };
        for (uint64_t j = 1; j < count; j++) {
            const uint8_t *rel = image + headers + j * width;
            uint32_t rel_type = core_le_load_u32(rel + 4);
            if ((rel_type != 4 && rel_type != 9) ||
                core_le_load_u32(rel + 44) != i) continue;
            uint64_t rel_offset = core_le_load_u64(rel + 24);
            uint64_t rel_size = core_le_load_u64(rel + 32);
            uint64_t entry_size = core_le_load_u64(rel + 56);
            uint64_t minimum = rel_type == 4 ? 24 : 16;
            if (result.relocations || entry_size < minimum ||
                !entry_size || rel_size % entry_size ||
                !span(size, rel_offset, rel_size) ||
                rel_size / entry_size > UINT32_MAX ||
                entry_size > UINT32_MAX) return false;
            result.relocations = image + rel_offset;
            result.relocation_count = (uint32_t)(rel_size / entry_size);
            result.relocation_entry_size = (uint32_t)entry_size;
        }
        *out = result;
        return true;
    }
    return false;
}

static bool find_macho(const uint8_t *image, size_t size,
                       const char *segment, const char *name,
                       core_object_section_t *out)
{
    if (!span(size, 0, 32) || core_le_load_u32(image) != 0xfeedfacf ||
        core_le_load_u32(image + 12) != 1 || !segment) return false;
    uint32_t command_count = core_le_load_u32(image + 16);
    uint32_t command_bytes = core_le_load_u32(image + 20);
    if (!span(size, 32, command_bytes)) return false;
    size_t cursor = 32, command_end = 32 + command_bytes;
    uint64_t section_index = 0;
    for (uint32_t i = 0; i < command_count; i++) {
        if (!span(command_end, cursor, 8)) return false;
        uint32_t command = core_le_load_u32(image + cursor);
        uint32_t command_size = core_le_load_u32(image + cursor + 4);
        if (command_size < 8 || !span(command_end, cursor, command_size))
            return false;
        if (command == 0x19) {
            if (command_size < 72) return false;
            uint32_t nsections = core_le_load_u32(image + cursor + 64);
            if (!table_span(command_size, 72, nsections, 80)) return false;
            for (uint32_t j = 0; j < nsections; j++) {
                section_index++;
                const uint8_t *section = image + cursor + 72 + j * 80;
                if (!fixed_name(section, 16, name) ||
                    !fixed_name(section + 16, 16, segment)) continue;
                uint64_t length = core_le_load_u64(section + 40);
                uint32_t offset = core_le_load_u32(section + 48);
                uint32_t alignment_power = core_le_load_u32(section + 52);
                uint32_t rel_offset = core_le_load_u32(section + 56);
                uint32_t rel_count = core_le_load_u32(section + 60);
                if (section_index > UINT32_MAX || alignment_power >= 64 ||
                    !span(size, offset, length) ||
                    !table_span(size, rel_offset, rel_count, 8)) return false;
                core_object_section_t result = {
                    .bytes = image + offset,
                    .size = (size_t)length,
                    .native_index = (uint32_t)section_index,
                    .alignment = UINT64_C(1) << alignment_power,
                    .flags = core_le_load_u32(section + 64),
                    .relocations = image + rel_offset,
                    .relocation_count = rel_count,
                    .relocation_entry_size = 8,
                };
                *out = result;
                return true;
            }
        }
        cursor += command_size;
    }
    return false;
}

static bool find_coff(const uint8_t *image, size_t size, const char *name,
                      core_object_section_t *out)
{
    if (!span(size, 0, 20)) return false;
    uint16_t machine = core_le_load_u16(image);
    if (machine != 0x8664 && machine != 0xaa64) return false;
    uint16_t count = core_le_load_u16(image + 2);
    uint16_t optional_size = core_le_load_u16(image + 16);
    if (optional_size || !table_span(size, 20, count, 40)) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *header = image + 20 + i * 40;
        if (!fixed_name(header, 8, name)) continue;
        uint32_t length = core_le_load_u32(header + 16);
        uint32_t offset = core_le_load_u32(header + 20);
        uint32_t rel_offset = core_le_load_u32(header + 24);
        uint16_t rel_count = core_le_load_u16(header + 32);
        uint32_t flags = core_le_load_u32(header + 36);
        if (!span(size, offset, length) ||
            !table_span(size, rel_offset, rel_count, 10) ||
            (flags & 0x01000000)) return false;
        uint32_t alignment_power = (flags >> 20) & 15;
        core_object_section_t result = {
            .bytes = image + offset,
            .size = length,
            .native_index = i + 1,
            .alignment = alignment_power ? UINT64_C(1) << (alignment_power - 1) : 1,
            .flags = flags,
            .relocations = image + rel_offset,
            .relocation_count = rel_count,
            .relocation_entry_size = 10,
        };
        *out = result;
        return true;
    }
    return false;
}

bool core_object_section_find(const void *image, size_t image_size,
                              core_target_object_format_t format,
                              const char *segment, const char *name,
                              core_object_section_t *out)
{
    if (!image || !name || !out) return false;
    const uint8_t *bytes = image;
    switch (format) {
    case CORE_TARGET_OBJECT_ELF:
        return find_elf(bytes, image_size, name, out);
    case CORE_TARGET_OBJECT_MACH_O:
        return find_macho(bytes, image_size, segment, name, out);
    case CORE_TARGET_OBJECT_COFF:
        return find_coff(bytes, image_size, name, out);
    default:
        return false;
    }
}
