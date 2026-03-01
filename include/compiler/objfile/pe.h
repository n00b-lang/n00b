/**
 * @file n00b_pe.h
 * @brief Parsed PE/COFF types and parse/query API.
 *
 * Only PE32+ (64-bit) is supported.  PE32 (magic 0x10B) returns
 * `N00B_ERR_NOT_SUPPORTED`.  PE is always little-endian.
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"

#include "compiler/objfile/pe_types.h"
#include "compiler/objfile/bstream.h"

// ============================================================================
// Parsed section
// ============================================================================

typedef struct n00b_pe_section {
    n00b_string_t  *name;
    uint32_t        virtual_size;
    uint32_t        virtual_address;
    uint32_t        raw_size;
    uint32_t        raw_offset;
    uint32_t        characteristics;
    n00b_buffer_t  *content;          ///< Nullable (.bss-like sections)
} n00b_pe_section_t;

// ============================================================================
// Import types
// ============================================================================

typedef struct n00b_pe_imported_func {
    n00b_string_t  *name;       ///< Nullable (ordinal import)
    uint16_t        hint;
    uint16_t        ordinal;
    bool            is_ordinal;
    uint64_t        iat_value;  ///< IAT entry value
} n00b_pe_imported_func_t;

typedef struct n00b_pe_import {
    n00b_string_t           *name;       ///< DLL name
    n00b_pe_imported_func_t *functions;
    uint32_t                 num_functions;
    uint32_t                 ilt_rva;     ///< For builder
    uint32_t                 iat_rva;     ///< For builder
    uint32_t                 time_date_stamp;
    uint32_t                 forwarder_chain;
} n00b_pe_import_t;

// ============================================================================
// Export types
// ============================================================================

typedef struct n00b_pe_exported_func {
    n00b_string_t  *name;              ///< Nullable (ordinal-only)
    uint32_t        ordinal;
    uint32_t        rva;
    bool            is_forwarded;
    n00b_string_t  *forward_name;      ///< Nullable (full "lib.func")
    n00b_string_t  *forward_library;   ///< Nullable (part before '.')
    n00b_string_t  *forward_function;  ///< Nullable (part after '.')
} n00b_pe_exported_func_t;

typedef struct n00b_pe_export_info {
    n00b_string_t           *name;         ///< Module name
    uint32_t                 ordinal_base;
    n00b_pe_exported_func_t *functions;
    uint32_t                 num_functions;
    uint32_t                 characteristics;
    uint32_t                 time_date_stamp;
    uint16_t                 major_version;
    uint16_t                 minor_version;
} n00b_pe_export_info_t;

// ============================================================================
// Relocation types
// ============================================================================

typedef struct n00b_pe_relocation {
    uint32_t rva;    ///< page_rva + (entry & 0x0FFF)
    uint8_t  type;   ///< Upper 4 bits of entry
} n00b_pe_relocation_t;

// ============================================================================
// Debug types
// ============================================================================

/// POGO (Profile Guided Optimization) entry.
typedef struct n00b_pe_pogo_entry {
    uint32_t        rva;
    uint32_t        size;
    n00b_string_t  *name;           ///< Section/symbol name
} n00b_pe_pogo_entry_t;

typedef struct n00b_pe_debug_entry {
    uint32_t       characteristics;
    uint32_t       time_date_stamp;
    uint16_t       major_version;
    uint16_t       minor_version;
    uint32_t       type;
    uint32_t       size_of_data;
    uint32_t       address_of_raw_data;
    uint32_t       pointer_to_raw_data;
    n00b_string_t *pdb_path;        ///< Nullable, filled for CODEVIEW/RSDS
    n00b_buffer_t *raw_data;        ///< Nullable

    // CODEVIEW/RSDS typed fields
    uint8_t        guid[16];        ///< RSDS GUID (filled for CODEVIEW)
    uint32_t       age;             ///< RSDS age

    // POGO typed fields
    n00b_pe_pogo_entry_t *pogo_entries;
    uint32_t              num_pogo_entries;

    // REPRO typed fields
    n00b_buffer_t *repro_hash;      ///< Nullable (REPRO hash)

    // PDBCHECKSUM typed fields
    n00b_string_t *checksum_algorithm;  ///< e.g. "SHA256"
    n00b_buffer_t *checksum_data;       ///< Nullable

    // EX_DLLCHAR typed fields
    uint32_t       ex_dll_characteristics;
} n00b_pe_debug_entry_t;

// ============================================================================
// TLS types
// ============================================================================

typedef struct n00b_pe_tls {
    uint64_t       raw_data_start_va;
    uint64_t       raw_data_end_va;
    uint64_t       address_of_index;
    uint64_t      *callbacks;          ///< VA array, nullptr-terminated
    uint32_t       num_callbacks;
    uint32_t       size_of_zero_fill;
    uint32_t       characteristics;
    n00b_buffer_t *raw_data;           ///< Nullable
} n00b_pe_tls_t;

// ============================================================================
// Resource types
// ============================================================================

typedef struct n00b_pe_resource_node {
    bool            is_directory;
    uint32_t        id;                ///< Numeric ID (or 0 if named)
    n00b_string_t  *name;             ///< Nullable (named entry)
    // Directory fields (if is_directory):
    struct n00b_pe_resource_node *children;
    uint32_t                      num_children;
    // Data leaf fields (if !is_directory):
    n00b_buffer_t  *data;
    uint32_t        code_page;
} n00b_pe_resource_node_t;

// ============================================================================
// Load Configuration types
// ============================================================================

typedef struct n00b_pe_load_config {
    uint32_t  size;                       ///< Declared size in bytes
    uint32_t  time_date_stamp;
    uint16_t  major_version;
    uint16_t  minor_version;
    uint32_t  global_flags_clear;
    uint32_t  global_flags_set;
    uint32_t  critical_section_default_timeout;
    uint64_t  decommit_free_block_threshold;
    uint64_t  decommit_total_free_threshold;
    uint64_t  lock_prefix_table;
    uint64_t  maximum_allocation_size;
    uint64_t  virtual_memory_threshold;
    uint64_t  process_affinity_mask;
    uint32_t  process_heap_flags;
    uint16_t  csd_version;
    uint16_t  dependent_load_flags;
    uint64_t  edit_list;
    uint64_t  security_cookie;
    uint64_t  se_handler_table;
    uint64_t  se_handler_count;
    // CFG fields (from Win 8.1+)
    uint64_t  guard_cf_check_function_pointer;
    uint64_t  guard_cf_dispatch_function_pointer;
    uint64_t  guard_cf_function_table;
    uint64_t  guard_cf_function_count;
    uint32_t  guard_flags;
} n00b_pe_load_config_t;

#define N00B_PE_GUARD_CF_INSTRUMENTED        0x00000100
#define N00B_PE_GUARD_CFW_INSTRUMENTED       0x00000200
#define N00B_PE_GUARD_CF_FUNCTION_TABLE_PRESENT 0x00000400

// ============================================================================
// Certificate types
// ============================================================================

typedef struct n00b_pe_certificate {
    uint16_t        revision;
    uint16_t        certificate_type;
    n00b_buffer_t  *raw_data;
} n00b_pe_certificate_t;

// ============================================================================
// Delay import types
// ============================================================================

typedef struct n00b_pe_delay_import {
    n00b_string_t           *name;           ///< DLL name
    uint32_t                 attributes;
    uint32_t                 handle_rva;      ///< Module handle RVA
    n00b_pe_imported_func_t *functions;
    uint32_t                 num_functions;
} n00b_pe_delay_import_t;

// ============================================================================
// Exception types (x64 only)
// ============================================================================

typedef struct n00b_pe_exception_entry {
    uint32_t begin_rva;
    uint32_t end_rva;
    uint32_t unwind_rva;
} n00b_pe_exception_entry_t;

// ============================================================================
// COFF symbol types
// ============================================================================

typedef struct n00b_pe_symbol {
    n00b_string_t  *name;
    uint32_t        value;
    int16_t         section_number;   ///< 1-based, 0=UNDEFINED, -1=ABSOLUTE, -2=DEBUG
    uint16_t        type;
    uint8_t         storage_class;
} n00b_pe_symbol_t;

// ============================================================================
// Bound import types
// ============================================================================

typedef struct n00b_pe_bound_import {
    n00b_string_t  *name;
    uint32_t        time_date_stamp;
} n00b_pe_bound_import_t;

// ============================================================================
// Top-level binary struct
// ============================================================================

typedef struct n00b_pe_binary {
    // DOS header (full 64 bytes)
    n00b_pe_dos_header_t dos_header;
    uint32_t        pe_offset;        ///< e_lfanew (== dos_header.e_lfanew)

    // Rich header
    n00b_pe_rich_entry_t *rich_entries;
    uint32_t              num_rich_entries;
    uint32_t              rich_key;       ///< XOR key
    n00b_buffer_t        *rich_raw;       ///< Nullable, raw decrypted bytes
    uint16_t        machine;
    uint16_t        characteristics;
    uint32_t        time_date_stamp;
    uint32_t        pointer_to_symbol_table;
    uint32_t        number_of_symbols;
    n00b_buffer_t  *dos_stub;         ///< DOS stub between DOS header and PE sig

    // Optional header fields
    uint16_t        magic;            ///< 0x20B for PE32+
    uint8_t         major_linker_version;
    uint8_t         minor_linker_version;
    uint32_t        size_of_code;
    uint32_t        size_of_initialized_data;
    uint32_t        size_of_uninitialized_data;
    uint32_t        entry_point;      ///< RVA
    uint32_t        base_of_code;
    uint64_t        imagebase;
    uint32_t        section_alignment;
    uint32_t        file_alignment;
    uint32_t        size_of_image;
    uint32_t        size_of_headers;
    uint32_t        checksum;
    uint16_t        subsystem;
    uint16_t        dll_characteristics;
    uint16_t        major_os_version;
    uint16_t        minor_os_version;
    uint16_t        major_image_version;
    uint16_t        minor_image_version;
    uint16_t        major_subsystem_version;
    uint16_t        minor_subsystem_version;
    uint32_t        win32_version_value;
    uint64_t        size_of_stack_reserve;
    uint64_t        size_of_stack_commit;
    uint64_t        size_of_heap_reserve;
    uint64_t        size_of_heap_commit;
    uint32_t        loader_flags;

    // Data directories (raw)
    n00b_pe_data_directory_t data_dirs[N00B_PE_NUM_DATA_DIRS];
    uint32_t                 num_data_dirs;

    // Sections
    n00b_pe_section_t *sections;
    uint32_t           num_sections;

    // Imports
    n00b_pe_import_t  *imports;
    uint32_t           num_imports;

    // Exports
    n00b_pe_export_info_t *export_info;  ///< Nullable

    // Relocations
    n00b_pe_relocation_t *relocations;
    uint32_t              num_relocations;

    // Debug
    n00b_pe_debug_entry_t *debug_entries;
    uint32_t               num_debug_entries;

    // TLS
    n00b_pe_tls_t *tls;               ///< Nullable

    // Resources
    n00b_pe_resource_node_t *resources; ///< Nullable (root of tree)

    // Load Configuration
    n00b_pe_load_config_t *load_config; ///< Nullable

    // Delay imports
    n00b_pe_delay_import_t *delay_imports;
    uint32_t                num_delay_imports;

    // Exceptions (x64 RUNTIME_FUNCTION entries)
    n00b_pe_exception_entry_t *exceptions;
    uint32_t                   num_exceptions;

    // COFF symbols
    n00b_pe_symbol_t *symbols;
    uint32_t          num_symbols;

    // Bound imports
    n00b_pe_bound_import_t *bound_imports;
    uint32_t                num_bound_imports;

    // Certificates (Authenticode)
    n00b_pe_certificate_t *certificates;
    uint32_t               num_certificates;

    // Overlay
    n00b_buffer_t  *overlay;

    // Stream
    n00b_bstream_t  *stream;
} n00b_pe_binary_t;

// ============================================================================
// Parse API
// ============================================================================

extern n00b_result_t(n00b_pe_binary_t *) n00b_pe_parse(n00b_bstream_t *stream);

// ============================================================================
// Section queries
// ============================================================================

extern n00b_pe_section_t *n00b_pe_section_by_name(n00b_pe_binary_t *bin,
                                                    const char *name);
extern n00b_pe_section_t *n00b_pe_section_at_rva(n00b_pe_binary_t *bin,
                                                   uint32_t rva);
extern bool               n00b_pe_has_section(n00b_pe_binary_t *bin,
                                               const char *name);
extern bool               n00b_pe_is_dll(n00b_pe_binary_t *bin);

// ============================================================================
// Import queries
// ============================================================================

extern n00b_pe_import_t        *n00b_pe_import_by_name(n00b_pe_binary_t *bin,
                                                        const char *dll);
extern n00b_pe_imported_func_t *n00b_pe_imported_func_by_name(
                                    n00b_pe_import_t *imp, const char *name);
extern bool                     n00b_pe_has_imports(n00b_pe_binary_t *bin);

// ============================================================================
// Export queries
// ============================================================================

extern n00b_pe_exported_func_t *n00b_pe_export_by_name(n00b_pe_binary_t *bin,
                                                        const char *name);
extern n00b_pe_exported_func_t *n00b_pe_export_by_ordinal(
                                    n00b_pe_binary_t *bin, uint32_t ordinal);
extern bool                     n00b_pe_has_exports(n00b_pe_binary_t *bin);

// ============================================================================
// RVA / VA / Offset utilities
// ============================================================================

/// Convert an RVA to a file offset using the section table. Returns 0 on fail.
extern uint32_t n00b_pe_rva_to_offset(n00b_pe_binary_t *bin, uint32_t rva);

/// Convert a file offset to an RVA. Returns 0 on fail.
extern uint32_t n00b_pe_offset_to_rva(n00b_pe_binary_t *bin, uint32_t offset);

/// Convert an RVA to a virtual address (rva + imagebase).
extern uint64_t n00b_pe_rva_to_va(n00b_pe_binary_t *bin, uint32_t rva);

/// Convert a virtual address to an RVA. Returns 0 on fail (VA < imagebase).
extern uint32_t n00b_pe_va_to_rva(n00b_pe_binary_t *bin, uint64_t va);

/// Convert a virtual address to a file offset. Returns 0 on fail.
extern uint32_t n00b_pe_va_to_offset(n00b_pe_binary_t *bin, uint64_t va);

/// Read content at a given RVA from the backing stream.
extern n00b_buffer_t *n00b_pe_get_content_at_rva(n00b_pe_binary_t *bin,
                                                   uint32_t rva, uint32_t size);

/// Read content at a given VA from the backing stream.
extern n00b_buffer_t *n00b_pe_get_content_at_va(n00b_pe_binary_t *bin,
                                                  uint64_t va, uint32_t size);

// ============================================================================
// Rich header queries
// ============================================================================

extern bool               n00b_pe_has_rich_header(n00b_pe_binary_t *bin);

// ============================================================================
// Debug / TLS / Resource queries
// ============================================================================

extern n00b_pe_debug_entry_t *n00b_pe_debug_entry_by_type(
                                   n00b_pe_binary_t *bin, uint32_t type);
extern bool               n00b_pe_has_tls(n00b_pe_binary_t *bin);
extern n00b_pe_tls_t     *n00b_pe_get_tls(n00b_pe_binary_t *bin);
extern bool               n00b_pe_has_debug(n00b_pe_binary_t *bin);
extern n00b_string_t     *n00b_pe_pdb_path(n00b_pe_binary_t *bin);
extern bool               n00b_pe_has_resources(n00b_pe_binary_t *bin);
extern n00b_pe_resource_node_t *n00b_pe_resource_by_type(
                                    n00b_pe_binary_t *bin, uint32_t type_id);

// ============================================================================
// Load Configuration queries
// ============================================================================

extern bool                    n00b_pe_has_configuration(n00b_pe_binary_t *bin);
extern n00b_pe_load_config_t  *n00b_pe_get_load_config(n00b_pe_binary_t *bin);
extern bool                    n00b_pe_has_guard_cf(n00b_pe_binary_t *bin);

// ============================================================================
// Delay import queries
// ============================================================================

extern bool                     n00b_pe_has_delay_imports(n00b_pe_binary_t *bin);
extern n00b_pe_delay_import_t  *n00b_pe_delay_import_by_name(
                                    n00b_pe_binary_t *bin, const char *dll);

// ============================================================================
// Exception queries
// ============================================================================

extern bool                        n00b_pe_has_exceptions(n00b_pe_binary_t *bin);
extern n00b_pe_exception_entry_t  *n00b_pe_exception_at_rva(
                                       n00b_pe_binary_t *bin, uint32_t rva);

// ============================================================================
// COFF symbol queries
// ============================================================================

extern bool n00b_pe_has_symbols(n00b_pe_binary_t *bin);

// ============================================================================
// Bound import queries
// ============================================================================

extern bool n00b_pe_has_bound_imports(n00b_pe_binary_t *bin);

// ============================================================================
// Checksum
// ============================================================================

/// Compute the PE checksum over the binary's backing stream.
extern uint32_t n00b_pe_compute_checksum(n00b_pe_binary_t *bin);

/// Return true if stored checksum matches computed checksum.
extern bool     n00b_pe_verify_checksum(n00b_pe_binary_t *bin);

// ============================================================================
// Section entropy
// ============================================================================

/// Shannon entropy of a section's raw content (0.0 – 8.0).
extern double n00b_pe_section_entropy(n00b_pe_section_t *section);

// ============================================================================
// Demangled names
// ============================================================================

extern n00b_string_t *n00b_pe_imported_func_demangled(
                          n00b_pe_imported_func_t *f);
extern n00b_string_t *n00b_pe_exported_func_demangled(
                          n00b_pe_exported_func_t *f);

// ============================================================================
// Import hash (imphash)
// ============================================================================

/// Compute the imphash (MD5 of lowercase "dll.func" pairs, joined by ",").
extern n00b_string_t *n00b_pe_imphash(n00b_pe_binary_t *bin);

// ============================================================================
// Authenticode / Certificate queries
// ============================================================================

extern bool            n00b_pe_has_signatures(n00b_pe_binary_t *bin);
extern n00b_buffer_t  *n00b_pe_authentihash_sha256(n00b_pe_binary_t *bin);

// ============================================================================
// Resource interpretation
// ============================================================================

/// Key-value pair from VS_VERSIONINFO StringFileInfo.
typedef struct {
    n00b_string_t *key;
    n00b_string_t *value;
} n00b_pe_version_string_t;

/// Parsed VS_VERSIONINFO (RT_VERSION resource).
typedef struct {
    uint32_t  file_version_ms;
    uint32_t  file_version_ls;
    uint32_t  product_version_ms;
    uint32_t  product_version_ls;
    uint32_t  file_flags_mask;
    uint32_t  file_flags;
    uint32_t  file_os;
    uint32_t  file_type;
    uint32_t  file_subtype;

    n00b_pe_version_string_t *strings;
    uint32_t                  num_strings;
} n00b_pe_version_info_t;

extern n00b_pe_version_info_t *n00b_pe_get_version_info(n00b_pe_binary_t *bin);
extern n00b_string_t          *n00b_pe_get_manifest(n00b_pe_binary_t *bin);
extern uint32_t                n00b_pe_icon_count(n00b_pe_binary_t *bin);
extern n00b_buffer_t          *n00b_pe_get_icon(n00b_pe_binary_t *bin,
                                                uint32_t          index);

// ============================================================================
// Relocation block grouping
// ============================================================================

/// A group of relocations sharing the same 4KB page.
typedef struct {
    uint32_t              page_rva;      ///< Base page RVA (rva & ~0xFFF)
    n00b_pe_relocation_t *entries;       ///< Pointer into the binary's flat array
    uint32_t              num_entries;
} n00b_pe_reloc_block_t;

/// Group the binary's flat relocation array by 4KB page.
/// Caller receives an array of blocks and their count.
extern n00b_pe_reloc_block_t *n00b_pe_reloc_blocks(n00b_pe_binary_t *bin,
                                                     uint32_t *num_blocks);
