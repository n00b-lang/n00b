#pragma once

#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/layout.h"
#include "ir/liveness.h"
#include "lib/alloc.h"
#include "ir/typelang.h"
#include "lib/hash.h"
#include "th/typehash.h"

typedef enum {
    N00BB_FRAME_ROOT_PARAM,
    N00BB_FRAME_ROOT_LOCAL,
    N00BB_FRAME_ROOT_INSTR,
    N00BB_FRAME_ROOT_STORAGE,
} n00bb_frame_root_kind_t;

typedef bool (*n00bb_frame_root_visit_fn)(void *context,
    n00bb_frame_root_kind_t kind, size_t index, ir_value_t *value,
    ir_local_t *local, ir_instr_t *instruction);

static inline bool
n00bb_frame_pointer_option(th_type *type)
{
    type = th_builtin_applied(type, TH_BUILTIN_OPTION, 1);
    if (!type) return false;
    const th_arg *element = th_named_arg(type, 0);
    return element && element->kind == TH_ARG_TYPE
        && th_kind_of(th_resolve(element->type)) == TH_INDIRECTION;
}

static inline bool
n00bb_frame_native_pointer(ir_module_t *module, th_type *type)
{
    return ir_machine_of(module, type).class == IR_CLASS_PTR
        || n00bb_frame_pointer_option(type)
        || th_kind_of(th_resolve(type)) == TH_SEQUENCE;
}

static inline th_type *
n00bb_frame_root_type(n00bb_frame_root_kind_t kind, ir_value_t *value,
                      ir_local_t *local, ir_instr_t *instruction)
{
    if (kind == N00BB_FRAME_ROOT_STORAGE)
        return instruction && instruction->n_operands == 1
            && instruction->operands[0].kind == IR_OPD_TYPE
                ? instruction->operands[0].type : nullptr;
    return value ? value->type : local ? local->type : nullptr;
}

static inline bool
n00bb_frame_pointer_free_value(ir_module_t *module, th_type *type)
{
    type = th_resolve(type);
    ir_class_t machine = ir_machine_of(module, type).class;
    if (machine == IR_CLASS_BOOL || machine == IR_CLASS_SINT
        || machine == IR_CLASS_UINT || machine == IR_CLASS_FLOAT)
        return true;
    th_type *option = th_builtin_applied(type, TH_BUILTIN_OPTION, 1);
    if (option) {
        const th_arg *element = th_named_arg(option, 0);
        return element && element->kind == TH_ARG_TYPE
            && n00bb_frame_pointer_free_value(module, element->type);
    }
    th_type *definition = ir_type_definition(module, type);
    if (!definition) definition = type;
    definition = th_resolve(definition);
    if (th_kind_of(definition) != TH_RECORD) return false;
    for (size_t i = 0; i < th_record_len(definition); i++) {
        const th_member *member = th_record_member(definition, i);
        if (!member || !n00bb_frame_pointer_free_value(module, member->type))
            return false;
    }
    return true;
}

static inline bool
n00bb_frame_traced_value(ir_func_t *function, th_type *type)
{
    if (n00bb_frame_pointer_free_value(function->owner, type)) return false;
    th_kind kind = th_kind_of(th_resolve(type));
    if (kind == TH_VAR || kind == TH_TRAIT) return true;
    ir_class_t category = ir_machine_of(function->owner, type).class;
    if (category != IR_CLASS_AGGREGATE && category != IR_CLASS_OPAQUE)
        return false;
    th_type *definition = ir_type_definition(function->owner, type);
    if (definition && th_kind_of(definition) == TH_BITINT) return false;
    return th_is_closed(ir_module_types(function->owner), type);
}

