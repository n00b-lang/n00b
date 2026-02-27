/**
 * @file infer_expr.h
 * @brief Interpreter for `@infer` expression mini-language.
 *
 * Evaluates `@infer("...")` annotation strings against the walk context,
 * producing a `n00b_tc_type_t *` for the annotated parse node.
 *
 * ## Mini-language
 *
 * | Syntax                    | Meaning                                        |
 * |---------------------------|------------------------------------------------|
 * | `$N`                      | Type of Nth non-terminal child                 |
 * | `$N[T1, T2]`              | Parameterized type: name from child N's text   |
 * | `$N[...$M]`               | Same, with params spread from NT-child M       |
 * | `` `x ``                  | Fresh type variable (same name = same var)     |
 * | `bool`, `int`, `nil`, ... | Primitive type lookup                          |
 * | `name[T1, T2]`            | Parameterized type (list, dict, ref, ...)      |
 * | `A unify B`               | Unify two types, return the unified result     |
 * | `A \| B`                  | Sum type                                       |
 * | `lookup($N)`              | Symtab lookup: child N's identifier → type_var |
 * | `return_of($N)`           | Return type of function-typed child N          |
 * | `$return`                 | Current function's return type variable         |
 * | `(expr)`                  | Grouping                                       |
 */
#pragma once

#include "slay/types.h"
#include "slay/parse_tree.h"
#include "slay/grammar.h"
#include "slay/symtab.h"

typedef struct n00b_tc_type_s n00b_tc_type_t;
typedef struct n00b_tc_ctx_s  n00b_tc_ctx_t;

// Forward declare the dict type used for node_types.
typedef struct n00b_dict_s n00b_dict_s;

/**
 * @brief Evaluate an `@infer` expression string.
 *
 * Parses and interprets the expression against the current walk context,
 * producing a type for the annotated parse tree node.
 *
 * @param tc_ctx      Type-checking context (for fresh vars, prims, unify).
 * @param symtab      Symbol table (for `lookup($N)`).
 * @param grammar     Grammar (for child resolution).
 * @param node        The parse tree node being annotated.
 * @param node_types  Dict mapping `(uintptr_t)node → n00b_tc_type_t *`.
 * @param expr        The `@infer` expression string to evaluate.
 * @return            The resulting type, or NULL on parse/eval error.
 */
n00b_tc_type_t *n00b_infer_eval(
    n00b_tc_ctx_t  *tc_ctx,
    n00b_symtab_t  *symtab,
    n00b_grammar_t *grammar,
    n00b_parse_tree_t *node,
    void           *node_types,
    n00b_string_t   expr);
