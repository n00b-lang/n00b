#include "ir/range.h"

#include "ir/doms.h"

#include <stdlib.h>
#include <string.h>

struct ir_index_proofs {
    const ir_instr_t **indexes;
    size_t count;
    size_t capacity;
};

typedef struct {
    const ir_value_t **values;
    size_t count;
    size_t capacity;
} bound_set_t;

typedef struct {
    const ir_value_t *sequence;
    const ir_value_t *parameter;
    const ir_value_t *actual;
} sequence_origin_t;

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

static bool
same_address(const ir_value_t *left, const ir_value_t *right,
             unsigned depth)
{
    left = uncopied(left);
    right = uncopied(right);
    if (left == right) return left != nullptr;
    if (!left || !right || depth == 0 || !left->def || !right->def)
        return false;
    if (left->def->op == IR_OP_ADDRESS_OF
        && right->def->op == IR_OP_ADDRESS_OF) {
        const ir_value_t *left_root = value_operand(left->def, 0);
        const ir_value_t *right_root = value_operand(right->def, 0);
        return left_root && right_root
            && same_value(left_root, right_root);
    }
    if (left->def->op != IR_OP_FIELD
        || right->def->op != IR_OP_FIELD
        || left->def->n_operands != 2
        || right->def->n_operands != 2
        || left->def->operands[1].kind != IR_OPD_NAME
        || right->def->operands[1].kind != IR_OPD_NAME
        || strcmp(left->def->operands[1].name,
                  right->def->operands[1].name)) return false;
    return same_address(value_operand(left->def, 0),
                        value_operand(right->def, 0), depth - 1);
}

static bool
same_sequence(const ir_value_t *sequence, const ir_value_t *base,
              bool base_is_address)
{
    sequence = uncopied(sequence);
    base = uncopied(base);
    if (!sequence || !base) return false;
    if (!base_is_address) return same_value(sequence, base);
    return sequence->def && sequence->def->op == IR_OP_LOAD
        && same_address(value_operand(sequence->def, 0), base, 16);
}

static bool
actual_matches_address(const ir_value_t *actual,
                       const ir_value_t *address)
{
    actual = uncopied(actual);
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
extracted_path_matches(const ir_value_t *value,
                       const ir_value_t *parameter,
                       const ir_value_t *actual,
                       const ir_value_t *address, unsigned depth)
{
    if (!depth) return false;
    value = uncopied(value);
    address = uncopied(address);
    if (value == parameter)
        return actual_matches_address(actual, address);
    const ir_instr_t *extract = value ? value->def : nullptr;
    const ir_instr_t *field = address ? address->def : nullptr;
    if (!extract || !field || extract->op != IR_OP_EXTRACT
        || field->op != IR_OP_FIELD || extract->n_operands != 2
        || field->n_operands != 2
        || extract->operands[1].kind != IR_OPD_NAME
        || field->operands[1].kind != IR_OPD_NAME
        || strcmp(extract->operands[1].name,
                  field->operands[1].name)) return false;
    return extracted_path_matches(value_operand(extract, 0), parameter,
                                  actual, value_operand(field, 0),
                                  depth - 1);
}

static bool
origin_matches_index(const sequence_origin_t *origin,
                     const ir_instr_t *index)
{
    const ir_value_t *base = value_operand(index, 0);
    if (origin->parameter)
        return index->op == IR_OP_INDEX
            && extracted_path_matches(origin->sequence,
                origin->parameter, origin->actual, base, 16);
    return same_sequence(origin->sequence, base,
                         index->op == IR_OP_INDEX);
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
        if (reaches_block(ir_succ_at(end, i), target, seen,
                          block_count)) return true;
    return false;
}

static bool
effect_can_precede_index(ir_func_t *function, ir_instr_t *effect,
                         ir_instr_t *index)
{
    ir_block_t *from = effect->block;
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
    else can_reach = reaches_block(from, target, seen,
                                   function->n_blocks);
    free(seen);
    return can_reach;
}

static bool
element_or_sibling_write(const ir_instr_t *effect,
                         const ir_value_t *index_base)
{
    if (effect->op != IR_OP_STORE
        && effect->op != IR_OP_SEQUENCE_SET) return false;
    const ir_value_t *target = uncopied(value_operand(effect, 0));
    const ir_instr_t *target_def = target ? target->def : nullptr;
    index_base = uncopied(index_base);
    const ir_instr_t *source_field = index_base
        ? index_base->def : nullptr;
    if (!source_field || source_field->op != IR_OP_FIELD) return false;
    if (effect->op == IR_OP_SEQUENCE_SET)
        return same_address(value_operand(effect, 0), index_base, 16);
    if (target_def && target_def->op == IR_OP_INDEX)
        return same_address(value_operand(target_def, 0), index_base, 16);
    if (!target_def || target_def->op != IR_OP_FIELD
        || source_field->n_operands != 2
        || target_def->n_operands != 2
        || source_field->operands[1].kind != IR_OPD_NAME
        || target_def->operands[1].kind != IR_OPD_NAME
        || !strcmp(source_field->operands[1].name,
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
pure_function(const ir_func_t *function, unsigned depth)
{
    if (!function || !function->n_blocks || depth == 0) return false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *in = block->instrs[i];
            if (in->op == IR_OP_CALL) {
                const ir_value_t *callee = value_operand(in, 0);
                if (!callee || callee->kind != IR_VAL_FUNC
                    || !pure_function(callee->owner, depth - 1)) return false;
            }
            else if (in->op != IR_OP_TRAP && !ir_op_is_terminator(in->op)
                     && (ir_op_info(in->op)->flags & IR_OPF_SIDEEFFECT))
                return false;
        }
    }
    return true;
}

static const ir_value_t *
extraction_root(const ir_value_t *value)
{
    for (unsigned depth = 0; value && depth < 16; depth++) {
        value = uncopied(value);
        const ir_instr_t *definition = value ? value->def : nullptr;
        if (!definition || definition->op != IR_OP_EXTRACT)
            return value;
        value = value_operand(definition, 0);
    }
    return nullptr;
}

static bool
length_origin(const ir_instr_t *length, sequence_origin_t *origin)
{
    if (!length || !length->dest || !origin) return false;
    uint64_t width = 0;
    if (!unsigned_width(length->block->owner->owner,
                        length->dest, &width)) return false;
    if (length->op == IR_OP_SEQUENCE_LENGTH) {
        *origin = (sequence_origin_t){.sequence = value_operand(length, 0)};
        return origin->sequence != nullptr;
    }
    if (length->op != IR_OP_CALL) return false;
    const ir_value_t *callee = value_operand(length, 0);
    ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
        ? callee->owner : nullptr;
    if (!target || !pure_function(target, 8)) return false;
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
            source = current;
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
            .sequence = source,
            .parameter = root,
            .actual = actual,
        };
        return true;
    }
    return false;
}

