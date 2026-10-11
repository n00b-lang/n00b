#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/doms.h"
#include "th/typehash.h"

typedef struct {
    const ir_value_t **items;
    size_t count;
    size_t capacity;
} value_set_t;

static ir_annot_t *value_annot(const ir_value_t *value, const char *key)
{
    return ir_annot_get(ir_value_annots((ir_value_t *)value), key);
}

static bool annot_bool(const ir_value_t *value, const char *key, bool *out)
{
    ir_annot_t *annot = value_annot(value, key);
    if (!annot || annot->kind != IR_ANNOT_BOOL) return false;
    *out = annot->b;
    return true;
}

static bool annot_uint(const ir_value_t *value, const char *key, uint64_t *out)
{
    ir_annot_t *annot = value_annot(value, key);
    if (!annot || annot->kind != IR_ANNOT_INT || annot->i.neg
        || annot->i.mag > UINT64_MAX) return false;
    *out = (uint64_t)annot->i.mag;
    return true;
}

static bool value_set_add(value_set_t *set, const ir_value_t *value,
                          bool *already_present)
{
    for (size_t i = 0; i < set->count; i++) {
        if (set->items[i] == value) {
            *already_present = true;
            return true;
        }
    }
    *already_present = false;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 8;
        const ir_value_t **grown = realloc(
            set->items, capacity * sizeof(*grown));
        if (!grown) return false;
        set->items = grown;
        set->capacity = capacity;
    }
    set->items[set->count++] = value;
    return true;
}

static bool check_alias_edges(const ir_value_t *value, const char *prefix,
                              uint64_t count, uint64_t point,
                              value_set_t *work)
{
    char key[64];
    for (uint64_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "ir.%s%llu", prefix,
                 (unsigned long long)i);
        ir_annot_t *edge = value_annot(value, key);
        if (!edge || edge->kind != IR_ANNOT_VALUE || !edge->v) return false;

        snprintf(key, sizeof(key), "ir.%s%llu_escapes", prefix,
                 (unsigned long long)i);
        bool escapes = false;
        if (!annot_bool(value, key, &escapes) || escapes) return false;

        snprintf(key, sizeof(key), "ir.%s%llu_has_last_use", prefix,
                 (unsigned long long)i);
        bool has_last_use = false;
        if (!annot_bool(value, key, &has_last_use) || !has_last_use)
            return false;

        snprintf(key, sizeof(key), "ir.%s%llu_last_use", prefix,
                 (unsigned long long)i);
        uint64_t last_use = 0;
        if (!annot_uint(value, key, &last_use) || last_use >= point)
            return false;

        bool already_present = false;
        if (!value_set_add(work, edge->v, &already_present)) return false;
    }
    return true;
}

static bool
instruction_uses(const ir_instr_t *instruction, const ir_value_t *value)
{
    for (uint32_t i = 0; i < instruction->n_operands; i++)
        if (ir_operand_value(&instruction->operands[i]) == value) return true;
    return false;
}

static bool
block_belongs_to(const ir_func_t *function, const ir_block_t *block)
{
    return block && block->owner == function
        && block->index < function->n_blocks
        && function->blocks[block->index] == block;
}

static bool
instruction_belongs_to(const ir_func_t *function,
                       const ir_instr_t *instruction)
{
    return instruction && block_belongs_to(function, instruction->block)
        && instruction->index < instruction->block->n_instrs
        && instruction->block->instrs[instruction->index] == instruction;
}

