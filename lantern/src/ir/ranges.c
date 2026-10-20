#include "ir/ranges.h"

#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/memssa.h"
#include "ir/places.h"
#include "ir/summary.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/list.h"

#include <stdlib.h>

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
} range_entry_t;
core_list_decl(range_entry_t);

typedef const ir_instr_t *proof_index_ptr_t;
typedef const ir_value_t *bound_value_ptr_t;
core_list_decl(proof_index_ptr_t);
core_list_decl(bound_value_ptr_t);

typedef core_list_t(bound_value_ptr_t) bound_set_t;

struct ir_ranges_t {
    ir_func_t                      *f;
    ir_doms_t                      *doms;
    core_dict_t                     slots;
    core_list_t(range_entry_t)      entries;
    core_list_t(proof_index_ptr_t)  proven;
    bool                            proofs_built;
    bool                            narrowing;
};

typedef struct {
    const ir_value_t *sequence;
    const ir_value_t *parameter;
    const ir_value_t *actual;
} sequence_origin_t;

static ir_int_t
int_min(ir_int_t a, ir_int_t b)
{
    return a < b ? a : b;
}

static ir_int_t
int_max(ir_int_t a, ir_int_t b)
{
    return a > b ? a : b;
}

static const ir_value_t *
value_operand(const ir_instr_t *instruction, uint32_t index)
{
    return instruction && index < instruction->n_operands
        && instruction->operands[index].kind == IR_OPD_VALUE
        ? instruction->operands[index].value : nullptr;
}

static const ir_value_t *
uncopied(const ir_value_t *value)
{
    for (uint32_t i = 0; value && value->def && i < 64; i++) {
        const ir_instr_t *definition = value->def;
        if (definition->op != IR_OP_COPY) break;
        const ir_value_t *source = value_operand(definition, 0);
        if (!source || source == value) break;
        value = source;
    }
    return value;
}

static bool
same_value(const ir_value_t *left, const ir_value_t *right)
{
    return uncopied(left) == uncopied(right);
}

bool
ir_range_type_bounds(const ir_func_t *f, const ir_value_t *value,
                     ir_interval_t *out)
{
    if (!f || !value || !value->type) return false;
    ir_machine_t machine = ir_machine_of(f->owner, value->type);
    if ((machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width > 64) return false;
    if (machine.class == IR_CLASS_SINT) {
        ir_int_t half = (ir_int_t)1 << (machine.width - 1);
        *out = (ir_interval_t){.lo = -half, .hi = half - 1};
    }
    else *out = (ir_interval_t){
        .lo = 0, .hi = ((ir_int_t)1 << machine.width) - 1};
    return true;
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

static ir_op_t
swapped(ir_op_t op)
{
    switch (op) {
    case IR_OP_LT: return IR_OP_GT;
    case IR_OP_LE: return IR_OP_GE;
    case IR_OP_GT: return IR_OP_LT;
    case IR_OP_GE: return IR_OP_LE;
    default:       return op;
    }
}

static ir_op_t
negated(ir_op_t op)
{
    switch (op) {
    case IR_OP_LT: return IR_OP_GE;
    case IR_OP_LE: return IR_OP_GT;
    case IR_OP_GT: return IR_OP_LE;
    case IR_OP_GE: return IR_OP_LT;
    case IR_OP_EQ: return IR_OP_NE;
    case IR_OP_NE: return IR_OP_EQ;
    default:       return op;
    }
}

static bool
comparison(ir_op_t op)
{
    return op == IR_OP_LT || op == IR_OP_LE || op == IR_OP_GT
        || op == IR_OP_GE || op == IR_OP_EQ || op == IR_OP_NE;
}

static void
constrain(ir_interval_t *range, ir_op_t op, ir_interval_t other)
{
    switch (op) {
    case IR_OP_LT: range->hi = int_min(range->hi, other.hi - 1); break;
    case IR_OP_LE: range->hi = int_min(range->hi, other.hi); break;
    case IR_OP_GT: range->lo = int_max(range->lo, other.lo + 1); break;
    case IR_OP_GE: range->lo = int_max(range->lo, other.lo); break;
    case IR_OP_EQ:
        range->lo = int_max(range->lo, other.lo);
        range->hi = int_min(range->hi, other.hi);
        break;
    case IR_OP_NE:
        if (other.lo == other.hi && range->lo == other.lo) range->lo++;
        else if (other.lo == other.hi && range->hi == other.lo) range->hi--;
        break;
    default:
        break;
    }
}

static void
apply_condition(ir_ranges_t *r, const ir_value_t *value,
                const ir_value_t *condition, bool sense,
                ir_interval_t *range)
{
    const ir_instr_t *test = uncopied(condition)
        ? uncopied(condition)->def : nullptr;
    if (!test || !comparison(test->op) || test->n_operands != 2) return;
    const ir_value_t *left  = value_operand(test, 0);
    const ir_value_t *right = value_operand(test, 1);
    if (!left || !right || is_signed(r->f, left) != is_signed(r->f, right))
        return;
    ir_op_t op = sense ? test->op : negated(test->op);
    ir_interval_t other;
    if (same_value(left, value) && global_range(r, right, &other))
        constrain(range, op, other);
    else if (same_value(right, value) && global_range(r, left, &other))
        constrain(range, swapped(op), other);
}

static bool
edge_condition(const ir_block_t *from, const ir_block_t *to,
               const ir_value_t **condition, bool *sense)
{
    ir_instr_t *end = from ? ir_block_terminator((ir_block_t *)from)
                           : nullptr;
    if (!end || end->op != IR_OP_BRANCH || end->n_operands != 3)
        return false;
    ir_block_t *yes = ir_succ_of(&end->operands[1]);
    ir_block_t *no  = ir_succ_of(&end->operands[2]);
    if (yes == no || (to != yes && to != no)) return false;
    *condition = value_operand(end, 0);
    *sense     = to == yes;
    return *condition != nullptr;
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
            && edge_condition(parent, at, &condition, &sense))
            apply_condition(r, value, condition, sense, range);
        at = parent;
    }
}

