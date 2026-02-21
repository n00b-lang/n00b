#include "logic/logic_program.h"
#include "core/alloc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

void
n00b_logic_init(n00b_logic_t *prog)
{
    *prog = (typeof(*prog)){};
    n00b_dl_engine_init(&prog->engine);
    n00b_dl_str_i64_map_init(&prog->sym_to_csp);
    n00b_dl_i64_str_map_init(&prog->csp_to_sym);
    prog->store       = nullptr;
    prog->datalog_ran = false;
    prog->_heap       = false;
}

n00b_logic_t *
n00b_logic_new(void)
{
    n00b_logic_t *prog = n00b_alloc(n00b_logic_t);
    n00b_logic_init(prog);
    prog->_heap = true;
    return prog;
}

void
n00b_logic_free(n00b_logic_t *prog)
{
    n00b_dl_engine_free(&prog->engine);
    n00b_dl_str_i64_map_free(&prog->sym_to_csp);
    n00b_dl_i64_str_map_free(&prog->csp_to_sym);
    if (prog->store) {
        n00b_csp_store_free(prog->store);
        prog->store = nullptr;
    }
    prog->datalog_ran = false;
    if (prog->_heap) {
        n00b_free(prog);
    }
}

// ============================================================================
// Internal: ensure store exists
// ============================================================================

static void
ensure_store(n00b_logic_t *prog)
{
    if (!prog->store) {
        prog->store = n00b_csp_store_new();
    }
}

// ============================================================================
// Datalog wrappers
// ============================================================================

n00b_dl_rel_id_t
n00b_logic_relation(n00b_logic_t *prog, n00b_string_t name, int32_t arity)
{
    return n00b_dl_engine_relation(&prog->engine, name, arity);
}

n00b_dl_sym_t
n00b_logic_const(n00b_logic_t *prog, n00b_string_t name)
{
    return n00b_dl_const(&prog->engine, name);
}

n00b_dl_sym_t
n00b_logic_int(n00b_logic_t *prog, int64_t value)
{
    return n00b_dl_int(&prog->engine, value);
}

n00b_dl_sym_t
n00b_logic_var(n00b_logic_t *prog, n00b_string_t name)
{
    return n00b_dl_var(&prog->engine, name);
}

void
n00b_logic_add_fact(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                     int32_t arity, const n00b_dl_sym_t *args)
{
    n00b_dl_add_fact(&prog->engine, rel, arity, args);
}

void
n00b_logic_add_rule(n00b_logic_t *prog, n00b_dl_rule_t rule)
{
    n00b_dl_add_rule(&prog->engine, rule);
}

// ============================================================================
// CSP wrappers
// ============================================================================

n00b_csp_var_id_t
n00b_logic_csp_var(n00b_logic_t *prog, n00b_string_t name,
                    n00b_csp_domain_t domain)
{
    ensure_store(prog);
    n00b_csp_var_id_t id = n00b_csp_new_var(prog->store, name, domain);

    // Track in bridge maps
    n00b_dl_str_i64_map_put(&prog->sym_to_csp, name, (int64_t)id);
    n00b_dl_i64_str_map_put(&prog->csp_to_sym, (int64_t)id, name);

    return id;
}

bool
n00b_logic_csp_eq(n00b_logic_t *prog, n00b_csp_var_id_t x,
                   n00b_csp_var_id_t y)
{
    ensure_store(prog);
    return n00b_csp_post_eq(prog->store, x, y);
}

bool
n00b_logic_csp_ne(n00b_logic_t *prog, n00b_csp_var_id_t x,
                   n00b_csp_var_id_t y)
{
    ensure_store(prog);
    return n00b_csp_post_ne(prog->store, x, y);
}

bool
n00b_logic_csp_lt(n00b_logic_t *prog, n00b_csp_var_id_t x,
                   n00b_csp_var_id_t y)
{
    ensure_store(prog);
    return n00b_csp_post_lt(prog->store, x, y);
}

bool
n00b_logic_csp_le(n00b_logic_t *prog, n00b_csp_var_id_t x,
                   n00b_csp_var_id_t y)
{
    ensure_store(prog);
    return n00b_csp_post_le(prog->store, x, y);
}

bool
n00b_logic_csp_eq_const(n00b_logic_t *prog, n00b_csp_var_id_t x, int64_t c)
{
    ensure_store(prog);
    return n00b_csp_post_eq_const(prog->store, x, c);
}

bool
n00b_logic_csp_in(n00b_logic_t *prog, n00b_csp_var_id_t x,
                   n00b_csp_domain_t dom)
{
    ensure_store(prog);
    return n00b_csp_post_in(prog->store, x, dom);
}

