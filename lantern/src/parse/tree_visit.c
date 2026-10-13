#include "parse/tree_visit.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"

#include <string.h>

struct parse_reduce_state_t;
static void reduce_release(parse_tree_walk_t *w);

typedef struct {
    parse_visit_fn pre;
    parse_visit_fn post;
    void        *thunk;
    int32_t      phase;
} visitor_t;

core_list_decl(visitor_t);

typedef struct {
    parse_walk_stage_fn fn;
    void *thunk;
} stage_visitor_t;

core_list_decl(stage_visitor_t);

typedef struct {
    size_t before_kind;
    parse_walk_stage_fn entering;
    parse_walk_stage_fn exiting;
    void *thunk;
} annotation_boundary_t;

core_list_decl(annotation_boundary_t);

typedef core_list_t(int32_t) chain_t;

typedef struct {
    parse_tree_t *node;
    parse_tree_t *parent;
    int32_t           depth;
    int32_t           child_index;
    int32_t           next_child;
    bool              entered;
    bool              descend;
    bool              skip_callbacks;
    bool              removed;
    uint64_t suppressed[(PARSE_ANNOT_KIND_COUNT + 63) / 64];
} frame_t;

core_list_decl(frame_t);
core_list_decl(parse_tree_ptr_t);

static void fill(parse_tree_walk_t *w, parse_visit_t *v, frame_t *f);

struct parse_tree_walk_t {
    parse_grammar_t          *g;
    void                   *user;
    core_list_t(visitor_t)   visitors;
    chain_t                 any;
    chain_t                 leaf;
    chain_t                 by_annot[PARSE_ANNOT_KIND_COUNT];
    parse_annot_kind_t     *annot_order;
    size_t                  annot_order_len;
    core_list_t(annotation_boundary_t) boundaries;
    core_list_t(stage_visitor_t) stages[PARSE_WALK_CHILD_COMPLETE + 1];
    core_dict_t             *by_nt;
    core_list_t(frame_t)     stack;
    struct parse_reduce_state_t *reduce;
    int32_t                 max_phase;
    bool                    stopped;
    bool                    failed;
    uint32_t                replaced;
    parse_tree_t       *new_root;
    bool                    root_removed;
    core_list_t(parse_tree_ptr_t) detached;
};

static int32_t
add_visitor(parse_tree_walk_t *w, parse_visit_fn pre, parse_visit_fn post,
            void *thunk, int32_t phase)
{
    visitor_t v = {.pre = pre, .post = post, .thunk = thunk, .phase = phase};

    core_list_push(w->visitors, v);

    if (phase > w->max_phase) {
        w->max_phase = phase;
    }

    return (int32_t)(w->visitors.len - 1);
}

parse_tree_walk_t *
parse_tree_walk_new(parse_grammar_t *g, void *user)
{
    parse_tree_walk_t *w = core_alloc(parse_tree_walk_t);

    memset(w, 0, sizeof(*w));

    w->g     = g;
    w->user  = user;
    w->by_nt = core_alloc(core_dict_t);

    core_dict_init(w->by_nt, core_hash_word, nullptr);

    return w;
}

void
parse_tree_walk_free(parse_tree_walk_t *w)
{
    if (!w) {
        return;
    }

    core_dict_foreach (w->by_nt, e) {
        chain_t *c = (chain_t *)e->value;

        core_free(c->data);
        core_free(c);
    }

    core_dict_free(w->by_nt);
    core_free(w->by_nt);

    for (int i = 0; i < PARSE_ANNOT_KIND_COUNT; i++) {
        core_free(w->by_annot[i].data);
    }

    core_free(w->any.data);
    core_free(w->leaf.data);
    core_free(w->visitors.data);
    core_free(w->annot_order);
    core_list_free(w->boundaries);
    for (size_t i = 0; i <= PARSE_WALK_CHILD_COMPLETE; i++) {
        core_free(w->stages[i].data);
    }
    core_free(w->stack.data);
    core_list_free(w->detached);
    reduce_release(w);
    core_free(w);
}

void
parse_walk_on_node(parse_tree_walk_t *w, parse_visit_fn pre, parse_visit_fn post,
                 void *thunk)
{
    core_list_push(w->any, add_visitor(w, pre, post, thunk, 0));
}

