#include "ir/storage_origin.h"

#include <stdlib.h>
#include <string.h>

#include "ir/layout.h"
#include "lib/dict.h"

typedef struct {
    const void *binding;
    const ir_value_t *source;
} storage_assignment_t;

typedef enum {
    ORIGIN_RAW_MAPPING,
    ORIGIN_UNMANAGED_OBJECT,
    ORIGIN_MODE_COUNT,
} origin_mode_t;

typedef struct {
    const ir_instr_t *field;
    const ir_value_t *source;
} storage_field_assignment_t;

typedef struct {
    const ir_instr_t *field;
    origin_mode_t mode;
    uintptr_t state;
} storage_field_proof_t;

struct ir_storage_origin_t {
    ir_func_t *const *functions;
    size_t function_count;
    ir_mapping_base_source_fn source;
    ir_unmanaged_parameter_source_fn unmanaged_parameter;
    ir_closed_callers_fn closed_callers;
    bool closed_field_writes;
    void *context;
    core_dict_t values[ORIGIN_MODE_COUNT];
    core_dict_t locals[ORIGIN_MODE_COUNT];
    core_dict_t globals[ORIGIN_MODE_COUNT];
    core_dict_t returns[ORIGIN_MODE_COUNT];
    storage_assignment_t *assignments;
    size_t assignment_count;
    size_t assignment_capacity;
    storage_field_assignment_t *field_assignments;
    size_t field_assignment_count;
    size_t field_assignment_capacity;
    storage_field_proof_t *field_proofs;
    size_t field_proof_count;
    size_t field_proof_capacity;
    ir_instr_t **calls;
    size_t call_count;
    size_t call_capacity;
    const ir_func_t **indirect_functions;
    size_t indirect_count;
    size_t indirect_capacity;
    const char **dynamic_methods;
    size_t method_count;
    size_t method_capacity;
    size_t pending_global_proofs;
    size_t pending_parameter_proofs;
};

enum { ORIGIN_VISITING = 1, ORIGIN_UNKNOWN, ORIGIN_UNMANAGED };

static bool
append_assignment(ir_storage_origin_t *analysis, const void *binding,
                  const ir_value_t *source)
{
    if (analysis->assignment_count == analysis->assignment_capacity) {
        size_t capacity = analysis->assignment_capacity
            ? analysis->assignment_capacity * 2 : 32;
        storage_assignment_t *items = realloc(analysis->assignments,
            capacity * sizeof(*items));
        if (!items) return false;
        analysis->assignments = items;
        analysis->assignment_capacity = capacity;
    }
    analysis->assignments[analysis->assignment_count++] =
        (storage_assignment_t){binding, source};
    return true;
}

static bool
append_field_assignment(ir_storage_origin_t *analysis,
                        const ir_instr_t *field, const ir_value_t *source)
{
    if (analysis->field_assignment_count == analysis->field_assignment_capacity) {
        size_t capacity = analysis->field_assignment_capacity
            ? analysis->field_assignment_capacity * 2 : 32;
        storage_field_assignment_t *items = realloc(analysis->field_assignments,
            capacity * sizeof(*items));
        if (!items) return false;
        analysis->field_assignments = items;
        analysis->field_assignment_capacity = capacity;
    }
    analysis->field_assignments[analysis->field_assignment_count++] =
        (storage_field_assignment_t){field, source};
    return true;
}

static bool
append_call(ir_storage_origin_t *analysis, ir_instr_t *call)
{
    if (analysis->call_count == analysis->call_capacity) {
        size_t capacity = analysis->call_capacity
            ? analysis->call_capacity * 2 : 32;
        ir_instr_t **items = realloc(analysis->calls,
            capacity * sizeof(*items));
        if (!items) return false;
        analysis->calls = items;
        analysis->call_capacity = capacity;
    }
    analysis->calls[analysis->call_count++] = call;
    return true;
}

static bool
append_indirect_function(ir_storage_origin_t *analysis,
                         const ir_func_t *function)
{
    if (analysis->indirect_count == analysis->indirect_capacity) {
        size_t capacity = analysis->indirect_capacity
            ? analysis->indirect_capacity * 2 : 16;
        const ir_func_t **items = realloc(analysis->indirect_functions,
            capacity * sizeof(*items));
        if (!items) return false;
        analysis->indirect_functions = items;
        analysis->indirect_capacity = capacity;
    }
    analysis->indirect_functions[analysis->indirect_count++] = function;
    return true;
}

