/**
 * @file asp_rule.h
 * @brief Rule builder for the Datalog engine.
 *
 * Provides an incremental builder API for constructing Datalog
 * rules (head :- body) from code.
 */
#pragma once

#include "logic/asp_types.h"

/**
 * @brief Incremental rule builder.
 */
typedef struct {
    n00b_dl_literal_t    head;
    n00b_dl_body_goal_t *body;
    int32_t              body_len;
    int32_t              body_cap;
} n00b_dl_rule_builder_t;

/**
 * @brief Initialize a rule builder.
 * @param b Builder to initialize.
 */
void n00b_dl_rule_builder_init(n00b_dl_rule_builder_t *b);

/**
 * @brief Set the head literal.
 *
 * @param b     Builder.
 * @param rel   Relation ID.
 * @param arity Number of arguments.
 * @param args  Symbol arguments (copied).
 */
void n00b_dl_rule_builder_head(n00b_dl_rule_builder_t *b,
                                 n00b_dl_rel_id_t        rel,
                                 int32_t                 arity,
                                 const n00b_dl_sym_t    *args);

/**
 * @brief Add a body literal.
 *
 * @param b       Builder.
 * @param rel     Relation ID.
 * @param arity   Number of arguments.
 * @param args    Symbol arguments (copied).
 * @param negated `true` for negated literals.
 */
void n00b_dl_rule_builder_add(n00b_dl_rule_builder_t *b,
                                n00b_dl_rel_id_t        rel,
                                int32_t                 arity,
                                const n00b_dl_sym_t    *args,
                                bool                    negated);

/**
 * @brief Add a builtin constraint goal.
 *
 * @param b       Builder.
 * @param builtin Builtin (expression tree ownership transferred).
 */
void n00b_dl_rule_builder_add_builtin(n00b_dl_rule_builder_t *b,
                                        n00b_dl_builtin_t       builtin);

/**
 * @brief Finalize and produce a rule.
 *
 * The builder is consumed; re-initialize before reuse.
 *
 * @param b Builder.
 * @return Completed rule.
 */
n00b_dl_rule_t n00b_dl_rule_builder_finish(n00b_dl_rule_builder_t *b);

/**
 * @brief Free resources held by a rule.
 * @param rule Rule to free.
 */
void n00b_dl_rule_free(n00b_dl_rule_t *rule);
