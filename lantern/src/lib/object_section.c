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

static bool fixed_name(const uint8_t *base, size_t width, const char *name)
{
    size_t length = strlen(name);
    if (length > width || memcmp(base, name, length)) return false;
    for (size_t i = length; i < width; i++) {
        if (base[i]) return false;
    }
    return true;
}

static bool coff_name_view(const uint8_t *image, size_t size,
                           const uint8_t *header,
                           const uint8_t **name, size_t *length)
{
    if (header[0] != '/') {
        const uint8_t *end = memchr(header, 0, 8);
        *name = header;
        *length = end ? (size_t)(end - header) : 8;
        return true;
    }
    uint32_t string_index = 0;
    size_t cursor = 1;
    for (; cursor < 8 && header[cursor]; cursor++) {
        if (header[cursor] < '0' || header[cursor] > '9' ||
            string_index > (UINT32_MAX - (header[cursor] - '0')) / 10)
            return false;
        string_index = string_index * 10 + (header[cursor] - '0');
    }
    if (cursor == 1 || cursor == 8 || string_index < 4) return false;
    uint32_t symbol_offset = core_le_load_u32(image + 8);
    uint32_t symbol_count = core_le_load_u32(image + 12);
    if (!symbol_offset ||
        !table_span(size, symbol_offset, symbol_count, 18)) return false;
    uint64_t strings = (uint64_t)symbol_offset + (uint64_t)symbol_count * 18;
    if (!span(size, strings, 4)) return false;
    uint32_t string_bytes = core_le_load_u32(image + strings);
    if (string_index >= string_bytes ||
        !span(size, strings, string_bytes)) return false;
    const uint8_t *start = image + strings + string_index;
    const uint8_t *end = memchr(start, 0, string_bytes - string_index);
    if (!end) return false;
    *name = start;
    *length = (size_t)(end - start);
    return true;
}

static bool find_elf(const uint8_t *image, size_t size, const char *name,
                     uint32_t wanted_index,
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
        if (wanted_index && i != wanted_index) continue;
        if (name_offset >= names_size) return false;
        const uint8_t *start = image + names_offset + name_offset;
        const uint8_t *end = memchr(start, 0, names_size - name_offset);
        if (!end) return false;
        if (name && ((size_t)(end - start) != strlen(name)
            || memcmp(start, name, (size_t)(end - start)))) continue;
        uint32_t type = core_le_load_u32(header + 4);
        uint64_t offset = core_le_load_u64(header + 24);
        uint64_t length = core_le_load_u64(header + 32);
        if (type == 8 || !span(size, offset, length) || i > UINT32_MAX)
            return false;
        core_object_section_t result = {
            .bytes = image + offset,
            .size = (size_t)length,
            .name = start,
            .name_length = (size_t)(end - start),
            .native_index = (uint32_t)i,
            .virtual_address = core_le_load_u64(header + 16),
            .alignment = core_le_load_u64(header + 48),
            .flags = core_le_load_u64(header + 8),
            .machine = core_le_load_u16(image + 18),
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
            result.relocation_has_explicit_addend = rel_type == 4;
            result.native_symbol_table_index = core_le_load_u32(rel + 40);
        }
        *out = result;
        return true;
    }
    return false;
}

