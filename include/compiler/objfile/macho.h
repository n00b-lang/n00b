/**
 * @file n00b_macho.h
 * @brief High-level parsed Mach-O 64-bit types and parse API.
 *
 * Provides fully-parsed Mach-O binary representation with resolved names,
 * extracted content, and structured access to segments, sections, symbols,
 * dylibs, binding/rebase info, exports, and code signatures.
 */
#pragma once

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "compiler/objfile/types.h"
#include "compiler/objfile/bstream.h"
#include "compiler/objfile/macho_types.h"

// Forward declarations for circular references.
typedef struct n00b_macho_section  n00b_macho_section_t;
typedef struct n00b_macho_segment  n00b_macho_segment_t;

// ============================================================================
// Parsed header
// ============================================================================

typedef struct n00b_macho_header {
    uint32_t  magic;
    uint32_t  cputype;
    uint32_t  cpusubtype;
    uint32_t  filetype;
    uint32_t  ncmds;
    uint32_t  sizeofcmds;
    uint32_t  flags;
    uint32_t  reserved;
} n00b_macho_header_t;

// ============================================================================
// Load command (raw)
// ============================================================================

typedef struct n00b_macho_command {
    uint32_t        cmd;
    uint32_t        cmdsize;
    n00b_buffer_t  *raw_data;
} n00b_macho_command_t;

// ============================================================================
// Parsed relocation
// ============================================================================

typedef struct n00b_macho_relocation {
    int32_t         address;
    uint32_t        symbolnum;
    bool            pcrel;
    uint8_t         length;
    bool            is_extern;
    uint8_t         type;
    bool            scattered;
    int32_t         scattered_value;
} n00b_macho_relocation_t;

// ============================================================================
// Parsed section
// ============================================================================

struct n00b_macho_section {
    char            sectname[17];
    char            segname[17];
    uint64_t        addr;
    uint64_t        size;
    uint32_t        offset;
    uint32_t        align;
    uint32_t        reloff;
    uint32_t        nreloc;
    uint32_t        flags;
    uint32_t        reserved1;
    uint32_t        reserved2;
    uint32_t        reserved3;
    n00b_buffer_t  *content;
    n00b_macho_relocation_t *relocations;
    uint32_t        num_relocations;
};

// ============================================================================
// Parsed segment
// ============================================================================

struct n00b_macho_segment {
    char                     name[17];
    uint64_t                 vmaddr;
    uint64_t                 vmsize;
    uint64_t                 fileoff;
    uint64_t                 filesize;
    uint32_t                 maxprot;
    uint32_t                 initprot;
    uint32_t                 nsects;
    uint32_t                 flags;
    n00b_buffer_t           *content;
    n00b_macho_section_t    *sections;
};

// ============================================================================
// Parsed symbol
// ============================================================================

typedef struct n00b_macho_symbol {
    n00b_string_t  *name;
    n00b_string_t  *demangled_name; ///< Auto-demangled (C++/Rust), or nullptr.
    uint8_t         type;
    uint8_t         sect;
    uint16_t        desc;
    uint64_t        value;
} n00b_macho_symbol_t;

// ============================================================================
// Parsed dylib reference
// ============================================================================

typedef struct n00b_macho_dylib {
    n00b_string_t  *name;
    uint32_t        timestamp;
    uint32_t        current_version;
    uint32_t        compat_version;
    uint32_t        cmd;
} n00b_macho_dylib_t;

// ============================================================================
// Binding info
// ============================================================================

typedef struct n00b_macho_binding {
    uint8_t         type;
    int64_t         addend;
    uint64_t        address;
    n00b_string_t  *symbol_name;
    int32_t         library_ordinal;
    uint8_t         segment_index;
    bool            is_weak;
    bool            is_lazy;
} n00b_macho_binding_t;

// ============================================================================
// Rebase info
// ============================================================================

typedef struct n00b_macho_rebase {
    uint8_t         type;
    uint64_t        address;
    uint8_t         segment_index;
} n00b_macho_rebase_t;

