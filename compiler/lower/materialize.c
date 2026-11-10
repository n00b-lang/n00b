#include "lower_internal.h"

static ir_value_t *
n00bb_unwidened_operand(ir_lower_context_t *context, ir_operand_t operand,
                        th_type *target);
static ir_value_t *
n00bb_narrow_product(ir_lower_context_t *context, ir_value_t *value,
                     th_type *target, ir_span_t span);
static ir_value_t *
n00bb_typed_integer_value(ir_lower_context_t *context,
                          parse_tree_t *node, ir_value_t *value,
                          th_type *target, ir_span_t fallback);
static bool
n00bb_union_admits(ir_module_t *module, th_type *target, th_type *value);
static bool
tuple_widens(ir_module_t *module, th_type *from, th_type *to);

static ir_value_t *
n00bb_unwidened_operand(ir_lower_context_t *context, ir_operand_t operand,
                        th_type *target)
{
    ir_instr_t *widening = operand.kind == IR_OPD_VALUE && operand.value
        ? operand.value->def : nullptr;
    if (!widening || widening->op != IR_OP_CAST || widening->n_operands != 1
        || widening->operands[0].kind != IR_OPD_VALUE
        || ir_instr_has_mod(widening, CORE_STRING_STATIC("trap")))
        return nullptr;
    ir_value_t *original = widening->operands[0].value;
    return original && (ir_type_eq(context->module, original->type, target)
        || n00bb_integer_type_promotes_to(original->type, target))
        ? original : nullptr;
}

static ir_value_t *
n00bb_narrow_product(ir_lower_context_t *context, ir_value_t *value,
                     th_type *target, ir_span_t span)
{
    ir_instr_t *product = value->def;
    if (!product || product->op != IR_OP_MUL || product->n_operands != 2)
        return nullptr;
    ir_value_t *left = n00bb_unwidened_operand(context, product->operands[0],
                                               target);
    ir_value_t *right = n00bb_unwidened_operand(context,
                                                product->operands[1], target);
    if (!left || !right) return nullptr;
    left = n00bb_widen_integer_operand(context, left, target, span);
    right = n00bb_widen_integer_operand(context, right, target, span);
    ir_instr_t *narrow = left && right
        ? ir_lower_emit_instruction(context, IR_OP_MUL, span) : nullptr;
    if (!narrow || !ir_lower_set_temp_dest(context, narrow,
                       CORE_STRING_STATIC("__n00bb_"), target)
        || !ir_instr_operand_value(narrow, left)
        || !ir_instr_operand_value(narrow, right)
        || !ir_instr_add_mod(narrow, CORE_STRING_STATIC("trap")))
        return nullptr;
    ir_instr_t *left_widening = product->operands[0].value->def;
    ir_instr_t *right_widening = product->operands[1].value->def;
    ir_func_t *function = context->function;
    if (ir_value_has_uses(function, product->dest)) return narrow->dest;
    (void)ir_instr_erase(product);
    if (!ir_value_has_uses(function, left_widening->dest))
        (void)ir_instr_erase(left_widening);
    if (right_widening != left_widening
        && !ir_value_has_uses(function, right_widening->dest))
        (void)ir_instr_erase(right_widening);
    return narrow->dest;
}

