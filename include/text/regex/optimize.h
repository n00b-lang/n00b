/**
 * @file optimize.h
 * @brief Regex match-time optimization heuristics (resharp Optimizations.fs port).
 *
 * Analyzes compiled regex ASTs to select fast-path strategies:
 * - **Prefix acceleration**: skip ahead in input to first possible match start
 * - **Fixed-length detection**: bypass reverse DFA when match length is known
 * - **Match override**: replace regex with literal string comparison
 * - **Potential match start**: compute character sets that can begin a match
 *
 * These optimizations are computed once during `n00b_regex_new()` and stored
 * on the `n00b_regex_t` handle for use by the matching engine.
 */
#pragma once

#include "n00b.h"
#include "text/regex/charset.h"
#include "text/regex/node.h"

// ============================================================================
// Initial accelerator — how to skip to the first possible match start
// ============================================================================

typedef enum {
    N00B_RE_ACCEL_NONE,              /**< No acceleration available. */
    N00B_RE_ACCEL_FIXED_PREFIX,      /**< Literal codepoint prefix. */
    N00B_RE_ACCEL_FIXED_PREFIX_CI,   /**< Case-insensitive literal prefix. */
    N00B_RE_ACCEL_MINTERM_PREFIX,    /**< Sequence of minterm sets. */
    N00B_RE_ACCEL_SINGLE_MINTERM,    /**< Single rare minterm at start. */
    N00B_RE_ACCEL_POTENTIAL_START,   /**< Speculative multi-set start. */
} n00b_regex_accel_kind_t;

/**
 * @brief Pre-computed prefix acceleration for match startup.
 *
 * Encodes the longest deterministic prefix of the regex — a sequence
 * of character positions where only one derivative successor exists.
 * The matching engine can skip ahead until the prefix is found.
 */
typedef struct n00b_regex_accelerator_t {
    n00b_regex_accel_kind_t kind;
    bool                    needs_reverse_start; /**< Prefix was stripped from a nullable head — use reverse DFA to find true match start. */

    union {
        /** N00B_RE_ACCEL_FIXED_PREFIX / N00B_RE_ACCEL_FIXED_PREFIX_CI */
        struct {
            n00b_codepoint_t *codepoints;
            uint32_t          len;
            uint32_t          transition_state; /**< DFA state after prefix. */
        } fixed_prefix;

        /** N00B_RE_ACCEL_MINTERM_PREFIX: sequence of minterm charsets. */
        struct {
            n00b_regex_charset_t *minterms;
            uint32_t              len;
            uint32_t              transition_state;
            uint8_t               first_ascii_map[128]; /**< Pre-computed ASCII bitmap for first minterm. */
        } minterm_prefix;

        /** N00B_RE_ACCEL_SINGLE_MINTERM: one charset to scan for. */
        struct {
            n00b_regex_charset_t minterm;
            uint32_t             transition_state; /**< 0 = no transition. */
            uint8_t              ascii_map[128];   /**< Pre-computed ASCII membership bitmap. */
        } single_minterm;

        /** N00B_RE_ACCEL_POTENTIAL_START: multi-set potential starts. */
        struct {
            n00b_regex_charset_t *sets;
            uint32_t              len;
            uint8_t               ascii_map[128]; /**< Pre-computed merged ASCII membership bitmap. */
        } potential_start;
    };
} n00b_regex_accelerator_t;

// ============================================================================
// Length lookup — how to detect match end efficiently
// ============================================================================

typedef enum {
    N00B_RE_LEN_MATCH_END,              /**< Default: full reverse DFA scan. */
    N00B_RE_LEN_FIXED,                  /**< Match has constant length. */
    N00B_RE_LEN_FIXED_PREFIX_MATCH_END, /**< Skip initial transitions. */
    N00B_RE_LEN_SET_LOOKUP,             /**< Single-char lookup after prefix. */
    N00B_RE_LEN_REMAINING_SETS,         /**< Bounded loop after prefix. */
} n00b_regex_len_kind_t;

