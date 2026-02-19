/**
 * @file ncc_limits.h
 * @brief Named constants for buffer sizes and initial capacities.
 *
 * Centralises magic numbers used across the bootstrap compiler.
 * Fixed-size buffers should always be paired with overflow checks
 * via #NCC_CHECK_SNPRINTF.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

// ====================================================================
// Fixed buffer sizes
// ====================================================================

/** Generated identifier names (struct tags, lock vars, etc.). */
#define NCC_IDENT_BUF       256

/** Integer-to-string formatting (decimal int/long long). */
#define NCC_INTSTR_BUF      32

/** Pipe I/O read chunk. */
#define NCC_IO_CHUNK        8192

/** Line marker for `# 1 "file"` directives. */
#define NCC_LINE_MARKER_BUF 4096

/** Preprocessor directive construction. */
#define NCC_DIRECTIVE_BUF   1024

/** Deprecated-attribute / annotation buffers. */
#define NCC_ATTR_BUF        512

/** Crash handler backtrace depth. */
#define NCC_BACKTRACE_DEPTH 200

// ====================================================================
// Growable array initial capacities
// ====================================================================

/** Small collections (function args, constexpr args). */
#define NCC_CAP_SMALL       16

/** General tree-traversal stacks. */
#define NCC_CAP_MEDIUM      32

/** Deeper traversals (type normalization, tree copy). */
#define NCC_CAP_LARGE       64

/** Emit buffer, print stack in compile_file. */
#define NCC_CAP_XLARGE      128

// ====================================================================
// Overflow checking
// ====================================================================

/**
 * @brief Abort if an snprintf was truncated.
 *
 * Call immediately after `snprintf()` for any fixed-size buffer whose
 * truncation would produce silently wrong output (e.g. generated
 * identifiers).  I/O and diagnostic buffers where truncation is
 * harmless do not need this check.
 *
 * @param ret   Return value from snprintf
 * @param buf   The buffer (must be an array, not a pointer)
 */
#define NCC_CHECK_SNPRINTF(ret, buf)                                     \
    do {                                                                 \
        if ((ret) < 0 || (ret) >= (int)sizeof(buf)) {                   \
            fprintf(stderr,                                              \
                    "ncc: internal error: identifier buffer overflow "   \
                    "(need %d, have %zu)\n",                             \
                    (ret), sizeof(buf));                                 \
            abort();                                                     \
        }                                                                \
    } while (0)
