#pragma once
/** @file query.h
 *  @brief Composable, type-safe character queries.
 *
 *  Provides filter constructors and query functions for searching the
 *  Unicode codepoint space by property combinations, script, category,
 *  range, and name.
 *
 *  ### Usage
 *
 *  ```c
 *  // All Latin uppercase letters:
 *  n00b_cp_filter_t filters[] = {
 *      n00b_filter_gc(N00B_UNICODE_GC_LU),
 *      n00b_filter_script(N00B_UNICODE_SCRIPT_LATIN),
 *  };
 *  uint32_t count;
 *  auto *results = n00b_cp_query_n(filters, 2, .out_count = &count);
 *  ```
 *
 *  ### Convenience macros
 *
 *  ```c
 *  // Builds a compound literal filter array and calls n00b_cp_query_n:
 *  auto *results = n00b_cp_query(
 *      n00b_filter_gc(N00B_UNICODE_GC_LU),
 *      n00b_filter_script(N00B_UNICODE_SCRIPT_LATIN));
 *  ```
 *
 *  ### Related modules
 *
 *  - `unicode/properties.h` -- underlying property lookup functions
 *  - `unicode/encoding.h` -- UTF-8 encoding used by name search
 */

#include "unicode/types_ext.h"
#include "unicode/properties.h"

// ===========================================================================
// Filter objects
// ===========================================================================

/** @brief Predicate function type for codepoint filtering. */
typedef bool (*n00b_cp_predicate_fn)(n00b_codepoint_t cp, void *ctx);

/** @brief A composable codepoint filter (predicate + context). */
typedef struct n00b_cp_filter_t {
    n00b_cp_predicate_fn predicate; /**< Filter predicate function */
    void                *ctx;       /**< Opaque context for the predicate */
} n00b_cp_filter_t;

// ===========================================================================
// Filter constructors
// ===========================================================================

/** @brief Filter by General_Category. */
n00b_cp_filter_t n00b_filter_gc(n00b_unicode_gc_t gc);

/** @brief Filter by Script. */
n00b_cp_filter_t n00b_filter_script(n00b_unicode_script_t script);

/** @brief Filter by Bidi_Class. */
n00b_cp_filter_t n00b_filter_bidi(n00b_unicode_bidi_class_t bidi);

/** @brief Filter by binary property. */
n00b_cp_filter_t n00b_filter_property(n00b_unicode_property_t prop);

/** @brief Filter by codepoint range [lo, hi]. */
n00b_cp_filter_t n00b_filter_range(n00b_codepoint_t lo, n00b_codepoint_t hi);

/** @brief Filter by Block. */
n00b_cp_filter_t n00b_filter_block(n00b_unicode_block_t block);

/** @brief Filter by East_Asian_Width. */
n00b_cp_filter_t n00b_filter_eaw(n00b_unicode_eaw_t eaw);

// ===========================================================================
// Query functions
// ===========================================================================

/** @brief AND query: returns codepoints matching ALL given filters.
 *
 *  @param filters  Array of filters (all must match).
 *  @param nfilters Number of filters.
 *
 *  @kw range_start  Start of codepoint range to search (default 0).
 *  @kw range_end    End of codepoint range to search (default 0x10FFFF).
 *  @kw max_results  Maximum results (0 = unlimited).
 *  @kw out_count    Pointer to receive result count.
 *  @kw allocator    Allocator for results.
 *
 *  @return Array of matching codepoints (caller frees).
 */
n00b_codepoint_t *
n00b_cp_query_n(const n00b_cp_filter_t *filters, int nfilters) _kargs
{
    n00b_codepoint_t  range_start = 0;
    n00b_codepoint_t  range_end   = 0x10FFFF;
    size_t            max_results = 0;
    uint32_t         *out_count   = nullptr;
    n00b_allocator_t *allocator   = nullptr;
};

/** @brief OR query: returns codepoints matching ANY given filter.
 *
 *  @param filters  Array of filters (any may match).
 *  @param nfilters Number of filters.
 *
 *  @kw range_start  Start of codepoint range to search (default 0).
 *  @kw range_end    End of codepoint range to search (default 0x10FFFF).
 *  @kw max_results  Maximum results (0 = unlimited).
 *  @kw out_count    Pointer to receive result count.
 *  @kw allocator    Allocator for results.
 *
 *  @return Array of matching codepoints (caller frees).
 */
n00b_codepoint_t *
n00b_cp_query_any_n(const n00b_cp_filter_t *filters, int nfilters) _kargs
{
    n00b_codepoint_t  range_start = 0;
    n00b_codepoint_t  range_end   = 0x10FFFF;
    size_t            max_results = 0;
    uint32_t         *out_count   = nullptr;
    n00b_allocator_t *allocator   = nullptr;
};

// ===========================================================================
// Convenience macros (compound literal filter arrays)
// ===========================================================================

/** @brief AND query with inline filter arguments.
 *  @code
 *  n00b_cp_query(n00b_filter_gc(N00B_UNICODE_GC_LU),
 *                n00b_filter_script(N00B_UNICODE_SCRIPT_LATIN));
 *  @endcode
 */
#define n00b_cp_query(...)                                        \
    n00b_cp_query_n(                                              \
        (const n00b_cp_filter_t[]){__VA_ARGS__},                  \
        (int)(sizeof((const n00b_cp_filter_t[]){__VA_ARGS__})     \
              / sizeof(n00b_cp_filter_t)))

/** @brief OR query with inline filter arguments. */
#define n00b_cp_query_any(...)                                    \
    n00b_cp_query_any_n(                                          \
        (const n00b_cp_filter_t[]){__VA_ARGS__},                  \
        (int)(sizeof((const n00b_cp_filter_t[]){__VA_ARGS__})     \
              / sizeof(n00b_cp_filter_t)))

// ===========================================================================
// Name lookup
// ===========================================================================

/** @brief Look up a codepoint's Unicode name.
 *  @param cp  The codepoint.
 *  @return    Static string with the name, or NULL if unknown.
 */
const char *n00b_unicode_cp_name(n00b_codepoint_t cp);

/** @brief Look up a codepoint by exact name (case-insensitive, ignoring
 *         medial hyphens and spaces per Unicode name matching rules).
 *  @param name  The character name to look up.
 *  @return      Option containing the codepoint, or none if not found.
 */
n00b_option_t(n00b_codepoint_t)
n00b_unicode_cp_from_name(const char *name);