/**
 * @brief Pre-computed match-end detection strategy.
 *
 * Avoids expensive reverse DFA traversal when the match length
 * is predictable from the regex structure.
 */
typedef struct n00b_regex_len_lookup_t {
    n00b_regex_len_kind_t kind;

    union {
        /** N00B_RE_LEN_FIXED */
        struct {
            int32_t length;
        } fixed;

        /** N00B_RE_LEN_FIXED_PREFIX_MATCH_END */
        struct {
            int32_t  prefix_length;
            uint32_t transition_state;
        } prefix_match_end;

        /** N00B_RE_LEN_SET_LOOKUP */
        struct {
            int32_t              prefix_length;
            n00b_regex_charset_t minterm;
        } set_lookup;

        /** N00B_RE_LEN_REMAINING_SETS */
        struct {
            int32_t              prefix_length;
            n00b_regex_charset_t minterm;
            uint8_t              max_remaining;
        } remaining;
    };
} n00b_regex_len_lookup_t;

// ============================================================================
// Match override — replace regex with faster operation
// ============================================================================

typedef enum {
    N00B_RE_OVERRIDE_NONE,          /**< No override — use full engine. */
    N00B_RE_OVERRIDE_FIXED_STRING,  /**< Replace with literal string search. */
} n00b_regex_override_kind_t;

typedef struct n00b_regex_override_t {
    n00b_regex_override_kind_t kind;

    union {
        /** N00B_RE_OVERRIDE_FIXED_STRING */
        struct {
            n00b_codepoint_t *codepoints;
            uint32_t          len;
        } fixed_string;
    };
} n00b_regex_override_t;

// ============================================================================
// Combined optimization result
// ============================================================================

/**
 * @brief All pre-computed optimizations for a compiled regex.
 */
typedef struct n00b_regex_optimizations_t {
    n00b_regex_accelerator_t accelerator;
    n00b_regex_len_lookup_t  len_lookup;
    n00b_regex_override_t    override_;
} n00b_regex_optimizations_t;

// ============================================================================
// API
// ============================================================================

// Forward declaration — full definition in regex.h
struct n00b_regex_t;

/**
 * @brief Analyze a compiled regex and compute match-time optimizations.
 *
 * Examines the regex AST to extract prefix sets, detect fixed-length
 * patterns, and select the best acceleration strategy. Results are
 * stored in @p out.
 *
 * @param re   A fully parsed regex (builder + minterms must be populated).
 * @param out  Output: filled with the selected optimizations.
 */
void
n00b_regex_find_optimizations(struct n00b_regex_t      *re,
                               n00b_regex_optimizations_t *out);

/**
 * @brief Strip prefix node: remove anchors, lookarounds, and optional
 *        prefixes from a regex node to extract the "essential" prefix.
 *
 * @param b     Builder containing the node pool.
 * @param node  The regex node to simplify.
 * @return A simplified node suitable for prefix analysis.
 */
uint32_t
n00b_regex_get_prefix_node(n00b_regex_builder_t *b, uint32_t node);

/**
 * @brief Compute the longest deterministic prefix minterm sequence.
 *
 * Follows the unique derivative chain until a branch or nullable state.
 *
 * @param re          Compiled regex.
 * @param start_node  The node to analyze (usually the prefix-stripped root).
 * @param out_sets    Output: array of minterm charsets (caller-allocated, max @p max_len).
 * @param max_len     Maximum prefix length to compute.
 * @return Number of minterms written to @p out_sets.
 */
uint32_t
n00b_regex_calc_prefix_sets(struct n00b_regex_t  *re,
                             uint32_t              start_node,
                             n00b_regex_charset_t *out_sets,
                             uint32_t              max_len);

/**
 * @brief Get fixed prefix length and remaining node.
 *
 * @param b          Builder.
 * @param node       Node to analyze.
 * @param out_remain Output: remaining node after prefix (or UINT32_MAX if none).
 * @return Fixed prefix length, or -1 if prefix is not fixed.
 */
int32_t
n00b_regex_get_fixed_prefix_length(n00b_regex_builder_t *b,
                                    uint32_t              node,
                                    uint32_t             *out_remain);
