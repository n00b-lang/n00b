#include <stdlib.h>

#include "private/ir/internal.h"
#include "ir/analyze.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/liveness.h"
#include "ir/pdg_memory.h"
#include "ir/operands.h"
#include "ir/places.h"
#include "ir/walk.h"
#include "lib/dict.h"

static ir_diag_t *say(ir_diags_t *d, ir_diagcode_t code, const ir_instr_t *in,
                      const ir_block_t *b, const ir_func_t *f,
                      ir_message_t message)
{
    ir_diag_t *dg = ir_diag_begin_at(d, code, in, b, f, IR_NO_LABEL);

    if (dg) {
        ir_diag_message(dg, d, message);
    }
    return dg;
}

static void note_declaration(ir_diags_t *d, ir_diag_t *dg,
                             const ir_local_t *l)
{
    if (!dg || !ir_span_known(l->span)) {
        return;
    }
    core_string_t name = ir_local_label(l);
    ir_diag_secondary(d, dg, l->span,
                      IR_MESSAGE_ARGS(DECLARED_HERE, CORE_TEXT_STRING(name)));
    core_free(name.data);
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
    a->rd     = ir_pdg_rd(ir_func_pdg(f));

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
            vi->func,
            IR_MESSAGE_ARGS(DEAD_BLOCK, CORE_TEXT_STRING(b->name)));
    }

    return false;
}

static bool analyze_operand(ir_visit_t *vi)
{
    A          *a = vi->user;
    ir_local_t *l = ir_operand_local(vi->operand);

    if (l && !ir_local_read_earlier(vi->instr, vi->index, l)
        && ir_rd_maybe_unset(a->rd, vi->instr, l)
        && !th_zero_size(vi->func->owner->ctx->types, l->type)) {
        ir_annot_t *copyout = ir_annot_get(&vi->instr->annots,
            CORE_STRING_STATIC("ir.copyout_read"));
        bool returned = vi->instr->op == IR_OP_RETURN
            || (copyout && copyout->kind == IR_ANNOT_BOOL && copyout->b);
        if (!l->synthetic || (returned && l->source_name.u8_bytes)) {
            core_string_t name = ir_local_label(l);
            ir_diag_t *dg = say(a->d, IR_DIAG_UNSET_LOCAL, vi->instr,
                vi->block, vi->func, returned
                    ? IR_MESSAGE_ARGS(UNSET_AT_RETURN, CORE_TEXT_STRING(name))
                    : IR_MESSAGE_ARGS(UNSET_READ, CORE_TEXT_STRING(name)));
            core_free(name.data);
            note_declaration(a->d, dg, l);
        }
        else {
            say(a->d, IR_DIAG_UNSET_LOCAL, vi->instr, vi->block, vi->func,
                IR_MESSAGE(UNSET_TEMPORARY));
        }
    }

    return true;
}

static bool analyze_instr_end(ir_visit_t *vi)
{
    A          *a  = vi->user;
    ir_instr_t *in = vi->instr;

    ir_local_t *l = in->dest_kind == IR_DEST_LOCAL ? in->dest_local : nullptr;

    if (l && !l->synthetic && !ir_rd_reaches_a_use(a->rd, in)) {
        core_string_t name = ir_local_display_name(l);
        ir_span_t span = ir_diag_name_span(in->span, name);
        ir_diag_t *dg = ir_span_known(span)
            ? ir_diag_begin(a->d, IR_DIAG_UNUSED_LOCAL, span, IR_NO_LABEL)
            : ir_diag_begin_at(a->d, IR_DIAG_UNUSED_LOCAL, in, vi->block,
                               vi->func, IR_NO_LABEL);
        if (dg) {
            core_string_t shown = ir_local_label(l);
            ir_diag_message(dg, a->d, IR_MESSAGE_ARGS(UNUSED_ASSIGNMENT,
                                                      CORE_TEXT_STRING(shown)));
            core_free(shown.data);
        }
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

static bool mark_read_store(void *context, const ir_memdef_t *def)
{
    core_dict_t *read = context;

    if (def->kind == IR_MEMDEF_STORE && def->instr) {
        core_dict_add(read, def->instr, def->instr);
    }

    return true;
}

void ir_dead_stores(ir_func_t *f, ir_dead_store_fn report, void *context)
{
    if (!f || !f->n_blocks || !report) {
        return;
    }

    size_t       scope  = ir_analysis_scope_begin();
    ir_places_t *places = ir_func_places(f);
    ir_pdg_memory_t *memssa = ir_pdg_memory(ir_func_pdg(f));
    core_dict_t  read;

    core_dict_init(&read, nullptr, nullptr);

    for (uint32_t a = 0; places && memssa
                         && a < ir_places_access_count(places); a++) {
        if (ir_places_access_at(places, a)->kind == IR_ACCESS_LOAD) {
            ir_pdg_memory_visit_clobbers(memssa, a, mark_read_store, &read);
        }
    }

    for (uint32_t b = 0; places && memssa && b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];

        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in    = block->instrs[i];
            uint32_t    first = 0;
            uint32_t    n     = ir_places_accesses_of(places, in, &first);

            if (core_dict_contains(&read, in) || !in->span.start_line
                || in->span.implied) {
                continue;
            }

            for (uint32_t a = first; a < first + n; a++) {
                const ir_access_t *access = ir_places_access_at(places, a);
                const uint32_t    *ids    = ir_places_access_places(places,
                                                                   access);

                const ir_place_t  *place  = access->count == 1
                                          ? ir_place_at(places, ids[0])
                                          : nullptr;

                if (access->kind != IR_ACCESS_STORE || access->direct
                    || !place) {
                    continue;
                }

                bool local  = place->kind == IR_PLACE_LOCAL
                    && !place->exposed;
                bool global = (place->kind == IR_PLACE_GLOBAL
                               || place->kind == IR_PLACE_THREAD)
                    && access->whole
                    && !ir_pdg_memory_store_observed(memssa, in, ids[0]);

                if (!local && !global) {
                    continue;
                }

                report(in, context);
                break;
            }
        }
    }

    core_dict_free(&read);
    ir_analysis_scope_end(scope);
}

