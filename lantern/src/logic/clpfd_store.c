#include "logic/clpfd_store.h"
#include "lib/alloc.h"

#include <errno.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal structures
// ---------------------------------------------------------------------------

typedef struct {
  int32_t *indices;
  int32_t count;
  int32_t cap;
} csp_index_list_t;

typedef struct {
  core_csp_var_id_t var;
  core_csp_domain_t old_domain;
} csp_trail_entry_t;

// Choice point: records both trail and constraint positions.
typedef struct {
  int32_t trail_mark;
  int32_t con_mark;
} csp_choice_point_t;

struct core_csp_store {
  // Variables
  core_csp_var_t *vars;
  int32_t var_count;
  int32_t var_cap;

  // Constraints
  core_csp_constraint_t *constraints;
  int32_t con_count;
  int32_t con_cap;

  // Watchlist: var_id -> list of constraint indices
  csp_index_list_t *watchlist;
  int32_t watchlist_cap;

  // AC-3 propagation queue (circular buffer)
  int32_t *queue;
  int32_t queue_head;
  int32_t queue_tail;
  int32_t queue_cap;
  bool *in_queue;
  int32_t in_queue_cap;

  // Trail for backtracking
  csp_trail_entry_t *trail;
  int32_t trail_len;
  int32_t trail_cap;

  // Choice point stack
  csp_choice_point_t *choice_points;
  int32_t cp_count;
  int32_t cp_cap;

