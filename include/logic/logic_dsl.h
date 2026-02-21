/**
 * @file logic_dsl.h
 * @brief Declarative text DSL for the Datalog + CSP pipeline.
 *
 * Compiles a Prolog-like text format into `n00b_logic_t` programs.
 * Supports facts, rules, CSP variable declarations, constraints,
 * queries, and `solve` commands.
 *
 * ## Example
 *
 * ```prolog
 * edge(a, b).
 * edge(b, c).
 * edge(a, c).
 *
 * color(Node) in 1..3 :- edge(Node, _).
 * color(Node) in 1..3 :- edge(_, Node).
 * color(X) != color(Y) :- edge(X, Y).
 *
 * solve.
 * ```
 */
#pragma once

#include "logic/logic_program.h"

/**
 * @brief Result of compiling/running a DSL program.
 */
typedef struct {
    n00b_logic_t *prog;           /**< Compiled program (caller frees via n00b_dsl_result_free()). */
    bool          solved;         /**< true if `solve` succeeded. */
    int64_t       solution_count; /**< Number of solutions if `solve all`. */
    n00b_string_t error;          /**< Error message, or empty string on success. */
    int32_t       error_line;     /**< 1-based line of error. */
    int32_t       error_col;      /**< 1-based column of error. */
} n00b_dsl_result_t;

/**
 * @brief Compile a DSL source string into a logic program.
 *
 * Parses, compiles, and executes the phased instruction list.
 * Does not run `solve` commands.
 *
 * @param src Source text.
 * @return    Result with compiled program or error.
 */
n00b_dsl_result_t n00b_dsl_compile(n00b_string_t src);

/**
 * @brief Compile and run a DSL program, including solve commands.
 *
 * @param src Source text.
 * @param cb  Solution callback for `solve all` (may be nullptr).
 * @param ctx User context for callback.
 * @return    Result with solved state.
 */
n00b_dsl_result_t n00b_dsl_run(n00b_string_t src,
                                 n00b_logic_solution_cb cb, void *ctx);

/**
 * @brief Free a DSL result (including the embedded logic program).
 */
void n00b_dsl_result_free(n00b_dsl_result_t *r);
