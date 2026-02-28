/**
 * @file logic_program.h
 * @brief Unified Datalog + CSP pipeline interface.
 *
 * Composes the Datalog semi-naive engine (`n00b_dl_*`) with the
 * finite-domain constraint solver (`n00b_csp_*`) into a single
 * `n00b_logic_t` program.  Datalog computes relational structure;
 * CSP solves assignments over that structure.
 *
 * ## Quick start (ergonomic API)
 *
 * ```c
 * auto prog = n00b_logic_new();
 * n00b_logic_fact(prog, STR("edge"), &STR("a"), &STR("b"));
 * n00b_logic_fact(prog, STR("edge"), &STR("b"), &STR("c"));
 * n00b_logic_fact(prog, STR("edge"), &STR("a"), &STR("c"));
 * n00b_logic_bridge(prog, STR("edge"),
 *     .domain = n00b_csp_dom_range(1, 3),
 *     .constraint = N00B_CSP_CON_NE);
 * n00b_logic_solve(prog);
 * int64_t ca = n00b_result_get(n00b_logic_get_int(prog, STR("a")));
 * n00b_logic_free(prog);
 * ```
 *
 * ## Low-level API
 *
 * ```c
 * n00b_logic_t prog;
 * n00b_logic_init(&prog);
 *
 * n00b_dl_rel_id_t edge = n00b_logic_relation(&prog, r"edge", 2);
 * n00b_dl_sym_t a = n00b_logic_const(&prog, r"a");
 * n00b_dl_sym_t b = n00b_logic_const(&prog, r"b");
 * n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, b});
 *
 * n00b_logic_run_datalog(&prog);
 * n00b_logic_vars_from_rel(&prog, edge, 0, n00b_csp_dom_range(1, 3));
 * n00b_logic_constrain_pairs(&prog, edge, N00B_CSP_CON_NE);
 * n00b_logic_run_csp(&prog);
 *
 * n00b_logic_free(&prog);
 * ```
 */
#pragma once

#include "logic/asp_engine.h"
#include "logic/clpfd_label.h"
#include "core/vargs.h"

/**
 * @brief Unified logic program (Datalog + CSP).
 */
typedef struct {
    n00b_dl_engine_t       engine;
    n00b_csp_store_t      *store;       /**< nullptr until first CSP call. */
    n00b_dl_str_i64_map_t  sym_to_csp;  /**< symbol name -> csp_var_id. */
    n00b_dl_i64_str_map_t  csp_to_sym;  /**< reverse mapping. */
    bool                   datalog_ran;
    bool                   _heap;       /**< True if heap-allocated via n00b_logic_new(). */
} n00b_logic_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Initialize a stack-allocated logic program.
 * @param prog Program to initialize.
 */
void n00b_logic_init(n00b_logic_t *prog);

/**
 * @brief Allocate and initialize a new logic program on the heap.
 * @return Heap-allocated, initialized program. Free with n00b_logic_free().
 */
n00b_logic_t *n00b_logic_new(void);

/**
 * @brief Free all resources held by a logic program.
 *
 * If `prog` was created via n00b_logic_new(), the pointer itself
 * is also freed.
 *
 * @param prog Program to free.
 */
void n00b_logic_free(n00b_logic_t *prog);

// ============================================================================
// Datalog wrappers
// ============================================================================

/** @brief Define or look up a relation. */
n00b_dl_rel_id_t n00b_logic_relation(n00b_logic_t *prog, n00b_string_t *name,
                                       int32_t arity);

/** @brief Intern a constant symbol. */
n00b_dl_sym_t n00b_logic_const(n00b_logic_t *prog, n00b_string_t *name);

/** @brief Intern an integer symbol. */
n00b_dl_sym_t n00b_logic_int(n00b_logic_t *prog, int64_t value);

/** @brief Intern a logic variable. */
n00b_dl_sym_t n00b_logic_var(n00b_logic_t *prog, n00b_string_t *name);

/** @brief Add a ground fact. */
void n00b_logic_add_fact(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                           int32_t arity, const n00b_dl_sym_t *args);

/** @brief Add a rule. */
void n00b_logic_add_rule(n00b_logic_t *prog, n00b_dl_rule_t rule);

// ============================================================================
// CSP wrappers (auto-creates store on first call)
// ============================================================================

/**
 * @brief Create a CSP variable.
 *
 * Auto-creates the constraint store if needed.
 *
 * @param prog   Program.
 * @param name   Variable name.
 * @param domain Initial domain.
 * @return       Variable ID.
 */
n00b_csp_var_id_t n00b_logic_csp_var(n00b_logic_t *prog, n00b_string_t *name,
                                       n00b_csp_domain_t domain);

/** @brief Post X = Y. */
bool n00b_logic_csp_eq(n00b_logic_t *prog, n00b_csp_var_id_t x,
                         n00b_csp_var_id_t y);

/** @brief Post X != Y. */
bool n00b_logic_csp_ne(n00b_logic_t *prog, n00b_csp_var_id_t x,
                         n00b_csp_var_id_t y);