ir_liveness_t
ir_value_liveness_after(const ir_value_t *value, const ir_instr_t *point)
{
    if (!value || !point || !point->block || !point->block->owner)
        return IR_LIVENESS_UNKNOWN;
    ir_func_t *function = point->block->owner;
    if ((value->kind != IR_VAL_PARAM && value->kind != IR_VAL_LOCAL
         && value->kind != IR_VAL_INSTR)
        || value->owner != function
        || !block_belongs_to(function, point->block)
        || point->index >= point->block->n_instrs
        || point->block->instrs[point->index] != point)
        return IR_LIVENESS_UNKNOWN;
    if (value->kind == IR_VAL_INSTR
        && (!instruction_belongs_to(function, value->def)
            || value->def->dest != value
            || (value->def->block == point->block
                && value->def->index > point->index)))
        return IR_LIVENESS_UNKNOWN;

    uint32_t block_count = function->n_blocks;
    bool *queued = calloc(block_count, sizeof(*queued));
    ir_block_t **work = malloc(block_count * sizeof(*work));
    if (!queued || !work) {
        free(queued);
        free(work);
        return IR_LIVENESS_UNKNOWN;
    }

    size_t work_count = 0;
    ir_block_t *entry = ir_block_entry(function);
    if (!entry || !block_belongs_to(function, entry)) {
        free(queued);
        free(work);
        return IR_LIVENESS_UNKNOWN;
    }
    queued[entry->index] = true;
    work[work_count++] = entry;
    while (work_count) {
        ir_block_t *block = work[--work_count];
        ir_instr_t *terminator = ir_block_terminator(block);
        for (uint32_t i = 0; terminator && i < ir_succ_count(terminator); i++) {
            ir_block_t *successor = ir_succ_at(terminator, i);
            if (!block_belongs_to(function, successor)) continue;
            if (!queued[successor->index]) {
                queued[successor->index] = true;
                work[work_count++] = successor;
            }
        }
    }
    if (!queued[point->block->index]
        || (value->kind == IR_VAL_INSTR
            && !queued[value->def->block->index])) {
        free(queued);
        free(work);
        return IR_LIVENESS_UNKNOWN;
    }

    if (value->kind == IR_VAL_INSTR
        && value->def->block != point->block) {
        memset(queued, 0, block_count * sizeof(*queued));
        work_count = 0;
        if (entry != value->def->block) {
            queued[entry->index] = true;
            work[work_count++] = entry;
        }
        while (work_count) {
            ir_block_t *block = work[--work_count];
            ir_instr_t *terminator = ir_block_terminator(block);
            for (uint32_t i = 0; terminator
                 && i < ir_succ_count(terminator); i++) {
                ir_block_t *successor = ir_succ_at(terminator, i);
                if (!block_belongs_to(function, successor)
                    || successor == value->def->block)
                    continue;
                if (!queued[successor->index]) {
                    queued[successor->index] = true;
                    work[work_count++] = successor;
                }
            }
        }
        if (queued[point->block->index]) {
            free(queued);
            free(work);
            return IR_LIVENESS_UNKNOWN;
        }
    }

    memset(queued, 0, block_count * sizeof(*queued));
    work_count = 0;
    bool complete = true;
    for (uint32_t i = point->index + 1; i < point->block->n_instrs; i++) {
        if (instruction_uses(point->block->instrs[i], value)) {
            free(queued);
            free(work);
            return IR_LIVENESS_LIVE;
        }
    }

    ir_instr_t *terminator = ir_block_terminator(point->block);
    if (!terminator) complete = false;
    for (uint32_t i = 0; terminator && i < ir_succ_count(terminator); i++) {
        ir_block_t *successor = ir_succ_at(terminator, i);
        if (!block_belongs_to(function, successor)) {
            complete = false;
            continue;
        }
        if (!queued[successor->index]) {
            queued[successor->index] = true;
            work[work_count++] = successor;
        }
    }

    while (work_count) {
        ir_block_t *block = work[--work_count];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            if (instruction_uses(block->instrs[i], value)) {
                free(queued);
                free(work);
                return IR_LIVENESS_LIVE;
            }
        }

        terminator = ir_block_terminator(block);
        if (!terminator) {
            complete = false;
            continue;
        }
        for (uint32_t i = 0; i < ir_succ_count(terminator); i++) {
            ir_block_t *successor = ir_succ_at(terminator, i);
            if (!block_belongs_to(function, successor)) {
                complete = false;
                continue;
            }
            if (!queued[successor->index]) {
                queued[successor->index] = true;
                work[work_count++] = successor;
            }
        }
    }

    free(queued);
    free(work);
    return complete ? IR_LIVENESS_DEAD : IR_LIVENESS_UNKNOWN;
}

