/**
 * @file asp_engine.h
 * @brief Main engine API for the Datalog engine.
 *
 * Manages relations, rules, symbol interning, stratification,
 * and semi-naive fixpoint evaluation.
 *
 * ```c
 * n00b_dl_engine_t eng;
 * n00b_dl_engine_init(&eng);
 *
 * n00b_dl_rel_id_t edge = n00b_dl_engine_relation(&eng, n00b_cstr("edge"), 2);
 * n00b_dl_sym_t a = n00b_dl_const(&eng, n00b_cstr("a"));
 * n00b_dl_sym_t b = n00b_dl_const(&eng, n00b_cstr("b"));
 * n00b_dl_add_fact(&eng, edge, 2, (n00b_dl_sym_t[]){a, b});
 *
 * n00b_dl_run(&eng);
 * n00b_dl_engine_free(&eng);
 * ```
 */
#pragma once

#include "logic/asp_types.h"
#include "logic/asp_intern.h"
#include "logic/asp_relation.h"
#include "logic/asp_rule.h"
#include "adt/option.h"
#include "adt/result.h"
#include "adt/list.h"

typedef n00b_dl_relation_t *n00b_dl_relation_ptr_t;

/**
 * @brief Datalog engine state.
 */
typedef struct {
    n00b_dl_intern_t                    intern;
    n00b_list_t(n00b_dl_relation_ptr_t) relations;
    n00b_dl_rule_list_t                 rules;
    int32_t                             num_strata;
    bool                                stratified;
    int64_t                             iterations;
    int64_t                             facts_derived;
} n00b_dl_engine_t;

// Lifecycle
void n00b_dl_engine_init(n00b_dl_engine_t *eng);
void n00b_dl_engine_free(n00b_dl_engine_t *eng);

// Schema
n00b_dl_rel_id_t n00b_dl_engine_relation(n00b_dl_engine_t *eng,
                                           n00b_string_t    *name,
                                           int32_t           arity);

// Symbol interning shortcuts
n00b_dl_sym_t n00b_dl_const(n00b_dl_engine_t *eng, n00b_string_t *name);
n00b_dl_sym_t n00b_dl_int(n00b_dl_engine_t *eng, int64_t value);
n00b_dl_sym_t n00b_dl_var(n00b_dl_engine_t *eng, n00b_string_t *name);

// Facts and rules
void n00b_dl_add_fact(n00b_dl_engine_t *eng, n00b_dl_rel_id_t rel,
                        int32_t arity, const n00b_dl_sym_t *args);
void n00b_dl_add_rule(n00b_dl_engine_t *eng, n00b_dl_rule_t rule);

/**
 * @brief Run stratification and semi-naive fixpoint evaluation.
 *
 * @param eng Engine to run.
 * @return `false` if stratification failed (e.g., recursive negation).
 */
bool n00b_dl_run(n00b_dl_engine_t *eng);

// Expression constructors
n00b_dl_expr_t *n00b_dl_expr_sym(n00b_dl_sym_t sym);
n00b_dl_expr_t *n00b_dl_expr_int_lit(int64_t val);
n00b_dl_expr_t *n00b_dl_expr_binop(n00b_dl_expr_kind_t  op,
                                     n00b_dl_expr_t      *l,
                                     n00b_dl_expr_t      *r);
n00b_dl_expr_t *n00b_dl_expr_neg(n00b_dl_expr_t *operand);
void            n00b_dl_expr_free(n00b_dl_expr_t *expr);

/**
 * @brief Extract int64 value from an interned integer symbol.
 *
 * Integer symbols are interned as "#42", "#-7", etc.
 *
 * @param eng Engine.
 * @param sym Symbol to decode.
 * @return The integer value on success, or `EINVAL` if not an integer symbol.
 */
n00b_result_t(int64_t) n00b_dl_sym_to_int64(n00b_dl_engine_t *eng,
                                              n00b_dl_sym_t     sym);

// Query (callback-based)

/**
 * @brief Callback type for tuple iteration.
 *
 * @param tuple Array of `arity` symbol values.
 * @param arity Number of columns.
 * @param ctx   User-supplied context pointer.
 * @return `true` to continue iteration, `false` to stop early.
 */
typedef bool (*n00b_dl_query_cb)(const n00b_dl_sym_t *tuple, int32_t arity,
                                  void *ctx);

void   n00b_dl_query(n00b_dl_engine_t *eng, n00b_dl_rel_id_t rel,
                       n00b_dl_query_cb cb, void *ctx);
size_t n00b_dl_count(n00b_dl_engine_t *eng, n00b_dl_rel_id_t rel);

// Query by name and introspection
n00b_option_t(n00b_dl_rel_id_t) n00b_dl_find_relation(n00b_dl_engine_t *eng,
                                                        n00b_string_t    *name);
n00b_string_t   *n00b_dl_relation_name(n00b_dl_engine_t *eng,
                                         n00b_dl_rel_id_t  id);
n00b_option_t(int32_t) n00b_dl_relation_arity(n00b_dl_engine_t *eng,
                                               n00b_dl_rel_id_t  id);
size_t           n00b_dl_count_by_name(n00b_dl_engine_t *eng,
                                         n00b_string_t    *name);
int64_t          n00b_dl_iterations(n00b_dl_engine_t *eng);
int64_t          n00b_dl_total_facts(n00b_dl_engine_t *eng);
int32_t          n00b_dl_num_relations(n00b_dl_engine_t *eng);

// Symbol -> string
n00b_string_t *n00b_dl_sym_to_str(n00b_dl_engine_t *eng, n00b_dl_sym_t sym);
