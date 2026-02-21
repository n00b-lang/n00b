#include "logic/clpfd_label.h"

// ---------------------------------------------------------------------------
// MRV variable selection
// ---------------------------------------------------------------------------

// Returns the ID of the unground variable with the smallest domain,
// or -1 if all variables are ground.
static n00b_csp_var_id_t
select_mrv(n00b_csp_store_t *s)
{
    int32_t           n    = n00b_csp_var_count(s);
    n00b_csp_var_id_t best = -1;
    int64_t           best_size = INT64_MAX;

    for (int32_t i = 0; i < n; i++) {
        n00b_result_t(bool) gr = n00b_csp_var_is_ground(s, i);
        if (n00b_result_get(gr)) {
            continue;
        }

        n00b_result_t(const n00b_csp_domain_t *) dr = n00b_csp_var_domain(s, i);
        const n00b_csp_domain_t *dom = n00b_result_get(dr);
        int64_t sz = n00b_csp_dom_size(dom);

        if (sz < best_size) {
            best_size = sz;
            best      = i;
        }
    }

    return best;
}

// ---------------------------------------------------------------------------
// Domain value collection (for safe iteration during backtracking)
// ---------------------------------------------------------------------------

typedef struct {
    int64_t *values;
    int32_t  count;
    int32_t  cap;
} collect_values_ctx_t;

static bool
collect_value_cb(int64_t value, void *ctx)
{
    collect_values_ctx_t *c = (collect_values_ctx_t *)ctx;

    if (c->count >= c->cap) {
        return false;
    }

    c->values[c->count++] = value;
    return true;
}

// ---------------------------------------------------------------------------
// Recursive labeling (find-first)
// ---------------------------------------------------------------------------

static bool
label_recursive(n00b_csp_store_t *s)
{
    n00b_csp_var_id_t var = select_mrv(s);

    if (var < 0) {
        // All variables are ground — solution found.
        return true;
    }

    // Snapshot the domain values before trying assignments.
    // We need a copy because push/pop will modify the domain.
    n00b_result_t(const n00b_csp_domain_t *) dr = n00b_csp_var_domain(s, var);
    const n00b_csp_domain_t *dom = n00b_result_get(dr);
    int64_t sz = n00b_csp_dom_size(dom);

    if (sz <= 0) {
        return false;
    }

    // Collect values into a local buffer.
    int64_t              buf[sz > 64 ? 64 : sz];
    collect_values_ctx_t cctx = {
        .values = buf,
        .count  = 0,
        .cap    = (int32_t)(sz > 64 ? 64 : sz),
    };
    n00b_csp_dom_iterate(s, var, collect_value_cb, &cctx);

    for (int32_t i = 0; i < cctx.count; i++) {
        n00b_csp_push_state(s);

        if (n00b_csp_post_eq_const(s, var, buf[i])) {
            if (label_recursive(s)) {
                return true;  // Leave store in solved state.
            }
        }

        n00b_csp_pop_state(s);
    }

    return false;  // Exhausted all values.
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool
n00b_csp_label(n00b_csp_store_t *s)
{
    return label_recursive(s);
}

// ---------------------------------------------------------------------------
// Enumerate-all labeling
// ---------------------------------------------------------------------------

typedef struct {
    n00b_csp_solution_cb cb;
    void                *ctx;
    int64_t              count;
    bool                 stopped;
} label_all_state_t;

static void
label_all_recursive(n00b_csp_store_t *s, label_all_state_t *state)
{
    if (state->stopped) {
        return;
    }

    n00b_csp_var_id_t var = select_mrv(s);

    if (var < 0) {
        // All variables ground — report solution.
        state->count++;
        if (state->cb && !state->cb(s, state->ctx)) {
            state->stopped = true;
        }
        return;
    }

    n00b_result_t(const n00b_csp_domain_t *) dr = n00b_csp_var_domain(s, var);
    const n00b_csp_domain_t *dom = n00b_result_get(dr);
    int64_t sz = n00b_csp_dom_size(dom);

    if (sz <= 0) {
        return;
    }

    int64_t              buf[sz > 64 ? 64 : sz];
    collect_values_ctx_t cctx = {
        .values = buf,
        .count  = 0,
        .cap    = (int32_t)(sz > 64 ? 64 : sz),
    };
    n00b_csp_dom_iterate(s, var, collect_value_cb, &cctx);

    for (int32_t i = 0; i < cctx.count && !state->stopped; i++) {
        n00b_csp_push_state(s);

        if (n00b_csp_post_eq_const(s, var, buf[i])) {
            label_all_recursive(s, state);
        }

        n00b_csp_pop_state(s);
    }
}

int64_t
n00b_csp_label_all(n00b_csp_store_t *s, n00b_csp_solution_cb cb, void *ctx)
{
    label_all_state_t state = {
        .cb      = cb,
        .ctx     = ctx,
        .count   = 0,
        .stopped = false,
    };

    n00b_csp_push_state(s);
    label_all_recursive(s, &state);
    n00b_csp_pop_state(s);

    return state.count;
}