static ir_value_t *
n00bb_typed_integer_value(ir_lower_context_t *context,
                          parse_tree_t *node, ir_value_t *value,
                          th_type *target, ir_span_t fallback)
{
    if (!value || !target || ir_type_eq(context->module, value->type, target))
        return value;
    ir_instr_t *constant = value->def;
    ir_span_t span = node ? lower_span_for_tree(context, node)
        : constant && ir_span_known(constant->span) ? constant->span
        : fallback;
    if (!constant || constant->op != IR_OP_CONST
        || constant->n_operands != 1
        || constant->operands[0].kind != IR_OPD_CONST
        || constant->operands[0].lit.is_float) {
        ir_machine_t source = ir_machine_of(context->module, value->type);
        ir_machine_t destination = ir_machine_of(context->module, target);
        if ((source.class != IR_CLASS_SINT
                && source.class != IR_CLASS_UINT)
            || (destination.class != IR_CLASS_SINT
                && destination.class != IR_CLASS_UINT)) return value;
        if (source.width > destination.width) {
            ir_value_t *narrowed = n00bb_narrow_product(context, value,
                                                        target, span);
            if (narrowed) return narrowed;
        }
        ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST,
                                                     span);
        if (!cast || !ir_lower_set_temp_dest(context, cast, CORE_STRING_STATIC("__n00bb_"),
                                             target)
            || !ir_instr_operand_value(cast, value)
            || (!n00bb_machine_promotes(source, destination)
                && !ir_instr_add_mod(cast, CORE_STRING_STATIC("trap"))))
            return nullptr;
        return cast->dest;
    }
    ir_machine_t machine = ir_machine_of(context->module, target);
    ir_machine_t source = ir_machine_of(context->module, value->type);
    ir_lit_t literal = constant->operands[0].lit.i;
    if ((source.class != IR_CLASS_SINT && source.class != IR_CLASS_UINT)
        || (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width > 128)
        return value;
    if (machine.class == IR_CLASS_UINT && literal.neg) return value;
    if (machine.width < 128) {
        ir_uint_t limit = (ir_uint_t)1 << (machine.width
            - (machine.class == IR_CLASS_SINT));
        if (literal.mag > limit
            || (literal.mag == limit
                && (machine.class == IR_CLASS_UINT || !literal.neg)))
            return value;
    }
    else if (machine.class == IR_CLASS_SINT) {
        ir_uint_t sign = (ir_uint_t)1 << 127;
        if (literal.mag > sign || (literal.mag == sign && !literal.neg))
            return value;
    }
    ir_instr_t *typed = ir_lower_emit_instruction(context, IR_OP_CONST, span);
    if (!typed || !ir_lower_set_temp_dest(context, typed, CORE_STRING_STATIC("__n00bb_"),
                                          target)
        || !ir_instr_operand_int_lit(typed, literal)) return nullptr;
    return typed->dest;
}

ir_value_t *
n00bb_typed_integer_literal(ir_lower_context_t *context,
                            parse_tree_t *node, th_type *target)
{
    ir_value_t *value = ir_lower_node_value(context, node);
    ir_value_t *typed = n00bb_typed_integer_value(context, node, value,
                                                  target,
                                                  lower_span_for_tree(context,
                                                                      node));
    if (!typed || typed == value) return typed;
    return ir_lower_set_node_value(context, node, typed) ? typed : nullptr;
}

ir_value_t *
n00bb_emit_wrapping(ir_lower_context_t *context, ir_op_t op,
                    ir_value_t *payload, th_type *target, ir_span_t span)
{
    ir_instr_t *instruction = ir_lower_emit_instruction(context, op, span);
    if (!instruction || !ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), target)
        || (payload && !ir_instr_operand_value(instruction, payload)))
        return nullptr;
    return instruction->dest;
}

static bool
n00bb_union_admits(ir_module_t *module, th_type *target, th_type *value)
{
    if (th_builtin_kind(ir_module_types(module), target) == TH_BUILTIN_ANY)
        return true;
    th_comb comb;
    if (!th_sum_comb(th_resolve(target), &comb) || comb != TH_UNION)
        return false;
    for (size_t i = 0; i < th_sum_len(target); i++)
        if (ir_type_eq(module, value, th_sum_arm(target, i))) return true;
    return false;
}

static bool
tuple_widens(ir_module_t *module, th_type *from, th_type *to)
{
    if (ir_type_eq(module, from, to)) return true;
    if (is_integer_type(from)) return n00bb_integer_type_promotes_to(from, to);
    from = th_resolve(from);
    to = th_resolve(to);
    size_t count = th_named_nargs(from);
    if (!th_builtin_applied(from, TH_BUILTIN_TUPLE, count)
        || !th_builtin_applied(to, TH_BUILTIN_TUPLE, count)) return false;
    for (size_t i = 0; i < count; i++) {
        const th_arg *left = th_named_arg(from, i);
        const th_arg *right = th_named_arg(to, i);
        if (!left || !right || left->kind != TH_ARG_TYPE
            || right->kind != TH_ARG_TYPE
            || !tuple_widens(module, left->type, right->type)) return false;
    }
    return true;
}

