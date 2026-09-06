#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/walk.h"

typedef struct {
    ir_visit_fn pre;
    ir_visit_fn post;
    void       *thunk;
} slot_t;

typedef struct chain_t {
    slot_t         *v;
    size_t          n;
    size_t          cap;
} chain_t;

struct ir_walk_t {
    void   *user;
    chain_t module;
    chain_t global;
    chain_t func;
    chain_t param;
    chain_t local;
    chain_t block;
    chain_t instr;
    chain_t operand;
    chain_t by_op[IR_OP_COUNT];
    bool    stopped;
};

static void add(chain_t *c, ir_visit_fn pre, ir_visit_fn post, void *thunk)
{
    if (c->n >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4;
        c->v   = realloc(c->v, c->cap * sizeof(*c->v));
    }

    c->v[c->n++] = (slot_t){.pre = pre, .post = post, .thunk = thunk};
}

ir_walk_t *ir_walk_new(void *user)
{
    ir_walk_t *w = calloc(1, sizeof(*w));

    w->user = user;

    return w;
}

void ir_walk_free(ir_walk_t *w)
{
    if (!w) {
        return;
    }

    free(w->module.v);
    free(w->global.v);
    free(w->func.v);
    free(w->param.v);
    free(w->local.v);
    free(w->block.v);
    free(w->instr.v);
    free(w->operand.v);

    for (int i = 0; i < IR_OP_COUNT; i++) {
        free(w->by_op[i].v);
    }

    free(w);
}

void *ir_walk_user(ir_walk_t *w)
{
    return w ? w->user : nullptr;
}

void ir_walk_on_module(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                       void *thunk)
{
    add(&w->module, pre, post, thunk);
}

void ir_walk_on_global(ir_walk_t *w, ir_visit_fn fn, void *thunk)
{
    add(&w->global, fn, nullptr, thunk);
}

void ir_walk_on_func(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                     void *thunk)
{
    add(&w->func, pre, post, thunk);
}

void ir_walk_on_param(ir_walk_t *w, ir_visit_fn fn, void *thunk)
{
    add(&w->param, fn, nullptr, thunk);
}

void ir_walk_on_local(ir_walk_t *w, ir_visit_fn fn, void *thunk)
{
    add(&w->local, fn, nullptr, thunk);
}

void ir_walk_on_block(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                      void *thunk)
{
    add(&w->block, pre, post, thunk);
}

void ir_walk_on_instr(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                      void *thunk)
{
    add(&w->instr, pre, post, thunk);
}

void ir_walk_on_op(ir_walk_t *w, ir_op_t op, ir_visit_fn pre,
                   ir_visit_fn post, void *thunk)
{
    if (op >= 0 && op < IR_OP_COUNT) {
        add(&w->by_op[op], pre, post, thunk);
    }
}

void ir_walk_on_operand(ir_walk_t *w, ir_visit_fn fn, void *thunk)
{
    add(&w->operand, fn, nullptr, thunk);
}

void ir_walk_stop(ir_walk_t *w)
{
    if (w) {
        w->stopped = true;
    }
}

bool ir_walk_stopped(ir_walk_t *w)
{
    return w && w->stopped;
}

static bool run(chain_t *c, ir_visit_t *v, bool pre, bool reverse)
{
    bool asked = false;
    bool wants = false;

    for (size_t k = 0; k < c->n; k++) {
        size_t      i  = reverse ? c->n - 1 - k : k;
        ir_visit_fn fn = pre ? c->v[i].pre : c->v[i].post;

        if (!fn) {
            continue;
        }

        v->thunk = c->v[i].thunk;

        bool want = fn(v);

        if (pre) {
            asked = true;
            wants = wants || want;
        }
    }

    return !asked || wants;
}

static void walk_instr(ir_walk_t *w, ir_visit_t *v, ir_instr_t *in)
{
    v->instr   = in;
    v->operand = nullptr;
    v->index   = in->index;

    bool descend = run(&w->instr, v, true, false);

    if (in->op >= 0 && in->op < IR_OP_COUNT) {
        descend = run(&w->by_op[in->op], v, true, false) || descend;
    }

    if (descend && !w->stopped) {
        for (uint32_t i = 0; i < in->n_operands; i++) {
            v->operand = &in->operands[i];
            v->index   = i;
            run(&w->operand, v, true, false);

            if (w->stopped) {
                break;
            }
        }
    }

    v->operand = nullptr;
    v->index   = in->index;

    if (in->op >= 0 && in->op < IR_OP_COUNT) {
        run(&w->by_op[in->op], v, false, true);
    }

    run(&w->instr, v, false, true);
    v->instr = nullptr;
}

static void walk_func(ir_walk_t *w, ir_visit_t *v, ir_func_t *f)
{
    v->func = f;

    bool descend = run(&w->func, v, true, false);

    if (descend && !w->stopped) {
        for (uint32_t i = 0; i < f->n_params; i++) {
            v->param = f->params[i];
            v->index = i;
            run(&w->param, v, true, false);
        }

        v->param = nullptr;

        for (uint32_t i = 0; i < f->n_locals; i++) {
            v->local = f->locals[i];
            v->index = i;
            run(&w->local, v, true, false);
        }

        v->local = nullptr;

        for (uint32_t b = 0; b < f->n_blocks && !w->stopped; b++) {
            ir_block_t *bb = f->blocks[b];

            v->block = bb;
            v->index = b;

            if (run(&w->block, v, true, false)) {
                for (uint32_t i = 0; i < bb->n_instrs && !w->stopped; i++) {
                    walk_instr(w, v, bb->instrs[i]);
                }
            }

            v->block = bb;
            v->index = b;
            run(&w->block, v, false, true);
        }

        v->block = nullptr;
    }

    run(&w->func, v, false, true);
    v->func = nullptr;
}

void ir_walk_run_func(ir_walk_t *w, ir_func_t *f)
{
    if (!w || !f) {
        return;
    }

    ir_visit_t v = {.walk = w, .user = w->user, .module = f->owner};

    w->stopped = false;
    walk_func(w, &v, f);
}

void ir_walk_run(ir_walk_t *w, ir_module_t *m)
{
    if (!w || !m) {
        return;
    }

    ir_visit_t v = {.walk = w, .user = w->user, .module = m};

    w->stopped = false;

    if (run(&w->module, &v, true, false)) {
        for (size_t i = 0; i < ir_global_count(m) && !w->stopped; i++) {
            v.global = ir_global_at(m, i);
            v.index  = (uint32_t)i;
            run(&w->global, &v, true, false);
        }

        v.global = nullptr;

        for (size_t i = 0; i < ir_func_count(m) && !w->stopped; i++) {
            v.index = (uint32_t)i;
            walk_func(w, &v, ir_func_at(m, i));
        }
    }

    run(&w->module, &v, false, true);
}
