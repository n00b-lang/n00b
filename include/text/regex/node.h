/**
 * @file node.h
 * @brief Regex AST node pool and builder with algebraic simplification.
 *
 * Regex patterns are compiled to a pool of nodes referenced by uint32_t IDs.
 * The builder deduplicates structurally-equal nodes and applies algebraic
 * simplifications (e.g., Or(x, Nothing) = x) at construction time.
 *
 * Node IDs 0..6 are reserved sentinels:
 *   0 = NOTHING  (empty language / dead state)
 *   1 = EPSILON  (empty string / always-nullable)
 *   2 = ANY      (single wildcard / matches any one codepoint)
 *   3 = DOTSTAR  (.* / matches anything including empty)
 *   4 = ANYPLUS  (.+ / at least one codepoint)
 *   5 = END      (end-of-input anchor)
 *   6 = BEGIN    (start-of-input anchor)
 */
#pragma once

#include "n00b.h"
#include "text/regex/charset.h"
#include "adt/list.h"
#include "adt/dict.h"
#include "adt/result.h"

// ============================================================================
// Node kind enum
// ============================================================================

typedef enum {
    N00B_RE_SINGLETON,  /**< Matches one codepoint in a charset predicate. */
    N00B_RE_CONCAT,     /**< Concatenation of head and tail. */
    N00B_RE_OR,         /**< Alternation (union). */
    N00B_RE_AND,        /**< Intersection. */
    N00B_RE_NOT,        /**< Complement. */
    N00B_RE_LOOP,       /**< Counted repetition {min, max}. */
    N00B_RE_LOOKAROUND, /**< Lookahead / lookbehind. */
    N00B_RE_BEGIN,      /**< Start-of-input anchor (\A). */
    N00B_RE_END,        /**< End-of-input anchor (\z). */
} n00b_regex_node_kind_t;

// ============================================================================
// Well-known node IDs (reserved sentinels)
// ============================================================================

#define N00B_RE_ID_NOTHING  0  /**< Singleton(empty set) — matches nothing. */
#define N00B_RE_ID_EPSILON  1  /**< Loop(NOTHING, 0, MAX) — empty string. */
#define N00B_RE_ID_ANY      2  /**< Singleton(full set) — one codepoint. */
#define N00B_RE_ID_DOTSTAR  3  /**< Loop(ANY, 0, MAX) — any string. */
#define N00B_RE_ID_ANYPLUS  4  /**< Loop(ANY, 1, MAX) — one or more. */
#define N00B_RE_ID_END      5  /**< End anchor. */
#define N00B_RE_ID_BEGIN    6  /**< Begin anchor. */
#define N00B_RE_SENTINEL_COUNT 7

// ============================================================================
// Node type
// ============================================================================

/**
 * @brief A single regex AST node.
 *
 * Nodes are stored in a flat pool and referenced by uint32_t ID.
 * The union payload is tagged by @c kind.
 */
typedef struct n00b_regex_node_t {
    n00b_regex_node_kind_t kind;
    uint32_t               id;
    bool                   is_always_nullable;
    bool                   can_be_nullable;
    bool                   depends_on_anchor;
    bool                   contains_lookaround; /**< True if this or any child is a lookaround. */
    bool                   has_prefix_lookbehind; /**< Concat head chain starts with a lookbehind. */
    bool                   has_suffix_lookahead;  /**< Concat tail chain ends with a lookahead. */
    n00b_regex_charset_t   start_set;       /**< Overapprox of first-char set. */
    n00b_regex_charset_t   subsumed_by;     /**< Union of minterms this node can match. */
    int32_t                min_length;      /**< Minimum match length (-1 = unknown). */
    int32_t                max_length;      /**< Maximum match length (-1 = unbounded/unknown). */

    union {
        /** N00B_RE_SINGLETON */
        struct {
            n00b_regex_charset_t set;
        } singleton;

        /** N00B_RE_CONCAT */
        struct {
            uint32_t head;
            uint32_t tail;
        } concat;

        /** N00B_RE_OR, N00B_RE_AND */
        struct {
            uint32_t *children;
            uint32_t  count;
        } multi;

        /** N00B_RE_NOT */
        struct {
            uint32_t inner;
        } not_;

        /** N00B_RE_LOOP */
        struct {
            uint32_t body;
            int32_t  lo;
            int32_t  hi;   /**< INT32_MAX = unbounded. */
        } loop;

        /**
         * N00B_RE_LOOKAROUND
         *
         * Follows resharp's derivative model:
         * - `relative_to` tracks how many derivative steps have been taken
         *   since the lookaround was created (incremented each step).
         * - `pending_nullable_pos` records the relative positions where the
         *   body became nullable during derivative steps (for deferred
         *   lookaround acceptance).
         * - `n_pending` is the count of pending nullable positions.
         */
        struct {
            uint32_t  body;
            bool      look_back;
            int32_t   relative_to;
            int32_t  *pending_nullable_pos;
            uint16_t  n_pending;
        } lookaround;
    };
} n00b_regex_node_t;

