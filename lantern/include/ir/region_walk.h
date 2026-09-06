#pragma once

#include "ir/pass.h"

#define IR_REGION_KIND_COUNT (IR_REGION_BLOCK + 1)

typedef struct ir_region_walk_t ir_region_walk_t;

typedef struct {
    ir_region_walk_t *walk;
    ir_region_t      *region;
    ir_region_t      *parent;
    uint32_t          index;
    int32_t           depth;
    void             *thunk;
    void             *user;
} ir_region_visit_t;

typedef bool (*ir_region_fn)(ir_region_visit_t *v);

ir_region_walk_t *ir_region_walk_new(void *user);
void              ir_region_walk_free(ir_region_walk_t *w);
void             *ir_region_walk_user(ir_region_walk_t *w);

void ir_region_on_node(ir_region_walk_t *w, ir_region_fn pre,
                       ir_region_fn post, void *thunk);
void ir_region_on_kind(ir_region_walk_t *w, ir_region_kind_t kind,
                       ir_region_fn pre, ir_region_fn post, void *thunk);

void ir_region_walk_run(ir_region_walk_t *w, ir_region_t *root);
void ir_region_walk_run_func(ir_region_walk_t *w, ir_func_t *f);

void ir_region_walk_stop(ir_region_walk_t *w);
bool ir_region_walk_stopped(ir_region_walk_t *w);
