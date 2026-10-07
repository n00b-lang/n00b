#include "lib/object_emit_internal.h"

#include <string.h>

#include "lib/alloc.h"
#include "lib/byteorder.h"

enum {
    MACH_HEADER_BYTES = 32,
    MACH_SEGMENT_BYTES = 72,
    MACH_SECTION_BYTES = 80,
    MACH_SYMTAB_COMMAND_BYTES = 24,
    MACH_DYSYMTAB_COMMAND_BYTES = 80,
    MACH_SYMBOL_BYTES = 16,
    MACH_RELOCATION_BYTES = 8,
};

bool core_object_emit_macho(const core_target_desc_t *target,
                            const core_object_emit_section_t *sections,
                            size_t section_count,
                            const core_object_emit_symbol_t *symbols,
                            size_t symbol_count,
                            const core_object_emit_absolute64_t *relocations,
                            size_t relocation_count,
                            uint8_t **out_image, size_t *out_size)
{
    if (target->call_abi != CORE_TARGET_CALL_DARWIN_AARCH64 ||
        section_count > UINT8_MAX) return false;
    for (size_t i = 0; i < section_count; i++) {
        if (strlen(sections[i].name) > 16 ||
            strlen(sections[i].segment) > 16 ||
            sections[i].size > UINT32_MAX) return false;
    }

    uint32_t *rel_counts = core_alloc_array(uint32_t, section_count);
    uint32_t *rel_written = core_alloc_array(uint32_t, section_count);
    size_t *section_addresses = core_alloc_array(size_t, section_count);
    size_t *section_file_offsets = core_alloc_array(size_t, section_count);
    size_t *rel_offsets = core_alloc_array(size_t, section_count);
    uint32_t *symbol_map = core_alloc_array(uint32_t, symbol_count);
    uint32_t *symbol_names = core_alloc_array(uint32_t, symbol_count);
    uint8_t *image = NULL;
    bool success = false;
    size_t local_count = 0, external_count = 0;
    for (size_t i = 0; i < relocation_count; i++)
        rel_counts[relocations[i].section_index - 1]++;
    for (size_t i = 0; i < symbol_count; i++) {
        if (!symbols[i].exported) local_count++;
        else if (symbols[i].section_index) external_count++;
    }

    size_t segment_command, command_bytes, data_base, vm_size = 0;
    if (!core_object_size_mul(section_count, MACH_SECTION_BYTES,
                              &segment_command) ||
        !core_object_size_add(segment_command, MACH_SEGMENT_BYTES,
                              &segment_command) ||
        !core_object_size_add(segment_command, MACH_SYMTAB_COMMAND_BYTES,
                              &command_bytes) ||
        !core_object_size_add(command_bytes, MACH_DYSYMTAB_COMMAND_BYTES,
                              &command_bytes) ||
        command_bytes > UINT32_MAX ||
        !core_object_size_add(MACH_HEADER_BYTES, command_bytes, &data_base))
        goto done;
    size_t max_alignment = 1;
    for (size_t i = 0; i < section_count; i++) {
        if (sections[i].alignment > max_alignment)
            max_alignment = sections[i].alignment;
    }
    if (!core_object_size_align(data_base, max_alignment, &data_base))
        goto done;
    for (size_t i = 0; i < section_count; i++) {
        if (!core_object_size_align(vm_size, sections[i].alignment,
                                    &vm_size) ||
            !core_object_size_add(data_base, vm_size,
                                  &section_file_offsets[i]) ||
            section_file_offsets[i] > UINT32_MAX) goto done;
        section_addresses[i] = vm_size;
        if (!core_object_size_add(vm_size, sections[i].size, &vm_size))
            goto done;
    }
    size_t offset;
    if (!core_object_size_add(data_base, vm_size, &offset)) goto done;
    for (size_t i = 0; i < section_count; i++) {
        if (!rel_counts[i]) continue;
        size_t bytes;
        if (!core_object_size_mul(rel_counts[i], MACH_RELOCATION_BYTES,
                                  &bytes) ||
            !core_object_size_align(offset, 4, &offset) ||
            offset > UINT32_MAX) goto done;
        rel_offsets[i] = offset;
        if (!core_object_size_add(offset, bytes, &offset)) goto done;
    }
    if (!core_object_size_align(offset, 8, &offset) ||
        offset > UINT32_MAX) goto done;
    size_t symtab_offset = offset, symtab_bytes;
    if (!core_object_size_mul(symbol_count, MACH_SYMBOL_BYTES,
                              &symtab_bytes) ||
        !core_object_size_add(offset, symtab_bytes, &offset) ||
        offset > UINT32_MAX) goto done;
    size_t str_offset = offset, str_size = 1;
    for (size_t i = 0; i < symbol_count; i++) {
        size_t length = strlen(symbols[i].name);
        if (length == SIZE_MAX ||
            !core_object_size_add(str_size, length + 1, &str_size) ||
            str_size > UINT32_MAX) goto done;
    }
    if (!core_object_size_add(offset, str_size, &offset) ||
        !core_object_size_align(offset, 4, &offset) ||
        offset - str_offset > UINT32_MAX) goto done;
    image = core_alloc_size(offset, 1);

    core_le_store_u32(image, 0xfeedfacf);
    core_le_store_u32(image + 4, 0x0100000c);
    core_le_store_u32(image + 8, 0);
    core_le_store_u32(image + 12, 1);
    core_le_store_u32(image + 16, 3);
    core_le_store_u32(image + 20, (uint32_t)command_bytes);
    uint8_t *segment = image + MACH_HEADER_BYTES;
    core_le_store_u32(segment, 0x19);
    core_le_store_u32(segment + 4, (uint32_t)segment_command);
    core_le_store_u64(segment + 32, vm_size);
    core_le_store_u64(segment + 40, data_base);
    core_le_store_u64(segment + 48, vm_size);
    core_le_store_u32(segment + 56, 7);
    core_le_store_u32(segment + 60, 7);
    core_le_store_u32(segment + 64, (uint32_t)section_count);
    for (size_t i = 0; i < section_count; i++) {
        const core_object_emit_section_t *section = &sections[i];
        uint8_t *header = segment + MACH_SEGMENT_BYTES +
                          i * MACH_SECTION_BYTES;
        memcpy(header, section->name, strlen(section->name));
        memcpy(header + 16, section->segment, strlen(section->segment));
        core_le_store_u64(header + 32, section_addresses[i]);
        core_le_store_u64(header + 40, section->size);
        core_le_store_u32(header + 48, (uint32_t)section_file_offsets[i]);
        uint32_t power = 0;
        for (uint32_t alignment = section->alignment; alignment > 1;
             alignment >>= 1) power++;
        core_le_store_u32(header + 52, power);
        if (rel_counts[i]) {
            core_le_store_u32(header + 56, (uint32_t)rel_offsets[i]);
            core_le_store_u32(header + 60, rel_counts[i]);
        }
        core_le_store_u32(header + 64,
                          section->executable ? 0x800000 : 0);
        if (section->size)
            memcpy(image + section_file_offsets[i], section->bytes,
                   section->size);
    }
    uint8_t *symtab_command = segment + segment_command;
    core_le_store_u32(symtab_command, 2);
    core_le_store_u32(symtab_command + 4, MACH_SYMTAB_COMMAND_BYTES);
    core_le_store_u32(symtab_command + 8, (uint32_t)symtab_offset);
    core_le_store_u32(symtab_command + 12, (uint32_t)symbol_count);
    core_le_store_u32(symtab_command + 16, (uint32_t)str_offset);
    core_le_store_u32(symtab_command + 20, (uint32_t)(offset - str_offset));
    uint8_t *dysymtab = symtab_command + MACH_SYMTAB_COMMAND_BYTES;
    core_le_store_u32(dysymtab, 0xb);
    core_le_store_u32(dysymtab + 4, MACH_DYSYMTAB_COMMAND_BYTES);
    core_le_store_u32(dysymtab + 12, (uint32_t)local_count);
    core_le_store_u32(dysymtab + 16, (uint32_t)local_count);
    core_le_store_u32(dysymtab + 20, (uint32_t)external_count);
    core_le_store_u32(dysymtab + 24,
                      (uint32_t)(local_count + external_count));
    core_le_store_u32(dysymtab + 28,
                      (uint32_t)(symbol_count - local_count - external_count));

    uint32_t local_next = 0, external_next = (uint32_t)local_count;
    uint32_t undefined_next = (uint32_t)(local_count + external_count);
    size_t string_cursor = str_offset + 1;
    for (size_t i = 0; i < symbol_count; i++) {
        const core_object_emit_symbol_t *symbol = &symbols[i];
        uint32_t index = !symbol->exported ? local_next++
                       : symbol->section_index ? external_next++
                                               : undefined_next++;
        symbol_map[i] = index;
        symbol_names[i] = (uint32_t)(string_cursor - str_offset);
        size_t length = strlen(symbol->name) + 1;
        memcpy(image + string_cursor, symbol->name, length);
        string_cursor += length;
        uint8_t *entry = image + symtab_offset +
                         (size_t)index * MACH_SYMBOL_BYTES;
        core_le_store_u32(entry, symbol_names[i]);
        entry[4] = (symbol->section_index ? 0x0e : 0) |
                   (symbol->exported ? 1 : 0);
        entry[5] = (uint8_t)symbol->section_index;
        if (symbol->section_index)
            core_le_store_u64(entry + 8,
                              section_addresses[symbol->section_index - 1] +
                                  symbol->section_offset);
    }
    for (size_t i = 0; i < relocation_count; i++) {
        const core_object_emit_absolute64_t *rel = &relocations[i];
        const core_object_emit_symbol_t *symbol = &symbols[rel->symbol_index];
        size_t source = rel->section_index - 1;
        uint8_t *entry = image + rel_offsets[source] +
                         (size_t)rel_written[source]++ * MACH_RELOCATION_BYTES;
        core_le_store_u32(entry, (uint32_t)rel->section_offset);
        bool external = symbol->exported || !symbol->section_index;
        uint32_t target_index = external ? symbol_map[rel->symbol_index]
                                         : symbol->section_index;
        core_le_store_u32(entry + 4, target_index | (3u << 25) |
                                       (external ? (1u << 27) : 0));
        uint64_t addend = rel->addend_bits;
        if (!external)
            addend += section_addresses[symbol->section_index - 1] +
                      symbol->section_offset;
        core_le_store_u64(image + section_file_offsets[source] +
                              rel->section_offset, addend);
    }
    *out_image = image;
    *out_size = offset;
    image = NULL;
    success = true;

done:
    core_free(image);
    core_free(symbol_names);
    core_free(symbol_map);
    core_free(rel_offsets);
    core_free(section_file_offsets);
    core_free(section_addresses);
    core_free(rel_written);
    core_free(rel_counts);
    return success;
}
