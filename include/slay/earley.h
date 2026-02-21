#pragma once

/**
 * @file earley.h
 * @brief Earley parser engine with LR(0)-table-driven prediction and Leo
 *        optimization for right-recursive grammars.
 *
 * Based on the Earley (1968/1970) algorithm with extensions:
 * - LR(0) prediction states for fast non-terminal prediction
 * - Leo (1991) optimization: right-recursive rules run in O(n) instead of O(n³)
 * - BSR (Binary Subtree Representation) parse forest for efficient tree extraction
 * - Cost/penalty-based ambiguity resolution
 *
 * Consumes tokens lazily from an `n00b_token_stream_t`.
 */

#include "slay/grammar.h"
#include "slay/parse_forest.h"

typedef struct n00b_earley_parser_t n00b_earley_parser_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new Earley parser for a grammar.
 *
 * Finalizes the grammar if not already done.
 *
 * @param g  Grammar to parse with (must not be NULL).
 * @return Newly allocated Earley parser.
 */
n00b_earley_parser_t *n00b_earley_new(n00b_grammar_t *g);

/**
 * @brief Free an Earley parser and all associated state.
 * @param p  Parser to free (NULL is a no-op).
 */
void n00b_earley_free(n00b_earley_parser_t *p);

/**
 * @brief Reset an Earley parser for reuse with the same grammar.
 *
 * Clears per-parse state (chart, BSR set, token cache) but keeps
 * the grammar reference intact.
 *
 * @param p  Parser to reset.
 */
void n00b_earley_reset(n00b_earley_parser_t *p);

// ============================================================================
// Parsing
// ============================================================================

/**
 * @brief Parse from a token stream with the Earley parser.
 *
 * Tokens are consumed lazily from the stream as needed.
 *
 * @param p   Earley parser to use.
 * @param ts  Token stream to consume.
 * @return True if parsing succeeded (at least one complete parse found).
 */
bool n00b_earley_parse(n00b_earley_parser_t *p,
                        n00b_token_stream_t  *ts);

// ============================================================================
// Results
// ============================================================================

/**
 * @brief Get the best parse tree from the last successful parse.
 *
 * Returns the lowest-cost, lowest-penalty tree.
 *
 * @param p  Earley parser to query.
 * @return The parse tree, or NULL if the last parse failed.
 */
n00b_parse_tree_t *n00b_earley_get_tree(n00b_earley_parser_t *p);

/**
 * @brief Get the full parse forest from the last successful parse.
 * @param p  Earley parser to query.
 * @return Forest wrapping all trees (empty if last parse failed).
 */
n00b_parse_forest_t n00b_earley_get_forest(n00b_earley_parser_t *p);

/**
 * @brief Count valid completions of the start symbol.
 * @param p  Earley parser (must have completed a parse).
 * @return Number of valid completions.
 */
int32_t n00b_earley_parse_count(n00b_earley_parser_t *p);

// ============================================================================
// One-shot parse (implements n00b_parse_fn_t)
// ============================================================================

/**
 * @brief Parse a token stream against a grammar using Earley in a single call.
 *
 * Wraps the full Earley lifecycle (new -> parse -> get_forest -> free).
 * Signature matches `n00b_parse_fn_t` so it can be used as a
 * pluggable parse engine.
 *
 * @param g   Grammar to parse with.
 * @param ts  Token stream to consume.
 * @return Parse forest (empty on failure).
 */
n00b_parse_forest_t n00b_earley_parse_grammar(n00b_grammar_t      *g,
                                               n00b_token_stream_t *ts);
