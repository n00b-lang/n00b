/**
 * @file charset.h
 * @brief BDD-based character set solver and minterm computation for the regex engine.
 *
 * Character sets are represented as Binary Decision Diagrams (BDDs) over the
 * 21-bit Unicode codepoint space. Each BDD node tests one bit of a codepoint
 * and branches to "hi" (bit=1) or "lo" (bit=0) children. The terminal nodes
 * TRUE and FALSE represent membership/non-membership.
 *
 * BDD boolean operations (or, and, not) use recursive Shannon expansion with
 * memoization. The solver interns all nodes so structurally-equal BDDs share
 * the same index.
 *
 * Minterms partition the alphabet into non-overlapping equivalence classes
 * based on which predicates (charsets) a codepoint satisfies. This compresses
 * the input alphabet for DFA construction.
 */
#pragma once

#include "n00b.h"
#include "adt/list.h"
#include "adt/dict.h"
#include "text/unicode/types.h"
#include "text/unicode/properties.h"

// ============================================================================
// BDD node
// ============================================================================

/**
 * @brief A single BDD node: tests bit @c var of a codepoint.
 *
 * Leaf sentinels use var = UINT32_MAX (TRUE) and var = UINT32_MAX-1 (FALSE).
 */
typedef struct n00b_regex_bdd_node_t {
    uint32_t var; /**< Bit position (0..20), or sentinel for leaves. */
    uint32_t lo;  /**< Child index when bit=0. */
    uint32_t hi;  /**< Child index when bit=1. */
} n00b_regex_bdd_node_t;

/** A charset is an index into the solver's BDD node pool. */
typedef uint32_t n00b_regex_charset_t;

/** A minterm ID — index into the minterm table. */
typedef uint16_t n00b_regex_minterm_id_t;

/** Sentinel var values for BDD leaves. */
#define N00B_REGEX_BDD_TRUE_VAR  UINT32_MAX
#define N00B_REGEX_BDD_FALSE_VAR (UINT32_MAX - 1)

// ============================================================================
// Solver
// ============================================================================

/**
 * @brief BDD-based character set solver.
 *
 * Manages a pool of interned BDD nodes and memoized boolean operations.
 * All charsets produced by this solver are indices into @c nodes.
 */
typedef struct n00b_regex_solver_t {
    n00b_list_t(n00b_regex_bdd_node_t) nodes;      /**< BDD node pool. */
    n00b_dict_t(uint64_t, uint32_t)   *memo_or;     /**< Or operation cache. */
    n00b_dict_t(uint64_t, uint32_t)   *memo_and;    /**< And operation cache. */
    n00b_dict_t(uint32_t, uint32_t)   *memo_not;    /**< Not operation cache. */
    n00b_dict_t(uint32_t, uint32_t)   *gc_cache;    /**< Unicode GC → BDD cache. */
    n00b_regex_charset_t               true_id;      /**< Index of TRUE leaf. */
    n00b_regex_charset_t               false_id;     /**< Index of FALSE leaf. */
} n00b_regex_solver_t;

/** @brief Create a new BDD solver with TRUE/FALSE sentinel nodes. */
n00b_regex_solver_t n00b_regex_solver_new(void);

/**
 * @brief Build a BDD for a codepoint range [lo, hi] inclusive.
 * @pre lo <= hi <= 0x10FFFF
 */
n00b_regex_charset_t
n00b_regex_charset_range(n00b_regex_solver_t *s, uint32_t lo, uint32_t hi);

/** @brief Build a BDD for a single codepoint. */
n00b_regex_charset_t
n00b_regex_charset_single(n00b_regex_solver_t *s, n00b_codepoint_t cp);

/** @brief Build a BDD matching all codepoints with the given general category. */
n00b_regex_charset_t
n00b_regex_charset_from_gc(n00b_regex_solver_t *s, n00b_unicode_gc_t gc);

/** @brief Boolean OR of two charsets. */
n00b_regex_charset_t
n00b_regex_charset_or(n00b_regex_solver_t *s,
                      n00b_regex_charset_t a,
                      n00b_regex_charset_t b);