void
parse_walk_on_leaf(parse_tree_walk_t *w, parse_visit_fn fn, void *thunk)
{
    core_list_push(w->leaf, add_visitor(w, fn, nullptr, thunk, 0));
}

void
parse_walk_on_stage(parse_tree_walk_t *w, parse_walk_stage_t stage,
                    parse_walk_stage_fn fn, void *thunk)
{
    if (!w || !fn || stage < PARSE_WALK_PREPARE
        || stage > PARSE_WALK_CHILD_COMPLETE) return;
    core_list_push(w->stages[stage],
                   ((stage_visitor_t){.fn = fn, .thunk = thunk}));
}

bool
parse_visit_suppress_annotation(parse_visit_t *v, parse_annot_kind_t kind)
{
    if (!v || !v->walk || kind < 0 || kind >= PARSE_ANNOT_KIND_COUNT)
        return false;
    parse_tree_walk_t *w = v->walk;
    if (!w->stack.len || w->stack.data[w->stack.len - 1].node != v->node)
        return false;
    frame_t *f = &w->stack.data[w->stack.len - 1];
    f->suppressed[kind / 64] |= UINT64_C(1) << (kind % 64);
    return true;
}

bool
parse_walk_set_annotation_order(parse_tree_walk_t *w,
                                const parse_annot_kind_t *kinds,
                                size_t kind_count)
{
    if (!w || (kind_count && !kinds)) return false;
    for (size_t i = 0; i < kind_count; i++) {
        if (kinds[i] < 0 || kinds[i] >= PARSE_ANNOT_KIND_COUNT) return false;
        for (size_t j = 0; j < i; j++) {
            if (kinds[i] == kinds[j]) return false;
        }
    }
    parse_annot_kind_t *copy = nullptr;
    if (kind_count) {
        copy = core_alloc_array(parse_annot_kind_t, kind_count);
        if (!copy) return false;
        memcpy(copy, kinds, kind_count * sizeof(*copy));
    }
    core_free(w->annot_order);
    w->annot_order = copy;
    w->annot_order_len = kind_count;
    return true;
}

bool
parse_walk_on_annotation_boundary(parse_tree_walk_t *w, size_t before_kind,
                                  parse_walk_stage_fn entering,
                                  parse_walk_stage_fn exiting, void *thunk)
{
    if (!w || !w->annot_order || !before_kind
        || before_kind >= w->annot_order_len || (!entering && !exiting))
        return false;
    core_list_push(w->boundaries, ((annotation_boundary_t){
        .before_kind = before_kind,
        .entering = entering,
        .exiting = exiting,
        .thunk = thunk,
    }));
    return true;
}

void
parse_walk_on_annot_phase(parse_tree_walk_t *w, parse_annot_kind_t kind,
                        int32_t phase, parse_visit_fn pre, parse_visit_fn post,
                        void *thunk)
{
    if (kind < 0 || kind >= PARSE_ANNOT_KIND_COUNT || phase < 0) {
        return;
    }

    core_list_push(w->by_annot[kind], add_visitor(w, pre, post, thunk, phase));
}

void
parse_walk_on_annot(parse_tree_walk_t *w, parse_annot_kind_t kind, parse_visit_fn pre,
                  parse_visit_fn post, void *thunk)
{
    parse_walk_on_annot_phase(w, kind, 0, pre, post, thunk);
}

bool
parse_walk_on_nt(parse_tree_walk_t *w, core_string_t nt_name, parse_visit_fn pre,
               parse_visit_fn post, void *thunk)
{
    if (!w->g) {
        return false;
    }

    parse_nonterm_t *nt = parse_find_nonterm(w->g, nt_name);

    if (!nt) {
        return false;
    }

    bool     found = false;
    chain_t *c     = (chain_t *)core_dict_get(w->by_nt, (uintptr_t)nt->id,
                                             &found);

    if (!found) {
        c = core_alloc(chain_t);
        memset(c, 0, sizeof(*c));
        core_dict_put(w->by_nt, (uintptr_t)nt->id, c);
    }

    core_list_push(*c, add_visitor(w, pre, post, thunk, 0));

    return true;
}

static chain_t *
nt_chain(parse_tree_walk_t *w, int64_t nt_id)
{
    bool  found = false;
    void *c     = core_dict_get(w->by_nt, (uintptr_t)nt_id, &found);

    return found ? (chain_t *)c : nullptr;
}

