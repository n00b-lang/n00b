#include "ir/ranges.h"

#include "ir/abcd.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/memssa.h"
#include "ir/operands.h"
#include "ir/places.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

enum {
    WIDEN_AFTER   = 3,
    MAX_PASSES    = 64,
    NARROW_PASSES = 2,
};

typedef struct {
    const ir_value_t *value;
    ir_interval_t     range;
    uint32_t          changes;
    bool              set;
    bool              clamped;
} range_entry_t;
core_list_decl(range_entry_t);

typedef struct {
    const ir_instr_t *user;
    uint32_t          operand;
} trap_use_t;
core_list_decl(trap_use_t);

struct ir_ranges_t {
    ir_func_t                 *f;
    ir_doms_t                 *doms;
    ir_abcd_t                 *abcd;
    ir_places_t               *places;
    ir_memssa_t               *memssa;
    core_dict_t                slots;
    core_list_t(range_entry_t) entries;
    core_dict_t                trap_uses;
    bool                       narrowing;
    bool                       clamps_known;
};

static bool
same_value(const ir_value_t *left, const ir_value_t *right)
{
    return ir_value_uncopied(left) == ir_value_uncopied(right);
}

bool
ir_range_type_bounds(const ir_func_t *f, const ir_value_t *value,
                     ir_interval_t *out)
{
    return f && value
        && ir_interval_of_integer_type(f->owner, value->type, out);
}

static bool
is_signed(const ir_func_t *f, const ir_value_t *value)
{
    return value && value->type
        && ir_machine_of(f->owner, value->type).class == IR_CLASS_SINT;
}

static range_entry_t *
entry_of(ir_ranges_t *r, const ir_value_t *value)
{
    bool found = false;
    void *slot = core_dict_get(&r->slots, (void *)value, &found);
    return found ? &r->entries.data[(uintptr_t)slot - 1] : nullptr;
}

static bool
global_range(ir_ranges_t *r, const ir_value_t *value, ir_interval_t *out)
{
    ir_interval_t type;
    if (!ir_range_type_bounds(r->f, value, &type)) return false;
    range_entry_t *entry = entry_of(r, value);
    *out = entry && entry->set ? entry->range : type;
    return true;
}

static void
constrain(ir_interval_t *range, ir_op_t op, ir_interval_t other)
{
    *range = ir_interval_constrain(*range, op, other);
}

static void
apply_condition(ir_ranges_t *r, const ir_value_t *value,
                const ir_value_t *condition, bool sense,
                ir_interval_t *range)
{
    const ir_instr_t *test = ir_condition_test(condition, &sense);
    if (!test || !ir_op_is_comparison(test->op) || test->n_operands != 2) return;
    const ir_value_t *left  = ir_value_operand(test, 0);
    const ir_value_t *right = ir_value_operand(test, 1);
    if (!left || !right || is_signed(r->f, left) != is_signed(r->f, right))
        return;
    ir_op_t op = sense ? test->op : ir_comparison_negated(test->op);
    ir_interval_t other;
    if (same_value(left, value) && global_range(r, right, &other))
        constrain(range, op, other);
    else if (same_value(right, value) && global_range(r, left, &other))
        constrain(range, ir_comparison_swapped(op), other);
}

static void
refine_in_block(ir_ranges_t *r, const ir_value_t *value,
                const ir_block_t *block, ir_interval_t *range)
{
    for (const ir_block_t *at = block; at && r->doms;) {
        ir_block_t *parent = ir_idom_of(r->doms, (ir_block_t *)at);
        if (!parent || parent == at) break;
        const ir_value_t *condition = nullptr;
        bool sense = false;
        if (ir_pred_count(r->doms, (ir_block_t *)at) == 1
            && ir_edge_condition(parent, at, &condition, &sense))
            apply_condition(r, value, condition, sense, range);
        at = parent;
    }
}

static bool
trap_bound(ir_ranges_t *r, const trap_use_t *use, ir_interval_t *out)
{
    const ir_instr_t *in = use->user;
    ir_interval_t type, other = ir_interval_point(0);
    if (!ir_range_type_bounds(r->f, in->dest, &type)) return false;
    if ((in->op == IR_OP_ADD || in->op == IR_OP_SUB)
        && !global_range(r, ir_value_operand(in, 1 - use->operand), &other))
        return false;
    switch (in->op) {
    case IR_OP_CAST:
        *out = type;
        return true;
    case IR_OP_NEG:
        *out = ir_interval_apply(IR_OP_NEG, type, other);
        return true;
    case IR_OP_ADD:
        *out = ir_interval_apply(IR_OP_SUB, type, other);
        return true;
    case IR_OP_SUB:
        *out = use->operand == 0 ? ir_interval_apply(IR_OP_ADD, type, other)
                                 : ir_interval_apply(IR_OP_SUB, other, type);
        return true;
    default:
        return false;
    }
}

