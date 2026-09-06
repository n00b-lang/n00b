#include <stdlib.h>

#include "ir_internal.h"
#include "ir/analyze.h"
#include "ir/doms.h"

static void say(ir_diags_t *d, ir_diagcode_t code, const ir_instr_t *in,
                const ir_block_t *b, const ir_func_t *f, const char *label,
                const char *fmt, const char *name)
{
    ir_diag_t *dg = ir_diag_begin_at(d, code, in, b, f, label);

    if (!dg) {
        return;
    }

    ir_sb sb = {0};

    ir_sb_fmt(&sb, fmt, name);

    char *msg = ir_sb_take(&sb);

    ir_diag_message(dg, d, msg);
    free(msg);
}

static void dead_code(ir_func_t *f, ir_doms_t *dom, ir_diags_t *d)
{
    for (uint32_t i = 1; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        if (ir_block_reachable(dom, b)) {
            continue;
        }
        say(d, IR_DIAG_DEAD_CODE, b->n_instrs ? b->instrs[0] : nullptr, b, f,
            "this block", "no path from the entry block reaches %s", b->name);
    }
}

static void local_facts(ir_func_t *f, ir_doms_t *dom, ir_rd_t *rd,
                        ir_diags_t *d)
{
    for (uint32_t i = 0; i < f->n_blocks; i++) {
        ir_block_t *b = f->blocks[i];

        if (!ir_block_reachable(dom, b)) {
            continue;
        }

        for (uint32_t k = 0; k < b->n_instrs; k++) {
            ir_instr_t *in = b->instrs[k];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                ir_local_t *l = ir_operand_local(&in->operands[o]);

                if (l && !ir_local_read_earlier(in, o, l)
                    && ir_rd_maybe_unset(rd, in, l)) {
                    say(d, IR_DIAG_UNSET_LOCAL, in, b, f, "this read",
                        "$%s is read on a path that does not assign it",
                        l->name);
                }
            }

            if (in->dest_kind == IR_DEST_LOCAL && in->dest_local
                && !ir_rd_reaches_a_use(rd, in)) {
                say(d, IR_DIAG_UNUSED_LOCAL, in, b, f, "this assignment",
                    "$%s is assigned here and no read of it is reached",
                    in->dest_local->name);
            }
        }
    }
}

bool ir_analyze(ir_module_t *m, ir_diags_t *d)
{
    if (!m || !d) {
        return false;
    }

    for (uint32_t i = 0; i < m->n_funcs; i++) {
        ir_func_t *f = m->funcs[i];

        if (!f->n_blocks) {
            continue;
        }

        ir_doms_t *dom = ir_doms_of(f);

        dead_code(f, dom, d);

        if (f->n_locals) {
            local_facts(f, dom, ir_rd_of(f, dom), d);
        }
    }

    return !ir_diags_failed(d);
}