static bool
run_chain(parse_tree_walk_t *w, chain_t *c, parse_visit_t *v, bool pre,
          bool reverse, int32_t phase)
{
    bool asked = false;
    bool wants = true;

    if (!c) {
        return true;
    }

    for (size_t k = 0; k < c->len; k++) {
        size_t       i  = reverse ? c->len - 1 - k : k;
        visitor_t   *vi = &w->visitors.data[c->data[i]];
        parse_visit_fn fn = pre ? vi->pre : vi->post;

        if (!fn || (phase >= 0 && vi->phase != phase)) {
            continue;
        }

        v->thunk = vi->thunk;

        bool want = fn(v);

        if (pre) {
            asked = true;
            wants = wants && want;
        }

        if (w->stopped) break;
    }

    return !asked || wants;
}

static parse_visit_outcome_t
run_stage(parse_tree_walk_t *w, parse_walk_stage_t stage, parse_visit_t *v)
{
    parse_visit_outcome_t result = PARSE_VISIT_CONTINUE;
    for (size_t i = 0; i < w->stages[stage].len; i++) {
        stage_visitor_t *callback = &w->stages[stage].data[i];
        v->thunk = callback->thunk;
        parse_visit_outcome_t outcome = callback->fn(v);
        if (outcome == PARSE_VISIT_ERROR) {
            w->failed = true;
            w->stopped = true;
            return outcome;
        }
        if (outcome == PARSE_VISIT_STOP || w->stopped) {
            w->stopped = true;
            return PARSE_VISIT_STOP;
        }
        if (outcome == PARSE_VISIT_PRUNE) result = outcome;
    }
    return result;
}

static parse_visit_outcome_t
run_annotation_boundaries(parse_tree_walk_t *w, parse_visit_t *v,
                          size_t position, bool entering)
{
    parse_visit_outcome_t result = PARSE_VISIT_CONTINUE;
    v->annot = nullptr;
    for (size_t i = 0; i < w->boundaries.len; i++) {
        annotation_boundary_t *boundary = &w->boundaries.data[i];
        parse_walk_stage_fn fn = entering ? boundary->entering
                                          : boundary->exiting;
        if (boundary->before_kind != position || !fn) continue;
        v->thunk = boundary->thunk;
        parse_visit_outcome_t outcome = fn(v);
        if (outcome == PARSE_VISIT_ERROR) {
            w->failed = w->stopped = true;
            return outcome;
        }
        if (outcome == PARSE_VISIT_STOP || w->stopped) {
            w->stopped = true;
            return PARSE_VISIT_STOP;
        }
        if (outcome == PARSE_VISIT_PRUNE) result = outcome;
    }
    return result;
}

static bool
annotation_suppressed(parse_tree_walk_t *w, parse_annot_kind_t kind)
{
    if (!w->stack.len) return false;
    frame_t *f = &w->stack.data[w->stack.len - 1];
    return (f->suppressed[kind / 64] & (UINT64_C(1) << (kind % 64))) != 0;
}

static bool
run_annots(parse_tree_walk_t *w, parse_visit_t *v, bool pre, bool reverse)
{
    bool   descend = true;
    size_t n_nt    = parse_nt_annot_count(v->nt);
    size_t n_rule  = parse_rule_annot_count(v->rule);
    size_t total   = n_nt + n_rule;

    size_t kind_passes = w->annot_order ? w->annot_order_len : 1;
    for (size_t kp = 0; kp < kind_passes && !w->stopped; kp++) {
        size_t kind_index = reverse ? kind_passes - 1 - kp : kp;
        parse_annot_kind_t selected = PARSE_ANNOT_NONE;
        if (w->annot_order) {
            selected = w->annot_order[kind_index];
        }
        for (int32_t p = 0; p <= w->max_phase && !w->stopped; p++) {
            int32_t phase = reverse ? w->max_phase - p : p;
            for (size_t k = 0; k < total; k++) {
                size_t i = reverse ? total - 1 - k : k;
                parse_annotation_t *a = i < n_nt
                    ? parse_nt_annot_at(v->nt, i)
                    : parse_rule_annot_at(v->rule, i - n_nt);
                if (!a || (w->annot_order && a->kind != selected)
                    || annotation_suppressed(w, a->kind)) continue;
                v->annot = a;
                if (!run_chain(w, &w->by_annot[a->kind], v, pre, reverse,
                               phase)) descend = false;
                if (w->stopped) break;
            }
        }
        if (w->annot_order && !w->stopped) {
            size_t position = pre ? kind_index + 1 : kind_index;
            if (run_annotation_boundaries(w, v, position, pre)
                == PARSE_VISIT_PRUNE) descend = false;
        }
    }

    v->annot = nullptr;

    return descend;
}

