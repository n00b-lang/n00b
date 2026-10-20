#include "logic/clpfd_label.h"

// ---------------------------------------------------------------------------
// MRV variable selection
// ---------------------------------------------------------------------------

// Returns the ID of the unground variable with the smallest domain,
// or -1 if all variables are ground.
static core_csp_var_id_t
select_mrv(core_csp_store_t *s)
{
    int32_t           n    = core_csp_var_count(s);
    core_csp_var_id_t best = -1;
    int64_t           best_size = INT64_MAX;

    for (int32_t i = 0; i < n; i++) {
        core_csp_bool_result_t gr = core_csp_var_is_ground(s, i);
        if (core_result_get_ok(gr)) {
            continue;
        }

        core_csp_domain_result_t dr = core_csp_var_domain(s, i);
        const core_csp_domain_t *dom = core_result_get_ok(dr);
        int64_t sz = core_csp_dom_size(dom);

        if (sz < best_size) {
            best_size = sz;
            best      = i;
        }
    }

    return best;
}

// ---------------------------------------------------------------------------
// Recursive labeling (find-first)
// ---------------------------------------------------------------------------

typedef struct {
    core_csp_store_t *store;
    core_csp_var_id_t var;
    bool found;
} label_branch_t;

static bool label_recursive(core_csp_store_t *store);

static bool
try_label_value(int64_t value, void *context)
{
    label_branch_t *branch = context;
    core_csp_push_state(branch->store);
    if (core_csp_post_eq_const(branch->store, branch->var, value)
        && label_recursive(branch->store)) {
        branch->found = true;
        return false;
    }
    core_csp_pop_state(branch->store);
    return true;
}

static bool
label_recursive(core_csp_store_t *s)
{
    core_csp_var_id_t var = select_mrv(s);

    if (var < 0) {
        // All variables are ground — solution found.
        return true;
    }

    // Snapshot the domain values before trying assignments.
    // We need a copy because push/pop will modify the domain.
    core_csp_domain_result_t dr = core_csp_var_domain(s, var);
    core_csp_domain_t snapshot =
        core_csp_dom_clone(core_result_get_ok(dr));
    label_branch_t branch = {.store = s, .var = var};
    core_csp_dom_visit(&snapshot, try_label_value, &branch);
    core_csp_dom_free(&snapshot);
    return branch.found;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool
core_csp_label(core_csp_store_t *s)
{
    return label_recursive(s);
}

// ---------------------------------------------------------------------------
// Enumerate-all labeling
// ---------------------------------------------------------------------------

typedef struct {
    core_csp_solution_cb cb;
    void                *ctx;
    int64_t              count;
    bool                 stopped;
} label_all_state_t;

typedef struct {
    core_csp_store_t *store;
    core_csp_var_id_t var;
    label_all_state_t *state;
} label_all_branch_t;

static void label_all_recursive(core_csp_store_t *store,
                                label_all_state_t *state);

static bool
try_label_all_value(int64_t value, void *context)
{
    label_all_branch_t *branch = context;
    core_csp_push_state(branch->store);
    if (core_csp_post_eq_const(branch->store, branch->var, value))
        label_all_recursive(branch->store, branch->state);
    core_csp_pop_state(branch->store);
    return !branch->state->stopped;
}

static void
label_all_recursive(core_csp_store_t *s, label_all_state_t *state)
{
    if (state->stopped) {
        return;
    }

    core_csp_var_id_t var = select_mrv(s);

    if (var < 0) {
        // All variables ground — report solution.
        state->count++;
        if (state->cb && !state->cb(s, state->ctx)) {
            state->stopped = true;
        }
        return;
    }

    core_csp_domain_result_t dr = core_csp_var_domain(s, var);
    core_csp_domain_t snapshot =
        core_csp_dom_clone(core_result_get_ok(dr));
    label_all_branch_t branch = {.store = s, .var = var, .state = state};
    core_csp_dom_visit(&snapshot, try_label_all_value, &branch);
    core_csp_dom_free(&snapshot);
}

int64_t
core_csp_label_all(core_csp_store_t *s, core_csp_solution_cb cb, void *ctx)
{
    label_all_state_t state = {
        .cb      = cb,
        .ctx     = ctx,
        .count   = 0,
        .stopped = false,
    };

    core_csp_push_state(s);
    label_all_recursive(s, &state);
    core_csp_pop_state(s);

    return state.count;
}
