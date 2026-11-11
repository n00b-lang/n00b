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
    for (uint32_t param = 0; param < operand_index; param++)
        if (ir_call_argument(call, param).role != IR_ARG_VALUE)
            return false;
    ir_value_t *parameter = callee->params[operand_index - 1];
    ir_summary_t *summary = ir_summary_of(ir_analysis_summaries(), callee);
    if (summary && !(ir_summary_param_escape(summary, operand_index - 1)
                     & IR_SESCAPE_ADDRESS))
        return true;
    return callee->n_blocks
        && address_has_only_local_uses(callee, parameter, previous);
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
                    const ir_value_t *referenced =
                        ir_operand_referenced_value(operand);
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
                    if (ir_op_is_address_operand(use->op, k)) continue;
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