static bool
replace_frame_node(parse_tree_walk_t *w, frame_t *f, parse_tree_t *replacement)
{
    if (!replacement || replacement == f->node) return true;
    parse_tree_t *old = f->node;
    if (f->parent) {
        if (f->child_index < 0
            || (size_t)f->child_index >= core_tree_num_children(f->parent)
            || core_tree_child(f->parent, (size_t)f->child_index) != old)
            return false;
        if (!core_tree_set_child(f->parent, (size_t)f->child_index,
                                 replacement)) return false;
    }
    else w->new_root = replacement;
    f->node = replacement;
    core_list_push(w->detached, old);
    w->replaced++;
    return true;
}

static bool
remove_frame_node(parse_tree_walk_t *w, frame_t *f)
{
    parse_tree_t *old = f->node;
    if (f->parent) {
        if (f->child_index < 0
            || (size_t)f->child_index >= core_tree_num_children(f->parent)
            || core_tree_child(f->parent, (size_t)f->child_index) != old)
            return false;
        if (core_tree_remove_child(f->parent, (size_t)f->child_index) != old)
            return false;
    }
    else w->root_removed = true;
    core_list_push(w->detached, old);
    f->node = nullptr;
    f->removed = true;
    w->replaced++;
    return true;
}

static bool
apply_frame_edit(parse_tree_walk_t *w, frame_t *f, parse_visit_t *v,
                 bool allow_revisit)
{
    if ((v->remove && v->replace)
        || (v->revisit_replacement
            && (!allow_revisit || !v->replace || v->replace == f->node)))
        return false;
    if (v->remove) return remove_frame_node(w, f);
    return replace_frame_node(w, f, v->replace);
}

static void
complete_frame(parse_tree_walk_t *w)
{
    size_t top = w->stack.len - 1;
    frame_t finished = w->stack.data[top];
    w->stack.len = top;
    if (!top || w->stopped) return;
    frame_t *parent = &w->stack.data[top - 1];
    if (finished.removed && parent->next_child > 0)
        parent->next_child--;
    parse_visit_t visit;
    fill(w, &visit, parent);
    visit.completed_child = finished.removed ? nullptr : finished.node;
    visit.completed_child_index = finished.child_index;
    parse_visit_outcome_t outcome = run_stage(w, PARSE_WALK_CHILD_COMPLETE,
                                               &visit);
    if (outcome == PARSE_VISIT_PRUNE) parent->descend = false;
}

static void
fill(parse_tree_walk_t *w, parse_visit_t *v, frame_t *f)
{
    memset(v, 0, sizeof(*v));

    v->walk        = w;
    v->grammar     = w->g;
    v->node        = f->node;
    v->parent      = f->parent;
    v->depth       = f->depth;
    v->child_index = f->child_index;
    v->user        = w->user;

    if (core_tree_is_leaf(f->node)) {
        return;
    }

    v->pn = &core_tree_node_value(f->node);
    v->pn->parent = f->parent;

    if (v->pn->id < 0 || v->pn->id == PARSE_EMPTY_STRING || v->pn->group_top) {
        return;
    }

    v->nt   = parse_get_nonterm(w->g, v->pn->id);
    v->rule = parse_get_node_rule(w->g, v->pn);
}

static void
push_frame(parse_tree_walk_t *w, parse_tree_t *node, parse_tree_t *parent,
           int32_t depth, int32_t child_index)
{
    frame_t f = {
        .node        = node,
        .parent      = parent,
        .depth       = depth,
        .child_index = child_index,
    };

    core_list_push(w->stack, f);
}

void
parse_tree_walk_run(parse_tree_walk_t *w, parse_tree_t *root)
{
    parse_tree_walk_run_with_parent(w, root, nullptr, 0, -1);
}