static bool
block_reachable_without(const ir_func_t *function, const ir_block_t *start,
                        const ir_block_t *forbidden,
                        const ir_block_t *target, bool *complete)
{
    uint32_t count = function->n_blocks;
    bool *visited = calloc(count, sizeof(*visited));
    ir_block_t **work = malloc(count * sizeof(*work));
    if (!visited || !work) {
        free(visited);
        free(work);
        *complete = false;
        return false;
    }
    size_t work_count = 0;
    if (!block_belongs_to(function, start)) {
        *complete = false;
        free(visited);
        free(work);
        return false;
    }
    visited[start->index] = true;
    work[work_count++] = (ir_block_t *)start;
    bool found = start == target;
    while (work_count && !found) {
        ir_block_t *block = work[--work_count];
        ir_instr_t *terminator = ir_block_terminator(block);
        if (!terminator) {
            *complete = false;
            continue;
        }
        for (uint32_t i = 0; i < ir_succ_count(terminator); i++) {
            ir_block_t *successor = ir_succ_at(terminator, i);
            if (!block_belongs_to(function, successor)) {
                *complete = false;
                continue;
            }
            if (successor == forbidden || visited[successor->index])
                continue;
            if (successor == target) {
                found = true;
                break;
            }
            visited[successor->index] = true;
            work[work_count++] = successor;
        }
    }
    free(visited);
    free(work);
    return found;
}

ir_liveness_t
ir_value_liveness_on_edge(const ir_value_t *value, const ir_block_t *from,
                          const ir_block_t *to)
{
    if (!value || !from || !to || !from->owner || from->owner != to->owner)
        return IR_LIVENESS_UNKNOWN;
    ir_func_t *function = from->owner;
    if ((value->kind != IR_VAL_PARAM && value->kind != IR_VAL_LOCAL
         && value->kind != IR_VAL_INSTR)
        || value->owner != function || !block_belongs_to(function, from)
        || !block_belongs_to(function, to))
        return IR_LIVENESS_UNKNOWN;
    ir_instr_t *terminator = ir_block_terminator((ir_block_t *)from);
    bool is_edge = false;
    for (uint32_t i = 0; terminator && i < ir_succ_count(terminator); i++)
        is_edge |= ir_succ_at(terminator, i) == to;
    if (!is_edge) return IR_LIVENESS_UNKNOWN;

    ir_block_t *entry = ir_block_entry(function);
    bool complete = true;
    if (!entry || !block_reachable_without(function, entry, nullptr,
                                            from, &complete)
        || !complete)
        return IR_LIVENESS_UNKNOWN;

    if (value->kind == IR_VAL_INSTR) {
        if (!instruction_belongs_to(function, value->def)
            || value->def->dest != value)
            return IR_LIVENESS_UNKNOWN;
        if (value->def->block == from) {
            if (value->def->index >= terminator->index)
                return IR_LIVENESS_UNKNOWN;
        }
        else if (value->def->block == to) return IR_LIVENESS_UNKNOWN;
        else {
            complete = true;
            if (block_reachable_without(function, entry, value->def->block,
                                        from, &complete))
                return IR_LIVENESS_UNKNOWN;
            if (!complete) return IR_LIVENESS_UNKNOWN;
            complete = true;
            if (!block_reachable_without(function, entry, nullptr,
                                        value->def->block, &complete)
                || !complete)
                return IR_LIVENESS_UNKNOWN;
        }
    }

    uint32_t block_count = function->n_blocks;
    bool *visited = calloc(block_count, sizeof(*visited));
    ir_block_t **work = malloc(block_count * sizeof(*work));
    if (!visited || !work) {
        free(visited);
        free(work);
        return IR_LIVENESS_UNKNOWN;
    }
    size_t work_count = 0;
    visited[to->index] = true;
    work[work_count++] = (ir_block_t *)to;
    bool cfg_complete = true;
    while (work_count) {
        ir_block_t *block = work[--work_count];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            if (instruction_uses(block->instrs[i], value)) {
                free(visited);
                free(work);
                return IR_LIVENESS_LIVE;
            }
        }
        ir_instr_t *last = ir_block_terminator(block);
        if (!last) {
            cfg_complete = false;
            continue;
        }
        for (uint32_t i = 0; i < ir_succ_count(last); i++) {
            ir_block_t *successor = ir_succ_at(last, i);
            if (!block_belongs_to(function, successor)) {
                cfg_complete = false;
                continue;
            }
            if (visited[successor->index]) continue;
            visited[successor->index] = true;
            work[work_count++] = successor;
        }
    }
    free(visited);
    free(work);
    return cfg_complete ? IR_LIVENESS_DEAD : IR_LIVENESS_UNKNOWN;
}