static inline bool
n00bb_frame_call_point(void *context, const ir_instr_t *instruction)
{
    (void)context;
    switch (instruction->op) {
    case IR_OP_CONST: case IR_OP_ENUM_CONST: case IR_OP_ENUM_VALUE:
    case IR_OP_ENUM_FROM_VALUE: case IR_OP_BITSET_CONST:
    case IR_OP_BITSET_UNION: case IR_OP_BITSET_DIFFERENCE:
    case IR_OP_BITSET_INTERSECTION: case IR_OP_BITSET_XOR:
    case IR_OP_BITSET_COMPLEMENT: case IR_OP_BITSET_CONTAINS:
    case IR_OP_BITSET_IS_EMPTY: case IR_OP_BITSET_VALUE:
    case IR_OP_BITSET_FROM_VALUE: case IR_OP_PARAM_PRESENT: case IR_OP_UNDEF:
    case IR_OP_COPY: case IR_OP_ADDRESS_OF: case IR_OP_ALLOCA: case IR_OP_LOAD:
    case IR_OP_FIELD: case IR_OP_INDEX: case IR_OP_SEQUENCE_LENGTH:
    case IR_OP_OFFSET: case IR_OP_DIFF: case IR_OP_EXTRACT:
    case IR_OP_EXTRACT_INDEX: case IR_OP_INSERT: case IR_OP_VARIANT_MAKE:
    case IR_OP_VARIANT_TAG: case IR_OP_VARIANT_PAYLOAD:
    case IR_OP_OPTION_IS_NIL: case IR_OP_NIL: case IR_OP_OPTION_NONE:
    case IR_OP_OPTION_SOME: case IR_OP_OPTION_GET: case IR_OP_ADD:
    case IR_OP_SUB: case IR_OP_MUL: case IR_OP_DIV: case IR_OP_REM:
    case IR_OP_NEG: case IR_OP_AND: case IR_OP_OR: case IR_OP_XOR:
    case IR_OP_NOT: case IR_OP_SHL: case IR_OP_SHR: case IR_OP_ROL:
    case IR_OP_ROR: case IR_OP_CLZ: case IR_OP_CTZ: case IR_OP_POPCOUNT:
    case IR_OP_BITREVERSE: case IR_OP_BYTESWAP: case IR_OP_EQ: case IR_OP_NE:
    case IR_OP_LT: case IR_OP_LE: case IR_OP_GT: case IR_OP_GE:
    case IR_OP_CAST: case IR_OP_BITCAST: case IR_OP_NOMINAL_CAST:
    case IR_OP_SELECT: case IR_OP_PHI: case IR_OP_SIZEOF: case IR_OP_ALIGNOF:
    case IR_OP_OFFSETOF: case IR_OP_TYPEHASH: case IR_OP_ARMTAG:
    case IR_OP_TYPE_INDEX: case IR_OP_JUMP: case IR_OP_BRANCH:
    case IR_OP_SWITCH: case IR_OP_RETURN: case IR_OP_FAIL: case IR_OP_TRAP:
    case IR_OP_UNREACHABLE:
        return false;
    default:
        return true;
    }
}

typedef enum : uint8_t {
    N00BB_FRAME_TYPE_NONE,
    N00BB_FRAME_TYPE_TRACED,
    N00BB_FRAME_TYPE_POINTER,
} n00bb_frame_type_class_t;

static inline n00bb_frame_type_class_t
n00bb_frame_type_class(ir_func_t *function, th_type *type)
{
    if (n00bb_frame_native_pointer(function->owner, type))
        return N00BB_FRAME_TYPE_POINTER;
    return n00bb_frame_traced_value(function, type) ? N00BB_FRAME_TYPE_TRACED
                                                   : N00BB_FRAME_TYPE_NONE;
}

static inline bool
n00bb_frame_values_across(ir_func_t *function, ir_value_t **values,
                          size_t count, bool *across)
{
    if (!ir_live_across_points(ir_func_liveness(function), values, count,
                               n00bb_frame_call_point, nullptr, across))
        for (size_t v = 0; v < count; v++) across[v] = true;
    return true;
}

typedef struct {
    n00bb_frame_root_kind_t kind;
    bool                    pointer;
    ir_value_t             *value;
    ir_local_t             *local;
    ir_instr_t             *instruction;
} n00bb_frame_root_t;

typedef struct {
    n00bb_frame_root_t *roots;
    size_t              count;
    size_t              pointer_count;
} n00bb_frame_root_list_t;

static inline void
n00bb_frame_root_list_release(n00bb_frame_root_list_t *list)
{
    core_free(list->roots);
    *list = (n00bb_frame_root_list_t){0};
}