/** @brief Post X < Y. */
bool n00b_logic_csp_lt(n00b_logic_t *prog, n00b_csp_var_id_t x,
                         n00b_csp_var_id_t y);

/** @brief Post X <= Y. */
bool n00b_logic_csp_le(n00b_logic_t *prog, n00b_csp_var_id_t x,
                         n00b_csp_var_id_t y);

/** @brief Post X = c (constant). */
bool n00b_logic_csp_eq_const(n00b_logic_t *prog, n00b_csp_var_id_t x,
                               int64_t c);

/** @brief Post X in D (domain). */
bool n00b_logic_csp_in(n00b_logic_t *prog, n00b_csp_var_id_t x,
                         n00b_csp_domain_t dom);

/** @brief Push CSP state (choice point). */
void n00b_logic_csp_push(n00b_logic_t *prog);

/** @brief Pop CSP state (backtrack). */
void n00b_logic_csp_pop(n00b_logic_t *prog);

// ============================================================================
// Bridge: Datalog -> CSP
// ============================================================================

/**
 * @brief Create CSP variables from distinct symbols in a relation column.
 *
 * For each distinct symbol in `rel[col]`, creates a CSP variable
 * named after the symbol with the given domain.  Skips symbols
 * that already have CSP variables.
 *
 * @param prog   Program.
 * @param rel    Relation ID.
 * @param col    Column index.
 * @param domain Domain for new variables.
 * @return       Number of variables created.
 *
 * @pre Datalog has been run.
 */
int32_t n00b_logic_vars_from_rel(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                                   int32_t col, n00b_csp_domain_t domain);

/**
 * @brief Post one constraint per tuple in a binary relation.
 *
 * For each (A, B) tuple in `rel`, posts `con_kind(csp_var(A), csp_var(B))`.
 *
 * @param prog     Program.
 * @param rel      Binary relation ID.
 * @param con_kind Constraint kind (N00B_CSP_CON_EQ, N00B_CSP_CON_NE, etc.).
 * @return         false if any constraint is immediately unsatisfiable.
 *
 * @pre Datalog has been run and both columns have been bridged.
 */
bool n00b_logic_constrain_pairs(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                                  n00b_csp_con_kind_t con_kind);

/**
 * @brief Look up the CSP variable for a Datalog symbol.
 *
 * @param prog Program.
 * @param sym  Datalog symbol.
 * @return     Option containing CSP variable ID, or none.
 */
n00b_option_t(n00b_csp_var_id_t) n00b_logic_csp_find(n00b_logic_t *prog,
                                                        n00b_dl_sym_t sym);

// ============================================================================
// Execution
// ============================================================================

/**
 * @brief Run the Datalog engine.
 * @return false if stratification failed.
 */
bool n00b_logic_run_datalog(n00b_logic_t *prog);

/**
 * @brief Run CSP propagation.
 * @return false if unsatisfiable.
 */
bool n00b_logic_run_csp(n00b_logic_t *prog);

/**
 * @brief Run Datalog then CSP in sequence.
 * @return false if either fails.
 */
bool n00b_logic_run(n00b_logic_t *prog);

// ============================================================================
// Solving (Datalog + CSP propagation + labeling)
// ============================================================================

/**
 * @brief Callback invoked for each solution found by n00b_logic_solve_all().
 *
 * @param prog Program in solved state.
 * @param ctx  User context.
 * @return     true to continue, false to stop.
 */
typedef bool (*n00b_logic_solution_cb)(n00b_logic_t *prog, void *ctx);

/**
 * @brief Run the full pipeline: Datalog + CSP propagation + labeling.
 *
 * Finds the first complete assignment.  On success the CSP store is
 * left with all variables ground.
 *
 * @return true if a solution was found.
 */
bool n00b_logic_solve(n00b_logic_t *prog);

/**
 * @brief Enumerate all solutions via the full pipeline.
 *
 * Runs Datalog + CSP propagation, then labels all solutions.
 * Calls @p cb for each solution.  The store is restored to its
 * post-propagation state when done.
 *
 * @param prog Program.
 * @param cb   Callback per solution (may be nullptr to just count).
 * @param ctx  User context.
 * @return     Number of solutions found.
 */
int64_t n00b_logic_solve_all(n00b_logic_t *prog, n00b_logic_solution_cb cb,
                               void *ctx);

// ============================================================================
// Query
// ============================================================================

/** @brief Iterate tuples via callback. */
void n00b_logic_query(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                        n00b_dl_query_cb cb, void *ctx);

/** @brief Count tuples in a relation. */
size_t n00b_logic_count(n00b_logic_t *prog, n00b_dl_rel_id_t rel);

/** @brief Get a CSP variable's current domain. */
n00b_result_t(const n00b_csp_domain_t *) n00b_logic_csp_domain(
    n00b_logic_t *prog, n00b_csp_var_id_t var);

