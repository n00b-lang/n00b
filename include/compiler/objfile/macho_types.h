/**
 * @file n00b_macho_types.h
 * @brief Raw on-disk Mach-O 64-bit packed structures and constants.
 *
 * All structures use `#pragma pack(push, 1)` to match on-disk layout.
 * Only 64-bit variants are provided.
 */
#pragma once

#include <stdint.h>

// ============================================================================
// Mach-O magic numbers
// ============================================================================

#define MH_MAGIC_64    0xFEEDFACFu
#define MH_CIGAM_64    0xCFFAEDFEu
#define MH_MAGIC       0xFEEDFACEu
#define MH_CIGAM       0xCEFAEDFEu
#define FAT_MAGIC      0xCAFEBABEu
#define FAT_CIGAM      0xBEBAFECAu

// ============================================================================
// File types (mh_filetype)
// ============================================================================

#define MH_OBJECT      1
#define MH_EXECUTE     2
#define MH_FVMLIB      3
#define MH_CORE        4
#define MH_PRELOAD     5
#define MH_DYLIB       6
#define MH_DYLINKER    7
#define MH_BUNDLE      8
#define MH_DYLIB_STUB  9
#define MH_DSYM        10
#define MH_KEXT_BUNDLE 11
#define MH_FILESET     12

// ============================================================================
// Mach-O header flags
// ============================================================================

#define MH_NOUNDEFS               0x00000001u
#define MH_INCRLINK               0x00000002u
#define MH_DYLDLINK               0x00000004u
#define MH_BINDATLOAD             0x00000008u
#define MH_PREBOUND               0x00000010u
#define MH_SPLIT_SEGS             0x00000020u
#define MH_TWOLEVEL               0x00000080u
#define MH_FORCE_FLAT             0x00000100u
#define MH_NOMULTIDEFS            0x00000200u
#define MH_NOFIXPREBINDING        0x00000400u
#define MH_PREBINDABLE            0x00000800u
#define MH_ALLMODSBOUND           0x00001000u
#define MH_SUBSECTIONS_VIA_SYMBOLS 0x00002000u
#define MH_CANONICAL              0x00004000u
#define MH_WEAK_DEFINES           0x00008000u
#define MH_BINDS_TO_WEAK          0x00010000u
#define MH_ALLOW_STACK_EXECUTION  0x00020000u
#define MH_ROOT_SAFE              0x00040000u
#define MH_SETUID_SAFE            0x00080000u
#define MH_NO_REEXPORTED_DYLIBS  0x00100000u
#define MH_PIE                    0x00200000u
#define MH_DEAD_STRIPPABLE_DYLIB 0x00400000u
#define MH_HAS_TLV_DESCRIPTORS   0x00800000u
#define MH_NO_HEAP_EXECUTION     0x01000000u
#define MH_APP_EXTENSION_SAFE    0x02000000u
#define MH_NLIST_OUTOFSYNC_WITH_DYLDINFO 0x04000000u
#define MH_SIM_SUPPORT           0x08000000u
#define MH_DYLIB_IN_CACHE        0x80000000u

// ============================================================================
// CPU types
// ============================================================================

#define CPU_TYPE_X86       7
#define CPU_TYPE_X86_64    (CPU_TYPE_X86 | 0x01000000)
#define CPU_TYPE_ARM       12
#define CPU_TYPE_ARM64     (CPU_TYPE_ARM | 0x01000000)
#define CPU_TYPE_POWERPC   18
#define CPU_TYPE_POWERPC64 (CPU_TYPE_POWERPC | 0x01000000)

// CPU subtypes
#define CPU_SUBTYPE_ALL           0
#define CPU_SUBTYPE_X86_ALL       3
#define CPU_SUBTYPE_X86_64_ALL    3
#define CPU_SUBTYPE_ARM_ALL       0
#define CPU_SUBTYPE_ARM64_ALL     0
#define CPU_SUBTYPE_ARM64E        2

// ============================================================================
// Load command types
// ============================================================================

#define LC_REQ_DYLD                0x80000000u

