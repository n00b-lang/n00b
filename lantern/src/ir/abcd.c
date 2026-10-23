#include "private/ir/internal.h"
#include "ir/abcd.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/memssa.h"
#include "ir/operands.h"
#include "ir/pass.h"
#include "ir/ranges.h"
#include "ir/summary.h"
#include "ir/vn.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"

#include <stdlib.h>

enum {
    STEP_BUDGET  = 4096,
    CALL_DEPTH   = 4,
    PATH_DEPTH   = 16,
    MAX_EDGES    = 64,
};

typedef enum : uint8_t {
    NODE_VALUE,
    NODE_ZERO,
    NODE_LENGTH,
} node_kind_t;

typedef struct {
    node_kind_t       kind;
    const ir_value_t *value;
} node_t;

typedef struct {
    node_t            from;
    ir_int_t          weight;
    const ir_instr_t *point;
    bool              all;
} edge_t;
core_list_decl(edge_t);

typedef const ir_value_t *value_ptr_t;
typedef const ir_instr_t *instr_ptr_t;
core_list_decl(value_ptr_t);
core_list_decl(instr_ptr_t);

typedef struct {
    const ir_value_t *sequence;
    const ir_value_t *parameter;
    const ir_value_t *actual;
} sequence_origin_t;

typedef struct {
    const ir_instr_t *length;
    sequence_origin_t origin;
} length_t;
core_list_decl(length_t);

typedef struct {
    ir_func_t *function;
    uint32_t   parameter;
    bool       known;
    bool       bounded;
    ir_int_t   offset;
} callee_bound_t;
core_list_decl(callee_bound_t);

struct ir_abcd_t {
    ir_func_t                  *f;
    ir_doms_t                  *doms;
    ir_value_numbers_t         *numbers;
    ir_ranges_t                *ranges;
    ir_memssa_t                *memssa;
    ir_places_t                *places;
    core_list_t(length_t)       lengths;
    core_list_t(instr_ptr_t)    effects;
    core_list_t(callee_bound_t) callees;
    uint32_t                    call_depth;
    bool                      **reach;
    uint32_t                    reach_count;
    core_dict_t                 offsets_of;
    core_dict_t                 checks_of;
};

typedef struct {
    bool     has_true;
    ir_int_t true_at;
    bool     has_false;
    ir_int_t false_at;
    bool     active;
    ir_int_t active_at;
} memo_t;

typedef struct {
    ir_abcd_t               *abcd;
    bool                     upper;
    core_list_t(value_ptr_t) targets;
    bool                     length_target;
    bool                     type_target;
    ir_interval_t            type_limit;
    uint32_t                 origin;
    const ir_instr_t        *access;
    core_dict_t              memo;
    uint32_t                 steps;
} query_t;

