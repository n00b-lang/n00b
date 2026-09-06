#ifndef IR_VERIFY_H
#define IR_VERIFY_H

#include "ir/diag.h"
#include "ir/ir.h"
#include "ir/doms.h"

ir_props_t ir_verify(ir_module_t *m, ir_diags_t *out);

typedef struct ir_locsnap_t ir_locsnap_t;

ir_locsnap_t *ir_loc_snapshot(ir_module_t *m);
bool          ir_loc_monotone(ir_locsnap_t *before, ir_module_t *m,
                              const char *agent, ir_diags_t *out);

ir_pass_result_t ir_pass_run_checked(const ir_pass_t *p, ir_module_t *m,
                                     void *state, ir_props_t *unmet,
                                     ir_diags_t *d);

#endif
