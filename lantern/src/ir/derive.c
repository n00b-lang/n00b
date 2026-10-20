#include <string.h>

#include "private/ir/internal.h"
#include "ir/pass.h"

static void reindex(ir_block_t *b)
{
    for (uint32_t i = 0; i < b->n_instrs; i++) {
        b->instrs[i]->index = i;
    }
}

ir_instr_t *ir_instr_insert(ir_block_t *b, ir_op_t op, uint32_t at)
{
    ir_instr_t *in = ir_instr_add(b, op);

    if (!in || at >= b->n_instrs) {
        return in;
    }

    for (uint32_t i = b->n_instrs - 1; i > at; i--) {
        b->instrs[i] = b->instrs[i - 1];
    }
    b->instrs[at] = in;
    reindex(b);

    return in;
}

ir_span_t ir_span_cover(ir_span_t a, ir_span_t b)
{
    if (!ir_span_known(a)) {
        return b;
    }

    if (!ir_span_known(b) || a.file != b.file) {
        return a;
    }

    ir_span_t out = a;

    if (b.start_byte < a.start_byte) {
        out.start_byte = b.start_byte;
        out.start_line = b.start_line;
        out.start_col  = b.start_col;
    }

    if (b.end_byte > a.end_byte) {
        out.end_byte = b.end_byte;
        out.end_line = b.end_line;
        out.end_col  = b.end_col;
    }
    out.implied = a.implied && b.implied;

    return out;
}

ir_instr_t *ir_instr_derived(ir_block_t *b, ir_op_t op, const ir_instr_t *from,
                             uint32_t at)
{
    ir_instr_t *in = ir_instr_insert(b, op, at);

    if (in && from) {
        in->span   = from->span;
        in->origin = from->origin;
    }

    return in;
}

ir_instr_t *ir_instr_generated(ir_block_t *b, ir_op_t op, ir_span_t near,
                               core_string_t pass, uint32_t at)
{
    ir_instr_t *in = ir_instr_insert(b, op, at);

    if (!in) {
        return nullptr;
    }
    in->span = near;

    if (!ir_span_known(in->span) && b) {
        in->span = ir_span_known(b->span) ? b->span
                 : b->owner               ? b->owner->span
                                          : IR_SPAN_UNKNOWN;
    }
    in->origin = ir_origin_new(b->owner->owner->ctx, IR_ORIGIN_GENERATED_BY,
                               in->span, pass, nullptr);

    return in;
}

bool ir_instr_move(ir_instr_t *in, ir_block_t *to, uint32_t at)
{
    if (!in || !to || !in->block) {
        return false;
    }

    ir_block_t *from = in->block;
    uint32_t    seen = 0;

    for (uint32_t i = 0; i < from->n_instrs; i++) {
        if (from->instrs[i] == in) {
            seen = 1;
            continue;
        }
        from->instrs[i - seen] = from->instrs[i];
    }

    if (!seen) {
        return false;
    }
    from->n_instrs--;
    reindex(from);

    ir_ctx_t *ctx = to->owner->owner->ctx;
    size_t    cap = to->cap_instrs;

    to->instrs     = ir_grow(ctx, to->instrs, &cap, to->n_instrs + 1u,
                             sizeof(ir_instr_t *));
    to->cap_instrs = (uint32_t)cap;

    uint32_t where = at < to->n_instrs ? at : to->n_instrs;

    for (uint32_t i = to->n_instrs; i > where; i--) {
        to->instrs[i] = to->instrs[i - 1];
    }
    to->instrs[where] = in;
    to->n_instrs++;
    in->block         = to;
    reindex(to);

    return true;
}

bool ir_instr_inlined_from(ir_instr_t *in, ir_span_t call_site,
                           core_string_t agent)
{
    if (!in || !in->block || !agent.data) {
        return false;
    }

    ir_origin_t *o = ir_origin_new(in->block->owner->owner->ctx,
                                   IR_ORIGIN_INLINED_FROM, call_site, agent,
                                   in->origin);

    if (!o) {
        return false;
    }
    in->origin = o;

    return true;
}