static bool
same_value(ir_abcd_t *a, const ir_value_t *left, const ir_value_t *right)
{
    return ir_value_numbers_same(a->numbers, ir_value_uncopied(left),
                                 ir_value_uncopied(right));
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
integer_value(ir_module_t *module, const ir_value_t *value)
{
    ir_machine_t machine = value ? ir_machine_of(module, value->type)
                                 : (ir_machine_t){0};
    return machine.class == IR_CLASS_UINT || machine.class == IR_CLASS_SINT;
}

static bool
same_address(ir_abcd_t *a, const ir_value_t *left, const ir_value_t *right,
             unsigned depth)
{
    left  = ir_value_uncopied(left);
    right = ir_value_uncopied(right);
    if (left == right) return left != nullptr;
    if (!left || !right) return false;
    if (same_value(a, left, right)) return true;
    if (depth == 0 || !left->def || !right->def) return false;
    if (left->def->op != IR_OP_FIELD || right->def->op != IR_OP_FIELD
        || left->def->n_operands != 2 || right->def->n_operands != 2
        || left->def->operands[1].kind != IR_OPD_NAME
        || right->def->operands[1].kind != IR_OPD_NAME
        || !core_string_eq(left->def->operands[1].name,
                           right->def->operands[1].name)) return false;
    return same_address(a, ir_value_operand(left->def, 0),
                        ir_value_operand(right->def, 0), depth - 1);
}

static bool
same_sequence(ir_abcd_t *a, const ir_value_t *sequence,
              const ir_value_t *base, bool base_is_address)
{
    sequence = ir_value_uncopied(sequence);
    base     = ir_value_uncopied(base);
    if (!sequence || !base) return false;
    if (!base_is_address) return same_value(a, sequence, base);
    return sequence->def && sequence->def->op == IR_OP_LOAD
        && same_address(a, ir_value_operand(sequence->def, 0), base,
                        PATH_DEPTH);
}

static bool
actual_matches_address(ir_abcd_t *a, const ir_value_t *actual,
                       const ir_value_t *address)
{
    actual  = ir_value_uncopied(actual);
    address = ir_value_uncopied(address);
    if (actual == address) return actual != nullptr;
    const ir_instr_t *definition = address ? address->def : nullptr;
    if (definition && definition->op == IR_OP_ADDRESS_OF
        && same_value(a, actual, ir_value_operand(definition, 0))) return true;
    definition = actual ? actual->def : nullptr;
    return definition && definition->op == IR_OP_LOAD
        && same_address(a, ir_value_operand(definition, 0), address,
                        PATH_DEPTH);
}

static bool
extracted_path_matches(ir_abcd_t *a, const ir_value_t *value,
                       const ir_value_t *parameter, const ir_value_t *actual,
                       const ir_value_t *address, unsigned depth)
{
    if (!depth) return false;
    value   = ir_value_uncopied(value);
    address = ir_value_uncopied(address);
    if (value == parameter) return actual_matches_address(a, actual, address);
    const ir_instr_t *extract = value ? value->def : nullptr;
    const ir_instr_t *field   = address ? address->def : nullptr;
    if (!extract || !field || extract->op != IR_OP_EXTRACT
        || field->op != IR_OP_FIELD || extract->n_operands != 2
        || field->n_operands != 2
        || extract->operands[1].kind != IR_OPD_NAME
        || field->operands[1].kind != IR_OPD_NAME
        || !core_string_eq(extract->operands[1].name,
                           field->operands[1].name)) return false;
    return extracted_path_matches(a, ir_value_operand(extract, 0), parameter,
                                  actual, ir_value_operand(field, 0), depth - 1);
}

static bool
addressed(const ir_instr_t *access)
{
    return access->op == IR_OP_INDEX || access->op == IR_OP_SEQUENCE_SET;
}

static bool
origin_matches_access(ir_abcd_t *a, const sequence_origin_t *origin,
                      const ir_instr_t *access)
{
    const ir_value_t *base = ir_value_operand(access, 0);
    if (origin->parameter)
        return addressed(access)
            && extracted_path_matches(a, origin->sequence, origin->parameter,
                                      origin->actual, base, PATH_DEPTH);
    return same_sequence(a, origin->sequence, base, addressed(access));
}

static const bool *
successors_reach(ir_abcd_t *a, const ir_block_t *from)
{
    uint32_t count = a->f->n_blocks;
    if (!a->reach) {
        a->reach = core_alloc_array(bool *, count ? count : 1);
        a->reach_count = count;
    }
    if (a->reach[from->index]) return a->reach[from->index];
    bool *seen = core_alloc_array(bool, count ? count : 1);
    core_list_t(instr_ptr_t) pending = core_list_new(instr_ptr_t);
    ir_instr_t *end = ir_block_terminator((ir_block_t *)from);
    if (end) core_list_push(pending, end);
    while (pending.len) {
        const ir_instr_t *terminator = pending.data[--pending.len];
        for (uint32_t i = 0; i < ir_succ_count((ir_instr_t *)terminator);
             i++) {
            ir_block_t *next = ir_succ_at((ir_instr_t *)terminator, i);
            if (!next || next->index >= count || seen[next->index]) continue;
            seen[next->index] = true;
            ir_instr_t *last = ir_block_terminator(next);
            if (last) core_list_push(pending, last);
        }
    }
    core_list_free(pending);
    a->reach[from->index] = seen;
    return seen;
}

static bool
instruction_reaches(ir_abcd_t *a, const ir_instr_t *from,
                    const ir_instr_t *to)
{
    if (from->block == to->block && from->index < to->index) return true;
    return successors_reach(a, from->block)[to->block->index];
}

static bool
allocation(ir_op_t op)
{
    return op == IR_OP_ALLOC || op == IR_OP_ALLOCA || op == IR_OP_ALLOC_USING
        || op == IR_OP_ARRAY_NEW || op == IR_OP_ARRAY_NEW_USING
        || op == IR_OP_VARIANT_NEW || op == IR_OP_VARIANT_NEW_USING
        || op == IR_OP_SEQUENCE_SHAPE_CHECK || op == IR_OP_RETAIN_REF
        || op == IR_OP_PREFETCH || op == IR_OP_SIGNAL_FENCE;
}

static bool
pure_call(const ir_instr_t *call)
{
    return ir_summary_call_pure(ir_analysis_summaries(), call);
}

static bool
element_or_sibling_write(ir_abcd_t *a, const ir_instr_t *effect,
                         const ir_value_t *base)
{
    if (effect->op != IR_OP_STORE && effect->op != IR_OP_SEQUENCE_SET)
        return false;
    const ir_value_t *target     = ir_value_uncopied(ir_value_operand(effect, 0));
    const ir_instr_t *target_def = target ? target->def : nullptr;
    base = ir_value_uncopied(base);
    if (effect->op == IR_OP_SEQUENCE_SET)
        return same_address(a, ir_value_operand(effect, 0), base, PATH_DEPTH);
    if (target_def && target_def->op == IR_OP_INDEX)
        return same_address(a, ir_value_operand(target_def, 0), base,
                            PATH_DEPTH);
    const ir_instr_t *source_field = base ? base->def : nullptr;
    if (!source_field || source_field->op != IR_OP_FIELD) return false;
    if (!target_def || target_def->op != IR_OP_FIELD
        || source_field->n_operands != 2 || target_def->n_operands != 2
        || source_field->operands[1].kind != IR_OPD_NAME
        || target_def->operands[1].kind != IR_OPD_NAME
        || core_string_eq(source_field->operands[1].name,
                          target_def->operands[1].name)) return false;
    const ir_value_t *source_parent = ir_value_operand(source_field, 0);
    const ir_value_t *target_parent = ir_value_operand(target_def, 0);
    th_type *record = source_parent
        ? th_ind_referent(source_parent->type) : nullptr;
    record = th_resolve(record);
    if (record && th_kind_of(record) == TH_NAMED)
        record = ir_type_definition(effect->block->owner->owner, record);
    return record && th_kind_of(th_resolve(record)) == TH_RECORD
        && same_address(a, source_parent, target_parent, PATH_DEPTH);
}

static bool
store_may_replace(ir_abcd_t *a, const ir_instr_t *effect,
                  const ir_value_t *base)
{
    if (element_or_sibling_write(a, effect, base)) return false;
    ir_places_t *places = a->places;
    const uint32_t *held = nullptr;
    uint32_t held_count = 0;
    if (!places || !ir_places_of(places, base, &held, &held_count)
        || !held_count) return true;
    uint32_t first = 0;
    uint32_t count = ir_places_accesses_of(places, effect, &first);
    for (uint32_t i = first; i < first + count; i++) {
        const ir_access_t *access = ir_places_access_at(places, i);
        if (access->kind != IR_ACCESS_STORE) continue;
        const uint32_t *written = ir_places_access_places(places, access);
        for (uint32_t w = 0; w < access->count; w++)
            for (uint32_t h = 0; h < held_count; h++)
                if (ir_places_may_alias(places, written[w], held[h]))
                    return true;
    }
    return false;
}

static bool
local_holds(const ir_instr_t *effect, const ir_value_t *base)
{
    base = ir_value_uncopied(base);
    const ir_instr_t *address = base ? base->def : nullptr;
    while (address && address->op == IR_OP_FIELD) {
        const ir_value_t *parent = ir_value_uncopied(ir_value_operand(address, 0));
        address = parent ? parent->def : nullptr;
    }
    const ir_value_t *root = address && address->op == IR_OP_ADDRESS_OF
        ? ir_value_operand(address, 0) : nullptr;
    return !root || (root->kind == IR_VAL_LOCAL
                     && root->local == effect->dest_local);
}

static bool
clobbers(ir_abcd_t *a, const ir_instr_t *effect, const ir_value_t *base,
         bool through_address)
{
    if (effect->op == IR_OP_TRAP || ir_op_is_terminator(effect->op))
        return false;
    if (effect->dest_kind == IR_DEST_LOCAL)
        return through_address && local_holds(effect, base);
    if (effect->op == IR_OP_CALL || effect->op == IR_OP_CALLBACK_INVOKE)
        return !pure_call(effect);
    if (effect->op == IR_OP_STORE || effect->op == IR_OP_SEQUENCE_SET)
        return through_address && store_may_replace(a, effect, base);
    return (ir_op_info(effect->op)->flags & IR_OPF_SIDEEFFECT)
        && !allocation(effect->op);
}

static bool
may_clobber(const ir_instr_t *effect)
{
    if (effect->op == IR_OP_TRAP || ir_op_is_terminator(effect->op))
        return false;
    return effect->dest_kind == IR_DEST_LOCAL
        || ((effect->op == IR_OP_CALL || effect->op == IR_OP_CALLBACK_INVOKE)
            && !pure_call(effect))
        || effect->op == IR_OP_STORE || effect->op == IR_OP_SEQUENCE_SET
        || ((ir_op_info(effect->op)->flags & IR_OPF_SIDEEFFECT)
            && !allocation(effect->op));
}

static bool
sequence_stable(ir_abcd_t *a, const ir_value_t *base, bool through_address,
                const ir_instr_t *from, const ir_instr_t *to)
{
    for (size_t e = 0; e < a->effects.len; e++) {
        const ir_instr_t *effect = a->effects.data[e];
        if (effect != from && instruction_reaches(a, from, effect)
            && instruction_reaches(a, effect, to)
            && clobbers(a, effect, base, through_address))
            return false;
    }
    return true;
}

static const ir_value_t *
extraction_root(const ir_value_t *value)
{
    for (unsigned depth = 0; value && depth < PATH_DEPTH; depth++) {
        value = ir_value_uncopied(value);
        const ir_instr_t *definition = value ? value->def : nullptr;
        if (!definition || definition->op != IR_OP_EXTRACT) return value;
        value = ir_value_operand(definition, 0);
    }
    return nullptr;
}

static bool
length_origin(const ir_instr_t *length, sequence_origin_t *origin)
{
    if (!length || !length->dest) return false;
    uint64_t width = 0;
    if (!unsigned_width(length->block->owner->owner, length->dest, &width))
        return false;
    if (length->op == IR_OP_SEQUENCE_LENGTH) {
        *origin = (sequence_origin_t){.sequence = ir_value_operand(length, 0)};
        return origin->sequence != nullptr;
    }
    if (length->op != IR_OP_CALL) return false;
    const ir_value_t *callee = ir_value_operand(length, 0);
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
            const ir_value_t *value = ir_value_uncopied(ir_value_operand(in, 0));
            const ir_instr_t *definition = value ? value->def : nullptr;
            if (!definition || definition->op != IR_OP_SEQUENCE_LENGTH)
                return false;
            const ir_value_t *current = ir_value_operand(definition, 0);
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
        const ir_value_t *actual = ir_value_operand(length, i + 1);
        if (!actual) return false;
        *origin = (sequence_origin_t){
            .sequence = source, .parameter = root, .actual = actual};
        return true;
    }
    return false;
}

static bool
precedes(ir_abcd_t *a, const ir_instr_t *first, const ir_instr_t *second)
{
    if (first == second) return false;
    if (first->block == second->block) return first->index < second->index;
    return ir_dominates(a->doms, first->block, second->block);
}

static bool
access_check(const ir_instr_t *in)
{
    return in->op == IR_OP_INDEX || in->op == IR_OP_SEQUENCE_GET
        || in->op == IR_OP_SEQUENCE_SET;
}

