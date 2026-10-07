#include "lib/object_emit_internal.h"

#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"

enum {
    ELF_HEADER_BYTES = 64,
    ELF_SECTION_BYTES = 64,
    ELF_SYMBOL_BYTES = 24,
    ELF_RELA_BYTES = 24,
    ELF_USER_SECTION_BASE = 4,
    ELF_RESERVED_SECTION_START = 0xff00,
};

bool core_object_emit_elf_native_section_index(size_t section_count,
                                               uint32_t input_section_index,
                                               uint32_t *native_section_index)
{
    if (!native_section_index || !input_section_index ||
        input_section_index > section_count ||
        section_count > ELF_RESERVED_SECTION_START - ELF_USER_SECTION_BASE)
        return false;
    *native_section_index = ELF_USER_SECTION_BASE + input_section_index - 1;
    return true;
}

static bool add_cstring(size_t *size, const char *value)
{
    size_t length = strlen(value);
    return length < SIZE_MAX && core_object_size_add(*size, length + 1, size) &&
           *size <= UINT32_MAX;
}

static void write_section_header(uint8_t *header, uint32_t name, uint32_t type,
                                 uint64_t flags, uint64_t offset, uint64_t size,
                                 uint32_t link, uint32_t info, uint64_t alignment,
                                 uint64_t entry_size)
{
    core_le_store_u32(header, name);
    core_le_store_u32(header + 4, type);
    core_le_store_u64(header + 8, flags);
    core_le_store_u64(header + 24, offset);
    core_le_store_u64(header + 32, size);
    core_le_store_u32(header + 40, link);
    core_le_store_u32(header + 44, info);
    core_le_store_u64(header + 48, alignment);
    core_le_store_u64(header + 56, entry_size);
}