// ============================================================================
// Export trie node
// ============================================================================

typedef struct n00b_macho_export {
    n00b_string_t  *name;
    uint64_t        address;
    uint64_t        flags;
    uint64_t        other;
    n00b_string_t  *import_name;
} n00b_macho_export_t;

// ============================================================================
// Code signature
// ============================================================================

typedef struct n00b_macho_code_signature {
    uint32_t        dataoff;
    uint32_t        datasize;
    n00b_buffer_t  *data;
} n00b_macho_code_signature_t;

// ============================================================================
// Function starts
// ============================================================================

typedef struct n00b_macho_function_starts {
    uint64_t       *addresses;
    uint32_t        count;
} n00b_macho_function_starts_t;

// ============================================================================
// Build version (LC_BUILD_VERSION)
// ============================================================================

typedef struct n00b_macho_build_tool {
    uint32_t        tool;
    uint32_t        version;
} n00b_macho_build_tool_t;

typedef struct n00b_macho_build_version {
    uint32_t                platform;
    uint32_t                minos;
    uint32_t                sdk;
    n00b_macho_build_tool_t *tools;
    uint32_t                num_tools;
} n00b_macho_build_version_t;

// ============================================================================
// Version min (LC_VERSION_MIN_*)
// ============================================================================

typedef struct n00b_macho_version_min {
    uint32_t        cmd;
    uint32_t        version;
    uint32_t        sdk;
} n00b_macho_version_min_t;

// ============================================================================
// Data in code (LC_DATA_IN_CODE)
// ============================================================================

typedef struct n00b_macho_data_in_code_entry {
    uint32_t        offset;
    uint16_t        length;
    uint16_t        kind;
} n00b_macho_data_in_code_entry_t;

typedef struct n00b_macho_data_in_code {
    n00b_macho_data_in_code_entry_t *entries;
    uint32_t                         count;
} n00b_macho_data_in_code_t;

// ============================================================================
// Encryption info (LC_ENCRYPTION_INFO_64)
// ============================================================================

typedef struct n00b_macho_encryption_info {
    uint32_t        cryptoff;
    uint32_t        cryptsize;
    uint32_t        cryptid;
} n00b_macho_encryption_info_t;

// ============================================================================
// Fileset entry (LC_FILESET_ENTRY)
// ============================================================================

typedef struct n00b_macho_fileset_entry {
    uint64_t        vmaddr;
    uint64_t        fileoff;
    n00b_string_t  *entry_id;
    uint32_t        reserved;
} n00b_macho_fileset_entry_t;

// ============================================================================
// Linker option (LC_LINKER_OPTION)
// ============================================================================

typedef struct n00b_macho_linker_option {
    n00b_string_t **strings;
    uint32_t        count;
} n00b_macho_linker_option_t;

// ============================================================================
// Code signature (parsed detail)
// ============================================================================

typedef struct n00b_macho_cs_code_directory {
    uint32_t        version;
    uint32_t        flags;
    n00b_string_t  *identifier;
    n00b_string_t  *team_id;
    uint8_t         hash_type;
    uint8_t         hash_size;
    uint32_t        n_code_slots;
    uint32_t        n_special_slots;
    uint32_t        code_limit;
    uint32_t        page_size;
} n00b_macho_cs_code_directory_t;

typedef struct n00b_macho_cs_blob {
    uint32_t        type;
    uint32_t        offset;
    n00b_buffer_t  *data;
} n00b_macho_cs_blob_t;

typedef struct n00b_macho_code_signature_parsed {
    n00b_macho_cs_code_directory_t *code_directory;
    n00b_string_t                   *entitlements_xml;
    n00b_buffer_t                   *requirements;
    n00b_buffer_t                   *cms_signature;
    n00b_macho_cs_blob_t            *blobs;
    uint32_t                         num_blobs;
} n00b_macho_code_signature_parsed_t;

// ============================================================================
// Chained fixups (LC_DYLD_CHAINED_FIXUPS)
// ============================================================================

