/**
 * @file n00b_macho_query.c
 * @brief MachO query API — lookup functions over parsed Mach-O binaries.
 */

#include <string.h>
#include "compiler/objfile/macho.h"

n00b_macho_segment_t *
n00b_macho_segment_by_name(n00b_macho_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_segments; i++) {
        if (strcmp(bin->segments[i].name, name) == 0) {
            return &bin->segments[i];
        }
    }

    return nullptr;
}

n00b_macho_section_t *
n00b_macho_section_by_name(n00b_macho_binary_t *bin,
                           const char *segname,
                           const char *sectname)
{
    if (!bin || !segname || !sectname) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_segments; i++) {
        n00b_macho_segment_t *seg = &bin->segments[i];

        if (strcmp(seg->name, segname) != 0) {
            continue;
        }

        for (uint32_t j = 0; j < seg->nsects; j++) {
            if (strcmp(seg->sections[j].sectname, sectname) == 0) {
                return &seg->sections[j];
            }
        }
    }

    return nullptr;
}

n00b_macho_symbol_t *
n00b_macho_symbol_by_name(n00b_macho_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_symbols; i++) {
        if (bin->symbols[i].name && bin->symbols[i].name->data
            && strcmp(bin->symbols[i].name->data, name) == 0) {
            return &bin->symbols[i];
        }
    }

    return nullptr;
}

n00b_macho_dylib_t *
n00b_macho_dylib_by_name(n00b_macho_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_dylibs; i++) {
        if (bin->dylibs[i].name && bin->dylibs[i].name->data
            && strstr(bin->dylibs[i].name->data, name)) {
            return &bin->dylibs[i];
        }
    }

    return nullptr;
}

n00b_macho_export_t *
n00b_macho_export_by_name(n00b_macho_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_exports; i++) {
        if (bin->exports[i].name && bin->exports[i].name->data
            && strcmp(bin->exports[i].name->data, name) == 0) {
            return &bin->exports[i];
        }
    }

    return nullptr;
}

n00b_macho_binding_t *
n00b_macho_binding_by_symbol(n00b_macho_binary_t *bin, const char *name)
{
    if (!bin || !name) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_bindings; i++) {
        if (bin->bindings[i].symbol_name && bin->bindings[i].symbol_name->data
            && strcmp(bin->bindings[i].symbol_name->data, name) == 0) {
            return &bin->bindings[i];
        }
    }

    return nullptr;
}

n00b_macho_command_t *
n00b_macho_command_by_type(n00b_macho_binary_t *bin, uint32_t cmd)
{
    if (!bin) {
        return nullptr;
    }

    for (uint32_t i = 0; i < bin->num_commands; i++) {
        if (bin->commands[i].cmd == cmd) {
            return &bin->commands[i];
        }
    }

    return nullptr;
}

bool
n00b_macho_has_segment(n00b_macho_binary_t *bin, const char *name)
{
    return n00b_macho_segment_by_name(bin, name) != nullptr;
}

bool
n00b_macho_has_dylib(n00b_macho_binary_t *bin, const char *name)
{
    return n00b_macho_dylib_by_name(bin, name) != nullptr;
}

bool
n00b_macho_has_entrypoint(n00b_macho_binary_t *bin)
{
    return bin && bin->entrypoint != 0;
}

n00b_macho_binary_t *
n00b_macho_fat_by_cputype(n00b_macho_fat_t *fat, uint32_t cputype)
{
    if (!fat) {
        return nullptr;
    }

    for (uint32_t i = 0; i < fat->count; i++) {
        if (fat->binaries[i] && fat->binaries[i]->header.cputype == cputype) {
            return fat->binaries[i];
        }
    }

    return nullptr;
}

bool
n00b_macho_has_build_version(n00b_macho_binary_t *bin)
{
    return bin && bin->build_version != nullptr;
}

n00b_macho_build_version_t *
n00b_macho_get_build_version(n00b_macho_binary_t *bin)
{
    if (!bin) return nullptr;
    return bin->build_version;
}

bool
n00b_macho_has_rpath(n00b_macho_binary_t *bin)
{
    return bin && bin->num_rpaths > 0;
}

n00b_string_t *
n00b_macho_rpath_at(n00b_macho_binary_t *bin, uint32_t idx)
{
    if (!bin || idx >= bin->num_rpaths) {
        return nullptr;
    }

    return bin->rpaths[idx];
}

bool
n00b_macho_has_version_min(n00b_macho_binary_t *bin)
{
    return bin && bin->version_min != nullptr;
}

n00b_macho_version_min_t *
n00b_macho_get_version_min(n00b_macho_binary_t *bin)
{
    if (!bin) return nullptr;
    return bin->version_min;
}

bool
n00b_macho_has_data_in_code(n00b_macho_binary_t *bin)
{
    return bin && bin->data_in_code != nullptr;
}

n00b_macho_data_in_code_t *
n00b_macho_get_data_in_code(n00b_macho_binary_t *bin)
{
    if (!bin) return nullptr;
    return bin->data_in_code;
}

bool
n00b_macho_has_encryption_info(n00b_macho_binary_t *bin)
{
    return bin && bin->encryption_info != nullptr;
}

n00b_macho_encryption_info_t *
n00b_macho_get_encryption_info(n00b_macho_binary_t *bin)
{
    if (!bin) return nullptr;
    return bin->encryption_info;
}

n00b_macho_code_signature_parsed_t *
n00b_macho_get_code_signature(n00b_macho_binary_t *bin)
{
    if (!bin) return nullptr;
    return bin->code_signature_parsed;
}

n00b_string_t *
n00b_macho_get_entitlements(n00b_macho_binary_t *bin)
{
    if (!bin || !bin->code_signature_parsed) {
        return nullptr;
    }

    return bin->code_signature_parsed->entitlements_xml;
}

n00b_string_t *
n00b_macho_codesign_identifier(n00b_macho_binary_t *bin)
{
    if (!bin || !bin->code_signature_parsed
        || !bin->code_signature_parsed->code_directory) {
        return nullptr;
    }

    return bin->code_signature_parsed->code_directory->identifier;
}

n00b_string_t *
n00b_macho_codesign_team_id(n00b_macho_binary_t *bin)
{
    if (!bin || !bin->code_signature_parsed
        || !bin->code_signature_parsed->code_directory) {
        return nullptr;
    }

    return bin->code_signature_parsed->code_directory->team_id;
}