void
parse_tree_walk_run_with_parent(parse_tree_walk_t *w, parse_tree_t *root,
                                parse_tree_t *parent, int32_t depth,
                                int32_t child_index)
{
    if (!w || !root) {
        return;
    }

    w->stopped   = false;
    w->failed    = false;
    w->replaced  = 0;
    w->new_root  = nullptr;
    w->root_removed = false;
    w->stack.len = 0;

    push_frame(w, root, parent, depth, child_index);

    while (w->stack.len > 0) {
        size_t      top = w->stack.len - 1;
        frame_t    *f   = &w->stack.data[top];
        parse_visit_t v;

        if (!f->entered) {
            f->entered = true;
            f->descend = !w->stopped;

            fill(w, &v, f);

            parse_visit_outcome_t outcome = w->stopped ? PARSE_VISIT_STOP
                : run_stage(w, PARSE_WALK_PREPARE, &v);
            f = &w->stack.data[top];
            if (!w->stopped && !apply_frame_edit(w, f, &v, true)) {
                w->failed = w->stopped = true;
            }
            if (!w->stopped && v.revisit_replacement) {
                f->entered = false;
                f->descend = false;
                f->next_child = 0;
                memset(f->suppressed, 0, sizeof(f->suppressed));
                continue;
            }
            if (outcome == PARSE_VISIT_PRUNE) f->skip_callbacks = true;
            if (w->stopped || f->skip_callbacks || f->removed) {
                complete_frame(w);
                continue;
            }
            fill(w, &v, f);

            if (core_tree_is_leaf(f->node)) {
                run_chain(w, &w->leaf, &v, true, false, -1);
                parse_visit_outcome_t leaf_exit = w->stopped
                    ? PARSE_VISIT_STOP
                    : run_stage(w, PARSE_WALK_BEFORE_EXIT, &v);
                if (!w->stopped && leaf_exit != PARSE_VISIT_PRUNE)
                    run_stage(w, PARSE_WALK_AFTER_EXIT, &v);
                if (!w->stopped
                    && !apply_frame_edit(w, &w->stack.data[top], &v, false))
                    w->failed = w->stopped = true;
                complete_frame(w);
                continue;
            }

            if (!w->stopped) {
                if (!run_chain(w, &w->any, &v, true, false, -1)) {
                    f->descend = false;
                }

                if (v.nt && !run_chain(w, nt_chain(w, v.pn->id), &v, true,
                                       false, -1)) {
                    f->descend = false;
                }

                if (v.nt && !run_annots(w, &v, true, false)) {
                    f->descend = false;
                }
                if (!w->stopped) {
                    parse_visit_outcome_t after_enter = run_stage(
                        w, PARSE_WALK_AFTER_ENTER, &v);
                    if (after_enter == PARSE_VISIT_PRUNE) f->descend = false;
                }
            }

            f = &w->stack.data[top];
        }

        size_t nc = f->descend && !w->stopped ? core_tree_num_children(f->node)
                                              : 0;

        if ((size_t)f->next_child < nc) {
            int32_t           ix    = f->next_child++;
            parse_tree_t *child = core_tree_child(f->node, ix);
            parse_tree_t *node  = f->node;
            int32_t           depth = f->depth;

            push_frame(w, child, node, depth + 1, ix);
            continue;
        }

        fill(w, &v, &w->stack.data[top]);

        parse_visit_outcome_t exit_outcome = w->stopped ? PARSE_VISIT_STOP
            : run_stage(w, PARSE_WALK_BEFORE_EXIT, &v);
        if (exit_outcome == PARSE_VISIT_PRUNE) {
            complete_frame(w);
            continue;
        }
        if (w->stopped) {
            complete_frame(w);
            continue;
        }

        if (v.nt) {
            run_annots(w, &v, false, true);
            run_chain(w, nt_chain(w, v.pn->id), &v, false, true, -1);
        }

        run_chain(w, &w->any, &v, false, true, -1);

        if (!w->stopped)
            run_stage(w, PARSE_WALK_AFTER_EXIT, &v);

        if (!w->stopped
            && !apply_frame_edit(w, &w->stack.data[top], &v, false))
            w->failed = w->stopped = true;

        complete_frame(w);
    }
}

parse_tree_t *
parse_tree_walk_rewrite(parse_tree_walk_t *w, parse_tree_t *root)
{
    if (!w || !root) {
        return root;
    }

    w->replaced = 0;
    w->new_root = nullptr;

    parse_tree_walk_run(w, root);

    return w->root_removed ? nullptr : w->new_root ? w->new_root : root;
}

