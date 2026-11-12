#include "lower_internal.h"

static ir_value_t *
n00bb_intrinsic_placeholder(ir_lower_context_t *context,
                            parse_sym_entry_t *symbol, th_type *callable,
                            ir_value_t *const *values, size_t count,
                            th_type *result_type, ir_span_t span);
static ir_lower_result_t
n00bb_lower_overloaded_call(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *call_node,
                            parse_tree_t *arguments, core_string_t name,
                            th_type *result_type, ir_span_t span);
static ir_lower_result_t
n00bb_lower_dispatch_call(ir_lower_context_t *context, parse_visit_t *visit,
                          parse_tree_t *call_node,
                          parse_tree_t *receiver_node, parse_tree_t *arguments,
                          bool index_call, core_string_t method,
                          th_type *result_type, ir_span_t span);
static void
n00bb_constructor_assignment_target(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    th_type *result_type,
                                    parse_tree_t **assignment,
                                    ir_value_t **address);
static ir_lower_result_t
n00bb_lower_dispatch_new(ir_lower_context_t *context, parse_visit_t *visit,
                         th_type *result_type, ir_span_t span);

static bool
n00bb_emit_copyout_write(ir_lower_context_t *context,
                         const n00bb_copyout_t *copyout,
                         ir_value_t *value, ir_span_t span)
{
    if (!n00bb_emit_sequence_shape_check(context, value,
            copyout->required_shape, span)) return false;
    if (copyout->index_target)
        return n00bb_lower_index_store_at(context, copyout->grammar,
            copyout->site_node, copyout->index_target,
            copyout->source_node, value);
    if (copyout->local)
        return n00bb_emit_local_write(context, copyout->local, value, span);
    if (copyout->address)
        return n00bb_emit_indirect_write(context, copyout->address, value,
                                         span);
    ir_shape_t *target_shape = ir_value_shape(copyout->global);
    if (!target_shape && copyout->symbol)
        target_shape = parse_sym_binding(copyout->symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (target_shape != copyout->required_shape
        && !n00bb_emit_sequence_shape_check(context, value, target_shape,
                                             span)) return false;
    if (!copyout->global || copyout->global->kind != IR_VAL_GLOBAL
        || !copyout->global->global) return false;
    ir_instr_t *store = ir_lower_emit_instruction(context, IR_OP_GLOBAL_STORE,
                                                  span);
    if (!store || !ir_instr_set_binding_initialization(store, true)
        || !ir_instr_operand_value(store, copyout->global)
        || !ir_instr_operand_value(store, value)) return false;
    return true;
}

static th_type *
n00bb_copyout_type(ir_lower_context_t *context,
                   const n00bb_copyout_t *copyout)
{
    return copyout->local ? copyout->local->type
        : copyout->global ? copyout->global->type
        : copyout->address ? th_ind_referent(copyout->address->type)
        : copyout->index_target
            ? n00bb_node_type(context, copyout->index_target) : nullptr;
}

bool
n00bb_emit_call_copyout(ir_lower_context_t *context,
                        const n00bb_copyout_t *copyout,
                        ir_value_t *successful_result, ir_span_t span)
{
    ir_instr_t *projection = n00bb_copyout_type(context, copyout)
            && successful_result
        ? ir_lower_emit_instruction(context, IR_OP_CALL_OUTPUT, span)
        : nullptr;
    if (!projection || !ir_lower_set_temp_dest(context, projection,
        CORE_STRING_STATIC("__n00bb_copyout_"),
        th_var(ir_module_types(context->module)))
        || !ir_instr_operand_value(projection, successful_result)
        || !ir_instr_operand_uint(projection, copyout->output_index))
        return false;
    return n00bb_emit_copyout_write(context, copyout, projection->dest, span);
}

static ir_value_t *
n00bb_intrinsic_placeholder(ir_lower_context_t *context,
                            parse_sym_entry_t *symbol, th_type *callable,
                            ir_value_t *const *values, size_t count,
                            th_type *result_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_intrinsic.%.*s",
                       (int)symbol->name.u8_bytes, symbol->name.data);
    ir_func_t *function = ir_func_find(context->module,
                                       core_buffer_string(&name));
    if (!function) {
        th_type *declaration = th_type_clone(ir_module_types(context->module),
                                             callable);
        function = declaration
            ? ir_func_add(context->module, core_buffer_string(&name),
                          declaration, IR_LINK_EXTERN)
            : nullptr;
        if (function && !ir_annot_set(lower->ir_context, &function->annots,
                (ir_annot_t){.key = CORE_STRING_STATIC("n00bb.intrinsic"),
                             .kind = IR_ANNOT_STR,
                             .s = symbol->name}))
            function = nullptr;
    }
    core_buffer_release(&name);
    ir_value_t *callee = function
        ? ir_func_address(context->module, function) : nullptr;
    ir_instr_t *call = callee
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    bool ok = call
        && ir_lower_set_temp_dest(context, call,
               CORE_STRING_STATIC("__n00bb_intrinsic_"), result_type)
        && ir_instr_operand_value(call, callee);
    for (size_t i = 0; ok && i < count; i++)
        ok = values[i] && ir_instr_operand_value(call, values[i]);
    if (ok && lower->unit) {
        core_dict_put(&tu_state(lower->unit)->intrinsics, function, symbol);
        n00bb_rewrite_later(context, call);
    }
    return ok ? call->dest : nullptr;
}

static bool
n00bb_dispatch_keywords(ir_lower_context_t *context, parse_grammar_t *grammar,
                        parse_tree_t *call, parse_tree_t *arguments,
                        ir_instr_t *dispatch)
{
    core_list_t(n00bb_tree_ptr_t) keywords = core_list_new(n00bb_tree_ptr_t);
    collect_keyword_nodes(grammar,
        child_nt(grammar, arguments, CORE_STRING_STATIC("keyword-args")),
        &keywords);
    bool ok = dispatch != nullptr;
    for (size_t i = 0; ok && i < keywords.len; i++) {
        parse_tree_t *expression = child_nt(grammar, keywords.data[i],
            CORE_STRING_STATIC("expression"));
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        ok = value && ir_instr_operand_named(dispatch,
            call_keyword_name(keywords.data[i]), value);
    }
    core_list_free(keywords);
    n00bb_lower_context_t *lower = context->user;
    if (ok && call)
        core_dict_put(&tu_state(lower->unit)->dispatch_calls, dispatch, call);
    return ok;
}

bool
n00bb_overloaded(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                 parse_tree_t *callee, parse_sym_entry_t *symbol)
{
    core_string_t spelled = symbol ? symbol->name : (core_string_t){0};
    bool unqualified = simple_identifier_expression(grammar, callee, &spelled);
    n00bb_link_use_site_t *site = state && !unqualified
        ? link_use_site_for_node(state, callee) : nullptr;
    return state && symbol && n00bb_function_has_overloads(state, symbol,
        (n00bb_call_lookup_t){
            .unqualified = unqualified,
            .origin_module = site ? site->origin_module : (core_string_t){0},
            .name = spelled,
        });
}

static bool
mark_write_back(ir_lower_context_t *context, parse_grammar_t *grammar,
                parse_tree_t *receiver, ir_value_t *value)
{
    bool place = receiver && (n00bb_simple_lvalue(receiver)
        || member_access_node(grammar, receiver)
        || n00bb_index_expression_node(grammar, receiver));
    n00bb_lower_context_t *lower = context->user;
    return !place || ir_annot_set(lower->ir_context, &value->def->annots,
        (ir_annot_t){.key = N00BB_WRITE_BACK_KEY, .kind = IR_ANNOT_BOOL,
                     .b = true});
}

static ir_lower_result_t
n00bb_lower_overloaded_call(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *call_node,
                            parse_tree_t *arguments, core_string_t name,
                            th_type *result_type, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    if (n00bb_call_is_error_propagated(grammar, call_node))
        result_type = ir_call_outcome_type(context->module,
            th_func_with_error(types, result_type, th_var(types), nullptr, 0,
                               nullptr, nullptr, 0));
    core_list_t(n00bb_tree_ptr_t) nodes = core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(grammar,
        child_nt(grammar, arguments, CORE_STRING_STATIC("positional-args")),
        &nodes);
    ir_value_t **values = core_alloc_array(ir_value_t *,
                                           nodes.len ? nodes.len : 1);
    bool ok = nodes.len > 0;
    for (size_t i = 0; ok && i < nodes.len; i++) {
        values[i] = ir_lower_node_value(context, nodes.data[i]);
        ok = values[i] != nullptr;
    }
    ir_value_t *value = ok && result_type
        ? n00bb_dispatch(context, name, values, nodes.len, result_type,
                            span)
        : nullptr;
    if (value && !n00bb_dispatch_keywords(context, grammar, call_node,
                                          arguments, value->def))
        value = nullptr;
    if (value && !mark_write_back(context, grammar,
                                  nodes.len ? nodes.data[0] : nullptr, value))
        value = nullptr;
    core_free(values);
    core_list_free(nodes);
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
        : lower_failed_after(context, span,
              CORE_STRING_STATIC("could not lower an overloaded call"));
}

static ir_lower_result_t
n00bb_lower_dispatch_call(ir_lower_context_t *context, parse_visit_t *visit,
                          parse_tree_t *call_node,
                          parse_tree_t *receiver_node, parse_tree_t *arguments,
                          bool index_call, core_string_t method,
                          th_type *result_type, ir_span_t span)
{
    parse_grammar_t *grammar = visit->grammar;
    th_ctx *types = ir_module_types(context->module);
    bool propagated = n00bb_call_is_error_propagated(grammar, call_node);
    th_type *outcome = propagated
        ? ir_call_outcome_type(context->module,
              th_func_with_error(types, result_type, th_var(types), nullptr,
                                 0, nullptr, nullptr, 0))
        : nullptr;
    core_list_t(n00bb_tree_ptr_t) nodes = core_list_new(n00bb_tree_ptr_t);
    core_list_push(nodes, receiver_node);
    parse_tree_t *contents = index_call
        && core_tree_num_children(call_node) > 2
            ? core_tree_child(call_node, 2) : nullptr;
    if (index_call) {
        for (size_t i = 0; contents && i < core_tree_num_children(contents);
             i++) {
            parse_tree_t *child = core_tree_child(contents, i);
            if (nt_is(grammar, child, CORE_STRING_STATIC("expression")))
                core_list_push(nodes, child);
        }
    }
    else
        collect_positional_nodes(grammar,
            child_nt(grammar, arguments,
                     CORE_STRING_STATIC("positional-args")), &nodes);
    ir_value_t **values = core_alloc_array(ir_value_t *, nodes.len);
    bool ok = method.data && receiver_node;
    for (size_t i = 0; ok && i < nodes.len; i++) {
        values[i] = ir_lower_node_value(context, nodes.data[i]);
        ok = values[i] != nullptr;
    }
    ir_value_t *value = ok && (!propagated || outcome)
        ? n00bb_dispatch(context, method, values, nodes.len,
                            outcome ? outcome : result_type, span)
        : nullptr;
    if (value && !index_call
        && !n00bb_dispatch_keywords(context, grammar, call_node, arguments,
                                    value->def))
        value = nullptr;
    if (value && !index_call
        && !mark_write_back(context, grammar, receiver_node, value))
        value = nullptr;
    core_free(values);
    core_list_free(nodes);
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
        : lower_internal(context, span, "could not lower a dispatched call");
}