static bool
sequence_stable_until(ir_func_t *function, const ir_value_t *index_base,
                      ir_instr_t *length, ir_instr_t *index)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *effect = block->instrs[i];
            if (block == length->block && i < length->index) continue;
            if (effect->op == IR_OP_TRAP
                || ir_op_is_terminator(effect->op)) continue;
            bool clobbers = false;
            if (effect->dest_kind == IR_DEST_LOCAL) clobbers = true;
            else if (effect->op == IR_OP_CALL) {
                const ir_value_t *callee = value_operand(effect, 0);
                clobbers = !callee || callee->kind != IR_VAL_FUNC
                    || !pure_function(callee->owner, 8);
            }
            else if (ir_op_info(effect->op)->flags & IR_OPF_SIDEEFFECT)
                clobbers = !element_or_sibling_write(effect, index_base);
            if (clobbers && effect_can_precede_index(function, effect,
                                                     index)) return false;
        }
    }
    return true;
}

static bool
contains(const bound_set_t *set, const ir_value_t *value)
{
    for (size_t i = 0; i < set->count; i++)
        if (set->values[i] == value) return true;
    return false;
}

static bool
add_bound(bound_set_t *set, const ir_value_t *value)
{
    if (contains(set, value)) return true;
    if (set->count == set->capacity) {
        size_t next = set->capacity ? set->capacity * 2 : 16;
        const ir_value_t **values = realloc(set->values,
                                           next * sizeof(*values));
        if (!values) return false;
        set->values = values;
        set->capacity = next;
    }
    set->values[set->count++] = value;
    return true;
}

static bool
constant_one(const ir_value_t *value)
{
    value = uncopied(value);
    const ir_instr_t *in = value ? value->def : nullptr;
    return in && in->op == IR_OP_CONST && in->n_operands == 1
        && in->operands[0].kind == IR_OPD_CONST
        && !in->operands[0].lit.is_float
        && !in->operands[0].lit.i.neg
        && in->operands[0].lit.i.mag == 1;
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
    return in->op == IR_OP_SUB && ir_instr_has_mod(in, "trap")
        && same_value(value_operand(in, 0), limit)
        && constant_one(value_operand(in, 1));
}

static bool bound_return(const ir_func_t *function,
                         const ir_value_t *limit, unsigned depth);

static bool
call_bounded(const ir_instr_t *call, const ir_value_t *limit,
             unsigned depth)
{
    const ir_value_t *callee = value_operand(call, 0);
    if (!callee || callee->kind != IR_VAL_FUNC || !callee->owner
        || !pure_function(callee->owner, depth)) return false;
    const ir_func_t *function = callee->owner;
    for (uint32_t i = 0; i < function->n_params; i++) {
        if (same_value(value_operand(call, i + 1), limit)
            && bound_return(function, function->params[i], depth - 1))
            return true;
    }
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
            || left_width != result_width
            || right_width != result_width) return false;
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
    for (size_t pass = 0; pass <= value_count; pass++) {
        bool changed = false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            const ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                const ir_instr_t *in = block->instrs[i];
                if (!in->dest || contains(set, in->dest)
                    || !instruction_bounded(function->owner, in, limit,
                                            set, depth)) continue;
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
    if (!depth || !pure_function(function, depth)) return false;
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
    free(set.values);
    return ok && returned;
}

