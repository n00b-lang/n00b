/**
 * @file infer.h
 * @brief Bridge between slay parse trees and the typecheck module.
 *
 * Translates `<type-spec>` parse subtrees into `n00b_tc_type_t *` nodes,
 * and provides the literal type callback typedef for modifier/encoding
 * refinement.
 */
#pragma once

#include "slay/types.h"
#include "slay/parse_tree.h"
#include "slay/grammar.h"

// Forward declarations to avoid header collision between
// typecheck/types.h and slay/token.h (both declare n00b_option_t(n00b_string_t)).
typedef struct n00b_tc_type_s n00b_tc_type_t;
typedef struct n00b_tc_ctx_s  n00b_tc_ctx_t;

/**
 * @brief Translate a `<type-spec>` parse subtree into a type node.
 *
 * Recursively walks the type-spec grammar NTs and produces the
 * corresponding `n00b_tc_type_t *`.
 *
 * | Parse subtree             | Type constructor                      |
 * |---------------------------|---------------------------------------|
 * | `<tspec-tvar>` (`` ` ``ID)| `n00b_tc_var(ctx, name)`              |
 * | bare `IDENTIFIER`         | `n00b_tc_lookup_prim` or `n00b_tc_prim` |
 * | `<tspec-parameterized>`   | `n00b_tc_param(ctx, name, params...)`  |
 * | `<tspec-func>`            | `n00b_tc_fn(ctx, params..., .returns)` |
 *
 * @param ctx        Type-checking context.
 * @param g          Grammar (needed for NT name lookups).
 * @param type_node  Root of the `<type-spec>` subtree.
 * @return           Translated type, or nullptr on failure.
 */
extern n00b_tc_type_t *n00b_tc_translate_type_spec(n00b_tc_ctx_t     *ctx,
                                                      n00b_grammar_t    *g,
                                                      n00b_parse_tree_t *type_node);