ir_value_t *
n00bb_comptime_flag(ir_module_t *module, th_type *type)
{
    core_string_t name = CORE_STRING_STATIC(IR_COMPTIME_GLOBAL);
    ir_global_t *global = ir_global_find(module, name);
    if (!global) {
        global = ir_global_add(module, name, type);
        if (!global) return nullptr;
        global->is_extern = true;
    }
    return ir_global_address(module, global);
}

bool
n00bb_emit_comptime_call(ir_lower_context_t *context, ir_func_t *callee,
                         ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    ir_module_t *module = context->module;
    th_type *bool_type = n00bb_ir_builtin_type(module,
                                               CORE_STRING_STATIC("bool"));
    th_type *nil_type = n00bb_ir_builtin_type(module,
                                              CORE_STRING_STATIC("nil"));
    ir_value_t *flag = bool_type ? n00bb_comptime_flag(module, bool_type)
                                 : nullptr;
    ir_instr_t *load = flag
        ? ir_lower_emit_instruction(context, IR_OP_LOAD, span) : nullptr;
    ir_block_t *run = load ? n00bb_branch_block(context, lower, "const_run")
                           : nullptr;
    ir_block_t *join = run ? n00bb_branch_block(context, lower, "const_join")
                           : nullptr;
    ir_instr_t *branch = join && ir_lower_set_temp_dest(context, load,
            CORE_STRING_STATIC("__n00bb_"), bool_type)
        && ir_instr_operand_value(load, flag)
        ? ir_instr_add(context->block, IR_OP_BRANCH) : nullptr;
    if (!branch || !ir_instr_operand_value(branch, load->dest)
        || !ir_instr_operand_block(branch, run)
        || !ir_instr_operand_block(branch, join)) return false;
    branch->span = span;
    context->block = run;
    ir_instr_t *call = ir_lower_emit_instruction(context, IR_OP_CALL, span);
    ir_instr_t *jump = call && nil_type && ir_lower_set_temp_dest(context,
            call, CORE_STRING_STATIC("__n00bb_"), nil_type)
        && ir_instr_operand_value(call, ir_func_address(module, callee))
        ? ir_instr_add(context->block, IR_OP_JUMP) : nullptr;
    if (!jump || !ir_instr_operand_block(jump, join)) return false;
    jump->span = span;
    context->block = join;
    context->reachable = true;
    return true;
}

ir_lower_result_t
n00bb_emit_intrinsic(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                     ir_value_t *const *operands, size_t count,
                     bool copyouts, th_type *call_result_type,
                     ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    bool option_builtin = n00bb_builtin_option_function(symbol);
    bool byte_offset_builtin = n00bb_builtin_byte_offset_function(symbol);
    bool byte_diff_builtin = n00bb_builtin_byte_diff_function(symbol);
    const n00bb_unsafe_arithmetic_t *unsafe_arithmetic =
        n00bb_builtin_unsafe_arithmetic(symbol);
    bool bit_count_builtin = n00bb_builtin_bit_count_function(symbol);
    bool gc_memory_builtin = n00bb_builtin_gc_memory_function(symbol);
    bool volatile_load_builtin = n00bb_builtin_volatile_load_function(symbol);
    bool signal_fence_builtin = n00bb_builtin_signal_fence_function(symbol);
    bool comptime_builtin = n00bb_builtin_comptime_function(symbol);
    bool prefetch_builtin = n00bb_builtin_prefetch_function(symbol);
    bool atomic_rmw_builtin = n00bb_builtin_atomic_rmw_function(symbol);
    bool atomic_cas_builtin = n00bb_builtin_atomic_cas_function(symbol);
    bool atomic_access_builtin = n00bb_builtin_atomic_access_function(symbol);
    if (option_builtin) {
        ir_op_t option_op = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__option_is_nil"))
            ? IR_OP_OPTION_IS_NIL : IR_OP_OPTION_GET_OR_ELSE;
        size_t expected = option_op == IR_OP_OPTION_GET_OR_ELSE ? 2 : 1;
        bool valid = count == expected && !copyouts;
        ir_instr_t *instruction = valid
            ? ir_lower_emit_instruction(context, option_op, span) : nullptr;
        bool emitted = instruction
            && ir_lower_set_temp_dest(context, instruction, CORE_STRING_STATIC("__n00bb_"),
                                      call_result_type);
        for (size_t i = 0; emitted && i < count; i++)
            emitted = ir_instr_operand_value(instruction, operands[i]);
        if (emitted && option_op != IR_OP_OPTION_IS_NIL)
            emitted = n00bb_set_reference_access(lower->ir_context,
                instruction->dest, call_result_type);
        if (!emitted)
            return lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_ONE_OPTION_VALUE);
        return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
    }
    if (byte_offset_builtin) {
        th_ctx *types = ir_module_types(context->module);
        th_type *byte = th_name(types, CORE_STRING_STATIC("byte"));
        th_type *indexable = byte ? th_ind(types, TH_INDEXABLE, byte)
                                  : nullptr;
        bool valid = count == 2 && !copyouts && indexable;
        if (!valid) {
            return lower_builtin_failure(context, false, span, symbol,
                N00B_TEXT_PHRASE_TAKES_A_BYTE_REFERENCE_AND);
        }
        ir_instr_t *cast = ir_lower_emit_instruction(context,
            IR_OP_BITCAST, span);
        bool emitted = cast && ir_lower_set_temp_dest(context, cast,
            CORE_STRING_STATIC("__n00bb_"), indexable)
            && ir_instr_operand_value(cast, operands[0]);
        ir_instr_t *offset = emitted ? ir_lower_emit_instruction(context,
            IR_OP_OFFSET, span) : nullptr;
        emitted = offset && ir_lower_set_temp_dest(context, offset,
            CORE_STRING_STATIC("__n00bb_"), indexable)
            && ir_instr_operand_value(offset, cast->dest)
            && ir_instr_operand_value(offset, operands[1]);
        ir_instr_t *result = emitted ? ir_lower_emit_instruction(context,
            IR_OP_BITCAST, span) : nullptr;
        emitted = result && ir_lower_set_temp_dest(context, result,
            CORE_STRING_STATIC("__n00bb_"), call_result_type)
            && ir_instr_operand_value(result, offset->dest);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, result->dest}
            : lower_internal(context, span,
                "could not emit byte reference offset");
    }
    if (byte_diff_builtin) {
        bool valid = count == 2 && !copyouts;
        ir_instr_t *difference = valid
            ? ir_lower_emit_instruction(context, IR_OP_DIFF, span) : nullptr;
        bool emitted = difference
            && ir_lower_set_temp_dest(context, difference, CORE_STRING_STATIC("__n00bb_"),
                                      call_result_type)
            && ir_instr_operand_value(difference, operands[0])
            && ir_instr_operand_value(difference, operands[1]);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, difference->dest}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_TWO_BYTE_REFERENCES);
    }
    if (unsafe_arithmetic && unsafe_arithmetic->unary) {
        bool valid = count == 1 && !copyouts;
        ir_value_t *result = valid
            ? n00bb_emit_wrapping(context, unsafe_arithmetic->op,
                                  operands[0], call_result_type, span)
            : nullptr;
        return result
            ? (ir_lower_result_t){IR_LOWER_VALUE, result}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_ONE_INTEGER_OPERAND);
    }
    if (unsafe_arithmetic) {
        bool valid = count == 2 && !copyouts;
        ir_value_t *right = valid && unsafe_arithmetic->shift
            ? n00bb_lower_shift_count(context, nullptr, operands[1],
                                      call_result_type, span)
            : valid ? operands[1] : nullptr;
        ir_value_t *result = right
            ? n00bb_emit_binary_ir_with(context, unsafe_arithmetic->op, true,
                call_result_type, operands[0], right, span)
            : nullptr;
        return result
            ? (ir_lower_result_t){IR_LOWER_VALUE, result}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_TWO_INTEGER_OPERANDS_OF);
    }
    if (bit_count_builtin) {
        bool valid = count == 1 && !copyouts;
        ir_op_t op = core_string_eq(symbol->name,
            CORE_STRING_STATIC("__count_leading_zeros"))
                ? IR_OP_CLZ : IR_OP_CTZ;
        ir_instr_t *count = valid
            ? ir_lower_emit_instruction(context, op, span) : nullptr;
        bool emitted = count
            && ir_lower_set_temp_dest(context, count, CORE_STRING_STATIC("__n00bb_"),
                                      call_result_type)
            && ir_instr_operand_value(count, operands[0]);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, count->dest}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_ONE_UINT_OPERAND);
    }
    if (gc_memory_builtin) {
        bool valid = count == 3 && !copyouts;
        bool copy = core_string_eq(symbol->name,
            CORE_STRING_STATIC("__gc_memcpy"));
        ir_instr_t *memory = valid
            ? ir_lower_emit_instruction(context,
                copy ? IR_OP_MEMCPY : IR_OP_MEMSET, span) : nullptr;
        bool emitted = memory
            && ir_instr_operand_value(memory, operands[0])
            && ir_instr_operand_value(memory, operands[1])
            && ir_instr_operand_value(memory, operands[2]);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_builtin_failure(context, valid, span, symbol, copy
                ? N00B_TEXT_PHRASE_TAKES_A_DESTINATION_A_SOURCE
                : N00B_TEXT_PHRASE_TAKES_AN_ADDRESS_A_BYTE);
    }
    if (volatile_load_builtin) {
        bool valid = count == 1 && !copyouts;
        ir_instr_t *load = valid
            ? ir_lower_emit_instruction(context, IR_OP_LOAD, span) : nullptr;
        bool emitted = load && ir_lower_set_temp_dest(context, load,
            CORE_STRING_STATIC("__n00bb_"), call_result_type)
            && ir_instr_operand_value(load, operands[0])
            && ir_instr_add_mod(load, CORE_STRING_STATIC("volatile"))
            && n00bb_set_reference_access(lower->ir_context,
                load->dest, call_result_type);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, load->dest}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_ONE_REFERENCE);
    }
    if (signal_fence_builtin) {
        bool valid = !count && !copyouts;
        ir_instr_t *fence = valid
            ? ir_lower_emit_instruction(context, IR_OP_SIGNAL_FENCE, span)
            : nullptr;
        return fence
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_NO_ARGUMENTS);
    }
    if (comptime_builtin) {
        bool valid = !count && !copyouts;
        ir_value_t *flag = valid
            ? n00bb_comptime_flag(context->module, call_result_type)
            : nullptr;
        ir_instr_t *load = flag
            ? ir_lower_emit_instruction(context, IR_OP_LOAD, span) : nullptr;
        bool emitted = load
            && ir_lower_set_temp_dest(context, load,
                                      CORE_STRING_STATIC("__n00bb_"),
                                      call_result_type)
            && ir_instr_operand_value(load, flag);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, load->dest}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_NO_ARGUMENTS);
    }
    if (prefetch_builtin) {
        bool valid = count == 1 && !copyouts;
        ir_instr_t *prefetch = valid
            ? ir_lower_emit_instruction(context, IR_OP_PREFETCH, span)
            : nullptr;
        bool emitted = prefetch
            && ir_instr_operand_value(prefetch, operands[0]);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_ONE_REFERENCE);
    }
    if (atomic_rmw_builtin) {
        if (count != 2 || copyouts) {
            return lower_builtin_failure(context, false, span, symbol,
                N00B_TEXT_PHRASE_TAKES_A_REFERENCE_TO_AN);
        }
        ir_instr_t *rmw = ir_lower_emit_instruction(context,
            IR_OP_ATOMIC_RMW, span);
        bool emitted = rmw && ir_lower_set_temp_dest(context, rmw,
            CORE_STRING_STATIC("__n00bb_"), call_result_type)
            && ir_instr_operand_value(rmw, operands[0])
            && ir_instr_operand_name(rmw,
                core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_exchange"))
                    || core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__atomic_exchange_acquire"))
                    || core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__atomic_exchange_relaxed"))
                    ? CORE_STRING_STATIC("xchg") : core_string_eq(symbol->name,
                        CORE_STRING_STATIC("__atomic_fetch_add"))
                        || core_string_eq(symbol->name,
                            CORE_STRING_STATIC("__atomic_fetch_add_relaxed"))
                            ? CORE_STRING_STATIC("add") : core_string_eq(symbol->name,
                                CORE_STRING_STATIC("__atomic_fetch_sub"))
                                    ? CORE_STRING_STATIC("sub") : core_string_eq(symbol->name,
                                        CORE_STRING_STATIC("__atomic_fetch_and"))
                                            ? CORE_STRING_STATIC("and")
                                            : CORE_STRING_STATIC("or"))
            && ir_instr_operand_value(rmw, operands[1])
            && (!core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_fetch_or_release"))
                || ir_instr_add_mod(rmw, CORE_STRING_STATIC("release")))
            && ((!core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_fetch_or_relaxed"))
                && !core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_exchange_relaxed"))
                && !core_string_eq(symbol->name,
                    CORE_STRING_STATIC("__atomic_fetch_add_relaxed")))
                || ir_instr_add_mod(rmw, CORE_STRING_STATIC("relaxed")));
        if (emitted && core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_exchange_acquire")))
            emitted = ir_instr_add_mod(rmw, CORE_STRING_STATIC("acquire"));
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, rmw->dest}
            : lower_internal(context, span,
                "could not emit atomic operation");
    }
    if (atomic_cas_builtin) {
        if (count != 3 || copyouts) {
            return lower_builtin_failure(context, false, span, symbol,
                N00B_TEXT_PHRASE_TAKES_A_REFERENCE_TO_AN_2);
        }
        ir_instr_t *cas = ir_lower_emit_instruction(context,
            IR_OP_ATOMIC_CAS, span);
        bool emitted = cas && ir_lower_set_temp_dest(context, cas,
            CORE_STRING_STATIC("__n00bb_"), call_result_type)
            && ir_instr_operand_value(cas, operands[0])
            && ir_instr_operand_value(cas, operands[1])
            && ir_instr_operand_value(cas, operands[2]);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, cas->dest}
            : lower_internal(context, span,
                "could not emit atomic compare-exchange");
    }
    if (atomic_access_builtin) {
        bool loading = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_relaxed"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_acquire"));
        bool relaxed = core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_relaxed"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_store_relaxed"));
        bool ordered = relaxed
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_load_acquire"))
            || core_string_eq(symbol->name,
                CORE_STRING_STATIC("__atomic_store_release"));
        bool valid = count == (loading ? 1 : 2) && !copyouts;
        ir_instr_t *access = valid ? ir_lower_emit_instruction(context,
            loading ? IR_OP_LOAD : IR_OP_STORE, span) : nullptr;
        bool emitted = access
            && (!loading || ir_lower_set_temp_dest(context, access,
                    CORE_STRING_STATIC("__n00bb_"), call_result_type))
            && ir_instr_operand_value(access, operands[0])
            && (loading || ir_instr_operand_value(access, operands[1]))
            && ir_instr_add_mod(access, CORE_STRING_STATIC("atomic"))
            && (!ordered || ir_instr_add_mod(access,
                relaxed ? CORE_STRING_STATIC("relaxed")
                : loading ? CORE_STRING_STATIC("acquire")
                : CORE_STRING_STATIC("release")))
            && (!loading || n00bb_set_reference_access(lower->ir_context,
                access->dest, call_result_type));
        if (!emitted)
            return lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_A_REFERENCE_TO_AN);
        return loading
            ? (ir_lower_result_t){IR_LOWER_VALUE, access->dest}
            : (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    }
    return lower_internal(context, span, "call is not an intrinsic");
}