static bool
range_at(ir_abcd_t *a, const ir_value_t *value, const ir_instr_t *at,
         ir_interval_t *out)
{
    return a->ranges && value && ir_range_of(a->ranges, value, at, out);
}

static bool
exact_arithmetic(ir_abcd_t *a, const ir_instr_t *in)
{
    if (ir_instr_has_mod((ir_instr_t *)in, CORE_STRING_STATIC("trap")))
        return true;
    return a->ranges && ir_range_overflow(a->ranges, in, nullptr, nullptr)
        == IR_OVERFLOW_NEVER;
}

static void
push_edge(core_list_t(edge_t) *edges, node_t from, ir_int_t weight,
          const ir_instr_t *point, bool all)
{
    if (edges->len < MAX_EDGES)
        core_list_push(*edges, ((edge_t){from, weight, point, all}));
}

static node_t
value_node(const ir_value_t *value)
{
    return (node_t){.kind = NODE_VALUE, .value = value};
}

static void
condition_edges(query_t *q, const ir_value_t *value, const ir_instr_t *at,
                core_list_t(edge_t) *edges)
{
    ir_abcd_t *a = q->abcd;
    for (const ir_block_t *block = at ? at->block : nullptr;
         block && a->doms;) {
        ir_block_t *parent = ir_idom_of(a->doms, (ir_block_t *)block);
        if (!parent || parent == block) break;
        const ir_value_t *condition = nullptr;
        bool sense = false;
        const ir_instr_t *test = nullptr;
        if (ir_pred_count(a->doms, (ir_block_t *)block) == 1
            && ir_edge_condition(parent, block, &condition, &sense))
            test = ir_condition_test(condition, &sense);
        if (test && ir_op_is_comparison(test->op) && test->n_operands == 2) {
            const ir_value_t *left  = ir_value_operand(test, 0);
            const ir_value_t *right = ir_value_operand(test, 1);
            ir_op_t op = sense ? test->op : ir_comparison_negated(test->op);
            const ir_value_t *other = nullptr;
            if (same_value(a, left, value)) other = right;
            else if (same_value(a, right, value)) {
                other = left;
                op = ir_comparison_swapped(op);
            }
            const ir_instr_t *point = ir_block_terminator(parent);
            if (other && q->upper) {
                if (op == IR_OP_LT) push_edge(edges, value_node(other), -1,
                                              point, false);
                if (op == IR_OP_LE || op == IR_OP_EQ)
                    push_edge(edges, value_node(other), 0, point, false);
            }
            if (other && !q->upper) {
                if (op == IR_OP_GT) push_edge(edges, value_node(other), 1,
                                              point, false);
                if (op == IR_OP_GE || op == IR_OP_EQ)
                    push_edge(edges, value_node(other), 0, point, false);
            }
        }
        block = parent;
    }
}

static void
checked_access_edges(query_t *q, const ir_value_t *value,
                     const ir_instr_t *at, core_list_t(edge_t) *edges)
{
    ir_abcd_t *a = q->abcd;
    if (!q->upper || !q->length_target || !q->access) return;
    const ir_value_t *base = ir_value_operand(q->access, 0);
    uint32_t number = ir_value_number(a->numbers, ir_value_uncopied(value));
    core_list_t(instr_ptr_t) *checks = number
        ? core_dict_get(&a->checks_of, (void *)(uintptr_t)number, nullptr)
        : nullptr;
    for (size_t c = 0; checks && c < checks->len; c++) {
        const ir_instr_t *check = checks->data[c];
        if (check == q->access
            || !ir_dominates(a->doms, check->block, at->block)
            || !precedes(a, check, at)
            || addressed(check) != addressed(q->access)
            || !same_address(a, ir_value_operand(check, 0), base,
                             PATH_DEPTH)
            || !sequence_stable(a, base, addressed(q->access), check,
                                q->access))
            continue;
        push_edge(edges, (node_t){.kind = NODE_LENGTH}, -1, check, false);
    }
}

static bool callee_return_bound(ir_abcd_t *a, ir_func_t *callee,
                                uint32_t parameter, ir_int_t *offset);

static void
definition_edges(query_t *q, const ir_value_t *value,
                 core_list_t(edge_t) *edges)
{
    ir_abcd_t *a = q->abcd;
    const ir_instr_t *in = ir_value_uncopied(value) ? ir_value_uncopied(value)->def : nullptr;
    if (!in) return;
    ir_module_t *module = a->f->owner;
    const ir_value_t *left = ir_value_operand(in, 0);
    const ir_value_t *right = ir_value_operand(in, 1);
    ir_interval_t other = {0};
    switch (in->op) {
    case IR_OP_CAST: {
        ir_interval_t from = {0}, to = {0};
        bool preserving = left
            && ir_range_type_bounds(a->f, left, &from)
            && ir_range_type_bounds(a->f, value, &to)
            && ir_interval_contains(to, from);
        uint64_t from_width = 0, to_width = 0;
        bool truncating = q->upper && left
            && unsigned_width(module, left, &from_width)
            && unsigned_width(module, value, &to_width);
        if (left && integer_value(module, left)
            && (preserving || truncating
                || ir_instr_has_mod((ir_instr_t *)in,
                                    CORE_STRING_STATIC("trap"))))
            push_edge(edges, value_node(left), 0, in, false);
        break;
    }
    case IR_OP_ADD:
        if (!exact_arithmetic(a, in) || !left || !right) break;
        if (range_at(a, right, in, &other)
            && (q->upper ? other.hi != ir_int_max : other.lo != ir_int_min))
            push_edge(edges, value_node(left), q->upper ? other.hi : other.lo,
                      in, false);
        if (range_at(a, left, in, &other)
            && (q->upper ? other.hi != ir_int_max : other.lo != ir_int_min))
            push_edge(edges, value_node(right),
                      q->upper ? other.hi : other.lo, in, false);
        break;
    case IR_OP_SUB:
        if (!exact_arithmetic(a, in) || !left || !right
            || !range_at(a, right, in, &other)) break;
        if (q->upper && other.lo != ir_int_min)
            push_edge(edges, value_node(left), -other.lo, in, false);
        if (!q->upper && other.hi != ir_int_max)
            push_edge(edges, value_node(left), -other.hi, in, false);
        break;
    case IR_OP_AND: {
        uint64_t width = 0;
        if (!unsigned_width(module, value, &width)) break;
        if (q->upper) {
            if (left) push_edge(edges, value_node(left), 0, in, false);
            if (right) push_edge(edges, value_node(right), 0, in, false);
        }
        break;
    }
    case IR_OP_REM:
    case IR_OP_SHR: {
        uint64_t width = 0;
        if (!q->upper || !unsigned_width(module, value, &width)) break;
        if (left) push_edge(edges, value_node(left), 0, in, false);
        if (in->op == IR_OP_REM && right)
            push_edge(edges, value_node(right), -1, in, false);
        break;
    }
    case IR_OP_PHI:
        for (uint32_t i = 0; i < in->n_operands; i++) {
            const ir_operand_t *od = &in->operands[i];
            if (od->kind != IR_OPD_PHI_ARM || !od->phi.value) {
                edges->len = 0;
                return;
            }
            push_edge(edges, value_node(od->phi.value), 0,
                      ir_block_terminator(od->phi.block), true);
        }
        break;
    case IR_OP_SELECT: {
        const ir_value_t *third = ir_value_operand(in, 2);
        if (right && third) {
            push_edge(edges, value_node(right), 0, in, true);
            push_edge(edges, value_node(third), 0, in, true);
        }
        break;
    }
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET: {
        uint32_t access = a->memssa
            ? ir_memssa_access_of(a->memssa, in, IR_ACCESS_LOAD) : UINT32_MAX;
        uint32_t count = access == UINT32_MAX ? 0
            : ir_memssa_element_link_count(a->memssa, access);
        size_t mark = edges->len;
        for (uint32_t l = 0; l < count; l++) {
            const ir_memdef_t *store = ir_memssa_def(a->memssa,
                ir_memssa_element_link(a->memssa, access, l));
            const ir_instr_t *write = store && store->kind == IR_MEMDEF_STORE
                ? store->instr : nullptr;
            const ir_value_t *stored = !write ? nullptr
                : write->op == IR_OP_STORE ? ir_value_operand(write, 1)
                : write->op == IR_OP_SEQUENCE_SET ? ir_value_operand(write, 2)
                : nullptr;
            if (!stored) {
                edges->len = mark;
                break;
            }
            push_edge(edges, value_node(stored), 0, write, true);
        }
        break;
    }
    case IR_OP_CALL: {
        const ir_value_t *callee = left;
        ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
            ? callee->owner : nullptr;
        if (!target || !q->upper || !pure_call(in)) break;
        for (uint32_t p = 0; p < target->n_params; p++) {
            ir_int_t offset = 0;
            const ir_value_t *argument = ir_value_operand(in, p + 1);
            if (argument && callee_return_bound(a, target, p, &offset))
                push_edge(edges, value_node(argument), offset, in, false);
        }
        break;
    }
    default:
        break;
    }
}