static bool
executed_before(ir_ranges_t *r, const ir_instr_t *first,
                const ir_instr_t *at)
{
    if (first->block == at->block) return first->index < at->index;
    return r->doms && ir_dominates(r->doms, first->block, at->block);
}

static void
refine_by_traps(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at,
                ir_interval_t *range)
{
    core_list_t(trap_use_t) *uses = core_dict_get(&r->trap_uses,
        (void *)ir_value_uncopied(value), nullptr);
    for (size_t i = 0; uses && at && i < uses->len; i++) {
        ir_interval_t bound;
        if (!executed_before(r, uses->data[i].user, at)
            || !trap_bound(r, &uses->data[i], &bound)) continue;
        ir_interval_t met = {ir_int_greater(range->lo, bound.lo),
                             ir_int_lesser(range->hi, bound.hi)};
        if (met.lo <= met.hi) *range = met;
    }
}

static bool
range_at(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at,
         ir_interval_t *out)
{
    if (!global_range(r, value, out)) return false;
    if (at) refine_in_block(r, value, at->block, out);
    refine_by_traps(r, value, at, out);
    return true;
}

static bool
range_on_edge(ir_ranges_t *r, const ir_value_t *value, ir_block_t *from,
              ir_block_t *to, ir_interval_t *out)
{
    if (!global_range(r, value, out)) return false;
    refine_in_block(r, value, from, out);
    refine_by_traps(r, value, ir_block_terminator(from), out);
    const ir_value_t *condition = nullptr;
    bool sense = false;
    if (ir_edge_condition(from, to, &condition, &sense))
        apply_condition(r, value, condition, sense, out);
    return true;
}

static bool
constant_of(const ir_instr_t *in, ir_int_t *out)
{
    if (in->op != IR_OP_CONST || in->n_operands != 1
        || in->operands[0].kind != IR_OPD_CONST
        || in->operands[0].lit.is_float) return false;
    ir_lit_t literal = in->operands[0].lit.i;
    if (literal.mag > ((ir_uint_t)1 << 126)) return false;
    *out = literal.neg ? -(ir_int_t)literal.mag : (ir_int_t)literal.mag;
    return true;
}

static ir_interval_t
fit(ir_interval_t exact, ir_interval_t type, bool trapping)
{
    if (exact.lo >= type.lo && exact.hi <= type.hi) return exact;
    if (!trapping) return type;
    ir_interval_t clamped = {ir_int_greater(exact.lo, type.lo),
                             ir_int_lesser(exact.hi, type.hi)};
    return clamped.lo <= clamped.hi ? clamped : type;
}

static bool
exact_arithmetic(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *left,
                 ir_interval_t *right, ir_interval_t *exact)
{
    const ir_value_t *a = ir_value_operand(in, 0);
    const ir_value_t *b = ir_value_operand(in, 1);
    if (!a || !range_at(r, a, in, left)) return false;
    switch (in->op) {
    case IR_OP_NEG:
        *right = (ir_interval_t){0, 0};
        break;
    case IR_OP_ADD:
    case IR_OP_SUB:
    case IR_OP_MUL:
    case IR_OP_SHL:
        if (!b || !range_at(r, b, in, right)) return false;
        if (in->op == IR_OP_SHL
            && (left->lo < 0 || right->lo < 0 || right->hi >= 127))
            return false;
        break;
    default:
        return false;
    }
    *exact = ir_interval_apply(in->op, *left, *right);
    return exact->lo != ir_int_min && exact->hi != ir_int_max;
}

static ir_int_t
mask_ceiling(ir_int_t value)
{
    ir_int_t mask = 0;
    while (mask < value) mask = (mask << 1) | 1;
    return mask;
}

static bool element_range(ir_ranges_t *r, const ir_instr_t *load,
                          ir_interval_t *out);

