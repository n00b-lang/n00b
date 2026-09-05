#ifndef IR_VERIFY_H
#define IR_VERIFY_H

#include "ir/diag.h"
#include "ir/ir.h"

ir_props_t ir_verify(ir_module_t *m, ir_diags_t *out);

typedef struct ir_locsnap_t ir_locsnap_t;

ir_locsnap_t *ir_loc_snapshot(ir_module_t *m);
bool          ir_loc_monotone(ir_locsnap_t *before, ir_module_t *m,
                              const char *agent, ir_diags_t *out);

ir_pass_result_t ir_pass_run_checked(const ir_pass_t *p, ir_module_t *m,
                                     void *state, ir_props_t *unmet,
                                     ir_diags_t *d);

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

#endif