static bool
append_dynamic_method(ir_storage_origin_t *analysis, const char *name)
{
    if (analysis->method_count == analysis->method_capacity) {
        size_t capacity = analysis->method_capacity
            ? analysis->method_capacity * 2 : 16;
        const char **items = realloc(analysis->dynamic_methods,
            capacity * sizeof(*items));
        if (!items) return false;
        analysis->dynamic_methods = items;
        analysis->method_capacity = capacity;
    }
    analysis->dynamic_methods[analysis->method_count++] = name;
    return true;
}

static const ir_global_t *
global_identity(const ir_global_t *global)
{
    const ir_global_t *identity = global
        ? ir_global_identity((ir_global_t *)global) : nullptr;
    return identity ? identity : global;
}

ir_storage_origin_t *
ir_storage_origin_new(ir_func_t *const *reachable, size_t count,
                      ir_mapping_base_source_fn source,
                      ir_unmanaged_parameter_source_fn unmanaged_parameter,
                      ir_closed_callers_fn closed_callers,
                      bool closed_field_writes, void *context)
{
    if ((count && !reachable) || !source) return nullptr;
    ir_storage_origin_t *analysis = calloc(1, sizeof(*analysis));
    if (!analysis) return nullptr;
    analysis->functions = reachable;
    analysis->function_count = count;
    analysis->source = source;
    analysis->unmanaged_parameter = unmanaged_parameter;
    analysis->closed_callers = closed_callers;
    analysis->closed_field_writes = closed_field_writes;
    analysis->context = context;
    for (size_t mode = 0; mode < ORIGIN_MODE_COUNT; mode++) {
        core_dict_init(&analysis->values[mode], nullptr, nullptr);
        core_dict_init(&analysis->locals[mode], nullptr, nullptr);
        core_dict_init(&analysis->globals[mode], nullptr, nullptr);
        core_dict_init(&analysis->returns[mode], nullptr, nullptr);
    }
    for (size_t i = 0; i < count; i++) {
        ir_func_t *function = reachable[i];
        for (uint32_t b = 0; function && b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t j = 0; block && j < block->n_instrs; j++) {
                ir_instr_t *instruction = block->instrs[j];
                if (instruction->dest_kind == IR_DEST_LOCAL
                    && instruction->dest_local
                    && !append_assignment(analysis, instruction->dest_local,
                        instruction->op == IR_OP_COPY
                            && instruction->n_operands == 1
                            ? ir_operand_value(&instruction->operands[0])
                            : nullptr)) goto failed;
                if (instruction->op == IR_OP_GLOBAL_STORE
                    && instruction->n_operands == 2) {
                    ir_value_t *target = ir_operand_value(
                        &instruction->operands[0]);
                    if (target && target->kind == IR_VAL_GLOBAL
                        && !append_assignment(analysis,
                            global_identity(target->global),
                            ir_operand_value(&instruction->operands[1])))
                        goto failed;
                }
                if (instruction->op == IR_OP_CALL
                    && !append_call(analysis, instruction)) goto failed;
                if (instruction->op == IR_OP_TRAIT_METHOD
                    && instruction->n_operands >= 2
                    && instruction->operands[1].kind == IR_OPD_NAME
                    && instruction->operands[1].name
                    && !append_dynamic_method(analysis,
                        instruction->operands[1].name)) goto failed;
                for (uint32_t operand = 0;
                     operand < instruction->n_operands; operand++) {
                    const ir_value_t *value = ir_operand_value(
                        &instruction->operands[operand]);
                    const ir_instr_t *address = value
                        && value->kind == IR_VAL_INSTR ? value->def : nullptr;
                    const ir_value_t *target = address
                        && address->op == IR_OP_ADDRESS_OF
                        && address->n_operands == 1
                            ? ir_operand_value(&address->operands[0]) : nullptr;
                    if (target && target->kind == IR_VAL_GLOBAL
                        && !(instruction->op == IR_OP_LOAD && operand == 0)) {
                        const ir_value_t *source = operand == 0
                            && (instruction->op == IR_OP_STORE
                                || instruction->op == IR_OP_SEQUENCE_SET)
                            ? ir_operand_value(&instruction->operands[
                                instruction->op == IR_OP_STORE ? 1 : 2])
                            : nullptr;
                        if (!append_assignment(analysis,
                                global_identity(target->global), source))
                            goto failed;
                    }
                    if (address && address->op == IR_OP_FIELD
                        && !(instruction->op == IR_OP_LOAD && operand == 0)) {
                        const ir_value_t *source = operand == 0
                            && (instruction->op == IR_OP_STORE
                                || instruction->op == IR_OP_SEQUENCE_SET)
                            ? ir_operand_value(&instruction->operands[
                                instruction->op == IR_OP_STORE ? 1 : 2])
                            : nullptr;
                        if (!append_field_assignment(analysis, address, source))
                            goto failed;
                    }
                    if (value && value->kind == IR_VAL_FUNC
                        && value->owner
                        && (instruction->op != IR_OP_CALL || operand != 0)
                        && !append_indirect_function(analysis,
                            value->owner)) goto failed;
                }
            }
        }
    }
    return analysis;