static bool
range_at(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at,
         ir_interval_t *out)
{
    if (!global_range(r, value, out)) return false;
    if (at) refine_in_block(r, value, at->block, out);
    return true;
}

static bool
range_on_edge(ir_ranges_t *r, const ir_value_t *value, ir_block_t *from,
              ir_block_t *to, ir_interval_t *out)
{
    if (!global_range(r, value, out)) return false;
    refine_in_block(r, value, from, out);
    const ir_value_t *condition = nullptr;
    bool sense = false;
    if (edge_condition(from, to, &condition, &sense))
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
    ir_interval_t clamped = {int_max(exact.lo, type.lo),
                             int_min(exact.hi, type.hi)};
    return clamped.lo <= clamped.hi ? clamped : type;
}

static bool
small(ir_interval_t range)
{
    const ir_int_t limit = (ir_int_t)1 << 63;
    return range.lo > -limit && range.hi < limit;
}

static bool
multiply(ir_interval_t a, ir_interval_t b, ir_interval_t *out)
{
    if (!small(a) || !small(b)) return false;
    ir_int_t products[] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo,
                           a.hi * b.hi};
    *out = (ir_interval_t){products[0], products[0]};
    for (int i = 1; i < 4; i++) {
        out->lo = int_min(out->lo, products[i]);
        out->hi = int_max(out->hi, products[i]);
    }
    return true;
}

