#include <stdlib.h>

#include "ir_internal.h"
#include "ir/verify.h"

bool ir_pass_declares(const ir_pass_t *p)
{
    if (!p || !p->name || !p->run) {
        return false;
    }

    return (p->establishes & p->invalidates) == 0;
}

ir_pass_result_t ir_pass_run_checked(const ir_pass_t *p, ir_module_t *m,
                                     void *state, ir_props_t *unmet,
                                     ir_diags_t *d)
{
    ir_locsnap_t    *before = d ? ir_loc_snapshot(m) : nullptr;
    uint32_t         said   = d ? ir_diags_count(d) : 0;
    ir_pass_result_t r      = ir_pass_run(p, m, state, unmet);

    if (r == IR_PASS_OK && before && !ir_loc_monotone(before, m, p->name, d)) {
        return IR_PASS_FAILED;
    }

    if (r == IR_PASS_FAILED && d && ir_diags_count(d) == said) {
        ir_diag_t *dg = ir_diag_begin_at(d, IR_DIAG_INTERNAL, nullptr, nullptr,
                                         nullptr, "this pass");

        if (dg) {
            ir_sb b = {0};

            ir_sb_fmt(&b, "%s failed and said nothing, which §3 does not "
                          "allow: a pass reports what it could not do",
                      p->name);

            char *msg = ir_sb_take(&b);

            ir_diag_message(dg, d, msg);
            free(msg);
        }
    }

    return r;
}

ir_pass_result_t ir_pass_run(const ir_pass_t *p, ir_module_t *m, void *state,
                             ir_props_t *unmet)
{
    if (unmet) {
        *unmet = 0;
    }

    if (!ir_pass_declares(p) || !m) {
        return IR_PASS_INVALID;
    }

    ir_props_t missing = (ir_props_t)(p->requires & ~ir_module_props(m));

    if (missing) {
        if (unmet) {
            *unmet = missing;
        }

        return IR_PASS_UNMET;
    }

    if (!p->run(m, state)) {
        return IR_PASS_FAILED;
    }

    ir_module_invalidate(m, p->invalidates);
    ir_module_establish(m, p->establishes);

    return IR_PASS_OK;
}