#define LC_SEGMENT                 0x01
#define LC_SYMTAB                  0x02
#define LC_SYMSEG                  0x03
#define LC_THREAD                  0x04
#define LC_UNIXTHREAD              0x05
#define LC_DYSYMTAB                0x0B
#define LC_LOAD_DYLIB              0x0C
#define LC_ID_DYLIB                0x0D
#define LC_LOAD_DYLINKER           0x0E
#define LC_ID_DYLINKER             0x0F
#define LC_PREBOUND_DYLIB          0x10
#define LC_ROUTINES                0x11
#define LC_SUB_FRAMEWORK           0x12
#define LC_SUB_UMBRELLA            0x13
#define LC_SUB_CLIENT              0x14
#define LC_SUB_LIBRARY             0x15
#define LC_TWOLEVEL_HINTS          0x16
#define LC_PREBIND_CKSUM           0x17
#define LC_LOAD_WEAK_DYLIB         (0x18 | LC_REQ_DYLD)
#define LC_SEGMENT_64              0x19
#define LC_ROUTINES_64             0x1A
#define LC_UUID                    0x1B
#define LC_RPATH                   (0x1C | LC_REQ_DYLD)
#define LC_CODE_SIGNATURE          0x1D
#define LC_SEGMENT_SPLIT_INFO      0x1E
#define LC_REEXPORT_DYLIB          (0x1F | LC_REQ_DYLD)
#define LC_LAZY_LOAD_DYLIB         0x20
#define LC_ENCRYPTION_INFO         0x21
#define LC_DYLD_INFO               0x22
#define LC_DYLD_INFO_ONLY          (0x22 | LC_REQ_DYLD)
#define LC_LOAD_UPWARD_DYLIB       (0x23 | LC_REQ_DYLD)
#define LC_VERSION_MIN_MACOSX      0x24
#define LC_VERSION_MIN_IPHONEOS    0x25
#define LC_FUNCTION_STARTS         0x26
#define LC_DYLD_ENVIRONMENT        0x27
#define LC_MAIN                    (0x28 | LC_REQ_DYLD)
#define LC_DATA_IN_CODE            0x29
#define LC_SOURCE_VERSION          0x2A
#define LC_DYLIB_CODE_SIGN_DRS     0x2B
#define LC_ENCRYPTION_INFO_64      0x2C
#define LC_LINKER_OPTION           0x2D
#define LC_LINKER_OPTIMIZATION_HINT 0x2E
#define LC_VERSION_MIN_TVOS        0x2F
#define LC_VERSION_MIN_WATCHOS     0x30
#define LC_NOTE                    0x31
#define LC_BUILD_VERSION           0x32
#define LC_DYLD_EXPORTS_TRIE       (0x33 | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS     (0x34 | LC_REQ_DYLD)
#define LC_FILESET_ENTRY           (0x35 | LC_REQ_DYLD)

// ============================================================================
// Section types and attributes
// ============================================================================

#define S_REGULAR                    0x00
#define S_ZEROFILL                   0x01
#define S_CSTRING_LITERALS           0x02
#define S_4BYTE_LITERALS             0x03
#define S_8BYTE_LITERALS             0x04
#define S_LITERAL_POINTERS           0x05
#define S_NON_LAZY_SYMBOL_POINTERS   0x06
#define S_LAZY_SYMBOL_POINTERS       0x07
#define S_SYMBOL_STUBS               0x08
#define S_MOD_INIT_FUNC_POINTERS     0x09
#define S_MOD_TERM_FUNC_POINTERS     0x0A

#define SECTION_TYPE                 0x000000FFu
#define SECTION_ATTRIBUTES           0xFFFFFF00u
#define S_ATTR_PURE_INSTRUCTIONS     0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS     0x00000400u

// ============================================================================
// Nlist types
// ============================================================================

#define N_STAB   0xE0
#define N_PEXT   0x10
#define N_TYPE   0x0E
#define N_EXT    0x01

#define N_UNDF   0x00
#define N_ABS    0x02
#define N_SECT   0x0E
#define N_PBUD   0x0C
#define N_INDR   0x0A

// Reference types (in n_desc)
#define REFERENCE_TYPE                            0x0F
#define REFERENCE_FLAG_UNDEFINED_NON_LAZY         0
#define REFERENCE_FLAG_UNDEFINED_LAZY             1
#define REFERENCE_FLAG_DEFINED                    2
#define REFERENCE_FLAG_PRIVATE_DEFINED            3
#define REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY 4
#define REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY     5

// ============================================================================
// Relocation types
// ============================================================================