static bool
exact_arithmetic(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *left,
                 ir_interval_t *right, ir_interval_t *exact)
{
    const ir_value_t *a = value_operand(in, 0);
    const ir_value_t *b = value_operand(in, 1);
    if (!a || !range_at(r, a, in, left)) return false;
    switch (in->op) {
    case IR_OP_NEG:
        *right = (ir_interval_t){0, 0};
        *exact = (ir_interval_t){-left->hi, -left->lo};
        return true;
    case IR_OP_ADD:
    case IR_OP_SUB:
    case IR_OP_MUL:
    case IR_OP_SHL:
        break;
    default:
        return false;
    }
    if (!b || !range_at(r, b, in, right)) return false;
    switch (in->op) {
    case IR_OP_ADD:
        *exact = (ir_interval_t){left->lo + right->lo, left->hi + right->hi};
        return true;
    case IR_OP_SUB:
        *exact = (ir_interval_t){left->lo - right->hi, left->hi - right->lo};
        return true;
    case IR_OP_MUL:
        return multiply(*left, *right, exact);
    case IR_OP_SHL:
        if (left->lo < 0 || right->lo < 0 || right->hi > 62) return false;
        *exact = (ir_interval_t){left->lo << right->lo,
                                 left->hi << right->hi};
        return small(*exact);
    default:
        return false;
    }
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
    case IR_OP_CAST:
        if (range_at(r, value_operand(in, 0), in, &a)
            && a.lo >= type.lo && a.hi <= type.hi) *out = a;
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
        if (range_at(r, value_operand(in, 0), in, &a)
            && range_at(r, value_operand(in, 1), in, &b)) {
            if (a.lo >= 0 && b.lo >= 0)
                *out = (ir_interval_t){0, int_min(a.hi, b.hi)};
            else if (a.lo >= 0) *out = (ir_interval_t){0, a.hi};
            else if (b.lo >= 0) *out = (ir_interval_t){0, b.hi};
        }
        return true;
    case IR_OP_OR:
    case IR_OP_XOR:
        if (range_at(r, value_operand(in, 0), in, &a)
            && range_at(r, value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo >= 0) {
            ir_interval_t candidate = {0, mask_ceiling(int_max(a.hi, b.hi))};
            if (candidate.hi <= type.hi) *out = candidate;
        }
        return true;
    case IR_OP_SHR:
        if (range_at(r, value_operand(in, 0), in, &a)
            && range_at(r, value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo >= 0 && b.hi < 127)
            *out = (ir_interval_t){a.lo >> b.hi, a.hi >> b.lo};
        return true;
    case IR_OP_DIV:
        if (range_at(r, value_operand(in, 0), in, &a)
            && range_at(r, value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo > 0)
            *out = (ir_interval_t){a.lo / b.hi, a.hi / b.lo};
        return true;
    case IR_OP_REM:
        if (range_at(r, value_operand(in, 0), in, &a)
            && range_at(r, value_operand(in, 1), in, &b)
            && a.lo >= 0 && b.lo > 0)
            *out = (ir_interval_t){0, int_min(a.hi, b.hi - 1)};
        return true;
    case IR_OP_CLZ:
    case IR_OP_CTZ:
    case IR_OP_POPCOUNT: {
        ir_machine_t machine = ir_machine_of(r->f->owner,
            value_operand(in, 0) ? value_operand(in, 0)->type : nullptr);
        if (machine.width && (ir_int_t)machine.width <= type.hi)
            *out = (ir_interval_t){0, (ir_int_t)machine.width};
        return true;
    }
    case IR_OP_SEQUENCE_LENGTH:
        *out = (ir_interval_t){int_max(type.lo, 0), type.hi};
        return true;
    case IR_OP_SELECT:
        if (range_at(r, value_operand(in, 1), in, &a)
            && range_at(r, value_operand(in, 2), in, &b))
            *out = (ir_interval_t){int_min(a.lo, b.lo), int_max(a.hi, b.hi)};
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
        *out = any ? (ir_interval_t){int_min(out->lo, range.lo),
                                     int_max(out->hi, range.hi)}
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
        next.lo = int_max(next.lo, entry->range.lo);
        next.hi = int_min(next.hi, entry->range.hi);
        if (next.lo > next.hi) return false;
    }
    else if (entry->set) {
        next.lo = int_min(next.lo, entry->range.lo);
        next.hi = int_max(next.hi, entry->range.hi);
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

ir_ranges_t *
ir_ranges_build(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    ir_ranges_t *r = core_alloc(ir_ranges_t);
    r->f       = f;
    r->doms    = ir_func_doms(f);
    r->entries = core_list_new(range_entry_t);
    r->proven  = core_list_new(proof_index_ptr_t);
    core_dict_init(&r->slots, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++)
            track(r, f->blocks[b]->instrs[i]->dest);
    for (uint32_t round = 0; r->doms && round < MAX_PASSES; round++)
        if (!pass(r)) break;
    r->narrowing = true;
    for (uint32_t round = 0; r->doms && round < NARROW_PASSES; round++)
        if (!pass(r)) break;
    r->narrowing = false;
    return r;
}

void
ir_ranges_free(ir_ranges_t *r)
{
    if (!r) return;
    core_list_free(r->entries);
    core_list_free(r->proven);
    core_dict_free(&r->slots);
    core_free(r);
}

bool
ir_range_of(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at,
            ir_interval_t *out)
{
    if (!r || !value) return false;
    ir_interval_t global;
    if (!global_range(r, value, &global)) return false;
    if (value->kind == IR_VAL_INSTR && value->def
        && value->def->op == IR_OP_CONST) {
        ir_int_t constant = 0;
        ir_interval_t type;
        if (constant_of(value->def, &constant)
            && ir_range_type_bounds(r->f, value, &type)
            && constant >= type.lo && constant <= type.hi) {
            *out = (ir_interval_t){constant, constant};
            return true;
        }
    }
    *out = global;
    if (at) refine_in_block(r, value, at->block, out);
    return true;
}

ir_overflow_t
ir_range_overflow(ir_ranges_t *r, const ir_instr_t *in, ir_interval_t *left,
                  ir_interval_t *right)
{
    ir_interval_t type, a = {0}, b = {0}, exact;
    if (!r || !in || !in->dest
        || !ir_range_type_bounds(r->f, in->dest, &type)
        || !exact_arithmetic(r, in, &a, &b, &exact))
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
    const ir_instr_t *in = uncopied(address) ? uncopied(address)->def
                                             : nullptr;
    if (!in || in->op != IR_OP_INDEX) return false;
    *base  = value_operand(in, 0);
    *index = value_operand(in, 1);
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
        || !element_index(value_operand(store, 0), &store_base,
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
        || !global_range(query->ranges, value_operand(def->instr, 1),
                         &stored)) {
        query->unknown = true;
        return false;
    }
    query->range = query->any
        ? (ir_interval_t){int_min(query->range.lo, stored.lo),
                          int_max(query->range.hi, stored.hi)}
        : stored;
    query->any = true;
    return true;
}

static bool
element_range(ir_ranges_t *r, const ir_instr_t *load, ir_interval_t *out)
{
    element_query_t query = {.ranges = r};
    if (!element_index(value_operand(load, 0), &query.base, &query.index))
        return false;
    ir_places_t *places = ir_func_places(r->f);
    ir_memssa_t *memssa = ir_func_memssa(r->f);
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

static bool
unsigned_width(ir_module_t *module, const ir_value_t *value,
               uint64_t *width)
{
    if (!value) return false;
    ir_machine_t machine = ir_machine_of(module, value->type);
    if (machine.class != IR_CLASS_UINT || !machine.width
        || machine.width > 128) return false;
    *width = machine.width;
    return true;
}

static bool
same_address(const ir_value_t *left, const ir_value_t *right,
             unsigned depth)
{
    left  = uncopied(left);
    right = uncopied(right);
    if (left == right) return left != nullptr;
    if (!left || !right || depth == 0 || !left->def || !right->def)
        return false;
    if (left->def->op == IR_OP_ADDRESS_OF
        && right->def->op == IR_OP_ADDRESS_OF) {
        const ir_value_t *left_root  = value_operand(left->def, 0);
        const ir_value_t *right_root = value_operand(right->def, 0);
        return left_root && right_root && same_value(left_root, right_root);
    }
    if (left->def->op != IR_OP_FIELD || right->def->op != IR_OP_FIELD
        || left->def->n_operands != 2 || right->def->n_operands != 2
        || left->def->operands[1].kind != IR_OPD_NAME
        || right->def->operands[1].kind != IR_OPD_NAME
        || !core_string_eq(left->def->operands[1].name,
                           right->def->operands[1].name)) return false;
    return same_address(value_operand(left->def, 0),
                        value_operand(right->def, 0), depth - 1);
}

static bool
same_sequence(const ir_value_t *sequence, const ir_value_t *base,
              bool base_is_address)
{
    sequence = uncopied(sequence);
    base     = uncopied(base);
    if (!sequence || !base) return false;
    if (!base_is_address) return same_value(sequence, base);
    return sequence->def && sequence->def->op == IR_OP_LOAD
        && same_address(value_operand(sequence->def, 0), base, 16);
}

static bool
actual_matches_address(const ir_value_t *actual, const ir_value_t *address)
{
    actual  = uncopied(actual);
    address = uncopied(address);
    if (actual == address) return actual != nullptr;
    const ir_instr_t *definition = address ? address->def : nullptr;
    if (definition && definition->op == IR_OP_ADDRESS_OF
        && same_value(actual, value_operand(definition, 0))) return true;
    definition = actual ? actual->def : nullptr;
    return definition && definition->op == IR_OP_LOAD
        && same_address(value_operand(definition, 0), address, 16);
}

static bool
extracted_path_matches(const ir_value_t *value, const ir_value_t *parameter,
                       const ir_value_t *actual, const ir_value_t *address,
                       unsigned depth)
{
    if (!depth) return false;
    value   = uncopied(value);
    address = uncopied(address);
    if (value == parameter) return actual_matches_address(actual, address);
    const ir_instr_t *extract = value ? value->def : nullptr;
    const ir_instr_t *field   = address ? address->def : nullptr;
    if (!extract || !field || extract->op != IR_OP_EXTRACT
        || field->op != IR_OP_FIELD || extract->n_operands != 2
        || field->n_operands != 2
        || extract->operands[1].kind != IR_OPD_NAME
        || field->operands[1].kind != IR_OPD_NAME
        || !core_string_eq(extract->operands[1].name,
                           field->operands[1].name)) return false;
    return extracted_path_matches(value_operand(extract, 0), parameter,
                                  actual, value_operand(field, 0), depth - 1);
}

static bool
origin_matches_index(const sequence_origin_t *origin, const ir_instr_t *index)
{
    const ir_value_t *base = value_operand(index, 0);
    if (origin->parameter)
        return index->op == IR_OP_INDEX
            && extracted_path_matches(origin->sequence, origin->parameter,
                                      origin->actual, base, 16);
    return same_sequence(origin->sequence, base, index->op == IR_OP_INDEX);
}

static bool
reaches_block(ir_block_t *from, ir_block_t *target, bool *seen,
              uint32_t block_count)
{
    if (!from || from->index >= block_count || seen[from->index])
        return false;
    if (from == target) return true;
    seen[from->index] = true;
    ir_instr_t *end = ir_block_terminator(from);
    for (uint32_t i = 0; end && i < ir_succ_count(end); i++)
        if (reaches_block(ir_succ_at(end, i), target, seen, block_count))
            return true;
    return false;
}

static bool
effect_can_precede_index(ir_func_t *function, ir_instr_t *effect,
                         const ir_instr_t *index)
{
    ir_block_t *from   = effect->block;
    ir_block_t *target = index->block;
    if (from == target && effect->index < index->index) return true;
    bool *seen = calloc(function->n_blocks, sizeof(*seen));
    if (!seen) return true;
    ir_instr_t *end = ir_block_terminator(from);
    bool can_reach = false;
    if (from == target) {
        for (uint32_t i = 0; end && i < ir_succ_count(end); i++)
            if (reaches_block(ir_succ_at(end, i), target, seen,
                              function->n_blocks)) {
                can_reach = true;
                break;
            }
    }
    else can_reach = reaches_block(from, target, seen, function->n_blocks);
    free(seen);
    return can_reach;
}

static bool
element_or_sibling_write(const ir_instr_t *effect, const ir_value_t *index_base)
{
    if (effect->op != IR_OP_STORE && effect->op != IR_OP_SEQUENCE_SET)
        return false;
    const ir_value_t *target     = uncopied(value_operand(effect, 0));
    const ir_instr_t *target_def = target ? target->def : nullptr;
    index_base = uncopied(index_base);
    const ir_instr_t *source_field = index_base ? index_base->def : nullptr;
    if (!source_field || source_field->op != IR_OP_FIELD) return false;
    if (effect->op == IR_OP_SEQUENCE_SET)
        return same_address(value_operand(effect, 0), index_base, 16);
    if (target_def && target_def->op == IR_OP_INDEX)
        return same_address(value_operand(target_def, 0), index_base, 16);
    if (!target_def || target_def->op != IR_OP_FIELD
        || source_field->n_operands != 2 || target_def->n_operands != 2
        || source_field->operands[1].kind != IR_OPD_NAME
        || target_def->operands[1].kind != IR_OPD_NAME
        || core_string_eq(source_field->operands[1].name,
                          target_def->operands[1].name)) return false;
    const ir_value_t *source_parent = value_operand(source_field, 0);
    const ir_value_t *target_parent = value_operand(target_def, 0);
    th_type *record = source_parent
        ? th_ind_referent(source_parent->type) : nullptr;
    record = th_resolve(record);
    if (record && th_kind_of(record) == TH_NAMED)
        record = ir_type_definition(effect->block->owner->owner, record);
    return record && th_kind_of(th_resolve(record)) == TH_RECORD
        && same_address(source_parent, target_parent, 16);
}

static bool
pure_call(const ir_instr_t *call)
{
    return ir_summary_call_pure(ir_analysis_summaries(), call);
}

static const ir_value_t *
extraction_root(const ir_value_t *value)
{
    for (unsigned depth = 0; value && depth < 16; depth++) {
        value = uncopied(value);
        const ir_instr_t *definition = value ? value->def : nullptr;
        if (!definition || definition->op != IR_OP_EXTRACT) return value;
        value = value_operand(definition, 0);
    }
    return nullptr;
}

static bool
length_origin(const ir_instr_t *length, sequence_origin_t *origin)
{
    if (!length || !length->dest || !origin) return false;
    uint64_t width = 0;
    if (!unsigned_width(length->block->owner->owner, length->dest, &width))
        return false;
    if (length->op == IR_OP_SEQUENCE_LENGTH) {
        *origin = (sequence_origin_t){.sequence = value_operand(length, 0)};
        return origin->sequence != nullptr;
    }
    if (length->op != IR_OP_CALL) return false;
    const ir_value_t *callee = value_operand(length, 0);
    ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
        ? callee->owner : nullptr;
    if (!target || !pure_call(length)) return false;
    const ir_value_t *source = nullptr;
    bool returned = false;
    for (uint32_t b = 0; b < target->n_blocks; b++) {
        const ir_block_t *block = target->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_RETURN) continue;
            const ir_value_t *value = uncopied(value_operand(in, 0));
            const ir_instr_t *definition = value ? value->def : nullptr;
            if (!definition || definition->op != IR_OP_SEQUENCE_LENGTH)
                return false;
            const ir_value_t *current = value_operand(definition, 0);
            if (!current || (source && source != current)) return false;
            source   = current;
            returned = true;
        }
    }
    if (!returned) return false;
    const ir_value_t *root = extraction_root(source);
    if (!root || root->kind != IR_VAL_PARAM) return false;
    for (uint32_t i = 0; i < target->n_params; i++) {
        if (target->params[i] != root) continue;
        const ir_value_t *actual = value_operand(length, i + 1);
        if (!actual) return false;
        *origin = (sequence_origin_t){
            .sequence = source, .parameter = root, .actual = actual};
        return true;
    }
    return false;
}

static bool
sequence_stable_until(ir_func_t *function, const ir_value_t *index_base,
                      const ir_instr_t *length, const ir_instr_t *index)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *effect = block->instrs[i];
            if (block == length->block && i < length->index) continue;
            if (effect->op == IR_OP_TRAP || ir_op_is_terminator(effect->op))
                continue;
            bool clobbers = false;
            if (effect->dest_kind == IR_DEST_LOCAL) clobbers = true;
            else if (effect->op == IR_OP_CALL
                     || effect->op == IR_OP_CALLBACK_INVOKE)
                clobbers = !pure_call(effect);
            else if (ir_op_info(effect->op)->flags & IR_OPF_SIDEEFFECT)
                clobbers = !element_or_sibling_write(effect, index_base);
            if (clobbers && effect_can_precede_index(function, effect, index))
                return false;
        }
    }
    return true;
}

