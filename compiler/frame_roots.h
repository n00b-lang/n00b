#pragma once

#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/layout.h"
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
n00bb_frame_roots_filtered(ir_func_t *function, bool pointers_only,
                           n00bb_frame_root_visit_fn visit, void *context,
                           size_t *count)
{
    if (!function || !function->owner || !count) return false;
    size_t next = 0;
    for (uint32_t i = 0; i < function->n_params; i++) {
        ir_value_t *value = function->params[i];
        if (!value || !value->type) return false;
        if (!n00bb_frame_native_pointer(function->owner, value->type)
            && (pointers_only
                || !n00bb_frame_traced_value(function, value->type))) continue;
        if (visit && !visit(context, N00BB_FRAME_ROOT_PARAM, next,
                            value, nullptr, nullptr)) return false;
        next++;
    }
    for (uint32_t i = 0; i < function->n_locals; i++) {
        ir_local_t *local = function->locals[i];
        if (!local || !local->type) return false;
        if (!n00bb_frame_native_pointer(function->owner, local->type)
            && (pointers_only
                || !n00bb_frame_traced_value(function, local->type))) continue;
        if (visit && !visit(context, N00BB_FRAME_ROOT_LOCAL, next,
                            nullptr, local, nullptr)) return false;
        next++;
    }
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        if (!block) return false;
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (!instruction) return false;
            ir_value_t *value = instruction->dest_kind == IR_DEST_VALUE
                ? instruction->dest : nullptr;
            if (!value) continue;
            if (!value->type) return false;
            if (!n00bb_frame_native_pointer(function->owner, value->type)
                && (pointers_only
                    || !n00bb_frame_traced_value(function, value->type)))
                continue;
            if (visit && !visit(context, N00BB_FRAME_ROOT_INSTR, next,
                                value, nullptr, instruction)) return false;
            next++;
            th_type *stored = n00bb_frame_root_type(
                N00BB_FRAME_ROOT_STORAGE, nullptr, nullptr, instruction);
            bool stack = instruction->op == IR_OP_ALLOCA
                || (instruction->op == IR_OP_ALLOC
                    && ir_allocation_has_only_local_uses(function,
                                                          instruction));
            if (stack && stored
                && (n00bb_frame_native_pointer(function->owner, stored)
                    || (!pointers_only
                        && n00bb_frame_traced_value(function, stored)))) {
                if (visit && !visit(context, N00BB_FRAME_ROOT_STORAGE, next,
                                    nullptr, nullptr, instruction))
                    return false;
                next++;
            }
        }
    }
    *count = next;
    return true;
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
        const char *module = ir_module_name(context->function->owner);
        const char *function = context->function->name;
        if (!module || !function || !context->function->type) return false;
        uint64_t module_hash = core_hash_raw(module, strlen(module));
        uint64_t function_hash = core_hash_raw(function, strlen(function));
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
        .name = context->names[index],
        .type = n00bb_frame_native_pointer(context->function->owner, type)
            ? th_name(ir_module_types(context->function->owner), "u64")
            : th_kind_of(th_resolve(type)) == TH_VAR
                || th_kind_of(th_resolve(type)) == TH_TRAIT
                ? th_name(ir_module_types(context->function->owner), "Any")
                : type,
    };
    return context->members[index].type != nullptr;
}

static inline th_type *
n00bb_frame_payload_type(ir_func_t *function, size_t *root_count)
{
    size_t count = 0;
    if (!n00bb_frame_roots(function, nullptr, nullptr, &count)
        || !root_count) return nullptr;
    *root_count = count;
    if (!count || count > SIZE_MAX / sizeof(th_member)) return nullptr;
    th_member *members = calloc(count, sizeof(*members));
    char (*names)[96] = calloc(count, sizeof(*names));
    if (!members || !names) {
        free(members);
        free(names);
        return nullptr;
    }
    n00bb_frame_member_context_t context = {function, members, names};
    size_t filled = 0;
    bool valid = n00bb_frame_roots(function, n00bb_frame_member,
                                   &context, &filled) && filled == count;
    th_type *payload = valid ? th_record(ir_module_types(function->owner),
                                          members, count) : nullptr;
    free(members);
    free(names);
    return payload;
}
