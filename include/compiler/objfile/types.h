/**
 * @file n00b_types.h
 * @brief Error codes, format/architecture enums, and forward declarations.
 *
 * This header provides the foundational type definitions shared across all
 * LIEF modules.  It is self-contained — no n00b headers required.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Error codes
// ============================================================================

/**
 * @brief Domain-specific error codes for LIEF operations.
 *
 * Negative values to avoid collision with errno.  Used as the `err` payload
 * inside `n00b_result_t`.
 */
typedef enum {
    N00B_OK               =  0,
    N00B_ERR_READ         = -100,
    N00B_ERR_NOT_FOUND    = -101,
    N00B_ERR_CORRUPTED    = -102,
    N00B_ERR_PARSE        = -103,
    N00B_ERR_BUILD        = -104,
    N00B_ERR_NOT_SUPPORTED = -105,
    N00B_ERR_OUT_OF_BOUNDS = -106,
} n00b_error_t;

// ============================================================================
// Binary format
// ============================================================================

typedef enum {
    N00B_FMT_UNKNOWN = 0,
    N00B_FMT_ELF,
    N00B_FMT_MACHO,
    N00B_FMT_PE,
} n00b_format_t;

// ============================================================================
// Endianness
// ============================================================================

typedef enum {
    N00B_ENDIAN_LITTLE = 0,
    N00B_ENDIAN_BIG    = 1,
} n00b_endian_t;

// ============================================================================
// Architecture
// ============================================================================

typedef enum {
    N00B_ARCH_UNKNOWN = 0,
    N00B_ARCH_X86,
    N00B_ARCH_X86_64,
    N00B_ARCH_ARM,
    N00B_ARCH_ARM64,
    N00B_ARCH_PPC,
    N00B_ARCH_PPC64,
    N00B_ARCH_MIPS,
    N00B_ARCH_MIPS64,
    N00B_ARCH_RISCV32,
    N00B_ARCH_RISCV64,
    N00B_ARCH_SPARC,
    N00B_ARCH_SPARC64,
    N00B_ARCH_S390X,
    N00B_ARCH_LOONGARCH64,
} n00b_arch_t;

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct n00b_stream  n00b_bstream_t;
typedef struct n00b_binary  n00b_binary_t;
