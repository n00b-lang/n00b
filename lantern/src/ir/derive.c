#include <string.h>

#include "private/ir/internal.h"
#include "ir/pass.h"
#include "lib/dict.h"

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

static ir_value_t **operand_use(ir_operand_t *op)
{
    switch (op->kind) {
    case IR_OPD_PHI_ARM:
        return &op->phi.value;
    case IR_OPD_NAMED_ARG:
        return &op->named.value;
    case IR_OPD_VALUE:
    case IR_OPD_SPREAD:
    case IR_OPD_FRESH:
        return ir_operand_local(op) ? nullptr : &op->value;
    default:
        return nullptr;
    }
}

typedef struct {
    ir_instr_t *in;
    uint32_t    operand;
} ir_use_t;

core_list_decl(ir_use_t);
typedef core_list_t(ir_use_t) ir_use_list_t;

struct ir_uses_t {
    core_dict_t by_value;
};

static void uses_free(void *raw)
{
    struct ir_uses_t *uses = raw;

    core_dict_foreach(&uses->by_value, item) {
        ir_use_list_t *list = item->value;
        core_list_free(*list);
        core_free(list);
    }
    core_dict_free(&uses->by_value);
    core_free(uses);
}

static ir_use_list_t *uses_list(struct ir_uses_t *uses, ir_value_t *value,
                                bool create)
{
    ir_use_list_t *list = core_dict_get(&uses->by_value, value, nullptr);

    if (!list && create) {
        list  = core_alloc(ir_use_list_t);
        *list = core_list_new(ir_use_t);
        core_dict_put(&uses->by_value, value, list);
    }

    return list;
}

static void uses_note(struct ir_uses_t *uses, ir_instr_t *in, uint32_t o)
{
    ir_value_t **use = operand_use(&in->operands[o]);

    if (use && *use) {
        core_list_push(*uses_list(uses, *use, true),
                       ((ir_use_t){in, o}));
    }
}

static struct ir_uses_t *uses_of(ir_func_t *f)
{
    if (f->uses) {
        return f->uses;
    }

    struct ir_uses_t *uses = core_alloc(struct ir_uses_t);

    core_dict_init(&uses->by_value, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
            ir_instr_t *in = f->blocks[b]->instrs[k];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                uses_note(uses, in, o);
            }
        }
    }
    f->uses = uses;
    ir_ctx_on_free(f->owner->ctx, uses_free, uses);

    return uses;
}

static ir_value_t **use_slot(const ir_func_t *f, ir_use_t use,
                             const ir_value_t *value)
{
    ir_instr_t *in    = use.in;
    ir_block_t *block = in->block;

    if (!block || block->owner != f || block->index >= f->n_blocks
        || f->blocks[block->index] != block || in->index >= block->n_instrs
        || block->instrs[in->index] != in || use.operand >= in->n_operands) {
        return nullptr;
    }

    ir_value_t **slot = operand_use(&in->operands[use.operand]);

    return slot && *slot == value ? slot : nullptr;
}

void ir_instr_use_added(ir_instr_t *in, uint32_t o)
{
    ir_func_t *f = in && in->block ? in->block->owner : nullptr;

    if (f && f->uses && o < in->n_operands) {
        uses_note(f->uses, in, o);
    }
}

void ir_instr_uses_changed(ir_instr_t *in)
{
    ir_func_t *f = in && in->block ? in->block->owner : nullptr;

    for (uint32_t o = 0; f && f->uses && o < in->n_operands; o++) {
        uses_note(f->uses, in, o);
    }
}

static void move_uses(ir_func_t *f, struct ir_uses_t *uses, ir_value_t *from,
                      ir_value_t *to, ir_use_filter_fn keep, void *state)
{
    ir_use_list_t *list = from && from != to
        ? uses_list(uses, from, false) : nullptr;
    size_t         kept = 0;

    for (size_t i = 0; list && i < list->len; i++) {
        ir_use_t      use  = list->data[i];
        ir_value_t  **slot = use_slot(f, use, from);

        if (!slot) {
            continue;
        }
        if (keep && keep(use.in, state)) {
            list->data[kept++] = use;
            continue;
        }
        *slot = to;
        if (to) {
            core_list_push(*uses_list(uses, to, true), use);
        }
    }
    if (list) {
        list->len = kept;
    }
}

void ir_value_replace_uses(ir_func_t *f, ir_value_t *from, ir_value_t *to,
                           ir_use_filter_fn keep, void *state)
{
    if (f) {
        move_uses(f, uses_of(f), from, to, keep, state);
    }
}

bool ir_value_has_uses(const ir_func_t *function, const ir_value_t *value)
{
    ir_func_t     *f    = (ir_func_t *)function;
    ir_use_list_t *list = f && value
        ? uses_list(uses_of(f), (ir_value_t *)value, false) : nullptr;
    size_t         kept = 0;
    bool           used = false;

    for (size_t i = 0; list && i < list->len; i++) {
        ir_use_t use = list->data[i];

        if (!use_slot(f, use, value)) {
            continue;
        }
        list->data[kept++] = use;
        used = used
            || use.in->operands[use.operand].kind == IR_OPD_VALUE;
    }
    if (list) {
        list->len = kept;
    }

    return used;
}

