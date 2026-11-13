#include "lower_internal.h"

static ir_lower_result_t
n00bb_lower_referent_read(ir_lower_context_t *context, parse_visit_t *visit,
                          th_type *result_type, ir_span_t span);
static ir_lower_result_t
n00bb_lower_tuple_field(ir_lower_context_t *context, parse_visit_t *visit,
                        th_type *result_type, ir_span_t span);

static ir_lower_result_t
n00bb_lower_referent_read(ir_lower_context_t *context, parse_visit_t *visit,
                          th_type *result_type, ir_span_t span)
{
    if (n00bb_simple_assignment_field_target(visit->grammar, visit->node)
        || n00bb_reference_target(visit->grammar, visit->node))
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    n00bb_lower_context_t *lower = context->user;
    ir_value_t *address = lower ? n00bb_record_field_address(context,
        visit->grammar, lower->unit, visit->node, span) : nullptr;
    if (!address)
        return lower_report(context, N00B_DIAG_VALUE_NEEDS_REFERENCE, span,
            N00B_TEXT_VALUE_READS_THROUGH_A_REFERENCE,
            N00BB_NO_ARGS);
    ir_instr_t *load = ir_lower_emit_instruction(context, IR_OP_LOAD, span);
    if (!load || !ir_lower_set_temp_dest(context, load,
            CORE_STRING_STATIC("__n00bb_"), result_type)
        || !ir_instr_operand_value(load, address)
        || !n00bb_set_reference_access(lower->ir_context, load->dest,
                                       result_type))
        return lower_internal(context, span,
                           "could not load through a reference");
    return (ir_lower_result_t){IR_LOWER_VALUE, load->dest};
}

static ir_lower_result_t
n00bb_lower_tuple_field(ir_lower_context_t *context, parse_visit_t *visit,
                        th_type *result_type, ir_span_t span)
{
    if (n00bb_simple_assignment_field_target(visit->grammar, visit->node))
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    parse_tree_t *parent = parse_tree_parent(visit->node);
    if (parent && core_tree_num_children(parent) > 1
        && core_tree_child(parent, 0) == visit->node
        && parse_tree_leaf_is(core_tree_child(parent, 1),
                              CORE_STRING_STATIC("(")))
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    parse_tree_t *base_node = core_tree_child(visit->node, 0);
    ir_value_t *base = ir_lower_node_value(context, base_node);
    th_type *record = base ? th_resolve(base->type) : nullptr;
    core_string_t name = parse_tree_leaf_text(core_tree_child(visit->node, 2));
    if (!record || (th_kind_of(record) == TH_RECORD
                    && !record_lookup_text(nullptr, record, name)))
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    ir_value_t *field = n00bb_extract_step(context, base,
        (n00bb_projection_step_t){.name = name}, result_type, span);
    return field ? (ir_lower_result_t){IR_LOWER_VALUE, field}
                 : lower_internal(context, span,
                                  "could not emit a tuple field read");
}

static bool
called_or_referenced(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *parent = parse_tree_parent(node);
    while (parent && core_tree_num_children(parent) == 1) {
        node = parent;
        parent = parse_tree_parent(node);
    }
    const parse_rule_t *rule = parent ? parse_tree_rule(grammar, parent)
                                      : nullptr;
    if (!rule) return false;
    parse_annotation_t *operator = parse_rule_find_annot(grammar, rule,
        PARSE_ANNOT_OPERATOR);
    if (operator && core_string_eq(operator->op_kind,
                                   CORE_STRING_STATIC("ref")))
        return true;
    return parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)
        && core_tree_num_children(parent)
        && core_tree_child(parent, 0) == node;
}

