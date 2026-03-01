/**
 * @file n00b_pe_types.h
 * @brief Raw on-disk PE/COFF structures and constants.
 *
 * All structs are packed to match the on-disk layout.  Only PE32+
 * (64-bit) is supported.  Self-contained — no n00b headers required.
 */
#pragma once

#include <stdint.h>

// ============================================================================
// Magic / Signature
// ============================================================================

#define N00B_PE_MAGIC_MZ         0x5A4D      ///< DOS "MZ" magic
#define N00B_PE_SIGNATURE        0x00004550  ///< "PE\0\0"
#define N00B_PE_OPT_MAGIC_PE32P  0x020B      ///< PE32+ (64-bit)
#define N00B_PE_OPT_MAGIC_PE32   0x010B      ///< PE32 (32-bit, not supported)

// ============================================================================
// Machine types
// ============================================================================

#define N00B_PE_MACHINE_UNKNOWN  0x0000
#define N00B_PE_MACHINE_I386     0x014C
#define N00B_PE_MACHINE_ARM      0x01C0
#define N00B_PE_MACHINE_ARMNT    0x01C4
#define N00B_PE_MACHINE_AMD64    0x8664
#define N00B_PE_MACHINE_ARM64    0xAA64
#define N00B_PE_MACHINE_RISCV32  0x5032
#define N00B_PE_MACHINE_RISCV64  0x5064

// ============================================================================
// File header characteristics
// ============================================================================

#define N00B_PE_CHAR_RELOCS_STRIPPED    0x0001
#define N00B_PE_CHAR_EXECUTABLE_IMAGE  0x0002
#define N00B_PE_CHAR_LINE_NUMS_STRIPPED 0x0004
#define N00B_PE_CHAR_LARGE_ADDRESS     0x0020
#define N00B_PE_CHAR_32BIT_MACHINE     0x0100
#define N00B_PE_CHAR_DEBUG_STRIPPED     0x0200
#define N00B_PE_CHAR_SYSTEM            0x1000
#define N00B_PE_CHAR_DLL               0x2000

// ============================================================================
// DLL characteristics
// ============================================================================

#define N00B_PE_DLLCHAR_HIGH_ENTROPY_VA  0x0020
#define N00B_PE_DLLCHAR_DYNAMIC_BASE     0x0040
#define N00B_PE_DLLCHAR_FORCE_INTEGRITY  0x0080
#define N00B_PE_DLLCHAR_NX_COMPAT       0x0100
#define N00B_PE_DLLCHAR_NO_ISOLATION    0x0200
#define N00B_PE_DLLCHAR_NO_SEH          0x0400
#define N00B_PE_DLLCHAR_NO_BIND         0x0800
#define N00B_PE_DLLCHAR_APPCONTAINER    0x1000
#define N00B_PE_DLLCHAR_WDM_DRIVER      0x2000
#define N00B_PE_DLLCHAR_GUARD_CF        0x4000
#define N00B_PE_DLLCHAR_TERMINAL_SERVER  0x8000

// ============================================================================
// Subsystem
// ============================================================================

#define N00B_PE_SUBSYSTEM_UNKNOWN          0
#define N00B_PE_SUBSYSTEM_NATIVE           1
#define N00B_PE_SUBSYSTEM_WINDOWS_GUI      2
#define N00B_PE_SUBSYSTEM_WINDOWS_CUI      3
#define N00B_PE_SUBSYSTEM_OS2_CUI          5
#define N00B_PE_SUBSYSTEM_POSIX_CUI        7
#define N00B_PE_SUBSYSTEM_EFI_APPLICATION  10
#define N00B_PE_SUBSYSTEM_EFI_BOOT_DRIVER  11
#define N00B_PE_SUBSYSTEM_EFI_RUNTIME      12
#define N00B_PE_SUBSYSTEM_EFI_ROM          13
#define N00B_PE_SUBSYSTEM_XBOX             14

// ============================================================================
// Section characteristics
// ============================================================================

#define N00B_PE_SCN_CNT_CODE          0x00000020
#define N00B_PE_SCN_CNT_INITIALIZED   0x00000040
#define N00B_PE_SCN_CNT_UNINITIALIZED 0x00000080
#define N00B_PE_SCN_LNK_INFO         0x00000200
#define N00B_PE_SCN_LNK_REMOVE       0x00000800
#define N00B_PE_SCN_LNK_COMDAT       0x00001000
#define N00B_PE_SCN_ALIGN_1BYTES     0x00100000
#define N00B_PE_SCN_ALIGN_16BYTES    0x00500000
#define N00B_PE_SCN_LNK_NRELOC_OVFL  0x01000000
#define N00B_PE_SCN_MEM_DISCARDABLE  0x02000000
#define N00B_PE_SCN_MEM_NOT_CACHED   0x04000000
#define N00B_PE_SCN_MEM_NOT_PAGED    0x08000000
#define N00B_PE_SCN_MEM_SHARED       0x10000000
#define N00B_PE_SCN_MEM_EXECUTE      0x20000000
#define N00B_PE_SCN_MEM_READ         0x40000000
#define N00B_PE_SCN_MEM_WRITE        0x80000000