// ============================================================================
// Builder
// ============================================================================

/**
 * @brief Regex node builder — creates, deduplicates, and simplifies nodes.
 */
typedef struct n00b_regex_builder_t {
    n00b_list_t(n00b_regex_node_t) nodes;  /**< Node pool. */
    n00b_dict_t(uint64_t, uint32_t) *dedup; /**< Structural hash → node ID. */
    n00b_regex_solver_t             *solver;
} n00b_regex_builder_t;

/** @brief Create a builder, pre-populated with sentinel nodes. */
n00b_regex_builder_t
n00b_regex_builder_new(n00b_regex_solver_t *solver);

/** @brief Access a node by ID. */
static inline n00b_regex_node_t *
n00b_regex_node_get(n00b_regex_builder_t *b, uint32_t id)
{
    assert(id < b->nodes.len);
    return &b->nodes.data[id];
}

// -- Node constructors (with dedup + simplification) --

/** @brief Create a Singleton(charset) node. */
uint32_t n00b_regex_mk_singleton(n00b_regex_builder_t *b,
                                  n00b_regex_charset_t  set);

/** @brief Create a Concat(head, tail) node. */
uint32_t n00b_regex_mk_concat(n00b_regex_builder_t *b,
                               uint32_t head, uint32_t tail);

/** @brief Create an Or node from an array of children. */
uint32_t n00b_regex_mk_or(n00b_regex_builder_t *b,
                           uint32_t *children, uint32_t count);

/** @brief Create an Or of exactly two children. */
uint32_t n00b_regex_mk_or2(n00b_regex_builder_t *b,
                            uint32_t a, uint32_t b_);

/** @brief Create an And node from an array of children. */
uint32_t n00b_regex_mk_and(n00b_regex_builder_t *b,
                            uint32_t *children, uint32_t count);

/**
 * @brief Create a Not(inner) node.
 *
 * Returns `result_err` with `N00B_RE_PARSE_NOT_LOOKAROUND` or
 * `N00B_RE_PARSE_NOT_ANCHOR` if inner contains unsupported patterns.
 */
n00b_result_t(uint32_t)
n00b_regex_mk_not(n00b_regex_builder_t *b, uint32_t inner);

/** @brief Create a Loop(body, lo, hi) node. */
uint32_t n00b_regex_mk_loop(n00b_regex_builder_t *b,
                             uint32_t body, int32_t lo, int32_t hi);

/**
 * @brief Create a Lookaround node (resharp-compatible).
 *
 * @param body       The lookaround body node.
 * @param look_back  true = lookbehind, false = lookahead.
 * @param relative_to  Derivative step counter (0 for freshly parsed).
 * @param pending    Array of pending nullable positions (can be NULL).
 * @param n_pending  Count of pending nullable positions.
 */
uint32_t n00b_regex_mk_lookaround(n00b_regex_builder_t *b,
                                   uint32_t body, bool look_back,
                                   int32_t relative_to,
                                   int32_t *pending, uint16_t n_pending);
