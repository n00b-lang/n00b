#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/doms.h"
#include "lib/list.h"
#include "th/typehash.h"

typedef const ir_value_t *ir_value_ptr_t;
core_list_decl(ir_value_ptr_t);
typedef core_list_t(ir_value_ptr_t) value_set_t;

static ir_annot_t *value_annot(const ir_value_t *value, core_string_t key)
{
    return ir_annot_get(ir_value_annots((ir_value_t *)value), key);
}

static core_string_t edge_key(char *buffer, size_t capacity,
                              core_string_t prefix, uint64_t index,
                              core_string_t suffix)
{
    int written = snprintf(buffer, capacity, "ir.%.*s%llu%.*s",
                           (int)prefix.u8_bytes, prefix.data,
                           (unsigned long long)index,
                           (int)suffix.u8_bytes, suffix.data);
    size_t length = written < 0 ? 0 : (size_t)written;

    if (length >= capacity) {
        length = capacity - 1;
    }
    return core_string_borrow(buffer, length);
}

static bool annot_bool(const ir_value_t *value, core_string_t key, bool *out)
{
    ir_annot_t *annot = value_annot(value, key);
    if (!annot || annot->kind != IR_ANNOT_BOOL) return false;
    *out = annot->b;
    return true;
}

static bool annot_uint(const ir_value_t *value, core_string_t key,
                       uint64_t *out)
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
    if (core_option_is_set(core_list_find(*set, value))) {
        *already_present = true;
        return true;
    }
    *already_present = false;
    return core_list_try_push(*set, value);
}