static inline void
n00bb_frame_root_push(n00bb_frame_root_list_t *list,
                      n00bb_frame_type_class_t class,
                      n00bb_frame_root_t root)
{
    root.pointer = class == N00BB_FRAME_TYPE_POINTER;
    list->pointer_count += root.pointer;
    list->roots[list->count++] = root;
}

static inline bool
n00bb_frame_root_list(ir_func_t *function, n00bb_frame_root_list_t *list)
{
    *list = (n00bb_frame_root_list_t){0};
    if (!function || !function->owner) return false;
    size_t instructions = 0, candidates = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        if (!block) return false;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!instruction) return false;
            ir_value_t *value = instruction->dest_kind == IR_DEST_VALUE
                ? instruction->dest : nullptr;
            if (value && !value->type) return false;
            instructions++;
            candidates += value && n00bb_frame_type_class(function,
                                                          value->type);
        }
    }
    for (uint32_t i = 0; i < function->n_params; i++)
        if (!function->params[i] || !function->params[i]->type) return false;
    for (uint32_t i = 0; i < function->n_locals; i++)
        if (!function->locals[i] || !function->locals[i]->type) return false;
    ir_value_t **values = core_alloc_array(ir_value_t *,
                                           candidates ? candidates : 1);
    bool *across = core_alloc_array(bool, candidates ? candidates : 1);
    list->roots = core_alloc_array(n00bb_frame_root_t,
        function->n_params + function->n_locals + 2 * instructions + 1);
    size_t next = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *instruction = function->blocks[b]->instrs[i];
            ir_value_t *value = instruction->dest_kind == IR_DEST_VALUE
                ? instruction->dest : nullptr;
            if (value && n00bb_frame_type_class(function, value->type))
                values[next++] = value;
        }
    n00bb_frame_values_across(function, values, candidates, across);
    for (uint32_t i = 0; i < function->n_params; i++) {
        ir_value_t *value = function->params[i];
        n00bb_frame_type_class_t class = n00bb_frame_type_class(function,
                                                                value->type);
        if (class) n00bb_frame_root_push(list, class, (n00bb_frame_root_t){
            .kind = N00BB_FRAME_ROOT_PARAM, .value = value});
    }
    for (uint32_t i = 0; i < function->n_locals; i++) {
        ir_local_t *local = function->locals[i];
        n00bb_frame_type_class_t class = n00bb_frame_type_class(function,
                                                                local->type);
        if (class) n00bb_frame_root_push(list, class, (n00bb_frame_root_t){
            .kind = N00BB_FRAME_ROOT_LOCAL, .local = local});
    }
    next = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *instruction = function->blocks[b]->instrs[i];
            ir_value_t *value = instruction->dest_kind == IR_DEST_VALUE
                ? instruction->dest : nullptr;
            n00bb_frame_type_class_t class = value
                ? n00bb_frame_type_class(function, value->type)
                : N00BB_FRAME_TYPE_NONE;
            if (!class) continue;
            if (across[next++])
                n00bb_frame_root_push(list, class, (n00bb_frame_root_t){
                    .kind = N00BB_FRAME_ROOT_INSTR, .value = value,
                    .instruction = instruction});
            th_type *stored = n00bb_frame_root_type(
                N00BB_FRAME_ROOT_STORAGE, nullptr, nullptr, instruction);
            bool stack = instruction->op == IR_OP_ALLOCA
                || (instruction->op == IR_OP_ALLOC
                    && ir_allocation_has_only_local_uses(function,
                                                          instruction));
            n00bb_frame_type_class_t storage = stack && stored
                ? n00bb_frame_type_class(function, stored)
                : N00BB_FRAME_TYPE_NONE;
            if (storage)
                n00bb_frame_root_push(list, storage, (n00bb_frame_root_t){
                    .kind = N00BB_FRAME_ROOT_STORAGE,
                    .instruction = instruction});
        }
    core_free(values);
    core_free(across);
    return true;
}