static bool
constant_operand(ir_abcd_t *a, const ir_instr_t *in, uint32_t index,
                 ir_int_t *value)
{
    const ir_value_t *operand = ir_value_operand(in, index);
    return operand && ir_value_number_constant(a->numbers,
        ir_value_number(a->numbers, ir_value_uncopied(operand)), value);
}

static void
inverse_edges(query_t *q, const ir_value_t *value, const ir_instr_t *at,
              core_list_t(edge_t) *edges)
{
    ir_abcd_t *a = q->abcd;
    uint32_t number = ir_value_number(a->numbers, ir_value_uncopied(value));
    core_list_t(instr_ptr_t) *users = number
        ? core_dict_get(&a->offsets_of, (void *)(uintptr_t)number, nullptr)
        : nullptr;
    for (size_t i = 0; users && i < users->len; i++) {
        const ir_instr_t *user = users->data[i];
        if (!user->dest || !precedes(a, user, at)
            || !exact_arithmetic(a, user)) continue;
        ir_int_t k = 0;
        bool left_is_value = same_value(a, ir_value_operand(user, 0), value);
        if (user->op == IR_OP_SUB && left_is_value
            && constant_operand(a, user, 1, &k))
            push_edge(edges, value_node(user->dest), k, user, false);
        else if (user->op == IR_OP_ADD
                 && ((left_is_value && constant_operand(a, user, 1, &k))
                     || (!left_is_value && constant_operand(a, user, 0, &k))))
            push_edge(edges, value_node(user->dest), -k, user, false);
    }
}

static void
collect_edges(query_t *q, node_t node, const ir_instr_t *at,
              core_list_t(edge_t) *edges)
{
    ir_abcd_t *a = q->abcd;
    if (node.kind != NODE_VALUE) return;
    const ir_value_t *value = node.value;
    ir_interval_t range = {0};
    if (range_at(a, value, at, &range)) {
        if (q->upper && range.hi != ir_int_max)
            push_edge(edges, (node_t){.kind = NODE_ZERO}, range.hi, at,
                      false);
        if (!q->upper && range.lo != ir_int_min)
            push_edge(edges, (node_t){.kind = NODE_ZERO}, range.lo, at,
                      false);
    }
    definition_edges(q, value, edges);
    if (!ir_value_stable(value)) return;
    condition_edges(q, value, at, edges);
    checked_access_edges(q, value, at, edges);
    inverse_edges(q, value, at, edges);
}

static bool
is_target(query_t *q, node_t node)
{
    if (node.kind == NODE_LENGTH) return q->length_target;
    if (q->type_target) {
        if (node.kind == NODE_ZERO)
            return q->upper ? q->type_limit.hi >= 0 : q->type_limit.lo <= 0;
        ir_interval_t bounds = {0};
        uint32_t number = ir_value_number(q->abcd->numbers,
                                          ir_value_uncopied(node.value));
        return number != q->origin
            && ir_range_type_bounds(q->abcd->f, node.value, &bounds)
            && (q->upper ? bounds.hi <= q->type_limit.hi
                         : bounds.lo >= q->type_limit.lo);
    }
    if (node.kind == NODE_ZERO)
        return !q->targets.len && !q->length_target;
    for (size_t i = 0; i < q->targets.len; i++)
        if (same_value(q->abcd, node.value, q->targets.data[i])) return true;
    return false;
}

static bool
zero_reaches(query_t *q, const ir_instr_t *at, ir_int_t needed)
{
    ir_abcd_t *a = q->abcd;
    for (size_t i = 0; i < q->targets.len; i++) {
        ir_interval_t range = {0};
        if (!range_at(a, q->targets.data[i], at, &range)) continue;
        if (q->upper ? range.lo >= -needed : range.hi <= -needed)
            return true;
    }
    return q->upper && q->length_target && needed >= 0;
}

static uint64_t
memo_key(query_t *q, node_t node, const ir_instr_t *at)
{
    uint32_t number = node.kind == NODE_VALUE
        ? ir_value_number(q->abcd->numbers, ir_value_uncopied(node.value)) : 0;
    uint64_t words[3] = {((uint64_t)node.kind << 32) | number,
                         (uint64_t)(uintptr_t)at,
                         node.kind == NODE_VALUE && !number
                             ? (uint64_t)(uintptr_t)node.value : 0};
    return core_xxh3_64bits_raw(words, sizeof(words)) | 1;
}

static bool
prove(query_t *q, node_t node, const ir_instr_t *at, ir_int_t c)
{
    if (is_target(q, node)) return q->upper ? c >= 0 : c <= 0;
    if (node.kind == NODE_ZERO) return zero_reaches(q, at, c);
    if (node.kind != NODE_VALUE || !node.value) return false;
    if (++q->steps > STEP_BUDGET) return false;
    uint64_t key = memo_key(q, node, at);
    memo_t *memo = core_dict_get(&q->memo, (void *)(uintptr_t)key, nullptr);
    if (!memo) {
        memo = core_alloc(memo_t);
        core_dict_put(&q->memo, (void *)(uintptr_t)key, memo);
    }
    if (memo->has_true && (q->upper ? c >= memo->true_at
                                    : c <= memo->true_at))
        return true;
    if (memo->has_false && (q->upper ? c <= memo->false_at
                                     : c >= memo->false_at))
        return false;
    if (memo->active)
        return q->upper ? c >= memo->active_at : c <= memo->active_at;
    memo->active = true;
    memo->active_at = c;
    core_list_t(edge_t) edges = core_list_new(edge_t);
    collect_edges(q, node, at, &edges);
    bool any_all = false, all_hold = true, proven = false;
    for (size_t i = 0; i < edges.len && !proven; i++) {
        edge_t edge = edges.data[i];
        ir_int_t needed = c - edge.weight;
        if (edge.all) {
            any_all = true;
            continue;
        }
        proven = prove(q, edge.from, edge.point, needed);
    }
    for (size_t i = 0; !proven && any_all && i < edges.len && all_hold; i++) {
        edge_t edge = edges.data[i];
        if (edge.all)
            all_hold = prove(q, edge.from, edge.point, c - edge.weight);
    }
    if (!proven && any_all && all_hold) proven = true;
    core_list_free(edges);
    memo = core_dict_get(&q->memo, (void *)(uintptr_t)key, nullptr);
    memo->active = false;
    if (proven) {
        if (!memo->has_true || (q->upper ? c < memo->true_at
                                         : c > memo->true_at)) {
            memo->has_true = true;
            memo->true_at = c;
        }
    }
    else if (q->steps <= STEP_BUDGET
             && (!memo->has_false || (q->upper ? c > memo->false_at
                                               : c < memo->false_at))) {
        memo->has_false = true;
        memo->false_at = c;
    }
    return proven;
}

static void
query_init(query_t *q, ir_abcd_t *a, bool upper)
{
    *q = (query_t){.abcd = a, .upper = upper};
    q->targets = core_list_new(value_ptr_t);
    core_dict_init(&q->memo, nullptr, nullptr);
}