static bool
mark_unobserved_calls(ir_lower_context_t *context, ir_span_t within)
{
    n00bb_lower_context_t *lower = context->user;
    ir_func_t *function = context->function;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            if (in->op != IR_OP_CALL || in->span.file != within.file
                || in->span.start_byte < within.start_byte
                || in->span.end_byte > within.end_byte) continue;
            if (!ir_annot_set(lower->ir_context, &in->annots, (ir_annot_t){
                    .key = CORE_STRING_STATIC("ir.unobserved"),
                    .kind = IR_ANNOT_BOOL, .b = true})) return false;
        }
    return true;
}

ir_lower_result_t
n00bb_lower_call(ir_lower_context_t *context, parse_visit_t *visit,
                 ir_value_t *const *operands, size_t operand_count,
                 th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    if (operand_count || !result_type)
        return lower_internal(context, span, "call has no result type");
    ir_lower_result_t context_result = {IR_LOWER_FAILED, nullptr};
    int context_call = n00bb_context_call(context, visit, result_type, span,
                                          &context_result);
    if (context_call < 0)
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    if (context_call > 0)
        return context_result;
    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("context-call")))
        return lower_report(context, N00B_DIAG_CONTEXT_OUTSIDE_WITH, span,
            N00B_TEXT_A_CONTEXT_CAN_BE_CALLED,
            N00BB_NO_ARGS);
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller_state = lower
        ? tu_state(lower->unit) : nullptr;
    parse_tree_t *call_node = visit->node;
    core_string_t index_trait = {0}, index_method = {0};
    bool index_call = index_dispatch_for_node(visit->grammar, call_node,
                                               &index_trait, &index_method);
    if (index_call && n00bb_reference_target(visit->grammar, call_node))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (index_call
        && (core_string_eq(index_trait, CORE_STRING_STATIC("Index_Write"))
            || core_string_eq(index_trait,
                              CORE_STRING_STATIC("Index_Slice_Write"))))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    parse_cf_label_t *call_label = lower && caller_state
        ? parse_cf_label_lookup(caller_state->annotations, call_node) : nullptr;
    parse_tree_t *callee = core_tree_num_children(call_node)
        ? core_tree_child(call_node, 0) : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, callee);
    parse_tree_t *receiver_node = index_call
        ? (core_tree_num_children(call_node)
            ? core_tree_child(call_node, 0) : nullptr)
        : member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
    bool member_call = member
        && !call_receiver_is_module(visit->grammar, lower->unit,
                                    receiver_node);
    n00bb_link_use_site_t *member_site = member && caller_state
        ? link_use_site_for_node(caller_state, member) : nullptr;
    n00bb_link_use_site_t *call_site = caller_state
        ? link_use_site_for_node(caller_state, call_node) : nullptr;
    parse_sym_entry_t *symbol = index_call ? nullptr
        : call_site && call_site->init_call ? call_site->symbol
        : member_site && member_site->symbol
            && member_site->symbol->kind == PARSE_SYM_FUNCTION
            ? member_site->symbol
        : callee && lower
        ? attached_symbol(visit->grammar, lower->unit, callee) : nullptr;
    bool callback_call = member_call
        && n00bb_builtin_callback_function(symbol);
    th_type *callback_type = callback_call && receiver_node && caller_state
        ? n00bb_dereferenced_type(caller_state->types,
                                  n00bb_node_type(context, receiver_node))
        : nullptr;
    n00bb_callback_contract_t *nominal_callback = callback_type
        ? n00bb_callback_contract_for_type(caller_state, callback_type)
        : nullptr;
    if (nominal_callback && !n00bb_register_callback_representation(
            context->module, callback_type))
        return lower_internal(context, span,
            "callback call has no registered value representation");
    if (index_call || (member_call && !callback_call)) {
        core_string_t method = index_call ? index_method
            : first_token_text(core_tree_child(member,
                  core_tree_num_children(member) - 1));
        parse_tree_t *arguments = call_label
            && call_label->kind == PARSE_CF_CALL ? call_label->then_body
            : call_arguments_node(visit->grammar, call_node);
        return n00bb_lower_dispatch_call(context, visit, call_node,
            receiver_node, arguments, index_call, method, result_type, span);
    }
    parse_sym_entry_t *call_annotation_symbol = parse_tree_sym(call_node);
    if (!(call_site && call_site->init_call)
        && symbol && call_annotation_symbol
        && call_annotation_symbol->kind == PARSE_SYM_FUNCTION
        && call_annotation_symbol != symbol
        && !(call_annotation_symbol->scope == symbol->scope
             && core_string_eq(call_annotation_symbol->name, symbol->name)))
        return lower_internal(context, span,
            "call annotation target differs from the bound callee symbol");
    parse_sym_entry_t *bound = symbol;
    if (symbol) symbol = canonical_symbol(symbol);
    if (symbol && symbol->kind != PARSE_SYM_FUNCTION) symbol = nullptr;
    if (symbol && n00bb_overloaded(visit->grammar, caller_state,
            core_tree_num_children(call_node) ? core_tree_child(call_node, 0)
                                              : nullptr,
            bound->kind == PARSE_SYM_FUNCTION ? bound : symbol)) {
        parse_tree_t *arguments = call_label
            && call_label->kind == PARSE_CF_CALL ? call_label->then_body
            : call_arguments_node(visit->grammar, call_node);
        return n00bb_lower_overloaded_call(context, visit->grammar, call_node,
                                           arguments, symbol->name,
                                           result_type, span);
    }
    if (!symbol && !bound && core_diagnostics_failed(lower_diagnostics(context)))
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (!symbol) {
        parse_tree_t *callee = core_tree_num_children(call_node)
            ? core_tree_child(call_node, 0) : nullptr;
        if (n00bb_callback_rejected(caller_state, bound ? bound->type
                : callee ? n00bb_node_type(context, callee) : nullptr))
            return (ir_lower_result_t){.status = IR_LOWER_FAILED};
        core_string_t text = source_text_of(lower->unit, callee);
        if (text.u8_bytes)
            lower_report(context, N00B_DIAG_NOT_CALLABLE,
                lower_span_for_tree(context, callee),
                N00B_TEXT_IS_NOT_A_FUNCTION_SO,
                N00BB_ARGS(ARG_TEXT(text)));
        else
            lower_report(context, N00B_DIAG_NOT_CALLABLE, span,
                N00B_TEXT_THIS_IS_NOT_A_FUNCTION,
                N00BB_NO_ARGS);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    bool native_builtin = n00bb_builtin_native_function(symbol);
    bool byte_offset_builtin = n00bb_builtin_byte_offset_function(symbol);
    bool byte_diff_builtin = n00bb_builtin_byte_diff_function(symbol);
    const n00bb_unsafe_arithmetic_t *unsafe_arithmetic =
        n00bb_builtin_unsafe_arithmetic(symbol);
    bool bit_count_builtin = n00bb_builtin_bit_count_function(symbol);
    bool gc_memory_builtin = n00bb_builtin_gc_memory_function(symbol);
    bool volatile_load_builtin = n00bb_builtin_volatile_load_function(symbol);
    bool signal_fence_builtin = n00bb_builtin_signal_fence_function(symbol);
    bool comptime_builtin = n00bb_builtin_comptime_function(symbol);
    bool prefetch_builtin = n00bb_builtin_prefetch_function(symbol);
    bool atomic_rmw_builtin = n00bb_builtin_atomic_rmw_function(symbol);
    bool atomic_cas_builtin = n00bb_builtin_atomic_cas_function(symbol);
    bool atomic_access_builtin = n00bb_builtin_atomic_access_function(symbol);
    bool option_builtin = n00bb_builtin_option_function(symbol);
    bool no_function_body = native_builtin
        || byte_offset_builtin || byte_diff_builtin
        || unsafe_arithmetic
        || bit_count_builtin
        || gc_memory_builtin
        || volatile_load_builtin
        || signal_fence_builtin
        || comptime_builtin
        || prefetch_builtin
        || atomic_rmw_builtin
        || atomic_cas_builtin
        || atomic_access_builtin
        || option_builtin
        || callback_call;
    core_translation_unit_t *callee_unit = !no_function_body && lower
        ? unit_for_symbol(visit->grammar, lower->units, symbol) : nullptr;
    if (!no_function_body && !callee_unit)
        return lower_internal(context, span,
                              "call target is outside the imported modules");
    n00bb_tu_state_t *callee_state = callee_unit
        ? tu_state(callee_unit) : nullptr;
    ir_func_t *callee_function = callee_state && symbol
        ? link_ir_function_for_symbol(
            tu_state(lower->unit)->link_analysis, symbol)
        : nullptr;
    if (!no_function_body && !callee_function)
        return lower_internal(context, span,
                              "call target has no canonical function");
    ir_value_t *callee_value = callback_call && receiver_node
        ? ir_lower_node_value(context, receiver_node)
        : callee_function
            ? ir_func_address(callee_function->owner, callee_function)
                        : nullptr;
    if (callback_call && !callee_value)
        return lower_internal(context, span,
                              "callback receiver has no reference value");
    if (!no_function_body && !callee_value)
        return lower_internal(context, span,
                              "call target has no function declaration");
    th_type *callable = n00bb_symbol_signature(symbol);
    if (callback_call && callee_value) {
        if (nominal_callback && nominal_callback->call)
            callable = n00bb_specialize_callback_role(
                ir_module_types(context->module), nominal_callback,
                callee_value->type, nominal_callback->call->type);
        else {
            th_type *reference_type = th_resolve(callee_value->type);
            callable = reference_type
                && th_kind_of(reference_type) == TH_INDIRECTION
                ? th_ind_referent(reference_type) : nullptr;
        }
    }
    th_type *signature = th_type_clone(ir_module_types(context->module),
                                       callable);
    if (!signature || th_kind_of(signature) != TH_FUNCTION)
        return lower_internal(context, span,
                              "call target does not have a function type");
    th_type *signature_return = th_func_ret(signature);
    if (!signature_return)
        return lower_internal(context, span,
            "call result does not match the resolved function return type");
    if (native_builtin
        && core_string_eq(symbol->name, CORE_STRING_STATIC("__unobserved"))) {
        parse_tree_t *argument = n00bb_call_positional_argument(
            visit->grammar, call_node, 0, 1);
        ir_value_t *value = argument
            ? ir_lower_node_value(context, argument) : nullptr;
        return value && mark_unobserved_calls(context,
                lower_span_for_tree(context, argument))
            ? (ir_lower_result_t){IR_LOWER_VALUE, value}
            : lower_internal(context, span,
                  "__unobserved has no lowered argument");
    }
    if (native_builtin) {
        const core_string_t *native_name = parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.abi.native"));
        core_string_t ir_name = native_name ? *native_name
                                            : (core_string_t){};
        ir_func_t *function = ir_func_find(context->module, ir_name);
        if (!function) {
            th_type *declaration = th_type_clone(
                ir_module_types(context->module), callable);
            function = declaration ? ir_func_add(context->module, ir_name,
                                                  declaration, IR_LINK_EXTERN)
                                   : nullptr;
        }
        if (function && !ir_annot_set(lower->ir_context,
                &function->annots, (ir_annot_t){
                    .key = CORE_STRING_STATIC("n00bb.abi.native"),
                    .kind = IR_ANNOT_STR,
                    .s = ir_name,
                })) return lower_internal(context, span,
                    "could not annotate a native function");
        callee_value = function
            ? ir_func_address(context->module, function) : nullptr;
        if (!callee_value)
            return lower_internal(context, span,
                "could not declare a native function");
    }
    parse_tree_t *args = call_label
        && call_label->kind == PARSE_CF_CALL ? call_label->then_body
        : call_arguments_node(visit->grammar, call_node);
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_ir_value_ptr_t) rest_values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_call_actual_t) actuals =
        core_list_new(n00bb_call_actual_t);
    core_list_t(n00bb_copyout_t) copyouts =
        core_list_new(n00bb_copyout_t);
    size_t parameter_count = th_func_nparams(signature);
    parse_tree_t *formal_params = callable_formal_parameters(visit->grammar,
                                                             symbol);
    parse_tree_t **argument_by_parameter = core_alloc_array(
        parse_tree_t *, parameter_count);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, parameter_count);
    bool *provided = core_alloc_array(bool, parameter_count);
    n00bb_call_plan_t plan = {0};
    bool mapped = n00bb_call_plan_init(&plan, visit->grammar, symbol,
                                       signature, 0);
    if (mapped) n00bb_call_plan_collect(&plan, args);
    if (mapped && !n00bb_call_plan_bind(&plan)) {
        n00bb_call_plan_report(&plan, lower_unit(context),
                               lower_diagnostics(context), call_node);
        mapped = false;
    }
    for (size_t i = 0; mapped && i < n00bb_call_plan_argument_count(&plan);
         i++) {
        size_t index = 0;
        parse_tree_t *argument = n00bb_call_plan_argument(&plan, i, &index);
        if (index != SIZE_MAX && index >= parameter_count) {
            lower_internal(context, lower_span_for_tree(context, argument),
                "a call argument has no parameter to lower into");
            mapped = false;
            break;
        }
        if (index != SIZE_MAX) {
            argument_by_parameter[index] = argument;
            provided[index] = true;
        }
        core_list_push(actuals, ((n00bb_call_actual_t){argument, index}));
    }
    n00bb_call_plan_free(&plan);
    if (!mapped) {
        ir_lower_result_t failed = lower_failed_after(context, span,
            CORE_STRING_STATIC(
                "call arguments could not be matched to parameters"));
        if (core_diagnostics_failed(lower_diagnostics(context)))
            failed = lower_placeholder(context, result_type, span);
        core_list_free(actuals);
        core_list_free(values);
        core_list_free(rest_values);
        core_list_free(copyouts);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        return failed;
    }
    th_type **argument_types = actuals.len
        ? core_alloc_array(th_type *, actuals.len) : nullptr;
    if (actuals.len && !argument_types) {
        core_list_free(actuals);
        core_list_free(values);
        core_list_free(rest_values);
        core_list_free(copyouts);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        return lower_internal(context, span,
            "could not allocate call argument type context");
    }
    for (size_t i = 0; i < actuals.len; i++) {
        parse_tree_t *argument = actuals.data[i].node;
        size_t index = actuals.data[i].parameter_index;
        const th_param *formal = index == SIZE_MAX
            ? th_func_rest(signature) : th_func_param(signature, index);
        th_type *actual = n00bb_node_type(context, argument);
        argument_types[i] = formal
            && th_mode_is_input_only(formal->mode) && actual
            && !th_is_closed(ir_module_types(context->module), actual)
            && !parse_tree_integer_literal_origin(argument)
                ? actual : nullptr;
    }
    bool collected = true;
    for (size_t i = 0; collected && i < actuals.len; i++) {
        size_t index = actuals.data[i].parameter_index;
        if (index == SIZE_MAX) {
            const th_param *rest = th_func_rest(signature);
            parse_tree_t *argument = actuals.data[i].node;
            ir_value_t *value = ir_lower_node_value(context, argument);
            if (!rest || !value) {
                collected = false;
                break;
            }
            if (rest->mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower ? lower->unit : nullptr, argument, value);
                if (!value) {
                    collected = false;
                    break;
                }
            }
            core_list_push(rest_values, value);
            continue;
        }
        const th_param *parameter = th_func_param(signature, index);
        parse_tree_t *argument = actuals.data[i].node;
        th_mode mode = parameter ? parameter->mode : TH_IN;
        function_parameter_info(visit->grammar, symbol, formal_params,
            index, nullptr, nullptr,nullptr, &mode);
        if (!parameter || !argument) {
            collected = false;
            break;
        }
        parse_sym_entry_t *formal = nullptr;
        function_parameter_info(visit->grammar, symbol, formal_params,
            index, &formal, nullptr, nullptr, nullptr);
        ir_shape_t *required_shape = formal
            ? parse_sym_binding(formal,
                CORE_STRING_STATIC("ir.array-shape"))
            : function_parameter_shape(symbol, index);
        if (th_mode_is_input_only(mode)) {
            ir_value_t *value = ir_lower_node_value(context, argument);
            if (!value) {
                collected = false;
                break;
            }
            if (mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower ? lower->unit : nullptr, argument, value);
                if (!value) {
                    collected = false;
                    break;
                }
            }
            value_by_parameter[index] = value;
            continue;
        }
        if (mode != TH_OUT && mode != TH_INOUT) {
            collected = false;
            break;
        }
        n00bb_out_location_t location = n00bb_out_argument_location(context,
            visit->grammar, argument);
        parse_sym_entry_t *arg_symbol = location.symbol;
        ir_local_t *target = location.local;
        ir_value_t *global = location.global;
        ir_value_t *address = location.address;
        parse_tree_t *index_target = location.index_target;
        parse_tree_t *lvalue = location.lvalue;
        th_type *target_type = location.type;
        if (!target_type) {
            core_string_t parameter_name = {0};
            function_parameter_info(visit->grammar, symbol, formal_params,
                index, nullptr, &parameter_name, nullptr, nullptr);
            report_unwritable_output(context, visit->grammar, argument,
                mode, symbol, parameter_name);
            collected = false;
            break;
        }
        parse_tree_t *aliased = nullptr;
        for (size_t previous = 0; arg_symbol && lvalue
                && previous < copyouts.len; previous++)
            if (copyouts.data[previous].symbol == arg_symbol
                && copyouts.data[previous].local == target
                && copyouts.data[previous].global == global)
                aliased = copyouts.data[previous].source_node;
        if (aliased) {
            lower_report(context, N00B_DIAG_SAME_OUTPUT_TWICE,
                lower_span_for_tree(context, argument),
                N00B_TEXT_IS_PASSED_TO_MORE_THAN,
                N00BB_ARGS(ARG_TEXT(arg_symbol->name), ARG_TEXT(symbol->name)));
            lower_secondary(context, lower_span_for_tree(context, aliased),
                N00B_TEXT_FIRST_PASSED_HERE,
                N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_PASS_A_SEPARATE_VARIABLE_TO,
                N00BB_NO_ARGS);
            collected = false;
            break;
        }
        ir_value_t *initial = nullptr;
        if (mode == TH_INOUT) {
            initial = n00bb_out_location_read(context, argument, &location,
                                              span);
            if (!initial || !n00bb_emit_sequence_shape_check(context,
                    initial, required_shape,
                    lower_span_for_tree(context, argument))) {
                collected = false;
                break;
            }
        }
        value_by_parameter[index] = initial;
        size_t output_index = 0;
        for (size_t previous = 0; previous < index; previous++) {
            const th_param *prior = th_func_param(signature, previous);
            if (prior && th_mode_produces_output(prior->mode)) output_index++;
        }
        core_list_push(copyouts,
            ((n00bb_copyout_t){.symbol = arg_symbol,
                               .local = target,
                               .global = global,
                               .address = address,
                               .grammar = visit->grammar,
                               .site_node = visit->node,
                               .index_target = index_target,
                               .source_node = argument,
                               .output_index = output_index,
                               .required_shape = required_shape}));
    }
    for (size_t i = 0; collected && i < parameter_count; i++) {
        parse_sym_entry_t *formal = nullptr;
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool has_formal = byte_offset_builtin
            || byte_diff_builtin || unsafe_arithmetic || bit_count_builtin
            || gc_memory_builtin
            || volatile_load_builtin
            || prefetch_builtin
            || atomic_rmw_builtin || atomic_cas_builtin
            || atomic_access_builtin
            || native_builtin
            || function_parameter_info(visit->grammar, symbol,
                formal_params, i, &formal, nullptr, &flags, &mode);
        const th_param *parameter = th_func_param(signature, i);
        if (!has_formal || !parameter) {
            collected = false;
            break;
        }
        if (flags & 1u) {
            if (!th_mode_is_input_only(parameter->mode)) {
                collected = false;
                break;
            }
            if (!provided[i]) {
                core_list_push(values, nullptr);
                continue;
            }
            ir_value_t *value = value_by_parameter[i];
            if (!value) {
                collected = false;
                break;
            }
            ir_shape_t *required_shape = formal
                ? parse_sym_binding(formal,
                    CORE_STRING_STATIC("ir.array-shape"))
                : function_parameter_shape(symbol, i);
            parse_tree_t *shape_source = argument_by_parameter[i]
                ? argument_by_parameter[i]
                : formal ? child_nt(visit->grammar, formal->decl_node,
                                    CORE_STRING_STATIC("expression")) : nullptr;
            if (th_mode_is_input_only(parameter->mode)
                && ((!n00bb_attach_expression_shape(context,
                        visit->grammar, shape_source, value)
                        && shape_source)
                    || !n00bb_emit_sequence_shape_check(context, value,
                    required_shape, formal && formal->decl_node
                        ? lower_span_for_tree(context, formal->decl_node)
                        : span))) {
                collected = false;
                break;
            }
            core_list_push(values, value);
        }
        else if (mode == TH_OUT) {
            core_list_push(values, nullptr);
        }
        else if (value_by_parameter[i]) {
            ir_shape_t *required_shape = formal
                ? parse_sym_binding(formal,
                    CORE_STRING_STATIC("ir.array-shape"))
                : function_parameter_shape(symbol, i);
            if (th_mode_is_input_only(mode)
                && (!n00bb_attach_expression_shape(context,
                        visit->grammar, argument_by_parameter[i],
                        value_by_parameter[i])
                    || !n00bb_emit_sequence_shape_check(context,
                        value_by_parameter[i], required_shape,
                        argument_by_parameter[i]
                            ? lower_span_for_tree(context,
                                argument_by_parameter[i]) : span))) {
                collected = false;
                break;
            }
            core_list_push(values, value_by_parameter[i]);
        }
        else {
            collected = false;
        }
    }
    if (collected && th_func_rest(signature)) {
        ir_value_t *array = n00bb_pack_rest_values(context,
            th_func_rest(signature)->type, &rest_values, span);
        if (array) core_list_push(values, array);
        else collected = false;
    }
    bool query = native_builtin && effects_query_builtin(symbol);
    ir_value_t *query_subject = query && parameter_count
        && argument_by_parameter[0]
        ? ir_lower_node_value(context, argument_by_parameter[0]) : nullptr;
    core_string_t query_name = {0};
    if (query && parameter_count == 2)
        (void)n00bb_call_literal_argument(visit->grammar, caller_state,
                                          call_node, 1, 2, &query_name);
    core_free(argument_types);
    core_list_free(actuals);
    core_list_free(rest_values);
    core_free(argument_by_parameter);
    core_free(value_by_parameter);
    core_free(provided);
    if (!collected) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_failed_after(context, span, CORE_STRING_STATIC(
            "call arguments could not be prepared for their parameter "
            "modes"));
    }
    if (native_builtin && core_string_eq(symbol->name,
            CORE_STRING_STATIC("__array_length"))) {
        bool valid = values.len == 1 && values.data[0]
            && !copyouts.len;
        ir_instr_t *length = valid
            ? ir_lower_emit_instruction(context, IR_OP_SEQUENCE_LENGTH, span)
            : nullptr;
        bool emitted = length
            && ir_lower_set_temp_dest(context, length, CORE_STRING_STATIC("__n00bb_"),
                                       th_func_ret(th_resolve(signature)))
            && ir_instr_operand_value(length, values.data[0]);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted
            ? (ir_lower_result_t){IR_LOWER_VALUE, length->dest}
            : lower_builtin_failure(context, valid, span, symbol,
                N00B_TEXT_PHRASE_TAKES_ONE_ARRAY);
    }
    th_ctx *types = ir_module_types(context->module);
    th_type *error_type = th_func_error(signature);
    bool has_error = error_type && !th_is_nil(error_type);
    th_type *call_result_type = has_error
        ? ir_call_outcome_type(context->module, signature)
        : signature_return;
    bool intrinsic = option_builtin || byte_offset_builtin
        || byte_diff_builtin || unsafe_arithmetic || bit_count_builtin
        || gc_memory_builtin || volatile_load_builtin || signal_fence_builtin
        || comptime_builtin || prefetch_builtin || atomic_rmw_builtin
        || atomic_cas_builtin || atomic_access_builtin;
    th_type *intrinsic_contract = n00bb_symbol_signature(symbol);
    bool open_intrinsic = intrinsic
        && (!th_is_closed(types, call_result_type)
            || (!option_builtin && intrinsic_contract
                && !th_is_closed(types, intrinsic_contract)));
    for (size_t i = 0; intrinsic && i < values.len; i++)
        open_intrinsic |= values.data[i]
            && (!th_is_closed(types, values.data[i]->type)
                || (!option_builtin
                    && th_option_item(th_resolve(values.data[i]->type))));
    if (open_intrinsic) {
        ir_value_t *placeholder = copyouts.len ? nullptr
            : n00bb_intrinsic_placeholder(context, symbol,
                  intrinsic_contract,
                  values.data, values.len, call_result_type, span);
        core_list_free(values);
        core_list_free(copyouts);
        return placeholder ? (ir_lower_result_t){IR_LOWER_VALUE, placeholder}
            : lower_internal(context, span,
                "could not lower an open intrinsic call");
    }
    if (intrinsic) {
        ir_lower_result_t emitted = n00bb_emit_intrinsic(context, symbol,
            values.data, values.len, copyouts.len, call_result_type, span);
        core_list_free(values);
        core_list_free(copyouts);
        return emitted;
    }
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
        nominal_callback ? IR_OP_CALLBACK_INVOKE : IR_OP_CALL, span);
    bool emitted = instruction && ir_lower_set_temp_dest(context, instruction,
                                                 CORE_STRING_STATIC("__n00bb_"), call_result_type)
            && ir_instr_operand_value(instruction, callee_value);
    for (size_t i = 0; emitted && i < values.len; i++) {
        if (values.data[i]) {
            emitted = ir_instr_operand_value(instruction, values.data[i]);
            continue;
        }
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool known = i < parameter_count
            && function_parameter_info(visit->grammar, symbol,
                formal_params, i, nullptr, nullptr,&flags, &mode);
        emitted = known && (flags & 1u) && th_mode_is_input_only(mode)
            ? ir_instr_operand_missing(instruction)
            : known && mode == TH_OUT
                ? ir_instr_operand_out(instruction) : false;
    }
    if (emitted) {
        core_list_t(th_param) call_params = core_list_new(th_param);
        bool signature_ok = true;
        for (size_t i = 0; signature_ok && i < parameter_count; i++) {
            const th_param *parameter = th_func_param(signature, i);
            th_mode mode = parameter ? parameter->mode : TH_IN;
            if (!native_builtin
                && !function_parameter_info(visit->grammar, symbol,
                    formal_params, i, nullptr, nullptr,nullptr,
                    &mode)) {
                signature_ok = false;
                break;
            }
            if (!parameter || !th_mode_is_valid(mode)) {
                signature_ok = false;
                break;
            }
            core_list_push(call_params, ((th_param){
                .mode = mode, .type = parameter->type,
            }));
        }
        const th_param *source_rest = th_func_rest(signature);
        th_param lowered_rest = {0};
        const th_param *rest = source_rest
            && lowered_rest_type(ir_module_types(context->module),
                source_rest, source_rest->mode, &lowered_rest)
            ? &lowered_rest : nullptr;
        if (source_rest && !rest) signature_ok = false;
        core_list_t(th_kwparam) keywords = core_list_new(th_kwparam);
        for (size_t i = 0; signature_ok
             && i < th_func_nkw(signature); i++) {
            const th_kwparam *keyword = th_func_kw(signature, i);
            if (!keyword) signature_ok = false;
            else core_list_push(keywords, *keyword);
        }
        th_type *call_signature = signature_ok
            ? th_func_collect_rest(ir_module_types(context->module),
                th_func_with_error(ir_module_types(context->module),
                    th_func_ret(signature), th_func_error(signature),
                    call_params.data, call_params.len, rest,
                    keywords.data, keywords.len)) : nullptr;
        emitted = call_signature
            && ir_instr_set_call_signature(instruction, call_signature);
        if (emitted && nominal_callback)
            emitted = ir_instr_set_callback_invoke_handler(instruction,
                n00bb_specialize_callback_role(
                    ir_module_types(context->module), nominal_callback,
                    callee_value->type, nominal_callback->handler->type));
        core_list_free(call_params);
        core_list_free(keywords);
    }
    if (!emitted) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_internal(context, span,
                           "could not emit the function call");
    }
    instruction->span = span;
    if (query && !annotate_effects_query(lower->ir_context, instruction,
                                         query_subject, query_name)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_internal(context, span,
                           "could not annotate the call's effects");
    }
    ir_shape_t *return_shape = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.return-shape"));
    if (!has_error && !n00bb_emit_sequence_shape_check(context,
            instruction->dest, return_shape, span)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_internal(context, span,
                           "could not emit the function return shape check");
    }
    if (!has_error && return_shape
        && !ir_value_set_shape(instruction->dest, return_shape)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_internal(context, span,
                           "could not attach the checked function return shape");
    }
    if (n00bb_open_type(context, call_result_type))
        n00bb_rewrite_later(context, instruction);
    if (!n00bb_set_reference_access(lower->ir_context, instruction->dest,
                                    call_result_type)) {
        core_list_free(values);
        core_list_free(copyouts);
        return lower_internal(context, span,
            "could not attach the resolved return alias");
    }
    core_list_free(values);
    n00bb_order_copyouts(&copyouts);
    if (copyouts.len) {
        if (has_error) {
            core_list_push(lower->pending_call_copyouts,
                ((n00bb_pending_copyouts_t){instruction->dest, copyouts}));
        }
        else {
            bool copied = true;
            for (size_t i = 0; copied && i < copyouts.len; i++)
                copied = n00bb_emit_call_copyout(context, &copyouts.data[i],
                                                  instruction->dest, span);
            core_list_free(copyouts);
            if (!copied)
                return lower_internal(context, span,
                                   "could not copy out call parameters");
        }
    }
    else core_list_free(copyouts);
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static void
n00bb_constructor_assignment_target(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    th_type *result_type,
                                    parse_tree_t **assignment,
                                    ir_value_t **address)
{
    parse_tree_t *child = visit->node;
    for (parse_tree_t *parent = parse_tree_parent(child);
         parent && !core_tree_is_leaf(parent);
         child = parent, parent = parse_tree_parent(parent)) {
        if (nt_is(visit->grammar, parent, CORE_STRING_STATIC("sym-decl"))) {
            if (child_nt(visit->grammar, parent, CORE_STRING_STATIC("expression")) != child)
                break;
            parse_sym_entry_t *symbol = parse_tree_sym(parent);
            ir_value_t *storage = symbol
                ? ir_lower_bound_address(context, symbol) : nullptr;
            th_type *referent = storage
                ? th_ind_referent(storage->type) : nullptr;
            if (referent && ir_type_eq(context->module, referent,
                                       result_type)) {
                *assignment = parent;
                *address = storage;
            }
            break;
        }
        if (core_tree_num_children(parent) != 1) break;
        if (!n00bb_primary_node(visit->grammar, parent)
            && !n00bb_postfix_node(visit->grammar, parent)
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("unary-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("mul-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("add-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("shift-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("bitand-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("bitxor-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("bitor-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("compare-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("equality-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("and-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("or-expr"))
            && !nt_is(visit->grammar, parent, CORE_STRING_STATIC("expression"))) break;
    }
}

static ir_lower_result_t
n00bb_lower_dispatch_new(ir_lower_context_t *context, parse_visit_t *visit,
                         th_type *result_type, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    parse_tree_t *args = child_nt(visit->grammar, visit->node,
                                  CORE_STRING_STATIC("call-args"));
    core_list_t(n00bb_tree_ptr_t) nodes = core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, args, CORE_STRING_STATIC("positional-args")),
        &nodes);
    th_type *address_type = n00bb_indirect_type(types, result_type);
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    bool ok = allocation
        && ir_lower_set_temp_dest(context, allocation,
               CORE_STRING_STATIC("__n00bb_new_storage_"), address_type)
        && ir_instr_operand_type(allocation, result_type);
    ir_instr_t *init = ok
        ? ir_lower_emit_instruction(context, IR_OP_DISPATCH, span) : nullptr;
    ok = init
        && ir_lower_set_temp_dest(context, init,
               CORE_STRING_STATIC("__n00bb_new_init_"),
               th_name(types, CORE_STRING_STATIC("nil")))
        && ir_instr_operand_name(init, CORE_STRING_STATIC("__init"))
        && ir_instr_operand_fresh(init, allocation->dest);
    for (size_t i = 0; ok && i < nodes.len; i++) {
        ir_value_t *value = ir_lower_node_value(context, nodes.data[i]);
        ok = value && ir_instr_operand_value(init, value);
    }
    ok = ok && n00bb_dispatch_keywords(context, visit->grammar, visit->node,
                                       args, init);
    core_list_free(nodes);
    if (ok) n00bb_rewrite_later(context, init);
    ir_value_t *value = ok
        ? n00bb_emit_indirect_read(context, allocation->dest, result_type,
                                   span)
        : nullptr;
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
        : lower_internal(context, span, "could not lower a dispatched `new`");
}

ir_lower_result_t
n00bb_lower_constructor_new(ir_lower_context_t *context, parse_visit_t *visit,
                            th_type *result_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *caller_state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = caller_state
        ? caller_state->link_analysis : nullptr;
    n00bb_link_use_site_t *site = caller_state
        ? link_use_site_for_node(caller_state, visit->node) : nullptr;
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    if (lower && !symbol)
        return n00bb_lower_dispatch_new(context, visit, result_type, span);
    if (!lower || !caller_state || !analysis || !symbol
        || symbol->kind != PARSE_SYM_FUNCTION)
        return lower_internal(context, span,
            "`new` has no resolved `__init`");

    core_translation_unit_t *callee_unit = unit_for_symbol(visit->grammar,
        lower->units, symbol);
    n00bb_tu_state_t *callee_state = callee_unit
        ? tu_state(callee_unit) : nullptr;
    ir_func_t *callee_function = callee_state
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    ir_module_t *callee_module = callee_state
        ? unit_module(callee_state) : nullptr;
    ir_value_t *callee = callee_module && callee_function
        ? ir_func_address(callee_module, callee_function) : nullptr;
    if (!callee || !callee_function)
        return lower_internal(context, span,
            "`__init` has no function in this execution phase");

    th_ctx *types = ir_module_types(context->module);
    th_type *source_type = constructor_signature_instance(types, symbol);
    if (!source_type || th_kind_of(source_type) != TH_FUNCTION
        || !th_func_nparams(source_type)
        || !th_is_nil(th_func_error(source_type))
        || !th_is_nil(th_func_ret(source_type)))
        return lower_report(context, N00B_DIAG_INIT_SIGNATURE, span,
            N00B_TEXT_THE_INIT_THAT_NEW_CALLS,
            N00BB_NO_ARGS);
    const th_param *receiver = th_func_param(source_type, 0);
    bool object = n00bb_object_type(types, result_type);
    bool object_in = object && receiver && receiver->mode == TH_IN;
    if (!receiver || (receiver->mode != TH_INOUT && !object_in))
        return lower_internal(context, span,
            "`__init` receiver does not match the constructed type");

    size_t parameter_count = th_func_nparams(source_type);
    parse_tree_t *function_signature = child_nt(visit->grammar,
        symbol->decl_node, CORE_STRING_STATIC("func-type-signature"));
    parse_tree_t *formal_params = child_nt(visit->grammar,
        function_signature, CORE_STRING_STATIC("param-decl"));
    core_list_t(parse_sym_entry_ptr_t) formals =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *rest_formal = nullptr;
    collect_ir_parameters(visit->grammar, formal_params, &formals,
                          &rest_formal);
    if (symbol->decl_node && formals.len != parameter_count) {
        core_list_free(formals);
        return lower_internal(context, span,
            "constructor parameter symbols do not match its signature");
    }

    parse_tree_t **argument_by_parameter = core_alloc_array(
        parse_tree_t *, parameter_count);
    ir_value_t **value_by_parameter = core_alloc_array(
        ir_value_t *, parameter_count);
    bool *provided = core_alloc_array(bool, parameter_count);
    bool mapped = argument_by_parameter && value_by_parameter && provided;
    if (mapped) {
        memset(argument_by_parameter, 0,
               parameter_count * sizeof(*argument_by_parameter));
        memset(value_by_parameter, 0,
               parameter_count * sizeof(*value_by_parameter));
        memset(provided, 0, parameter_count * sizeof(*provided));
        provided[0] = true;
    }
    n00bb_call_plan_t plan;
    mapped = n00bb_plan_call(&plan, visit->grammar, symbol, source_type,
        child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("call-args")),
        1) && mapped;
    for (size_t i = 1; mapped && i < plan.slot_count; i++) {
        if (!plan.slots[i].site) continue;
        if (i >= parameter_count) {
            mapped = false;
            break;
        }
        argument_by_parameter[i] = plan.slots[i].argument;
        provided[i] = true;
    }
    core_list_t(n00bb_tree_ptr_t) rest_arguments = plan.rest;
    plan.rest = core_list_new(n00bb_tree_ptr_t);
    n00bb_call_plan_free(&plan);
    if (!mapped) {
        core_list_free(rest_arguments);
        core_free(argument_by_parameter);
        core_free(value_by_parameter);
        core_free(provided);
        core_list_free(formals);
        return lower_report(context, N00B_DIAG_CONSTRUCTOR_ARGUMENTS, span,
            N00B_TEXT_THESE_ARGUMENTS_DO_NOT_MATCH,
            N00BB_NO_ARGS);
    }

    core_list_t(n00bb_copyout_t) copyouts =
        core_list_new(n00bb_copyout_t);
    bool collected = true;
    for (size_t i = 1; collected && i < parameter_count; i++) {
        const th_param *parameter = th_func_param(source_type, i);
        parse_sym_entry_t *formal = i < formals.len ? formals.data[i]
                                                    : nullptr;
        th_mode mode = TH_IN;
        uint8_t flags = 0;
        if (!parameter
            || !function_parameter_info(visit->grammar, symbol,
                formal_params, i, nullptr, nullptr,&flags, &mode)) {
            collected = false;
            break;
        }
        parse_tree_t *argument = argument_by_parameter[i];
        if (!argument && !(flags & 1u)) {
            collected = false;
            break;
        }
        if (!argument) continue;
        ir_shape_t *required_shape = formal ? parse_sym_binding(formal,
            CORE_STRING_STATIC("ir.array-shape")) : nullptr;
        if (th_mode_is_input_only(mode)) {
            ir_value_t *value = ir_lower_node_value(context, argument);
            if (!value
                || !n00bb_attach_expression_shape(context, visit->grammar,
                                                    argument, value)
                || !n00bb_emit_sequence_shape_check(context, value,
                    required_shape, lower_span_for_tree(context, argument))) {
                collected = false;
                break;
            }
            if (mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower->unit, argument, value);
                if (!value) {
                    collected = false;
                    break;
                }
            }
            value_by_parameter[i] = value;
            continue;
        }
        if (mode != TH_OUT && mode != TH_INOUT) {
            collected = false;
            break;
        }
        n00bb_out_location_t location = n00bb_out_argument_location(context,
            visit->grammar, argument);
        parse_sym_entry_t *arg_symbol = location.symbol;
        ir_local_t *target_local = location.local;
        ir_value_t *target_value = location.global;
        ir_value_t *address = location.address;
        parse_tree_t *index_target = location.index_target;
        parse_tree_t *lvalue = location.lvalue;
        th_type *target_type = location.type;
        if (!target_type) {
            collected = false;
            break;
        }
        bool aliases = false;
        for (size_t j = 0; lvalue && j < copyouts.len; j++)
            aliases |= copyouts.data[j].symbol == arg_symbol;
        if (aliases) {
            collected = false;
            break;
        }
        ir_value_t *initial = nullptr;
        if (mode == TH_INOUT) {
            initial = n00bb_out_location_read(context, argument, &location,
                                              span);
            if (!initial || !n00bb_emit_sequence_shape_check(context,
                    initial, required_shape,
                    lower_span_for_tree(context, argument))) {
                collected = false;
                break;
            }
        }
        value_by_parameter[i] = initial;
        size_t output_index = 1;
        for (size_t previous = 1; previous < i; previous++) {
            th_mode previous_mode = TH_IN;
            if (!function_parameter_info(visit->grammar, symbol,
                    formal_params, previous, nullptr, nullptr,
                    nullptr, &previous_mode)) {
                collected = false;
                break;
            }
            if (th_mode_produces_output(previous_mode)) output_index++;
        }
        if (!collected) break;
        core_list_push(copyouts,
            ((n00bb_copyout_t){.symbol = arg_symbol,
                .local = target_local, .global = target_value,
                .address = address, .grammar = visit->grammar,
                .site_node = visit->node, .index_target = index_target,
                .source_node = argument,
                .output_index = output_index,
                .required_shape = required_shape}));
    }
    for (size_t i = 1; collected && i < parameter_count; i++) {
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        const th_param *parameter = th_func_param(source_type, i);
        if (!function_parameter_info(visit->grammar, symbol, formal_params,
                i, nullptr, nullptr, &flags, &mode) || !parameter) {
            collected = false;
            break;
        }
        if (flags & 1u) {
            if (!th_mode_is_input_only(mode)) {
                collected = false;
                break;
            }
            if (!provided[i]) {
                value_by_parameter[i] = nullptr;
                continue;
            }
        }
        if (mode == TH_OUT) value_by_parameter[i] = nullptr;
        else if (!value_by_parameter[i]) collected = false;
    }

    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    ir_value_t *allocation_address = nullptr;
    parse_tree_t *direct_assignment = nullptr;
    ir_value_t *fresh_object = collected && object
        ? n00bb_object_allocate(context, result_type, nullptr, span)
        : nullptr;
    if (object && !fresh_object) collected = false;
    if (collected && !object_in)
        n00bb_constructor_assignment_target(context, visit, result_type,
                                            &direct_assignment,
                                            &allocation_address);
    if (collected && !object_in && !allocation_address) {
        th_type *address_type = n00bb_indirect_type(types, result_type);
        char allocation_buffer[48];
        core_string_t allocation_name = n00bb_format_name(allocation_buffer,
            sizeof(allocation_buffer), "__n00bb_construct_storage_%llu",
            (unsigned long long)context->next_value_id++);
        ir_instr_t *allocation = address_type
            ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
        collected = allocation
            && ir_instr_set_dest(allocation, allocation_name, address_type)
            && ir_instr_operand_type(allocation, result_type);
        if (collected) allocation_address = allocation->dest;
    }
    if (collected && object && !object_in)
        collected = n00bb_emit_indirect_write(context, allocation_address,
                                              fresh_object, span);
    n00bb_order_copyouts(&copyouts);
    for (size_t i = 1; collected && i < parameter_count; i++)
        core_list_push(values, value_by_parameter[i]);
    if (collected && th_func_rest(source_type)) {
        const th_param *rest = th_func_rest(source_type);
        core_list_t(n00bb_ir_value_ptr_t) items =
            core_list_new(n00bb_ir_value_ptr_t);
        for (size_t i = 0; collected && i < rest_arguments.len; i++) {
            parse_tree_t *argument = rest_arguments.data[i];
            ir_value_t *value = ir_lower_node_value(context, argument);
            collected = value != nullptr;
            if (collected && rest->mode == TH_CONSUME) {
                value = n00bb_emit_consume(context, visit->grammar,
                    lower->unit, argument, value);
                collected = value != nullptr;
            }
            if (collected) core_list_push(items, value);
        }
        ir_value_t *array = collected
            ? n00bb_pack_rest_values(context, rest->type, &items, span)
            : nullptr;
        if (array) core_list_push(values, array);
        else collected = false;
        core_list_free(items);
    }
    core_list_free(rest_arguments);

    char call_buffer[48];
    core_string_t call_name = n00bb_format_name(call_buffer,
        sizeof(call_buffer), "__n00bb_init_%llu",
        (unsigned long long)context->next_value_id++);
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    ir_instr_t *call = collected && nil_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    collected = call && ir_instr_set_dest(call, call_name, nil_type)
        && ir_instr_operand_value(call, callee)
        && (object_in ? ir_instr_operand_value(call, fresh_object)
                      : ir_instr_operand_fresh(call, allocation_address));
    for (size_t i = 0; collected && i < values.len; i++) {
        if (values.data[i]) {
            collected = ir_instr_operand_value(call, values.data[i]);
            continue;
        }
        size_t parameter_index = i + 1;
        uint8_t flags = 0;
        th_mode mode = TH_IN;
        bool known = parameter_index < parameter_count
            && function_parameter_info(visit->grammar, symbol,
                formal_params, parameter_index, nullptr, nullptr,
                &flags, &mode);
        collected = known && (flags & 1u) && th_mode_is_input_only(mode)
            ? ir_instr_operand_missing(call)
            : known && mode == TH_OUT ? ir_instr_operand_out(call) : false;
    }
    core_list_t(th_param) call_params = core_list_new(th_param);
    core_list_t(th_kwparam) call_keywords = core_list_new(th_kwparam);
    if (collected) {
        const th_param *self = th_func_param(source_type, 0);
        collected = self != nullptr;
        if (collected) core_list_push(call_params, ((th_param){
            .mode = object_in ? TH_IN : TH_INOUT, .type = self->type,
        }));
    }
    for (size_t i = 1; collected && i < parameter_count; i++) {
        const th_param *parameter = th_func_param(source_type, i);
        th_mode mode = TH_IN;
        collected = parameter && function_parameter_info(visit->grammar,
            symbol, formal_params, i, nullptr, nullptr,nullptr,
            &mode);
        if (collected) core_list_push(call_params, ((th_param){
            .mode = mode, .type = parameter->type,
        }));
    }
    const th_param *source_rest = th_func_rest(source_type);
    th_param lowered_rest = {0};
    const th_param *rest = source_rest
        && lowered_rest_type(types, source_rest, source_rest->mode,
                             &lowered_rest)
        ? &lowered_rest : nullptr;
    if (source_rest && !rest) collected = false;
    for (size_t i = 0; collected && i < th_func_nkw(source_type); i++) {
        const th_kwparam *keyword = th_func_kw(source_type, i);
        if (!keyword) collected = false;
        else core_list_push(call_keywords, *keyword);
    }
    th_type *call_signature = collected
        ? th_func_with_error(types, nil_type, nil_type, call_params.data,
            call_params.len, rest, call_keywords.data, call_keywords.len)
        : nullptr;
    collected = collected && call_signature
        && ir_instr_set_call_signature(call, call_signature);
    core_list_free(call_params);
    core_list_free(call_keywords);
    core_list_free(values);
    core_free(argument_by_parameter);
    core_free(value_by_parameter);
    core_free(provided);
    core_list_free(formals);
    if (!collected) {
        core_list_free(copyouts);
        return lower_failed_after(context, span, CORE_STRING_STATIC(
            "could not prepare the arguments or call for `__init`"));
    }
    for (size_t i = 0; i < copyouts.len; i++)
        if (!n00bb_emit_call_copyout(context, &copyouts.data[i], call->dest,
                                     span)) {
            core_list_free(copyouts);
            return lower_internal(context, span,
                "could not copy out constructor arguments");
    }
    core_list_free(copyouts);
    if (direct_assignment
        && !ir_lower_finish_assignment(context, direct_assignment,
                                       visit->node))
        return lower_internal(context, span,
            "could not complete direct constructor initialization");
    ir_value_t *constructed = object_in ? fresh_object
        : n00bb_emit_indirect_read(context, allocation_address, result_type,
                                   span);
    return constructed ? (ir_lower_result_t){IR_LOWER_VALUE, constructed}
        : lower_internal(context, span,
            "could not read the initialized object value");
}

