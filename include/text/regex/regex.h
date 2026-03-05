/**
 * @file regex.h
 * @brief Public API for the n00b regex engine.
 *
 * A non-backtracking, O(n) regex engine based on Brzozowski symbolic
 * derivatives with lazy DFA construction. Supports standard regex plus
 * extension operators (`&` intersection, `~` complement, `_` wildcard).
 *
 * Usage:
 * @code
 *     n00b_string_t *pat = n00b_string_from_cstr("\\w+");
 *     auto r = n00b_regex_new(pat);
 *     if (n00b_result_is_ok(r)) {
 *         n00b_regex_t *re = n00b_result_get(r);
 *         bool hit = n00b_regex_is_match(re, input);
 *     }
 * @endcode
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "core/data_lock.h"
#include "adt/list.h"
#include "adt/result.h"
#include "text/regex/charset.h"
#include "text/regex/node.h"
#include "text/regex/parse.h"
#include "text/regex/optimize.h"

// ============================================================================
// DFA state flags and NullKind (matches resharp's StateFlags/NullKind)
// ============================================================================

/** @brief How a state is nullable — affects match boundary computation. */
typedef enum {
    N00B_RE_NULL_CURRENT  = 0,  /**< Nullable at current position (offset 0). */
    N00B_RE_NULL_PREV     = 1,  /**< Nullable at previous position (offset 1). */
    N00B_RE_NULL_BOTH_01  = 2,  /**< Nullable at both current and previous. */
    N00B_RE_NULL_PENDING  = 4,  /**< Complex pending nullable positions. */
    N00B_RE_NULL_NOT      = 255, /**< Not nullable. */
} n00b_regex_null_kind_t;

/** @brief Per-state flags (matches resharp's StateFlags). */
typedef enum {
    N00B_RE_SF_NONE               = 0,
    N00B_RE_SF_INITIAL            = 1,
    N00B_RE_SF_ANCHOR_NULLABLE    = 4,   /**< Nullable due to anchor deps. */
    N00B_RE_SF_CAN_SKIP           = 8,
    N00B_RE_SF_BEGIN_NULLABLE     = 16,  /**< Nullable at Begin location. */
    N00B_RE_SF_END_NULLABLE       = 32,  /**< Nullable at End location. */
    N00B_RE_SF_ALWAYS_NULLABLE    = 64,  /**< Always nullable. */
    N00B_RE_SF_PENDING_NULLABLE   = 128, /**< Has pending nullable positions. */
} n00b_regex_state_flags_t;

// ============================================================================
// Pending nullable position range
// ============================================================================

/** @brief A range [lo, hi] of relative nullable positions. */
typedef struct n00b_regex_pending_range_t {
    uint16_t lo;
    uint16_t hi;
} n00b_regex_pending_range_t;

// ============================================================================
// DFA types
// ============================================================================

/** @brief One DFA state. */
typedef struct n00b_regex_dfa_state_t {
    uint32_t                    node_id;        /**< Regex node this state represents. */
    n00b_regex_state_flags_t    flags;          /**< State flags. */
    n00b_regex_null_kind_t      null_kind;      /**< How this state is nullable. */
    int32_t                    *transitions;    /**< State IDs indexed by minterm_id.
                                                     -1 = not yet computed (lazy). */
    uint16_t                    n_transitions;  /**< Size of transitions (== minterm count). */
    n00b_regex_pending_range_t *pending_ranges; /**< Pending nullable position ranges. */
    uint16_t                    n_pending;      /**< Number of pending ranges. */
    int32_t                     min_pending;    /**< Minimum pending nullable offset. */
    uint8_t                     skip_map[16];   /**< 128-bit bitmap: which ASCII bytes cause
                                                     a non-self-loop, non-dead transition.
                                                     Only valid when can_skip is true. */
    bool                        can_skip;       /**< True if skip_map is usable for skipping. */
} n00b_regex_dfa_state_t;

