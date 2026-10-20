#include <stdlib.h>

#include "private/ir/internal.h"
#include "ir/analyze.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/walk.h"

static void say(ir_diags_t *d, ir_diagcode_t code, const ir_instr_t *in,
                const ir_block_t *b, const ir_func_t *f, core_string_t label,
                core_string_t prefix, core_string_t name, core_string_t suffix)
{
    ir_diag_t *dg = ir_diag_begin_at(d, code, in, b, f, label);

    if (!dg) {
        return;
    }

    core_buffer_t *sb = core_buffer_empty();

    core_buffer_append_string(sb, prefix);
    core_buffer_append_string(sb, name);
    core_buffer_append_string(sb, suffix);

    core_string_t msg = core_buffer_take_string(sb);

    ir_diag_message(dg, d, msg);
    core_free(msg.data);
}

typedef struct {
    ir_diags_t *d;
    ir_doms_t  *dom;
    ir_rd_t    *rd;
    size_t      scope;
    bool        scoped;
} A;

static bool analyze_func(ir_visit_t *vi)
{
    A         *a = vi->user;
    ir_func_t *f = vi->func;

    if (!f->n_blocks) {
        return false;
    }

    a->scope  = ir_analysis_scope_begin();
    a->scoped = true;
    a->dom    = ir_func_doms(f);
    a->rd    = f->n_locals ? ir_rd_of(f, a->dom) : nullptr;

    return true;
}

static bool analyze_func_end(ir_visit_t *vi)
{
    A *a = vi->user;

    if (a->scoped) ir_analysis_scope_end(a->scope);
    a->scoped = false;
    a->dom    = nullptr;
    a->rd     = nullptr;

    return true;
}

static bool analyze_block(ir_visit_t *vi)
{
    A          *a = vi->user;
    ir_block_t *b = vi->block;

    if (ir_block_reachable(a->dom, b)) {
        return a->rd != nullptr;
    }

    if (b->index) {
        say(a->d, IR_DIAG_DEAD_CODE, b->n_instrs ? b->instrs[0] : nullptr, b,
            vi->func, CORE_STRING_STATIC("this block"),
            CORE_STRING_STATIC("no path from the entry block reaches "),
            b->name, CORE_STRING_STATIC(""));
    }

    return false;
}

static bool analyze_operand(ir_visit_t *vi)
{
    A          *a = vi->user;
    ir_local_t *l = ir_operand_local(vi->operand);

    if (l && !ir_local_read_earlier(vi->instr, vi->index, l)
        && ir_rd_maybe_unset(a->rd, vi->instr, l)) {
        say(a->d, IR_DIAG_UNSET_LOCAL, vi->instr, vi->block, vi->func,
            CORE_STRING_STATIC("this read"),
            l->source_name.data ? CORE_STRING_STATIC("'")
                                : CORE_STRING_STATIC("$"),
            ir_local_display_name(l),
            l->source_name.data
                ? CORE_STRING_STATIC("' is read on a path that does not assign it")
                : CORE_STRING_STATIC(" is read on a path that does not assign it"));
    }

    return true;
}

static bool analyze_instr_end(ir_visit_t *vi)
{
    A          *a  = vi->user;
    ir_instr_t *in = vi->instr;

    ir_local_t *l = in->dest_kind == IR_DEST_LOCAL ? in->dest_local : nullptr;

    if (l && !l->synthetic && !ir_rd_reaches_a_use(a->rd, in)) {
        say(a->d, IR_DIAG_UNUSED_LOCAL, in, vi->block, vi->func,
            CORE_STRING_STATIC("this assignment"),
            l->source_name.data ? CORE_STRING_STATIC("'")
                                : CORE_STRING_STATIC("$"),
            ir_local_display_name(l),
            l->source_name.data
                ? CORE_STRING_STATIC("' is assigned here and never read afterward")
                : CORE_STRING_STATIC(" is assigned here and never read afterward"));
    }

    return true;
}

bool ir_analyze(ir_module_t *m, ir_diags_t *d)
{
    if (!m || !d) {
        return false;
    }

    A          a = {.d = d};
    ir_walk_t *w = ir_walk_new(&a);

    ir_walk_on_func(w, analyze_func, analyze_func_end, nullptr);
    ir_walk_on_block(w, analyze_block, nullptr, nullptr);
    ir_walk_on_instr(w, nullptr, analyze_instr_end, nullptr);
    ir_walk_on_operand(w, analyze_operand, nullptr);
    ir_walk_run(w, m);
    ir_walk_free(w);

    return !ir_diags_failed(d);
}