#define GENERIC_RELOC_VANILLA       0
#define GENERIC_RELOC_PAIR          1
#define GENERIC_RELOC_SECTDIFF      2
#define GENERIC_RELOC_PB_LA_PTR     3
#define GENERIC_RELOC_LOCAL_SECTDIFF 4
#define GENERIC_RELOC_TLV           5

// x86_64 relocation types
#define X86_64_RELOC_UNSIGNED       0
#define X86_64_RELOC_SIGNED         1
#define X86_64_RELOC_BRANCH         2
#define X86_64_RELOC_GOT_LOAD       3
#define X86_64_RELOC_GOT            4
#define X86_64_RELOC_SUBTRACTOR     5
#define X86_64_RELOC_SIGNED_1       6
#define X86_64_RELOC_SIGNED_2       7
#define X86_64_RELOC_SIGNED_4       8
#define X86_64_RELOC_TLV            9

// ARM64 relocation types
#define ARM64_RELOC_UNSIGNED         0
#define ARM64_RELOC_SUBTRACTOR       1
#define ARM64_RELOC_BRANCH26         2
#define ARM64_RELOC_PAGE21           3
#define ARM64_RELOC_PAGEOFF12        4
#define ARM64_RELOC_GOT_LOAD_PAGE21  5
#define ARM64_RELOC_GOT_LOAD_PAGEOFF12 6
#define ARM64_RELOC_POINTER_TO_GOT   7
#define ARM64_RELOC_TLVP_LOAD_PAGE21 8
#define ARM64_RELOC_TLVP_LOAD_PAGEOFF12 9
#define ARM64_RELOC_ADDEND           10

// ============================================================================
// Bind opcodes (for dyld info)
// ============================================================================

#define BIND_OPCODE_MASK                          0xF0
#define BIND_IMMEDIATE_MASK                       0x0F
#define BIND_OPCODE_DONE                          0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM         0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB        0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM          0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM  0x40
#define BIND_OPCODE_SET_TYPE_IMM                   0x50
#define BIND_OPCODE_SET_ADDEND_SLEB                0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB    0x70
#define BIND_OPCODE_ADD_ADDR_ULEB                  0x80
#define BIND_OPCODE_DO_BIND                        0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB          0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED    0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xC0
#define BIND_OPCODE_THREADED                       0xD0
#define BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB 0
#define BIND_SUBOPCODE_THREADED_APPLY                            1

#define BIND_TYPE_POINTER          1
#define BIND_TYPE_TEXT_ABSOLUTE32   2
#define BIND_TYPE_TEXT_PCREL32      3

#define BIND_SPECIAL_DYLIB_SELF            0
#define BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE (-1)
#define BIND_SPECIAL_DYLIB_FLAT_LOOKUP     (-2)
#define BIND_SPECIAL_DYLIB_WEAK_LOOKUP     (-3)

// ============================================================================
// Rebase opcodes
// ============================================================================

#define REBASE_OPCODE_MASK                          0xF0
#define REBASE_IMMEDIATE_MASK                       0x0F
#define REBASE_OPCODE_DONE                          0x00
#define REBASE_OPCODE_SET_TYPE_IMM                  0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB   0x20
#define REBASE_OPCODE_ADD_ADDR_ULEB                 0x30
#define REBASE_OPCODE_ADD_ADDR_IMM_SCALED           0x40
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES           0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES          0x60
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB       0x70
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80

#define REBASE_TYPE_POINTER         1
#define REBASE_TYPE_TEXT_ABSOLUTE32  2
#define REBASE_TYPE_TEXT_PCREL32     3

// ============================================================================
// Export trie flags
// ============================================================================

#define EXPORT_SYMBOL_FLAGS_KIND_MASK           0x03
#define EXPORT_SYMBOL_FLAGS_KIND_REGULAR        0x00
#define EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL   0x01
#define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE       0x02
#define EXPORT_SYMBOL_FLAGS_REEXPORT            0x08
#define EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER   0x10
#define EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION     0x04

// ============================================================================
// Platform identifiers (for LC_BUILD_VERSION)
// ============================================================================

#define PLATFORM_MACOS      1
#define PLATFORM_IOS        2
#define PLATFORM_TVOS       3
#define PLATFORM_WATCHOS    4
#define PLATFORM_BRIDGEOS   5
#define PLATFORM_MACCATALYST 6
#define PLATFORM_IOSSIMULATOR 7
#define PLATFORM_TVOSSIMULATOR 8
#define PLATFORM_WATCHOSSIMULATOR 9
#define PLATFORM_DRIVERKIT  10
#define PLATFORM_XROS       11
#define PLATFORM_XROS_SIMULATOR 12

