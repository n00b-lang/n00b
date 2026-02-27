#pragma once

/**
 * @file n00b_compile.h
 * @brief N00b-specific compilation pipeline orchestrator.
 *
 * Wires together the generic slay toolkit with n00b-specific components:
 * - N00b tokenizer (`n00b_lang_tokenize`)
 * - N00b type-spec translator (`n00b_tc_translate_type_spec`)
 *
 * This is the boundary between slay (language-agnostic) and the
 * n00b compiler (language-specific).
 */

#include "slay/annot_walk.h"
#include "slay/n00b_parse.h"

/**
 * @brief Run the full n00b annotation walk with n00b-specific callbacks.
 *
 * Sets up the annotation walk context with the n00b type-spec translator
 * and runs the walk.  This is equivalent to `n00b_annot_walk_tree_full_ex`
 * with `n00b_tc_translate_type_spec` as the callback.
 *
 * @param g     Grammar (must have annotations from n00b BNF).
 * @param tree  Root of the parse tree.
 * @return Walk result, or NULL on error. Caller owns it.
 */
n00b_annot_result_t *n00b_compile_walk(n00b_grammar_t    *g,
                                        n00b_parse_tree_t *tree);

/**
 * @brief Run the full n00b annotation walk from a parse result.
 *
 * Convenience wrapper that extracts the grammar and tree from the
 * parse result and calls `n00b_compile_walk`.
 *
 * @param result  Parse result (must have succeeded).
 * @return Walk result, or NULL on error. Caller owns it.
 */
n00b_annot_result_t *n00b_compile_walk_result(n00b_parse_result_t *result);