static inline bool
n00bb_frame_root_list_visit(const n00bb_frame_root_list_t *list,
                            bool pointers_only,
                            n00bb_frame_root_visit_fn visit, void *context)
{
    size_t index = 0;
    for (size_t r = 0; r < list->count; r++) {
        const n00bb_frame_root_t *root = &list->roots[r];
        if (pointers_only && !root->pointer) continue;
        if (!visit(context, root->kind, index++, root->value, root->local,
                   root->instruction)) return false;
    }
    return true;
}

static inline bool
n00bb_frame_roots_filtered(ir_func_t *function, bool pointers_only,
                           n00bb_frame_root_visit_fn visit, void *context,
                           size_t *count)
{
    n00bb_frame_root_list_t list;
    if (!count || !n00bb_frame_root_list(function, &list)) return false;
    *count = pointers_only ? list.pointer_count : list.count;
    bool ok = !visit
        || n00bb_frame_root_list_visit(&list, pointers_only, visit, context);
    n00bb_frame_root_list_release(&list);
    return ok;
}

static inline bool
n00bb_frame_pointer_roots(ir_func_t *function,
                          n00bb_frame_root_visit_fn visit, void *context,
                          size_t *count)
{
    return n00bb_frame_roots_filtered(function, true, visit, context, count);
}

static inline bool
n00bb_frame_roots(ir_func_t *function, n00bb_frame_root_visit_fn visit,
                  void *context, size_t *count)
{
    return n00bb_frame_roots_filtered(function, false, visit, context, count);
}

typedef struct {
    ir_func_t *function;
    th_member *members;
    char (*names)[96];
} n00bb_frame_member_context_t;

static inline bool
n00bb_frame_member(void *opaque, n00bb_frame_root_kind_t kind, size_t index,
                   ir_value_t *value, ir_local_t *local,
                   ir_instr_t *instruction)
{
    (void)kind;
    (void)instruction;
    n00bb_frame_member_context_t *context = opaque;
    th_type *type = n00bb_frame_root_type(kind, value, local, instruction);
    if (!type) return false;
    int length;
    if (index == 0) {
        core_string_t module = ir_module_name(context->function->owner);
        core_string_t function = context->function->name;
        if (!module.data || !function.data || !context->function->type)
            return false;
        uint64_t module_hash = core_hash_raw(module.data, module.u8_bytes);
        uint64_t function_hash = core_hash_raw(function.data,
                                               function.u8_bytes);
        uint64_t signature_hash = th_hash(ir_module_types(
            context->function->owner), context->function->type);
        length = snprintf(context->names[index], 96,
            "frame_%016" PRIx64 "_%016" PRIx64 "_%016" PRIx64 "_root_0",
            module_hash, function_hash, signature_hash);
    } else {
        length = snprintf(context->names[index], 96, "root_%zu", index);
    }
    if (length < 0 || length >= 96)
        return false;
    context->members[index] = (th_member){
        .name = core_string_borrow(context->names[index], (size_t)length),
        .type = n00bb_frame_native_pointer(context->function->owner, type)
            ? th_name(ir_module_types(context->function->owner),
                      CORE_STRING_STATIC("u64"))
            : th_kind_of(th_resolve(type)) == TH_VAR
                || th_kind_of(th_resolve(type)) == TH_TRAIT
                ? th_name(ir_module_types(context->function->owner),
                          CORE_STRING_STATIC("Any"))
                : type,
    };
    return context->members[index].type != nullptr;
}

static inline th_type *
n00bb_frame_payload_type(ir_func_t *function,
                         const n00bb_frame_root_list_t *roots)
{
    size_t count = roots->count;
    if (!count || count > SIZE_MAX / sizeof(th_member)) return nullptr;
    th_member *members = calloc(count, sizeof(*members));
    char (*names)[96] = calloc(count, sizeof(*names));
    if (!members || !names) {
        free(members);
        free(names);
        return nullptr;
    }
    n00bb_frame_member_context_t context = {function, members, names};
    bool valid = n00bb_frame_root_list_visit(roots, false,
                                             n00bb_frame_member, &context);
    th_type *payload = valid ? th_record(ir_module_types(function->owner),
                                          members, count) : nullptr;
    free(members);
    free(names);
    return payload;
}