static bool
contains(const bound_set_t *set, const ir_value_t *value)
{
    return core_option_is_set(core_list_find(*set, value));
}

static bool
add_bound(bound_set_t *set, const ir_value_t *value)
{
    if (contains(set, value)) return true;
    return core_list_try_push(*set, value);
}

static bool
constant_one(const ir_value_t *value)
{
    value = uncopied(value);
    const ir_instr_t *in = value ? value->def : nullptr;
    ir_int_t constant = 0;
    return in && constant_of(in, &constant) && constant == 1;
}

static bool
mask_below_limit(ir_module_t *module, const ir_value_t *mask,
                 const ir_value_t *limit, unsigned depth)
{
    if (!mask || depth == 0) return false;
    const ir_instr_t *in = mask->def;
    if (!in) return false;
    if (in->op == IR_OP_COPY || in->op == IR_OP_CAST) {
        const ir_value_t *source = value_operand(in, 0);
        uint64_t source_width = 0, mask_width = 0;
        return source && unsigned_width(module, source, &source_width)
            && unsigned_width(module, mask, &mask_width)
            && mask_width >= source_width
            && mask_below_limit(module, source, limit, depth - 1);
    }
    return in->op == IR_OP_SUB
        && ir_instr_has_mod((ir_instr_t *)in, CORE_STRING_STATIC("trap"))
        && same_value(value_operand(in, 0), limit)
        && constant_one(value_operand(in, 1));
}