static bool
collect_alias_edges(const ir_value_t *value, const char *prefix,
                    uint64_t count, value_set_t *work)
{
    char key[64];
    for (uint64_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "ir.%s%llu", prefix,
                 (unsigned long long)i);
        ir_annot_t *edge = value_annot(value, key);
        if (!edge || edge->kind != IR_ANNOT_VALUE || !edge->v) return false;

        snprintf(key, sizeof(key), "ir.%s%llu_escapes", prefix,
                 (unsigned long long)i);
        bool escapes = false;
        if (!annot_bool(value, key, &escapes) || escapes) return false;

        bool already_present = false;
        if (!value_set_add(work, edge->v, &already_present)) return false;
    }
    return true;
}

static bool
collect_inverse_alias_edges(const ir_value_t *value, value_set_t *work)
{
    ir_func_t *function = value ? value->owner : nullptr;
    if (!function) return true;

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_value_t *derived = block->instrs[i]->dest;
            if (!derived || derived == value) continue;
            uint64_t count = 0;
            if (!annot_uint(derived, "ir.alias_count", &count)) {
                if (value_annot(derived, "ir.alias_count")) return false;
                continue;
            }
            for (uint64_t edge = 0; edge < count; edge++) {
                char key[64];
                snprintf(key, sizeof(key), "ir.alias%llu",
                         (unsigned long long)edge);
                ir_annot_t *target = value_annot(derived, key);
                if (!target || target->kind != IR_ANNOT_VALUE || !target->v)
                    return false;
                if (target->v == value) {
                    bool already_present = false;
                    if (!value_set_add(work, derived, &already_present))
                        return false;
                    break;
                }
            }
        }
    }
    return true;
}

static bool
forwarding_alias_input(const ir_instr_t *instruction, uint32_t operand)
{
    switch (instruction->op) {
    case IR_OP_ADDRESS_OF:
    case IR_OP_COPY:
    case IR_OP_CAST:
    case IR_OP_BITCAST:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_BOX:
        return operand == 0;
    case IR_OP_FIELD:
    case IR_OP_INDEX:
    case IR_OP_OFFSET:
        return operand == 0;
    case IR_OP_SELECT:
        return operand == 1 || operand == 2;
    case IR_OP_PHI:
        return ir_op_operand_kind(instruction->op, operand)
            == IR_OPD_PHI_ARM;
    default:
        return false;
    }
}

static bool
non_escaping_pointer_use(const ir_instr_t *instruction, uint32_t operand)
{
    switch (instruction->op) {
    case IR_OP_LOAD:
        return operand == 0;
    case IR_OP_STORE:
        return operand == 1;
    case IR_OP_FINALIZE:
    case IR_OP_DEALLOC:
    case IR_OP_DIFF:
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_LT:
    case IR_OP_LE:
    case IR_OP_GT:
    case IR_OP_GE:
        return true;
    default:
        return false;
    }
}

static bool
collect_structural_aliases(const ir_value_t *value, value_set_t *work)
{
    ir_func_t *function = value ? value->owner : nullptr;
    if (!function) return true;

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            ir_value_t *derived = instruction->dest;
            for (uint32_t operand = 0; operand < instruction->n_operands;
                 operand++) {
                if (ir_operand_value(&instruction->operands[operand])
                        == value
                    && value->type
                    && th_kind_of(value->type) == TH_INDIRECTION
                    && !forwarding_alias_input(instruction, operand)
                    && !non_escaping_pointer_use(instruction, operand))
                    return false;
            }
            if (!derived || th_kind_of(derived->type) != TH_INDIRECTION)
                continue;

            uint32_t first = 0;
            uint32_t limit = 0;
            switch (instruction->op) {
            case IR_OP_ADDRESS_OF:
                limit = instruction->n_operands;
                break;
            case IR_OP_COPY:
                limit = instruction->n_operands;
                break;
            case IR_OP_CAST:
            case IR_OP_BITCAST:
            case IR_OP_NOMINAL_CAST:
            case IR_OP_BOX:
                limit = instruction->n_operands;
                break;
            case IR_OP_FIELD:
            case IR_OP_INDEX:
            case IR_OP_OFFSET:
                if (instruction->n_operands < 2) continue;
                limit = 1;
                break;
            case IR_OP_SELECT:
                if (instruction->n_operands < 3) continue;
                first = 1;
                limit = 3;
                break;
            case IR_OP_PHI:
                limit = instruction->n_operands;
                break;
            default:
                continue;
            }

            for (uint32_t operand = first; operand < limit; operand++) {
                ir_value_t *source = ir_operand_value(
                    &instruction->operands[operand]);
                if (!source) continue;
                if (instruction->op != IR_OP_ADDRESS_OF
                    && (!source->type
                        || th_kind_of(source->type) != TH_INDIRECTION))
                    continue;
                if (source == value) {
                    bool already_present = false;
                    if (!value_set_add(work, derived, &already_present))
                        return false;
                }
                if (derived == value) {
                    bool already_present = false;
                    if (!value_set_add(work, source, &already_present))
                        return false;
                }
            }
        }
    }
    return true;
}