/** @brief A lazy or fully-compiled DFA. */
typedef struct n00b_regex_dfa_t {
    n00b_list_t(n00b_regex_dfa_state_t) states;
    n00b_dict_t(uint32_t, uint32_t)    *node_to_state;
    n00b_regex_builder_t               *builder;
    n00b_regex_minterm_table_t         *minterms;
    n00b_dict_t(uint64_t, uint32_t)    *deriv_cache;
    int32_t                            *anchor_transitions; /**< Separate cache for End-location anchor transitions. */
    uint32_t                           *flat_transitions;   /**< Flat table: state * n_minterms + mt → next state.
                                                                 Only valid when is_flat is true. */
    uint32_t                            anchor_trans_size;
    uint32_t                            start_state;
    uint16_t                            n_minterms_cached;  /**< Cached minterm count for flat table indexing. */
    uint8_t                             mt_log2;            /**< log2(n_minterms_padded) for bit-shift indexing. */
    bool                                is_flat;            /**< True if flat_transitions is populated. */
    n00b_rwlock_t                      *lock;
} n00b_regex_dfa_t;

// ============================================================================
// Compiled regex handle
// ============================================================================

/** @brief A compiled regex — opaque, reusable, thread-safe after compile. */
typedef struct n00b_regex_t {
    n00b_regex_solver_t         solver;
    n00b_regex_builder_t        builder;
    n00b_regex_minterm_table_t *minterms;
    n00b_regex_dfa_t           *forward_dfa;
    n00b_regex_dfa_t           *reverse_dfa;
    n00b_string_t              *pattern;
    bool                        is_full_dfa;
    n00b_regex_optimizations_t  optimizations; /**< Pre-computed match-time optimizations. */
} n00b_regex_t;

/** @brief A single match result — a slice of the input. */
typedef struct n00b_regex_match_t {
    int64_t index;   /**< Byte offset into the input string. */
    int64_t length;  /**< Byte length of the match. */
} n00b_regex_match_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Compile a regex pattern.
 *
 * @param pattern  The regex pattern (UTF-8 n00b_string_t).
 *
 * @kw case_insensitive  Case-insensitive matching (default: false).
 * @kw multiline         ^ and $ match line boundaries (default: false).
 * @kw dot_all           . matches newlines (default: false).
 *
 * @return On success, result_ok with a regex handle.
 *         On failure, result_err with parse error code.
 */
n00b_result_t(n00b_regex_t *)
n00b_regex_new(n00b_string_t *pattern) _kargs {
    bool case_insensitive = false;
    bool multiline        = false;
    bool dot_all          = false;
};

/** @brief Test if the pattern matches anywhere in @p input. */
bool n00b_regex_is_match(n00b_regex_t *re, n00b_string_t *input);

/** @brief Count non-overlapping matches. */
int64_t n00b_regex_count(n00b_regex_t *re, n00b_string_t *input);

/**
 * @brief Find all non-overlapping matches.
 * @return A list of n00b_regex_match_t. Empty list if no matches.
 */
n00b_list_t(n00b_regex_match_t) *
n00b_regex_matches(n00b_regex_t *re, n00b_string_t *input);

/**
 * @brief Replace all non-overlapping matches with @p replacement.
 * @param replacement Replacement string. `$0` expands to the matched text.
 * @return A new string with replacements applied.
 */
n00b_string_t *
n00b_regex_replace(n00b_regex_t  *re,
                   n00b_string_t *input,
                   n00b_string_t *replacement);

/**
 * @brief Split @p input on matches of the pattern.
 * @return A list of n00b_string_t * segments.
 */
n00b_list_t(n00b_string_t *) *
n00b_regex_split(n00b_regex_t *re, n00b_string_t *input);

/** @brief Default DFA state threshold for precompilation. */
#define N00B_RE_DFA_THRESHOLD 10000

/**
 * @brief Force-compile the full DFA (bidirectional BFS).
 *
 * Uses resharp's bidirectional BFS: reverse phase from the end-location
 * transitions, then forward phase from the start state. Aborts if the
 * state count exceeds the threshold.
 *
 * After successful completion, matching is pure table lookups.
 *
 * @kw max_states  Safety limit on number of DFA states (default: N00B_RE_DFA_THRESHOLD).
 */
void n00b_regex_compile(n00b_regex_t *re) _kargs {
    uint32_t max_states = N00B_RE_DFA_THRESHOLD;
};

/** @brief Check whether the DFA is fully compiled. */
bool n00b_regex_is_compiled(n00b_regex_t *re);