ir_lower_result_t
n00bb_lower_module_member(ir_lower_context_t *context, parse_visit_t *visit,
                          ir_value_t *const *operands, size_t operand_count,
                          th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    if (operand_count || !result_type)
        return lower_internal(context, span,
                              "module member has no type");
    if (referent_access_node(visit->grammar, visit->node))
        return n00bb_lower_referent_read(context, visit, result_type, span);
    parse_node_resolution_t resolution = parse_tree_resolution(visit->node);
    parse_sym_entry_t *named = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
            && resolution.target.symbol
        ? canonical_symbol(resolution.target.symbol) : nullptr;
    if (named && named->kind == PARSE_SYM_FUNCTION
        && !called_or_referenced(visit->grammar, visit->node)) {
        lower_report(context, N00B_DIAG_FUNCTION_NAME_AS_VALUE, span,
                     N00B_TEXT_FUNCTION_NAME_AS_VALUE,
                     N00BB_ARGS(ARG_TEXT(named->name)));
        lower_help(context, N00B_TEXT_WRITE_REF_FUNCTION,
                   N00BB_ARGS(ARG_TEXT(named->name)));
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    __int128 folded = 0;
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && n00bb_untyped_integer_const(visit->grammar,
                                       resolution.target.symbol, &folded))
        return n00bb_emit_untyped_const(context, folded, span);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && resolution.target.symbol
        && resolution.target.symbol->kind == PARSE_SYM_ENUM_CONST) {
        parse_sym_entry_t *member = resolution.target.symbol;
        n00bb_lower_context_t *lower = context->user;
        if (lower && n00bb_flagset_instance_access(visit->grammar,
                lower->unit, visit->node, member)) {
            parse_tree_t *base_node = core_tree_child(visit->node, 0);
            ir_value_t *base = ir_lower_node_value(context, base_node);
            parse_sym_entry_t *base_symbol = attached_symbol(
                visit->grammar, lower->unit, base_node);
            if (!base && base_symbol)
                base = ir_lower_bound_value(context, base_symbol);
            n00bb_flagset_value_t *value = member->has_comptime_value
                ? member->comptime_value : nullptr;
            ir_instr_t *mask = value
                ? ir_lower_emit_instruction(context, IR_OP_BITSET_CONST, span)
                : nullptr;
            ir_instr_t *slice = mask
                ? ir_lower_emit_instruction(context,
                                            IR_OP_BITSET_INTERSECTION, span)
                : nullptr;
            if (!base || !value || !mask || !slice
                || !ir_lower_set_temp_dest(context, mask, CORE_STRING_STATIC("__n00bb_"),
                    member->type)
                || !ir_instr_operand_name(mask, member->name)
                || !ir_instr_operand_uint(mask, value->mask)
                || !ir_lower_set_temp_dest(context, slice,
                    CORE_STRING_STATIC("__n00bb_"), member->type)
                || !ir_instr_operand_value(slice, base)
                || !ir_instr_operand_value(slice, mask->dest))
                return lower_internal(context, span,
                                   "could not emit a flagset slice");
            return (ir_lower_result_t){IR_LOWER_VALUE, slice->dest};
        }
        if (n00bb_flagset_member(member)) {
            n00bb_flagset_value_t *value = member->has_comptime_value
                ? member->comptime_value : nullptr;
            if (!value || !member->type)
                return lower_internal(context, span,
                    "flagset constant has no matching resolved mask and type");
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                IR_OP_BITSET_CONST, span);
            if (!constant || !ir_lower_set_temp_dest(context, constant,
                CORE_STRING_STATIC("__n00bb_"), member->type)
                || !ir_instr_operand_name(constant, member->name)
                || !ir_instr_operand_uint(constant, value->mask))
                return lower_internal(context, span,
                    "could not emit the flagset constant");
            return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
        }
        n00bb_enum_value_t *value = member->has_comptime_value
            ? member->comptime_value : nullptr;
        if (!value || !member->type)
            return lower_internal(context, span,
                "enum constant has no matching resolved value and type");
        ir_instr_t *constant = ir_lower_emit_instruction(context,
            IR_OP_ENUM_CONST, span);
        if (!constant || !ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_"), member->type)
            || !ir_instr_operand_name(constant, member->name)
            || !ir_instr_operand_int_lit(constant,
                (ir_lit_t){.mag = value->magnitude, .neg = value->negative}))
            return lower_internal(context, span,
                "could not emit the enum constant");
        return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
    }
    if (resolution.kind == PARSE_NODE_RESOLUTION_NONE
        && core_tree_num_children(visit->node) == 3)
        return n00bb_lower_tuple_field(context, visit, result_type, span);
    if (resolution.kind != PARSE_NODE_RESOLUTION_SYMBOL
        || !resolution.target.symbol
        || resolution.target.symbol->kind != PARSE_SYM_VARIABLE)
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (core_tree_num_children(visit->node) != 3)
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (n00bb_variant_arm_symbol(visit->grammar, resolution.target.symbol)
        && n00bb_assignment_target(visit->grammar, visit->node, nullptr))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (n00bb_reference_target(visit->grammar, visit->node))
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    if (th_option_item(resolution.target.symbol->type)
        && th_kind_of(th_resolve(result_type)) == TH_FUNCTION)
        return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *member = resolution.target.symbol;
    if (member->is_field) {
        if (n00bb_simple_assignment_field_target(visit->grammar,
                                                 visit->node))
            return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
        parse_tree_t *base_node = core_tree_num_children(visit->node)
            ? core_tree_child(visit->node, 0) : nullptr;
        if (base_node && referent_access_node(visit->grammar, base_node)) {
            ir_value_t *address = n00bb_record_field_address(context,
                visit->grammar, lower->unit, visit->node, span);
            ir_instr_t *load = address
                ? ir_lower_emit_instruction(context, IR_OP_LOAD, span)
                : nullptr;
            if (!load || !ir_lower_set_temp_dest(context, load,
                CORE_STRING_STATIC("__n00bb_"), result_type)
                || !ir_instr_operand_value(load, address)
                || !n00bb_set_reference_access(lower->ir_context,
                                               load->dest, result_type))
                return lower_internal(context, span,
                    "could not load a field through a reference");
            return (ir_lower_result_t){IR_LOWER_VALUE, load->dest};
        }
        ir_value_t *base = base_node
            ? ir_lower_node_value(context, base_node) : nullptr;
        parse_sym_entry_t *base_symbol = base_node && lower
            ? attached_symbol(visit->grammar, lower->unit, base_node) : nullptr;
        if (!base && base_symbol)
            base = ir_lower_bound_value(context, base_symbol);
        if (!base) return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
        if (th_kind_of(th_resolve(base->type)) == TH_INDIRECTION
            || n00bb_reference_definition(ir_module_types(context->module),
                                          base->type)) {
            ir_value_t *address = n00bb_record_field_address(context,
                visit->grammar, lower->unit, visit->node, span);
            ir_instr_t *load = address
                ? ir_lower_emit_instruction(context, IR_OP_LOAD, span)
                : nullptr;
            if (!load || !ir_lower_set_temp_dest(context, load,
                CORE_STRING_STATIC("__n00bb_"), result_type)
                || !ir_instr_operand_value(load, address)
                || !n00bb_set_reference_access(lower->ir_context,
                                               load->dest, result_type))
                return lower_internal(context, span,
                    "could not load a referenced record field");
            return (ir_lower_result_t){IR_LOWER_VALUE, load->dest};
        }
        ir_instr_t *extract = ir_lower_emit_instruction(context, IR_OP_EXTRACT,
                                                        span);
        if (!extract || !ir_lower_set_temp_dest(context, extract,
            CORE_STRING_STATIC("__n00bb_"), result_type)
            || !ir_instr_operand_value(extract, base)
            || !ir_instr_operand_name(extract, member->name)
            || !n00bb_set_reference_access(lower->ir_context,
                                           extract->dest, result_type))
            return lower_internal(context, span,
                               "could not emit a record field read");
        return (ir_lower_result_t){IR_LOWER_VALUE, extract->dest};
    }
    ir_value_t *binding = ir_lower_bound_value(context,
                                               resolution.target.symbol);
    if (!binding) {
        th_ctx *types = ir_ctx_types(lower->ir_context);
        core_buffer_t message = {0};
        core_string_t node_text = n00bb_print_type(types, result_type);
        core_string_t binding_text = binding
            ? n00bb_print_type(types, binding->type)
            : CORE_STRING_STATIC("<missing>");
        core_buffer_printf(&message,
            "module variable has no matching canonical storage (use: %.*s, storage: %.*s)",
            (int)node_text.u8_bytes, node_text.data,
            (int)binding_text.u8_bytes, binding_text.data);
        ir_lower_result_t failed = lower_internal_string(context, span,
            core_buffer_string(&message));
        core_buffer_release(&message);
        return failed;
    }

    ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!copy || !ir_lower_set_temp_dest(context, copy, CORE_STRING_STATIC("__n00bb_"),
                                         result_type)
        || !ir_instr_operand_value(copy, binding))
        return lower_internal(context, span,
                           "could not emit a module variable read");
    return (ir_lower_result_t){IR_LOWER_VALUE, copy->dest};
}