failed:
    ir_storage_origin_free(analysis);
    return nullptr;
}

void
ir_storage_origin_free(ir_storage_origin_t *analysis)
{
    if (!analysis) return;
    for (size_t mode = 0; mode < ORIGIN_MODE_COUNT; mode++) {
        core_dict_free(&analysis->values[mode]);
        core_dict_free(&analysis->locals[mode]);
        core_dict_free(&analysis->globals[mode]);
        core_dict_free(&analysis->returns[mode]);
    }
    free(analysis->assignments);
    free(analysis->field_assignments);
    free(analysis->field_proofs);
    free(analysis->calls);
    free(analysis->indirect_functions);
    free(analysis->dynamic_methods);
    free(analysis);
}

static ir_func_t *
definition(ir_storage_origin_t *analysis, const ir_func_t *target)
{
    if (!target || !target->owner) return nullptr;
    for (size_t i = 0; i < analysis->function_count; i++) {
        ir_func_t *function = analysis->functions[i];
        if (function && function->n_blocks
            && function->owner && function->name && target->name
            && !strcmp(function->name, target->name)
            && !strcmp(ir_module_name(function->owner),
                       ir_module_name(target->owner))) return function;
    }
    return nullptr;
}

static bool prove_value(ir_storage_origin_t *analysis,
                        const ir_value_t *value, origin_mode_t mode);

static bool
prove_assignments(ir_storage_origin_t *analysis, const void *binding,
                  bool require_assignment, origin_mode_t mode)
{
    bool found = false;
    for (size_t i = 0; i < analysis->assignment_count; i++) {
        const storage_assignment_t *assignment = &analysis->assignments[i];
        if (assignment->binding != binding) continue;
        found = true;
        if (!assignment->source
            || !prove_value(analysis, assignment->source, mode)) return false;
    }
    return found || !require_assignment;
}

static bool
prove_cached(ir_storage_origin_t *analysis, core_dict_t *cache,
             const void *key, bool (*compute)(ir_storage_origin_t *,
                                               const void *, origin_mode_t),
             origin_mode_t mode)
{
    if (!key) return false;
    uintptr_t state = (uintptr_t)core_dict_get(cache, key, nullptr);
    if (state == ORIGIN_VISITING)
        return analysis->pending_global_proofs != 0
            || analysis->pending_parameter_proofs != 0;
    if (state) return state == ORIGIN_UNMANAGED;
    core_dict_put(cache, key, ORIGIN_VISITING);
    bool result = compute(analysis, key, mode);
    if (result && (analysis->pending_global_proofs
                   || analysis->pending_parameter_proofs))
        core_dict_remove(cache, key);
    else core_dict_put(cache, key,
                       result ? ORIGIN_UNMANAGED : ORIGIN_UNKNOWN);
    return result;
}

static bool
prove_local(ir_storage_origin_t *analysis, const void *key,
            origin_mode_t mode)
{
    const ir_local_t *local = key;
    return !local->address_taken && !local->has_init
        && prove_assignments(analysis, key, true, mode);
}