void
n00b_logic_csp_push(n00b_logic_t *prog)
{
    ensure_store(prog);
    n00b_csp_push_state(prog->store);
}

void
n00b_logic_csp_pop(n00b_logic_t *prog)
{
    if (prog->store) {
        n00b_csp_pop_state(prog->store);
    }
}

// ============================================================================
// Bridge: Datalog -> CSP
// ============================================================================

typedef struct {
    n00b_logic_t      *prog;
    int32_t            col;
    n00b_csp_domain_t  domain;
    int32_t            created;
} vars_from_rel_ctx_t;

static bool
vars_from_rel_cb(const n00b_dl_sym_t *tuple, int32_t arity, void *ctx)
{
    vars_from_rel_ctx_t *c = (vars_from_rel_ctx_t *)ctx;
    (void)arity;

    n00b_dl_sym_t  sym  = tuple[c->col];
    n00b_string_t  name = n00b_dl_sym_to_str(&c->prog->engine, sym);

    // Skip if already bridged
    int64_t *existing = n00b_dl_str_i64_map_get(&c->prog->sym_to_csp, name);
    if (existing) {
        return true;
    }

    ensure_store(c->prog);
    n00b_csp_domain_t dom = n00b_csp_dom_clone(&c->domain);
    n00b_csp_var_id_t id  = n00b_csp_new_var(c->prog->store, name, dom);

    n00b_dl_str_i64_map_put(&c->prog->sym_to_csp, name, (int64_t)id);
    n00b_dl_i64_str_map_put(&c->prog->csp_to_sym, (int64_t)id, name);

    c->created++;
    return true;
}

int32_t
n00b_logic_vars_from_rel(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                           int32_t col, n00b_csp_domain_t domain)
{
    vars_from_rel_ctx_t ctx = {
        .prog    = prog,
        .col     = col,
        .domain  = domain,
        .created = 0,
    };

    n00b_dl_query(&prog->engine, rel, vars_from_rel_cb, &ctx);
    n00b_csp_dom_free(&domain);
    return ctx.created;
}

typedef struct {
    n00b_logic_t        *prog;
    n00b_csp_con_kind_t  con_kind;
    bool                 ok;
} constrain_pairs_ctx_t;

static bool
constrain_pairs_cb(const n00b_dl_sym_t *tuple, int32_t arity, void *ctx)
{
    constrain_pairs_ctx_t *c = (constrain_pairs_ctx_t *)ctx;
    (void)arity;

    n00b_string_t name_a = n00b_dl_sym_to_str(&c->prog->engine, tuple[0]);
    n00b_string_t name_b = n00b_dl_sym_to_str(&c->prog->engine, tuple[1]);

    int64_t *id_a = n00b_dl_str_i64_map_get(&c->prog->sym_to_csp, name_a);
    int64_t *id_b = n00b_dl_str_i64_map_get(&c->prog->sym_to_csp, name_b);

    if (!id_a || !id_b) {
        // Symbol not bridged — skip
        return true;
    }

    n00b_csp_var_id_t va = (n00b_csp_var_id_t)*id_a;
    n00b_csp_var_id_t vb = (n00b_csp_var_id_t)*id_b;

    bool posted = true;
    switch (c->con_kind) {
    case N00B_CSP_CON_EQ:
        posted = n00b_csp_post_eq(c->prog->store, va, vb);
        break;
    case N00B_CSP_CON_NE:
        posted = n00b_csp_post_ne(c->prog->store, va, vb);
        break;
    case N00B_CSP_CON_LT:
        posted = n00b_csp_post_lt(c->prog->store, va, vb);
        break;
    case N00B_CSP_CON_LE:
        posted = n00b_csp_post_le(c->prog->store, va, vb);
        break;
    default:
        break;
    }

    if (!posted) {
        c->ok = false;
        return false; // stop iteration
    }
    return true;
}

bool
n00b_logic_constrain_pairs(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                            n00b_csp_con_kind_t con_kind)
{
    ensure_store(prog);
    constrain_pairs_ctx_t ctx = {
        .prog     = prog,
        .con_kind = con_kind,
        .ok       = true,
    };

    n00b_dl_query(&prog->engine, rel, constrain_pairs_cb, &ctx);
    return ctx.ok;
}

n00b_option_t(n00b_csp_var_id_t)
n00b_logic_csp_find(n00b_logic_t *prog, n00b_dl_sym_t sym)
{
    n00b_string_t name = n00b_dl_sym_to_str(&prog->engine, sym);
    int64_t *id = n00b_dl_str_i64_map_get(&prog->sym_to_csp, name);
    if (id) {
        return n00b_option_set(n00b_csp_var_id_t, (n00b_csp_var_id_t)*id);
    }
    return n00b_option_none(n00b_csp_var_id_t);
}

