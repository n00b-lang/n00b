#include <stdlib.h>

#include "private/ir/internal.h"
#include "ir/analyze.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/liveness.h"
#include "ir/memssa.h"
#include "ir/operands.h"
#include "ir/places.h"
#include "ir/walk.h"
#include "lib/dict.h"

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

static bool observes_global(ir_places_t *places, const ir_instr_t *in,
                            uint32_t place, bool *killed)
{
    if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE
        || in->op == IR_OP_RETURN || in->op == IR_OP_FAIL
        || in->op == IR_OP_TRAP || in->op == IR_OP_UNREACHABLE) {
        return true;
    }

    uint32_t first = 0;
    uint32_t n     = ir_places_accesses_of(places, in, &first);

    for (uint32_t a = first; a < first + n; a++) {
        const ir_access_t *access = ir_places_access_at(places, a);
        const uint32_t    *ids    = ir_places_access_places(places, access);

        for (uint32_t p = 0; p < access->count; p++) {
            if (!ir_places_may_alias(places, ids[p], place)) {
                continue;
            }
            if (access->kind == IR_ACCESS_LOAD || !access->whole
                || access->count != 1 || ids[p] != place) {
                return true;
            }
            *killed = true;
        }
    }

    return false;
}

static bool path_observes(ir_places_t *places, const ir_block_t *block,
                          uint32_t from, uint32_t place, bool *killed)
{
    for (uint32_t i = from; i < block->n_instrs && !*killed; i++) {
        if (observes_global(places, block->instrs[i], place, killed)) {
            return true;
        }
    }

    return false;
}

static bool global_store_dead(ir_func_t *f, ir_places_t *places,
                              const ir_instr_t *store, uint32_t place)
{
    bool         *seen  = core_alloc_array(bool, f->n_blocks);
    ir_block_t  **work  = core_alloc_array(ir_block_t *, f->n_blocks + 1);
    uint32_t      depth = 0;
    bool          dead  = true;
    bool          killed = false;

    if (path_observes(places, store->block, store->index + 1, place,
                      &killed)) {
        dead = false;
    }
    else if (!killed) {
        work[depth++] = store->block;
    }

    while (dead && depth) {
        ir_instr_t *terminator = ir_block_terminator(work[--depth]);
        uint32_t    successors = terminator ? ir_succ_count(terminator) : 0;

        if (!successors) {
            dead = false;
        }
        for (uint32_t s = 0; dead && s < successors; s++) {
            ir_block_t *next = ir_succ_at(terminator, s);

            if (!next || seen[next->index]) {
                continue;
            }
            seen[next->index] = true;
            killed = false;
            if (path_observes(places, next, 0, place, &killed)) {
                dead = false;
            }
            else if (!killed) {
                work[depth++] = next;
            }
        }
    }

    core_free(seen);
    core_free(work);

    return dead;
}

static bool reads_directly(const ir_block_t *block, uint32_t from,
                           const ir_value_t *root)
{
    for (uint32_t i = from; i < block->n_instrs; i++) {
        const ir_instr_t *in = block->instrs[i];

        for (uint32_t o = 0; o < in->n_operands; o++) {
            if (ir_operand_value(&in->operands[o]) == root) {
                return true;
            }
        }
    }

    return false;
}

static bool direct_read_follows(ir_func_t *f, const ir_instr_t *store,
                                const ir_value_t *root)
{
    if (!root) {
        return true;
    }
    ir_liveness_t known = ir_live_after(ir_func_liveness(f), root, store);
    if (known != IR_LIVENESS_UNKNOWN) {
        return known == IR_LIVENESS_LIVE;
    }
    if (reads_directly(store->block, store->index + 1, root)) {
        return true;
    }

    bool        *seen  = core_alloc_array(bool, f->n_blocks);
    ir_block_t **work  = core_alloc_array(ir_block_t *, f->n_blocks + 1);
    uint32_t     depth = 0;
    bool         found = false;

    work[depth++] = store->block;
    while (!found && depth) {
        ir_instr_t *terminator = ir_block_terminator(work[--depth]);

        for (uint32_t s = 0; !found && terminator
                             && s < ir_succ_count(terminator); s++) {
            ir_block_t *next = ir_succ_at(terminator, s);

            if (!next || seen[next->index]) {
                continue;
            }
            seen[next->index] = true;
            found             = reads_directly(next, 0, root);
            work[depth++]     = next;
        }
    }
    core_free(seen);
    core_free(work);

    return found;
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
    ir_memssa_t *memssa = ir_func_memssa(f);
    core_dict_t  read;

    core_dict_init(&read, nullptr, nullptr);

    for (uint32_t a = 0; places && memssa
                         && a < ir_places_access_count(places); a++) {
        if (ir_places_access_at(places, a)->kind == IR_ACCESS_LOAD) {
            ir_memssa_visit_clobbers(memssa, a, mark_read_store, &read);
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
                    && !place->exposed
                    && !direct_read_follows(f, in, place->root);
                bool global = (place->kind == IR_PLACE_GLOBAL
                               || place->kind == IR_PLACE_THREAD)
                    && access->whole
                    && global_store_dead(f, places, in, ids[0]);

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

    ir_span_t span = ir_func_report_span(f);

    ir_diag_t *dg = ir_diag_begin(d, IR_DIAG_UNBOUNDED_RECURSION, span,
                                  CORE_STRING_STATIC("this function"));

    if (!dg) {
        return false;
    }

    core_buffer_t *sb = core_buffer_empty();

    core_buffer_puts(sb, "function '");
    core_buffer_append_string(sb, f->name);
    core_buffer_puts(sb, "' recurses with no path that returns");

    core_string_t msg = core_buffer_take_string(sb);

    ir_diag_message(dg, d, msg);
    core_free(msg.data);

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