static bool
prove_global(ir_storage_origin_t *analysis, const void *key,
             origin_mode_t mode)
{
    const ir_global_t *global = key;
    if (global->is_extern || (global->has_static_initializer
        && (!global->static_initializer
            || global->static_initializer->kind != IR_GLOBAL_INIT_NIL)))
        return false;
    return prove_assignments(analysis, key, true, mode);
}

static bool
prove_global_value(ir_storage_origin_t *analysis, const ir_global_t *global,
                   origin_mode_t mode)
{
    global = global_identity(global);
    if (!global) return false;
    core_dict_t *cache = &analysis->globals[mode];
    uintptr_t state = (uintptr_t)core_dict_get(cache, global, nullptr);
    if (state == ORIGIN_VISITING)
        return global->has_static_initializer && global->static_initializer
            && global->static_initializer->kind == IR_GLOBAL_INIT_NIL;
    if (state) return state == ORIGIN_UNMANAGED;
    core_dict_put(cache, global, ORIGIN_VISITING);
    analysis->pending_global_proofs++;
    bool result = prove_global(analysis, global, mode);
    analysis->pending_global_proofs--;
    if (result && (analysis->pending_global_proofs
                   || analysis->pending_parameter_proofs))
        core_dict_remove(cache, global);
    else core_dict_put(cache, global,
                       result ? ORIGIN_UNMANAGED : ORIGIN_UNKNOWN);
    return result;
}

static bool
same_field(const ir_instr_t *left, const ir_instr_t *right)
{
    if (!left || !right || left->op != IR_OP_FIELD
        || right->op != IR_OP_FIELD || left->n_operands < 2
        || right->n_operands < 2
        || left->operands[1].kind != IR_OPD_NAME
        || right->operands[1].kind != IR_OPD_NAME
        || !left->operands[1].name || !right->operands[1].name
        || strcmp(left->operands[1].name, right->operands[1].name))
        return false;
    const ir_value_t *left_base = ir_operand_value(&left->operands[0]);
    const ir_value_t *right_base = ir_operand_value(&right->operands[0]);
    th_type *left_type = left_base ? th_ind_referent(left_base->type) : nullptr;
    th_type *right_type = right_base
        ? th_ind_referent(right_base->type) : nullptr;
    return left_type && right_type && left->block && left->block->owner
        && ir_type_eq(left->block->owner->owner, left_type, right_type);
}

static bool
prove_field_content(ir_storage_origin_t *analysis,
                    const ir_instr_t *field, origin_mode_t mode)
{
    if (!analysis->closed_field_writes || mode != ORIGIN_UNMANAGED_OBJECT
        || !field || field->op != IR_OP_FIELD || !field->n_operands
        || !prove_value(analysis, ir_operand_value(&field->operands[0]), mode))
        return false;
    size_t index = 0;
    for (; index < analysis->field_proof_count; index++)
        if (analysis->field_proofs[index].mode == mode
            && same_field(analysis->field_proofs[index].field, field)) break;
    if (index < analysis->field_proof_count) {
        uintptr_t state = analysis->field_proofs[index].state;
        if (state) return state == ORIGIN_UNMANAGED;
    } else {
        if (analysis->field_proof_count == analysis->field_proof_capacity) {
            size_t capacity = analysis->field_proof_capacity
                ? analysis->field_proof_capacity * 2 : 16;
            storage_field_proof_t *items = realloc(analysis->field_proofs,
                capacity * sizeof(*items));
            if (!items) return false;
            analysis->field_proofs = items;
            analysis->field_proof_capacity = capacity;
        }
        analysis->field_proofs[index] = (storage_field_proof_t){field, mode, 0};
        analysis->field_proof_count++;
    }
    analysis->field_proofs[index].state = ORIGIN_VISITING;
    bool found = false, proven = true;
    for (size_t i = 0; i < analysis->field_assignment_count; i++) {
        const storage_field_assignment_t *assignment =
            &analysis->field_assignments[i];
        if (!same_field(field, assignment->field)) continue;
        found = true;
        if (!assignment->source
            || !prove_value(analysis, assignment->source, mode)) {
            proven = false;
            break;
        }
    }
    bool result = found && proven;
    analysis->field_proofs[index].state = result
        && (analysis->pending_global_proofs
            || analysis->pending_parameter_proofs) ? 0
        : result ? ORIGIN_UNMANAGED : ORIGIN_UNKNOWN;
    return result;
}

