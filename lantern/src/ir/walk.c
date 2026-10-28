#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/doms.h"
#include "ir/walk.h"

#include "lib/dict.h"
#include "lib/list.h"

typedef struct {
    ir_visit_fn pre;
    ir_visit_fn post;
    void       *thunk;
} slot_t;

core_list_decl(slot_t);
typedef ir_func_t *ir_walk_func_ptr_t;
core_list_decl(ir_walk_func_ptr_t);
typedef ir_block_t *ir_walk_block_ptr_t;
core_list_decl(ir_walk_block_ptr_t);
core_list_decl(bool);

typedef core_list_t(slot_t) chain_t;

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
    core_list_push(*c, ((slot_t){.pre = pre, .post = post, .thunk = thunk}));
}

ir_walk_t *ir_walk_new(void *user)
{
    ir_walk_t *w = core_alloc(typeof(*w));

    w->user = user;

    return w;
}

void ir_walk_free(ir_walk_t *w)
{
    if (!w) {
        return;
    }

    core_list_free(w->module);
    core_list_free(w->global);
    core_list_free(w->func);
    core_list_free(w->param);
    core_list_free(w->local);
    core_list_free(w->block);
    core_list_free(w->instr);
    core_list_free(w->operand);

    for (int i = 0; i < IR_OP_COUNT; i++) {
        core_list_free(w->by_op[i]);
    }

    core_free(w);
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

bool
ir_walk_reachable_functions(ir_func_t *const *roots, size_t root_count,
                            ir_reachable_func_fn visit,
                            ir_reachable_body_fn body, void *user)
{
    if ((!roots && root_count) || !visit) return false;
    core_list_t(ir_walk_func_ptr_t) functions =
        core_list_new(ir_walk_func_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    bool ok = true;
    for (size_t i = 0; ok && i < root_count; i++) {
        ok = roots[i] != nullptr;
        if (ok && core_dict_add(&seen, roots[i], roots[i]))
            core_list_push(functions, roots[i]);
    }
    core_list_t(ir_walk_block_ptr_t) blocks =
        core_list_new(ir_walk_block_ptr_t);
    core_list_t(bool) reached = core_list_new(bool);
    for (size_t i = 0; ok && i < functions.len; i++) {
        ok = visit(functions.data[i], user);
        ir_func_t *function = body ? body(functions.data[i], user)
                                   : functions.data[i];
        ok = ok && function;
        blocks.len = 0;
        reached.len = 0;
        for (uint32_t b = 0; ok && b < function->n_blocks; b++)
            core_list_push(reached, false);
        ir_block_t *entry = ok ? ir_block_entry(function) : nullptr;
        if (entry) {
            reached.data[entry->index] = true;
            core_list_push(blocks, entry);
        }
        for (size_t b = 0; ok && b < blocks.len; b++) {
            ir_block_t *block = blocks.data[b];
            ir_instr_t *terminator = ir_block_terminator(block);
            for (uint32_t o = 0; terminator && o < terminator->n_operands;
                 o++) {
                ir_block_t *to = ir_succ_of(&terminator->operands[o]);
                if (to && !reached.data[to->index]) {
                    reached.data[to->index] = true;
                    core_list_push(blocks, to);
                }
            }
            for (size_t n = 0; n < block->n_instrs; n++) {
                ir_instr_t *instruction = block->instrs[n];
                if (!instruction) continue;
                for (uint32_t o = 0; o < instruction->n_operands; o++) {
                    ir_value_t *value = ir_operand_value(
                        &instruction->operands[o]);
                    ir_func_t *target = value && value->kind == IR_VAL_FUNC
                        ? value->owner : nullptr;
                    if (target && core_dict_add(&seen, target, target))
                        core_list_push(functions, target);
                }
            }
        }
    }
    core_list_free(blocks);
    core_list_free(reached);
    core_dict_free(&seen);
    core_list_free(functions);
    return ok;
}

static void run_into(chain_t *c, ir_visit_t *v, bool pre, bool reverse,
                     bool *asked, bool *wants)
{
    for (size_t k = 0; k < c->len; k++) {
        size_t      i  = reverse ? c->len - 1 - k : k;
        ir_visit_fn fn = pre ? c->data[i].pre : c->data[i].post;

        if (!fn) {
            continue;
        }

        v->thunk = c->data[i].thunk;

        bool want = fn(v);

        if (pre) {
            *asked = true;
            *wants = *wants || want;
        }
    }
}

static bool run(chain_t *c, ir_visit_t *v, bool pre, bool reverse)
{
    bool asked = false;
    bool wants = false;

    run_into(c, v, pre, reverse, &asked, &wants);

    return !asked || wants;
}

static void walk_instr(ir_walk_t *w, ir_visit_t *v, ir_instr_t *in)
{
    v->instr   = in;
    v->operand = nullptr;
    v->index   = in->index;

    bool asked = false;
    bool wants = false;

    run_into(&w->instr, v, true, false, &asked, &wants);

    if (in->op >= 0 && in->op < IR_OP_COUNT) {
        run_into(&w->by_op[in->op], v, true, false, &asked, &wants);
    }

    bool descend = !asked || wants;

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