bool
ir_unsigned_sub_proven_safe(const ir_instr_t *sub)
{
    if (!sub || sub->op != IR_OP_SUB || !sub->dest
        || !ir_instr_has_mod(sub, "trap")
        || !(ir_module_props(sub->block->owner->owner) & IR_PROP_SSA))
        return false;
    const ir_value_t *limit = value_operand(sub, 0);
    const ir_value_t *difference = uncopied(value_operand(sub, 1));
    const ir_instr_t *inner = difference ? difference->def : nullptr;
    if (!inner || inner->op != IR_OP_SUB
        || !ir_instr_has_mod(inner, "trap")) return false;
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
    free(set.values);
    return safe;
}

static bool
record_proof(ir_index_proofs_t *proofs, const ir_instr_t *index)
{
    if (proofs->count == proofs->capacity) {
        size_t next = proofs->capacity ? proofs->capacity * 2 : 16;
        const ir_instr_t **indexes = realloc(proofs->indexes,
                                             next * sizeof(*indexes));
        if (!indexes) return false;
        proofs->indexes = indexes;
        proofs->capacity = next;
    }
    proofs->indexes[proofs->count++] = index;
    return true;
}

static bool
checked_index_dominates(ir_doms_t *doms,
                        const ir_instr_t *guard,
                        const ir_instr_t *index)
{
    if (guard == index) return false;
    if (guard->block == index->block)
        return guard->index < index->index;
    return ir_dominates(doms, guard->block, index->block);
}

ir_index_proofs_t *
ir_index_proofs_new(ir_func_t *function)
{
    if (!function) return nullptr;
    ir_index_proofs_t *proofs = calloc(1, sizeof(*proofs));
    if (!proofs) return nullptr;
    if (!(ir_module_props(function->owner) & IR_PROP_SSA)) return proofs;
    ir_doms_t *doms = ir_doms_of(function);
    if (!doms) return proofs;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *length = block->instrs[i];
            sequence_origin_t origin = {0};
            if (!length_origin(length, &origin))
                continue;
            bound_set_t set = {0};
            if (!compute_bounds(function, length->dest, &set, 8)) {
                free(set.values);
                ir_index_proofs_free(proofs);
                return nullptr;
            }
            for (uint32_t ib = 0; ib < function->n_blocks; ib++) {
                ir_block_t *index_block = function->blocks[ib];
                if (!ir_dominates(doms, block, index_block)) continue;
                for (uint32_t ii = 0; ii < index_block->n_instrs; ii++) {
                    ir_instr_t *index = index_block->instrs[ii];
                    if (index->op != IR_OP_INDEX
                        && index->op != IR_OP_SEQUENCE_GET) continue;
                    if (block == index_block && i >= ii) continue;
                    if (!origin_matches_index(&origin, index)
                        || !sequence_stable_until(function,
                            value_operand(index, 0), length, index)) continue;
                    bool bounded = contains(&set, value_operand(index, 1));
                    for (uint32_t gb = 0; !bounded && gb < function->n_blocks;
                         gb++) {
                        ir_block_t *guard_block = function->blocks[gb];
                        if (!ir_dominates(doms, block, guard_block)) continue;
                        for (uint32_t gi = 0; !bounded
                             && gi < guard_block->n_instrs; gi++) {
                            ir_instr_t *guard = guard_block->instrs[gi];
                            if (guard->op != IR_OP_INDEX
                                || (block == guard_block && i >= gi)
                                || !checked_index_dominates(doms, guard,
                                                             index)
                                || !origin_matches_index(&origin, guard)
                                || !sequence_stable_until(function,
                                    value_operand(guard, 0), length, guard))
                                continue;
                            bound_set_t guarded = {0};
                            bounded = add_bound(&guarded,
                                value_operand(guard, 1))
                                && compute_bounds(function, length->dest,
                                                  &guarded, 8)
                                && contains(&guarded,
                                            value_operand(index, 1));
                            free(guarded.values);
                        }
                    }
                    if (bounded && !ir_index_proven(proofs, index)
                        && !record_proof(proofs, index)) {
                        free(set.values);
                        ir_index_proofs_free(proofs);
                        return nullptr;
                    }
                }
            }
            free(set.values);
        }
    }
    return proofs;
}

bool
ir_index_proven(const ir_index_proofs_t *proofs, const ir_instr_t *index)
{
    if (!proofs || !index) return false;
    for (size_t i = 0; i < proofs->count; i++)
        if (proofs->indexes[i] == index) return true;
    return false;
}

void
ir_index_proofs_free(ir_index_proofs_t *proofs)
{
    if (!proofs) return;
    free(proofs->indexes);
    free(proofs);
}