bool core_object_emit_elf(const core_target_desc_t *target,
                          const core_object_emit_section_t *sections,
                          size_t section_count,
                          const core_object_emit_symbol_t *symbols,
                          size_t symbol_count,
                          const core_object_emit_absolute64_t *relocations,
                          size_t relocation_count,
                          uint8_t **out_image, size_t *out_size)
{
    if (target->call_abi != CORE_TARGET_CALL_SYSV_AMD64 ||
        section_count > ELF_RESERVED_SECTION_START - ELF_USER_SECTION_BASE ||
        symbol_count >= UINT32_MAX) return false;

    uint32_t *rel_counts = core_alloc_array(uint32_t, section_count);
    size_t *data_offsets = core_alloc_array(size_t, section_count);
    size_t *rel_offsets = core_alloc_array(size_t, section_count);
    uint32_t *rel_written = core_alloc_array(uint32_t, section_count);
    uint32_t *section_names = core_alloc_array(uint32_t, section_count);
    uint32_t *rel_names = core_alloc_array(uint32_t, section_count);
    uint32_t *symbol_names = core_alloc_array(uint32_t, symbol_count);
    uint32_t *symbol_map = core_alloc_array(uint32_t, symbol_count);
    uint8_t *image = NULL;
    bool success = false;

    for (size_t i = 0; i < relocation_count; i++)
        rel_counts[relocations[i].section_index - 1]++;
    size_t rel_section_count = 0, local_count = 0;
    for (size_t i = 0; i < section_count; i++)
        rel_section_count += rel_counts[i] != 0;
    for (size_t i = 0; i < symbol_count; i++)
        local_count += !symbols[i].exported;

    size_t section_headers, shstr_size = 1, str_size = 1;
    if (!core_object_size_add(ELF_USER_SECTION_BASE, section_count,
                              &section_headers) ||
        !core_object_size_add(section_headers, rel_section_count,
                              &section_headers) ||
        section_headers > UINT32_MAX ||
        !add_cstring(&shstr_size, ".shstrtab") ||
        !add_cstring(&shstr_size, ".strtab") ||
        !add_cstring(&shstr_size, ".symtab")) goto done;
    for (size_t i = 0; i < section_count; i++) {
        if (!add_cstring(&shstr_size, sections[i].name)) goto done;
        if (rel_counts[i] &&
            (!core_object_size_add(shstr_size, 5, &shstr_size) ||
             !add_cstring(&shstr_size, sections[i].name))) goto done;
    }
    for (size_t i = 0; i < symbol_count; i++)
        if (!add_cstring(&str_size, symbols[i].name)) goto done;

    size_t symtab_bytes, shdr_bytes, offset = ELF_HEADER_BYTES;
    if (!core_object_size_mul(symbol_count + 1, ELF_SYMBOL_BYTES,
                              &symtab_bytes) ||
        !core_object_size_mul(section_headers, ELF_SECTION_BYTES,
                              &shdr_bytes)) goto done;
    size_t shstr_offset = offset;
    if (!core_object_size_add(offset, shstr_size, &offset)) goto done;
    size_t str_offset = offset;
    if (!core_object_size_add(offset, str_size, &offset) ||
        !core_object_size_align(offset, 8, &offset)) goto done;
    size_t symtab_offset = offset;
    if (!core_object_size_add(offset, symtab_bytes, &offset)) goto done;
    for (size_t i = 0; i < section_count; i++) {
        if (!core_object_size_align(offset, sections[i].alignment, &offset))
            goto done;
        data_offsets[i] = offset;
        if (!core_object_size_add(offset, sections[i].size, &offset)) goto done;
    }
    for (size_t i = 0; i < section_count; i++) {
        if (!rel_counts[i]) continue;
        size_t bytes;
        if (!core_object_size_mul(rel_counts[i], ELF_RELA_BYTES, &bytes) ||
            !core_object_size_align(offset, 8, &offset)) goto done;
        rel_offsets[i] = offset;
        if (!core_object_size_add(offset, bytes, &offset)) goto done;
    }
    if (!core_object_size_align(offset, 8, &offset)) goto done;
    size_t shdr_offset = offset;
    if (!core_object_size_add(offset, shdr_bytes, &offset)) goto done;
    image = core_alloc_size(offset, 1);

    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 2;
    image[5] = 1;
    image[6] = 1;
    core_le_store_u16(image + 16, 1);
    core_le_store_u16(image + 18, 62);
    core_le_store_u32(image + 20, 1);
    core_le_store_u64(image + 40, shdr_offset);
    core_le_store_u16(image + 52, ELF_HEADER_BYTES);
    core_le_store_u16(image + 58, ELF_SECTION_BYTES);
    core_le_store_u16(image + 60, section_headers >= ELF_RESERVED_SECTION_START
                                         ? 0 : (uint16_t)section_headers);
    core_le_store_u16(image + 62, 1);
    if (section_headers >= ELF_RESERVED_SECTION_START)
        core_le_store_u64(image + shdr_offset + 32, section_headers);

    size_t string_cursor = shstr_offset + 1;
    uint32_t shstr_name = 1;
    memcpy(image + string_cursor, ".shstrtab", 10);
    string_cursor += 10;
    uint32_t str_name = (uint32_t)(string_cursor - shstr_offset);
    memcpy(image + string_cursor, ".strtab", 8);
    string_cursor += 8;
    uint32_t symtab_name = (uint32_t)(string_cursor - shstr_offset);
    memcpy(image + string_cursor, ".symtab", 8);
    string_cursor += 8;
    for (size_t i = 0; i < section_count; i++) {
        size_t length = strlen(sections[i].name) + 1;
        section_names[i] = (uint32_t)(string_cursor - shstr_offset);
        memcpy(image + string_cursor, sections[i].name, length);
        string_cursor += length;
        if (rel_counts[i]) {
            rel_names[i] = (uint32_t)(string_cursor - shstr_offset);
            memcpy(image + string_cursor, ".rela", 5);
            string_cursor += 5;
            memcpy(image + string_cursor, sections[i].name, length);
            string_cursor += length;
        }
    }
    string_cursor = str_offset + 1;
    for (size_t i = 0; i < symbol_count; i++) {
        size_t length = strlen(symbols[i].name) + 1;
        symbol_names[i] = (uint32_t)(string_cursor - str_offset);
        memcpy(image + string_cursor, symbols[i].name, length);
        string_cursor += length;
    }

    uint32_t local_next = 1;
    uint32_t global_next = (uint32_t)local_count + 1;
    for (size_t i = 0; i < symbol_count; i++) {
        uint32_t index = symbols[i].exported ? global_next++ : local_next++;
        symbol_map[i] = index;
        uint8_t *entry = image + symtab_offset + (size_t)index * ELF_SYMBOL_BYTES;
        core_le_store_u32(entry, symbol_names[i]);
        entry[4] = symbols[i].exported ? 0x10 : 0;
        core_le_store_u16(entry + 6,
                          symbols[i].section_index
                              ? (uint16_t)(ELF_USER_SECTION_BASE +
                                           symbols[i].section_index - 1)
                              : 0);
        core_le_store_u64(entry + 8, symbols[i].section_offset);
    }

    write_section_header(image + shdr_offset + ELF_SECTION_BYTES, shstr_name,
                         3, 0, shstr_offset, shstr_size, 0, 0, 1, 0);
    write_section_header(image + shdr_offset + 2 * ELF_SECTION_BYTES, str_name,
                         3, 0, str_offset, str_size, 0, 0, 1, 0);
    write_section_header(image + shdr_offset + 3 * ELF_SECTION_BYTES,
                         symtab_name, 2, 0, symtab_offset, symtab_bytes,
                         2, (uint32_t)local_count + 1, 8, ELF_SYMBOL_BYTES);
    for (size_t i = 0; i < section_count; i++) {
        const core_object_emit_section_t *section = &sections[i];
        if (section->size)
            memcpy(image + data_offsets[i], section->bytes, section->size);
        uint64_t flags = 2 | (section->writable ? 1 : 0) |
                         (section->executable ? 4 : 0);
        write_section_header(image + shdr_offset +
                                 (ELF_USER_SECTION_BASE + i) * ELF_SECTION_BYTES,
                             section_names[i], 1, flags, data_offsets[i],
                             section->size, 0, 0, section->alignment, 0);
    }
    size_t rel_header_index = ELF_USER_SECTION_BASE + section_count;
    for (size_t i = 0; i < section_count; i++) {
        if (!rel_counts[i]) continue;
        write_section_header(image + shdr_offset +
                                 rel_header_index++ * ELF_SECTION_BYTES,
                             rel_names[i], 4, 0, rel_offsets[i],
                             (size_t)rel_counts[i] * ELF_RELA_BYTES,
                             3, (uint32_t)(ELF_USER_SECTION_BASE + i),
                             8, ELF_RELA_BYTES);
    }
    for (size_t i = 0; i < relocation_count; i++) {
        const core_object_emit_absolute64_t *rel = &relocations[i];
        size_t section = rel->section_index - 1;
        uint8_t *entry = image + rel_offsets[section] +
                         (size_t)rel_written[section]++ * ELF_RELA_BYTES;
        core_le_store_u64(entry, rel->section_offset);
        core_le_store_u64(entry + 8,
                          ((uint64_t)symbol_map[rel->symbol_index] << 32) | 1);
        core_le_store_u64(entry + 16, rel->addend_bits);
        core_le_store_u64(image + data_offsets[section] + rel->section_offset,
                          rel->addend_bits);
    }
    *out_image = image;
    *out_size = offset;
    image = NULL;
    success = true;

done:
    core_free(image);
    core_free(symbol_map);
    core_free(symbol_names);
    core_free(rel_names);
    core_free(section_names);
    core_free(rel_written);
    core_free(rel_offsets);
    core_free(data_offsets);
    core_free(rel_counts);
    return success;
}