bool ir_value_visit_uses(ir_func_t *f, const ir_value_t *value,
                         ir_use_visit_fn visit, void *state)
{
    ir_use_list_t *list = f && value
        ? uses_list(uses_of(f), (ir_value_t *)value, false) : nullptr;

    for (size_t i = 0; list && i < list->len; i++) {
        ir_use_t use = list->data[i];

        if (use_slot(f, use, value) && !visit(use.in, use.operand, state)) {
            return false;
        }
    }

    return true;
}

ir_value_t *ir_value_replacement(core_dict_t *replacements, ir_value_t *value)
{
    for (ir_value_t *next; value
         && (next = core_dict_get(replacements, value, nullptr));) {
        value = next;
    }

    return value;
}

void ir_func_replace_values(ir_func_t *f, core_dict_t *replacements)
{
    struct ir_uses_t *uses = f ? uses_of(f) : nullptr;

    core_dict_foreach(replacements, item) {
        if (!uses) {
            break;
        }
        ir_value_t *from = item->key;
        move_uses(f, uses, from, ir_value_replacement(replacements, from),
                  nullptr, nullptr);
    }
}

bool ir_func_uses_consistent(ir_func_t *f)
{
    if (!f || !f->uses) {
        return true;
    }

    core_dict_t indexed;
    bool        consistent = true;

    core_dict_init(&indexed, nullptr, nullptr);
    core_dict_foreach(&f->uses->by_value, item) {
        ir_use_list_t *list = item->value;

        for (size_t i = 0; i < list->len; i++) {
            ir_use_t      use   = list->data[i];
            ir_value_t  **slot  = use_slot(f, use, item->key);
            ir_use_list_t *held = slot
                ? core_dict_get(&indexed, use.in, nullptr) : nullptr;

            if (!slot) {
                continue;
            }
            if (!held) {
                held  = core_alloc(ir_use_list_t);
                *held = core_list_new(ir_use_t);
                core_dict_put(&indexed, use.in, held);
            }
            core_list_push(*held, use);
        }
    }
    for (uint32_t b = 0; consistent && b < f->n_blocks; b++) {
        for (uint32_t k = 0; consistent && k < f->blocks[b]->n_instrs; k++) {
            ir_instr_t    *in   = f->blocks[b]->instrs[k];
            ir_use_list_t *held = core_dict_get(&indexed, in, nullptr);

            for (uint32_t o = 0; consistent && o < in->n_operands; o++) {
                ir_value_t **use  = operand_use(&in->operands[o]);
                bool         seen = !use || !*use;

                for (size_t i = 0; !seen && held && i < held->len; i++) {
                    seen = held->data[i].operand == o;
                }
                consistent = seen;
            }
        }
    }
    core_dict_foreach(&indexed, item) {
        ir_use_list_t *held = item->value;
        core_list_free(*held);
        core_free(held);
    }
    core_dict_free(&indexed);

    return consistent;
}

static void repoint_phis(ir_block_t *target, ir_block_t *from, ir_block_t *to)
{
    for (uint32_t i = 0; target && i < target->n_instrs; i++) {
        ir_instr_t *in = target->instrs[i];

        for (uint32_t o = 0; in->op == IR_OP_PHI && o < in->n_operands; o++) {
            if (in->operands[o].kind == IR_OPD_PHI_ARM
                && in->operands[o].phi.block == from) {
                in->operands[o].phi.block = to;
            }
        }
    }
}

ir_block_t *ir_block_split(ir_block_t *b, uint32_t at, core_string_t name)
{
    ir_block_t *tail = b && at <= b->n_instrs ? ir_block_add(b->owner, name)
                                              : nullptr;

    while (tail && b->n_instrs > at) {
        if (!ir_instr_move(b->instrs[at], tail, tail->n_instrs)) {
            return nullptr;
        }
    }

    ir_instr_t *last = tail && tail->n_instrs
                         ? tail->instrs[tail->n_instrs - 1]
                         : nullptr;

    for (uint32_t o = 0; last && o < last->n_operands; o++) {
        ir_operand_t *op = &last->operands[o];

        if (op->kind == IR_OPD_BLOCK || op->kind == IR_OPD_DEFAULT_BLOCK) {
            repoint_phis(op->block, b, tail);
        }
        else if (op->kind == IR_OPD_SWITCH_ARM) {
            repoint_phis(op->arm.block, b, tail);
        }
    }

    return tail;
}

bool ir_block_erase(ir_block_t *b)
{
    ir_func_t *f = b ? b->owner : nullptr;

    if (!f || b->n_instrs || b->index >= f->n_blocks
        || f->blocks[b->index] != b) {
        return false;
    }
    for (uint32_t i = b->index + 1; i < f->n_blocks; i++) {
        f->blocks[i - 1]        = f->blocks[i];
        f->blocks[i - 1]->index = i - 1;
    }
    f->n_blocks--;
    if (f->block_index) {
        core_dict_remove((core_dict_t *)f->block_index, &b->name);
    }
    b->owner = nullptr;

    return true;
}

bool ir_instr_erase(ir_instr_t *in)
{
    ir_block_t *from = in ? in->block : nullptr;
    uint32_t    seen = 0;

    for (uint32_t i = 0; from && i < from->n_instrs; i++) {
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
    in->block = nullptr;

    return true;
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
    if (from->owner != to->owner) {
        ir_instr_uses_changed(in);
    }

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
