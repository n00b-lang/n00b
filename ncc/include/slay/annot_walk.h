#pragma once

/**
 * @file annot_walk.h
 * @brief Post-parse reclassify walk -- DFS the best tree, firing
 *        @reclassify annotations to mutate token IDs.
 *
 * When a production has @reclassify(guard_ref, "guard_text", NEW_TYPE),
 * matching declarations cause subsequent IDENTIFIER tokens to be
 * reclassified to NEW_TYPE (e.g., TYPEDEF_NAME).
 */

#include "slay/grammar.h"
#include "slay/parse_tree.h"
#include "slay/symtab.h"

/**
 * @brief Walk a parse tree, firing @reclassify annotations to mutate tokens.
 *
 * DFS post-order: children fire before parents so inner declarations
 * take effect first.  For each rule with @reclassify, checks the guard
 * child for the guard text, extracts the declared identifier, then
 * mutates all matching IDENTIFIER tokens after the declaration point.
 *
 * @param g       Grammar with reclassify annotations.
 * @param tree    Root of the parse tree to walk.
 * @param tokens  Array of token pointers (from token stream).
 * @param ntokens Number of tokens in the array.
 * @return Number of tokens reclassified.
 */
int32_t n00b_annot_reclassify_walk(n00b_grammar_t    *g,
                                    n00b_parse_tree_t *tree,
                                    n00b_token_info_t **tokens,
                                    int32_t            ntokens);

/**
 * @brief Same as n00b_annot_reclassify_walk, but also populates a
 *        symbol table with typedef entries for each reclassified name.
 *
 * @param g       Grammar with reclassify annotations.
 * @param tree    Root of the parse tree to walk.
 * @param tokens  Array of token pointers (from token stream).
 * @param ntokens Number of tokens in the array.
 * @param st      Symbol table to populate (NULL to skip symtab).
 * @return Number of tokens reclassified.
 */
int32_t n00b_annot_reclassify_walk_with_symtab(n00b_grammar_t    *g,
                                                 n00b_parse_tree_t *tree,
                                                 n00b_token_info_t **tokens,
                                                 int32_t            ntokens,
                                                 n00b_symtab_t     *st);