ir_span_t ir_func_report_span(const ir_func_t *f)
{
    ir_span_t span = f->span;

    for (uint32_t b = 0; !span.start_line && b < f->n_blocks; b++) {
        for (uint32_t i = 0; !span.start_line && i < f->blocks[b]->n_instrs;
             i++) {
            span = f->blocks[b]->instrs[i]->span;
        }
    }

    return span;
}

bool ir_report_unbounded_recursion(ir_summaries_t *summaries, ir_func_t *f,
                                   ir_diags_t *d)
{
    ir_summary_t *summary = ir_summary_of(summaries, f);

    if (!summary || !d
        || !(ir_summary_flags(summary) & IR_SUMMARY_UNBOUNDED_RECURSION)) {
        return false;
    }

    ir_span_t span = IR_SPAN_UNKNOWN;

    for (uint32_t b = 0; !ir_span_known(span) && b < f->n_blocks; b++) {
        for (uint32_t i = 0; !ir_span_known(span)
             && i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = f->blocks[b]->instrs[i];
            const ir_value_t *callee = in->op == IR_OP_CALL && in->n_operands
                ? ir_operand_value(&in->operands[0]) : nullptr;

            if (callee && callee->kind == IR_VAL_FUNC && callee->owner == f) {
                span = in->span;
            }
        }
    }
    if (!ir_span_known(span)) {
        span = ir_func_report_span(f);
    }

    ir_diag_t *dg = ir_diag_begin(d, IR_DIAG_UNBOUNDED_RECURSION, span,
                                  IR_MESSAGE(CALLS_ITSELF_AGAIN));

    if (!dg) {
        return false;
    }
    ir_diag_message(dg, d, IR_MESSAGE_ARGS(UNBOUNDED_RECURSION,
                                           CORE_TEXT_STRING(f->name)));

    return true;
}

static void note_dead_store(const ir_instr_t *store, void *context)
{
    core_dict_put(context, (void *)store, 1);
}

uint32_t ir_unused_params(const ir_func_t *f, uint32_t *out,
                          uint32_t capacity)
{
    if (!f || !f->n_blocks || !f->n_params) {
        return 0;
    }

    bool *used     = core_alloc_array(bool, f->n_params);
    bool *stored   = core_alloc_array(bool, f->n_params);
    bool  pending  = false;

    for (uint32_t b = 0; b < f->n_blocks; b++) {
        const ir_block_t *block = f->blocks[b];

        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_value_t *value = ir_operand_used_value(in, o);

                if (value && value->kind == IR_VAL_PARAM
                    && value->index < f->n_params
                    && f->params[value->index] == value) {
                    bool *mark = in->op == IR_OP_STORE && o == 1
                        ? stored : used;
                    mark[value->index] = true;
                }
            }
        }
    }

    for (uint32_t p = 0; p < f->n_params; p++) {
        pending |= stored[p] && !used[p];
    }

    if (pending) {
        core_dict_t dead;

        core_dict_init(&dead, nullptr, nullptr);
        ir_dead_stores((ir_func_t *)f, note_dead_store, &dead);
        for (uint32_t b = 0; b < f->n_blocks; b++) {
            for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in    = f->blocks[b]->instrs[i];
                const ir_value_t *value = in->op == IR_OP_STORE
                                              && in->n_operands > 1
                    ? ir_operand_used_value(in, 1) : nullptr;

                if (value && value->kind == IR_VAL_PARAM
                    && value->index < f->n_params
                    && !core_dict_contains(&dead, (void *)in)) {
                    used[value->index] = true;
                }
            }
        }
        core_dict_free(&dead);
    }

    uint32_t count = 0;

    for (uint32_t p = 0; p < f->n_params && count < capacity; p++) {
        if (!used[p]) {
            out[count++] = p;
        }
    }
    core_free(used);
    core_free(stored);

    return count;
}