uint32_t
parse_tree_walk_replaced(const parse_tree_walk_t *w)
{
    return w ? w->replaced : 0;
}

void
parse_tree_walk_stop(parse_tree_walk_t *w)
{
    if (w) {
        w->stopped = true;
    }
}

bool
parse_tree_walk_stopped(parse_tree_walk_t *w)
{
    return w && w->stopped;
}

bool
parse_tree_walk_failed(parse_tree_walk_t *w)
{
    return w && w->failed;
}

parse_tree_t *
parse_tree_walk_take_detached(parse_tree_walk_t *w)
{
    if (!w || !w->detached.len) return nullptr;
    return w->detached.data[--w->detached.len];
}

typedef struct {
    parse_reduce_fn fn;
    void         *thunk;
} reducer_t;

core_list_decl(reducer_t);

struct parse_reduce_state_t {
    core_list_t(reducer_t) by_nt;
    reducer_t             any;
    reducer_t             leaf;
    core_list_t(reducer_t) unit_leaf;
    bool                  have_any;
    bool                  have_leaf;
};

static struct parse_reduce_state_t *reduce_state(parse_tree_walk_t *w)
{
    if (!w->reduce) {
        w->reduce = core_alloc(struct parse_reduce_state_t);
        memset(w->reduce, 0, sizeof(*w->reduce));
    }

    return w->reduce;
}

bool parse_walk_reduce_nt(parse_tree_walk_t *w, core_string_t nt_name,
                        parse_reduce_fn fn, void *thunk)
{
    if (!w || !w->g) {
        return false;
    }

    parse_nonterm_t *nt = parse_find_nonterm(w->g, nt_name);

    if (!nt) {
        return false;
    }

    int64_t id = nt->id;

    struct parse_reduce_state_t *st = reduce_state(w);

    while ((int64_t)core_list_len(st->by_nt) <= id) {
        reducer_t none = {0};

        core_list_push(st->by_nt, none);
    }

    st->by_nt.data[id] = (reducer_t){.fn = fn, .thunk = thunk};

    return true;
}

void parse_walk_reduce_default(parse_tree_walk_t *w, parse_reduce_fn fn, void *thunk)
{
    struct parse_reduce_state_t *st = reduce_state(w);

    st->any      = (reducer_t){.fn = fn, .thunk = thunk};
    st->have_any = true;
}

void parse_walk_reduce_leaf(parse_tree_walk_t *w, parse_reduce_fn fn, void *thunk)
{
    struct parse_reduce_state_t *st = reduce_state(w);

    st->leaf      = (reducer_t){.fn = fn, .thunk = thunk};
    st->have_leaf = true;
}

void parse_walk_reduce_leaf_in(parse_tree_walk_t *w, int32_t unit,
                             parse_reduce_fn fn, void *thunk)
{
    struct parse_reduce_state_t *st = reduce_state(w);

    while (core_list_len(st->unit_leaf) <= (size_t)unit) {
        core_list_push(st->unit_leaf, ((reducer_t){0}));
    }
    st->unit_leaf.data[unit] = (reducer_t){.fn = fn, .thunk = thunk};
}

static reducer_t *leaf_in_unit(struct parse_reduce_state_t *st, int32_t unit)
{
    if (unit > 0 && (size_t)unit < core_list_len(st->unit_leaf)
        && st->unit_leaf.data[unit].fn) {
        return &st->unit_leaf.data[unit];
    }

    return nullptr;
}

typedef struct {
    parse_tree_t *node;
    size_t            next;
    size_t            mark;
    bool              entered;
} rframe_t;

core_list_decl(rframe_t);

static reducer_t *leaf_for(struct parse_reduce_state_t *st, parse_grammar_t *g,
                           core_list_t(rframe_t) frames)
{
    for (size_t i = frames.len - 1; i-- > 0;) {
        parse_tree_t *p = frames.data[i].node;

        if (!p || core_tree_is_leaf(p)) {
            continue;
        }

        parse_nonterm_t *nt = parse_get_nonterm(g, core_tree_node_value(p).id);
        reducer_t     *hit = nt ? leaf_in_unit(st, nt->unit) : nullptr;

        if (hit) {
            return hit;
        }
    }

    return st->have_leaf ? &st->leaf : nullptr;
}