ir_value_t *
n00bb_emit_outcome_payload(ir_lower_context_t *context, ir_value_t *outcome,
                          core_string_t arm, th_type *type, ir_span_t span)
{
    ir_instr_t *payload = ir_lower_emit_instruction(context,
        IR_OP_VARIANT_PAYLOAD, span);
    if (!payload || !ir_lower_set_temp_dest(context, payload,
                                            CORE_STRING_STATIC("__n00bb_outcome_"), type)
        || !ir_instr_operand_value(payload, outcome)
        || !ir_instr_operand_name(payload, arm))
        return nullptr;
    bool ok_arm = core_string_eq(arm, CORE_STRING_STATIC("ok"));
    if (ok_arm && ir_value_shape(outcome)
        && !ir_value_set_shape(payload->dest, ir_value_shape(outcome)))
        return nullptr;
    return payload->dest;
}

bool
n00bb_operator_error_text(ir_lower_context_t *context, ir_diagcode_t code,
                          ir_span_t span, core_string_t message)
{
    lower_error_text(context, code, span, message);
    return false;
}

bool
names_symbol_directly(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (parse_tree_is_direct_symbol_reference(node)) return true;
        if (core_tree_num_children(node) != 1) return false;
        node = core_tree_child(node, 0);
    }
    return false;
}