// ============================================================================
// Execution
// ============================================================================

bool
n00b_logic_run_datalog(n00b_logic_t *prog)
{
    bool ok = n00b_dl_run(&prog->engine);
    if (ok) {
        prog->datalog_ran = true;
    }
    return ok;
}

bool
n00b_logic_run_csp(n00b_logic_t *prog)
{
    if (!prog->store) {
        return true; // No CSP constraints → vacuously satisfied
    }
    return n00b_csp_propagate(prog->store);
}

bool
n00b_logic_run(n00b_logic_t *prog)
{
    if (!n00b_logic_run_datalog(prog)) {
        return false;
    }
    return n00b_logic_run_csp(prog);
}

// ============================================================================
// Solving (Datalog + CSP propagation + labeling)
// ============================================================================

bool
n00b_logic_solve(n00b_logic_t *prog)
{
    if (!n00b_logic_run(prog)) {
        return false;
    }
    if (!prog->store) {
        return true;  // No CSP variables — vacuously solved.
    }
    return n00b_csp_label(prog->store);
}

typedef struct {
    n00b_logic_solution_cb cb;
    n00b_logic_t          *prog;
    void                  *ctx;
} solve_all_adapter_t;

static bool
solve_all_adapter_cb(n00b_csp_store_t *s, void *ctx)
{
    solve_all_adapter_t *a = (solve_all_adapter_t *)ctx;
    (void)s;
    if (a->cb) {
        return a->cb(a->prog, a->ctx);
    }
    return true;
}

int64_t
n00b_logic_solve_all(n00b_logic_t *prog, n00b_logic_solution_cb cb, void *ctx)
{
    if (!n00b_logic_run(prog)) {
        return 0;
    }
    if (!prog->store) {
        // No CSP variables — the Datalog result is the single solution.
        if (cb) {
            cb(prog, ctx);
        }
        return 1;
    }

    solve_all_adapter_t adapter = {
        .cb   = cb,
        .prog = prog,
        .ctx  = ctx,
    };

    return n00b_csp_label_all(prog->store, solve_all_adapter_cb, &adapter);
}

// ============================================================================
// Query
// ============================================================================

void
n00b_logic_query(n00b_logic_t *prog, n00b_dl_rel_id_t rel,
                  n00b_dl_query_cb cb, void *ctx)
{
    n00b_dl_query(&prog->engine, rel, cb, ctx);
}

size_t
n00b_logic_count(n00b_logic_t *prog, n00b_dl_rel_id_t rel)
{
    return n00b_dl_count(&prog->engine, rel);
}

n00b_result_t(const n00b_csp_domain_t *)
n00b_logic_csp_domain(n00b_logic_t *prog, n00b_csp_var_id_t var)
{
    if (!prog->store) {
        return n00b_result_err(const n00b_csp_domain_t *, EINVAL);
    }
    return n00b_csp_var_domain(prog->store, var);
}

n00b_result_t(bool)
n00b_logic_csp_is_ground(n00b_logic_t *prog, n00b_csp_var_id_t var)
{
    if (!prog->store) {
        return n00b_result_err(bool, EINVAL);
    }
    return n00b_csp_var_is_ground(prog->store, var);
}

n00b_result_t(int64_t)
n00b_logic_csp_value(n00b_logic_t *prog, n00b_csp_var_id_t var)
{
    if (!prog->store) {
        return n00b_result_err(int64_t, EINVAL);
    }
    return n00b_csp_var_value(prog->store, var);
}

// ============================================================================
// Ergonomic API
// ============================================================================

void
n00b_logic_fact(n00b_logic_t *prog, n00b_string_t rel, +)
{
    int32_t arity = (int32_t)vargs->nargs;

    // Look up or create the relation.
    n00b_dl_rel_id_t rel_id = n00b_logic_relation(prog, rel, arity);

    // Intern each argument as a constant symbol.
    n00b_dl_sym_t syms[arity];
    for (int32_t i = 0; i < arity; i++) {
        n00b_string_t *name = (n00b_string_t *)n00b_vargs_next(vargs);
        syms[i] = n00b_logic_const(prog, *name);
    }

    n00b_logic_add_fact(prog, rel_id, arity, syms);
}