ir_func_t *
n00bb_record_field_method(ir_lower_context_t *context,
                          const th_env_method_t *selected,
                          ir_module_t **owner_module)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    core_link_declaration_t *declaration = state && state->link
        ? linked_method_declaration(state->link, selected) : nullptr;
    parse_sym_entry_t *symbol = initializer_declaration_symbol(declaration);
    core_translation_unit_t *owner = symbol && analysis
        ? unit_for_symbol(analysis->grammar, &analysis->units, symbol)
        : nullptr;
    n00bb_tu_state_t *owner_state = owner ? tu_state(owner) : nullptr;
    if (owner_module) *owner_module = owner_state
        ? unit_module(owner_state) : nullptr;
    ir_func_t *function = symbol && analysis
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    return function;
}

ir_value_t *
n00bb_record_field_call(ir_lower_context_t *context, ir_block_t *block,
                        const th_env_method_t *selected,
                        ir_value_t *left, ir_value_t *right,
                        th_type *result_type, core_string_t name)
{
    ir_module_t *owner = nullptr;
    ir_func_t *function = n00bb_record_field_method(context, selected,
                                                     &owner);
    ir_value_t *callee = owner && function
        ? ir_func_address(owner, function) : nullptr;
    th_type *signature = callee ? selected->signature : nullptr;
    ir_instr_t *call = signature ? ir_instr_add(block, IR_OP_CALL) : nullptr;
    bool ok = call && ir_instr_set_dest(call, name, result_type)
        && ir_instr_operand_value(call, callee)
        && ir_instr_operand_value(call, left)
        && (!right || ir_instr_operand_value(call, right))
        && ir_instr_set_call_signature(call, signature);
    return ok ? call->dest : nullptr;
}
