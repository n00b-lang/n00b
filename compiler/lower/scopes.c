#include "lower_internal.h"

static bool
n00bb_binding_in_loop_scope(parse_sym_entry_t *symbol,
                            parse_tree_t *loop)
{
    for (parse_scope_t *scope = symbol ? symbol->scope : nullptr;
         scope; scope = scope->parent)
        if (scope->node == loop) return true;
    return false;
}

static bool
n00bb_mark_binding_reentry(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    if (!context || !lower || !symbol || !symbol->scope) return false;

    for (size_t i = lower->loops.len; i > 0; i--) {
        n00bb_loop_frame_t *frame = &lower->loops.data[i - 1];
        if (!frame->active || !frame->node || !frame->header) continue;
        if (!n00bb_binding_in_loop_scope(symbol, frame->node)) continue;

        ir_value_t *binding = ir_lower_bound_address(context, symbol);
        if (!binding) {
            ir_local_t *local = ir_lower_bound_local(context, symbol);
            binding = local ? ir_local_value(context->function, local)
                            : nullptr;
        }
        return binding && ir_value_set_binding_reentry(binding,
                                                       frame->header);
    }
    return true;
}

ir_mutability_t
local_binding_mutability(const parse_sym_entry_t *symbol)
{
    return symbol->kind == PARSE_SYM_VARIABLE
        && symbol->mutability == PARSE_SYM_IMMUTABLE
        ? IR_MUT_IMMUTABLE : IR_MUT_MUTABLE;
}

bool
n00bb_set_flag(ir_lower_context_t *context, ir_local_t *flag, bool value,
               ir_span_t span)
{
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    ir_instr_t *constant = bool_type
        ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
    if (!constant || !ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_"), bool_type)
        || !ir_instr_operand_int(constant, value ? 1 : 0)) return false;
    ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    return copy && ir_instr_set_dest_local(copy, flag)
        && ir_instr_operand_value(copy, constant->dest);
}

static ir_value_t *
n00bb_storage_address(ir_lower_context_t *context, ir_local_t *storage,
                      ir_span_t span)
{
    th_type *pointer_type = th_ind(ir_module_types(context->module), 0,
                                   storage->type);
    ir_instr_t *take = ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF,
                                                 span);
    return take && ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_finalizing_"), pointer_type)
        && ir_instr_operand_local(take, storage) ? take->dest : nullptr;
}

ir_value_t *
n00bb_emit_temp(ir_lower_context_t *context, ir_op_t op, th_type *type,
                ir_value_t *operand, ir_span_t span)
{
    ir_instr_t *in = ir_lower_emit_instruction(context, op, span);
    return in && ir_lower_set_temp_dest(context, in,
            CORE_STRING_STATIC("__n00bb_"), type)
        && ir_instr_operand_value(in, operand) ? in->dest : nullptr;
}

static bool
n00bb_emit_finalizer_call(ir_lower_context_t *context,
                          const th_env_method_t *method,
                          ir_value_t *address, ir_span_t span)
{
    ir_module_t *owner = nullptr;
    ir_func_t *function = n00bb_record_field_method(context, method, &owner);
    ir_value_t *callee = owner && function
        ? ir_func_address(owner, function) : nullptr;
    th_type *signature = callee ? method->signature : nullptr;
    th_type *nil_type = th_name(ir_module_types(context->module),
                                CORE_STRING_STATIC("nil"));
    ir_value_t *consumed = signature
        ? n00bb_emit_consume_value(context, address, nullptr, span)
        : nullptr;
    ir_instr_t *call = consumed
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    return call && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_finalized_"), nil_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, consumed)
        && ir_instr_set_call_signature(call, signature);
}

bool
n00bb_emit_local_finalizer(ir_lower_context_t *context, ir_local_t *handle,
                           const th_env_method_t *method, ir_span_t span)
{
    ir_value_t *address = n00bb_storage_address(context, handle, span);
    return address && n00bb_emit_finalizer_call(context, method, address,
                                                span);
}

static bool
n00bb_mark_open_storage(ir_lower_context_t *context, ir_instr_t *allocation,
                        ir_local_t *local, core_string_t storage_class)
{
    if ((storage_class.data && storage_class.u8_bytes)
        || !n00bb_open_type(context, local->type)) return true;
    n00bb_lower_context_t *lower = context->user;
    if (!lower->unit) return true;
    core_dict_put(&tu_state(lower->unit)->open_storage, allocation, local);
    n00bb_rewrite_later(context, allocation);
    return true;
}

