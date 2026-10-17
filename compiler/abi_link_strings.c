#include "abi_link.h"

#include <string.h>

#include "abi_string.h"
#include "lib/alloc.h"

_Static_assert((int)N00BB_ABI_STRING_LONG == (int)N00B_ABI_STRING_LONG,
               "long string tags agree");
_Static_assert((int)N00BB_ABI_STRING_SPAN == (int)N00B_ABI_STRING_SPAN,
               "span string tags agree");
_Static_assert((int)N00BB_ABI_STRING_UTF8 == (int)N00B_ABI_STRING_UTF8,
               "string encodings agree");

static bool
is_tu_string_pool(const core_target_desc_t *target,
                  const core_object_emit_section_t *section)
{
    const char *name = target->object_format == CORE_TARGET_OBJECT_MACH_O
        ? "__n00b_strings"
        : target->object_format == CORE_TARGET_OBJECT_COFF
            ? ".rdata$S" : ".rodata.n00b_strings";
    return section->name && !strcmp(section->name, name)
        && (target->object_format != CORE_TARGET_OBJECT_MACH_O
            || (section->segment && !strcmp(section->segment, "__TEXT")));
}

bool
n00bb_abi_consolidate_native_strings(
    const core_target_desc_t *target,
    n00bb_abi_name_pool_t *pool,
    n00bb_abi_native_sections_t *sections,
    n00bb_abi_native_relocations_t *native)
{
    if (!target || !pool || !sections || !native
        || (sections->map_count && (!sections->sections || !sections->maps))
        || (native->symbol_count && !native->symbols)
        || (native->relocation_count && !native->relocations)
        || sections->count > UINT32_MAX - 2
        || native->symbol_count >= UINT32_MAX) return false;

    size_t pool_count = 0;
    for (size_t i = 0; i < sections->count; i++)
        pool_count += is_tu_string_pool(target, &sections->sections[i]);
    if (!pool_count) return true;

    size_t kept_sections = sections->count - pool_count;
    uint32_t *section_ordinals = core_alloc_array(uint32_t,
                                                   sections->count + 2);
    uint32_t *symbol_indexes = core_alloc_array(uint32_t,
                                                 native->symbol_count);
    core_object_emit_section_t *new_sections = core_alloc_array(
        core_object_emit_section_t, kept_sections);
    uint32_t *new_finals = core_alloc_array(uint32_t, kept_sections + 1);
    n00bb_abi_native_section_map_t *new_maps = core_alloc_array(
        n00bb_abi_native_section_map_t, sections->map_count + 1);
    core_object_emit_symbol_t *new_symbols = core_alloc_array(
        core_object_emit_symbol_t, native->symbol_count);
    core_object_emit_absolute64_t *new_relocations = core_alloc_array(
        core_object_emit_absolute64_t, native->relocation_count);
    bool valid = section_ordinals
        && (!native->symbol_count || (symbol_indexes && new_symbols))
        && (!kept_sections || new_sections) && new_finals && new_maps
        && (!native->relocation_count || new_relocations);
    size_t section_count = 0, symbol_count = 0, map_count = 0;
    for (size_t i = 0; valid && i < sections->count; i++) {
        if (is_tu_string_pool(target, &sections->sections[i])) continue;
        uint32_t ordinal = (uint32_t)(2 + section_count);
        valid = core_object_emit_native_section_index(target,
            1 + kept_sections, ordinal, &new_finals[section_count]);
        if (!valid) break;
        section_ordinals[i + 2] = ordinal;
        new_sections[section_count] = sections->sections[i];
        section_count++;
    }
    for (size_t i = 0; valid && i < sections->map_count; i++) {
        uint32_t ordinal = section_ordinals[sections->maps[i].section + 2];
        if (!ordinal) continue;
        new_maps[map_count] = sections->maps[i];
        new_maps[map_count].section = ordinal - 2;
        new_maps[map_count].final_section_index = new_finals[ordinal - 2];
        map_count++;
    }
    for (size_t i = 0; valid && i < native->symbol_count; i++) {
        const core_object_emit_symbol_t *symbol = &native->symbols[i];
        uint32_t old = symbol->section_index;
        valid = !old || (old >= 2 && old < sections->count + 2);
        if (!valid) break;
        if (old && !section_ordinals[old]) {
            valid = !symbol->exported;
            symbol_indexes[i] = UINT32_MAX;
            continue;
        }
        symbol_indexes[i] = (uint32_t)symbol_count;
        new_symbols[symbol_count] = *symbol;
        if (old) new_symbols[symbol_count].section_index =
            section_ordinals[old];
        symbol_count++;
    }
    for (size_t i = 0; valid && i < native->relocation_count; i++) {
        const core_object_emit_absolute64_t *source =
            &native->relocations[i];
        uint32_t old_source = source->section_index;
        valid = old_source >= 2 && old_source < sections->count + 2
            && section_ordinals[old_source]
            && source->symbol_index < native->symbol_count;
        if (!valid) break;
        core_object_emit_absolute64_t next = *source;
        next.section_index = section_ordinals[old_source];
        uint32_t mapped_symbol = symbol_indexes[source->symbol_index];
        if (mapped_symbol == UINT32_MAX) {
            const core_object_emit_section_t *from =
                &sections->sections[old_source - 2];
            const core_object_emit_symbol_t *target_symbol =
                &native->symbols[source->symbol_index];
            const core_object_emit_section_t *old_pool =
                &sections->sections[target_symbol->section_index - 2];
            uint64_t at = target_symbol->section_offset;
            n00bb_abi_string_header_t header;
            valid = source->section_offset <= from->size
                && sizeof(n00b_abi_string_v1)
                    <= from->size - source->section_offset
                && at <= old_pool->size
                && source->addend_bits < old_pool->size - at
                && n00bb_abi_string_decode(
                    from->bytes + source->section_offset,
                    sizeof(n00b_abi_string_v1), &header)
                && (header.tag == N00BB_ABI_STRING_LONG
                    || header.tag == N00BB_ABI_STRING_SPAN)
                && header.length <=
                    old_pool->size - at - source->addend_bits;
            if (!valid) break;
            n00b_abi_string_v1 encoded;
            size_t pool_offset;
            valid = n00bb_abi_name_pool_value_tagged(pool,
                core_string_borrow(
                    (const char *)old_pool->bytes + at + source->addend_bits,
                    (size_t)header.length),
                (n00b_abi_string_encoding_v1)header.encoding,
                (n00b_abi_string_tag_v1)header.tag,
                &encoded, &pool_offset);
            if (!valid) break;
            next.symbol_index = (uint32_t)symbol_count;
            next.addend_bits = pool_offset;
        }
        else next.symbol_index = mapped_symbol;
        new_relocations[i] = next;
    }
    if (valid) valid = section_count == kept_sections;
    if (valid) {
        for (size_t i = 0; i < sections->count; i++) {
            if (!is_tu_string_pool(target, &sections->sections[i])) continue;
            core_free((void *)sections->sections[i].name);
            core_free((void *)sections->sections[i].segment);
            core_free((void *)sections->sections[i].bytes);
        }
        for (size_t i = 0; i < native->symbol_count; i++)
            if (symbol_indexes[i] == UINT32_MAX)
                core_free((void *)native->symbols[i].name);
        core_free(sections->sections);
        core_free(sections->final_indices);
        core_free(sections->maps);
        core_free(native->symbols);
        core_free(native->relocations);
        sections->sections = new_sections;
        sections->final_indices = new_finals;
        sections->maps = new_maps;
        sections->map_count = map_count;
        sections->count = kept_sections;
        native->symbols = new_symbols;
        native->symbol_count = symbol_count;
        native->relocations = new_relocations;
    }
    else {
        core_free(new_sections);
        core_free(new_finals);
        core_free(new_maps);
        core_free(new_symbols);
        core_free(new_relocations);
    }
    core_free(section_ordinals);
    core_free(symbol_indexes);
    return valid;
}
