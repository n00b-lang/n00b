/**
 * @file n00b_macho_build.h
 * @brief Mach-O binary builder — struct helpers and serialization.
 *
 * Provides functions to create and populate `n00b_macho_binary_t` structs
 * from scratch, then serialize them to `n00b_buffer_t *` via
 * `n00b_macho_build()`.
 */
#pragma once

#include "compiler/objfile/macho.h"
#include "compiler/objfile/writer.h"
#include "adt/result.h"

// ============================================================================
// Binary creation
// ============================================================================

/**
 * @brief Create a new empty Mach-O 64-bit binary struct.
 *
 * Initializes the header with MH_MAGIC_64 and the given CPU type/subtype.
 *
 * @param cputype    CPU type (e.g. CPU_TYPE_X86_64, CPU_TYPE_ARM64).
 * @param cpusubtype CPU subtype.
 * @param filetype   File type (MH_EXECUTE, MH_DYLIB, etc.).
 */
extern n00b_macho_binary_t *n00b_macho_binary_new(uint32_t cputype,
    uint32_t cpusubtype, uint32_t filetype);

// ============================================================================
// Segment/section management
// ============================================================================

extern n00b_macho_segment_t *n00b_macho_add_segment(n00b_macho_binary_t *bin,
    const char *name, uint32_t initprot, uint32_t maxprot);

extern n00b_macho_section_t *n00b_macho_add_section(n00b_macho_segment_t *seg,
    const char *sectname, const char *segname, uint32_t flags, uint32_t align);

// ============================================================================
// Symbol management
// ============================================================================

extern n00b_macho_symbol_t *n00b_macho_add_symbol(n00b_macho_binary_t *bin,
    const char *name, uint8_t type, uint8_t sect, uint16_t desc,
    uint64_t value);

// ============================================================================
// Dylib management
// ============================================================================

extern n00b_macho_dylib_t *n00b_macho_add_dylib(n00b_macho_binary_t *bin,
    const char *path, uint32_t current_version, uint32_t compat_version);

// ============================================================================
// Binding info
// ============================================================================

extern n00b_macho_binding_t *n00b_macho_add_binding(n00b_macho_binary_t *bin,
    const char *symbol_name, int32_t library_ordinal, uint8_t segment_index,
    uint64_t address, uint8_t type, int64_t addend);

// ============================================================================
// Rebase info
// ============================================================================

extern n00b_macho_rebase_t *n00b_macho_add_rebase(n00b_macho_binary_t *bin,
    uint8_t segment_index, uint64_t address, uint8_t type);

// ============================================================================
// Export info
// ============================================================================

extern n00b_macho_export_t *n00b_macho_add_export(n00b_macho_binary_t *bin,
    const char *name, uint64_t address, uint64_t flags);

// ============================================================================
// Convenience setters
// ============================================================================

extern void n00b_macho_set_entry(n00b_macho_binary_t *bin, uint64_t entryoff,
    uint64_t stacksize);

extern void n00b_macho_set_dylinker(n00b_macho_binary_t *bin,
    const char *path);

extern void n00b_macho_set_uuid(n00b_macho_binary_t *bin,
    const uint8_t uuid[16]);

extern void n00b_macho_set_function_starts(n00b_macho_binary_t *bin,
    uint64_t *addresses, uint32_t count);

extern void n00b_macho_set_source_version(n00b_macho_binary_t *bin,
    uint64_t version);

extern void n00b_macho_set_build_version(n00b_macho_binary_t *bin,
    uint32_t platform, uint32_t minos, uint32_t sdk,
    n00b_macho_build_tool_t *tools, uint32_t num_tools);

extern void n00b_macho_add_rpath(n00b_macho_binary_t *bin, const char *path);

extern void n00b_macho_set_version_min(n00b_macho_binary_t *bin,
    uint32_t cmd, uint32_t version, uint32_t sdk);

extern void n00b_macho_add_linker_option(n00b_macho_binary_t *bin,
    const char **strings, uint32_t count);

extern void n00b_macho_set_data_in_code(n00b_macho_binary_t *bin,
    n00b_macho_data_in_code_entry_t *entries, uint32_t count);

extern void n00b_macho_set_encryption_info(n00b_macho_binary_t *bin,
    uint32_t cryptoff, uint32_t cryptsize, uint32_t cryptid);

extern void n00b_macho_add_fileset_entry(n00b_macho_binary_t *bin,
    uint64_t vmaddr, uint64_t fileoff, const char *entry_id);

// ============================================================================
// Builder
// ============================================================================

/**
 * @brief Serialize the Mach-O binary struct to a buffer.
 *
 * Auto-generates load commands, __LINKEDIT content (nlist + string table,
 * plus dyld info if bindings/rebases/exports are present), and computes
 * layout with page alignment.
 *
 * @param bin  Populated Mach-O binary struct.
 * @return Ok(buffer) or Err(N00B_ERR_BUILD).
 */
extern n00b_result_t(n00b_buffer_t *) n00b_macho_build(
    n00b_macho_binary_t *bin);

/**
 * @brief Serialize a fat/universal binary to a buffer.
 *
 * Builds each contained binary via `n00b_macho_build()`, then wraps
 * them in a fat container with proper alignment.
 *
 * @param fat  Fat binary container.
 * @return Ok(buffer) or Err(N00B_ERR_BUILD).
 */
extern n00b_result_t(n00b_buffer_t *) n00b_macho_build_fat(
    n00b_macho_fat_t *fat);