bool
n00bb_bind_local(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                 ir_local_t *local, ir_span_t span)
{
    if (!context || !symbol || !local) return false;
    if (!ir_span_known(local->span))
        local->span = lower_symbol_name_span(context, symbol);
    core_string_t storage_class = symbol->selected_storage_class;
    bool dynamic = symbol->storage_duration == PARSE_SYM_STORAGE_DYNAMIC;
    if ((!storage_class.data || !storage_class.u8_bytes) && !dynamic) {
        return ir_lower_bind_local(context, symbol, local)
            && n00bb_mark_binding_reentry(context, symbol);
    }
    parse_sym_set_storage_duration(symbol, PARSE_SYM_STORAGE_DYNAMIC);
    th_type *address_type = th_ind(ir_module_types(context->module), 0,
                                   local->type);
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    return allocation
        && ir_lower_set_temp_dest(context, allocation, CORE_STRING_STATIC("__n00bb_storage_"),
                                  address_type)
        && ir_instr_operand_type(allocation, local->type)
        && ((!storage_class.data || !storage_class.u8_bytes)
            || ir_instr_set_storage_class(allocation, storage_class,
                                          symbol->storage_class_required))
        && ir_lower_bind_local_storage(context, symbol, local,
                                       allocation->dest)
        // Storage for a `let` or `const` keeps saying so, since writes reach
        // it through its address rather than the local.
        && (local->mutability == IR_MUT_MUTABLE
            || ir_annot_set(ir_module_ctx(context->module), &allocation->annots,
                            (ir_annot_t){.key = N00BB_IMMUTABLE_STORAGE_KEY,
                                         .kind = IR_ANNOT_BOOL, .b = true}))
        && n00bb_mark_open_storage(context, allocation, local, storage_class)
        && n00bb_mark_binding_reentry(context, symbol);
}

// One object a function makes whose type has a finalizer: the handle is kept
// in `handle`, and `owned` says the object was made on this path.
typedef struct {
    ir_instr_t *cast;
    ir_local_t *handle;
    ir_local_t *owned;
    th_env_method_t method;
} n00bb_frame_object_t;

core_list_decl(n00bb_frame_object_t);

// The object made by `in` (the conversion of a fresh `__fields` allocation
// to its object type), when that type is closed and has a finalizer.
static ir_instr_t *
finalizable_object(core_translation_unit_t *unit, ir_instr_t *in,
                   th_env_method_t *method)
{
    ir_value_t *fields = in->op == IR_OP_NOMINAL_CAST && in->dest
        && in->n_operands ? ir_operand_value(&in->operands[0]) : nullptr;
    ir_instr_t *allocation = fields && fields->kind == IR_VAL_INSTR
        ? fields->def : nullptr;
    th_ctx *types = tu_state(unit)->types;
    th_type *object = in->dest ? th_resolve(in->dest->type) : nullptr;
    if (!allocation || allocation->op != IR_OP_ALLOC
        || !ir_instr_is_fresh(allocation) || !object
        || !th_is_closed(types, object) || !n00bb_object_type(types, object))
        return nullptr;
    return n00bb_type_finalizer(unit, object, method) ? allocation : nullptr;
}

static ir_block_t *
split_named(ir_lower_context_t *context, ir_block_t *block, uint32_t at)
{
    n00bb_lower_context_t *lower = context->user;
    char buffer[64];
    for (;;) {
        core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
            "__n00bb_frame_object_%llu",
            (unsigned long long)lower->next_block_id++);
        if (!ir_block_find(context->function, name))
            return ir_block_split(block, at, name);
    }
}

static ir_local_t *
frame_object_local(ir_lower_context_t *context, const char *role,
                   th_type *type)
{
    char buffer[64];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        "__n00bb_%s_%llu", role,
        (unsigned long long)context->next_value_id++);
    return ir_local_add(context->function, name, type);
}