static bool bound_return(const ir_func_t *function, const ir_value_t *limit,
                         unsigned depth);

static bool
call_bounded(const ir_instr_t *call, const ir_value_t *limit, unsigned depth)
{
    const ir_value_t *callee = value_operand(call, 0);
    if (!callee || callee->kind != IR_VAL_FUNC || !callee->owner || !depth
        || !pure_call(call)) return false;
    const ir_func_t *function = callee->owner;
    for (uint32_t i = 0; i < function->n_params; i++)
        if (same_value(value_operand(call, i + 1), limit)
            && bound_return(function, function->params[i], depth - 1))
            return true;
    return false;
}

static bool
instruction_bounded(ir_module_t *module, const ir_instr_t *in,
                    const ir_value_t *limit, const bound_set_t *set,
                    unsigned depth)
{
    if (!in->dest) return false;
    uint64_t result_width = 0, limit_width = 0;
    if (!unsigned_width(module, in->dest, &result_width)
        || !unsigned_width(module, limit, &limit_width)
        || result_width < limit_width) return false;
    if (in->op == IR_OP_COPY || in->op == IR_OP_CAST)
        return contains(set, value_operand(in, 0));
    if (in->op == IR_OP_AND && in->n_operands == 2) {
        uint64_t left_width = 0, right_width = 0;
        if (!unsigned_width(module, value_operand(in, 0), &left_width)
            || !unsigned_width(module, value_operand(in, 1), &right_width)
            || left_width != result_width || right_width != result_width)
            return false;
        return mask_below_limit(module, value_operand(in, 0), limit, 16)
            || mask_below_limit(module, value_operand(in, 1), limit, 16);
    }
    if (in->op == IR_OP_CALL && depth > 1)
        return call_bounded(in, limit, depth - 1);
    if (in->op == IR_OP_PHI && in->n_operands) {
        for (uint32_t i = 0; i < in->n_operands; i++)
            if (in->operands[i].kind != IR_OPD_PHI_ARM
                || !contains(set, in->operands[i].phi.value)) return false;
        return true;
    }
    return false;
}

