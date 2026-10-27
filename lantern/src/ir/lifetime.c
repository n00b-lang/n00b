#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/liveness.h"
#include "ir/operands.h"
#include "ir/summary.h"

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


ir_liveness_t
ir_value_liveness_after(const ir_value_t *value, const ir_instr_t *point)
{
    if (!value || !point || !point->block || !point->block->owner)
        return IR_LIVENESS_UNKNOWN;
    return ir_live_after(ir_func_liveness(point->block->owner), value,
                         point);
}

ir_liveness_t
ir_value_liveness_on_edge(const ir_value_t *value, const ir_block_t *from,
                          const ir_block_t *to)
{
    if (!value || !from || !to || !from->owner || from->owner != to->owner)
        return IR_LIVENESS_UNKNOWN;
    return ir_live_on_edge(ir_func_liveness(from->owner), value, from, to);
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

struct ir_liveness_query {
    const ir_value_t *value;
    value_set_t aliases;
    bool complete;
    ir_live_t *live;
};

ir_liveness_query_t *
ir_liveness_query_new(const ir_value_t *value)
{
    if (!value) return NULL;
    ir_liveness_query_t *query = core_alloc(ir_liveness_query_t);
    query->value = value;
    query->complete = collect_alias_closure(value, &query->aliases);
    query->live = ir_func_liveness(value->owner);
    return query;
}

void
ir_liveness_query_free(ir_liveness_query_t *query)
{
    if (!query) return;
    core_list_free(query->aliases);
    core_free(query);
}

static ir_liveness_t
set_after(ir_live_t *live, const value_set_t *values, const ir_instr_t *point)
{
    bool unknown = false;
    for (size_t i = 0; i < values->len; i++) {
        ir_liveness_t state = ir_live_after(live, values->data[i], point);
        if (state == IR_LIVENESS_LIVE) return IR_LIVENESS_LIVE;
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

static void
set_after_each(ir_live_t *live, const value_set_t *values,
               const ir_block_t *block, ir_liveness_t *states,
               ir_liveness_t *scratch)
{
    for (uint32_t i = 0; i < block->n_instrs; i++)
        states[i] = IR_LIVENESS_DEAD;
    for (size_t v = 0; v < values->len; v++) {
        ir_live_after_each(live, values->data[v], block, scratch);
        for (uint32_t i = 0; i < block->n_instrs; i++)
            if (scratch[i] == IR_LIVENESS_LIVE
                || (scratch[i] == IR_LIVENESS_UNKNOWN
                    && states[i] == IR_LIVENESS_DEAD))
                states[i] = scratch[i];
    }
}

static ir_liveness_t
set_on_edge(ir_live_t *live, const value_set_t *values,
            const ir_block_t *from, const ir_block_t *to)
{
    bool unknown = false;
    for (size_t i = 0; i < values->len; i++) {
        ir_liveness_t state = ir_live_on_edge(live, values->data[i], from,
                                              to);
        if (state == IR_LIVENESS_LIVE) return IR_LIVENESS_LIVE;
        unknown |= state == IR_LIVENESS_UNKNOWN;
    }
    return unknown ? IR_LIVENESS_UNKNOWN : IR_LIVENESS_DEAD;
}

static ir_liveness_t
set_at_block_entry(ir_live_t *live, const value_set_t *values,
                   ir_block_t *block)
{
    uint32_t preds = ir_live_pred_count(live, block);
    if (!preds) return IR_LIVENESS_UNKNOWN;
    bool found_live = false;
    for (uint32_t p = 0; p < preds; p++) {
        ir_liveness_t state = set_on_edge(live, values,
                                          ir_live_pred_at(live, block, p),
                                          block);
        if (state == IR_LIVENESS_UNKNOWN) return IR_LIVENESS_UNKNOWN;
        found_live |= state == IR_LIVENESS_LIVE;
    }
    return found_live ? IR_LIVENESS_LIVE : IR_LIVENESS_DEAD;
}

ir_liveness_t
ir_liveness_query_alias_after(const ir_liveness_query_t *query,
                              const ir_instr_t *point)
{
    if (!query || !query->complete) return IR_LIVENESS_UNKNOWN;
    return set_after(query->live, &query->aliases, point);
}

ir_liveness_t
ir_liveness_query_alias_on_edge(const ir_liveness_query_t *query,
                                const ir_block_t *from,
                                const ir_block_t *to)
{
    if (!query || !query->complete) return IR_LIVENESS_UNKNOWN;
    return set_on_edge(query->live, &query->aliases, from, to);
}

ir_liveness_t
ir_liveness_query_direct_after(const ir_liveness_query_t *query,
                               const ir_instr_t *point)
{
    return query ? ir_live_after(query->live, query->value, point)
                 : IR_LIVENESS_UNKNOWN;
}

ir_liveness_t
ir_liveness_query_direct_on_edge(const ir_liveness_query_t *query,
                                 const ir_block_t *from,
                                 const ir_block_t *to)
{
    return query ? ir_live_on_edge(query->live, query->value, from, to)
                 : IR_LIVENESS_UNKNOWN;
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
    if (!entry || !ir_block_belongs_to(function, entry)) return false;
    bool local_starts_at_entry = value->kind == IR_VAL_LOCAL
        && !local_has_binding_initialization(value, function);
    if (!query->live || !ir_live_valid(query->live)) return false;
    if (!query->complete) return true;
    ir_live_t *live = query->live;
    const value_set_t *aliases = &query->aliases;
    bool ok = true;
    uint32_t widest = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        if (function->blocks[b]->n_instrs > widest)
            widest = function->blocks[b]->n_instrs;
    ir_liveness_t *states = core_alloc_array(ir_liveness_t, widest);
    ir_liveness_t *scratch = core_alloc_array(ir_liveness_t, widest);
    for (uint32_t b = 0; ok && b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        set_after_each(live, aliases, block, states, scratch);
        for (uint32_t i = 0; ok && i < block->n_instrs; i++) {
            ir_instr_t *point = block->instrs[i];
            if (states[i] != IR_LIVENESS_DEAD) continue;
            bool starts_here = value_starts_at_instruction(
                value, point, local_starts_at_entry);
            bool is_entry_parameter = value->kind == IR_VAL_PARAM
                && block == entry && i == 0;
            ir_liveness_t before = i
                ? states[i - 1]
                : set_at_block_entry(live, aliases, block);
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
        if (set_after(live, aliases, terminator) != IR_LIVENESS_LIVE)
            continue;
        for (uint32_t s = 0; ok && s < ir_succ_count(terminator); s++) {
            ir_block_t *target = ir_succ_at(terminator, s);
            if (set_on_edge(live, aliases, block, target) != IR_LIVENESS_DEAD)
                continue;
            ok = visitor(context, (ir_lifetime_endpoint_t){
                .function = function,
                .binding = (ir_value_t *)value,
                .from = block,
                .to = target,
            });
        }
    }
    core_free(states);
    core_free(scratch);
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

static bool
count_endpoint(void *context, ir_lifetime_endpoint_t endpoint)
{
    ir_cleanup_range_t *range = context;
    if (endpoint.from) range->edge_endpoints++;
    else range->point_endpoints++;
    return true;
}

static void
count_edge(ir_cleanup_range_t *range, ir_liveness_t direct,
           ir_liveness_t aliases, bool exit, bool ends)
{
    range->cfg_edges++;
    range->direct_live_edges += direct == IR_LIVENESS_LIVE;
    range->direct_dead_edges += direct == IR_LIVENESS_DEAD;
    range->direct_unknown_edges += direct == IR_LIVENESS_UNKNOWN;
    range->alias_live_edges += aliases == IR_LIVENESS_LIVE;
    range->alias_dead_edges += aliases == IR_LIVENESS_DEAD;
    range->alias_unknown_edges += aliases == IR_LIVENESS_UNKNOWN;
    range->range_ends += ends;
    bool unknown = direct == IR_LIVENESS_UNKNOWN
        || aliases == IR_LIVENESS_UNKNOWN;
    if (unknown) range->complete = false;
    if (!exit) return;
    range->exit_edges++;
    if (direct == IR_LIVENESS_DEAD && aliases == IR_LIVENESS_DEAD)
        range->safe_exit_edges++;
    else if (unknown) range->unknown_exit_edges++;
    else {
        range->live_exit_edges++;
        range->complete = false;
    }
}

bool
ir_value_cleanup_range(const ir_value_t *value, ir_value_escape_fn escapes,
                       void *context, ir_cleanup_range_t *out)
{
    *out = (ir_cleanup_range_t){.complete = true, .resolved = true};
    ir_func_t *function = value ? value->owner : NULL;
    if (!function || !function->n_blocks) return false;
    ir_liveness_query_t *query = ir_liveness_query_new(value);
    if (!query) return false;
    if (!ir_liveness_query_visit_endpoints(query, count_endpoint, out))
        out->resolved = false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        ir_instr_t *terminator = ir_block_terminator(block);
        if (!terminator) {
            out->resolved = false;
            continue;
        }
        uint32_t successors = ir_succ_count(terminator);
        ir_liveness_t after = ir_liveness_query_alias_after(query, terminator);
        for (uint32_t s = 0; s < successors; s++) {
            ir_block_t *target = ir_succ_at(terminator, s);
            if (!target) {
                out->resolved = false;
                continue;
            }
            ir_liveness_t aliases = ir_liveness_query_alias_on_edge(query,
                block, target);
            count_edge(out, ir_liveness_query_direct_on_edge(query, block,
                                                             target),
                       aliases, false,
                       after == IR_LIVENESS_LIVE
                           && aliases == IR_LIVENESS_DEAD);
        }
        if (!successors) {
            ir_liveness_t aliases = ir_liveness_query_alias_after(query,
                terminator);
            count_edge(out, ir_liveness_query_direct_after(query, terminator),
                       aliases, true, aliases == IR_LIVENESS_DEAD);
        }
    }
    ir_liveness_query_free(query);
    out->escapes = escapes && escapes(context, value);
    out->resolved &= out->cfg_edges != 0;
    out->complete &= out->resolved && !out->escapes;
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
    ir_summary_t *summary = ir_summary_of(ir_analysis_summaries(), callee);
    if (summary && !(ir_summary_param_escape(summary, operand_index - 1)
                     & IR_SESCAPE_ADDRESS))
        return true;
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

