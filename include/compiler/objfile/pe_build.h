/**
 * @file n00b_pe_build.h
 * @brief PE binary builder — struct helpers and serialization.
 *
 * Provides functions to create and populate `n00b_pe_binary_t` structs
 * from scratch, then serialize them to `n00b_buffer_t *` via
 * `n00b_pe_build()`.
 */
#pragma once

#include "compiler/objfile/pe.h"
#include "compiler/objfile/writer.h"
#include "adt/result.h"

// ============================================================================
// Binary creation
// ============================================================================

/**
 * @brief Create a new empty PE32+ binary struct.
 *
 * Initializes with default alignments (section_alignment=0x1000,
 * file_alignment=0x200) and standard PE32+ magic.
 *
 * @param machine    Machine type (N00B_PE_MACHINE_AMD64, etc.).
 * @param subsystem  Subsystem (N00B_PE_SUBSYSTEM_WINDOWS_CUI, etc.).
 */
extern n00b_pe_binary_t *n00b_pe_binary_new(uint16_t machine,
                                             uint16_t subsystem);

// ============================================================================
// Section management
// ============================================================================

/**
 * @brief Add a new section to the binary.
 * @return Pointer to the new section (within the binary's sections array).
 */
extern n00b_pe_section_t *n00b_pe_add_section(n00b_pe_binary_t *bin,
                                               const char *name,
                                               uint32_t characteristics);

// ============================================================================
// Import management
// ============================================================================

extern n00b_pe_import_t        *n00b_pe_add_import(n00b_pe_binary_t *bin,
                                                    const char *dll_name);

extern n00b_pe_imported_func_t *n00b_pe_add_imported_func(
                                    n00b_pe_import_t *imp,
                                    const char *name, uint16_t hint);

extern n00b_pe_imported_func_t *n00b_pe_add_imported_func_ordinal(
                                    n00b_pe_import_t *imp,
                                    uint16_t ordinal);

// ============================================================================
// Export management
// ============================================================================

/**
 * @brief Initialize export info on the binary (if not already set)
 *        and add an exported function.
 */
extern n00b_pe_exported_func_t *n00b_pe_add_export(n00b_pe_binary_t *bin,
                                                    const char *name,
                                                    uint32_t rva,
                                                    uint32_t ordinal);

/// Set the module name for the export directory.
extern void n00b_pe_set_export_name(n00b_pe_binary_t *bin, const char *name);

// ============================================================================
// Relocation management
// ============================================================================

extern void n00b_pe_add_relocation(n00b_pe_binary_t *bin,
                                    uint32_t rva, uint8_t type);

// ============================================================================
// Mutation APIs
// ============================================================================

/// Remove a section by name (case-sensitive). Shifts subsequent sections down.
extern void n00b_pe_remove_section(n00b_pe_binary_t *bin, const char *name);

/// Remove an import DLL by name (case-insensitive).
extern void n00b_pe_remove_import(n00b_pe_binary_t *bin, const char *dll_name);

/// Remove all imports.
extern void n00b_pe_remove_all_imports(n00b_pe_binary_t *bin);

/// Remove an exported function by name.
extern void n00b_pe_remove_export(n00b_pe_binary_t *bin,
                                   const char *func_name);

/// Set (or allocate) the TLS structure on the binary.
extern n00b_pe_tls_t *n00b_pe_set_tls(n00b_pe_binary_t *bin);

/// Remove TLS from the binary.
extern void n00b_pe_remove_tls(n00b_pe_binary_t *bin);

/// Add a TLS callback VA.
extern void n00b_pe_add_tls_callback(n00b_pe_binary_t *bin,
                                      uint64_t callback_va);

/// Strip synthetic sections (.idata, .edata, .reloc) from a parsed binary
/// so the builder can regenerate them from the import/export/reloc arrays.
extern void n00b_pe_strip_synthetic_sections(n00b_pe_binary_t *bin);

// ============================================================================
// Builder
// ============================================================================

/**
 * @brief Serialize the PE binary struct to a buffer.
 *
 * Emits DOS header + stub, PE signature, file header, optional header,
 * section headers, synthetic sections (.idata, .edata, .reloc) as needed,
 * and user section data with proper file/section alignment.
 *
 * @param bin  Populated PE binary struct.
 * @return Ok(buffer) or Err(N00B_ERR_BUILD).
 */
extern n00b_result_t(n00b_buffer_t *) n00b_pe_build(n00b_pe_binary_t *bin);