static bool
compute_bounds(const ir_func_t *function, const ir_value_t *limit,
               bound_set_t *set, unsigned depth)
{
    size_t value_count = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        value_count += function->blocks[b]->n_instrs;
    for (size_t round = 0; round <= value_count; round++) {
        bool changed = false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            const ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                const ir_instr_t *in = block->instrs[i];
                if (!in->dest || contains(set, in->dest)
                    || !instruction_bounded(function->owner, in, limit, set,
                                            depth)) continue;
                if (!add_bound(set, in->dest)) return false;
                changed = true;
            }
        }
        if (!changed) return true;
    }
    return false;
}

static bool
bound_return(const ir_func_t *function, const ir_value_t *limit,
             unsigned depth)
{
    if (!depth || !function->n_blocks) return false;
    bound_set_t set = {0};
    bool ok = compute_bounds(function, limit, &set, depth);
    bool returned = false;
    for (uint32_t b = 0; ok && b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_RETURN) continue;
            returned = true;
            if (!contains(&set, value_operand(in, 0))) ok = false;
        }
    }
    core_list_free(set);
    return ok && returned;
}

static bool
nonnegative_at(ir_ranges_t *r, const ir_value_t *value, const ir_instr_t *at)
{
    ir_interval_t range;
    return ir_range_of(r, value, at, &range) && range.lo >= 0;
}

