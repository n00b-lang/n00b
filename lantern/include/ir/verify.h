#ifndef IR_VERIFY_H
#define IR_VERIFY_H

#include "ir/diag.h"
#include "ir/ir.h"

typedef struct {
    ir_func_t **functions;
    size_t      count;
    bool        module;
} ir_verify_failures_t;

ir_props_t ir_verify(ir_module_t *m, ir_diags_t *out);
ir_props_t ir_verify_with_failures(ir_module_t *m, ir_diags_t *out,
                                   ir_verify_failures_t *failures);
void       ir_verify_failures_release(ir_verify_failures_t *failures);
ir_props_t ir_verify_only(ir_module_t *m, ir_props_t want, ir_diags_t *out);

typedef struct ir_locsnap_t ir_locsnap_t;

ir_locsnap_t *ir_loc_snapshot(ir_module_t *m);
void          ir_loc_snapshot_free(ir_locsnap_t *s);
bool          ir_loc_monotone(ir_locsnap_t *before, ir_module_t *m,
                              core_string_t agent, ir_diags_t *out);

ir_pass_result_t ir_pass_run(const ir_pass_t *p, ir_module_t *m, void *state,
                             ir_props_t *unmet, ir_diags_t *d);

#endif