static void
query_free(query_t *q)
{
    core_dict_foreach(&q->memo, entry) core_free(entry->value);
    core_dict_free(&q->memo);
    core_list_free(q->targets);
}

static bool
run_query(ir_abcd_t *a, bool upper, const ir_value_t *value,
          const ir_value_t *bound, ir_int_t offset, const ir_instr_t *at)
{
    if (!a || !value || !at) return false;
    query_t q;
    query_init(&q, a, upper);
    if (bound) core_list_push(q.targets, bound);
    bool proven = prove(&q, value_node(value), at, offset);
    query_free(&q);
    return proven;
}

bool
ir_abcd_at_most(ir_abcd_t *a, const ir_value_t *value,
                const ir_value_t *bound, ir_int_t offset, const ir_instr_t *at)
{
    return run_query(a, true, value, bound, offset, at);
}

bool
ir_abcd_at_least(ir_abcd_t *a, const ir_value_t *value,
                 const ir_value_t *bound, ir_int_t offset,
                 const ir_instr_t *at)
{
    return run_query(a, false, value, bound, offset, at);
}


static bool
returns_bounded(ir_abcd_t *callee, const ir_value_t *parameter,
                ir_int_t offset)
{
    bool returns = false;
    for (uint32_t b = 0; b < callee->f->n_blocks; b++) {
        const ir_block_t *block = callee->f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (in->op != IR_OP_RETURN) continue;
            const ir_value_t *returned = ir_value_operand(in, 0);
            if (!returned
                || !ir_abcd_at_most(callee, returned, parameter, offset, in))
                return false;
            returns = true;
        }
    }
    return returns;
}

static bool
callee_return_bound(ir_abcd_t *a, ir_func_t *callee, uint32_t parameter,
                    ir_int_t *offset)
{
    for (size_t i = 0; i < a->callees.len; i++) {
        callee_bound_t *known = &a->callees.data[i];
        if (known->function != callee || known->parameter != parameter)
            continue;
        if (known->bounded) *offset = known->offset;
        return known->bounded;
    }
    callee_bound_t result = {.function = callee, .parameter = parameter,
                             .known = true};
    if (a->call_depth < CALL_DEPTH && callee->n_blocks
        && parameter < callee->n_params) {
        ir_abcd_t *inner = callee == a->f ? a : ir_func_abcd(callee);
        if (inner) {
            uint32_t depth = inner->call_depth;
            inner->call_depth = a->call_depth + 1;
            const ir_value_t *param = callee->params[parameter];
            if (returns_bounded(inner, param, -1)) {
                result.bounded = true;
                result.offset = -1;
            }
            else if (returns_bounded(inner, param, 0)) result.bounded = true;
            inner->call_depth = depth;
        }
    }
    core_list_push(a->callees, result);
    if (result.bounded) *offset = result.offset;
    return result.bounded;
}

bool
ir_abcd_check_redundant(ir_abcd_t *a, const ir_instr_t *access)
{
    if (!a || !access || !access_check(access)
        || !(ir_module_props(a->f->owner) & IR_PROP_SSA)) return false;
    const ir_annot_t *hoisted = ir_annot_get(
        &((ir_instr_t *)access)->annots,
        CORE_STRING_STATIC("ir.hoisted_check"));
    if (hoisted && hoisted->kind == IR_ANNOT_BOOL && hoisted->b) return true;
    const ir_value_t *base = ir_value_operand(access, 0);
    const ir_value_t *index = ir_value_operand(access, 1);
    if (!base || !index || !integer_value(a->f->owner, index)) return false;
    uint64_t width = 0;
    bool natural = unsigned_width(a->f->owner, index, &width)
        || ir_abcd_at_least(a, index, nullptr, 0, access);
    if (!natural) return false;
    query_t q;
    query_init(&q, a, true);
    q.length_target = true;
    q.access = access;
    for (size_t i = 0; i < a->lengths.len; i++) {
        const length_t *length = &a->lengths.data[i];
        if (precedes(a, length->length, access)
            && origin_matches_access(a, &length->origin, access)
            && sequence_stable(a, base, addressed(access), length->length,
                               access))
            core_list_push(q.targets, length->length->dest);
    }
    bool proven = prove(&q, value_node(index), access, -1);
    query_free(&q);
    return proven;
}

static void
index_by_number(core_dict_t *index, uint32_t number, const ir_instr_t *in)
{
    if (!number) return;
    core_list_t(instr_ptr_t) *list = core_dict_get(index,
        (void *)(uintptr_t)number, nullptr);
    if (!list) {
        list = core_alloc(core_list_t(instr_ptr_t));
        *list = core_list_new(instr_ptr_t);
        core_dict_put(index, (void *)(uintptr_t)number, list);
    }
    core_list_push(*list, in);
}

static void
free_index(core_dict_t *index)
{
    core_dict_foreach(index, entry) {
        core_list_t(instr_ptr_t) *list = entry->value;
        core_list_free(*list);
        core_free(list);
    }
    core_dict_free(index);
}

ir_abcd_t *
ir_abcd_build(ir_func_t *f)
{
    if (!f || !f->n_blocks) return nullptr;
    ir_abcd_t *a = core_alloc(ir_abcd_t);
    a->f = f;
    a->doms = ir_func_doms(f);
    a->numbers = ir_func_value_numbers(f);
    a->ranges = ir_func_ranges(f);
    a->memssa = ir_func_memssa(f);
    a->places = ir_func_places(f);
    a->lengths = core_list_new(length_t);
    a->effects = core_list_new(instr_ptr_t);
    a->callees = core_list_new(callee_bound_t);
    core_dict_init(&a->offsets_of, nullptr, nullptr);
    core_dict_init(&a->checks_of, nullptr, nullptr);
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_block_t *block = f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            sequence_origin_t origin = {0};
            if (length_origin(in, &origin))
                core_list_push(a->lengths, ((length_t){in, origin}));
            if (may_clobber(in)) core_list_push(a->effects, in);
            if (access_check(in))
                index_by_number(&a->checks_of, ir_value_number(a->numbers,
                    ir_value_uncopied(ir_value_operand(in, 1))), in);
            ir_int_t constant = 0;
            const ir_value_t *offset_of = nullptr;
            if (in->op == IR_OP_SUB && constant_operand(a, in, 1, &constant))
                offset_of = ir_value_operand(in, 0);
            else if (in->op == IR_OP_ADD) {
                if (constant_operand(a, in, 1, &constant))
                    offset_of = ir_value_operand(in, 0);
                else if (constant_operand(a, in, 0, &constant))
                    offset_of = ir_value_operand(in, 1);
            }
            if (offset_of && in->dest)
                index_by_number(&a->offsets_of, ir_value_number(a->numbers,
                    ir_value_uncopied(offset_of)), in);
        }
    }
    return a;
}

