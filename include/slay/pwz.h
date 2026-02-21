#pragma once

/**
 * @file pwz.h
 * @brief PWZ (Parsing With Zippers) derivative-based parser for arbitrary CFGs.
 *
 * Based on Darragh & Adams (ICFP 2020). Handles ambiguous and
 * left-recursive grammars via generalized zippers and memoization.
 */

#include "slay/grammar.h"
#include "slay/parse_forest.h"

typedef struct n00b_token_stream_t n00b_token_stream_t;

// ============================================================================
// Opaque parser state
// ============================================================================

/** @brief Opaque PWZ parser state. */
typedef struct n00b_pwz_parser_t n00b_pwz_parser_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new PWZ parser for a grammar.
 *
 * Finalizes the grammar if not already done, then builds the
 * internal expression graph.
 *
 * @param g  Grammar to parse with (must not be NULL).
 * @return Newly allocated PWZ parser.
 */
n00b_pwz_parser_t *n00b_pwz_new(n00b_grammar_t *g);

/**
 * @brief Free a PWZ parser and all associated state.
 * @param p  PWZ parser to free (NULL is a no-op).
 */
void n00b_pwz_free(n00b_pwz_parser_t *p);

/**
 * @brief Reset a PWZ parser for reuse with the same grammar.
 *
 * Clears per-parse state (memos, worklists, results) but keeps the
 * expression graph intact.
 *
 * @param p  PWZ parser to reset.
 */
void n00b_pwz_reset(n00b_pwz_parser_t *p);

// ============================================================================
// Parsing
// ============================================================================

/**
 * @brief Parse from a token stream with the PWZ parser.
 *
 * Tokens are consumed lazily from the stream as needed.
 *
 * @param p   PWZ parser to use.
 * @param ts  Token stream to consume.
 * @return True if parsing succeeded (at least one complete parse found).
 */
bool n00b_pwz_parse(n00b_pwz_parser_t   *p,
                     n00b_token_stream_t *ts);

// ============================================================================
// Results
// ============================================================================

/**
 * @brief Get the parse tree from the last successful parse.
 *
 * Returns the single "best" tree (longest / latest completion).
 *
 * @param p  PWZ parser to query.
 * @return The parse tree, or NULL if the last parse failed.
 */
n00b_parse_tree_t *n00b_pwz_get_tree(n00b_pwz_parser_t *p);

/**
 * @brief Get all parse trees from an ambiguous parse.
 * @param p  PWZ parser to query.
 * @return Array of parse tree pointers (empty if last parse failed).
 */
n00b_parse_tree_array_t n00b_pwz_get_trees(n00b_pwz_parser_t *p);

/**
 * @brief Get a parse forest from the last successful parse.
 * @param p  PWZ parser to query.
 * @return Forest wrapping all trees (empty if last parse failed).
 */
n00b_parse_forest_t n00b_pwz_get_forest(n00b_pwz_parser_t *p);

// ============================================================================
// One-shot parse (implements n00b_parse_fn_t)
// ============================================================================

/**
 * @brief Parse a token stream against a grammar using PWZ in a single call.
 *
 * Wraps the full PWZ lifecycle (new → parse → get_forest → free).
 * Signature matches `n00b_parse_fn_t` so it can be used as a
 * pluggable parse engine.
 *
 * @param g   Grammar to parse with.
 * @param ts  Token stream to consume.
 * @return Parse forest (empty on failure).
 */
n00b_parse_forest_t n00b_pwz_parse_grammar(n00b_grammar_t      *g,
                                             n00b_token_stream_t *ts);