ir_liveness_t
ir_value_alias_liveness_after(const ir_value_t *value,
                              const ir_instr_t *point)
{
    value_set_t work = {0};
    bool already_present = false;
    if (!value || !value_set_add(&work, value, &already_present)) {
        free(work.items);
        return IR_LIVENESS_UNKNOWN;
    }

    bool complete = true;
    for (size_t i = 0; complete && i < work.count; i++) {
        const ir_value_t *current = work.items[i];
        bool analysis_complete = false;
        bool escapes = false;
        bool unresolved = false;
        uint64_t aliases = 0;
        uint64_t aliased_by = 0;
        complete = annot_bool(current, "ir.alias_analysis_complete",
                              &analysis_complete)
            && analysis_complete
            && annot_bool(current, "ir.lifetime_escapes", &escapes)
            && !escapes
            && annot_bool(current, "ir.alias_unresolved", &unresolved)
            && !unresolved
            && annot_uint(current, "ir.alias_count", &aliases)
            && annot_uint(current, "ir.aliased_by_count", &aliased_by)
            && collect_alias_edges(current, "alias", aliases, &work)
            && collect_alias_edges(current, "aliased_by", aliased_by, &work)
            && collect_inverse_alias_edges(current, &work)
            && collect_structural_aliases(current, &work);
    }

    if (!complete) {
        free(work.items);
        return IR_LIVENESS_UNKNOWN;
    }

    bool unknown = false;
    for (size_t i = 0; i < work.count; i++) {
        ir_liveness_t state = ir_value_liveness_after(work.items[i], point);
        if (state == IR_LIVENESS_LIVE) {
            free(work.items);
            return IR_LIVENESS_LIVE;
        }
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    free(work.items);
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

ir_liveness_t
ir_value_alias_liveness_on_edge(const ir_value_t *value,
                                const ir_block_t *from,
                                const ir_block_t *to)
{
    value_set_t work = {0};
    bool already_present = false;
    if (!value || !value_set_add(&work, value, &already_present)) {
        free(work.items);
        return IR_LIVENESS_UNKNOWN;
    }

    bool complete = true;
    for (size_t i = 0; complete && i < work.count; i++) {
        const ir_value_t *current = work.items[i];
        bool analysis_complete = false;
        bool escapes = false;
        bool unresolved = false;
        uint64_t aliases = 0;
        uint64_t aliased_by = 0;
        complete = annot_bool(current, "ir.alias_analysis_complete",
                              &analysis_complete)
            && analysis_complete
            && annot_bool(current, "ir.lifetime_escapes", &escapes)
            && !escapes
            && annot_bool(current, "ir.alias_unresolved", &unresolved)
            && !unresolved
            && annot_uint(current, "ir.alias_count", &aliases)
            && annot_uint(current, "ir.aliased_by_count", &aliased_by)
            && collect_alias_edges(current, "alias", aliases, &work)
            && collect_alias_edges(current, "aliased_by", aliased_by, &work)
            && collect_inverse_alias_edges(current, &work)
            && collect_structural_aliases(current, &work);
    }
    if (!complete) {
        free(work.items);
        return IR_LIVENESS_UNKNOWN;
    }

    bool unknown = false;
    for (size_t i = 0; i < work.count; i++) {
        ir_liveness_t state = ir_value_liveness_on_edge(work.items[i], from,
                                                        to);
        if (state == IR_LIVENESS_LIVE) {
            free(work.items);
            return IR_LIVENESS_LIVE;
        }
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    free(work.items);
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

static ir_liveness_t
alias_liveness_at_block_entry(const ir_value_t *value,
                              const ir_func_t *function,
                              const ir_block_t *block)
{
    bool found_predecessor = false;
    bool found_live = false;
    bool found_unknown = false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *predecessor = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(predecessor);
        for (uint32_t s = 0; terminator && s < ir_succ_count(terminator); s++) {
            if (ir_succ_at(terminator, s) != block) continue;
            found_predecessor = true;
            ir_liveness_t state = ir_value_alias_liveness_on_edge(
                value, predecessor, (ir_block_t *)block);
            found_live |= state == IR_LIVENESS_LIVE;
            found_unknown |= state == IR_LIVENESS_UNKNOWN;
        }
    }
    if (found_unknown || !found_predecessor) return IR_LIVENESS_UNKNOWN;
    return found_live ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
}

static bool
local_has_binding_initialization(const ir_value_t *value,
                                 const ir_func_t *function)
{
    if (!value || value->kind != IR_VAL_LOCAL || !value->local || !function)
        return false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (ir_instr_is_binding_initialization(instruction)
                && instruction->op == IR_OP_COPY
                && instruction->dest_kind == IR_DEST_LOCAL
                && instruction->dest_local == value->local) return true;
        }
    }
    return false;
}

static bool
value_starts_at_instruction(const ir_value_t *value,
                            const ir_instr_t *instruction,
                            bool local_starts_at_entry)
{
    if (value->kind == IR_VAL_INSTR) return value->def == instruction;
    if (value->kind != IR_VAL_LOCAL || !value->local) return false;
    if (ir_instr_is_binding_initialization(instruction)
        && instruction->op == IR_OP_COPY
        && instruction->dest_kind == IR_DEST_LOCAL
        && instruction->dest_local == value->local) return true;
    ir_block_t *entry = local_starts_at_entry
        ? ir_block_entry(value->owner) : NULL;
    return entry && instruction->block == entry && instruction->index == 0;
}

bool
ir_value_visit_lifetime_endpoints(
    const ir_value_t *value, ir_lifetime_endpoint_visitor_fn visitor,
    void *context)
{
    if (!value || !visitor || !value->owner || !value->owner->n_blocks
        || (value->kind != IR_VAL_PARAM && value->kind != IR_VAL_LOCAL
            && value->kind != IR_VAL_INSTR)) return false;
    ir_func_t *function = value->owner;
    ir_block_t *entry = ir_block_entry(function);
    if (!entry || !block_belongs_to(function, entry)) return false;
    bool local_starts_at_entry = value->kind == IR_VAL_LOCAL
        && !local_has_binding_initialization(value, function);

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        if (!block_belongs_to(function, block) || !block->n_instrs)
            return false;
        uint32_t terminators = 0;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!instruction_belongs_to(function, instruction)) return false;
            bool is_terminator = ir_opinfo_is_terminator(
                ir_module_op_info(function->owner, instruction->op));
            terminators += is_terminator;
            if (is_terminator && i + 1 != block->n_instrs) return false;
        }
        ir_instr_t *terminator = ir_block_terminator(block);
        if (terminators != 1 || !terminator) return false;
        for (uint32_t s = 0; s < ir_succ_count(terminator); s++) {
            ir_block_t *target = ir_succ_at(terminator, s);
            if (!block_belongs_to(function, target)) return false;
        }
    }

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *point = block->instrs[i];
            ir_liveness_t after = ir_value_alias_liveness_after(value, point);
            if (after != IR_LIVENESS_DEAD) continue;
            bool starts_here = value_starts_at_instruction(
                value, point, local_starts_at_entry);
            bool is_entry_parameter = value->kind == IR_VAL_PARAM
                && block == entry && i == 0;
            ir_liveness_t before = i
                ? ir_value_alias_liveness_after(value, block->instrs[i - 1])
                : alias_liveness_at_block_entry(value, function, block);
            bool terminator_fallback = point == terminator
                && before == IR_LIVENESS_UNKNOWN;
            if (!starts_here && !is_entry_parameter
                && before != IR_LIVENESS_LIVE && !terminator_fallback)
                continue;
            if (!visitor(context, (ir_lifetime_endpoint_t){
                    .function = function,
                    .binding = (ir_value_t *)value,
                    .point = point,
                })) return false;
        }
    }

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        if (ir_value_alias_liveness_after(value, terminator)
            != IR_LIVENESS_LIVE) continue;
        for (uint32_t s = 0; s < ir_succ_count(terminator); s++) {
            ir_block_t *target = ir_succ_at(terminator, s);
            if (ir_value_alias_liveness_on_edge(value, block, target)
                != IR_LIVENESS_DEAD) continue;
            if (!visitor(context, (ir_lifetime_endpoint_t){
                    .function = function,
                    .binding = (ir_value_t *)value,
                    .from = block,
                    .to = target,
                })) return false;
        }
    }
    return true;
}