static bool find_macho(const uint8_t *image, size_t size,
                       const char *segment, const char *name,
                       uint32_t wanted_index,
                       core_object_section_t *out)
{
    if (!span(size, 0, 32) || core_le_load_u32(image) != 0xfeedfacf ||
        core_le_load_u32(image + 12) != 1 || (name && !segment)) return false;
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
                if (wanted_index && section_index != wanted_index) continue;
                if (name && (!fixed_name(section, 16, name) ||
                    !fixed_name(section + 16, 16, segment))) continue;
                const uint8_t *name_end = memchr(section, 0, 16);
                const uint8_t *segment_end = memchr(section + 16, 0, 16);
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
                    .name = section,
                    .name_length = name_end ? (size_t)(name_end - section)
                                            : 16,
                    .segment = section + 16,
                    .segment_length = segment_end
                        ? (size_t)(segment_end - (section + 16)) : 16,
                    .native_index = (uint32_t)section_index,
                    .virtual_address = core_le_load_u64(section + 32),
                    .alignment = UINT64_C(1) << alignment_power,
                    .flags = core_le_load_u32(section + 64),
                    .relocations = image + rel_offset,
                    .relocation_count = rel_count,
                    .relocation_entry_size = 8,
                    .machine = core_le_load_u32(image + 4),
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
                      uint32_t wanted_index,
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
        if (wanted_index && i + 1 != wanted_index) continue;
        const uint8_t *section_name;
        size_t name_length;
        if (!coff_name_view(image, size, header, &section_name,
                            &name_length)) return false;
        if (name && (name_length != strlen(name)
            || memcmp(section_name, name, name_length))) continue;
        uint32_t length = core_le_load_u32(header + 16);
        uint32_t offset = core_le_load_u32(header + 20);
        uint32_t rel_offset = core_le_load_u32(header + 24);
        uint16_t rel_count = core_le_load_u16(header + 32);
        uint32_t flags = core_le_load_u32(header + 36);
        if (!span(size, offset, length)) return false;
        uint32_t actual_rel_count = rel_count;
        uint64_t actual_rel_offset = rel_offset;
        if (flags & 0x01000000) {
            if (rel_count != UINT16_MAX || !span(size, rel_offset, 10))
                return false;
            uint32_t wire_count = core_le_load_u32(image + rel_offset);
            if (wire_count <= UINT16_MAX ||
                core_le_load_u32(image + rel_offset + 4) ||
                core_le_load_u16(image + rel_offset + 8) ||
                !table_span(size, rel_offset, wire_count, 10)) return false;
            actual_rel_count = wire_count - 1;
            actual_rel_offset += 10;
        }
        else if (!table_span(size, rel_offset, rel_count, 10)) return false;
        uint32_t alignment_power = (flags >> 20) & 15;
        core_object_section_t result = {
            .bytes = image + offset,
            .size = length,
            .name = section_name,
            .name_length = name_length,
            .native_index = i + 1,
            .virtual_address = core_le_load_u32(header + 12),
            .alignment = alignment_power ? UINT64_C(1) << (alignment_power - 1) : 1,
            .flags = flags,
            .relocations = image + actual_rel_offset,
            .relocation_count = actual_rel_count,
            .relocation_entry_size = 10,
            .machine = machine,
        };
        *out = result;
        return true;
    }
    return false;
}

bool core_object_absolute64_relocation_at(
    const core_object_section_t *section,
    core_target_object_format_t format, uint32_t index,
    core_object_absolute64_relocation_t *out)
{
    if (!section || !out || !section->relocations ||
        index >= section->relocation_count) return false;
    const uint8_t *raw = section->relocations +
                         (size_t)index * section->relocation_entry_size;
    core_object_absolute64_relocation_t result = {0};
    switch (format) {
    case CORE_TARGET_OBJECT_ELF: {
        if (section->relocation_entry_size <
            (section->relocation_has_explicit_addend ? 24u : 16u)) return false;
        uint64_t info = core_le_load_u64(raw + 8);
        uint32_t type = (uint32_t)info;
        if (!((section->machine == 62 && type == 1) ||
              (section->machine == 183 && type == 257))) return false;
        result.section_offset = core_le_load_u64(raw);
        result.native_symbol_index = (uint32_t)(info >> 32);
        break;
    }
    case CORE_TARGET_OBJECT_MACH_O: {
        if (section->relocation_entry_size != 8) return false;
        uint32_t address = core_le_load_u32(raw);
        uint32_t info = core_le_load_u32(raw + 4);
        if ((address & 0x80000000u) ||
            !(section->machine == 0x01000007 ||
              section->machine == 0x0100000c) ||
            ((info >> 24) & 1) || ((info >> 25) & 3) != 3 ||
            ((info >> 28) & 15) != 0) return false;
        result.section_offset = address;
        result.native_symbol_index = info & 0x00ffffffu;
        result.target_is_section = !((info >> 27) & 1);
        break;
    }
    case CORE_TARGET_OBJECT_COFF:
        if (section->relocation_entry_size != 10 ||
            section->machine != 0x8664 ||
            core_le_load_u16(raw + 8) != 1) return false;
        result.section_offset = core_le_load_u32(raw);
        result.native_symbol_index = core_le_load_u32(raw + 4);
        break;
    default:
        return false;
    }
    if (!span(section->size, result.section_offset, 8)) return false;
    result.addend_bits =
        format == CORE_TARGET_OBJECT_ELF && section->relocation_has_explicit_addend
            ? core_le_load_u64(raw + 16)
            : core_le_load_u64(section->bytes + result.section_offset);
    *out = result;
    return true;
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
        return find_elf(bytes, image_size, name, 0, out);
    case CORE_TARGET_OBJECT_MACH_O:
        return find_macho(bytes, image_size, segment, name, 0, out);
    case CORE_TARGET_OBJECT_COFF:
        return find_coff(bytes, image_size, name, 0, out);
    default:
        return false;
    }
}