ir_value_t *
widened_value(ir_lower_context_t *context, ir_value_t *value, th_type *target,
              ir_span_t span)
{
    if (ir_type_eq(context->module, value->type, target)) return value;
    if (is_integer_type(value->type))
        return n00bb_widen_integer_operand(context, value, target, span);
    th_type *from = th_resolve(value->type);
    th_type *to = th_resolve(target);
    size_t count = th_named_nargs(from);
    ir_value_t **items = core_alloc_array(ir_value_t *, count + 1);
    bool ok = items != nullptr;
    for (size_t i = 0; ok && i < count; i++) {
        ir_instr_t *extract = ir_lower_emit_instruction(context,
            IR_OP_EXTRACT_INDEX, span);
        ok = extract && ir_lower_set_temp_dest(context, extract,
                CORE_STRING_STATIC("__n00bb_"), th_named_arg(from, i)->type)
            && ir_instr_operand_value(extract, value)
            && ir_instr_operand_uint(extract, i);
        items[i] = ok ? widened_value(context, extract->dest,
                                      th_named_arg(to, i)->type, span)
                      : nullptr;
        ok = items[i] != nullptr;
    }
    ir_instr_t *aggregate = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
                                             CORE_STRING_STATIC("__n00bb_"),
                                             target);
    for (size_t i = 0; ok && i < count; i++)
        ok = ir_instr_operand_value(aggregate, items[i]);
    core_free(items);
    return ok ? aggregate->dest : nullptr;
}

static ir_value_t *
view_of_reference(ir_lower_context_t *context, ir_value_t *reference,
                  th_type *target, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *fields = th_fields_record(types, target);
    th_type *items = fields
        ? th_record_lookup(fields, CORE_STRING_STATIC("items")) : nullptr;
    th_type *count = fields
        ? th_record_lookup(fields, CORE_STRING_STATIC("count")) : nullptr;
    ir_value_t *none = items
        ? n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr, items, span)
        : nullptr;
    ir_instr_t *start = count ? n00bb_emit_uint_constant(context, count, 0,
        span, CORE_STRING_STATIC("__n00bb_view_")) : nullptr;
    ir_instr_t *one = count ? n00bb_emit_uint_constant(context, count, 1,
        span, CORE_STRING_STATIC("__n00bb_view_")) : nullptr;
    ir_instr_t *aggregate = none && start && one
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    return aggregate && ir_lower_set_temp_dest(context, aggregate,
            CORE_STRING_STATIC("__n00bb_view_"), target)
        && ir_instr_operand_value(aggregate, reference)
        && ir_instr_operand_value(aggregate, none)
        && ir_instr_operand_value(aggregate, start->dest)
        && ir_instr_operand_value(aggregate, one->dest)
        ? aggregate->dest : nullptr;
}