static bool
condition_bounds(const ir_value_t *condition, bool sense,
                 const ir_value_t *index, const ir_value_t *limit,
                 const bound_set_t *set)
{
    const ir_instr_t *test = uncopied(condition)
        ? uncopied(condition)->def : nullptr;
    if (!test || !comparison(test->op) || test->n_operands != 2) return false;
    const ir_value_t *left  = value_operand(test, 0);
    const ir_value_t *right = value_operand(test, 1);
    ir_op_t op = sense ? test->op : negated(test->op);
    if (same_value(right, index)) {
        const ir_value_t *swap = left;
        left  = right;
        right = swap;
        op    = swapped(op);
    }
    if (!same_value(left, index)) return false;
    bool strict_limit = same_value(right, limit) || contains(set, right);
    if (op == IR_OP_LT) return strict_limit;
    return op == IR_OP_LE && contains(set, right);
}

static bool
guarded_by_condition(ir_ranges_t *r, const ir_instr_t *at,
                     const ir_value_t *index, const ir_value_t *limit,
                     const bound_set_t *set)
{
    if (!nonnegative_at(r, index, at)) return false;
    for (const ir_block_t *block = at->block; block && r->doms;) {
        ir_block_t *parent = ir_idom_of(r->doms, (ir_block_t *)block);
        if (!parent || parent == block) break;
        const ir_value_t *condition = nullptr;
        bool sense = false;
        if (ir_pred_count(r->doms, (ir_block_t *)block) == 1
            && edge_condition(parent, block, &condition, &sense)
            && condition_bounds(condition, sense, index, limit, set))
            return true;
        block = parent;
    }
    return false;
}

static bool
interval_bounded(ir_ranges_t *r, const ir_instr_t *at, const ir_value_t *index,
                 const ir_value_t *limit)
{
    ir_interval_t position, length;
    return ir_range_of(r, index, at, &position)
        && ir_range_of(r, limit, at, &length)
        && position.lo >= 0 && position.hi < length.lo;
}

static bool
checked_index_dominates(ir_doms_t *doms, const ir_instr_t *guard,
                        const ir_instr_t *index)
{
    if (guard == index) return false;
    if (guard->block == index->block) return guard->index < index->index;
    return ir_dominates(doms, guard->block, index->block);
}