int32_t
n00b_logic_bridge(n00b_logic_t *prog, n00b_string_t rel) _kargs
{
    n00b_csp_domain_t   domain;
    n00b_csp_con_kind_t constraint = -1;
}
{
    // Auto-run Datalog if needed.
    if (!prog->datalog_ran) {
        if (!n00b_logic_run_datalog(prog)) {
            return 0;
        }
    }

    // Look up the relation by name.
    auto rid_opt = n00b_dl_find_relation(&prog->engine, rel);
    if (!n00b_option_is_set(rid_opt)) {
        return 0;
    }
    n00b_dl_rel_id_t rid = n00b_option_get(rid_opt);

    auto arity_opt = n00b_dl_relation_arity(&prog->engine, rid);
    int32_t arity  = n00b_option_get(arity_opt);
    int32_t created = 0;

    // Create CSP vars for each column.
    for (int32_t col = 0; col < arity; col++) {
        n00b_csp_domain_t col_dom = n00b_csp_dom_clone(&domain);
        created += n00b_logic_vars_from_rel(prog, rid, col, col_dom);
    }

    // Post pairwise constraints if requested.
    if ((int)constraint != -1) {
        n00b_logic_constrain_pairs(prog, rid, constraint);
    }

    n00b_csp_dom_free(&domain);
    return created;
}

n00b_csp_var_id_t
n00b_logic_int_var(n00b_logic_t *prog, n00b_string_t name,
                    int64_t lo, int64_t hi)
{
    return n00b_logic_csp_var(prog, name, n00b_csp_dom_range(lo, hi));
}

bool
n00b_logic_constrain(n00b_logic_t *prog,
                      n00b_string_t var_a, n00b_string_t var_b,
                      n00b_csp_con_kind_t kind)
{
    int64_t *id_a = n00b_dl_str_i64_map_get(&prog->sym_to_csp, var_a);
    int64_t *id_b = n00b_dl_str_i64_map_get(&prog->sym_to_csp, var_b);

    if (!id_a || !id_b) {
        return false;
    }

    n00b_csp_var_id_t va = (n00b_csp_var_id_t)*id_a;
    n00b_csp_var_id_t vb = (n00b_csp_var_id_t)*id_b;

    ensure_store(prog);

    switch (kind) {
    case N00B_CSP_CON_EQ:
        return n00b_csp_post_eq(prog->store, va, vb);
    case N00B_CSP_CON_NE:
        return n00b_csp_post_ne(prog->store, va, vb);
    case N00B_CSP_CON_LT:
        return n00b_csp_post_lt(prog->store, va, vb);
    case N00B_CSP_CON_LE:
        return n00b_csp_post_le(prog->store, va, vb);
    default:
        return false;
    }
}

bool
n00b_logic_alldiff(n00b_logic_t *prog, +)
{
    int32_t n = (int32_t)vargs->nargs;
    n00b_csp_var_id_t vars[n];

    for (int32_t i = 0; i < n; i++) {
        n00b_string_t *name = (n00b_string_t *)n00b_vargs_next(vargs);
        int64_t *id = n00b_dl_str_i64_map_get(&prog->sym_to_csp, *name);
        if (!id) {
            return false;
        }
        vars[i] = (n00b_csp_var_id_t)*id;
    }

    ensure_store(prog);
    return n00b_csp_post_alldiff(prog->store, vars, n);
}

bool
n00b_logic_linear(n00b_logic_t *prog, const n00b_linear_term_t *terms,
                   int32_t n) _kargs { int64_t rhs = 0; }
{
    n00b_csp_var_id_t vars[n];
    int64_t           coeffs[n];

    for (int32_t i = 0; i < n; i++) {
        int64_t *id = n00b_dl_str_i64_map_get(&prog->sym_to_csp, terms[i].name);
        if (!id) {
            return false;
        }
        vars[i]   = (n00b_csp_var_id_t)*id;
        coeffs[i] = terms[i].coeff;
    }

    ensure_store(prog);
    return n00b_csp_post_linear(prog->store, vars, coeffs, n, rhs);
}

n00b_result_t(int64_t)
n00b_logic_get_int(n00b_logic_t *prog, n00b_string_t name)
{
    if (!prog->store) {
        return n00b_result_err(int64_t, ENOENT);
    }

    n00b_option_t(n00b_csp_var_id_t) opt = n00b_csp_find_var(prog->store,
                                                                name);
    if (!n00b_option_is_set(opt)) {
        // Also check the bridge maps.
        int64_t *id = n00b_dl_str_i64_map_get(&prog->sym_to_csp, name);
        if (!id) {
            return n00b_result_err(int64_t, ENOENT);
        }
        return n00b_csp_var_value(prog->store, (n00b_csp_var_id_t)*id);
    }

    return n00b_csp_var_value(prog->store, n00b_option_get(opt));
}