  bool failed;
  int32_t failed_constraint;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void watchlist_add(core_csp_store_t *s, core_csp_var_id_t var,
                          int32_t con_idx) {
  if (var < 0 || var >= s->watchlist_cap) {
    return;
  }

  csp_index_list_t *wl = &s->watchlist[var];

  if (wl->count >= wl->cap) {
    int32_t new_cap = wl->cap ? wl->cap * 2 : 4;
    int32_t *new_indices = core_alloc_array(int32_t, new_cap);
    if (wl->count > 0) {
      memcpy(new_indices, wl->indices, wl->count * sizeof(int32_t));
    }
    core_free(wl->indices);
    wl->indices = new_indices;
    wl->cap = new_cap;
  }

  wl->indices[wl->count++] = con_idx;
}

// Remove all watchlist entries for constraint con_idx.
static void watchlist_remove(core_csp_store_t *s, int32_t con_idx) {
  core_csp_constraint_t *con = &s->constraints[con_idx];

  for (int32_t v = 0; v < con->var_count; v++) {
    core_csp_var_id_t var = con->vars[v];

    if (var < 0 || var >= s->watchlist_cap) {
      continue;
    }

    csp_index_list_t *wl = &s->watchlist[var];

    for (int32_t i = 0; i < wl->count; i++) {
      if (wl->indices[i] == con_idx) {
        wl->indices[i] = wl->indices[--wl->count];
        break;
      }
    }
  }
}

static void ensure_watchlist(core_csp_store_t *s, int32_t var_count) {
  if (var_count <= s->watchlist_cap) {
    return;
  }

  int32_t old_cap = s->watchlist_cap;
  int32_t new_cap = var_count;
  csp_index_list_t *new_wl = core_alloc_array(csp_index_list_t, new_cap);
  if (old_cap > 0) {
    memcpy(new_wl, s->watchlist, old_cap * sizeof(csp_index_list_t));
  }
  core_free(s->watchlist);
  s->watchlist = new_wl;
  s->watchlist_cap = new_cap;
  memset(&s->watchlist[old_cap], 0,
         (new_cap - old_cap) * sizeof(csp_index_list_t));
}

static void enqueue_constraint(core_csp_store_t *s, int32_t con_idx) {
  if (con_idx < 0 || con_idx >= s->in_queue_cap) {
    int32_t new_cap = con_idx + 16;
    bool *new_in_queue = core_alloc_array(bool, new_cap);
    if (s->in_queue_cap > 0) {
      memcpy(new_in_queue, s->in_queue, s->in_queue_cap * sizeof(bool));
    }
    core_free(s->in_queue);
    s->in_queue = new_in_queue;
    s->in_queue_cap = new_cap;
  }

  if (s->in_queue[con_idx]) {
    return;
  }

  // Grow queue if needed.
  int32_t next_tail = (s->queue_tail + 1) % s->queue_cap;

  if (next_tail == s->queue_head) {
    int32_t new_cap = s->queue_cap * 2;
    int32_t *new_q = core_alloc_array(int32_t, new_cap);
    int32_t count = 0;
    int32_t i = s->queue_head;

    while (i != s->queue_tail) {
      new_q[count++] = s->queue[i];
      i = (i + 1) % s->queue_cap;
    }

    core_free(s->queue);
    s->queue = new_q;
    s->queue_head = 0;
    s->queue_tail = count;
    s->queue_cap = new_cap;
  }

  s->queue[s->queue_tail] = con_idx;
  s->queue_tail = (s->queue_tail + 1) % s->queue_cap;
  s->in_queue[con_idx] = true;
}

static int32_t dequeue_constraint(core_csp_store_t *s) {
  if (s->queue_head == s->queue_tail) {
    return -1;
  }

  int32_t idx = s->queue[s->queue_head];
  s->queue_head = (s->queue_head + 1) % s->queue_cap;
  s->in_queue[idx] = false;

  return idx;
}

static void enqueue_watching(core_csp_store_t *s, core_csp_var_id_t var) {
  if (var < 0 || var >= s->watchlist_cap) {
    return;
  }

  csp_index_list_t *wl = &s->watchlist[var];

  for (int32_t i = 0; i < wl->count; i++) {
    int32_t ci = wl->indices[i];

    if (!s->constraints[ci].entailed && !s->constraints[ci].failed) {
      enqueue_constraint(s, ci);
    }
  }
}

static void trail_save(core_csp_store_t *s, core_csp_var_id_t var) {
  if (s->trail_len >= s->trail_cap) {
    int32_t new_cap = s->trail_cap ? s->trail_cap * 2 : 64;
    csp_trail_entry_t *new_trail = core_alloc_array(csp_trail_entry_t, new_cap);
    if (s->trail_len > 0) {
      memcpy(new_trail, s->trail, s->trail_len * sizeof(csp_trail_entry_t));
    }
    core_free(s->trail);
    s->trail = new_trail;
    s->trail_cap = new_cap;
  }

  s->trail[s->trail_len].var = var;
  s->trail[s->trail_len].old_domain = core_csp_dom_clone(&s->vars[var].domain);
  s->trail_len++;
}

static void update_ground(core_csp_store_t *s, core_csp_var_id_t var) {
  core_csp_var_t *v = &s->vars[var];

  if (core_csp_dom_is_singleton(&v->domain)) {
    v->ground = true;
    v->value = core_csp_dom_min(&v->domain);
  } else {
    v->ground = false;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

core_csp_store_t *core_csp_store_new(void) {
  core_csp_store_t *s = core_alloc(core_csp_store_t);
  s->queue_cap = 64;
  s->queue = core_alloc_array(int32_t, s->queue_cap);
  s->failed_constraint = -1;

  return s;
}

void core_csp_store_free(core_csp_store_t *s) {
  if (!s) {
    return;
  }

  // Free variable names and their owned domains.
  for (int32_t i = 0; i < s->var_count; i++) {
    core_free(s->vars[i].name.data);
    core_csp_dom_free(&s->vars[i].domain);
  }

  core_free(s->vars);

  // Free constraints.
  for (int32_t i = 0; i < s->con_count; i++) {
    core_free(s->constraints[i].vars);
    core_free(s->constraints[i].coeffs);
    core_csp_dom_free(&s->constraints[i].in_domain);
  }

  core_free(s->constraints);

  // Free watchlists.
  for (int32_t i = 0; i < s->watchlist_cap; i++) {
    core_free(s->watchlist[i].indices);
  }

  core_free(s->watchlist);

  // Free queue.
  core_free(s->queue);
  core_free(s->in_queue);

  // Free trail.
  for (int32_t i = 0; i < s->trail_len; i++) {
    core_csp_dom_free(&s->trail[i].old_domain);
  }

  core_free(s->trail);
  core_free(s->choice_points);
  core_free(s);
}

// ---------------------------------------------------------------------------
// Variables
// ---------------------------------------------------------------------------

core_csp_var_id_t core_csp_new_var(core_csp_store_t *s, core_string_t name,
                                   core_csp_domain_t dom) {
  if (s->var_count >= s->var_cap) {
    int32_t new_cap = s->var_cap ? s->var_cap * 2 : 16;
    core_csp_var_t *new_vars = core_alloc_array(core_csp_var_t, new_cap);
    if (s->var_count > 0) {
      memcpy(new_vars, s->vars, s->var_count * sizeof(core_csp_var_t));
    }
    core_free(s->vars);
    s->vars = new_vars;
    s->var_cap = new_cap;
  }

  core_csp_var_id_t id = s->var_count;
  core_csp_var_t *v = &s->vars[s->var_count++];
  v->id = id;
  v->name = core_string_dup(name);
  v->domain = dom;
  v->ground = false;
  v->value = 0;

  update_ground(s, id);
  ensure_watchlist(s, s->var_count);

  return id;
}

core_option_t(core_csp_var_id_t)
    core_csp_find_var(core_csp_store_t *s, core_string_t name) {
  for (int32_t i = 0; i < s->var_count; i++) {
    if (core_string_eq(s->vars[i].name, name)) {
      return core_option_set(core_csp_var_id_t, i);
    }
  }

  return core_option_none(core_csp_var_id_t);
}

core_csp_domain_result_t core_csp_var_domain(core_csp_store_t *s,
                                             core_csp_var_id_t v) {
  if (v < 0 || v >= s->var_count) {
    return core_result_err(core_csp_domain_result_t, EINVAL);
  }

  return core_result_ok(core_csp_domain_result_t, &s->vars[v].domain);
}

core_csp_bool_result_t core_csp_var_is_ground(core_csp_store_t *s,
                                              core_csp_var_id_t v) {
  if (v < 0 || v >= s->var_count) {
    return core_result_err(core_csp_bool_result_t, EINVAL);
  }

  return core_result_ok(core_csp_bool_result_t, s->vars[v].ground);
}

core_csp_int_result_t core_csp_var_value(core_csp_store_t *s,
                                         core_csp_var_id_t v) {
  if (v < 0 || v >= s->var_count) {
    return core_result_err(core_csp_int_result_t, EINVAL);
  }

  return core_result_ok(core_csp_int_result_t, s->vars[v].value);
}

// ---------------------------------------------------------------------------
// Variable queries
// ---------------------------------------------------------------------------

int32_t core_csp_var_count(const core_csp_store_t *s) { return s->var_count; }

int32_t core_csp_dom_iterate(core_csp_store_t *s, core_csp_var_id_t v,
                             core_csp_dom_iter_cb cb, void *ctx) {
  if (v < 0 || v >= s->var_count) {
    return -1;
  }

  const core_csp_domain_t *d = &s->vars[v].domain;
  int32_t count = 0;

  switch (d->tag) {
  case core_csp_domain_tag(core_csp_dom_interval_t): {
    core_csp_dom_interval_t iv =
        core_csp_domain_get(*d, core_csp_dom_interval_t);
    for (int64_t val = iv.lo; val <= iv.hi; val++) {
      count++;
      if (!cb(val, ctx)) {
        return count;
      }
    }
    break;
  }
  case core_csp_domain_tag(core_csp_dom_bitset_t): {
    core_csp_dom_bitset_t bs = core_csp_domain_get(*d, core_csp_dom_bitset_t);
    for (uint64_t bits = bs.bits; bits;) {
      int bit = __builtin_ctzll(bits);
      int64_t val = bs.base + bit;
      count++;
      if (!cb(val, ctx)) {
        return count;
      }
      bits &= bits - 1;
    }
    break;
  }
  case core_csp_domain_tag(core_csp_dom_sparse_t): {
    core_csp_dom_sparse_t sp = core_csp_domain_get(*d, core_csp_dom_sparse_t);
    for (int32_t i = 0; i < sp.count; i++) {
      count++;
      if (!cb(sp.values[i], ctx)) {
        return count;
      }
    }
    break;
  }
  default: // empty
    break;
  }

  return count;
}

// ---------------------------------------------------------------------------
// Constraint posting
// ---------------------------------------------------------------------------

static int32_t add_constraint(core_csp_store_t *s, core_csp_constraint_t con) {
  if (s->con_count >= s->con_cap) {
    int32_t new_cap = s->con_cap ? s->con_cap * 2 : 16;
    core_csp_constraint_t *new_cons =
        core_alloc_array(core_csp_constraint_t, new_cap);
    if (s->con_count > 0) {
      memcpy(new_cons, s->constraints,
             s->con_count * sizeof(core_csp_constraint_t));
    }
    core_free(s->constraints);
    s->constraints = new_cons;
    s->con_cap = new_cap;
  }

  int32_t idx = s->con_count;
  s->constraints[s->con_count++] = con;

  // Add to watchlists.
  for (int32_t i = 0; i < con.var_count; i++) {
    watchlist_add(s, con.vars[i], idx);
  }

  // Enqueue for initial propagation.
  enqueue_constraint(s, idx);

  return idx;
}

bool core_csp_post_eq(core_csp_store_t *s, core_csp_var_id_t x,
                      core_csp_var_id_t y) {
  return core_csp_post_eq_with_provenance(s, x, y, nullptr);
}

bool core_csp_post_eq_with_provenance(core_csp_store_t *s, core_csp_var_id_t x,
                                      core_csp_var_id_t y, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_EQ;
  con.var_count = 2;
  con.vars = core_alloc_array(core_csp_var_id_t, 2);
  con.vars[0] = x;
  con.vars[1] = y;
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_eq_const(core_csp_store_t *s, core_csp_var_id_t x,
                            int64_t c) {
  return core_csp_post_eq_const_with_provenance(s, x, c, nullptr);
}

bool core_csp_post_eq_const_with_provenance(core_csp_store_t *s,
                                            core_csp_var_id_t x, int64_t c,
                                            void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_EQ_CONST;
  con.var_count = 1;
  con.vars = core_alloc_array(core_csp_var_id_t, 1);
  con.vars[0] = x;
  con.constant = c;
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_ne(core_csp_store_t *s, core_csp_var_id_t x,
                      core_csp_var_id_t y) {
  return core_csp_post_ne_with_provenance(s, x, y, nullptr);
}

bool core_csp_post_ne_with_provenance(core_csp_store_t *s, core_csp_var_id_t x,
                                      core_csp_var_id_t y, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_NE;
  con.var_count = 2;
  con.vars = core_alloc_array(core_csp_var_id_t, 2);
  con.vars[0] = x;
  con.vars[1] = y;
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_lt(core_csp_store_t *s, core_csp_var_id_t x,
                      core_csp_var_id_t y) {
  return core_csp_post_lt_with_provenance(s, x, y, nullptr);
}

bool core_csp_post_lt_with_provenance(core_csp_store_t *s, core_csp_var_id_t x,
                                      core_csp_var_id_t y, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_LT;
  con.var_count = 2;
  con.vars = core_alloc_array(core_csp_var_id_t, 2);
  con.vars[0] = x;
  con.vars[1] = y;
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_le(core_csp_store_t *s, core_csp_var_id_t x,
                      core_csp_var_id_t y) {
  return core_csp_post_le_with_provenance(s, x, y, nullptr);
}

bool core_csp_post_le_with_provenance(core_csp_store_t *s, core_csp_var_id_t x,
                                      core_csp_var_id_t y, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_LE;
  con.var_count = 2;
  con.vars = core_alloc_array(core_csp_var_id_t, 2);
  con.vars[0] = x;
  con.vars[1] = y;
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_in(core_csp_store_t *s, core_csp_var_id_t x,
                      core_csp_domain_t dom) {
  return core_csp_post_in_with_provenance(s, x, dom, nullptr);
}

bool core_csp_post_in_with_provenance(core_csp_store_t *s, core_csp_var_id_t x,
                                      core_csp_domain_t dom, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_IN;
  con.var_count = 1;
  con.vars = core_alloc_array(core_csp_var_id_t, 1);
  con.vars[0] = x;
  con.in_domain = dom; // Takes ownership.
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_linear(core_csp_store_t *s, const core_csp_var_id_t *vars,
                          const int64_t *coeffs, int32_t n, int64_t rhs) {
  return core_csp_post_linear_with_provenance(s, vars, coeffs, n, rhs, nullptr);
}

bool core_csp_post_linear_with_provenance(core_csp_store_t *s,
                                          const core_csp_var_id_t *vars,
                                          const int64_t *coeffs, int32_t n,
                                          int64_t rhs, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_LINEAR;
  con.var_count = n;
  con.vars = core_alloc_array(core_csp_var_id_t, n);
  con.coeffs = core_alloc_array(int64_t, n);
  memcpy(con.vars, vars, n * sizeof(core_csp_var_id_t));
  memcpy(con.coeffs, coeffs, n * sizeof(int64_t));
  con.constant = rhs;
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

bool core_csp_post_alldiff(core_csp_store_t *s, const core_csp_var_id_t *vars,
                           int32_t n) {
  return core_csp_post_alldiff_with_provenance(s, vars, n, nullptr);
}

bool core_csp_post_alldiff_with_provenance(core_csp_store_t *s,
                                           const core_csp_var_id_t *vars,
                                           int32_t n, void *provenance) {
  core_csp_constraint_t con = {};
  con.kind = CORE_CSP_CON_ALLDIFF;
  con.var_count = n;
  con.vars = core_alloc_array(core_csp_var_id_t, n);
  memcpy(con.vars, vars, n * sizeof(core_csp_var_id_t));
  con.provenance = provenance;

  add_constraint(s, con);

  return core_csp_propagate(s);
}

// ---------------------------------------------------------------------------
// Backtracking
// ---------------------------------------------------------------------------

void core_csp_push_state(core_csp_store_t *s) {
  if (s->cp_count >= s->cp_cap) {
    int32_t new_cap = s->cp_cap ? s->cp_cap * 2 : 16;
    csp_choice_point_t *new_cps = core_alloc_array(csp_choice_point_t, new_cap);
    if (s->cp_count > 0) {
      memcpy(new_cps, s->choice_points,
             s->cp_count * sizeof(csp_choice_point_t));
    }
    core_free(s->choice_points);
    s->choice_points = new_cps;
    s->cp_cap = new_cap;
  }

  s->choice_points[s->cp_count].trail_mark = s->trail_len;
  s->choice_points[s->cp_count].con_mark = s->con_count;
  s->cp_count++;
}

void core_csp_pop_state(core_csp_store_t *s) {
  if (s->cp_count == 0) {
    return;
  }

  csp_choice_point_t cp = s->choice_points[--s->cp_count];

  // Undo trail entries in reverse order.
  for (int32_t i = s->trail_len - 1; i >= cp.trail_mark; i--) {
    core_csp_var_id_t var = s->trail[i].var;
    core_csp_dom_free(&s->vars[var].domain);
    s->vars[var].domain = s->trail[i].old_domain;
    // Don't free trail entry's domain — it was moved into the variable.
    update_ground(s, var);
  }

  s->trail_len = cp.trail_mark;

  // Remove constraints added since the choice point.
  for (int32_t i = s->con_count - 1; i >= cp.con_mark; i--) {
    watchlist_remove(s, i);
    core_free(s->constraints[i].vars);
    core_free(s->constraints[i].coeffs);
    core_csp_dom_free(&s->constraints[i].in_domain);
  }

  s->con_count = cp.con_mark;
  s->failed = false;
  s->failed_constraint = -1;

  // Clear the propagation queue.
  s->queue_head = 0;
  s->queue_tail = 0;

  if (s->in_queue) {
    memset(s->in_queue, 0, s->in_queue_cap * sizeof(bool));
  }

  // Reset entailment and failed flags for all surviving constraints.
  for (int32_t i = 0; i < s->con_count; i++) {
    s->constraints[i].entailed = false;
    s->constraints[i].failed = false;
  }
}

// ---------------------------------------------------------------------------
// Propagation
// ---------------------------------------------------------------------------

static bool propagate_eq(core_csp_store_t *s, core_csp_constraint_t *con) {
  core_csp_var_id_t x = con->vars[0];
  core_csp_var_id_t y = con->vars[1];

  // Intersect X with Y's domain.
  core_csp_domain_t y_clone = core_csp_dom_clone(&s->vars[y].domain);
  trail_save(s, x);

  if (core_csp_dom_intersect(&s->vars[x].domain, &y_clone)) {
    update_ground(s, x);
    enqueue_watching(s, x);
  }

  core_csp_dom_free(&y_clone);

  // Intersect Y with X's domain.
  core_csp_domain_t x_clone = core_csp_dom_clone(&s->vars[x].domain);
  trail_save(s, y);

  if (core_csp_dom_intersect(&s->vars[y].domain, &x_clone)) {
    update_ground(s, y);
    enqueue_watching(s, y);
  }

  core_csp_dom_free(&x_clone);

  if (core_csp_dom_is_empty(&s->vars[x].domain) ||
      core_csp_dom_is_empty(&s->vars[y].domain)) {
    con->failed = true;
    s->failed = true;
    return false;
  }

  // Check entailment: both ground and equal.
  if (s->vars[x].ground && s->vars[y].ground &&
      s->vars[x].value == s->vars[y].value) {
    con->entailed = true;
  }

  return true;
}

static bool propagate_eq_const(core_csp_store_t *s,
                               core_csp_constraint_t *con) {
  core_csp_var_id_t x = con->vars[0];
  int64_t c = con->constant;

  if (!core_csp_dom_contains(&s->vars[x].domain, c)) {
    con->failed = true;
    s->failed = true;
    return false;
  }

  if (s->vars[x].ground && s->vars[x].value == c) {
    con->entailed = true;
    return true;
  }

  trail_save(s, x);
  core_csp_domain_t singleton = core_csp_dom_singleton(c);
  core_csp_dom_intersect(&s->vars[x].domain, &singleton);
  update_ground(s, x);
  enqueue_watching(s, x);

  return true;
}

static bool propagate_ne(core_csp_store_t *s, core_csp_constraint_t *con) {
  core_csp_var_id_t x = con->vars[0];
  core_csp_var_id_t y = con->vars[1];

  // If X is ground, remove X's value from Y.
  if (s->vars[x].ground) {
    if (core_csp_dom_contains(&s->vars[y].domain, s->vars[x].value)) {
      trail_save(s, y);
      core_csp_dom_remove_value(&s->vars[y].domain, s->vars[x].value);
      update_ground(s, y);
      enqueue_watching(s, y);

      if (core_csp_dom_is_empty(&s->vars[y].domain)) {
        con->failed = true;
        s->failed = true;
        return false;
      }
    }
  }

  // If Y is ground, remove Y's value from X.
  if (s->vars[y].ground) {
    if (core_csp_dom_contains(&s->vars[x].domain, s->vars[y].value)) {
      trail_save(s, x);
      core_csp_dom_remove_value(&s->vars[x].domain, s->vars[y].value);
      update_ground(s, x);
      enqueue_watching(s, x);

      if (core_csp_dom_is_empty(&s->vars[x].domain)) {
        con->failed = true;
        s->failed = true;
        return false;
      }
    }
  }

  // Entailed if both ground and not equal.
  if (s->vars[x].ground && s->vars[y].ground &&
      s->vars[x].value != s->vars[y].value) {
    con->entailed = true;
  }

  return true;
}

static bool propagate_lt(core_csp_store_t *s, core_csp_constraint_t *con) {
  core_csp_var_id_t x = con->vars[0];
  core_csp_var_id_t y = con->vars[1];

  // X.max < Y.max (so X.max <= Y.max - 1)
  int64_t y_max = core_csp_dom_max(&s->vars[y].domain);
  trail_save(s, x);

  if (core_csp_dom_restrict_max(&s->vars[x].domain, y_max - 1)) {
    update_ground(s, x);
    enqueue_watching(s, x);
  }

  // Y.min > X.min (so Y.min >= X.min + 1)
  int64_t x_min = core_csp_dom_min(&s->vars[x].domain);
  trail_save(s, y);

  if (core_csp_dom_restrict_min(&s->vars[y].domain, x_min + 1)) {
    update_ground(s, y);
    enqueue_watching(s, y);
  }

  if (core_csp_dom_is_empty(&s->vars[x].domain) ||
      core_csp_dom_is_empty(&s->vars[y].domain)) {
    con->failed = true;
    s->failed = true;
    return false;
  }

  // Entailed if X.max < Y.min.
  if (core_csp_dom_max(&s->vars[x].domain) <
      core_csp_dom_min(&s->vars[y].domain)) {
    con->entailed = true;
  }

  return true;
}

static bool propagate_le(core_csp_store_t *s, core_csp_constraint_t *con) {
  core_csp_var_id_t x = con->vars[0];
  core_csp_var_id_t y = con->vars[1];

  // X.max <= Y.max
  int64_t y_max = core_csp_dom_max(&s->vars[y].domain);
  trail_save(s, x);

  if (core_csp_dom_restrict_max(&s->vars[x].domain, y_max)) {
    update_ground(s, x);
    enqueue_watching(s, x);
  }

  // Y.min >= X.min
  int64_t x_min = core_csp_dom_min(&s->vars[x].domain);
  trail_save(s, y);

  if (core_csp_dom_restrict_min(&s->vars[y].domain, x_min)) {
    update_ground(s, y);
    enqueue_watching(s, y);
  }

  if (core_csp_dom_is_empty(&s->vars[x].domain) ||
      core_csp_dom_is_empty(&s->vars[y].domain)) {
    con->failed = true;
    s->failed = true;
    return false;
  }

  if (core_csp_dom_max(&s->vars[x].domain) <=
      core_csp_dom_min(&s->vars[y].domain)) {
    con->entailed = true;
  }

  return true;
}

static bool propagate_in(core_csp_store_t *s, core_csp_constraint_t *con) {
  core_csp_var_id_t x = con->vars[0];

  trail_save(s, x);

  if (core_csp_dom_intersect(&s->vars[x].domain, &con->in_domain)) {
    update_ground(s, x);
    enqueue_watching(s, x);
  }

  if (core_csp_dom_is_empty(&s->vars[x].domain)) {
    con->failed = true;
    s->failed = true;
    return false;
  }

  con->entailed = true; // IN constraint only needs to fire once.

  return true;
}

// ---------------------------------------------------------------------------
// LINEAR propagator: a1*X1 + a2*X2 + ... = c
//
// Bounds propagation: for each Xi, compute the feasible range of Xi
// from the bounds of all other variables, then narrow Xi's domain.
// ---------------------------------------------------------------------------

typedef __int128 csp_i128_t;
typedef unsigned __int128 csp_u128_t;

typedef struct {
  uint64_t limbs[3];
} csp_u192_t;

static csp_i128_t csp_floor_div(csp_i128_t numerator,
                                csp_i128_t denominator) {
  csp_i128_t quotient = numerator / denominator;
  csp_i128_t remainder = numerator % denominator;
  return remainder < 0 ? quotient - 1 : quotient;
}

static csp_i128_t csp_ceil_div(csp_i128_t numerator,
                               csp_i128_t denominator) {
  csp_i128_t quotient = numerator / denominator;
  csp_i128_t remainder = numerator % denominator;
  return remainder > 0 ? quotient + 1 : quotient;
}

static void csp_u192_add_u128(csp_u192_t *sum, csp_u128_t value) {
  uint64_t low = (uint64_t)value;
  uint64_t high = (uint64_t)(value >> 64);
  uint64_t old_low = sum->limbs[0];
  sum->limbs[0] += low;
  csp_u128_t middle = (csp_u128_t)sum->limbs[1] + high +
                      (sum->limbs[0] < old_low);
  sum->limbs[1] = (uint64_t)middle;
  sum->limbs[2] += (uint64_t)(middle >> 64);
}

static int csp_u192_compare(const csp_u192_t *left,
                            const csp_u192_t *right) {
  for (size_t i = 3; i > 0; i--) {
    if (left->limbs[i - 1] != right->limbs[i - 1])
      return left->limbs[i - 1] < right->limbs[i - 1] ? -1 : 1;
  }
  return 0;
}

static bool linear_ground_holds(const core_csp_store_t *s,
                                const core_csp_constraint_t *con) {
  csp_u192_t positive = {{0}}, negative = {{0}};
  for (int32_t i = 0; i < con->var_count; i++) {
    csp_i128_t product =
        (csp_i128_t)con->coeffs[i] *
        s->vars[con->vars[i]].value;
    csp_u128_t magnitude = product < 0
                               ? (csp_u128_t)(-(product + 1)) + 1
                               : (csp_u128_t)product;
    csp_u192_add_u128(product < 0 ? &negative : &positive, magnitude);
  }
  if (con->constant >= 0) {
    csp_u192_add_u128(&negative, (uint64_t)con->constant);
  } else {
    uint64_t magnitude = 0 - (uint64_t)con->constant;
    csp_u192_add_u128(&positive, magnitude);
  }
  return csp_u192_compare(&positive, &negative) == 0;
}

static bool linear_bounds(const core_csp_store_t *s,
                          const core_csp_constraint_t *con,
                          csp_i128_t *sum_lo, csp_i128_t *sum_hi) {
  *sum_lo = 0;
  *sum_hi = 0;
  for (int32_t i = 0; i < con->var_count; i++) {
    const core_csp_domain_t *domain = &s->vars[con->vars[i]].domain;
    if (core_csp_dom_is_empty(domain))
      return false;
    int64_t lo = core_csp_dom_min(domain);
    int64_t hi = core_csp_dom_max(domain);
    csp_i128_t low_product = (csp_i128_t)con->coeffs[i] *
                             (con->coeffs[i] > 0 ? lo : hi);
    csp_i128_t high_product = (csp_i128_t)con->coeffs[i] *
                              (con->coeffs[i] > 0 ? hi : lo);
    csp_i128_t next;
    if (__builtin_add_overflow(*sum_lo, low_product, &next))
      return false;
    *sum_lo = next;
    if (__builtin_add_overflow(*sum_hi, high_product, &next))
      return false;
    *sum_hi = next;
  }
  return true;
}

static bool propagate_linear(core_csp_store_t *s, core_csp_constraint_t *con) {
  int32_t n = con->var_count;
  csp_i128_t sum_lo = 0;
  csp_i128_t sum_hi = 0;
  bool all_ground = true;
  for (int32_t i = 0; i < n; i++) {
    if (core_csp_dom_is_empty(&s->vars[con->vars[i]].domain)) {
      con->failed = true;
      s->failed = true;
      return false;
    }
    all_ground &= s->vars[con->vars[i]].ground;
  }

  bool safe_bounds = linear_bounds(s, con, &sum_lo, &sum_hi);
  if (safe_bounds) {
    if (con->constant < sum_lo || con->constant > sum_hi) {
      con->failed = true;
      s->failed = true;
      return false;
    }

    for (int32_t i = 0; i < n; i++) {
      int64_t coefficient = con->coeffs[i];
      if (!coefficient) continue;
      core_csp_var_id_t variable = con->vars[i];
      const core_csp_domain_t *domain = &s->vars[variable].domain;
      int64_t var_lo = core_csp_dom_min(domain);
      int64_t var_hi = core_csp_dom_max(domain);
      csp_i128_t term_lo = (csp_i128_t)coefficient *
                           (coefficient > 0 ? var_lo : var_hi);
      csp_i128_t term_hi = (csp_i128_t)coefficient *
                           (coefficient > 0 ? var_hi : var_lo);
      csp_i128_t rest_lo, rest_hi, product_lo, product_hi;
      if (__builtin_sub_overflow(sum_lo, term_lo, &rest_lo) ||
          __builtin_sub_overflow(sum_hi, term_hi, &rest_hi) ||
          __builtin_sub_overflow((csp_i128_t)con->constant, rest_hi,
                                 &product_lo) ||
          __builtin_sub_overflow((csp_i128_t)con->constant, rest_lo,
                                 &product_hi))
        continue;
      csp_i128_t new_lo, new_hi;
      if (coefficient > 0) {
        new_lo = csp_ceil_div(product_lo, coefficient);
        new_hi = csp_floor_div(product_hi, coefficient);
      } else {
        csp_i128_t divisor = -(csp_i128_t)coefficient;
        new_lo = csp_ceil_div(-product_hi, divisor);
        new_hi = csp_floor_div(-product_lo, divisor);
      }

      if (new_lo > new_hi || new_lo > INT64_MAX || new_hi < INT64_MIN) {
        con->failed = true;
        s->failed = true;
        return false;
      }
      int64_t narrowed_lo = new_lo < INT64_MIN ? INT64_MIN : (int64_t)new_lo;
      int64_t narrowed_hi = new_hi > INT64_MAX ? INT64_MAX : (int64_t)new_hi;
      if (narrowed_lo > var_lo || narrowed_hi < var_hi) {
        trail_save(s, variable);
        core_csp_dom_restrict_min(&s->vars[variable].domain, narrowed_lo);
        core_csp_dom_restrict_max(&s->vars[variable].domain, narrowed_hi);
        if (core_csp_dom_is_empty(&s->vars[variable].domain)) {
          con->failed = true;
          s->failed = true;
          return false;
        }
        update_ground(s, variable);
        enqueue_watching(s, variable);
        return true;
      }
    }
  }

  if (all_ground) {
    if (linear_ground_holds(s, con)) {
      con->entailed = true;
      return true;
    }
    con->failed = true;
    s->failed = true;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// ALLDIFF propagator: Régin's algorithm
//
// Uses maximum bipartite matching to achieve domain consistency.
// 1. Build bipartite graph: variables ↔ values
// 2. Find maximum matching via augmenting paths
// 3. Identify edges that cannot participate in any maximum matching
// 4. Remove those values from domains
// ---------------------------------------------------------------------------

// Value node indexing: values in the union of all domains are mapped
// to indices 0..num_values-1 via a sorted unique array.

typedef struct {
  // Value mapping
  int64_t *vals; // Sorted unique values from all domains
  int32_t num_vals;

  // Matching: match_var[i] = value index matched to var i, or -1
  //           match_val[j] = var index matched to value j, or -1
  int32_t *match_var;
  int32_t *match_val;

  // Adjacency: for each variable, which value indices are in its domain
  int32_t **adj; // adj[i] = array of value indices
  int32_t *adj_count;

  int32_t num_vars;
} alldiff_graph_t;

static int cmp_int64(const void *a, const void *b) {
  int64_t va = *(const int64_t *)a;
  int64_t vb = *(const int64_t *)b;
  return (va > vb) - (va < vb);
}

static int32_t val_index(const int64_t *vals, int32_t num_vals, int64_t v) {
  // Binary search in sorted vals array.
  int32_t lo = 0, hi = num_vals - 1;

  while (lo <= hi) {
    int32_t mid = lo + (hi - lo) / 2;
    if (vals[mid] == v) {
      return mid;
    }
    if (vals[mid] < v) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }

  return -1;
}

// Augmenting path search from an unmatched variable.
// Returns true if an augmenting path was found (and matching updated).
static bool augment(alldiff_graph_t *g, int32_t var, bool *visited) {
  for (int32_t k = 0; k < g->adj_count[var]; k++) {
    int32_t val_idx = g->adj[var][k];

    if (visited[val_idx]) {
      continue;
    }
    visited[val_idx] = true;

    // If this value is unmatched, or its current match can be rerouted
    if (g->match_val[val_idx] < 0 ||
        augment(g, g->match_val[val_idx], visited)) {
      g->match_var[var] = val_idx;
      g->match_val[val_idx] = var;
      return true;
    }
  }

  return false;
}

// Find maximum matching via Kuhn's algorithm (augmenting paths).
static int32_t max_matching(alldiff_graph_t *g) {
  int32_t matched = 0;

  for (int32_t i = 0; i < g->num_vars; i++) {
    g->match_var[i] = -1;
  }
  for (int32_t j = 0; j < g->num_vals; j++) {
    g->match_val[j] = -1;
  }

  for (int32_t i = 0; i < g->num_vars; i++) {
    bool *visited = core_alloc_array(bool, g->num_vals);
    if (augment(g, i, visited)) {
      matched++;
    }
    core_free(visited);
  }

  return matched;
}

// Tarjan's SCC on the residual graph to find prunable edges.
// In the residual graph:
//   - Matched edges go from value → variable
//   - Unmatched edges go from variable → value
//   - Free (unmatched) values are roots for DFS
// An edge (var, val) can be pruned if it's not in the matching AND
// var and val are in different SCCs of the residual graph.

typedef struct {
  int32_t *stack;
  bool *on_stack;
  int32_t *index;
  int32_t *lowlink;
  int32_t *scc_id;
  int32_t stack_top;
  int32_t next_index;
  int32_t next_scc;
  int32_t total_nodes; // num_vars + num_vals
} scc_state_t;

// Node numbering: variables are 0..num_vars-1, values are
// num_vars..num_vars+num_vals-1
#define VAR_NODE(i) (i)
#define VAL_NODE(j, nv) ((nv) + (j))

static void scc_strongconnect(alldiff_graph_t *g, scc_state_t *st,
                              int32_t node) {
  st->index[node] = st->next_index;
  st->lowlink[node] = st->next_index;
  st->next_index++;
  st->stack[st->stack_top++] = node;
  st->on_stack[node] = true;

  int32_t nv = g->num_vars;

  if (node < nv) {
    // Variable node: edges to unmatched values
    int32_t var = node;
    for (int32_t k = 0; k < g->adj_count[var]; k++) {
      int32_t val_idx = g->adj[var][k];
      if (val_idx == g->match_var[var]) {
        continue; // Skip matched edge (goes other direction)
      }
      int32_t target = VAL_NODE(val_idx, nv);
      if (st->index[target] < 0) {
        scc_strongconnect(g, st, target);
        if (st->lowlink[target] < st->lowlink[node]) {
          st->lowlink[node] = st->lowlink[target];
        }
      } else if (st->on_stack[target]) {
        if (st->index[target] < st->lowlink[node]) {
          st->lowlink[node] = st->index[target];
        }
      }
    }
  } else {
    // Value node: edge to matched variable (if any)
    int32_t val_idx = node - nv;
    int32_t matched_var = g->match_val[val_idx];
    if (matched_var >= 0) {
      int32_t target = VAR_NODE(matched_var);
      if (st->index[target] < 0) {
        scc_strongconnect(g, st, target);
        if (st->lowlink[target] < st->lowlink[node]) {
          st->lowlink[node] = st->lowlink[target];
        }
      } else if (st->on_stack[target]) {
        if (st->index[target] < st->lowlink[node]) {
          st->lowlink[node] = st->index[target];
        }
      }
    }
  }

  // Root of SCC?
  if (st->lowlink[node] == st->index[node]) {
    int32_t scc = st->next_scc++;
    int32_t w;
    do {
      w = st->stack[--st->stack_top];
      st->on_stack[w] = false;
      st->scc_id[w] = scc;
    } while (w != node);
  }
}

static bool propagate_alldiff(core_csp_store_t *s, core_csp_constraint_t *con) {
  int32_t n = con->var_count;

  if (n <= 1) {
    con->entailed = true;
    return true;
  }

  // Matching-based propagation expands domains into individual values. For
  // very broad intervals, defer this constraint until domains are narrowed.
  const int32_t max_expanded_values = 1 << 20;
  int32_t total_vals_cap = 0;
  bool broad_domain = false;

  for (int32_t i = 0; i < n; i++) {
    core_csp_domain_t *domain = &s->vars[con->vars[i]].domain;
    if (core_csp_dom_is_empty(domain)) {
      con->failed = true;
      s->failed = true;
      return false;
    }
    int64_t size = core_csp_dom_size(domain);
    if (size > max_expanded_values - total_vals_cap) {
      broad_domain = true;
    } else if (!broad_domain) {
      total_vals_cap += (int32_t)size;
    }
  }
  if (broad_domain) return true;

  int64_t *all_vals = core_alloc_array(int64_t, total_vals_cap + 1);
  int32_t all_count = 0;

  // Collect callback
  for (int32_t i = 0; i < n; i++) {
    core_csp_domain_t *dom = &s->vars[con->vars[i]].domain;

    switch (dom->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
      core_csp_dom_interval_t iv =
          core_csp_domain_get(*dom, core_csp_dom_interval_t);
      for (int64_t v = iv.lo; v <= iv.hi; v++) {
        all_vals[all_count++] = v;
      }
      break;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
      core_csp_dom_bitset_t bs =
          core_csp_domain_get(*dom, core_csp_dom_bitset_t);
      for (uint64_t bits = bs.bits; bits;) {
        int bit = __builtin_ctzll(bits);
        all_vals[all_count++] = bs.base + bit;
        bits &= bits - 1;
      }
      break;
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
      core_csp_dom_sparse_t sp =
          core_csp_domain_get(*dom, core_csp_dom_sparse_t);
      for (int32_t j = 0; j < sp.count; j++) {
        all_vals[all_count++] = sp.values[j];
      }
      break;
    }
    default: // empty
      con->failed = true;
      s->failed = true;
      core_free(all_vals);
      return false;
    }
  }

  // Sort and deduplicate
  qsort(all_vals, all_count, sizeof(int64_t), cmp_int64);
  int32_t unique_count = 0;
  for (int32_t i = 0; i < all_count; i++) {
    if (i == 0 || all_vals[i] != all_vals[i - 1]) {
      all_vals[unique_count++] = all_vals[i];
    }
  }

  // Build graph
  alldiff_graph_t g;
  g.num_vars = n;
  g.num_vals = unique_count;
  g.vals = all_vals;
  g.match_var = core_alloc_array(int32_t, n);
  g.match_val = core_alloc_array(int32_t, unique_count);
  g.adj = core_alloc_array(int32_t *, n);
  g.adj_count = core_alloc_array(int32_t, n);

  for (int32_t i = 0; i < n; i++) {
    core_csp_domain_t *dom = &s->vars[con->vars[i]].domain;
    int64_t sz = core_csp_dom_size(dom);
    g.adj[i] = core_alloc_array(int32_t, sz);
    g.adj_count[i] = 0;

    switch (dom->tag) {
    case core_csp_domain_tag(core_csp_dom_interval_t): {
      core_csp_dom_interval_t iv =
          core_csp_domain_get(*dom, core_csp_dom_interval_t);
      for (int64_t v = iv.lo; v <= iv.hi; v++) {
        g.adj[i][g.adj_count[i]++] = val_index(all_vals, unique_count, v);
      }
      break;
    }
    case core_csp_domain_tag(core_csp_dom_bitset_t): {
      core_csp_dom_bitset_t bs =
          core_csp_domain_get(*dom, core_csp_dom_bitset_t);
      for (uint64_t bits = bs.bits; bits;) {
        int bit = __builtin_ctzll(bits);
        g.adj[i][g.adj_count[i]++] =
            val_index(all_vals, unique_count, bs.base + bit);
        bits &= bits - 1;
      }
      break;
    }
    case core_csp_domain_tag(core_csp_dom_sparse_t): {
      core_csp_dom_sparse_t sp =
          core_csp_domain_get(*dom, core_csp_dom_sparse_t);
      for (int32_t j = 0; j < sp.count; j++) {
        g.adj[i][g.adj_count[i]++] =
            val_index(all_vals, unique_count, sp.values[j]);
      }
      break;
    }
    default:
      break;
    }
  }

  // Find maximum matching
  int32_t matched = max_matching(&g);

  if (matched < n) {
    // Not enough values to satisfy alldiff → fail
    con->failed = true;
    s->failed = true;
    goto cleanup;
  }

  // Run Tarjan's SCC on residual graph to find prunable edges.
  int32_t total_nodes = n + unique_count;

  scc_state_t st;
  st.stack = core_alloc_array(int32_t, total_nodes);
  st.on_stack = core_alloc_array(bool, total_nodes);
  st.index = core_alloc_array(int32_t, total_nodes);
  st.lowlink = core_alloc_array(int32_t, total_nodes);
  st.scc_id = core_alloc_array(int32_t, total_nodes);
  st.stack_top = 0;
  st.next_index = 0;
  st.next_scc = 0;

  for (int32_t i = 0; i < total_nodes; i++) {
    st.index[i] = -1;
  }

  // Start DFS from free (unmatched) value nodes, then remaining.
  for (int32_t j = 0; j < unique_count; j++) {
    if (g.match_val[j] < 0 && st.index[VAL_NODE(j, n)] < 0) {
      scc_strongconnect(&g, &st, VAL_NODE(j, n));
    }
  }
  for (int32_t i = 0; i < total_nodes; i++) {
    if (st.index[i] < 0) {
      scc_strongconnect(&g, &st, i);
    }
  }

  // Prune: remove (var, val) edges where:
  //   - The edge is NOT in the matching, AND
  //   - var and val are in different SCCs
  for (int32_t i = 0; i < n; i++) {
    core_csp_var_id_t vi = con->vars[i];
    int32_t var_scc = st.scc_id[VAR_NODE(i)];

    for (int32_t k = 0; k < g.adj_count[i]; k++) {
      int32_t val_idx = g.adj[i][k];

      // Skip matched edge
      if (g.match_var[i] == val_idx) {
        continue;
      }

      int32_t val_scc = st.scc_id[VAL_NODE(val_idx, n)];

      if (var_scc != val_scc) {
        // This value can be pruned from this variable's domain.
        trail_save(s, vi);
        core_csp_dom_remove_value(&s->vars[vi].domain, all_vals[val_idx]);

        if (core_csp_dom_is_empty(&s->vars[vi].domain)) {
          con->failed = true;
          s->failed = true;
          // Free SCC state
          core_free(st.stack);
          core_free(st.on_stack);
          core_free(st.index);
          core_free(st.lowlink);
          core_free(st.scc_id);
          goto cleanup;
        }

        update_ground(s, vi);
        enqueue_watching(s, vi);
      }
    }
  }

  // Check entailment: all vars ground with distinct values
  bool all_ground = true;
  for (int32_t i = 0; i < n; i++) {
    if (!s->vars[con->vars[i]].ground) {
      all_ground = false;
      break;
    }
  }
  if (all_ground) {
    con->entailed = true;
  }

  core_free(st.stack);
  core_free(st.on_stack);
  core_free(st.index);
  core_free(st.lowlink);
  core_free(st.scc_id);

cleanup:
  for (int32_t i = 0; i < n; i++) {
    core_free(g.adj[i]);
  }
  core_free(g.adj);
  core_free(g.adj_count);
  core_free(g.match_var);
  core_free(g.match_val);
  core_free(all_vals);

  return !s->failed;
}

bool core_csp_propagate(core_csp_store_t *s) {
  if (s->failed) {
    return false;
  }

  int32_t con_idx;

  while ((con_idx = dequeue_constraint(s)) >= 0) {
    core_csp_constraint_t *con = &s->constraints[con_idx];

    if (con->entailed || con->failed) {
      continue;
    }

    bool ok;

    switch (con->kind) {
    case CORE_CSP_CON_EQ:
      ok = propagate_eq(s, con);
      break;
    case CORE_CSP_CON_EQ_CONST:
      ok = propagate_eq_const(s, con);
      break;
    case CORE_CSP_CON_NE:
      ok = propagate_ne(s, con);
      break;
    case CORE_CSP_CON_LT:
      ok = propagate_lt(s, con);
      break;
    case CORE_CSP_CON_LE:
      ok = propagate_le(s, con);
      break;
    case CORE_CSP_CON_IN:
      ok = propagate_in(s, con);
      break;
    case CORE_CSP_CON_LINEAR:
      ok = propagate_linear(s, con);
      break;
    case CORE_CSP_CON_ALLDIFF:
      ok = propagate_alldiff(s, con);
      break;
    default:
      ok = true;
      break;
    }

    if (!ok) {
      s->failed_constraint = con_idx;
      return false;
    }
  }

  return true;
}

int32_t core_csp_failed_constraint(const core_csp_store_t *s) {
  return s ? s->failed_constraint : -1;
}

void *core_csp_failed_provenance(const core_csp_store_t *s) {
  int32_t index = core_csp_failed_constraint(s);
  return index >= 0 && index < s->con_count ? s->constraints[index].provenance
                                            : nullptr;
}