typedef struct n00b_macho_chained_import {
    int32_t         lib_ordinal;
    bool            weak_import;
    n00b_string_t  *symbol_name;
    int64_t         addend;
} n00b_macho_chained_import_t;

typedef struct n00b_macho_chained_fixups {
    n00b_macho_chained_import_t *imports;
    uint32_t                     num_imports;
    uint32_t                     imports_format;
    uint32_t                     symbols_format;
    n00b_buffer_t               *raw_data;  ///< Raw __LINKEDIT blob for passthrough.
} n00b_macho_chained_fixups_t;

// ============================================================================
// Top-level Mach-O binary
// ============================================================================

typedef struct n00b_macho_binary {
    n00b_macho_header_t            header;
    n00b_macho_command_t          *commands;
    uint32_t                       num_commands;
    n00b_macho_segment_t          *segments;
    uint32_t                       num_segments;
    n00b_macho_symbol_t           *symbols;
    uint32_t                       num_symbols;
    n00b_macho_dylib_t            *dylibs;
    uint32_t                       num_dylibs;
    n00b_macho_binding_t          *bindings;
    uint32_t                       num_bindings;
    n00b_macho_rebase_t           *rebases;
    uint32_t                       num_rebases;
    n00b_macho_export_t           *exports;
    uint32_t                       num_exports;
    n00b_macho_code_signature_t   *code_signature;
    n00b_macho_code_signature_parsed_t *code_signature_parsed;
    n00b_macho_function_starts_t  *function_starts;
    n00b_macho_chained_fixups_t   *chained_fixups;
    n00b_macho_build_version_t    *build_version;
    n00b_macho_version_min_t      *version_min;
    n00b_macho_data_in_code_t     *data_in_code;
    n00b_macho_encryption_info_t  *encryption_info;
    n00b_macho_fileset_entry_t    *fileset_entries;
    uint32_t                       num_fileset_entries;
    n00b_macho_linker_option_t    *linker_options;
    uint32_t                       num_linker_options;
    n00b_string_t                **rpaths;
    uint32_t                       num_rpaths;
    uint32_t                      *indirect_symbols;
    uint32_t                       num_indirect_symbols;
    n00b_string_t                 *dylinker;
    uint8_t                        uuid[16];
    uint64_t                       source_version;
    uint64_t                       entrypoint;       ///< From LC_MAIN entryoff.
    uint64_t                       stack_size;        ///< From LC_MAIN stacksize.
    n00b_buffer_t                 *overlay;
    n00b_bstream_t                 *stream;
    bool                           is_fat;
    uint64_t                       fat_offset;
} n00b_macho_binary_t;

// ============================================================================
// Fat binary container
// ============================================================================

typedef struct n00b_macho_fat {
    n00b_macho_binary_t **binaries;
    uint32_t              count;
} n00b_macho_fat_t;

// ============================================================================
// Parse API
// ============================================================================

/**
 * @brief Parse a Mach-O binary from a stream.
 *
 * Handles both single-architecture and fat/universal binaries.
 * Even non-fat binaries are wrapped in a `n00b_macho_fat_t` with `count = 1`.
 *
 * @param stream  Binary stream to parse from.
 * @return Ok(fat) or Err(N00B_ERR_*).
 */
extern n00b_result_t(n00b_macho_fat_t *) n00b_macho_parse(n00b_bstream_t *stream);

/**
 * @brief Parse a single Mach-O 64-bit binary from a stream.
 *
 * The stream should be positioned at the start of the Mach-O header.
 *
 * @param stream  Binary stream to parse from.
 * @return Ok(binary) or Err(N00B_ERR_*).
 */
extern n00b_result_t(n00b_macho_binary_t *) n00b_macho_parse_single(
    n00b_bstream_t *stream);

// ============================================================================
// Query API
// ============================================================================

/// Find a segment by name (e.g. "__TEXT"), or nullptr.
extern n00b_macho_segment_t *n00b_macho_segment_by_name(
    n00b_macho_binary_t *bin, const char *name);