bool core_object_section_count(const void *image, size_t image_size,
                               core_target_object_format_t format,
                               uint32_t *out_count)
{
    if (!image || !out_count) return false;
    const uint8_t *bytes = image;
    uint32_t count;
    switch (format) {
    case CORE_TARGET_OBJECT_ELF: {
        if (!span(image_size, 0, 64) ||
            memcmp(bytes, "\x7f" "ELF", 4) || bytes[4] != 2 ||
            bytes[5] != 1 || bytes[6] != 1 ||
            core_le_load_u16(bytes + 16) != 1) return false;
        uint64_t headers = core_le_load_u64(bytes + 40);
        uint16_t width = core_le_load_u16(bytes + 58);
        uint64_t sections = core_le_load_u16(bytes + 60);
        if (width < 64 || !span(image_size, headers, width)) return false;
        if (!sections) sections = core_le_load_u64(bytes + headers + 32);
        if (!sections || sections > UINT32_MAX + UINT64_C(1) ||
            !table_span(image_size, headers, sections, width)) return false;
        count = (uint32_t)(sections - 1);
        break;
    }
    case CORE_TARGET_OBJECT_MACH_O: {
        if (!span(image_size, 0, 32) ||
            core_le_load_u32(bytes) != 0xfeedfacf ||
            core_le_load_u32(bytes + 12) != 1) return false;
        uint32_t command_count = core_le_load_u32(bytes + 16);
        uint32_t command_bytes = core_le_load_u32(bytes + 20);
        if (!span(image_size, 32, command_bytes)) return false;
        size_t cursor = 32, end = 32 + command_bytes;
        count = 0;
        for (uint32_t i = 0; i < command_count; i++) {
            if (!span(end, cursor, 8)) return false;
            uint32_t command = core_le_load_u32(bytes + cursor);
            uint32_t size = core_le_load_u32(bytes + cursor + 4);
            if (size < 8 || !span(end, cursor, size)) return false;
            if (command == 0x19) {
                if (size < 72) return false;
                uint32_t sections = core_le_load_u32(bytes + cursor + 64);
                if (!table_span(size, 72, sections, 80)
                    || sections > UINT32_MAX - count) return false;
                count += sections;
            }
            cursor += size;
        }
        break;
    }
    case CORE_TARGET_OBJECT_COFF:
        if (!span(image_size, 0, 20) ||
            (core_le_load_u16(bytes) != 0x8664
                && core_le_load_u16(bytes) != 0xaa64) ||
            core_le_load_u16(bytes + 16) != 0) return false;
        count = core_le_load_u16(bytes + 2);
        if (!table_span(image_size, 20, count, 40)) return false;
        break;
    default:
        return false;
    }
    *out_count = count;
    return true;
}

bool core_object_section_at(const void *image, size_t image_size,
                            core_target_object_format_t format,
                            uint32_t native_index,
                            core_object_section_t *out)
{
    if (!image || !out || !native_index) return false;
    uint32_t count;
    if (!core_object_section_count(image, image_size, format, &count)
        || native_index > count) return false;
    const uint8_t *bytes = image;
    switch (format) {
    case CORE_TARGET_OBJECT_ELF:
        return find_elf(bytes, image_size, nullptr, native_index, out);
    case CORE_TARGET_OBJECT_MACH_O:
        return find_macho(bytes, image_size, nullptr, nullptr, native_index,
                          out);
    case CORE_TARGET_OBJECT_COFF:
        return find_coff(bytes, image_size, nullptr, native_index, out);
    default:
        return false;
    }
}

static bool string_name(const uint8_t *data, size_t size, uint32_t offset,
                        core_object_symbol_name_t *out)
{
    if (offset >= size) return false;
    const uint8_t *end = memchr(data + offset, 0, size - offset);
    if (!end) return false;
    *out = (core_object_symbol_name_t){data + offset,
                                       (size_t)(end - (data + offset))};
    return true;
}

static bool elf_symbol_name(const uint8_t *image, size_t size,
                            uint32_t table_index, uint32_t symbol_index,
                            core_object_symbol_name_t *out)
{
    if (!span(size, 0, 64) || memcmp(image, "\x7f" "ELF", 4) ||
        image[4] != 2 || image[5] != 1) return false;
    uint64_t headers = core_le_load_u64(image + 40);
    uint16_t width = core_le_load_u16(image + 58);
    uint64_t count = core_le_load_u16(image + 60);
    if (width < 64 || !span(size, headers, width)) return false;
    if (!count) count = core_le_load_u64(image + headers + 32);
    if (!table_index || table_index >= count ||
        !table_span(size, headers, count, width)) return false;
    const uint8_t *table = image + headers + table_index * width;
    uint32_t kind = core_le_load_u32(table + 4);
    if (kind != 2 && kind != 11) return false;
    uint64_t offset = core_le_load_u64(table + 24);
    uint64_t length = core_le_load_u64(table + 32);
    uint64_t entry_size = core_le_load_u64(table + 56);
    uint32_t strings_index = core_le_load_u32(table + 40);
    if (entry_size < 24 || !entry_size || length % entry_size ||
        symbol_index >= length / entry_size ||
        strings_index >= count || !span(size, offset, length)) return false;
    const uint8_t *strings = image + headers + strings_index * width;
    uint64_t string_offset = core_le_load_u64(strings + 24);
    uint64_t string_length = core_le_load_u64(strings + 32);
    if (core_le_load_u32(strings + 4) != 3 ||
        !span(size, string_offset, string_length)) return false;
    uint32_t name_offset =
        core_le_load_u32(image + offset + (uint64_t)symbol_index * entry_size);
    return string_name(image + string_offset, (size_t)string_length,
                       name_offset, out);
}