static bool
prove_return(ir_storage_origin_t *analysis, const void *key,
             origin_mode_t mode)
{
    const ir_func_t *function = key;
    bool found = false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; block && i < block->n_instrs; i++) {
            const ir_instr_t *instruction = block->instrs[i];
            if (instruction->op != IR_OP_RETURN) continue;
            found = true;
            if (!instruction->n_operands
                || !prove_value(analysis,
                    ir_operand_value(&instruction->operands[0]), mode))
                return false;
        }
    }
    return found;
}

static bool
prove_parameter(ir_storage_origin_t *analysis, const ir_value_t *parameter,
                origin_mode_t mode)
{
    if (mode == ORIGIN_UNMANAGED_OBJECT && analysis->unmanaged_parameter
        && analysis->unmanaged_parameter(parameter, analysis->context))
        return true;
    ir_func_t *owner = definition(analysis, parameter->owner);
    if (!owner || (analysis->closed_callers
            ? !analysis->closed_callers(owner, analysis->context)
            : owner->linkage != IR_LINK_INTERNAL))
        return false;
    for (size_t i = 0; i < analysis->indirect_count; i++)
        if (definition(analysis, analysis->indirect_functions[i]) == owner)
            return false;
    for (size_t i = 0; i < analysis->method_count; i++) {
        const char *method = analysis->dynamic_methods[i];
        size_t length = strlen(method);
        if (!strncmp(owner->name, method, length)
            && (owner->name[length] == 0 || owner->name[length] == '#'))
            return false;
    }
    bool found = false;
    analysis->pending_parameter_proofs++;
    for (size_t i = 0; i < analysis->call_count; i++) {
        const ir_instr_t *call = analysis->calls[i];
        const ir_value_t *callee = call->n_operands
            ? ir_operand_value(&call->operands[0]) : nullptr;
        if (!callee || callee->kind != IR_VAL_FUNC
            || definition(analysis, callee->owner) != owner) continue;
        found = true;
        uint32_t argument = parameter->index + 1;
        if (call->n_operands <= argument
            || call->operands[argument].kind != IR_OPD_VALUE
            || !prove_value(analysis,
                ir_operand_value(&call->operands[argument]), mode)) {
            analysis->pending_parameter_proofs--;
            return false;
        }
    }
    analysis->pending_parameter_proofs--;
    return found;
}

static bool
index_is_inline_field(const ir_instr_t *instruction)
{
    if (!instruction || instruction->n_operands < 1) return false;
    const ir_value_t *base = ir_operand_value(&instruction->operands[0]);
    const ir_instr_t *field = base && base->kind == IR_VAL_INSTR
        ? base->def : nullptr;
    if (!field || field->op != IR_OP_FIELD || field->n_operands != 2
        || field->operands[1].kind != IR_OPD_NAME
        || !field->operands[1].name || !field->block
        || !field->block->owner || !field->block->owner->owner)
        return false;
    const ir_value_t *object = ir_operand_value(&field->operands[0]);
    th_type *record = object ? th_ind_referent(object->type) : nullptr;
    ir_module_t *module = field->block->owner->owner;
    const ir_final_layout_t *layout = record
        ? ir_layout_final(module, record, ir_module_target(module)) : nullptr;
    if (!layout) return false;
    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        if (member->name && !strcmp(member->name, field->operands[1].name))
            return member->inline_element_count != 0;
    }
    return false;
}