bool
ir_value_can_finalize_on_edge(const ir_value_t *value,
                              const ir_block_t *from,
                              const ir_block_t *to)
{
    return ir_value_liveness_on_edge(value, from, to) == IR_LIVENESS_DEAD
        && ir_value_alias_liveness_on_edge(value, from, to)
            == IR_LIVENESS_DEAD;
}

bool ir_value_can_finalize_at(const ir_value_t *value, uint64_t point)
{
    value_set_t work = {0};
    bool already_present = false;
    if (!value || !value_set_add(&work, value, &already_present)) {
        free(work.items);
        return false;
    }

    bool safe = true;
    for (size_t i = 0; safe && i < work.count; i++) {
        const ir_value_t *current = work.items[i];
        bool complete = false;
        bool escapes = false;
        bool unresolved = false;
        uint64_t start = 0;
        uint64_t end = 0;
        uint64_t aliases = 0;
        uint64_t aliased_by = 0;
        if (!annot_bool(current, "ir.alias_analysis_complete", &complete)
            || !complete
            || !annot_bool(current, "ir.lifetime_escapes", &escapes)
            || escapes
            || !annot_bool(current, "ir.alias_unresolved", &unresolved)
            || unresolved
            || !annot_uint(current, "ir.lifetime_end", &end)
            || point < end
            || !annot_uint(current, "ir.alias_count", &aliases)
            || !annot_uint(current, "ir.aliased_by_count", &aliased_by)) {
            safe = false;
            break;
        }
        if (value_annot(current, "ir.lifetime_start")) {
            if (!annot_uint(current, "ir.lifetime_start", &start)
                || point < start) {
                safe = false;
                break;
            }
        }
        if (!check_alias_edges(current, "alias", aliases, point, &work)
            || !check_alias_edges(current, "aliased_by", aliased_by, point,
                                  &work)) {
            safe = false;
        }
    }
    free(work.items);
    return safe;
}