static bool
index_bounded(ir_ranges_t *r, const ir_instr_t *length,
              const sequence_origin_t *origin, const bound_set_t *set,
              const ir_instr_t *index)
{
    ir_func_t *function = r->f;
    const ir_value_t *position = value_operand(index, 1);
    if (contains(set, position)
        || guarded_by_condition(r, index, position, length->dest, set)
        || interval_bounded(r, index, position, length->dest))
        return true;
    for (uint32_t gb = 0; gb < function->n_blocks; gb++) {
        ir_block_t *guard_block = function->blocks[gb];
        if (!ir_dominates(r->doms, length->block, guard_block)) continue;
        for (uint32_t gi = 0; gi < guard_block->n_instrs; gi++) {
            ir_instr_t *guard = guard_block->instrs[gi];
            if (guard->op != IR_OP_INDEX
                || (length->block == guard_block && length->index >= gi)
                || !checked_index_dominates(r->doms, guard, index)
                || !origin_matches_index(origin, guard)
                || !sequence_stable_until(function, value_operand(guard, 0),
                                          length, guard)) continue;
            bound_set_t guarded = {0};
            bool bounded = add_bound(&guarded, value_operand(guard, 1))
                && compute_bounds(function, length->dest, &guarded, 8)
                && contains(&guarded, position);
            core_list_free(guarded);
            if (bounded) return true;
        }
    }
    return false;
}

static void
build_proofs(ir_ranges_t *r)
{
    r->proofs_built = true;
    ir_func_t *function = r->f;
    if (!r->doms || !(ir_module_props(function->owner) & IR_PROP_SSA))
        return;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *length = block->instrs[i];
            sequence_origin_t origin = {0};
            if (!length_origin(length, &origin)) continue;
            bound_set_t set = {0};
            if (!compute_bounds(function, length->dest, &set, 8)) {
                core_list_free(set);
                continue;
            }
            for (uint32_t ib = 0; ib < function->n_blocks; ib++) {
                ir_block_t *index_block = function->blocks[ib];
                if (!ir_dominates(r->doms, block, index_block)) continue;
                for (uint32_t ii = 0; ii < index_block->n_instrs; ii++) {
                    ir_instr_t *index = index_block->instrs[ii];
                    if ((index->op != IR_OP_INDEX
                         && index->op != IR_OP_SEQUENCE_GET)
                        || (block == index_block && i >= ii)
                        || core_option_is_set(core_list_find(r->proven,
                                                             index))
                        || !origin_matches_index(&origin, index)
                        || !sequence_stable_until(function,
                               value_operand(index, 0), length, index)
                        || !index_bounded(r, length, &origin, &set, index))
                        continue;
                    core_list_push(r->proven, index);
                }
            }
            core_list_free(set);
        }
    }
}

bool
ir_range_index_proven(ir_ranges_t *r, const ir_instr_t *index)
{
    if (!r || !index) return false;
    if (!r->proofs_built) build_proofs(r);
    return core_option_is_set(core_list_find(r->proven, index));
}

bool
ir_range_unsigned_sub_safe(ir_ranges_t *r, const ir_instr_t *sub)
{
    if (!r || !sub || sub->op != IR_OP_SUB || !sub->dest
        || !ir_instr_has_mod((ir_instr_t *)sub, CORE_STRING_STATIC("trap")))
        return false;
    if (ir_range_overflow(r, sub, nullptr, nullptr) == IR_OVERFLOW_NEVER)
        return true;
    if (!(ir_module_props(sub->block->owner->owner) & IR_PROP_SSA))
        return false;
    const ir_value_t *limit      = value_operand(sub, 0);
    const ir_value_t *difference = uncopied(value_operand(sub, 1));
    const ir_instr_t *inner      = difference ? difference->def : nullptr;
    if (!inner || inner->op != IR_OP_SUB
        || !ir_instr_has_mod((ir_instr_t *)inner, CORE_STRING_STATIC("trap")))
        return false;
    const ir_value_t *bounded = value_operand(inner, 0);
    uint64_t width = 0, other_width = 0;
    ir_module_t *module = sub->block->owner->owner;
    if (!unsigned_width(module, limit, &width)
        || !unsigned_width(module, sub->dest, &other_width)
        || width != other_width
        || !unsigned_width(module, difference, &other_width)
        || width != other_width
        || !unsigned_width(module, bounded, &other_width)
        || width != other_width) return false;
    bound_set_t set = {0};
    bool safe = compute_bounds(sub->block->owner, limit, &set, 8)
        && contains(&set, bounded);
    core_list_free(set);
    return safe;
}
