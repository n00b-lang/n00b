#include "ir_internal.h"

bool ir_pass_declares(const ir_pass_t *p)
{
    if (!p || !p->name || !p->run) {
        return false;
    }

    return (p->establishes & p->invalidates) == 0;
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
