#pragma once

/**
 * @file debug.h
 * @brief Debug output for slay grammars, parse trees, Earley state tables,
 *        and LR(0) tables.
 *
 * All functions print human-readable representations to a `FILE *`.
 * Useful for grammar development, parser debugging, and parse tree inspection.
 */

#include <stdio.h>

#include "slay/parse_tree.h"
#include "slay/parse_forest.h"
#include "slay/cfg.h"
#include "slay/cdg.h"
#include "slay/dfg.h"
#include "slay/cf_label.h"

// Forward declarations (avoid pulling in internal headers).
typedef struct n00b_earley_parser_t n00b_earley_parser_t;

/**
 * @brief Print all grammar rules to @p out.
 *
 * Shows rule index, NT name, RHS match items, cost, and error-rule flag.
 * Penalty and group rules are hidden when the grammar's hide flags are set.
 *
 * @param g    Grammar to print.
 * @param out  Output stream.
 */
void n00b_grammar_print(n00b_grammar_t *g, FILE *out);

/**
 * @brief Print the Earley parser chart to @p out.
 *
 * Each state shows token info and Earley items with rule, dot, origin,
 * penalty/cost, operation code, and subtree info.
 *
 * @param p         Earley parser with populated states.
 * @param out       Output stream.
 * @param show_all  If false, only show completed items.
 */
void n00b_parser_print_states(n00b_earley_parser_t *p, FILE *out, bool show_all);

/**
 * @brief Print a parse tree with indentation.
 *
 * Non-terminals show name, span, penalty, and cost.
 * Terminals show token text (or tid if no text) and span.
 *
 * @param tree  Root of the tree to print.
 * @param g     Grammar for NT name resolution (may be NULL for basic output).
 * @param out   Output stream.
 */
void n00b_tree_print(n00b_parse_tree_t *tree, n00b_grammar_t *g, FILE *out);

/**
 * @brief Print a parse forest (one or more trees).
 *
 * @param forest  Parse forest.
 * @param g       Grammar for NT name resolution.
 * @param out     Output stream.
 */
void n00b_forest_print(n00b_parse_forest_t *forest, n00b_grammar_t *g, FILE *out);

/**
 * @brief Format a parse tree node as a string.
 *
 * Returns a GC-managed `n00b_string_t`. Shows NT name or token text,
 * span, id, penalty, and cost.
 *
 * @param node  Parse tree node to format.
 * @return Formatted string representation.
 */
n00b_string_t *n00b_parse_node_repr(n00b_parse_tree_t *node);

/**
 * @brief Print the LR(0) state table to @p out.
 *
 * Shows states with items (rules + dot), goto entries, and
 * per-NT prediction states.
 *
 * @param g    Grammar with finalized LR(0) tables.
 * @param out  Output stream.
 */
void n00b_lr0_print(n00b_grammar_t *g, FILE *out);

/**
 * @brief Print a control flow graph to @p out.
 *
 * Shows block listing (with statements) and edge listing.
 *
 * @param cfg  CFG to print.
 * @param g    Grammar for parse node name resolution (may be NULL).
 * @param out  Output stream.
 */
void n00b_cfg_print(n00b_cfg_t *cfg, n00b_grammar_t *g, FILE *out);

/**
 * @brief Print control flow labels to @p out.
 *
 * Iterates the label dict and prints each label's kind, node, and
 * resolved subtree pointers.
 *
 * @param labels  CF label dict from `n00b_annot_walk_tree_full`.
 * @param g       Grammar for parse node name resolution (may be NULL).
 * @param out     Output stream.
 */
void n00b_cf_labels_print(n00b_cf_labels_t *labels, n00b_grammar_t *g, FILE *out);

/**
 * @brief Print a control dependence graph to @p out.
 *
 * Shows post-dominator tree and control dependence edges per block.
 *
 * @param cdg  CDG to print.
 * @param g    Grammar for parse node name resolution (may be NULL).
 * @param out  Output stream.
 */
void n00b_cdg_print(n00b_cdg_t *cdg, n00b_grammar_t *g, FILE *out);

/**
 * @brief Print a data flow graph to @p out.
 *
 * Shows def/use facts and DD edges with variable names, block IDs,
 * and line numbers.
 *
 * @param dfg  DFG to print.
 * @param g    Grammar for parse node name resolution (may be NULL).
 * @param out  Output stream.
 */
void n00b_dfg_print(n00b_dfg_t *dfg, n00b_grammar_t *g, FILE *out);
