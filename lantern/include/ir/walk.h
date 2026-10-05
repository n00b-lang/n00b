#pragma once

#include "ir/ir.h"

typedef struct ir_walk_t ir_walk_t;

typedef struct {
    ir_walk_t    *walk;
    ir_module_t  *module;
    ir_global_t  *global;
    ir_func_t    *func;
    ir_value_t   *param;
    ir_local_t   *local;
    ir_block_t   *block;
    ir_instr_t   *instr;
    ir_operand_t *operand;
    uint32_t      index;
    void         *thunk;
    void         *user;
} ir_visit_t;

typedef bool (*ir_visit_fn)(ir_visit_t *v);

ir_walk_t *ir_walk_new(void *user);
void       ir_walk_free(ir_walk_t *w);

void ir_walk_on_module(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                       void *thunk);
void ir_walk_on_global(ir_walk_t *w, ir_visit_fn fn, void *thunk);
void ir_walk_on_func(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                     void *thunk);
void ir_walk_on_param(ir_walk_t *w, ir_visit_fn fn, void *thunk);
void ir_walk_on_local(ir_walk_t *w, ir_visit_fn fn, void *thunk);
void ir_walk_on_block(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                      void *thunk);
void ir_walk_on_instr(ir_walk_t *w, ir_visit_fn pre, ir_visit_fn post,
                      void *thunk);
void ir_walk_on_op(ir_walk_t *w, ir_op_t op, ir_visit_fn pre,
                   ir_visit_fn post, void *thunk);
void ir_walk_on_operand(ir_walk_t *w, ir_visit_fn fn, void *thunk);

void ir_walk_run(ir_walk_t *w, ir_module_t *m);
void ir_walk_run_func(ir_walk_t *w, ir_func_t *f);

void ir_walk_stop(ir_walk_t *w);
bool ir_walk_stopped(ir_walk_t *w);

typedef bool (*ir_reachable_func_fn)(ir_func_t *function, void *user);

/* Visit each direct-call-reachable function once, beginning with roots.
 * Calls in unreachable basic blocks and indirect calls are excluded. */
bool ir_walk_reachable_functions(ir_func_t *const *roots, size_t root_count,
                                 ir_reachable_func_fn visit, void *user);