// ============================================================================
// Code signature constants
// ============================================================================

#define CSMAGIC_REQUIREMENT            0xFADE0C00u
#define CSMAGIC_REQUIREMENTS           0xFADE0C01u
#define CSMAGIC_CODEDIRECTORY          0xFADE0C02u
#define CSMAGIC_EMBEDDED_SIGNATURE     0xFADE0CC0u
#define CSMAGIC_EMBEDDED_SIGNATURE_OLD 0xFADE0B02u
#define CSMAGIC_DETACHED_SIGNATURE     0xFADE0CC1u
#define CSMAGIC_BLOBWRAPPER            0xFADE0B01u
#define CSMAGIC_EMBEDDED_ENTITLEMENTS  0xFADE7171u
#define CSMAGIC_EMBEDDED_DER_ENTITLEMENTS 0xFADE7172u

#define CSSLOT_CODEDIRECTORY           0u
#define CSSLOT_INFOSLOT                1u
#define CSSLOT_REQUIREMENTS            2u
#define CSSLOT_RESOURCEDIR             3u
#define CSSLOT_APPLICATION             4u
#define CSSLOT_ENTITLEMENTS            5u
#define CSSLOT_DER_ENTITLEMENTS        7u
#define CSSLOT_ALTERNATE_CODEDIRECTORIES 0x1000u
#define CSSLOT_SIGNATURESLOT           0x10000u

#define CS_HASHTYPE_SHA1               1u
#define CS_HASHTYPE_SHA256             2u
#define CS_HASHTYPE_SHA256_TRUNCATED   3u
#define CS_HASHTYPE_SHA384             4u

// ============================================================================
// Chained fixup constants
// ============================================================================

#define DYLD_CHAINED_PTR_ARM64E             1
#define DYLD_CHAINED_PTR_64                 2
#define DYLD_CHAINED_PTR_32                 3
#define DYLD_CHAINED_PTR_32_CACHE           4
#define DYLD_CHAINED_PTR_32_FIRMWARE        5
#define DYLD_CHAINED_PTR_64_OFFSET          6
#define DYLD_CHAINED_PTR_ARM64E_KERNEL      7
#define DYLD_CHAINED_PTR_64_KERNEL_CACHE    8
#define DYLD_CHAINED_PTR_ARM64E_USERLAND    12
#define DYLD_CHAINED_PTR_ARM64E_FIRMWARE    13
#define DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE 14
#define DYLD_CHAINED_PTR_ARM64E_USERLAND24  15

#define DYLD_CHAINED_IMPORT                 1
#define DYLD_CHAINED_IMPORT_ADDEND          2
#define DYLD_CHAINED_IMPORT_ADDEND64        3

// ============================================================================
// Chained fixup on-disk structures
// ============================================================================

/// Header at the start of the chained fixups data in __LINKEDIT.
typedef struct dyld_chained_fixups_header {
    uint32_t fixups_version;   ///< 0
    uint32_t starts_offset;    ///< Offset to dyld_chained_starts_in_image.
    uint32_t imports_offset;   ///< Offset to imports table.
    uint32_t symbols_offset;   ///< Offset to symbol strings.
    uint32_t imports_count;
    uint32_t imports_format;   ///< DYLD_CHAINED_IMPORT*
    uint32_t symbols_format;   ///< 0 = uncompressed, 1 = zlib compressed
} dyld_chained_fixups_header_t;

/// Per-segment starts descriptor.
typedef struct dyld_chained_starts_in_segment {
    uint32_t size;
    uint16_t page_size;        ///< 0x1000 or 0x4000
    uint16_t pointer_format;   ///< DYLD_CHAINED_PTR_*
    uint64_t segment_offset;   ///< Offset from mach_header to segment start.
    uint32_t max_valid_pointer; ///< For 32-bit only.
    uint16_t page_count;
    uint16_t page_start[];     ///< Per-page chain start offsets; 0xFFFF = no fixups.
} dyld_chained_starts_in_segment_t;

#define DYLD_CHAINED_PTR_START_NONE  0xFFFF
#define DYLD_CHAINED_PTR_START_MULTI 0x8000
#define DYLD_CHAINED_PTR_START_LAST  0x8000