static bool
typed_query(ir_abcd_t *a, bool upper, const ir_value_t *value,
            ir_int_t offset, th_type *limit_type, const ir_instr_t *at)
{
    ir_machine_t machine = ir_machine_of(a->f->owner, limit_type);
    if ((machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width > 128) return false;
    ir_interval_t limit = ir_interval_of_width(machine.width,
                                               machine.class == IR_CLASS_SINT);
    query_t q;
    query_init(&q, a, upper);
    q.type_target = true;
    q.type_limit = limit;
    q.origin = ir_value_number(a->numbers, ir_value_uncopied(value));
    bool proven = prove(&q, value_node(value), at, offset);
    query_free(&q);
    return proven;
}

bool
ir_abcd_overflow_safe(ir_abcd_t *a, const ir_instr_t *in)
{
    if (!a || !in || !in->dest) return false;
    const ir_value_t *source = ir_value_operand(in, 0);
    if (in->op == IR_OP_CAST && in->n_operands == 1 && source
        && integer_value(a->f->owner, source))
        return typed_query(a, true, source, 0, in->dest->type, in)
            && typed_query(a, false, source, 0, in->dest->type, in);
    if (in->op == IR_OP_NEG && in->n_operands == 1 && source
        && ir_machine_of(a->f->owner, in->dest->type).class
            == IR_CLASS_SINT)
        return typed_query(a, false, source, 1, in->dest->type, in);
    if (in->n_operands != 2) return false;
    ir_int_t k = 0;
    if (in->op == IR_OP_ADD) {
        if (constant_operand(a, in, 1, &k) && k > 0)
            return typed_query(a, true, ir_value_operand(in, 0), -k,
                               in->dest->type, in);
        if (constant_operand(a, in, 0, &k) && k > 0)
            return typed_query(a, true, ir_value_operand(in, 1), -k,
                               in->dest->type, in);
        return false;
    }
    if (in->op == IR_OP_SUB && constant_operand(a, in, 1, &k) && k > 0)
        return typed_query(a, false, ir_value_operand(in, 0), k,
                           in->dest->type, in);
    return false;
}

int
ir_abcd_compare(ir_abcd_t *a, const ir_instr_t *in)
{
    if (!a || !in || !ir_op_is_comparison(in->op) || in->n_operands != 2) return -1;
    const ir_value_t *left = ir_value_operand(in, 0);
    const ir_value_t *right = ir_value_operand(in, 1);
    ir_module_t *module = a->f->owner;
    if (!left || !right || !integer_value(module, left)
        || !integer_value(module, right)
        || ir_machine_of(module, left->type).class
            != ir_machine_of(module, right->type).class) return -1;
    bool lt = in->op == IR_OP_LT || in->op == IR_OP_GE;
    bool gt = in->op == IR_OP_GT || in->op == IR_OP_LE;
    if (lt || gt) {
        bool holds = lt ? ir_abcd_at_most(a, left, right, -1, in)
                        : ir_abcd_at_least(a, left, right, 1, in);
        bool fails = !holds && (lt ? ir_abcd_at_least(a, left, right, 0, in)
                                   : ir_abcd_at_most(a, left, right, 0, in));
        bool positive = in->op == IR_OP_LT || in->op == IR_OP_GT;
        return holds ? positive : fails ? !positive : -1;
    }
    if (in->op != IR_OP_EQ && in->op != IR_OP_NE) return -1;
    bool apart = ir_abcd_at_most(a, left, right, -1, in)
        || ir_abcd_at_least(a, left, right, 1, in);
    bool same = !apart && ir_abcd_at_most(a, left, right, 0, in)
        && ir_abcd_at_least(a, left, right, 0, in);
    bool equal = in->op == IR_OP_EQ;
    return apart ? !equal : same ? equal : -1;
}

typedef struct {
    ir_instr_t *comparison;
    int         outcome;
} decision_t;
core_list_decl(decision_t);

uint32_t
ir_abcd_fold(ir_func_t *f)
{
    ir_abcd_t *a = ir_func_abcd(f);
    if (!a) return 0;
    core_list_t(decision_t) decided = core_list_new(decision_t);
    for (uint32_t b = 0; b < f->n_blocks; b++)
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];
            if (!ir_op_is_comparison(in->op) || !in->dest) continue;
            int outcome = ir_abcd_compare(a, in);
            if (outcome >= 0)
                core_list_push(decided, ((decision_t){in, outcome}));
        }
    core_dict_t outcomes;
    core_dict_init(&outcomes, nullptr, nullptr);
    for (size_t d = 0; d < decided.len; d++) {
        ir_instr_t *in = decided.data[d].comparison;
        in->op = IR_OP_CONST;
        in->n_operands = 0;
        in->n_mods = 0;
        (void)ir_instr_operand_uint(in, (ir_uint_t)decided.data[d].outcome);
        core_dict_put(&outcomes, in->dest,
                      (void *)(uintptr_t)(decided.data[d].outcome + 1));
    }
    for (uint32_t u = 0; decided.len && u < f->n_blocks; u++) {
        ir_instr_t *term = ir_block_terminator(f->blocks[u]);
        bool found = false;
        uintptr_t outcome = term && term->op == IR_OP_BRANCH
            && term->n_operands == 3
            ? (uintptr_t)core_dict_get(&outcomes,
                ir_operand_value(&term->operands[0]), &found) : 0;
        if (found) (void)ir_branch_fold(term, outcome - 1 ? 0 : 1);
    }
    core_dict_free(&outcomes);
    uint32_t folded = (uint32_t)decided.len;
    core_list_free(decided);
    return folded;
}

typedef struct {
    ir_instr_t       *access;
    const ir_value_t *position;
    ir_int_t          offset;
} hoisted_access_t;
core_list_decl(hoisted_access_t);

typedef struct {
    ir_block_t                    *header;
    ir_block_t                    *preheader;
    bool                          *inside;
    const ir_instr_t              *test;
    ir_op_t                        op;
    const ir_value_t              *counter;
    const ir_value_t              *start;
    const ir_value_t              *bound;
    bool                           rising;
    core_list_t(hoisted_access_t)  accesses;
} monotone_loop_t;
core_list_decl(monotone_loop_t);

static void
natural_loop(ir_abcd_t *a, ir_block_t *header, ir_block_t *latch,
             bool *inside)
{
    uint32_t count = a->f->n_blocks;
    uint32_t *stack = core_alloc_array(uint32_t, count ? count : 1);
    uint32_t depth = 0;
    inside[header->index] = true;
    if (!inside[latch->index]) {
        inside[latch->index] = true;
        stack[depth++] = latch->index;
    }
    while (depth) {
        ir_block_t *block = a->f->blocks[stack[--depth]];
        for (uint32_t p = 0; p < ir_pred_count(a->doms, block); p++) {
            ir_block_t *pred = ir_pred_at(a->doms, block, p);
            if (!pred || pred->index >= count || inside[pred->index])
                continue;
            inside[pred->index] = true;
            stack[depth++] = pred->index;
        }
    }
    core_free(stack);
}

static bool
trap_block(ir_block_t *block)
{
    ir_instr_t *end = ir_block_terminator(block);
    return block->n_instrs == 1 && end
        && (end->op == IR_OP_TRAP || end->op == IR_OP_UNREACHABLE);
}

static bool
single_exit(ir_abcd_t *a, ir_block_t *header, ir_block_t *latch,
            const bool *inside)
{
    for (uint32_t b = 0; b < a->f->n_blocks; b++) {
        if (!inside[b]) continue;
        ir_block_t *block = a->f->blocks[b];
        if (block != header && !ir_dominates(a->doms, header, block))
            return false;
        ir_instr_t *end = ir_block_terminator(block);
        uint32_t successors = end ? ir_succ_count(end) : 0;
        if (!successors) return false;
        for (uint32_t s = 0; s < successors; s++) {
            ir_block_t *to = ir_succ_at(end, s);
            if (!to || to->index >= a->f->n_blocks) return false;
            if (!inside[to->index]) {
                if (block != header && !trap_block(to)) return false;
                continue;
            }
            if (to == header ? block != latch
                             : ir_dominates(a->doms, to, block))
                return false;
        }
    }
    return true;
}

static bool
invariant(const ir_value_t *value, const bool *inside)
{
    value = ir_value_uncopied(value);
    if (!value) return false;
    if (value->kind == IR_VAL_PARAM) return true;
    return value->kind == IR_VAL_INSTR && value->def
        && !inside[value->def->block->index];
}

static bool
unit_step(ir_abcd_t *a, const ir_instr_t *advance, const ir_value_t *counter,
          bool *rising)
{
    ir_int_t k = 0;
    if (!advance || !advance->dest || !exact_arithmetic(a, advance))
        return false;
    if (advance->op == IR_OP_ADD
        && ((same_value(a, ir_value_operand(advance, 0), counter)
             && constant_operand(a, advance, 1, &k))
            || (same_value(a, ir_value_operand(advance, 1), counter)
                && constant_operand(a, advance, 0, &k)))
        && k == 1) {
        *rising = true;
        return true;
    }
    *rising = false;
    return advance->op == IR_OP_SUB
        && same_value(a, ir_value_operand(advance, 0), counter)
        && constant_operand(a, advance, 1, &k) && k == 1;
}