// Keeps the handle of `object->cast` and marks the object made, right after
// the conversion; storage placement clears the mark for an allocation it
// moves to the heap (`n00bb.finalize_owned`), where the collector finalizes
// the object instead.
static bool
track_frame_object(ir_lower_context_t *context, n00bb_frame_object_t *object,
                   ir_instr_t *allocation)
{
    n00bb_lower_context_t *lower = context->user;
    ir_instr_t *cast = object->cast;
    th_type *truth = n00bb_ir_builtin_type(context->module,
                                           CORE_STRING_STATIC("bool"));
    object->handle = frame_object_local(context, "frame_object",
                                        cast->dest->type);
    object->owned = truth
        ? frame_object_local(context, "frame_object_made", truth) : nullptr;
    ir_block_t *block = cast->block;
    ir_block_t *tail = object->handle && object->owned
        ? split_named(context, block, cast->index + 1) : nullptr;
    if (!tail) return false;
    context->block = block;
    ir_instr_t *keep = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                 cast->span);
    return keep && ir_instr_set_dest_local(keep, object->handle)
        && ir_instr_operand_value(keep, cast->dest)
        && n00bb_set_flag(context, object->owned, true, cast->span)
        && n00bb_emit_jump(context, tail, cast->span)
        && ir_annot_set(lower->ir_context, &allocation->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC("n00bb.finalize_owned"),
               .kind = IR_ANNOT_VALUE,
               .v = ir_local_value(context->function, object->owned),
           });
}

// Finalizes, before `exit` (a `return` or `fail`), every object made on the
// path to it, latest first.
static bool
finalize_before_exit(ir_lower_context_t *context, ir_instr_t *exit,
                     const core_list_t(n00bb_frame_object_t) *objects)
{
    n00bb_lower_context_t *lower = context->user;
    ir_block_t *block = exit->block;
    ir_span_t span = exit->span;
    ir_block_t *tail = split_named(context, block, exit->index);
    if (!tail) return false;
    context->block = block;
    for (size_t i = objects->len; i > 0; i--) {
        const n00bb_frame_object_t *object = &objects->data[i - 1];
        ir_block_t *finalize = n00bb_branch_block(context, lower, "finalize");
        ir_block_t *next = n00bb_branch_block(context, lower, "finalized");
        if (!finalize || !next
            || !n00bb_emit_branch(context,
                   ir_local_value(context->function, object->owned),
                   finalize, next, span))
            return false;
        context->block = finalize;
        if (!n00bb_emit_local_finalizer(context, object->handle,
                                        &object->method, span)
            || !n00bb_emit_jump(context, next, span))
            return false;
        context->block = next;
    }
    return n00bb_emit_jump(context, tail, span);
}

bool
n00bb_finalize_frame_objects(ir_lower_context_t *context)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    ir_func_t *function = context ? context->function : nullptr;
    if (!lower || !lower->unit || !function || !function->n_blocks)
        return true;
    core_list_t(n00bb_frame_object_t) objects =
        core_list_new(n00bb_frame_object_t);
    core_list_t(n00bb_ir_instr_ptr_t) allocations =
        core_list_new(n00bb_ir_instr_ptr_t);
    core_list_t(n00bb_ir_instr_ptr_t) exits =
        core_list_new(n00bb_ir_instr_ptr_t);
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            n00bb_frame_object_t object = {.cast = in};
            ir_instr_t *allocation = finalizable_object(lower->unit, in,
                                                        &object.method);
            if (allocation) {
                core_list_push(objects, object);
                core_list_push(allocations, allocation);
            }
            if (in->op == IR_OP_RETURN || in->op == IR_OP_FAIL)
                core_list_push(exits, in);
        }
    bool ok = true;
    for (size_t i = 0; ok && i < objects.len; i++)
        ok = track_frame_object(context, &objects.data[i],
                                allocations.data[i]);
    // Every path starts with nothing made.
    ir_block_t *entry = ok && objects.len ? function->blocks[0] : nullptr;
    ir_block_t *body = entry ? split_named(context, entry, 0) : nullptr;
    if (entry) {
        context->block = entry;
        ok = body != nullptr;
        for (size_t i = 0; ok && i < objects.len; i++)
            ok = n00bb_set_flag(context, objects.data[i].owned, false,
                                function->span);
        ok = ok && n00bb_emit_jump(context, body, function->span);
    }
    for (size_t i = 0; ok && objects.len && i < exits.len; i++)
        ok = finalize_before_exit(context, exits.data[i], &objects);
    core_list_free(exits);
    core_list_free(allocations);
    core_list_free(objects);
    return ok;
}
