#include "lib/object_emit_internal.h"

#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"

typedef struct {
    uint32_t section_index;
    uint64_t section_offset;
} relocation_key_t;

static int compare_relocations(const void *left, const void *right)
{
    const relocation_key_t *a = left, *b = right;
    if (a->section_index != b->section_index)
        return a->section_index < b->section_index ? -1 : 1;
    if (a->section_offset == b->section_offset) return 0;
    return a->section_offset < b->section_offset ? -1 : 1;
}

static bool valid_inputs(const core_target_desc_t *target,
                         const core_object_emit_section_t *sections,
                         size_t section_count,
                         const core_object_emit_symbol_t *symbols,
                         size_t symbol_count,
                         const core_object_emit_absolute64_t *relocations,
                         size_t relocation_count)
{
    if (!core_target_validate(target) ||
        target->endian != CORE_TARGET_LITTLE_ENDIAN ||
        target->pointer_bits != 64 ||
        !sections || !section_count || section_count > UINT32_MAX ||
        (symbol_count && !symbols) || (relocation_count && !relocations) ||
        symbol_count > UINT32_MAX || relocation_count > UINT32_MAX)
        return false;
    for (size_t i = 0; i < section_count; i++) {
        const core_object_emit_section_t *section = &sections[i];
        if (!section->name || !section->name[0] ||
            (section->size && !section->bytes) ||
            !section->alignment ||
            (section->alignment & (section->alignment - 1))) return false;
        if (section->is_thread_local
            && (!section->writable || section->executable
                || (target->object_format == CORE_TARGET_OBJECT_MACH_O
                    && strcmp(section->segment ? section->segment : "",
                              "__DATA"))
                || (target->object_format == CORE_TARGET_OBJECT_COFF
                    && strncmp(section->name, ".tls$", 5)))) return false;
        if (section->is_thread_local_descriptor
            && (target->object_format != CORE_TARGET_OBJECT_MACH_O
                || section->is_thread_local || !section->writable
                || section->executable
                || strcmp(section->segment ? section->segment : "", "__DATA")))
            return false;
        if (target->object_format == CORE_TARGET_OBJECT_MACH_O &&
            (!section->segment || !section->segment[0])) return false;
    }
    for (size_t i = 0; i < symbol_count; i++) {
        const core_object_emit_symbol_t *symbol = &symbols[i];
        if (!symbol->name || !symbol->name[0] ||
            symbol->section_index > section_count ||
            (!symbol->section_index && (!symbol->exported ||
                                        symbol->section_offset)) ||
            (symbol->section_index &&
             symbol->section_offset >
                 sections[symbol->section_index - 1].size)) return false;
    }
    for (size_t i = 0; i < relocation_count; i++) {
        const core_object_emit_absolute64_t *relocation = &relocations[i];
        if (!relocation->section_index ||
            relocation->section_index > section_count ||
            relocation->symbol_index >= symbol_count ||
            (relocation->symbol_relocation
                && target->object_format != CORE_TARGET_OBJECT_MACH_O))
            return false;
        size_t size = sections[relocation->section_index - 1].size;
        if (relocation->section_offset > size ||
            size - relocation->section_offset < 8) return false;
    }
    relocation_key_t *relocation_keys =
        core_alloc_array(relocation_key_t, relocation_count);
    for (size_t i = 0; i < relocation_count; i++) {
        relocation_keys[i].section_index = relocations[i].section_index;
        relocation_keys[i].section_offset = relocations[i].section_offset;
    }
    qsort(relocation_keys, relocation_count, sizeof *relocation_keys,
          compare_relocations);
    bool valid = true;
    for (size_t i = 1; valid && i < relocation_count; i++) {
        if (!compare_relocations(&relocation_keys[i - 1],
                                 &relocation_keys[i])) valid = false;
    }
    core_free(relocation_keys);
    return valid;
}

bool core_object_emit(const core_target_desc_t *target,
                      const core_object_emit_section_t *sections,
                      size_t section_count,
                      const core_object_emit_symbol_t *symbols,
                      size_t symbol_count,
                      const core_object_emit_absolute64_t *relocations,
                      size_t relocation_count,
                      uint8_t **out_image, size_t *out_size)
{
    if (!out_image || !out_size ||
        !valid_inputs(target, sections, section_count, symbols, symbol_count,
                      relocations, relocation_count)) return false;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        return core_object_emit_elf(target, sections, section_count, symbols,
                                    symbol_count, relocations, relocation_count,
                                    out_image, out_size);
    case CORE_TARGET_OBJECT_MACH_O:
        return core_object_emit_macho(target, sections, section_count, symbols,
                                      symbol_count, relocations,
                                      relocation_count, out_image, out_size);
    case CORE_TARGET_OBJECT_COFF:
        return core_object_emit_coff(target, sections, section_count, symbols,
                                     symbol_count, relocations,
                                     relocation_count, out_image, out_size);
    default:
        return false;
    }
}

bool core_object_emit_native_section_index(
    const core_target_desc_t *target, size_t section_count,
    uint32_t input_section_index, uint32_t *native_section_index)
{
    if (!native_section_index || !core_target_validate(target) ||
        target->endian != CORE_TARGET_LITTLE_ENDIAN ||
        target->pointer_bits != 64 ||
        !input_section_index || input_section_index > section_count)
        return false;
    switch (target->object_format) {
    case CORE_TARGET_OBJECT_ELF:
        if (target->call_abi != CORE_TARGET_CALL_SYSV_AMD64) return false;
        return core_object_emit_elf_native_section_index(
            section_count, input_section_index, native_section_index);
    case CORE_TARGET_OBJECT_MACH_O:
        if (target->call_abi != CORE_TARGET_CALL_DARWIN_AARCH64 ||
            section_count > UINT8_MAX) return false;
        break;
    case CORE_TARGET_OBJECT_COFF:
        if (target->call_abi != CORE_TARGET_CALL_WINDOWS_X64 ||
            section_count > INT16_MAX) return false;
        break;
    default:
        return false;
    }
    *native_section_index = input_section_index;
    return true;
}