static core_diag_location_t
declared_callee_location(ir_lower_context_t *context, const ir_func_t *callee,
                         core_string_t name)
{
    core_translation_unit_t *unit =
        (core_translation_unit_t *)lower_unit(context);
    if (!unit) return (core_diag_location_t){0};
    if (callee->owner == context->module)
        return n00bb_name_location(unit, callee->span, name);
    core_buffer_t prefix = {0};
    core_buffer_puts(&prefix, "__ffi_");
    core_buffer_append_string(&prefix, ir_module_name(context->module));
    core_buffer_puts(&prefix, "_");
    bool generated = core_string_starts_with(ir_module_name(callee->owner),
                                             core_buffer_string(&prefix));
    core_buffer_release(&prefix);
    if (!generated) return (core_diag_location_t){0};
    core_buffer_t declaration = {0};
    core_buffer_puts(&declaration, "fn ");
    core_buffer_append_string(&declaration, name);
    core_string_t source = core_source_file_contents(&unit->source);
    ir_span_t whole = {
        .file = 0, .start_byte = 0, .end_byte = (uint32_t)source.u8_bytes,
        .start_line = 1, .start_col = 1,
    };
    core_diag_location_t at = n00bb_name_location(unit, whole,
        core_buffer_string(&declaration));
    core_buffer_release(&declaration);
    return at;
}