/** @brief Boolean AND of two charsets. */
n00b_regex_charset_t
n00b_regex_charset_and(n00b_regex_solver_t *s,
                       n00b_regex_charset_t a,
                       n00b_regex_charset_t b);

/** @brief Boolean NOT (complement) of a charset. */
n00b_regex_charset_t
n00b_regex_charset_not(n00b_regex_solver_t *s, n00b_regex_charset_t a);

/** @brief Test whether a charset contains a given codepoint. */
bool
n00b_regex_charset_contains(n00b_regex_solver_t *s,
                            n00b_regex_charset_t cs,
                            n00b_codepoint_t     cp);

/** @brief Test whether a charset is the empty set (FALSE). */
static inline bool
n00b_regex_charset_is_empty(n00b_regex_solver_t *s, n00b_regex_charset_t cs)
{
    return cs == s->false_id;
}

/** @brief Test whether a charset is the full set (TRUE). */
static inline bool
n00b_regex_charset_is_full(n00b_regex_solver_t *s, n00b_regex_charset_t cs)
{
    return cs == s->true_id;
}

// ============================================================================
// Minterm table
// ============================================================================

/**
 * @brief A table of minterms — non-overlapping equivalence classes that
 *        partition the Unicode codepoint space.
 *
 * Includes a precomputed lookup table for the full BMP (0-65535) for O(1)
 * classification, matching resharp's `createLookupUtf16` approach.
 * Codepoints above U+FFFF fall back to BDD walking.
 */
typedef struct n00b_regex_minterm_table_t {
    n00b_regex_charset_t    *minterms;         /**< Array of BDD indices, one per minterm. */
    n00b_regex_solver_t     *solver;           /**< Owning solver. */
    uint16_t                 count;            /**< Number of minterms. */
    n00b_regex_minterm_id_t *bmp_lut;          /**< 65536-entry BMP lookup table. */
    uint8_t                  byte_lut[256];    /**< ASCII fast path: byte → minterm ID.
                                                    0x00-0x7F are valid; 0x80-0xFF use sentinel
                                                    0xFF meaning "fall back to slow path". */
} n00b_regex_minterm_table_t;

/** Sentinel value in byte_lut indicating multi-byte UTF-8 (needs slow path). */
#define N00B_RE_BYTE_LUT_MULTI 0xFF

/**
 * @brief Compute minterms from a list of charset predicates.
 *
 * Uses the partition-tree algorithm: each predicate refines the existing
 * partitions by splitting any overlapping leaf into (leaf AND pred) and
 * (leaf AND NOT pred).
 *
 * @param s     The solver.
 * @param preds Array of charset predicates.
 * @param n     Number of predicates.
 * @return A new minterm table. Caller should eventually free it.
 */
n00b_regex_minterm_table_t *
n00b_regex_compute_minterms(n00b_regex_solver_t  *s,
                            n00b_regex_charset_t *preds,
                            uint32_t              n);

/**
 * @brief Classify a codepoint to its minterm ID.
 *
 * Fast path: ASCII codepoints use a precomputed lookup table (O(1)).
 * Slow path: non-ASCII walks BDDs.
 */
n00b_regex_minterm_id_t
n00b_regex_minterm_classify(n00b_regex_minterm_table_t *mt,
                            n00b_codepoint_t            cp);

/**
 * @brief Test whether the intersection of a charset and a minterm is non-empty.
 */
bool
n00b_regex_charset_intersects_minterm(n00b_regex_solver_t  *s,
                                     n00b_regex_charset_t  cs,
                                     n00b_regex_charset_t  mt);

/**
 * @brief Test whether charset @p a contains (is superset of) charset @p b.
 *
 * Returns true iff every codepoint in @p b is also in @p a,
 * i.e. `(b AND NOT a) == FALSE`.
 */
static inline bool
n00b_regex_charset_contains_set(n00b_regex_solver_t *s,
                                n00b_regex_charset_t a,
                                n00b_regex_charset_t b)
{
    return n00b_regex_charset_is_empty(s,
        n00b_regex_charset_and(s, b, n00b_regex_charset_not(s, a)));
}