typedef void *vptr_t;
core_list_decl(vptr_t);

#define REDUCE_LIST_MAGIC 0x52644c73u

void *parse_tree_walk_reduce(parse_tree_walk_t *w, parse_tree_t *root)
{
    if (!w || !root || !w->reduce) {
        return nullptr;
    }

    struct parse_reduce_state_t *st = w->reduce;

    core_list_t(rframe_t) frames = core_list_new(rframe_t);
    core_list_t(vptr_t)   done   = core_list_new(vptr_t);

    w->stopped = false;
    core_list_push(frames, ((rframe_t){.node = root}));

    while (frames.len > 0 && !w->stopped) {
        rframe_t *f = &frames.data[frames.len - 1];

        if (!f->entered) {
            parse_visit_t v;
            frame_t     vf = {.node = f->node};

            f->entered = true;
            f->mark    = done.len;

            fill(w, &v, &vf);
            run_chain(w, &w->any, &v, true, false, -1);

            if (v.nt && !w->stopped) {
                run_chain(w, nt_chain(w, v.pn->id), &v, true, false, -1);
                run_annots(w, &v, true, false);
            }

            if (w->stopped) {
                break;
            }
        }

        if (!core_tree_is_leaf(f->node)
            && f->next < core_tree_num_children(f->node)) {
            parse_tree_t *ch = core_tree_child(f->node, f->next++);

            core_list_push(frames, ((rframe_t){.node = ch}));
            continue;
        }

        f = &frames.data[frames.len - 1];

        reducer_t *use = nullptr;

        if (core_tree_is_leaf(f->node)) {
            use = leaf_for(st, w->g, frames);
        }
        else {
            parse_nt_node_t *pn = &core_tree_node_value(f->node);

            if (pn->id >= 0 && !pn->group_top
                && (size_t)pn->id < core_list_len(st->by_nt)
                && st->by_nt.data[pn->id].fn) {
                use = &st->by_nt.data[pn->id];
            }
            else if (st->have_any) {
                use = &st->any;
            }
        }

        void  *result = nullptr;
        size_t mark   = f->mark;

        if (use) {
            parse_reduce_t r = {
                .walk    = w,
                .grammar = w->g,
                .node    = f->node,
                .kids    = done.data + mark,
                .n_kids  = done.len - mark,
                .thunk   = use->thunk,
                .user    = w->user,
            };

            result = use->fn(&r);
        }
        else if (done.len - mark == 1) {
            result = done.data[mark];
        }

        {
            parse_visit_t v;
            frame_t     vf = {.node = f->node};

            fill(w, &v, &vf);

            if (v.nt) {
                run_annots(w, &v, false, true);
                run_chain(w, nt_chain(w, v.pn->id), &v, false, true, -1);
            }

            run_chain(w, &w->any, &v, false, true, -1);
        }

        if (!use && !core_tree_is_leaf(f->node)
            && core_tree_node_value(f->node).group_top) {
            size_t             n    = done.len - mark;
            parse_reduce_list_t *list = core_alloc(parse_reduce_list_t);

            list->n = n;
            list->v = n ? core_alloc_array(vptr_t, n) : nullptr;

            for (size_t i = 0; i < n; i++) {
                list->v[i] = done.data[mark + i];
            }

            list->magic = REDUCE_LIST_MAGIC;
            result      = list;
        }

        done.len = mark;
        core_list_push(done, result);
        frames.len--;
    }

    void *out = !w->stopped && done.len ? done.data[done.len - 1] : nullptr;

    core_list_free(frames);
    core_list_free(done);

    return out;
}

static void reduce_release(parse_tree_walk_t *w)
{
    if (!w || !w->reduce) {
        return;
    }

    core_free(w->reduce->by_nt.data);
    core_free(w->reduce);
    w->reduce = nullptr;
}

parse_reduce_list_t *parse_reduce_as_list(void *result)
{
    parse_reduce_list_t *l = result;

    return l && l->magic == REDUCE_LIST_MAGIC ? l : nullptr;
}

parse_reduce_list_t *parse_reduce_list_new(size_t n)
{
    parse_reduce_list_t *l = core_alloc(parse_reduce_list_t);

    l->magic = REDUCE_LIST_MAGIC;
    l->n     = n;
    l->v     = n ? core_alloc_array(vptr_t, n) : nullptr;

    return l;
}