static bool
loop_counter(ir_abcd_t *a, ir_block_t *latch, monotone_loop_t *loop)
{
    ir_instr_t *end = ir_block_terminator(loop->header);
    if (!end || end->op != IR_OP_BRANCH || end->n_operands != 3) return false;
    ir_block_t *yes = ir_succ_of(&end->operands[1]);
    ir_block_t *no  = ir_succ_of(&end->operands[2]);
    if (!yes || !no || loop->inside[yes->index] == loop->inside[no->index])
        return false;
    bool sense = loop->inside[yes->index];
    const ir_instr_t *test = ir_condition_test(ir_value_operand(end, 0), &sense);
    if (!test || !ir_op_is_comparison(test->op) || test->n_operands != 2
        || !test->dest) return false;
    ir_op_t op = sense ? test->op : ir_comparison_negated(test->op);
    for (uint32_t i = 0; i < loop->header->n_instrs; i++) {
        const ir_instr_t *phi = loop->header->instrs[i];
        if (phi->op != IR_OP_PHI || !phi->dest || phi->n_operands != 2)
            continue;
        const ir_value_t *bound = nullptr;
        ir_op_t oriented = op;
        if (same_value(a, ir_value_operand(test, 0), phi->dest))
            bound = ir_value_operand(test, 1);
        else if (same_value(a, ir_value_operand(test, 1), phi->dest)) {
            bound = ir_value_operand(test, 0);
            oriented = ir_comparison_swapped(op);
        }
        if (!bound || !invariant(bound, loop->inside)) continue;
        const ir_value_t *start = nullptr;
        const ir_value_t *next = nullptr;
        for (uint32_t o = 0; o < 2; o++) {
            const ir_operand_t *arm = &phi->operands[o];
            if (arm->kind != IR_OPD_PHI_ARM) break;
            if (arm->phi.block == latch) next = arm->phi.value;
            else if (arm->phi.block == loop->preheader)
                start = arm->phi.value;
        }
        const ir_value_t *advanced = ir_value_uncopied(next);
        bool rising = false;
        if (!start || !advanced
            || !unit_step(a, advanced->def, phi->dest, &rising))
            continue;
        bool upward = oriented == IR_OP_LT || oriented == IR_OP_LE;
        bool downward = oriented == IR_OP_GT || oriented == IR_OP_GE;
        if (rising ? !upward : !downward) continue;
        loop->test    = test;
        loop->op      = oriented;
        loop->counter = phi->dest;
        loop->start   = start;
        loop->bound   = ir_value_uncopied(bound);
        loop->rising  = rising;
        return true;
    }
    return false;
}

static bool
counter_offset(ir_abcd_t *a, const ir_value_t *index,
               const ir_value_t *counter, const ir_value_t **position,
               ir_int_t *offset)
{
    if (same_value(a, index, counter)) {
        *position = index;
        *offset   = 0;
        return true;
    }
    const ir_value_t *value = ir_value_uncopied(index);
    const ir_instr_t *in = value ? value->def : nullptr;
    ir_int_t k = 0;
    if (!in || !exact_arithmetic(a, in)) return false;
    for (uint32_t side = 0; in->op == IR_OP_ADD && side < 2; side++)
        if (same_value(a, ir_value_operand(in, side), counter)
            && constant_operand(a, in, 1 - side, &k)) {
            *position = ir_value_operand(in, side);
            *offset   = k;
            return true;
        }
    if (in->op == IR_OP_SUB && same_value(a, ir_value_operand(in, 0), counter)
        && constant_operand(a, in, 1, &k) && k != ir_int_min) {
        *position = ir_value_operand(in, 0);
        *offset   = -k;
        return true;
    }
    return false;
}

static bool
materializable(const ir_value_t *value, const bool *inside, unsigned depth)
{
    if (!value || !depth) return false;
    if (value->kind == IR_VAL_PARAM) return true;
    if (value->kind != IR_VAL_INSTR || !value->def) return false;
    const ir_instr_t *in = value->def;
    if (!inside[in->block->index]) return true;
    const ir_value_t *source = ir_value_operand(in, 0);
    if (in->op == IR_OP_COPY)
        return materializable(source, inside, depth - 1);
    if (in->op == IR_OP_ADDRESS_OF && in->n_operands == 1)
        return source && (source->kind == IR_VAL_LOCAL
                          || source->kind == IR_VAL_GLOBAL
                          || materializable(source, inside, depth - 1));
    return in->op == IR_OP_FIELD && in->n_operands == 2
        && in->operands[1].kind == IR_OPD_NAME
        && materializable(source, inside, depth - 1);
}

static bool
loop_stable(ir_abcd_t *a, const bool *inside, const ir_value_t *base,
            bool through_address)
{
    for (uint32_t b = 0; b < a->f->n_blocks; b++) {
        if (!inside[b]) continue;
        ir_block_t *block = a->f->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++)
            if (clobbers(a, block->instrs[i], base, through_address))
                return false;
    }
    return true;
}

static void
loop_accesses(ir_abcd_t *a, ir_block_t *latch, monotone_loop_t *loop)
{
    for (uint32_t b = 0; b < a->f->n_blocks; b++) {
        ir_block_t *block = a->f->blocks[b];
        if (!loop->inside[b] || !ir_dominates(a->doms, block, latch))
            continue;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *access = block->instrs[i];
            if ((access->op != IR_OP_INDEX
                 && access->op != IR_OP_SEQUENCE_GET)
                || !access->dest || access->n_operands != 2) continue;
            const ir_value_t *base  = ir_value_operand(access, 0);
            const ir_value_t *index = ir_value_operand(access, 1);
            const ir_value_t *position = nullptr;
            ir_int_t offset = 0;
            if (!base || !index || !integer_value(a->f->owner, index)
                || !counter_offset(a, index, loop->counter, &position,
                                   &offset)
                || !materializable(base, loop->inside, PATH_DEPTH)
                || !loop_stable(a, loop->inside, base,
                                access->op == IR_OP_INDEX)
                || ir_abcd_check_redundant(a, access)) continue;
            core_list_push(loop->accesses,
                ((hoisted_access_t){access, position, offset}));
        }
    }
}

static ir_value_t *
fresh_dest(ir_instr_t *in, th_type *type)
{
    return ir_instr_set_fresh_dest(in, "__abcd", type) ? in->dest : nullptr;
}

static ir_block_t *
fresh_block(ir_func_t *f, const char *role)
{
    for (uint32_t attempt = 0;; attempt++) {
        core_buffer_t name = {0};
        core_buffer_printf(&name, "__abcd_%s_%u", role, attempt);
        core_string_t text = core_buffer_string(&name);
        bool taken = ir_block_find(f, text) != nullptr;
        ir_block_t *block = taken ? nullptr : ir_block_add(f, text);
        core_buffer_release(&name);
        if (!taken) return block;
    }
}

static ir_value_t *
emit_binary(ir_block_t *into, ir_op_t op, const ir_instr_t *near,
            ir_value_t *left, ir_value_t *right, th_type *type, bool trap)
{
    ir_instr_t *in = ir_instr_derived(into, op, near, into->n_instrs);
    if (!in || !ir_instr_operand_value(in, left)
        || !ir_instr_operand_value(in, right)
        || (trap && !ir_instr_add_mod(in, CORE_STRING_STATIC("trap"))))
        return nullptr;
    return fresh_dest(in, type);
}

static ir_value_t *
emit_constant(ir_block_t *into, const ir_instr_t *near, ir_int_t value,
              th_type *type)
{
    ir_instr_t *in = ir_instr_derived(into, IR_OP_CONST, near,
                                      into->n_instrs);
    return in && ir_instr_operand_int(in, value)
        ? fresh_dest(in, type) : nullptr;
}

static ir_value_t *
emit_conversion(ir_block_t *into, const ir_instr_t *near, ir_value_t *value,
                th_type *type)
{
    if (!value || value->type == type) return value;
    ir_instr_t *in = ir_instr_derived(into, IR_OP_CAST, near, into->n_instrs);
    return in && ir_instr_operand_value(in, value)
        && ir_instr_add_mod(in, CORE_STRING_STATIC("trap"))
        ? fresh_dest(in, type) : nullptr;
}