static bool
transfer(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *out)
{
    ir_interval_t type;
    if (!in->dest || !ir_range_type_bounds(r->f, in->dest, &type))
        return false;
    *out = type;
    bool trapping = ir_instr_has_mod((ir_instr_t *)in,
                                     CORE_STRING_STATIC("trap"));
    ir_interval_t a, b, exact;
    ir_int_t constant = 0;
    switch (in->op) {
    case IR_OP_CONST:
        if (constant_of(in, &constant) && constant >= type.lo
            && constant <= type.hi)
            *out = (ir_interval_t){constant, constant};
        return true;
    case IR_OP_COPY:
        if (range_at(r, ir_value_operand(in, 0), in, &a)
            && a.lo >= type.lo && a.hi <= type.hi) *out = a;
        return true;
    case IR_OP_CAST:
        if (range_at(r, ir_value_operand(in, 0), in, &a))
            *out = fit(a, type, trapping);
        return true;
    case IR_OP_ADD:
    case IR_OP_SUB:
    case IR_OP_MUL:
    case IR_OP_NEG:
    case IR_OP_SHL:
        if (exact_arithmetic(r, in, &a, &b, &exact))
            *out = fit(exact, type, trapping);
        return true;
    case IR_OP_AND:
        if (range_at(r, ir_value_operand(in, 0), in, &a)
            && range_at(r, ir_value_operand(in, 1), in, &b)) {
            if (a.lo >= 0 && b.lo >= 0)
                *out = (ir_interval_t){0, ir_int_lesser(a.hi, b.hi)};
            else if (a.lo >= 0) *out = (ir_interval_t){0, a.hi};
            else if (b.lo >= 0) *out = (ir_interval_t){0, b.hi};
        }
        return true;
    case IR_OP_OR:
    case IR_OP_XOR:
        if (range_at(r, ir_value_operand(in, 0), in, &a)
            && range_at(r, ir_value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo >= 0) {
            ir_interval_t candidate = {0, mask_ceiling(ir_int_greater(a.hi, b.hi))};
            if (candidate.hi <= type.hi) *out = candidate;
        }
        return true;
    case IR_OP_SHR:
        if (range_at(r, ir_value_operand(in, 0), in, &a)
            && range_at(r, ir_value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo >= 0 && b.hi < 127)
            *out = (ir_interval_t){a.lo >> b.hi, a.hi >> b.lo};
        return true;
    case IR_OP_DIV:
        if (range_at(r, ir_value_operand(in, 0), in, &a)
            && range_at(r, ir_value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo > 0)
            *out = (ir_interval_t){a.lo / b.hi, a.hi / b.lo};
        return true;
    case IR_OP_REM:
        if (range_at(r, ir_value_operand(in, 0), in, &a)
            && range_at(r, ir_value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo > 0)
            *out = (ir_interval_t){0, ir_int_lesser(a.hi, b.hi - 1)};
        return true;
    case IR_OP_CLZ:
    case IR_OP_CTZ:
    case IR_OP_POPCOUNT: {
        ir_machine_t machine = ir_machine_of(r->f->owner,
            ir_value_operand(in, 0) ? ir_value_operand(in, 0)->type : nullptr);
        if (machine.width && (ir_int_t)machine.width <= type.hi)
            *out = (ir_interval_t){0, (ir_int_t)machine.width};
        return true;
    }
    case IR_OP_SEQUENCE_LENGTH:
        *out = (ir_interval_t){ir_int_greater(type.lo, 0), type.hi};
        return true;
    case IR_OP_SELECT:
        if (range_at(r, ir_value_operand(in, 1), in, &a)
            && range_at(r, ir_value_operand(in, 2), in, &b))
            *out = (ir_interval_t){ir_int_lesser(a.lo, b.lo), ir_int_greater(a.hi, b.hi)};
        return true;
    case IR_OP_LOAD:
        if (element_range(r, in, &a) && a.lo >= type.lo && a.hi <= type.hi)
            *out = a;
        return true;
    default:
        return true;
    }
}

static bool
phi_transfer(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *out)
{
    ir_interval_t type;
    if (!in->dest || !ir_range_type_bounds(r->f, in->dest, &type))
        return false;
    bool any = false;
    for (uint32_t i = 0; i < in->n_operands; i++) {
        if (in->operands[i].kind != IR_OPD_PHI_ARM) return false;
        const ir_value_t *arm = in->operands[i].phi.value;
        range_entry_t *entry = arm ? entry_of(r, arm) : nullptr;
        if (entry && !entry->set) continue;
        ir_interval_t range;
        if (!arm || !range_on_edge(r, arm, in->operands[i].phi.block,
                                   in->block, &range)) {
            *out = type;
            return true;
        }
        if (range.lo > range.hi) continue;
        *out = any ? (ir_interval_t){ir_int_lesser(out->lo, range.lo),
                                     ir_int_greater(out->hi, range.hi)}
                   : range;
        any = true;
    }
    if (!any) *out = type;
    return any;
}

static bool
track(ir_ranges_t *r, const ir_value_t *value)
{
    ir_interval_t type;
    if (!value || entry_of(r, value)
        || !ir_range_type_bounds(r->f, value, &type)) return true;
    core_list_push(r->entries, ((range_entry_t){
        .value = value, .range = type}));
    return core_dict_add(&r->slots, (void *)value,
                         (void *)(uintptr_t)r->entries.len);
}

static bool
update(ir_ranges_t *r, range_entry_t *entry, ir_interval_t next,
       const ir_interval_t *type)
{
    if (r->narrowing) {
        next.lo = ir_int_greater(next.lo, entry->range.lo);
        next.hi = ir_int_lesser(next.hi, entry->range.hi);
        if (next.lo > next.hi) return false;
    }
    else if (entry->set) {
        next.lo = ir_int_lesser(next.lo, entry->range.lo);
        next.hi = ir_int_greater(next.hi, entry->range.hi);
        if (++entry->changes > WIDEN_AFTER) {
            if (next.lo < entry->range.lo) next.lo = type->lo;
            if (next.hi > entry->range.hi) next.hi = type->hi;
        }
    }
    bool changed = !entry->set || next.lo != entry->range.lo
        || next.hi != entry->range.hi;
    entry->range = next;
    entry->set   = true;
    return changed;
}

static bool
pass(ir_ranges_t *r)
{
    bool changed = false;
    uint32_t n = r->f->n_blocks;
    ir_block_t **order = core_alloc_array(ir_block_t *, n ? n : 1);
    for (uint32_t b = 0; b < n; b++) {
        ir_block_t *block = r->f->blocks[b];
        if (!ir_block_reachable(r->doms, block)) continue;
        uint32_t at = ir_rpo_index(r->doms, block);
        if (at < n) order[at] = block;
    }
    for (uint32_t k = 0; k < n; k++) {
        ir_block_t *block = order[k];
        for (uint32_t i = 0; block && i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            range_entry_t *entry = in->dest ? entry_of(r, in->dest) : nullptr;
            ir_interval_t type, next;
            if (!entry || !ir_range_type_bounds(r->f, in->dest, &type))
                continue;
            bool ok = in->op == IR_OP_PHI ? phi_transfer(r, in, &next)
                                          : transfer(r, in, &next);
            if (ok && next.lo <= next.hi)
                changed |= update(r, entry, next, &type);
        }
    }
    core_free(order);
    return changed;
}

static bool
phi_arms(const ir_instr_t *phi, const ir_block_t *latch,
         const ir_value_t **init, const ir_value_t **next)
{
    if (phi->op != IR_OP_PHI || !phi->dest || phi->n_operands != 2)
        return false;
    *init = *next = nullptr;
    for (uint32_t o = 0; o < 2; o++) {
        const ir_operand_t *arm = &phi->operands[o];
        if (arm->kind != IR_OPD_PHI_ARM) return false;
        if (arm->phi.block == latch) *next = arm->phi.value;
        else *init = arm->phi.value;
    }
    return *init && *next;
}

static bool
phi_step(ir_ranges_t *r, const ir_instr_t *phi, const ir_value_t *next,
         ir_interval_t *step)
{
    const ir_value_t *advanced = ir_value_uncopied(next);
    const ir_instr_t *in = advanced ? advanced->def : nullptr;
    if (!in || (in->op != IR_OP_ADD && in->op != IR_OP_SUB)
        || !ir_instr_has_mod((ir_instr_t *)in, CORE_STRING_STATIC("trap")))
        return false;
    for (uint32_t side = 0; side < 2; side++) {
        if (!same_value(ir_value_operand(in, side), phi->dest)
            || (in->op == IR_OP_SUB && side == 1)
            || !range_at(r, ir_value_operand(in, 1 - side), in, step))
            continue;
        if (in->op == IR_OP_SUB)
            *step = ir_interval_apply(IR_OP_NEG, *step, ir_interval_point(0));
        return true;
    }
    return false;
}

static bool
trip_count(ir_ranges_t *r, ir_block_t *header, ir_block_t *latch,
           ir_int_t *count)
{
    ir_instr_t *end = ir_block_terminator(header);
    if (!end || end->op != IR_OP_BRANCH || end->n_operands != 3) return false;
    ir_block_t *yes = ir_succ_of(&end->operands[1]);
    ir_block_t *no  = ir_succ_of(&end->operands[2]);
    bool yes_inside = yes && ir_dominates(r->doms, yes, latch);
    bool no_inside  = no && ir_dominates(r->doms, no, latch);
    if (yes_inside == no_inside) return false;
    bool sense = yes_inside;
    const ir_instr_t *test = ir_condition_test(ir_value_operand(end, 0), &sense);
    if (!test || !ir_op_is_comparison(test->op) || test->n_operands != 2) return false;
    for (uint32_t i = 0; i < header->n_instrs; i++) {
        const ir_instr_t *phi = header->instrs[i];
        const ir_value_t *init = nullptr, *next = nullptr;
        ir_interval_t step, start, limit;
        if (!phi_arms(phi, latch, &init, &next)
            || !phi_step(r, phi, next, &step) || step.lo != step.hi
            || step.lo == 0) continue;
        ir_op_t op = sense ? test->op : ir_comparison_negated(test->op);
        const ir_value_t *bound = nullptr;
        if (same_value(ir_value_operand(test, 0), phi->dest))
            bound = ir_value_operand(test, 1);
        else if (same_value(ir_value_operand(test, 1), phi->dest)) {
            bound = ir_value_operand(test, 0);
            op = ir_comparison_swapped(op);
        }
        if (!bound || !range_at(r, init, end, &start)
            || !range_at(r, bound, end, &limit)) continue;
        bool rising = step.lo > 0;
        ir_int_t span = 0;
        if (rising && (op == IR_OP_LT || op == IR_OP_LE))
            span = limit.hi - start.lo + (op == IR_OP_LE);
        else if (!rising && (op == IR_OP_GT || op == IR_OP_GE))
            span = start.hi - limit.lo + (op == IR_OP_GE);
        else continue;
        ir_int_t stride = rising ? step.lo : -step.lo;
        if (limit.hi == ir_int_max || limit.lo == ir_int_min
            || start.lo == ir_int_min || start.hi == ir_int_max) continue;
        *count = span <= 0 ? 0 : (span + stride - 1) / stride;
        return true;
    }
    return false;
}

static bool
bound_loop_phis(ir_ranges_t *r)
{
    bool bounded = false;
    for (uint32_t b = 0; r->doms && b < r->f->n_blocks; b++) {
        ir_block_t *latch = r->f->blocks[b];
        ir_instr_t *end = ir_block_terminator(latch);
        for (uint32_t s = 0; end && s < ir_succ_count(end); s++) {
            ir_block_t *header = ir_succ_at(end, s);
            ir_int_t count = 0;
            if (!header || !ir_dominates(r->doms, header, latch)
                || ir_pred_count(r->doms, header) != 2
                || !trip_count(r, header, latch, &count)) continue;
            for (uint32_t i = 0; i < header->n_instrs; i++) {
                const ir_instr_t *phi = header->instrs[i];
                const ir_value_t *init = nullptr, *next = nullptr;
                ir_interval_t step, start;
                range_entry_t *entry = phi->dest ? entry_of(r, phi->dest)
                                                 : nullptr;
                if (!entry || !entry->set
                    || !phi_arms(phi, latch, &init, &next)
                    || !phi_step(r, phi, next, &step)
                    || !range_at(r, init, ir_block_terminator(header),
                                 &start)) continue;
                ir_interval_t per = {ir_int_lesser(step.lo, 0), ir_int_greater(step.hi, 0)};
                ir_interval_t reach = ir_interval_apply(IR_OP_ADD, start,
                    ir_interval_apply(IR_OP_MUL, ir_interval_point(count),
                                      per));
                ir_interval_t met = {ir_int_greater(entry->range.lo, reach.lo),
                                     ir_int_lesser(entry->range.hi, reach.hi)};
                if (met.lo > met.hi || (met.lo == entry->range.lo
                                        && met.hi == entry->range.hi))
                    continue;
                entry->range = met;
                bounded = true;
            }
        }
    }
    return bounded;
}

static void
collect_trap_uses(ir_ranges_t *r, const ir_instr_t *in)
{
    bool bounded = in->op == IR_OP_ADD || in->op == IR_OP_SUB
        || in->op == IR_OP_NEG || in->op == IR_OP_CAST;
    if (!bounded || !in->dest
        || !ir_instr_has_mod((ir_instr_t *)in, CORE_STRING_STATIC("trap")))
        return;
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_value_t *value = ir_value_uncopied(ir_value_operand(in, o));
        ir_interval_t type;
        if (!value || !ir_range_type_bounds(r->f, value, &type)) continue;
        core_list_t(trap_use_t) *uses = core_dict_get(&r->trap_uses,
            (void *)value, nullptr);
        if (!uses) {
            uses = core_alloc(core_list_t(trap_use_t));
            *uses = core_list_new(trap_use_t);
            core_dict_put(&r->trap_uses, (void *)value, uses);
        }
        core_list_push(*uses, ((trap_use_t){in, o}));
    }
}

ir_ranges_t *
ir_ranges_build(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    ir_ranges_t *r = core_alloc(ir_ranges_t);
    r->f       = f;
    r->doms    = ir_func_doms(f);
    r->entries = core_list_new(range_entry_t);
    core_dict_init(&r->slots, nullptr, nullptr);
    core_dict_init(&r->trap_uses, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            track(r, f->blocks[b]->instrs[i]->dest);
            collect_trap_uses(r, f->blocks[b]->instrs[i]);
        }
    for (uint32_t round = 0; r->doms && round < MAX_PASSES; round++)
        if (!pass(r)) break;
    r->narrowing = true;
    for (uint32_t round = 0; r->doms && round < NARROW_PASSES; round++)
        if (!pass(r)) break;
    for (uint32_t round = 0; r->doms && round < NARROW_PASSES
         && bound_loop_phis(r); round++)
        for (uint32_t narrow = 0; narrow < NARROW_PASSES; narrow++)
            if (!pass(r)) break;
    r->narrowing = false;
    return r;
}

void
ir_ranges_free(ir_ranges_t *r)
{
    if (!r) return;
    core_list_free(r->entries);
    core_dict_free(&r->slots);
    core_dict_foreach(&r->trap_uses, entry) {
        core_list_t(trap_use_t) *uses = entry->value;
        core_list_free(*uses);
        core_free(uses);
    }
    core_dict_free(&r->trap_uses);
    core_free(r);
}

static bool
constant_range(ir_ranges_t *r, const ir_value_t *value, ir_interval_t *out)
{
    ir_int_t constant = 0;
    ir_interval_t type;
    if (value->kind != IR_VAL_INSTR || !value->def
        || !constant_of(value->def, &constant)
        || !ir_range_type_bounds(r->f, value, &type)
        || constant < type.lo || constant > type.hi) return false;
    *out = ir_interval_point(constant);
    return true;
}

static bool
operand_clamped(ir_ranges_t *r, const ir_instr_t *in)
{
    for (uint32_t o = 0; o < in->n_operands; o++) {
        range_entry_t *entry = entry_of(r, ir_operand_value(&in->operands[o]));
        if (entry && entry->clamped) return true;
    }
    return false;
}

static bool
clamped_result(ir_ranges_t *r, const ir_instr_t *in)
{
    ir_interval_t a, b, exact, type;
    bool trapping = ir_instr_has_mod((ir_instr_t *)in,
                                     CORE_STRING_STATIC("trap"));
    switch (in->op) {
    case IR_OP_ADD:
    case IR_OP_SUB:
    case IR_OP_MUL:
    case IR_OP_NEG:
    case IR_OP_SHL:
        if (trapping && exact_arithmetic(r, in, &a, &b, &exact)
            && ir_range_type_bounds(r->f, in->dest, &type)
            && (exact.lo < type.lo || exact.hi > type.hi)) return true;
        return operand_clamped(r, in);
    case IR_OP_CAST:
        if (trapping && range_at(r, ir_value_operand(in, 0), in, &a)
            && ir_range_type_bounds(r->f, in->dest, &type)
            && (a.lo < type.lo || a.hi > type.hi)) return true;
        return operand_clamped(r, in);
    case IR_OP_COPY:
    case IR_OP_PHI:
    case IR_OP_SELECT:
        return operand_clamped(r, in);
    default:
        return false;
    }
}

static void
compute_clamps(ir_ranges_t *r)
{
    r->clamps_known = true;
    for (uint32_t round = 0, changed = 1; changed && round < MAX_PASSES;
         round++) {
        changed = 0;
        for (uint32_t b = 0; b < r->f->n_blocks; b++)
            for (uint32_t i = 0; i < r->f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = r->f->blocks[b]->instrs[i];
                range_entry_t *entry = in->dest ? entry_of(r, in->dest)
                                                : nullptr;
                if (!entry || entry->clamped || !clamped_result(r, in))
                    continue;
                entry->clamped = true;
                changed = 1;
            }
    }
}

bool
ir_range_of_conditions(ir_ranges_t *r, const ir_value_t *value,
                       const ir_instr_t *at, ir_interval_t *out)
{
    if (!r || !value || !global_range(r, value, out)) return false;
    if (constant_range(r, value, out)) return true;
    if (!r->clamps_known) compute_clamps(r);
    range_entry_t *entry = entry_of(r, value);
    if (entry && entry->clamped) (void)ir_range_type_bounds(r->f, value, out);
    if (at) refine_in_block(r, value, at->block, out);
    return true;
}

bool
ir_range_of(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at,
            ir_interval_t *out)
{
    if (!r || !value || !global_range(r, value, out)) return false;
    if (constant_range(r, value, out)) return true;
    if (at) refine_in_block(r, value, at->block, out);
    refine_by_traps(r, value, at, out);
    return true;
}

static bool
overflow_interval(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *left,
                  ir_interval_t *right, ir_interval_t *exact)
{
    const ir_value_t *a = ir_value_operand(in, 0);
    const ir_value_t *b = ir_value_operand(in, 1);
    switch (in->op) {
    case IR_OP_CAST:
        if (in->n_operands != 1 || !a || !range_at(r, a, in, left))
            return false;
        *right = ir_interval_point(0);
        *exact = *left;
        return true;
    case IR_OP_DIV:
    case IR_OP_REM: {
        if (!a || !b || !range_at(r, a, in, left)
            || !range_at(r, b, in, right)) return false;
        ir_interval_t negated = ir_interval_apply(IR_OP_NEG, *left,
                                                  ir_interval_point(0));
        bool minus_one = right->lo <= -1 && right->hi >= -1;
        *exact = !minus_one ? ir_interval_point(0)
            : right->lo == right->hi ? negated
            : ir_interval_join(negated, ir_interval_point(0));
        return true;
    }
    default:
        return exact_arithmetic(r, in, left, right, exact);
    }
}

static bool
range_within(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at,
             ir_interval_t bounds)
{
    ir_interval_t range;
    return r && value && range_at(r, value, at, &range)
        && range.lo >= bounds.lo && range.hi <= bounds.hi;
}

static bool
range_excludes(ir_ranges_t *r, const ir_value_t *value,
               const ir_instr_t *at, ir_int_t point)
{
    ir_interval_t range;
    return r && value && range_at(r, value, at, &range)
        && (point < range.lo || point > range.hi);
}

ir_overflow_t
ir_range_overflow(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *left,
                  ir_interval_t *right)
{
    ir_interval_t type, a = {0}, b = {0}, exact;
    if (!r || !in || !in->dest
        || !ir_range_type_bounds(r->f, in->dest, &type)
        || !overflow_interval(r, in, &a, &b, &exact))
        return IR_OVERFLOW_UNKNOWN;
    if (left) *left = a;
    if (right) *right = b;
    if (exact.lo >= type.lo && exact.hi <= type.hi) return IR_OVERFLOW_NEVER;
    if (exact.lo > type.hi || exact.hi < type.lo) return IR_OVERFLOW_ALWAYS;
    return IR_OVERFLOW_MAY;
}

static bool
element_index(const ir_value_t *address, const ir_value_t **base,
              const ir_value_t **index)
{
    const ir_instr_t *in = ir_value_uncopied(address) ? ir_value_uncopied(address)->def
                                             : nullptr;
    if (!in || in->op != IR_OP_INDEX) return false;
    *base  = ir_value_operand(in, 0);
    *index = ir_value_operand(in, 1);
    return *base && *index;
}

typedef struct {
    ir_ranges_t      *ranges;
    const ir_value_t *base;
    const ir_value_t *index;
    ir_interval_t     range;
    bool              any;
    bool              unknown;
} element_query_t;

static ir_element_relation_t
relate_elements(void *context, const ir_instr_t *store, const ir_instr_t *load)
{
    element_query_t *query = context;
    const ir_value_t *store_base = nullptr, *store_index = nullptr;
    if (!store || store->op != IR_OP_STORE
        || !element_index(ir_value_operand(store, 0), &store_base,
                          &store_index)
        || !same_value(store_base, query->base)) return IR_ELEMENT_MAYBE;
    if (same_value(store_index, query->index)) return IR_ELEMENT_SAME;
    ir_interval_t written, read;
    if (ir_range_of(query->ranges, store_index, store, &written)
        && ir_range_of(query->ranges, query->index, load, &read)
        && (written.hi < read.lo || read.hi < written.lo))
        return IR_ELEMENT_DISJOINT;
    return IR_ELEMENT_MAYBE;
}

static bool
collect_element_store(void *context, const ir_memdef_t *def)
{
    element_query_t *query = context;
    ir_interval_t stored;
    if (def->kind != IR_MEMDEF_STORE || !def->instr
        || def->instr->op != IR_OP_STORE
        || !global_range(query->ranges, ir_value_operand(def->instr, 1),
                         &stored)) {
        query->unknown = true;
        return false;
    }
    query->range = query->any
        ? (ir_interval_t){ir_int_lesser(query->range.lo, stored.lo),
                          ir_int_greater(query->range.hi, stored.hi)}
        : stored;
    query->any = true;
    return true;
}

static bool
element_range(ir_ranges_t *r, const ir_instr_t *load, ir_interval_t *out)
{
    element_query_t query = {.ranges = r};
    if (!element_index(ir_value_operand(load, 0), &query.base, &query.index))
        return false;
    if (!r->memssa) {
        r->places = ir_func_places(r->f);
        r->memssa = ir_func_memssa(r->f);
    }
    ir_places_t *places = r->places;
    ir_memssa_t *memssa = r->memssa;
    uint32_t first = 0;
    uint32_t count = places ? ir_places_accesses_of(places, load, &first) : 0;
    for (uint32_t a = first; memssa && a < first + count; a++)
        if (ir_places_access_at(places, a)->kind == IR_ACCESS_LOAD)
            ir_memssa_visit_element_defs(memssa, a, relate_elements,
                                         collect_element_store, &query);
    if (query.unknown || !query.any) return false;
    *out = query.range;
    return true;
}

ir_abcd_t *
ir_range_abcd(ir_ranges_t *r)
{
    if (r && !r->abcd) r->abcd = ir_func_abcd(r->f);
    return r ? r->abcd : nullptr;
}

bool
ir_range_index_proven(ir_ranges_t *r, const ir_instr_t *index)
{
    return r && index && ir_abcd_check_redundant(ir_range_abcd(r), index);
}

bool
ir_range_unsigned_sub_safe(ir_ranges_t *r, const ir_instr_t *sub)
{
    if (!r || !sub || sub->op != IR_OP_SUB || !sub->dest
        || !ir_instr_has_mod((ir_instr_t *)sub, CORE_STRING_STATIC("trap"))
        || ir_machine_of(r->f->owner, sub->dest->type).class
            != IR_CLASS_UINT)
        return false;
    if (ir_range_overflow(r, sub, nullptr, nullptr) == IR_OVERFLOW_NEVER)
        return true;
    if (!(ir_module_props(sub->block->owner->owner) & IR_PROP_SSA))
        return false;
    return ir_abcd_at_most(ir_range_abcd(r), ir_value_operand(sub, 1),
                           ir_value_operand(sub, 0), 0, sub);
}

bool
ir_range_site_proven(ir_ranges_t *r, const ir_instr_t *in,
                     ir_site_kind_t kind)
{
    uint32_t operand = 0;
    if (!r || !in || !in->n_operands || !ir_instr_site(in, kind, &operand))
        return false;
    const ir_value_t *value = ir_value_operand(in, operand);
    const ir_value_t *subject = ir_value_operand(in, 0);
    ir_machine_t machine = subject
        ? ir_machine_of(r->f->owner, subject->type) : (ir_machine_t){0};
    switch (kind) {
    case IR_SITE_DIVISOR:
        return range_excludes(r, value, in, 0)
            || (machine.class == IR_CLASS_UINT
                && ir_abcd_at_least(ir_range_abcd(r), value, nullptr, 1,
                                    in));
    case IR_SITE_SHIFT_AMOUNT:
        return machine.width
            && range_within(r, value, in,
                            (ir_interval_t){0, machine.width - 1});
    case IR_SITE_ADDRESS:
        return operand == 1 && ir_range_index_proven(r, in);
    default:
        return false;
    }
}

bool
ir_range_trap_proven(ir_ranges_t *r, const ir_instr_t *in)
{
    if (!r || !in || !(ir_module_props(r->f->owner) & IR_PROP_SSA))
        return false;
    if (ir_instr_site(in, IR_SITE_ADDRESS, nullptr)
        && ir_range_site_proven(r, in, IR_SITE_ADDRESS))
        return true;
    if (!in->dest) return false;
    bool fits = ir_range_overflow(r, in, nullptr, nullptr) == IR_OVERFLOW_NEVER
        || ir_abcd_overflow_safe(ir_range_abcd(r), in);
    if (ir_instr_site(in, IR_SITE_DIVISOR, nullptr))
        return fits && ir_range_site_proven(r, in, IR_SITE_DIVISOR);
    if (ir_instr_site(in, IR_SITE_SHIFT_AMOUNT, nullptr))
        return ir_range_site_proven(r, in, IR_SITE_SHIFT_AMOUNT)
            && (in->op == IR_OP_SHR || fits);
    switch (in->op) {
    case IR_OP_ADD:
    case IR_OP_MUL:
    case IR_OP_NEG:
    case IR_OP_CAST:
        return fits;
    case IR_OP_SUB:
        return fits || ir_range_unsigned_sub_safe(r, in);
    default:
        return false;
    }
}