static bool
prove_instruction(ir_storage_origin_t *analysis, const ir_instr_t *instruction,
                  origin_mode_t mode)
{
    if (!instruction) return false;
    const ir_operand_t *operands = instruction->operands;
    uint32_t count = instruction->n_operands;
    switch (instruction->op) {
    case IR_OP_NIL:
    case IR_OP_OPTION_NONE:
        return true;
    case IR_OP_ADDRESS_OF: {
        const ir_value_t *base = count == 1
            ? ir_operand_value(&operands[0]) : nullptr;
        return mode == ORIGIN_UNMANAGED_OBJECT && base
            && (base->kind == IR_VAL_LOCAL
            || base->kind == IR_VAL_GLOBAL);
    }
    case IR_OP_LOAD: {
        const ir_value_t *address = count == 1
            ? ir_operand_value(&operands[0]) : nullptr;
        const ir_instr_t *definition = address
            && address->kind == IR_VAL_INSTR ? address->def : nullptr;
        if (definition && definition->op == IR_OP_FIELD)
            return prove_field_content(analysis, definition, mode);
        const ir_value_t *global = definition
            && definition->op == IR_OP_ADDRESS_OF
            && definition->n_operands == 1
                ? ir_operand_value(&definition->operands[0]) : nullptr;
        return global && global->kind == IR_VAL_GLOBAL
            && prove_global_value(analysis, global->global, mode);
    }
    case IR_OP_COPY:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
        return count >= 1
            && prove_value(analysis, ir_operand_value(&operands[0]), mode);
    case IR_OP_CAST:
    case IR_OP_NOMINAL_CAST:
        return mode == ORIGIN_UNMANAGED_OBJECT && count == 1
            && prove_value(analysis, ir_operand_value(&operands[0]), mode);
    case IR_OP_BITCAST:
        return count == 1 && (mode == ORIGIN_UNMANAGED_OBJECT
            && (prove_value(analysis, ir_operand_value(&operands[0]),
                            ORIGIN_RAW_MAPPING)
                || prove_value(analysis, ir_operand_value(&operands[0]),
                               ORIGIN_UNMANAGED_OBJECT)));
    case IR_OP_FIELD:
        return mode == ORIGIN_UNMANAGED_OBJECT && count >= 1
            && prove_value(analysis, ir_operand_value(&operands[0]), mode);
    case IR_OP_INDEX:
        return mode == ORIGIN_UNMANAGED_OBJECT
            && index_is_inline_field(instruction)
            && prove_value(analysis, ir_operand_value(&operands[0]), mode);
    case IR_OP_OPTION_GET_OR_ELSE:
        return count == 2
            && prove_value(analysis, ir_operand_value(&operands[0]), mode)
            && prove_value(analysis, ir_operand_value(&operands[1]), mode);
    case IR_OP_SELECT:
        return count == 3
            && prove_value(analysis, ir_operand_value(&operands[1]), mode)
            && prove_value(analysis, ir_operand_value(&operands[2]), mode);
    case IR_OP_PHI:
        if (!count) return false;
        for (uint32_t i = 0; i < count; i++)
            if (!prove_value(analysis, ir_operand_value(&operands[i]), mode))
                return false;
        return true;
    case IR_OP_CALL: {
        const ir_value_t *callee = count
            ? ir_operand_value(&operands[0]) : nullptr;
        ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
            ? definition(analysis, callee->owner) : nullptr;
        return target && prove_cached(analysis, &analysis->returns[mode],
                                      target, prove_return, mode);
    }
    default:
        return false;
    }
}

static bool
compute_value(ir_storage_origin_t *analysis, const void *key,
              origin_mode_t mode)
{
    const ir_value_t *value = key;
    if (mode == ORIGIN_RAW_MAPPING
        && analysis->source(value, analysis->context)) return true;
    switch (value->kind) {
    case IR_VAL_LOCAL:
        return prove_cached(analysis, &analysis->locals[mode], value->local,
                            prove_local, mode);
    case IR_VAL_GLOBAL:
        return prove_global_value(analysis, value->global, mode);
    case IR_VAL_PARAM:
        return prove_parameter(analysis, value, mode);
    case IR_VAL_INSTR:
        return prove_instruction(analysis, value->def, mode);
    default:
        return false;
    }
}

static bool
prove_value(ir_storage_origin_t *analysis, const ir_value_t *value,
            origin_mode_t mode)
{
    return prove_cached(analysis, &analysis->values[mode], value,
                        compute_value, mode);
}

bool
ir_storage_origin_unmanaged(ir_storage_origin_t *analysis,
                             const ir_value_t *address)
{
    return analysis && address
        && prove_value(analysis, address, ORIGIN_UNMANAGED_OBJECT);
}
