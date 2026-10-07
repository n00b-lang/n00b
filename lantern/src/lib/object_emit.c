#include "lib/object_emit_internal.h"

#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"

typedef struct {
    const char *name;
    const char *segment;
} section_key_t;

typedef struct {
    uint32_t section_index;
    uint64_t section_offset;
} relocation_key_t;

static int compare_sections(const void *left, const void *right)
{
    const section_key_t *a = left, *b = right;
    int result = strcmp(a->name, b->name);
    if (result) return result;
    return strcmp(a->segment ? a->segment : "",
                  b->segment ? b->segment : "");
}

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
            relocation->symbol_index >= symbol_count) return false;
        size_t size = sections[relocation->section_index - 1].size;
        if (relocation->section_offset > size ||
            size - relocation->section_offset < 8) return false;
    }
    section_key_t *section_keys = core_alloc_array(section_key_t, section_count);
    relocation_key_t *relocation_keys =
        core_alloc_array(relocation_key_t, relocation_count);
    for (size_t i = 0; i < section_count; i++) {
        section_keys[i].name = sections[i].name;
        section_keys[i].segment = target->object_format == CORE_TARGET_OBJECT_MACH_O
                                      ? sections[i].segment : NULL;
    }
    for (size_t i = 0; i < relocation_count; i++) {
        relocation_keys[i].section_index = relocations[i].section_index;
        relocation_keys[i].section_offset = relocations[i].section_offset;
    }
    qsort(section_keys, section_count, sizeof *section_keys, compare_sections);
    qsort(relocation_keys, relocation_count, sizeof *relocation_keys,
          compare_relocations);
    bool valid = true;
    for (size_t i = 1; i < section_count; i++) {
        if (!compare_sections(&section_keys[i - 1], &section_keys[i])) {
            valid = false;
            break;
        }
    }
    for (size_t i = 1; valid && i < relocation_count; i++) {
        if (!compare_relocations(&relocation_keys[i - 1],
                                 &relocation_keys[i])) valid = false;
    }
    core_free(relocation_keys);
    core_free(section_keys);
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