// ============================================================================
// Data directory indices
// ============================================================================

#define N00B_PE_DD_EXPORT        0
#define N00B_PE_DD_IMPORT        1
#define N00B_PE_DD_RESOURCE      2
#define N00B_PE_DD_EXCEPTION     3
#define N00B_PE_DD_CERTIFICATE   4
#define N00B_PE_DD_BASERELOC     5
#define N00B_PE_DD_DEBUG         6
#define N00B_PE_DD_ARCHITECTURE  7
#define N00B_PE_DD_GLOBALPTR     8
#define N00B_PE_DD_TLS           9
#define N00B_PE_DD_LOAD_CONFIG   10
#define N00B_PE_DD_BOUND_IMPORT  11
#define N00B_PE_DD_IAT           12
#define N00B_PE_DD_DELAY_IMPORT  13
#define N00B_PE_DD_CLR_RUNTIME   14
#define N00B_PE_DD_RESERVED      15
#define N00B_PE_NUM_DATA_DIRS    16

// ============================================================================
// Import constants
// ============================================================================

#define N00B_PE_IMPORT_ORDINAL_FLAG64  0x8000000000000000ULL

// ============================================================================
// Relocation types
// ============================================================================

#define N00B_PE_REL_BASED_ABSOLUTE      0
#define N00B_PE_REL_BASED_HIGH          1
#define N00B_PE_REL_BASED_LOW           2
#define N00B_PE_REL_BASED_HIGHLOW       3
#define N00B_PE_REL_BASED_HIGHADJ       4
#define N00B_PE_REL_BASED_ARM_MOV32     5
#define N00B_PE_REL_BASED_THUMB_MOV32   7
#define N00B_PE_REL_BASED_RISCV_HIGH20  5   // Same value as ARM_MOV32 (arch-dependent)
#define N00B_PE_REL_BASED_RISCV_LOW12I  7   // Same value as THUMB_MOV32
#define N00B_PE_REL_BASED_RISCV_LOW12S  8
#define N00B_PE_REL_BASED_DIR64         10

// ============================================================================
// Debug types
// ============================================================================

#define N00B_PE_DEBUG_TYPE_UNKNOWN    0
#define N00B_PE_DEBUG_TYPE_COFF       1
#define N00B_PE_DEBUG_TYPE_CODEVIEW   2
#define N00B_PE_DEBUG_TYPE_FPO        3
#define N00B_PE_DEBUG_TYPE_MISC       4
#define N00B_PE_DEBUG_TYPE_EXCEPTION  5
#define N00B_PE_DEBUG_TYPE_FIXUP      6
#define N00B_PE_DEBUG_TYPE_VC_FEATURE  12
#define N00B_PE_DEBUG_TYPE_POGO       13
#define N00B_PE_DEBUG_TYPE_REPRO      16
#define N00B_PE_DEBUG_TYPE_PDBCHECKSUM 19
#define N00B_PE_DEBUG_TYPE_EX_DLLCHAR  20

#define N00B_PE_CV_SIGNATURE_RSDS    0x53445352  ///< "RSDS"

// ============================================================================
// Resource type IDs
// ============================================================================

#define N00B_PE_RT_CURSOR        1
#define N00B_PE_RT_BITMAP        2
#define N00B_PE_RT_ICON          3
#define N00B_PE_RT_MENU          4
#define N00B_PE_RT_DIALOG        5
#define N00B_PE_RT_STRING        6
#define N00B_PE_RT_FONTDIR       7
#define N00B_PE_RT_FONT          8
#define N00B_PE_RT_ACCELERATOR   9
#define N00B_PE_RT_RCDATA        10
#define N00B_PE_RT_MESSAGETABLE  11
#define N00B_PE_RT_GROUP_CURSOR  12
#define N00B_PE_RT_GROUP_ICON    14
#define N00B_PE_RT_VERSION       16
#define N00B_PE_RT_DLGINCLUDE    17
#define N00B_PE_RT_PLUGPLAY      19
#define N00B_PE_RT_VXD           20
#define N00B_PE_RT_ANICURSOR     21
#define N00B_PE_RT_ANIICON       22
#define N00B_PE_RT_HTML          23
#define N00B_PE_RT_MANIFEST      24

// ============================================================================
// Raw on-disk structures
// ============================================================================

#pragma pack(push, 1)

/// DOS header (64 bytes).
typedef struct {
    uint16_t e_magic;       ///< 0x5A4D ("MZ")
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;      ///< Offset to PE signature
} n00b_pe_dos_header_t;

/// COFF file header (20 bytes).
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} n00b_pe_file_header_t;

/// Data directory entry (8 bytes).
typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} n00b_pe_data_directory_t;