/// Compact import entry (DYLD_CHAINED_IMPORT).
typedef struct dyld_chained_import {
    uint32_t lib_ordinal :  8;
    uint32_t weak_import :  1;
    uint32_t name_offset : 23;
} dyld_chained_import_t;

/// Import with 32-bit addend (DYLD_CHAINED_IMPORT_ADDEND).
typedef struct dyld_chained_import_addend {
    uint32_t lib_ordinal :  8;
    uint32_t weak_import :  1;
    uint32_t name_offset : 23;
    int32_t  addend;
} dyld_chained_import_addend_t;

/// Import with 64-bit addend (DYLD_CHAINED_IMPORT_ADDEND64).
typedef struct dyld_chained_import_addend64 {
    uint64_t lib_ordinal : 16;
    uint64_t weak_import :  1;
    uint64_t reserved    : 15;
    uint64_t name_offset : 32;
    uint64_t addend;
} dyld_chained_import_addend64_t;

// ============================================================================
// Data-in-code entry kinds
// ============================================================================

#define DICE_KIND_DATA              0x0001
#define DICE_KIND_JUMP_TABLE8       0x0002
#define DICE_KIND_JUMP_TABLE16      0x0003
#define DICE_KIND_JUMP_TABLE32      0x0004
#define DICE_KIND_ABS_JUMP_TABLE32  0x0005

// ============================================================================
// Build tool constants (for LC_BUILD_VERSION)
// ============================================================================

#define TOOL_CLANG   1
#define TOOL_SWIFT   2
#define TOOL_LD      3
#define TOOL_LLD     4
#define TOOL_METAL   5
#define TOOL_AIRLLD  6
#define TOOL_AIRNT   7
#define TOOL_AIRPACK 8

// ============================================================================
// Raw on-disk Mach-O 64-bit structures
// ============================================================================

#pragma pack(push, 1)

/// Mach-O 64-bit header.
typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} n00b_macho_header64_t;

/// Generic load command header.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} n00b_macho_load_command_t;

/// 64-bit segment command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} n00b_macho_segment_command64_t;

/// 64-bit section.
typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} n00b_macho_section64_t;

/// Fat binary header.
typedef struct {
    uint32_t magic;
    uint32_t nfat_arch;
} n00b_macho_fat_header_t;

/// Fat architecture descriptor.
typedef struct {
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t offset;
    uint32_t size;
    uint32_t align;
} n00b_macho_fat_arch_t;

/// Symbol table command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} n00b_macho_symtab_command_t;

/// Dynamic symbol table command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} n00b_macho_dysymtab_command_t;

/// 64-bit symbol table entry (nlist_64).
typedef struct {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} n00b_macho_nlist64_t;

/// Dylib command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t name_offset;
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
} n00b_macho_dylib_command_t;

/// Dylinker command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t name_offset;
} n00b_macho_dylinker_command_t;

/// UUID command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint8_t  uuid[16];
} n00b_macho_uuid_command_t;

/// Entry point command (LC_MAIN).
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} n00b_macho_entry_point_command_t;

/// Dyld info command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    uint32_t export_off;
    uint32_t export_size;
} n00b_macho_dyld_info_command_t;

/// Linkedit data command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
} n00b_macho_linkedit_data_command_t;

/// Source version command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t version;
} n00b_macho_source_version_command_t;

/// Build version command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t platform;
    uint32_t minos;
    uint32_t sdk;
    uint32_t ntools;
} n00b_macho_build_version_command_t;

/// Build tool version.
typedef struct {
    uint32_t tool;
    uint32_t version;
} n00b_macho_build_tool_version_t;

/// Version min command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t version;
    uint32_t sdk;
} n00b_macho_version_min_command_t;

/// Encryption info 64 command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t cryptoff;
    uint32_t cryptsize;
    uint32_t cryptid;
    uint32_t pad;
} n00b_macho_encryption_info_command64_t;

/// Rpath command.
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t path_offset;
} n00b_macho_rpath_command_t;

/// Thread command (LC_THREAD, LC_UNIXTHREAD).
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    // Followed by flavor + count + state data (variable length).
} n00b_macho_thread_command_t;

/// Relocation info.
typedef struct {
    int32_t  r_address;
    uint32_t r_symbolnum_etc;  // packed bitfield
} n00b_macho_relocation_info_t;

#pragma pack(pop)