typedef struct local_use_probe_t {
    const ir_func_t *function;
    const ir_value_t *address;
    const struct local_use_probe_t *previous;
} local_use_probe_t;

static bool address_has_only_local_uses(const ir_func_t *function,
                                        const ir_value_t *address,
                                        const local_use_probe_t *previous);

static bool
call_argument_is_noescape(const ir_instr_t *call, uint32_t operand_index,
                          const local_use_probe_t *previous)
{
    if (call->op != IR_OP_CALL || !operand_index
        || operand_index >= call->n_operands
        || call->operands[0].kind != IR_OPD_VALUE
        || !call->operands[0].value
        || call->operands[0].value->kind != IR_VAL_FUNC)
        return false;
    ir_func_t *callee = call->operands[0].value->owner;
    if (!callee || operand_index > callee->n_params)
        return false;
    for (uint32_t i = 1; i <= operand_index; i++)
        if (call->operands[i].kind != IR_OPD_VALUE)
            return false;
    ir_value_t *parameter = callee->params[operand_index - 1];
    ir_annot_t *escapes = parameter
        ? ir_annot_get(&parameter->annots, "ir.lifetime_escapes") : nullptr;
    ir_annot_t *resolved = parameter
        ? ir_annot_get(&parameter->annots, "ir.alias_unresolved") : nullptr;
    ir_annot_t *complete = parameter
        ? ir_annot_get(&parameter->annots, "ir.alias_analysis_complete") : nullptr;
    ir_annot_t *aliases = parameter
        ? ir_annot_get(&parameter->annots, "ir.alias_count") : nullptr;
    ir_annot_t *aliased_by = parameter
        ? ir_annot_get(&parameter->annots, "ir.aliased_by_count") : nullptr;
    bool no_aliases = aliases && aliases->kind == IR_ANNOT_INT
        && !aliases->i.neg && !aliases->i.mag && aliased_by
        && aliased_by->kind == IR_ANNOT_INT && !aliased_by->i.neg
        && !aliased_by->i.mag;
    bool summarized = escapes && escapes->kind == IR_ANNOT_BOOL && !escapes->b
        && resolved && resolved->kind == IR_ANNOT_BOOL && !resolved->b
        && ((complete && complete->kind == IR_ANNOT_BOOL && complete->b)
            || no_aliases);
    return summarized || (callee->n_blocks
        && address_has_only_local_uses(callee, parameter, previous));
}