static ir_value_t *
emit_offset(ir_block_t *into, const ir_instr_t *near, ir_value_t *value,
            ir_int_t offset)
{
    if (!value || !offset) return value;
    ir_value_t *amount = emit_constant(into, near,
                                       offset < 0 ? -offset : offset,
                                       value->type);
    return amount ? emit_binary(into, offset < 0 ? IR_OP_SUB : IR_OP_ADD,
                                near, value, amount, value->type, true)
                  : nullptr;
}

static ir_value_t *
materialize(ir_block_t *into, const ir_value_t *value, const bool *inside)
{
    if (value->kind != IR_VAL_INSTR || !inside[value->def->block->index])
        return (ir_value_t *)value;
    const ir_instr_t *in = value->def;
    const ir_value_t *source = ir_value_operand(in, 0);
    if (in->op == IR_OP_COPY) return materialize(into, source, inside);
    ir_value_t *operand = materialize(into, source, inside);
    ir_instr_t *copy = ir_instr_derived(into, in->op, in, into->n_instrs);
    if (!operand || !copy || !ir_instr_operand_value(copy, operand)
        || (in->op == IR_OP_FIELD
            && !ir_instr_operand_name(copy, in->operands[1].name)))
        return nullptr;
    return fresh_dest(copy, value->type);
}

static ir_value_t *
probe(ir_block_t *into, const hoisted_access_t *hoisted, ir_value_t *base,
      ir_value_t *endpoint)
{
    const ir_instr_t *access = hoisted->access;
    ir_value_t *at = emit_conversion(into, access, endpoint,
                                     hoisted->position->type);
    at = emit_offset(into, access, at, hoisted->offset);
    at = emit_conversion(into, access, at,
                         ir_value_operand(access, 1)->type);
    ir_instr_t *check = at ? ir_instr_derived(into, access->op, access,
                                              into->n_instrs)
                           : nullptr;
    return check && ir_instr_operand_value(check, base)
        && ir_instr_operand_value(check, at)
        ? fresh_dest(check, access->dest->type) : nullptr;
}

static bool
hoist_loop(ir_func_t *f, monotone_loop_t *loop)
{
    ir_instr_t *entry = ir_block_terminator(loop->preheader);
    ir_block_t *checks = fresh_block(f, "loop_checks");
    ir_block_t *join = fresh_block(f, "loop_entry");
    if (!entry || entry->op != IR_OP_JUMP || !checks || !join) return false;
    ir_value_t *start = (ir_value_t *)loop->start;
    ir_value_t *bound = (ir_value_t *)loop->bound;
    const ir_instr_t *near = loop->test;
    ir_value_t *one = emit_constant(checks, near, 1, bound->type);
    ir_value_t *last = bound;
    if (loop->op == IR_OP_LT || loop->op == IR_OP_GT)
        last = emit_binary(checks, loop->rising ? IR_OP_SUB : IR_OP_ADD,
                           near, bound, one, bound->type, true);
    ir_value_t *low  = loop->rising ? start : last;
    ir_value_t *high = loop->rising ? last : start;
    if (!one || !last) return false;
    for (size_t i = 0; i < loop->accesses.len; i++) {
        hoisted_access_t *hoisted = &loop->accesses.data[i];
        ir_value_t *base = materialize(checks,
            ir_value_operand(hoisted->access, 0), loop->inside);
        ir_machine_t machine = ir_machine_of(f->owner,
            ir_value_operand(hoisted->access, 1)->type);
        bool below = hoisted->offset < 0 || machine.class == IR_CLASS_SINT;
        if (!base || !probe(checks, hoisted, base, high)
            || (below && !probe(checks, hoisted, base, low)))
            return false;
    }
    ir_instr_t *to_join = ir_instr_derived(checks, IR_OP_JUMP, entry,
                                           checks->n_instrs);
    ir_instr_t *to_header = ir_instr_derived(join, IR_OP_JUMP, entry, 0);
    ir_instr_t *enter = ir_instr_derived(loop->preheader, loop->op, near,
                                         entry->index);
    if (!to_join || !to_header || !enter
        || !ir_instr_operand_block(to_join, join)
        || !ir_instr_operand_block(to_header, loop->header)
        || !ir_instr_operand_value(enter, start)
        || !ir_instr_operand_value(enter, bound)
        || !fresh_dest(enter, near->dest->type)) return false;
    entry->op = IR_OP_BRANCH;
    entry->n_operands = 0;
    if (!ir_instr_operand_value(entry, enter->dest)
        || !ir_instr_operand_block(entry, checks)
        || !ir_instr_operand_block(entry, join)) return false;
    for (uint32_t i = 0; i < loop->header->n_instrs; i++) {
        ir_instr_t *phi = loop->header->instrs[i];
        for (uint32_t o = 0; phi->op == IR_OP_PHI && o < phi->n_operands;
             o++)
            if (phi->operands[o].kind == IR_OPD_PHI_ARM
                && phi->operands[o].phi.block == loop->preheader)
                phi->operands[o].phi.block = join;
    }
    for (size_t i = 0; i < loop->accesses.len; i++)
        if (!ir_annot_set(f->owner->ctx, &loop->accesses.data[i].access->annots,
                          (ir_annot_t){
                              .key  = CORE_STRING_STATIC("ir.hoisted_check"),
                              .kind = IR_ANNOT_BOOL,
                              .b    = true,
                          })) return false;
    return true;
}

uint32_t
ir_abcd_hoist(ir_func_t *f)
{
    ir_abcd_t *a = ir_func_abcd(f);
    if (!a || !a->doms || !(ir_module_props(f->owner) & IR_PROP_SSA))
        return 0;
    uint32_t count = f->n_blocks;
    core_list_t(monotone_loop_t) loops = core_list_new(monotone_loop_t);
    for (uint32_t b = 0; b < count; b++) {
        ir_block_t *latch = f->blocks[b];
        ir_instr_t *end = ir_block_terminator(latch);
        if (!end || end->op != IR_OP_JUMP) continue;
        ir_block_t *header = ir_succ_at(end, 0);
        if (!header || !ir_dominates(a->doms, header, latch)
            || ir_pred_count(a->doms, header) != 2) continue;
        ir_block_t *preheader = ir_pred_at(a->doms, header, 0);
        if (preheader == latch) preheader = ir_pred_at(a->doms, header, 1);
        ir_instr_t *entry = preheader ? ir_block_terminator(preheader)
                                      : nullptr;
        if (!entry || entry->op != IR_OP_JUMP || preheader == latch)
            continue;
        monotone_loop_t loop = {
            .header    = header,
            .preheader = preheader,
            .inside    = core_alloc_array(bool, count ? count : 1),
            .accesses  = core_list_new(hoisted_access_t),
        };
        natural_loop(a, header, latch, loop.inside);
        if (!loop.inside[preheader->index]
            && single_exit(a, header, latch, loop.inside)
            && loop_counter(a, latch, &loop))
            loop_accesses(a, latch, &loop);
        if (loop.accesses.len) {
            core_list_push(loops, loop);
            continue;
        }
        core_list_free(loop.accesses);
        core_free(loop.inside);
    }
    uint32_t hoisted = 0;
    for (size_t i = 0; i < loops.len; i++) {
        if (hoist_loop(f, &loops.data[i]))
            hoisted += (uint32_t)loops.data[i].accesses.len;
        core_list_free(loops.data[i].accesses);
        core_free(loops.data[i].inside);
    }
    core_list_free(loops);
    return hoisted;
}

void
ir_abcd_free(ir_abcd_t *a)
{
    if (!a) return;
    for (uint32_t b = 0; a->reach && b < a->reach_count; b++)
        core_free(a->reach[b]);
    core_free(a->reach);
    free_index(&a->offsets_of);
    free_index(&a->checks_of);
    core_list_free(a->lengths);
    core_list_free(a->effects);
    core_list_free(a->callees);
    core_free(a);
}
