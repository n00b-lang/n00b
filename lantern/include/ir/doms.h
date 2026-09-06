#ifndef IR_DOMS_H
#define IR_DOMS_H

#include "ir/ir.h"

uint32_t    ir_succ_count(ir_instr_t *t);
ir_block_t *ir_succ_of(const ir_operand_t *o);
ir_block_t *ir_succ_at(ir_instr_t *t, uint32_t i);

typedef struct ir_doms_t ir_doms_t;

ir_doms_t  *ir_doms_of(ir_func_t *f);
bool        ir_dominates(ir_doms_t *d, ir_block_t *a, ir_block_t *b);
ir_block_t *ir_idom_of(ir_doms_t *d, ir_block_t *b);
uint32_t    ir_rpo_index(ir_doms_t *d, ir_block_t *b);
uint32_t    ir_pred_count(ir_doms_t *d, ir_block_t *b);
uint32_t    ir_pred_total(ir_doms_t *d);
ir_block_t *ir_pred_at(ir_doms_t *d, ir_block_t *b, uint32_t i);
ir_block_t *ir_ipdom_of(ir_doms_t *d, ir_block_t *b);
bool        ir_block_reachable(ir_doms_t *d, ir_block_t *b);
uint32_t    ir_df_count(ir_doms_t *d, ir_block_t *b);
ir_block_t *ir_df_at(ir_doms_t *d, ir_block_t *b, uint32_t i);
uint32_t    ir_dom_kid_count(ir_doms_t *d, ir_block_t *b);
ir_block_t *ir_dom_kid_at(ir_doms_t *d, ir_block_t *b, uint32_t i);

typedef bool (*ir_dom_fn)(ir_block_t *b, void *thunk);

void ir_dom_walk(ir_doms_t *d, ir_block_t *from, ir_dom_fn pre,
                 ir_dom_fn post, void *thunk);

#endif
