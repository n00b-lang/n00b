#include "lib/object_emit_internal.h"

#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"

enum {
    COFF_HEADER_BYTES = 20,
    COFF_SECTION_BYTES = 40,
    COFF_SYMBOL_BYTES = 18,
    COFF_RELOCATION_BYTES = 10,
    COFF_RELOCATION_OVERFLOW = 0x01000000,
};

bool core_object_emit_coff(const core_target_desc_t *target,
                           const core_object_emit_section_t *sections,
                           size_t section_count,
                           const core_object_emit_symbol_t *symbols,
                           size_t symbol_count,
                           const core_object_emit_absolute64_t *relocations,
                           size_t relocation_count,
                           uint8_t **out_image, size_t *out_size)
{
    if (target->call_abi != CORE_TARGET_CALL_WINDOWS_X64 ||
        section_count > INT16_MAX) return false;
    uint32_t *rel_counts = core_alloc_array(uint32_t, section_count);
    uint32_t *rel_written = core_alloc_array(uint32_t, section_count);
    size_t *data_offsets = core_alloc_array(size_t, section_count);
    size_t *rel_offsets = core_alloc_array(size_t, section_count);
    uint32_t *section_name_offsets = core_alloc_array(uint32_t, section_count);
    uint32_t *symbol_name_offsets = core_alloc_array(uint32_t, symbol_count);
    uint8_t *image = NULL;
    bool success = false;

    for (size_t i = 0; i < relocation_count; i++)
        rel_counts[relocations[i].section_index - 1]++;
    size_t string_size = 4;
    for (size_t i = 0; i < section_count; i++) {
        const core_object_emit_section_t *section = &sections[i];
        if (section->size > UINT32_MAX || section->alignment > 8192)
            goto done;
        if (section->name.u8_bytes > 8) {
            size_t length = section->name.u8_bytes;
            if (length == SIZE_MAX || string_size > 9999999 ||
                !core_object_size_add(string_size, length + 1,
                                      &string_size) ||
                string_size > UINT32_MAX) goto done;
        }
    }
    for (size_t i = 0; i < symbol_count; i++) {
        if (symbols[i].section_offset > UINT32_MAX) goto done;
        if (symbols[i].name.u8_bytes > 8) {
            size_t length = symbols[i].name.u8_bytes;
            if (length == SIZE_MAX ||
                !core_object_size_add(string_size, length + 1,
                                      &string_size) ||
                string_size > UINT32_MAX) goto done;
        }
    }
    size_t headers_bytes, offset;
    if (!core_object_size_mul(section_count, COFF_SECTION_BYTES,
                              &headers_bytes) ||
        !core_object_size_add(COFF_HEADER_BYTES, headers_bytes, &offset))
        goto done;
    for (size_t i = 0; i < section_count; i++) {
        if (!core_object_size_align(offset, sections[i].alignment, &offset) ||
            offset > UINT32_MAX) goto done;
        data_offsets[i] = offset;
        if (!core_object_size_add(offset, sections[i].size, &offset)) goto done;
    }
    for (size_t i = 0; i < section_count; i++) {
        if (!rel_counts[i]) continue;
        if (rel_counts[i] == UINT32_MAX) goto done;
        size_t wire_count = (size_t)rel_counts[i] +
                            (rel_counts[i] >= UINT16_MAX);
        size_t bytes;
        if (!core_object_size_mul(wire_count, COFF_RELOCATION_BYTES,
                                  &bytes) ||
            !core_object_size_align(offset, 4, &offset) ||
            offset > UINT32_MAX) goto done;
        rel_offsets[i] = offset;
        if (!core_object_size_add(offset, bytes, &offset)) goto done;
    }
    if (!core_object_size_align(offset, 4, &offset) ||
        offset > UINT32_MAX) goto done;
    size_t symtab_offset = offset, symtab_bytes;
    if (!core_object_size_mul(symbol_count, COFF_SYMBOL_BYTES,
                              &symtab_bytes) ||
        !core_object_size_add(offset, symtab_bytes, &offset) ||
        offset > UINT32_MAX) goto done;
    size_t string_offset = offset;
    if (!core_object_size_add(offset, string_size, &offset)) goto done;
    image = core_alloc_size(offset, 1);

    core_le_store_u16(image, CORE_OBJECT_MACHINE_COFF_X86_64);
    core_le_store_u16(image + 2, (uint16_t)section_count);
    core_le_store_u32(image + 8, (uint32_t)symtab_offset);
    core_le_store_u32(image + 12, (uint32_t)symbol_count);
    core_le_store_u32(image + string_offset, (uint32_t)string_size);
    size_t string_cursor = string_offset + 4;
    for (size_t i = 0; i < section_count; i++) {
        const core_object_emit_section_t *section = &sections[i];
        uint8_t *header = image + COFF_HEADER_BYTES + i * COFF_SECTION_BYTES;
        size_t name_length = section->name.u8_bytes;
        if (name_length <= 8) memcpy(header, section->name.data, name_length);
        else {
            section_name_offsets[i] = (uint32_t)(string_cursor - string_offset);
            if (snprintf((char *)header, 8, "/%u",
                         section_name_offsets[i]) >= 8) goto done;
            memcpy(image + string_cursor, section->name.data, name_length);
            image[string_cursor + name_length] = 0;
            string_cursor += name_length + 1;
        }
        core_le_store_u32(header + 16, (uint32_t)section->size);
        if (section->size)
            core_le_store_u32(header + 20, (uint32_t)data_offsets[i]);
        if (rel_counts[i]) {
            core_le_store_u32(header + 24, (uint32_t)rel_offsets[i]);
            core_le_store_u16(header + 32,
                              rel_counts[i] >= UINT16_MAX
                                  ? UINT16_MAX : (uint16_t)rel_counts[i]);
        }
        uint32_t power = 1;
        for (uint32_t alignment = section->alignment; alignment > 1;
             alignment >>= 1) power++;
        uint32_t flags = (section->executable ? 0x20 : 0x40) |
                         (power << 20) | 0x40000000 |
                         (section->writable ? 0x80000000u : 0) |
                         (section->executable ? 0x20000000 : 0) |
                         (rel_counts[i] >= UINT16_MAX
                              ? COFF_RELOCATION_OVERFLOW : 0);
        core_le_store_u32(header + 36, flags);
        if (section->size)
            memcpy(image + data_offsets[i], section->bytes, section->size);
        if (rel_counts[i] >= UINT16_MAX)
            core_le_store_u32(image + rel_offsets[i], rel_counts[i] + 1);
    }
    for (size_t i = 0; i < symbol_count; i++) {
        const core_object_emit_symbol_t *symbol = &symbols[i];
        uint8_t *entry = image + symtab_offset + i * COFF_SYMBOL_BYTES;
        size_t name_length = symbol->name.u8_bytes;
        if (name_length <= 8) memcpy(entry, symbol->name.data, name_length);
        else {
            symbol_name_offsets[i] = (uint32_t)(string_cursor - string_offset);
            core_le_store_u32(entry + 4, symbol_name_offsets[i]);
            memcpy(image + string_cursor, symbol->name.data, name_length);
            image[string_cursor + name_length] = 0;
            string_cursor += name_length + 1;
        }
        core_le_store_u32(entry + 8, (uint32_t)symbol->section_offset);
        core_le_store_u16(entry + 12, (uint16_t)symbol->section_index);
        entry[16] = symbol->exported ? 2 : 3;
    }
    for (size_t i = 0; i < relocation_count; i++) {
        const core_object_emit_absolute64_t *rel = &relocations[i];
        size_t section = rel->section_index - 1;
        size_t index = rel_written[section]++ +
                       (rel_counts[section] >= UINT16_MAX);
        uint8_t *entry = image + rel_offsets[section] +
                         index * COFF_RELOCATION_BYTES;
        core_le_store_u32(entry, (uint32_t)rel->section_offset);
        core_le_store_u32(entry + 4, rel->symbol_index);
        core_le_store_u16(entry + 8, 1);
        core_le_store_u64(image + data_offsets[section] + rel->section_offset,
                          rel->addend_bits);
    }
    *out_image = image;
    *out_size = offset;
    image = NULL;
    success = true;

done:
    core_free(image);
    core_free(symbol_name_offsets);
    core_free(section_name_offsets);
    core_free(rel_offsets);
    core_free(data_offsets);
    core_free(rel_written);
    core_free(rel_counts);
    return success;
}