static bool
address_has_only_local_uses(const ir_func_t *function,
                            const ir_value_t *address,
                            const local_use_probe_t *previous)
{
    if (!function || !address) return false;
    for (const local_use_probe_t *at = previous; at; at = at->previous)
        if (at->function == function && at->address == address)
            return false;
    const local_use_probe_t probe = {
        .function = function, .address = address, .previous = previous,
    };
    size_t count = 1;
    size_t capacity = 4;
    const ir_value_t **addresses = malloc(capacity * sizeof(*addresses));
    if (!addresses) return false;
    addresses[0] = address;
    bool local = true;
    for (size_t cursor = 0; local && cursor < count; cursor++) {
        for (uint32_t i = 0; local && i < function->n_blocks; i++) {
            const ir_block_t *block = function->blocks[i];
            for (uint32_t j = 0; local && j < block->n_instrs; j++) {
                const ir_instr_t *use = block->instrs[j];
                for (uint32_t k = 0; local && k < use->n_operands; k++) {
                    const ir_operand_t *operand = &use->operands[k];
                    const ir_value_t *referenced = nullptr;
                    switch (operand->kind) {
                    case IR_OPD_VALUE:
                    case IR_OPD_SPREAD:
                    case IR_OPD_ARG:
                    case IR_OPD_OUT:
                    case IR_OPD_FRESH:
                        referenced = operand->value;
                        break;
                    case IR_OPD_PHI_ARM:
                        referenced = operand->phi.value;
                        break;
                    case IR_OPD_NAMED_ARG:
                        referenced = operand->named.value;
                        break;
                    default:
                        break;
                    }
                    if (referenced != addresses[cursor]) continue;
                    if (operand->kind == IR_OPD_FRESH
                        && use->op == IR_OP_CALL) continue;
                    if (operand->kind == IR_OPD_VALUE
                        && call_argument_is_noescape(use, k, &probe))
                        continue;
                    if (k != 0 || operand->kind != IR_OPD_VALUE) {
                        local = false;
                        break;
                    }
                    if (use->op == IR_OP_LOAD || use->op == IR_OP_STORE
                        || use->op == IR_OP_SEQUENCE_SET)
                        continue;
                    bool projection = use->op == IR_OP_FIELD
                        || use->op == IR_OP_INDEX
                        || use->op == IR_OP_OFFSET
                        || use->op == IR_OP_BITCAST
                        || (use->op == IR_OP_COPY && !use->dest_local);
                    if (!projection
                        || !use->dest || !use->dest->type
                        || th_kind_of(use->dest->type) != TH_INDIRECTION) {
                        local = false;
                        break;
                    }
                    bool seen = false;
                    for (size_t n = 0; n < count; n++)
                        if (addresses[n] == use->dest) seen = true;
                    if (seen) continue;
                    if (count == capacity) {
                        size_t next = capacity * 2;
                        const ir_value_t **grown = realloc(addresses,
                            next * sizeof(*addresses));
                        if (!grown) { local = false; break; }
                        addresses = grown;
                        capacity = next;
                    }
                    addresses[count++] = use->dest;
                }
            }
        }
    }
    free(addresses);
    return local;
}

bool
ir_allocation_has_only_local_uses(const ir_func_t *function,
                                   const ir_instr_t *allocation)
{
    bool required = false;
    if (!allocation || allocation->op != IR_OP_ALLOC || !allocation->dest
        || ir_instr_storage_class(allocation, &required) || required)
        return false;
    return address_has_only_local_uses(function, allocation->dest, nullptr);
}