static bool check_alias_edges(const ir_value_t *value, core_string_t prefix,
                              uint64_t count, uint64_t point,
                              value_set_t *work)
{
    char key[64];
    for (uint64_t i = 0; i < count; i++) {
        ir_annot_t *edge = value_annot(
            value, edge_key(key, sizeof(key), prefix, i,
                            CORE_STRING_STATIC("")));
        if (!edge || edge->kind != IR_ANNOT_VALUE || !edge->v) return false;

        bool escapes = false;
        if (!annot_bool(value,
                        edge_key(key, sizeof(key), prefix, i,
                                 CORE_STRING_STATIC("_escapes")),
                        &escapes)
            || escapes) return false;

        bool has_last_use = false;
        if (!annot_bool(value,
                        edge_key(key, sizeof(key), prefix, i,
                                 CORE_STRING_STATIC("_has_last_use")),
                        &has_last_use)
            || !has_last_use)
            return false;

        uint64_t last_use = 0;
        if (!annot_uint(value,
                        edge_key(key, sizeof(key), prefix, i,
                                 CORE_STRING_STATIC("_last_use")),
                        &last_use)
            || last_use >= point)
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
collect_alias_edges(const ir_value_t *value, core_string_t prefix,
                    uint64_t count, value_set_t *work)
{
    char key[64];
    for (uint64_t i = 0; i < count; i++) {
        ir_annot_t *edge = value_annot(
            value, edge_key(key, sizeof(key), prefix, i,
                            CORE_STRING_STATIC("")));
        if (!edge || edge->kind != IR_ANNOT_VALUE || !edge->v) return false;

        bool escapes = false;
        if (!annot_bool(value,
                        edge_key(key, sizeof(key), prefix, i,
                                 CORE_STRING_STATIC("_escapes")),
                        &escapes)
            || escapes) return false;

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
            if (!annot_uint(derived, CORE_STRING_STATIC("ir.alias_count"),
                            &count)) {
                if (value_annot(derived, CORE_STRING_STATIC("ir.alias_count")))
                    return false;
                continue;
            }
            for (uint64_t edge = 0; edge < count; edge++) {
                char key[64];
                ir_annot_t *target = value_annot(
                    derived, edge_key(key, sizeof(key),
                                      CORE_STRING_STATIC("alias"), edge,
                                      CORE_STRING_STATIC("")));
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
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW:
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
            case IR_OP_BOX_RAW:
            case IR_OP_BOX_FROM_RAW:
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

static bool
collect_alias_closure(const ir_value_t *value, value_set_t *work)
{
    bool already_present = false;
    if (!value || !value_set_add(work, value, &already_present)) return false;

    bool complete = true;
    for (size_t i = 0; complete && i < work->len; i++) {
        const ir_value_t *current = work->data[i];
        bool analysis_complete = false;
        bool escapes = false;
        bool unresolved = false;
        uint64_t aliases = 0;
        uint64_t aliased_by = 0;
        complete = annot_bool(current,
                              CORE_STRING_STATIC("ir.alias_analysis_complete"),
                              &analysis_complete)
            && analysis_complete
            && annot_bool(current, CORE_STRING_STATIC("ir.lifetime_escapes"),
                          &escapes)
            && !escapes
            && annot_bool(current, CORE_STRING_STATIC("ir.alias_unresolved"),
                          &unresolved)
            && !unresolved
            && annot_uint(current, CORE_STRING_STATIC("ir.alias_count"),
                          &aliases)
            && annot_uint(current, CORE_STRING_STATIC("ir.aliased_by_count"),
                          &aliased_by)
            && collect_alias_edges(current, CORE_STRING_STATIC("alias"),
                                   aliases, work)
            && collect_alias_edges(current, CORE_STRING_STATIC("aliased_by"),
                                   aliased_by, work)
            && collect_inverse_alias_edges(current, work)
            && collect_structural_aliases(current, work);
    }
    return complete;
}

static ir_liveness_t
alias_liveness_after_closure(const value_set_t *work,
                             const ir_instr_t *point)
{
    bool unknown = false;
    for (size_t i = 0; i < work->len; i++) {
        ir_liveness_t state = ir_value_liveness_after(work->data[i], point);
        if (state == IR_LIVENESS_LIVE) return IR_LIVENESS_LIVE;
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

static ir_liveness_t
alias_liveness_on_edge_closure(const value_set_t *work,
                               const ir_block_t *from,
                               const ir_block_t *to)
{
    bool unknown = false;
    for (size_t i = 0; i < work->len; i++) {
        ir_liveness_t state = ir_value_liveness_on_edge(work->data[i], from,
                                                        to);
        if (state == IR_LIVENESS_LIVE) return IR_LIVENESS_LIVE;
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

ir_liveness_t
ir_value_alias_liveness_after(const ir_value_t *value,
                              const ir_instr_t *point)
{
    value_set_t work = {0};
    bool complete = collect_alias_closure(value, &work);
    ir_liveness_t result = complete
        ? alias_liveness_after_closure(&work, point) : IR_LIVENESS_UNKNOWN;
    core_list_free(work);
    return result;
}

ir_liveness_t
ir_value_alias_liveness_on_edge(const ir_value_t *value,
                                const ir_block_t *from,
                                const ir_block_t *to)
{
    value_set_t work = {0};
    bool complete = collect_alias_closure(value, &work);
    ir_liveness_t result = complete
        ? alias_liveness_on_edge_closure(&work, from, to)
        : IR_LIVENESS_UNKNOWN;
    core_list_free(work);
    return result;
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

typedef struct {
    const ir_value_t *value;
    bool valid;
    int64_t *last_use;
    bool *reaches_use;
    bool *live_out;
    bool *after_escapes_def;
    bool *edge_escapes_def;
} alias_table_t;

typedef struct {
    const ir_func_t *function;
    uint32_t block_count;
    bool *reachable;
    uint32_t *pred_start;
    const ir_block_t **preds;
    alias_table_t *aliases;
    size_t alias_count;
} liveness_table_t;

static void
reach_from_entry(const ir_func_t *function, const ir_block_t *entry,
                 const ir_block_t *forbidden, bool *seen, ir_block_t **work)
{
    size_t work_count = 0;
    seen[entry->index] = true;
    work[work_count++] = (ir_block_t *)entry;
    while (work_count) {
        ir_block_t *block = work[--work_count];
        ir_instr_t *terminator = ir_block_terminator(block);
        for (uint32_t s = 0; s < ir_succ_count(terminator); s++) {
            ir_block_t *successor = ir_succ_at(terminator, s);
            if (successor == forbidden || seen[successor->index]) continue;
            seen[successor->index] = true;
            work[work_count++] = successor;
        }
    }
    (void)function;
}

static void
liveness_table_free(liveness_table_t *table)
{
    for (size_t i = 0; i < table->alias_count; i++) {
        alias_table_t *alias = &table->aliases[i];
        free(alias->last_use);
        free(alias->reaches_use);
        free(alias->live_out);
        free(alias->after_escapes_def);
        free(alias->edge_escapes_def);
    }
    free(table->aliases);
    free(table->reachable);
    free(table->pred_start);
    free(table->preds);
}

static bool
alias_table_build(liveness_table_t *table, alias_table_t *alias,
                  const ir_block_t *entry, ir_block_t **work)
{
    const ir_func_t *function = table->function;
    const ir_value_t *value = alias->value;
    uint32_t count = table->block_count;
    alias->valid = (value->kind == IR_VAL_PARAM
                    || value->kind == IR_VAL_LOCAL
                    || value->kind == IR_VAL_INSTR)
        && value->owner == function
        && (value->kind != IR_VAL_INSTR
            || (instruction_belongs_to(function, value->def)
                && value->def->dest == value));
    if (!alias->valid) return true;
    alias->last_use = malloc(count * sizeof(*alias->last_use));
    alias->reaches_use = calloc(count, sizeof(bool));
    alias->live_out = calloc(count, sizeof(bool));
    if (!alias->last_use || !alias->reaches_use || !alias->live_out)
        return false;
    size_t work_count = 0;
    for (uint32_t b = 0; b < count; b++) {
        const ir_block_t *block = function->blocks[b];
        alias->last_use[b] = -1;
        for (uint32_t i = 0; i < block->n_instrs; i++)
            if (instruction_uses(block->instrs[i], value))
                alias->last_use[b] = i;
        if (alias->last_use[b] >= 0) {
            alias->reaches_use[b] = true;
            work[work_count++] = (ir_block_t *)block;
        }
    }
    while (work_count) {
        const ir_block_t *block = work[--work_count];
        for (uint32_t p = table->pred_start[block->index];
             p < table->pred_start[block->index + 1]; p++) {
            const ir_block_t *pred = table->preds[p];
            alias->live_out[pred->index] = true;
            if (alias->reaches_use[pred->index]) continue;
            alias->reaches_use[pred->index] = true;
            work[work_count++] = (ir_block_t *)pred;
        }
    }
    if (value->kind != IR_VAL_INSTR) return true;
    const ir_block_t *def_block = value->def->block;
    alias->after_escapes_def = calloc(count, sizeof(bool));
    alias->edge_escapes_def = calloc(count, sizeof(bool));
    if (!alias->after_escapes_def || !alias->edge_escapes_def) return false;
    if (entry != def_block)
        reach_from_entry(function, entry, def_block,
                         alias->after_escapes_def, work);
    reach_from_entry(function, entry, def_block, alias->edge_escapes_def,
                     work);
    return true;
}

static bool
liveness_table_build(liveness_table_t *table, const ir_func_t *function,
                     const value_set_t *aliases)
{
    *table = (liveness_table_t){.function = function,
                                .block_count = function->n_blocks};
    uint32_t count = function->n_blocks;
    ir_block_t *entry = ir_block_entry((ir_func_t *)function);
    ir_block_t **work = malloc(count * sizeof(*work));
    table->reachable = calloc(count, sizeof(bool));
    table->pred_start = calloc((size_t)count + 1, sizeof(uint32_t));
    table->aliases = calloc(aliases->len ? aliases->len : 1,
                            sizeof(*table->aliases));
    bool ok = work && table->reachable && table->pred_start
        && table->aliases;
    size_t edges = 0;
    for (uint32_t b = 0; ok && b < count; b++) {
        ir_instr_t *terminator = ir_block_terminator(function->blocks[b]);
        for (uint32_t s = 0; s < ir_succ_count(terminator); s++) {
            table->pred_start[ir_succ_at(terminator, s)->index + 1]++;
            edges++;
        }
    }
    for (uint32_t b = 0; ok && b < count; b++)
        table->pred_start[b + 1] += table->pred_start[b];
    uint32_t *fill = ok ? calloc(count ? count : 1, sizeof(uint32_t))
                        : nullptr;
    table->preds = ok ? malloc((edges ? edges : 1) * sizeof(*table->preds))
                      : nullptr;
    ok = ok && fill && table->preds;
    for (uint32_t b = 0; ok && b < count; b++) {
        ir_instr_t *terminator = ir_block_terminator(function->blocks[b]);
        for (uint32_t s = 0; s < ir_succ_count(terminator); s++) {
            uint32_t to = ir_succ_at(terminator, s)->index;
            table->preds[table->pred_start[to] + fill[to]++] =
                function->blocks[b];
        }
    }
    free(fill);
    if (ok) reach_from_entry(function, entry, nullptr, table->reachable,
                             work);
    for (size_t i = 0; ok && i < aliases->len; i++) {
        table->aliases[i].value = aliases->data[i];
        table->alias_count = i + 1;
        ok = alias_table_build(table, &table->aliases[i], entry, work);
    }
    free(work);
    if (!ok) liveness_table_free(table);
    return ok;
}

static ir_liveness_t
table_alias_after(const liveness_table_t *table, const alias_table_t *alias,
                  const ir_instr_t *point)
{
    const ir_value_t *value = alias->value;
    const ir_block_t *block = point->block;
    if (!alias->valid || !table->reachable[block->index])
        return IR_LIVENESS_UNKNOWN;
    if (value->kind == IR_VAL_INSTR) {
        const ir_block_t *def_block = value->def->block;
        if ((def_block == block && value->def->index > point->index)
            || !table->reachable[def_block->index]
            || (def_block != block
                && alias->after_escapes_def[block->index]))
            return IR_LIVENESS_UNKNOWN;
    }
    return alias->last_use[block->index] > (int64_t)point->index
        || alias->live_out[block->index]
        ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
}

static ir_liveness_t
table_alias_on_edge(const liveness_table_t *table,
                    const alias_table_t *alias, const ir_block_t *from,
                    const ir_block_t *to)
{
    const ir_value_t *value = alias->value;
    if (!alias->valid || !table->reachable[from->index])
        return IR_LIVENESS_UNKNOWN;
    if (value->kind == IR_VAL_INSTR) {
        const ir_block_t *def_block = value->def->block;
        if (def_block == from) {
            if (value->def->index
                >= ir_block_terminator((ir_block_t *)from)->index)
                return IR_LIVENESS_UNKNOWN;
        }
        else if (def_block == to
                 || alias->edge_escapes_def[from->index]
                 || !table->reachable[def_block->index])
            return IR_LIVENESS_UNKNOWN;
    }
    return alias->reaches_use[to->index]
        ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
}

static ir_liveness_t
table_after(const liveness_table_t *table, const ir_instr_t *point)
{
    bool unknown = false;
    for (size_t i = 0; i < table->alias_count; i++) {
        ir_liveness_t state = table_alias_after(table, &table->aliases[i],
                                                point);
        if (state == IR_LIVENESS_LIVE) return IR_LIVENESS_LIVE;
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

static ir_liveness_t
table_on_edge(const liveness_table_t *table, const ir_block_t *from,
              const ir_block_t *to)
{
    bool unknown = false;
    for (size_t i = 0; i < table->alias_count; i++) {
        ir_liveness_t state = table_alias_on_edge(table, &table->aliases[i],
                                                  from, to);
        if (state == IR_LIVENESS_LIVE) return IR_LIVENESS_LIVE;
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

static ir_liveness_t
table_at_block_entry(const liveness_table_t *table, const ir_block_t *block)
{
    uint32_t first = table->pred_start[block->index];
    uint32_t last = table->pred_start[block->index + 1];
    if (first == last) return IR_LIVENESS_UNKNOWN;
    bool found_live = false;
    for (uint32_t p = first; p < last; p++) {
        ir_liveness_t state = table_on_edge(table, table->preds[p], block);
        if (state == IR_LIVENESS_UNKNOWN) return IR_LIVENESS_UNKNOWN;
        found_live |= state == IR_LIVENESS_LIVE;
    }
    return found_live ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
}

static bool
cfg_verified(const ir_func_t *function)
{
    ir_block_t *entry = function && function->n_blocks
        ? ir_block_entry((ir_func_t *)function) : NULL;
    if (!entry || !block_belongs_to(function, entry)) return false;
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
    return true;
}

struct ir_liveness_query {
    const ir_value_t *value;
    value_set_t aliases;
    bool complete;
    bool cfg_valid;
    bool alias_tabled;
    bool direct_tabled;
    liveness_table_t alias_table;
    liveness_table_t direct_table;
};

ir_liveness_query_t *
ir_liveness_query_new(const ir_value_t *value)
{
    if (!value) return NULL;
    ir_liveness_query_t *query = calloc(1, sizeof(*query));
    if (!query) return NULL;
    query->value = value;
    query->complete = collect_alias_closure(value, &query->aliases);
    query->cfg_valid = cfg_verified(value->owner);
    if (!query->cfg_valid) return query;
    value_set_t direct = {0};
    if (core_list_try_push(direct, value))
        query->direct_tabled = liveness_table_build(&query->direct_table,
            value->owner, &direct);
    core_list_free(direct);
    if (query->complete)
        query->alias_tabled = liveness_table_build(&query->alias_table,
            value->owner, &query->aliases);
    return query;
}

void
ir_liveness_query_free(ir_liveness_query_t *query)
{
    if (!query) return;
    if (query->alias_tabled) liveness_table_free(&query->alias_table);
    if (query->direct_tabled) liveness_table_free(&query->direct_table);
    core_list_free(query->aliases);
    free(query);
}

static bool
tabled_point(const ir_liveness_query_t *query, const ir_instr_t *point)
{
    return query->cfg_valid && instruction_belongs_to(query->value->owner,
                                                      point);
}

static bool
tabled_edge(const ir_liveness_query_t *query, const ir_block_t *from,
            const ir_block_t *to)
{
    const ir_func_t *function = query->value->owner;
    if (!query->cfg_valid || !block_belongs_to(function, from)
        || !block_belongs_to(function, to)) return false;
    ir_instr_t *terminator = ir_block_terminator((ir_block_t *)from);
    for (uint32_t s = 0; s < ir_succ_count(terminator); s++)
        if (ir_succ_at(terminator, s) == to) return true;
    return false;
}

ir_liveness_t
ir_liveness_query_alias_after(const ir_liveness_query_t *query,
                              const ir_instr_t *point)
{
    if (!query || !query->complete) return IR_LIVENESS_UNKNOWN;
    return query->alias_tabled && tabled_point(query, point)
        ? table_after(&query->alias_table, point)
        : alias_liveness_after_closure(&query->aliases, point);
}

ir_liveness_t
ir_liveness_query_alias_on_edge(const ir_liveness_query_t *query,
                                const ir_block_t *from,
                                const ir_block_t *to)
{
    if (!query || !query->complete) return IR_LIVENESS_UNKNOWN;
    return query->alias_tabled && tabled_edge(query, from, to)
        ? table_on_edge(&query->alias_table, from, to)
        : alias_liveness_on_edge_closure(&query->aliases, from, to);
}

ir_liveness_t
ir_liveness_query_direct_after(const ir_liveness_query_t *query,
                               const ir_instr_t *point)
{
    if (!query) return IR_LIVENESS_UNKNOWN;
    return query->direct_tabled && tabled_point(query, point)
        ? table_after(&query->direct_table, point)
        : ir_value_liveness_after(query->value, point);
}

ir_liveness_t
ir_liveness_query_direct_on_edge(const ir_liveness_query_t *query,
                                 const ir_block_t *from,
                                 const ir_block_t *to)
{
    if (!query) return IR_LIVENESS_UNKNOWN;
    return query->direct_tabled && tabled_edge(query, from, to)
        ? table_on_edge(&query->direct_table, from, to)
        : ir_value_liveness_on_edge(query->value, from, to);
}

bool
ir_liveness_query_visit_endpoints(const ir_liveness_query_t *query,
    ir_lifetime_endpoint_visitor_fn visitor, void *context)
{
    const ir_value_t *value = query ? query->value : NULL;
    if (!value || !visitor || !value->owner || !value->owner->n_blocks
        || (value->kind != IR_VAL_PARAM && value->kind != IR_VAL_LOCAL
            && value->kind != IR_VAL_INSTR)) return false;
    ir_func_t *function = value->owner;
    ir_block_t *entry = ir_block_entry(function);
    if (!entry || !block_belongs_to(function, entry)) return false;
    bool local_starts_at_entry = value->kind == IR_VAL_LOCAL
        && !local_has_binding_initialization(value, function);
    if (!query->cfg_valid) return false;
    if (!query->complete) return true;
    if (!query->alias_tabled) return false;
    const liveness_table_t *table = &query->alias_table;
    bool ok = true;
    for (uint32_t b = 0; ok && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        for (uint32_t i = 0; ok && i < block->n_instrs; i++) {
            ir_instr_t *point = block->instrs[i];
            if (table_after(table, point) != IR_LIVENESS_DEAD) continue;
            bool starts_here = value_starts_at_instruction(
                value, point, local_starts_at_entry);
            bool is_entry_parameter = value->kind == IR_VAL_PARAM
                && block == entry && i == 0;
            ir_liveness_t before = i
                ? table_after(table, block->instrs[i - 1])
                : table_at_block_entry(table, block);
            bool terminator_fallback = point == terminator
                && before == IR_LIVENESS_UNKNOWN;
            if (!starts_here && !is_entry_parameter
                && before != IR_LIVENESS_LIVE && !terminator_fallback)
                continue;
            ok = visitor(context, (ir_lifetime_endpoint_t){
                .function = function,
                .binding = (ir_value_t *)value,
                .point = point,
            });
        }
    }

    for (uint32_t b = 0; ok && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        if (table_after(table, terminator) != IR_LIVENESS_LIVE) continue;
        for (uint32_t s = 0; ok && s < ir_succ_count(terminator); s++) {
            ir_block_t *target = ir_succ_at(terminator, s);
            if (table_on_edge(table, block, target) != IR_LIVENESS_DEAD)
                continue;
            ok = visitor(context, (ir_lifetime_endpoint_t){
                .function = function,
                .binding = (ir_value_t *)value,
                .from = block,
                .to = target,
            });
        }
    }
    return ok;
}

bool
ir_value_visit_lifetime_endpoints(
    const ir_value_t *value, ir_lifetime_endpoint_visitor_fn visitor,
    void *context)
{
    ir_liveness_query_t *query = ir_liveness_query_new(value);
    if (!query) return false;
    bool result = ir_liveness_query_visit_endpoints(query, visitor, context);
    ir_liveness_query_free(query);
    return result;
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
        core_list_free(work);
        return false;
    }

    bool safe = true;
    for (size_t i = 0; safe && i < work.len; i++) {
        const ir_value_t *current = work.data[i];
        bool complete = false;
        bool escapes = false;
        bool unresolved = false;
        uint64_t start = 0;
        uint64_t end = 0;
        uint64_t aliases = 0;
        uint64_t aliased_by = 0;
        if (!annot_bool(current,
                        CORE_STRING_STATIC("ir.alias_analysis_complete"),
                        &complete)
            || !complete
            || !annot_bool(current, CORE_STRING_STATIC("ir.lifetime_escapes"),
                           &escapes)
            || escapes
            || !annot_bool(current, CORE_STRING_STATIC("ir.alias_unresolved"),
                           &unresolved)
            || unresolved
            || !annot_uint(current, CORE_STRING_STATIC("ir.lifetime_end"),
                           &end)
            || point < end
            || !annot_uint(current, CORE_STRING_STATIC("ir.alias_count"),
                           &aliases)
            || !annot_uint(current, CORE_STRING_STATIC("ir.aliased_by_count"),
                           &aliased_by)) {
            safe = false;
            break;
        }
        if (value_annot(current, CORE_STRING_STATIC("ir.lifetime_start"))) {
            if (!annot_uint(current, CORE_STRING_STATIC("ir.lifetime_start"),
                            &start)
                || point < start) {
                safe = false;
                break;
            }
        }
        if (!check_alias_edges(current, CORE_STRING_STATIC("alias"), aliases,
                               point, &work)
            || !check_alias_edges(current, CORE_STRING_STATIC("aliased_by"),
                                  aliased_by, point, &work)) {
            safe = false;
        }
    }
    core_list_free(work);
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
        ? ir_annot_get(&parameter->annots,
                       CORE_STRING_STATIC("ir.lifetime_escapes"))
        : nullptr;
    ir_annot_t *resolved = parameter
        ? ir_annot_get(&parameter->annots,
                       CORE_STRING_STATIC("ir.alias_unresolved"))
        : nullptr;
    ir_annot_t *complete = parameter
        ? ir_annot_get(&parameter->annots,
                       CORE_STRING_STATIC("ir.alias_analysis_complete"))
        : nullptr;
    ir_annot_t *aliases = parameter
        ? ir_annot_get(&parameter->annots,
                       CORE_STRING_STATIC("ir.alias_count"))
        : nullptr;
    ir_annot_t *aliased_by = parameter
        ? ir_annot_get(&parameter->annots,
                       CORE_STRING_STATIC("ir.aliased_by_count"))
        : nullptr;
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
    value_set_t addresses = {0};
    if (!core_list_try_push(addresses, address)) return false;
    bool local = true;
    for (size_t cursor = 0; local && cursor < addresses.len; cursor++) {
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
                    if (referenced != addresses.data[cursor]) continue;
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
                    if (!value_set_add(&addresses, use->dest, &seen)) {
                        local = false;
                        break;
                    }
                }
            }
        }
    }
    core_list_free(addresses);
    return local;
}

bool
ir_allocation_has_only_local_uses(const ir_func_t *function,
                                   const ir_instr_t *allocation)
{
    bool required = false;
    if (!allocation || allocation->op != IR_OP_ALLOC || !allocation->dest
        || ir_instr_storage_class(allocation, &required).data || required)
        return false;
    ir_annot_t *placed = ir_annot_get(&((ir_instr_t *)allocation)->annots,
                                      CORE_STRING_STATIC("ir.allocation_local"));
    if (placed && placed->kind == IR_ANNOT_BOOL) return placed->b;
    return address_has_only_local_uses(function, allocation->dest, nullptr);
}

struct ir_values_liveness {
    ir_func_t         *function;
    ir_value_t *const *values;
    size_t             count;
    uint32_t           words;
    uint64_t          *live_in;
    uint64_t          *live_out;
};

static bool
values_index(const ir_values_liveness_t *liveness, const ir_value_t *value,
             uint32_t *index)
{
    for (size_t i = 0; value && i < liveness->count; i++)
        if (liveness->values[i] == value) {
            *index = (uint32_t)i;
            return true;
        }
    return false;
}

static const ir_value_t *
values_operand(const ir_instr_t *instruction, uint32_t index)
{
    const ir_operand_t *operand = &instruction->operands[index];
    return operand->kind == IR_OPD_ARG ? operand->value
                                       : ir_operand_value(operand);
}

static void
values_set(uint64_t *bits, uint32_t index, bool live)
{
    if (live) bits[index / 64] |= UINT64_C(1) << (index % 64);
    else bits[index / 64] &= ~(UINT64_C(1) << (index % 64));
}

static bool
values_any(const uint64_t *bits, uint32_t words)
{
    for (uint32_t w = 0; w < words; w++)
        if (bits[w]) return true;
    return false;
}

static void
values_step(const ir_values_liveness_t *liveness,
            const ir_instr_t *instruction, uint64_t *live)
{
    uint32_t index = 0;
    if (instruction->dest && values_index(liveness, instruction->dest, &index))
        values_set(live, index, false);
    if (instruction->op == IR_OP_COPY && instruction->dest_local
        && values_index(liveness, ir_local_value(liveness->function,
                                                 instruction->dest_local),
                        &index))
        values_set(live, index, false);
    if (instruction->op == IR_OP_PHI) return;
    for (uint32_t o = 0; o < instruction->n_operands; o++)
        if (values_index(liveness, values_operand(instruction, o), &index))
            values_set(live, index, true);
}

static void
values_edge_uses(const ir_values_liveness_t *liveness, const ir_block_t *from,
                 const ir_block_t *to, uint64_t *live)
{
    for (uint32_t i = 0; i < to->n_instrs && to->instrs[i]->op == IR_OP_PHI;
         i++) {
        const ir_instr_t *phi = to->instrs[i];
        for (uint32_t o = 0; o < phi->n_operands; o++) {
            const ir_operand_t *operand = &phi->operands[o];
            uint32_t index = 0;
            if (operand->kind == IR_OPD_PHI_ARM && operand->phi.block == from
                && values_index(liveness, operand->phi.value, &index))
                values_set(live, index, true);
        }
    }
}

ir_values_liveness_t *
ir_values_liveness_new(ir_func_t *function, ir_value_t *const *values,
                       size_t count)
{
    ir_values_liveness_t *liveness = calloc(1, sizeof(*liveness));
    if (!liveness) return NULL;
    liveness->function = function;
    liveness->values   = values;
    liveness->count    = count;
    liveness->words    = (uint32_t)((count + 63) / 64);
    size_t total = (size_t)function->n_blocks * liveness->words;
    liveness->live_in  = calloc(total ? total : 1, sizeof(uint64_t));
    liveness->live_out = calloc(total ? total : 1, sizeof(uint64_t));
    uint64_t *scratch  = calloc(liveness->words ? liveness->words : 1,
                                sizeof(uint64_t));
    if (!liveness->live_in || !liveness->live_out || !scratch) {
        free(scratch);
        ir_values_liveness_free(liveness);
        return NULL;
    }
    size_t bytes = liveness->words * sizeof(uint64_t);
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = function->n_blocks; b-- > 0;) {
            ir_block_t *block = function->blocks[b];
            memset(scratch, 0, bytes);
            ir_instr_t *terminator = ir_block_terminator(block);
            for (uint32_t s = 0; terminator && s < ir_succ_count(terminator);
                 s++) {
                ir_block_t *successor = ir_succ_at(terminator, s);
                for (uint32_t w = 0; w < liveness->words; w++)
                    scratch[w] |= liveness->live_in[
                        (size_t)successor->index * liveness->words + w];
                values_edge_uses(liveness, block, successor, scratch);
            }
            memcpy(&liveness->live_out[(size_t)b * liveness->words], scratch,
                   bytes);
            for (uint32_t i = block->n_instrs; i-- > 0;)
                values_step(liveness, block->instrs[i], scratch);
            uint64_t *in = &liveness->live_in[(size_t)b * liveness->words];
            if (memcmp(in, scratch, bytes)) {
                memcpy(in, scratch, bytes);
                changed = true;
            }
        }
    }
    free(scratch);
    return liveness;
}

void
ir_values_liveness_free(ir_values_liveness_t *liveness)
{
    if (!liveness) return;
    free(liveness->live_in);
    free(liveness->live_out);
    free(liveness);
}

bool
ir_values_live_after_each(const ir_values_liveness_t *liveness,
                          const ir_block_t *block, bool *after)
{
    uint64_t *live = calloc(liveness->words ? liveness->words : 1,
                            sizeof(uint64_t));
    if (!live) return false;
    memcpy(live, &liveness->live_out[(size_t)block->index * liveness->words],
           liveness->words * sizeof(uint64_t));
    for (uint32_t i = block->n_instrs; i-- > 0;) {
        after[i] = values_any(live, liveness->words);
        values_step(liveness, block->instrs[i], live);
    }
    free(live);
    return true;
}

bool
ir_values_live_on_edge(const ir_values_liveness_t *liveness,
                       const ir_block_t *from, const ir_block_t *to)
{
    uint64_t *live = calloc(liveness->words ? liveness->words : 1,
                            sizeof(uint64_t));
    if (!live) return false;
    memcpy(live, &liveness->live_in[(size_t)to->index * liveness->words],
           liveness->words * sizeof(uint64_t));
    values_edge_uses(liveness, from, to, live);
    bool any = values_any(live, liveness->words);
    free(live);
    return any;
}