ir_value_t *
n00bb_materialize(ir_lower_context_t *context, parse_tree_t *node,
                  ir_value_t *value, th_type *target, ir_span_t span)
{
    if (!value || !target || ir_type_eq(context->module, value->type, target))
        return value;
    th_ctx *types = ir_module_types(context->module);
    n00bb_lower_context_t *solved = context->user;
    if ((!solved || !solved->solved)
        && (!th_is_closed(types, value->type) || !th_is_closed(types, target)))
        return value;
    if (n00bb_value_coerces(types, value->type, target)) {
        return n00bb_emit_wrapping(context, IR_OP_BUFFER_AS_STRING, value,
                                   target, span);
    }
    if (n00bb_reference_views(value->type, target))
        return view_of_reference(context, value, target, span);
    if (n00bb_string_where_buffer_expected(types, target, value->type)) {
        n00bb_lower_context_t *lower = context->user;
        parse_tree_t *site = node && lower
            ? n00bb_string_where_buffer_site(lower->grammar, types, node,
                                             target) : nullptr;
        core_string_t text = site
            ? source_text_of(lower ? lower->unit : nullptr, site)
            : (core_string_t){0};
        lower_report(context, N00B_DIAG_STRING_NOT_BUFFER,
            site ? lower_span_for_tree(context, site) : span,
            N00B_TEXT_IS_A_STRING_WHICH_IS,
            N00BB_ARGS(ARG_TEXT(text.u8_bytes ? text
                                : n00bb_text_form(N00B_TEXT_THIS_VALUE))));
        lower_help(context, N00B_TEXT_USE_BUFFER_FROM_STRING,
            N00BB_ARGS(ARG_TEXT(text.u8_bytes ? text
                                : CORE_STRING_STATIC("s"))));
        return nullptr;
    }
    if (!th_is_closed(types, target) && th_is_closed(types, value->type)
        && th_compatible(target, value->type))
        return value;
    th_type *atomic_element = n00bb_atomic_element_type(target);
    if (atomic_element && !n00bb_atomic_element_type(value->type)) {
        ir_value_t *inner = n00bb_materialize(context, node, value,
                                              atomic_element, span);
        if (!inner || !ir_type_eq(context->module, inner->type,
                                  atomic_element)) return inner;
        return n00bb_emit_atomic_wrap(context, inner, target, span);
    }
    th_type *option_element = th_option_item(target);
    if (option_element) {
        if (th_is_nil(value->type))
            return n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr,
                                       target, span);
        ir_value_t *inner = n00bb_materialize(context, node, value,
                                              option_element, span);
        if (!inner || (!ir_type_eq(context->module, inner->type,
                                   option_element)
                       && th_is_closed(types, option_element))) return inner;
        return n00bb_emit_wrapping(context, IR_OP_OPTION_SOME, inner,
            th_is_closed(types, option_element) ? target
                : th_option(types, inner->type), span);
    }
    ir_value_t *integer = n00bb_typed_integer_value(context, node, value,
                                                    target, span);
    if (integer != value) return integer;
    if (n00bb_union_admits(context->module, target, value->type))
        return n00bb_emit_wrapping(context, IR_OP_BOX, value, target, span);
    if (tuple_widens(context->module, value->type, target))
        return widened_value(context, value, target, span);
    th_comb comb;
    if (th_sum_comb(th_resolve(target), &comb) && comb == TH_UNION)
        for (size_t i = 0; i < th_sum_len(target); i++) {
            th_type *arm = th_sum_arm(target, i);
            if (!tuple_widens(context->module, value->type, arm)) continue;
            ir_value_t *widened = widened_value(context, value, arm, span);
            return widened ? n00bb_emit_wrapping(context, IR_OP_BOX, widened,
                                                 target, span) : nullptr;
        }
    return value;
}

n00bb_out_location_t
n00bb_out_argument_location(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *argument)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_out_location_t location = {0};
    location.index_target = n00bb_index_expression_node(grammar, argument);
    parse_tree_t *member_target = location.index_target ? nullptr
        : member_access_node(grammar, argument);
    parse_sym_entry_t *member_symbol = member_target
        ? attached_symbol(grammar, lower->unit, argument) : nullptr;
    parse_tree_t *field_target = !member_symbol || member_symbol->is_field
        ? member_target : nullptr;
    bool module_target = member_target && member_symbol
        && !member_symbol->is_field;
    location.lvalue = !location.index_target && !member_target
        ? n00bb_simple_lvalue(argument) : nullptr;
    location.symbol = location.index_target
        ? n00bb_assignment_root_symbol(grammar, lower->unit,
                                       location.index_target)
        : location.lvalue
        ? attached_symbol(grammar, lower->unit, location.lvalue)
        : field_target ? n00bb_assignment_root_symbol(grammar, lower->unit,
                                                      field_target)
        : module_target ? member_symbol : nullptr;
    location.local = location.lvalue
        ? ir_lower_bound_local(context, location.symbol) : nullptr;
    location.global = (location.lvalue || module_target) && !location.local
            && !location.address
        ? ir_lower_bound_value(context, location.symbol) : nullptr;
    if (member_target) {
        lower->reference_write = true;
        location.address = n00bb_addressable_expression_address(context,
            grammar, lower->unit, argument,
            lower_span_for_tree(context, argument));
        lower->reference_write = false;
    }
    location.type = location.index_target
        ? n00bb_node_type(context, location.index_target)
        : location.address ? th_ind_referent(location.address->type)
        : location.local ? location.local->type
        : location.global && location.global->kind == IR_VAL_GLOBAL
            ? location.global->type : nullptr;
    return location;
}

ir_value_t *
n00bb_out_location_read(ir_lower_context_t *context, parse_tree_t *argument,
                        const n00bb_out_location_t *location, ir_span_t span)
{
    return location->index_target ? ir_lower_node_value(context, argument)
        : location->address ? n00bb_emit_indirect_read(context,
            location->address, location->type, span)
        : location->local ? n00bb_emit_local_read(context, location->local,
                                                  span)
        : location->global;
}
