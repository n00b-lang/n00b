/**
 * @file n00b_abstract.h
 * @brief Format-agnostic binary interface and auto-detection.
 *
 * Provides the abstract `n00b_binary_t` handle that dispatches to
 * format-specific implementations (ELF, Mach-O).
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "adt/option.h"
#include "adt/result.h"
#include "compiler/objfile/types.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/macho.h"
#include "compiler/objfile/pe.h"
#include "compiler/objfile/dwarf.h"

// ============================================================================
// Abstract binary struct
// ============================================================================

/**
 * @brief Format-agnostic binary handle.
 *
 * `impl` points to the format-specific struct (e.g., a future
 * `n00b_elf_binary_t *` or `n00b_macho_binary_t *`).  Abstract
 * accessors dispatch through `format` to extract common properties.
 */
struct n00b_binary {
    n00b_format_t  format;
    n00b_arch_t    arch;
    uint64_t       entrypoint;
    uint64_t       imagebase;
    bool           is_pie;
    void          *impl;  ///< Format-specific backing struct (GC-managed).
};

// ============================================================================
// Abstract accessors
// ============================================================================

extern n00b_format_t n00b_binary_format(n00b_binary_t *b);
extern n00b_arch_t   n00b_binary_arch(n00b_binary_t *b);
extern uint64_t      n00b_binary_entrypoint(n00b_binary_t *b);
extern bool          n00b_binary_is_pie(n00b_binary_t *b);
extern uint64_t      n00b_binary_imagebase(n00b_binary_t *b);

// ============================================================================
// Abstract section
// ============================================================================

typedef struct n00b_abstract_section {
    n00b_string_t  *name;
    uint64_t        addr;
    uint64_t        size;
    n00b_buffer_t  *content;  ///< Nullable.
} n00b_abstract_section_t;

/// Number of sections in the binary.
extern uint32_t n00b_binary_section_count(n00b_binary_t *b);

/// Get abstract section at index.  Returns zeroed struct if out of range.
extern n00b_abstract_section_t n00b_binary_section_at(n00b_binary_t *b,
                                                       uint32_t idx);

// ============================================================================
// Abstract symbol
// ============================================================================

typedef struct n00b_abstract_symbol {
    n00b_string_t  *name;
    n00b_string_t  *demangled_name;  ///< Nullable.
    uint64_t        value;
    uint64_t        size;
} n00b_abstract_symbol_t;

/// Total number of symbols (symtab + dynsym for ELF, nlist for MachO).
extern uint32_t n00b_binary_symbol_count(n00b_binary_t *b);

/// Get abstract symbol at index.  Returns zeroed struct if out of range.
extern n00b_abstract_symbol_t n00b_binary_symbol_at(n00b_binary_t *b,
                                                     uint32_t idx);

// ============================================================================
// Format detection
// ============================================================================

/**
 * @brief Detect binary format from magic bytes.
 *
 * Checks for:
 * - ELF:   `\\x7fELF`
 * - MachO: `0xfeedface`, `0xfeedfacf` (native endian), `0xcafebabe` (fat)
 *
 * @param s  Stream positioned at byte 0 (position is not modified).
 * @return   Detected format, or `N00B_FMT_UNKNOWN`.
 */
extern n00b_format_t n00b_detect_format(n00b_bstream_t *s);

// ============================================================================
// Top-level parse
// ============================================================================

/**
 * @brief Parse a binary from a file path.
 *
 * Reads the file, auto-detects format, and dispatches to the appropriate
 * parser (ELF64 or Mach-O 64).  Populates abstract fields and stores
 * the format-specific binary via `impl`.
 *
 * @param path  Filesystem path.
 * @return      Ok(binary) or Err(error code).
 */
extern n00b_result_t(n00b_binary_t *) n00b_parse_file(const char *path);

// ============================================================================
// Downcast helpers
// ============================================================================

/**
 * @brief Get the ELF-specific binary if format is ELF.
 * @return Some(ELF binary pointer) if format is ELF and the typed
 *         binary record was parsed; none otherwise (non-ELF format,
 *         or partial-parse with null impl).
 */
extern n00b_option_t(n00b_elf_binary_t *) n00b_binary_as_elf(n00b_binary_t *b);

/**
 * @brief Get the first MachO binary if format is MachO.
 * @return Some(MachO binary pointer) if format is MachO, the fat
 *         container is non-empty, and the typed binary record was
 *         parsed; none otherwise.
 */
extern n00b_option_t(n00b_macho_binary_t *)
    n00b_binary_as_macho(n00b_binary_t *b);

/**
 * @brief Get the fat container if format is MachO.
 * @return Some(fat container pointer) if format is MachO and the fat
 *         container was parsed; none otherwise (non-MachO format,
 *         or partial-parse with null impl).
 */
extern n00b_option_t(n00b_macho_fat_t *)
    n00b_binary_as_macho_fat(n00b_binary_t *b);

/**
 * @brief Get the PE-specific binary if format is PE.
 * @return Some(PE binary pointer) if format is PE and the typed
 *         binary record was parsed; none otherwise (non-PE format,
 *         or partial-parse with null impl).
 */
extern n00b_option_t(n00b_pe_binary_t *) n00b_binary_as_pe(n00b_binary_t *b);

// ============================================================================
// DWARF debug info (abstract dispatch)
// ============================================================================

/**
 * @brief Parse DWARF debug information from a binary.
 *
 * Auto-detects format (ELF or Mach-O) and extracts DWARF sections.
 * PE binaries are not supported (returns Err).
 *
 * @param b  Abstract binary handle.
 * @return   Ok(dwarf info) or Err(error code).
 */
extern n00b_result_t(n00b_dwarf_info_t *)
    n00b_binary_dwarf(n00b_binary_t *b);