/** @brief Test whether a CSP variable is ground. */
n00b_result_t(bool) n00b_logic_csp_is_ground(n00b_logic_t *prog,
                                               n00b_csp_var_id_t var);

/** @brief Get the value of a ground CSP variable. */
n00b_result_t(int64_t) n00b_logic_csp_value(n00b_logic_t *prog,
                                              n00b_csp_var_id_t var);

// ============================================================================
// Ergonomic API (ncc extensions: variadic `+` and `_kargs`)
// ============================================================================

/**
 * @brief Add a ground fact using variadic `n00b_string_t *` arguments.
 *
 * The relation is created or looked up by name. Each variadic
 * argument is a `n00b_string_t *` constant symbol name that is
 * automatically interned. The arity is inferred from the
 * number of variadic arguments.
 *
 * ```c
 * n00b_logic_fact(prog, STR("edge"), &STR("a"), &STR("b"));
 * ```
 *
 * @param prog Program.
 * @param rel  Relation name.
 */
void n00b_logic_fact(n00b_logic_t *prog, n00b_string_t *rel, +);

/**
 * @brief Bridge a Datalog relation into the CSP.
 *
 * Auto-runs Datalog if needed. For each column in the relation,
 * creates CSP variables from distinct symbols with the given domain.
 * If `constraint` is not -1, posts that constraint for every tuple
 * pair in the relation.
 *
 * ```c
 * n00b_logic_bridge(prog, STR("edge"),
 *     .domain     = n00b_csp_dom_range(1, 3),
 *     .constraint = N00B_CSP_CON_NE);
 * ```
 *
 * @param prog Program.
 * @param rel  Relation name.
 * @return     Total CSP variables created across all columns.
 */
extern int32_t
n00b_logic_bridge(n00b_logic_t *prog, n00b_string_t *rel) _kargs
{
    n00b_csp_domain_t   domain;
    n00b_csp_con_kind_t constraint = -1;
};

/**
 * @brief Create a named CSP variable with a range domain.
 *
 * Shorthand for `n00b_logic_csp_var(prog, name, n00b_csp_dom_range(lo, hi))`.
 *
 * @param prog Program.
 * @param name Variable name.
 * @param lo   Domain lower bound (inclusive).
 * @param hi   Domain upper bound (inclusive).
 * @return     Variable ID.
 */
n00b_csp_var_id_t n00b_logic_int_var(n00b_logic_t *prog, n00b_string_t *name,
                                       int64_t lo, int64_t hi);

/**
 * @brief Post a constraint between two named CSP variables.
 *
 * Looks up both variables by name in the CSP bridge maps.
 *
 * @param prog  Program.
 * @param var_a First variable name.
 * @param var_b Second variable name.
 * @param kind  Constraint kind.
 * @return      false if the constraint is immediately unsatisfiable,
 *              or if either variable is not found.
 */
bool n00b_logic_constrain(n00b_logic_t *prog,
                            n00b_string_t *var_a, n00b_string_t *var_b,
                            n00b_csp_con_kind_t kind);

/**
 * @brief Get the integer value of a named CSP variable.
 *
 * Chains the CSP variable lookup and value extraction.
 * Returns `ENOENT` if the variable is not found, `EINVAL` if
 * not ground.
 *
 * @param prog Program (must be solved).
 * @param name Variable name.
 * @return     Result containing the integer value.
 */
n00b_result_t(int64_t) n00b_logic_get_int(n00b_logic_t *prog,
                                            n00b_string_t *name);

/**
 * @brief Term in a linear constraint: `coeff * variable`.
 */
typedef struct {
    int64_t        coeff;
    n00b_string_t *name;
} n00b_linear_term_t;

/**
 * @brief Post an all-different constraint on named CSP variables.
 *
 * Looks up each variadic `n00b_string_t *` name in `prog->sym_to_csp`
 * and posts a global alldiff constraint (Régin's algorithm).
 *
 * ```c
 * n00b_logic_alldiff(prog, r"x", r"y", r"z");
 * ```
 *
 * @param prog Program.
 * @return     false if any name is not found or immediately unsatisfiable.
 */
bool n00b_logic_alldiff(n00b_logic_t *prog, +);

/**
 * @brief Post a linear constraint: `sum(terms[i].coeff * terms[i].name) == rhs`.
 *
 * Looks up each term's variable name in `prog->sym_to_csp` and posts
 * a linear equality constraint via `n00b_csp_post_linear`.
 *
 * ```c
 * n00b_linear_term_t terms[] = {{2, r"x"}, {3, r"y"}};
 * n00b_logic_linear(prog, terms, 2, .rhs = 12);
 * ```
 *
 * @param prog  Program.
 * @param terms Array of coefficient/name pairs.
 * @param n     Number of terms.
 * @return      false if any name is not found or immediately unsatisfiable.
 */
extern bool n00b_logic_linear(n00b_logic_t *prog,
                                const n00b_linear_term_t *terms,
                                int32_t n) _kargs { int64_t rhs = 0; };