/// PE32+ optional header (240 bytes with 16 data directories).
typedef struct {
    uint16_t Magic;                     ///< 0x020B for PE32+
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    n00b_pe_data_directory_t DataDirectory[N00B_PE_NUM_DATA_DIRS];
} n00b_pe_optional_header64_t;

/// Section header (40 bytes).
typedef struct {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} n00b_pe_section_header_t;

/// Import descriptor (20 bytes).
typedef struct {
    uint32_t OriginalFirstThunk;  ///< RVA to ILT (Import Lookup Table)
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;                ///< RVA to DLL name
    uint32_t FirstThunk;          ///< RVA to IAT (Import Address Table)
} n00b_pe_import_descriptor_t;

/// Export directory (40 bytes).
typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;                  ///< RVA to module name
    uint32_t Base;                  ///< Ordinal base
    uint32_t NumberOfFunctions;
    uint32_t NumberOfNames;
    uint32_t AddressOfFunctions;    ///< RVA to function address table
    uint32_t AddressOfNames;        ///< RVA to name pointer table
    uint32_t AddressOfNameOrdinals; ///< RVA to ordinal table
} n00b_pe_export_directory_t;

/// Base relocation block header (8 bytes).
typedef struct {
    uint32_t PageRVA;
    uint32_t BlockSize;
} n00b_pe_base_reloc_block_t;

/// Debug directory entry (28 bytes).
typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Type;
    uint32_t SizeOfData;
    uint32_t AddressOfRawData;
    uint32_t PointerToRawData;
} n00b_pe_debug_directory_t;

/// TLS directory (PE32+, 40 bytes).
typedef struct {
    uint64_t RawDataStartVA;
    uint64_t RawDataEndVA;
    uint64_t AddressOfIndex;
    uint64_t AddressOfCallBacks;
    uint32_t SizeOfZeroFill;
    uint32_t Characteristics;
} n00b_pe_tls_directory64_t;

/// Resource directory header (16 bytes).
typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
} n00b_pe_resource_directory_t;

/// Resource directory entry (8 bytes).
typedef struct {
    uint32_t NameOrId;
    uint32_t OffsetToData;
} n00b_pe_resource_dir_entry_t;

/// Resource data entry (16 bytes).
typedef struct {
    uint32_t OffsetToData;  ///< RVA to data
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
} n00b_pe_resource_data_entry_t;

/// Delay import descriptor (32 bytes).
typedef struct {
    uint32_t Attributes;
    uint32_t Name;                ///< RVA to DLL name
    uint32_t ModuleHandle;        ///< RVA to module handle
    uint32_t DelayImportAddressTable;  ///< RVA to delay IAT
    uint32_t DelayImportNameTable;     ///< RVA to delay INT
    uint32_t BoundDelayImportTable;
    uint32_t UnloadDelayImportTable;
    uint32_t TimeDateStamp;
} n00b_pe_delay_import_descriptor_t;

/// Exception table entry (x64 RUNTIME_FUNCTION, 12 bytes).
typedef struct {
    uint32_t BeginAddress;
    uint32_t EndAddress;
    uint32_t UnwindInfoAddress;
} n00b_pe_runtime_function_t;

/// Bound import descriptor (8 bytes).
typedef struct {
    uint32_t TimeDateStamp;
    uint16_t OffsetModuleName;   ///< Offset from start of bound import table
    uint16_t NumberOfModuleForwarderRefs;
} n00b_pe_bound_import_descriptor_t;

#pragma pack(pop)

// ============================================================================
// Size constants
// ============================================================================

// ============================================================================
// Rich header
// ============================================================================

#define N00B_PE_RICH_MAGIC  0x68636952   ///< "Rich" (LE)
#define N00B_PE_DANS_MAGIC  0x536E6144   ///< "DanS" (LE)

typedef struct {
    uint16_t build_id;
    uint16_t tool_id;
    uint32_t count;
} n00b_pe_rich_entry_t;

// ============================================================================
// Size constants
// ============================================================================

// ============================================================================
// COFF symbol storage class constants
// ============================================================================

#define N00B_PE_SYM_CLASS_EXTERNAL    2
#define N00B_PE_SYM_CLASS_STATIC      3
#define N00B_PE_SYM_CLASS_FUNCTION    101
#define N00B_PE_SYM_CLASS_FILE        103

#define N00B_PE_COFF_SYMBOL_SIZE         18
#define N00B_PE_DELAY_IMPORT_DESC_SIZE   32
#define N00B_PE_RUNTIME_FUNCTION_SIZE    12
#define N00B_PE_BOUND_IMPORT_DESC_SIZE   8

#define N00B_PE_DOS_HEADER_SIZE          64
#define N00B_PE_FILE_HEADER_SIZE         20
#define N00B_PE_OPTIONAL_HEADER64_SIZE   240
#define N00B_PE_SECTION_HEADER_SIZE      40
#define N00B_PE_IMPORT_DESCRIPTOR_SIZE   20
#define N00B_PE_DATA_DIRECTORY_SIZE      8