/// Find a section by segment + section name, or nullptr.
extern n00b_macho_section_t *n00b_macho_section_by_name(
    n00b_macho_binary_t *bin, const char *segname, const char *sectname);

/// Find a symbol by name, or nullptr.
extern n00b_macho_symbol_t  *n00b_macho_symbol_by_name(
    n00b_macho_binary_t *bin, const char *name);

/// Find a dylib by substring match on its path, or nullptr.
extern n00b_macho_dylib_t   *n00b_macho_dylib_by_name(
    n00b_macho_binary_t *bin, const char *name);

/// Find an export by name, or nullptr.
extern n00b_macho_export_t  *n00b_macho_export_by_name(
    n00b_macho_binary_t *bin, const char *name);

/// Find a binding by symbol name, or nullptr.
extern n00b_macho_binding_t *n00b_macho_binding_by_symbol(
    n00b_macho_binary_t *bin, const char *name);

/// Find the first load command with the given type, or nullptr.
extern n00b_macho_command_t *n00b_macho_command_by_type(
    n00b_macho_binary_t *bin, uint32_t cmd);

/// True if a segment with the given name exists.
extern bool                  n00b_macho_has_segment(
    n00b_macho_binary_t *bin, const char *name);

/// True if a dylib matching the substring exists.
extern bool                  n00b_macho_has_dylib(
    n00b_macho_binary_t *bin, const char *name);

/// True if the binary has a non-zero entrypoint.
extern bool                  n00b_macho_has_entrypoint(
    n00b_macho_binary_t *bin);

/// Find a binary within a fat container by CPU type, or nullptr.
extern n00b_macho_binary_t  *n00b_macho_fat_by_cputype(
    n00b_macho_fat_t *fat, uint32_t cputype);

/// True if the binary has an LC_BUILD_VERSION.
extern bool n00b_macho_has_build_version(n00b_macho_binary_t *bin);

/// Get the build version info, or nullptr.
extern n00b_macho_build_version_t *n00b_macho_get_build_version(
    n00b_macho_binary_t *bin);

/// True if the binary has at least one LC_RPATH.
extern bool n00b_macho_has_rpath(n00b_macho_binary_t *bin);

/// Get the rpath at the given index, or nullptr.
extern n00b_string_t *n00b_macho_rpath_at(n00b_macho_binary_t *bin, uint32_t idx);

/// True if the binary has a version min command.
extern bool n00b_macho_has_version_min(n00b_macho_binary_t *bin);

/// Get the version min info, or nullptr.
extern n00b_macho_version_min_t *n00b_macho_get_version_min(
    n00b_macho_binary_t *bin);

/// True if the binary has LC_DATA_IN_CODE.
extern bool n00b_macho_has_data_in_code(n00b_macho_binary_t *bin);

/// Get the data-in-code info, or nullptr.
extern n00b_macho_data_in_code_t *n00b_macho_get_data_in_code(
    n00b_macho_binary_t *bin);

/// True if the binary has LC_ENCRYPTION_INFO_64.
extern bool n00b_macho_has_encryption_info(n00b_macho_binary_t *bin);

/// Get the encryption info, or nullptr.
extern n00b_macho_encryption_info_t *n00b_macho_get_encryption_info(
    n00b_macho_binary_t *bin);

/// Get the parsed code signature, or nullptr.
extern n00b_macho_code_signature_parsed_t *n00b_macho_get_code_signature(
    n00b_macho_binary_t *bin);

/// Get entitlements plist XML string, or nullptr.
extern n00b_string_t *n00b_macho_get_entitlements(n00b_macho_binary_t *bin);

/// Get code signing identifier, or nullptr.
extern n00b_string_t *n00b_macho_codesign_identifier(n00b_macho_binary_t *bin);

/// Get code signing team ID, or nullptr.
extern n00b_string_t *n00b_macho_codesign_team_id(n00b_macho_binary_t *bin);
