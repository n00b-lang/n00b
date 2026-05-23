/**
 * @file n00b_elf_query.c
 * @brief ELF query API — lookup functions over parsed ELF binaries.
 */

#include <string.h>
#include "compiler/objfile/elf.h"

n00b_option_t(n00b_elf_section_t *)
n00b_elf_section_by_name(n00b_elf_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return n00b_option_none(n00b_elf_section_t *);
    }

    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if (bin->sections[i].name && bin->sections[i].name->data
            && strcmp(bin->sections[i].name->data, name) == 0) {
            return n00b_option_set(n00b_elf_section_t *, &bin->sections[i]);
        }
    }

    return n00b_option_none(n00b_elf_section_t *);
}

n00b_option_t(n00b_elf_section_t *)
n00b_elf_section_by_type(n00b_elf_binary_t *bin, uint32_t type)
{
    if (!bin) {
        return n00b_option_none(n00b_elf_section_t *);
    }

    for (uint32_t i = 0; i < bin->num_sections; i++) {
        if (bin->sections[i].type == type) {
            return n00b_option_set(n00b_elf_section_t *, &bin->sections[i]);
        }
    }

    return n00b_option_none(n00b_elf_section_t *);
}

n00b_option_t(n00b_elf_section_t *)
n00b_elf_section_at_addr(n00b_elf_binary_t *bin, uint64_t addr)
{
    if (!bin) {
        return n00b_option_none(n00b_elf_section_t *);
    }

    for (uint32_t i = 0; i < bin->num_sections; i++) {
        n00b_elf_section_t *sec = &bin->sections[i];

        if (sec->size > 0 && addr >= sec->addr
            && addr < sec->addr + sec->size) {
            return n00b_option_set(n00b_elf_section_t *, sec);
        }
    }

    return n00b_option_none(n00b_elf_section_t *);
}

n00b_option_t(n00b_elf_segment_t *)
n00b_elf_segment_by_type(n00b_elf_binary_t *bin, uint32_t type)
{
    if (!bin) {
        return n00b_option_none(n00b_elf_segment_t *);
    }

    for (uint32_t i = 0; i < bin->num_segments; i++) {
        if (bin->segments[i].type == type) {
            return n00b_option_set(n00b_elf_segment_t *, &bin->segments[i]);
        }
    }

    return n00b_option_none(n00b_elf_segment_t *);
}

n00b_option_t(n00b_elf_segment_t *)
n00b_elf_segment_at_addr(n00b_elf_binary_t *bin, uint64_t vaddr)
{
    if (!bin) {
        return n00b_option_none(n00b_elf_segment_t *);
    }

    for (uint32_t i = 0; i < bin->num_segments; i++) {
        n00b_elf_segment_t *seg = &bin->segments[i];

        if (seg->memsz > 0 && vaddr >= seg->vaddr
            && vaddr < seg->vaddr + seg->memsz) {
            return n00b_option_set(n00b_elf_segment_t *, seg);
        }
    }

    return n00b_option_none(n00b_elf_segment_t *);
}

n00b_option_t(n00b_elf_symbol_t *)
n00b_elf_symtab_by_name(n00b_elf_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return n00b_option_none(n00b_elf_symbol_t *);
    }

    for (uint32_t i = 0; i < bin->num_symtab; i++) {
        if (bin->symtab_symbols[i].name && bin->symtab_symbols[i].name->data
            && strcmp(bin->symtab_symbols[i].name->data, name) == 0) {
            return n00b_option_set(n00b_elf_symbol_t *,
                                   &bin->symtab_symbols[i]);
        }
    }

    return n00b_option_none(n00b_elf_symbol_t *);
}

n00b_option_t(n00b_elf_symbol_t *)
n00b_elf_dynsym_by_name(n00b_elf_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return n00b_option_none(n00b_elf_symbol_t *);
    }

    for (uint32_t i = 0; i < bin->num_dynsym; i++) {
        if (bin->dynsym_symbols[i].name && bin->dynsym_symbols[i].name->data
            && strcmp(bin->dynsym_symbols[i].name->data, name) == 0) {
            return n00b_option_set(n00b_elf_symbol_t *,
                                   &bin->dynsym_symbols[i]);
        }
    }

    return n00b_option_none(n00b_elf_symbol_t *);
}

n00b_option_t(n00b_elf_symbol_t *)
n00b_elf_symbol_by_name(n00b_elf_binary_t *bin, const char *name)
{
    n00b_option_t(n00b_elf_symbol_t *) sym_opt
        = n00b_elf_symtab_by_name(bin, name);

    if (n00b_option_is_set(sym_opt)) {
        return sym_opt;
    }

    return n00b_elf_dynsym_by_name(bin, name);
}

n00b_option_t(n00b_elf_dynamic_t *)
n00b_elf_dynamic_by_tag(n00b_elf_binary_t *bin, int64_t tag)
{
    if (!bin) {
        return n00b_option_none(n00b_elf_dynamic_t *);
    }

    for (uint32_t i = 0; i < bin->num_dynamic; i++) {
        if (bin->dynamic_entries[i].tag == tag) {
            return n00b_option_set(n00b_elf_dynamic_t *,
                                   &bin->dynamic_entries[i]);
        }
    }

    return n00b_option_none(n00b_elf_dynamic_t *);
}

n00b_option_t(n00b_elf_note_t *)
n00b_elf_note_by_type(n00b_elf_binary_t *bin, uint32_t type)
{
    if (!bin) {
        return n00b_option_none(n00b_elf_note_t *);
    }

    for (uint32_t i = 0; i < bin->num_notes; i++) {
        if (bin->notes[i].type == type) {
            return n00b_option_set(n00b_elf_note_t *, &bin->notes[i]);
        }
    }

    return n00b_option_none(n00b_elf_note_t *);
}

bool
n00b_elf_has_section(n00b_elf_binary_t *bin, const char *name)
{
    return n00b_option_is_set(n00b_elf_section_by_name(bin, name));
}

bool
n00b_elf_has_segment(n00b_elf_binary_t *bin, uint32_t type)
{
    return n00b_option_is_set(n00b_elf_segment_by_type(bin, type));
}

bool
n00b_elf_has_interpreter(n00b_elf_binary_t *bin)
{
    return bin && bin->interpreter && bin->interpreter->u8_bytes > 0;
}

bool
n00b_elf_is_core(n00b_elf_binary_t *bin)
{
    return bin && bin->header.type == ET_CORE;
}

n00b_option_t(n00b_elf_core_info_t *)
n00b_elf_core_info(n00b_elf_binary_t *bin)
{
    if (!bin) return n00b_option_none(n00b_elf_core_info_t *);
    return n00b_option_from_nullable(n00b_elf_core_info_t *, bin->core_info);
}