bool
n00bb_unwrap_call_outcome(ir_lower_context_t *context, parse_visit_t *visit,
                          ir_value_t *outcome, ir_span_t span,
                          ir_value_t **success, bool trap_unhandled)
{
    if (success) *success = nullptr;
    if (!outcome && ir_diags_failed(context->diagnostics)) return false;
    bool variant = false;
    th_type *definition = outcome
        ? ir_type_definition(context->module, outcome->type) : nullptr;
    bool complete = outcome
        && th_named_info(outcome->type, nullptr, &variant, nullptr)
        && variant && definition && th_kind_of(definition) == TH_RECORD;
    const ir_func_t *target = !complete && outcome
        ? ir_call_function(outcome->def) : nullptr;
    if (target
        && th_is_closed(ir_module_types(context->module), outcome->type)) {
        core_string_t name = n00bb_name_leaf(
            n00bb_function_root(target->name));
        lower_report(context, N00B_DIAG_PROPAGATE_INFALLIBLE, span,
                     N00B_TEXT_CANNOT_FAIL_SO_THIS_HAS,
                     N00BB_ARGS(ARG_TEXT(name)));
        core_diag_location_t declared = declared_callee_location(context,
            target, name);
        if (declared.has_location)
            n00bb_report_secondary(lower_diagnostics(context),
                                   lower_unit(context), declared,
                                   N00B_TEXT_IS_DECLARED_HERE,
                                   N00BB_ARGS(ARG_TEXT(name)));
        lower_help(context, N00B_TEXT_REMOVE_THE, N00BB_NO_ARGS);
        if (success) *success = outcome;
        return true;
    }
    n00bb_lower_context_t *unwrap_lower = context->user;
    parse_tree_t *operand = !complete && core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_sym_entry_t *infallible = operand && unwrap_lower
            && core_tree_num_children(operand)
        ? attached_symbol(visit->grammar, unwrap_lower->unit,
                          core_tree_child(operand, 0)) : nullptr;
    if (infallible && infallible->kind == PARSE_SYM_FUNCTION) {
        lower_report(context, N00B_DIAG_PROPAGATE_INFALLIBLE, span,
                     N00B_TEXT_CANNOT_FAIL_SO_THIS_HAS,
                     N00BB_ARGS(ARG_TEXT(infallible->name)));
        n00bb_diag_declared_here(lower_diagnostics(context), infallible,
                                 N00B_TEXT_IS_DECLARED_HERE);
        lower_help(context, N00B_TEXT_REMOVE_THE, N00BB_NO_ARGS);
        return false;
    }
    if (!complete && ir_diags_failed(context->diagnostics)) return false;
    if (!complete)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "failure propagation has no complete call outcome");
    th_type *ok_type = th_record_lookup(definition, CORE_STRING_STATIC("ok"));
    th_type *error_type = th_record_lookup(definition, CORE_STRING_STATIC("error"));
    if (!ok_type || !error_type)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "failure propagation has no complete call outcome");

    uint64_t block_id = context->next_value_id++;
    char ok_block_buffer[48], error_block_buffer[48];
    core_string_t ok_block_name = n00bb_format_name(ok_block_buffer,
        sizeof(ok_block_buffer), "__n00bb_unwrap_ok_%llu",
        (unsigned long long)block_id);
    core_string_t error_block_name = n00bb_format_name(error_block_buffer,
        sizeof(error_block_buffer), "__n00bb_unwrap_error_%llu",
        (unsigned long long)block_id);
    ir_block_t *ok_block = ir_block_add(context->function, ok_block_name);
    ir_block_t *error_block = ir_block_add(context->function,
                                           error_block_name);
    ir_instr_t *branch = ok_block && error_block
        ? n00bb_branch_on_outcome(context, outcome, ok_block, error_block,
                                  span)
        : nullptr;
    bool emitted = branch != nullptr;
    if (!emitted)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not branch on the call outcome");
    branch->span = span;

    context->block = error_block;
    ir_value_t *error = n00bb_emit_outcome_payload(
        context, outcome, CORE_STRING_STATIC("error"), error_type, span);
    if (!error || !n00bb_emit_with_failure(context, visit->node))
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not unwind active context managers");
    int routed = n00bb_route_try_error(context, visit->node, error, span);
    if (routed < 0) return false;
    if (!routed) {
        n00bb_lower_context_t *lower = context->user;
        bool literal_producer = lower && lower->has_active_literal;
        if (trap_unhandled || literal_producer) {
            if (literal_producer
                && th_builtin_kind_of(th_resolve(error_type))
                    == TH_BUILTIN_STRING
                && !n00bb_emit_panic(context, error, span))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not lower an unhandled literal failure");
            ir_instr_t *trap = ir_instr_add(context->block, IR_OP_TRAP);
            if (!trap || !ir_instr_operand_value(trap, error)
                || !n00bb_describe_failure(lower, trap,
                       N00BB_FAILURE_UNCAUGHT, (core_string_t){0},
                       n00bb_print_type(ir_module_types(context->module),
                                        error_type)))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not lower an unhandled call failure");
            trap->span = span;
            context->reachable = false;
        }
        else if (!n00bb_failure_channel_accepts(context, error_type)) {
            if (!report_uncaught_failure(context, span, error_type,
                                         N00B_TEXT_PHRASE_THIS_CALL))
                return false;
        }
        else {
            ir_instr_t *fail = ir_instr_add(context->block, IR_OP_FAIL);
            if (!fail || !ir_instr_operand_value(fail, error))
                return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
                    "could not propagate the call error");
            fail->span = span;
            context->reachable = false;
        }
    }

    context->block = ok_block;
    context->reachable = true;
    if (trap_unhandled
        && ir_machine_of(context->module, ok_type).class == IR_CLASS_VOID)
        return true;
    ir_value_t *value = n00bb_emit_outcome_payload(
        context, outcome, CORE_STRING_STATIC("ok"), ok_type, span);
    if (!value)
        return n00bb_operator_error(context, IR_DIAG_INTERNAL, span,
            "could not extract the successful call value");
    if (success) *success = value;
    return true;
}