static bool macho_symbol_name(const uint8_t *image, size_t size,
                              uint32_t symbol_index,
                              core_object_symbol_name_t *out)
{
    if (!span(size, 0, 32) || core_le_load_u32(image) != 0xfeedfacf ||
        core_le_load_u32(image + 12) != 1) return false;
    uint32_t count = core_le_load_u32(image + 16);
    uint32_t command_bytes = core_le_load_u32(image + 20);
    if (!span(size, 32, command_bytes)) return false;
    size_t cursor = 32, end = 32 + command_bytes;
    for (uint32_t i = 0; i < count; i++) {
        if (!span(end, cursor, 8)) return false;
        uint32_t command = core_le_load_u32(image + cursor);
        uint32_t length = core_le_load_u32(image + cursor + 4);
        if (length < 8 || !span(end, cursor, length)) return false;
        if (command == 2) {
            if (length < 24) return false;
            uint32_t symbol_offset = core_le_load_u32(image + cursor + 8);
            uint32_t symbol_count = core_le_load_u32(image + cursor + 12);
            uint32_t string_offset = core_le_load_u32(image + cursor + 16);
            uint32_t string_bytes = core_le_load_u32(image + cursor + 20);
            if (symbol_index >= symbol_count ||
                !table_span(size, symbol_offset, symbol_count, 16) ||
                !span(size, string_offset, string_bytes)) return false;
            uint32_t name_offset = core_le_load_u32(
                image + symbol_offset + (uint64_t)symbol_index * 16);
            return string_name(image + string_offset, string_bytes,
                               name_offset, out);
        }
        cursor += length;
    }
    return false;
}

static bool coff_symbol_name(const uint8_t *image, size_t size,
                             uint32_t symbol_index,
                             core_object_symbol_name_t *out)
{
    if (!span(size, 0, 20) || core_le_load_u16(image + 16)) return false;
    uint32_t symbol_offset = core_le_load_u32(image + 8);
    uint32_t count = core_le_load_u32(image + 12);
    if (!symbol_offset || symbol_index >= count ||
        !table_span(size, symbol_offset, count, 18)) return false;
    for (uint32_t i = 0; i < symbol_index;) {
        uint8_t auxiliary = image[symbol_offset + (uint64_t)i * 18 + 17];
        if ((uint64_t)i + 1 + auxiliary > symbol_index) return false;
        i += 1 + auxiliary;
    }
    const uint8_t *entry = image + symbol_offset +
                           (uint64_t)symbol_index * 18;
    if (core_le_load_u32(entry)) {
        size_t length = 0;
        while (length < 8 && entry[length]) length++;
        *out = (core_object_symbol_name_t){entry, length};
        return true;
    }
    uint64_t strings = (uint64_t)symbol_offset + (uint64_t)count * 18;
    if (!span(size, strings, 4)) return false;
    uint32_t string_bytes = core_le_load_u32(image + strings);
    uint32_t name_offset = core_le_load_u32(entry + 4);
    if (string_bytes < 4 || !span(size, strings, string_bytes) ||
        name_offset < 4) return false;
    return string_name(image + strings, string_bytes, name_offset, out);
}

bool core_object_symbol_name_at(const void *image, size_t image_size,
                                core_target_object_format_t format,
                                const core_object_section_t *section,
                                uint32_t native_symbol_index,
                                core_object_symbol_name_t *out)
{
    if (!image || !section || !out) return false;
    const uint8_t *bytes = image;
    switch (format) {
    case CORE_TARGET_OBJECT_ELF:
        return elf_symbol_name(bytes, image_size,
                               section->native_symbol_table_index,
                               native_symbol_index, out);
    case CORE_TARGET_OBJECT_MACH_O:
        return macho_symbol_name(bytes, image_size, native_symbol_index, out);
    case CORE_TARGET_OBJECT_COFF:
        return coff_symbol_name(bytes, image_size, native_symbol_index, out);
    default:
        return false;
    }
}
