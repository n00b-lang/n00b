#include "lower_internal.h"

ir_instr_t *
n00bb_emit_checked_product(ir_lower_context_t *context, ir_value_t *left,
                           ir_value_t *right, th_type *uint_type,
                           ir_span_t span)
{
    ir_instr_t *product = ir_lower_emit_instruction(context, IR_OP_MUL, span);
    if (!product || !ir_lower_set_temp_dest(context, product,
                                            CORE_STRING_STATIC("__n00bb_array_product_"),
                                            uint_type)
        || !ir_instr_operand_value(product, left)
        || !ir_instr_operand_value(product, right)
        || !ir_instr_add_mod(product, CORE_STRING_STATIC("trap"))) return nullptr;
    return product;
}

ir_value_t *
n00bb_coerce_array_dimension_value(ir_lower_context_t *context,
                                   parse_tree_t *expression,
                                   ir_value_t *value, th_type *uint_type,
                                   ir_value_t *one, ir_span_t span,
                                   bool require_positive)
{
    (void)expression;
    th_type *source_type = value ? th_resolve(value->type) : nullptr;
    bool open = source_type
        && !th_is_closed(ir_module_types(context->module), source_type);
    if (!value || (!open && !is_integer_type(source_type))) return nullptr;
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST, span);
    if (open) n00bb_rewrite_later(context, cast);
    ir_machine_t source_machine = open ? (ir_machine_t){0}
        : ir_machine_of(context->module, source_type);
    ir_machine_t target_machine = ir_machine_of(context->module, uint_type);
    bool needs_fit = !open
        && (source_machine.width > target_machine.width
            || source_machine.class != target_machine.class);
    if (!cast || !ir_lower_set_temp_dest(context, cast,
                                         CORE_STRING_STATIC("__n00bb_array_extent_"), uint_type)
        || !ir_instr_operand_value(cast, value)
        || (needs_fit && !ir_instr_add_mod(cast, CORE_STRING_STATIC("trap")))) return nullptr;
    if (!require_positive) return cast->dest;
    ir_instr_t *check = ir_lower_emit_instruction(context, IR_OP_SUB, span);
    if (!check || !ir_lower_set_temp_dest(context, check,
                                          CORE_STRING_STATIC("__n00bb_array_positive_"),
                                          uint_type)
        || !ir_instr_operand_value(check, cast->dest)
        || !ir_instr_operand_value(check, one)
        || !ir_instr_add_mod(check, CORE_STRING_STATIC("trap"))) return nullptr;
    return cast->dest;
}

ir_value_t *
n00bb_lower_array_dimension_value(ir_lower_context_t *context,
                                  parse_tree_t *expression,
                                  th_type *uint_type, ir_value_t *one,
                                  ir_span_t span, bool require_positive)
{
    return n00bb_coerce_array_dimension_value(context, expression,
        ir_lower_node_value(context, expression), uint_type, one, span,
        require_positive);
}

ir_value_t *
n00bb_emit_array_allocation(ir_lower_context_t *context, th_type *leaf,
                            uint32_t rank, ir_shape_extent_t *extents,
                            th_type *result_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    ir_instr_t *one = uint_type
        ? n00bb_emit_uint_constant(context, uint_type, 1, span,
                                   CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
    if (!lower || !lower->ir_context || !one) return nullptr;
    for (uint32_t i = 0; i < rank; i++) {
        if (extents[i].value || extents[i].kind != IR_SHAPE_EXTENT_STATIC)
            continue;
        ir_instr_t *constant = n00bb_emit_uint_constant(context, uint_type,
            extents[i].static_value, span, CORE_STRING_STATIC("__n00bb_array_extent"));
        if (!constant) return nullptr;
        extents[i].value = constant->dest;
    }
    uint32_t known_prefix = 0;
    while (known_prefix < rank && extents[known_prefix].value)
        known_prefix++;
    for (uint32_t i = known_prefix; i < rank; i++)
        if (extents[i].value) return nullptr;
    ir_shape_t *shape = ir_shape_new(lower->ir_context, leaf, rank);
    bool ok = shape != nullptr;
    for (uint32_t i = 0; ok && i < rank; i++)
        ok = ir_shape_set_extent(shape, i, extents[i]);
    ok = ok && ir_shape_set_storage(shape, known_prefix, true);
    ir_value_t *count = one->dest;
    for (uint32_t i = 0; ok && i < known_prefix; i++) {
        ir_instr_t *product = n00bb_emit_checked_product(context, count,
            extents[i].value, uint_type, span);
        if (!product) ok = false;
        else count = product->dest;
    }
    ok = ok && ir_shape_set_element_count(shape, (ir_shape_measure_t){
        .kind = IR_SHAPE_MEASURE_RUNTIME,
        .value = count,
    });
    ir_value_t *stride = one->dest;
    for (uint32_t i = known_prefix; ok && i > 0; i--) {
        uint32_t dimension = i - 1;
        ok = ir_shape_set_stride(shape, dimension,
            (ir_shape_measure_t){.kind = IR_SHAPE_MEASURE_RUNTIME,
                                  .value = stride});
        if (ok && dimension > 0) {
            ir_instr_t *product = n00bb_emit_checked_product(context, stride,
                extents[dimension].value, uint_type, span);
            if (!product) ok = false;
            else stride = product->dest;
        }
    }
    ir_instr_t *instruction = ok
        ? ir_lower_emit_instruction(context, IR_OP_ARRAY_NEW, span) : nullptr;
    ok = ok && instruction
        && ir_lower_set_temp_dest(context, instruction,
                                  CORE_STRING_STATIC("__n00bb_array_new_"), result_type)
        && ir_instr_set_array_init(lower->ir_context, instruction,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO);
    for (uint32_t i = 0; ok && i < known_prefix; i++)
        ok = ir_instr_operand_value(instruction, extents[i].value);
    ok = ok && ir_value_set_shape(instruction->dest, shape);
    return ok ? instruction->dest : nullptr;
}

ir_lower_result_t
n00bb_lower_array_new(ir_lower_context_t *context, parse_visit_t *visit,
                      ir_value_t *const *operands, size_t operand_count,
                      th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    th_type *declared = parse_tree_type(child_nt(visit->grammar, visit->node,
        CORE_STRING_STATIC("type-spec")));
    if (declared) result_type = declared;
    if (!result_type)
        return lower_internal(context, span,
                              "`new` has no resolved result type");
    th_type *resolved_type = th_resolve(result_type);
    bool is_variant = false;
    if (th_named_info(resolved_type, nullptr, &is_variant, nullptr)
        && is_variant) {
        parse_tree_t *call_args = child_nt(visit->grammar, visit->node,
                                           CORE_STRING_STATIC("call-args"));
        core_list_t(n00bb_tree_ptr_t) positional =
            core_list_new(n00bb_tree_ptr_t);
        collect_positional_nodes(visit->grammar,
            child_nt(visit->grammar, call_args, CORE_STRING_STATIC("positional-args")),
            &positional);
        bool has_keywords = child_nt(visit->grammar, call_args,
                                     CORE_STRING_STATIC("keyword-args")) != nullptr;
        if (positional.len || has_keywords) {
            core_list_free(positional);
        {
            core_string_t type_text = lower_type_text(context, result_type);
            ir_lower_result_t result = lower_report(context,
                N00B_DIAG_NEW_VARIANT_ARGUMENTS, span,
                N00B_TEXT_NEW_FOR_VARIANT_TAKES_NO,
                N00BB_ARGS(ARG_TEXT(type_text)));
            lower_help(context, N00B_TEXT_CREATE_IT_WITH_NEW_THEN,
                N00BB_NO_ARGS);
            core_free(type_text.data);
            return result;
        }
        }
        core_list_free(positional);
        ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                                             IR_OP_VARIANT_NEW,
                                                             span);
        if (!instruction
            || !ir_lower_set_temp_dest(context, instruction,
                CORE_STRING_STATIC("__n00bb_variant_new_"), resolved_type))
            return lower_internal(context, span,
                "could not emit unset variant storage");
        return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
    }
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *caller = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *constructor_site = caller
        ? link_use_site_for_node(caller, visit->node) : nullptr;
    if (caller && (!constructor_site || !constructor_site->symbol
            || constructor_site->symbol->kind != PARSE_SYM_FUNCTION)
        && n00bb_flagset_type(lower->unit, caller, resolved_type, nullptr)) {
        ir_instr_t *constant = ir_lower_emit_instruction(context,
                                                          IR_OP_BITSET_CONST,
                                                          span);
        if (!constant || !ir_lower_set_temp_dest(context, constant,
                                                 CORE_STRING_STATIC("__n00bb_"), result_type)
            || !ir_instr_operand_name(constant, CORE_STRING_STATIC("nil"))
            || !ir_instr_operand_uint(constant, 0))
            return lower_internal(context, span,
                               "could not emit default flagset construction");
        return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
    }
    if (th_kind_of(resolved_type) != TH_SEQUENCE)
        return n00bb_lower_constructor_new(context, visit, result_type, span);

    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_analysis_t *analysis = tu ? tu->link_analysis : nullptr;
    if (!lower || !tu || !analysis)
        return lower_internal(context, span,
                           "array construction has no LinkUnit analysis");
    parse_tree_t *type_node = child_nt(visit->grammar, visit->node,
                                       CORE_STRING_STATIC("type-spec"));
    core_list_t(n00bb_tree_ptr_t) type_dimensions =
        core_list_new(n00bb_tree_ptr_t);
    bool type_shape_ok = !type_node
        || collect_array_dimension_nodes(visit->grammar, tu, type_node,
                                         &type_dimensions);
    if (!type_shape_ok) {
        core_list_free(type_dimensions);
        return lower_internal(context, span,
                              "could not determine the array type dimensions");
    }
    uint32_t rank = 0;
    th_type *leaf = th_resolve(result_type);
    while (th_kind_of(leaf) == TH_SEQUENCE) {
        leaf = th_resolve(th_seq_elem(leaf));
        if (!leaf || rank == UINT32_MAX) {
            core_list_free(type_dimensions);
            return lower_internal(context, span,
                                  "`new` has an invalid array type");
        }
        rank++;
    }
    parse_tree_t *call_args = child_nt(visit->grammar, visit->node,
                                       CORE_STRING_STATIC("call-args"));
    if (child_nt(visit->grammar, call_args, CORE_STRING_STATIC("keyword-args"))) {
        core_list_free(type_dimensions);
        return lower_report(context, N00B_DIAG_ARRAY_DIMENSIONS, span,
            N00B_TEXT_ARRAY_DIMENSIONS_GIVEN_TO_NEW,
            N00BB_NO_ARGS);
    }
    core_list_t(n00bb_tree_ptr_t) arguments =
        core_list_new(n00bb_tree_ptr_t);
    collect_positional_nodes(visit->grammar,
        child_nt(visit->grammar, call_args, CORE_STRING_STATIC("positional-args")),
        &arguments);
    size_t argument_offset = 0;
    size_t dimension_argument_count = arguments.len - argument_offset;
    if (type_dimensions.len > rank) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_internal(context, span,
                              "array type shape exceeds its sequence rank");
    }
    uint32_t unsized_count = rank;
    for (size_t i = 0; i < type_dimensions.len; i++)
        if (child_nt(visit->grammar, type_dimensions.data[i],
                     CORE_STRING_STATIC("expression"))) unsized_count--;
    if (dimension_argument_count > unsized_count
        || (!dimension_argument_count && unsized_count)) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
    {
        core_string_t type_text = lower_type_text(context, result_type);
        ir_lower_result_t result = lower_report(context,
            N00B_DIAG_ARRAY_DIMENSIONS, span,
            N00B_TEXT_NEW_DIMENSIONS_WITHOUT_SIZE,
            N00BB_ARGS(ARG_NUMBER(unsized_count), ARG_TEXT(type_text),
                       ARG_NUMBER(dimension_argument_count)));
        lower_help(context, N00B_TEXT_PASS_ONE_SIZE_FOR_EACH, N00BB_NO_ARGS);
        core_free(type_text.data);
        return result;
    }
    }

    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    ir_instr_t *one = uint_type
        ? n00bb_emit_uint_constant(context, uint_type, 1, span,
                                   CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
    if (!one) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        return lower_internal(context, span,
                           "could not create the array extent constant");
    }
    core_list_t(n00bb_ir_value_ptr_t) dimension_values =
        core_list_new(n00bb_ir_value_ptr_t);
    core_list_t(n00bb_tree_ptr_t) dimension_expressions =
        core_list_new(n00bb_tree_ptr_t);
    for (uint32_t i = 0; i < rank; i++) {
        core_list_push(dimension_values, (ir_value_t *)nullptr);
        core_list_push(dimension_expressions, (parse_tree_t *)nullptr);
    }
    bool ok = true;
    for (size_t i = 0; ok && i < type_dimensions.len; i++) {
        parse_tree_t *expression = child_nt(visit->grammar,
            type_dimensions.data[i], CORE_STRING_STATIC("expression"));
        if (!expression) continue;
        dimension_expressions.data[i] = expression;
        ir_shape_extent_t declared_extent = array_dimension_extent(
            visit->grammar, expression);
        if (declared_extent.kind == IR_SHAPE_EXTENT_STATIC) {
            ir_instr_t *constant = n00bb_emit_uint_constant(context,
                uint_type, declared_extent.static_value,
                lower_span_for_tree(context, expression),
                CORE_STRING_STATIC("__n00bb_array_extent"));
            if (!constant) {
                ok = false;
                break;
            }
            dimension_values.data[i] = constant->dest;
            continue;
        }
        if (declared_extent.kind == IR_SHAPE_EXTENT_RUNTIME) {
            dimension_values.data[i] = n00bb_lower_array_dimension_value(
                context, expression, uint_type, one->dest,
                lower_span_for_tree(context, expression), true);
            if (!dimension_values.data[i]) ok = false;
            continue;
        }
        core_string_t name = {0};
        simple_identifier_expression(visit->grammar, expression, &name);
        if (name.u8_bytes)
            lower_report(context, N00B_DIAG_ARRAY_EXTENT_NOT_CONSTANT,
                lower_span_for_tree(context, expression),
                N00B_TEXT_ARRAY_SIZE_MUST_BE_A,
                N00BB_ARGS(ARG_TEXT(name)));
        else
            lower_report(context, N00B_DIAG_ARRAY_EXTENT_NOT_CONSTANT,
                lower_span_for_tree(context, expression),
                N00B_TEXT_THIS_ARRAY_SIZE_MUST_BE,
                N00BB_NO_ARGS);
        lower_help(context, N00B_TEXT_USE_A_CONST_OR_LEAVE, N00BB_NO_ARGS);
        ok = false;
    }
    if (!ok) {
        core_list_free(arguments);
        core_list_free(type_dimensions);
        core_list_free(dimension_values);
        core_list_free(dimension_expressions);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }

    for (size_t i = 0; ok && i < dimension_argument_count; i++) {
        parse_tree_t *argument = arguments.data[argument_offset + i];
        ir_value_t *extent = n00bb_lower_array_dimension_value(context,
            argument, uint_type, one->dest,
            lower_span_for_tree(context, argument), false);
        if (!extent) { ok = false; break; }
        uint32_t dimension = 0;
        while (dimension < rank && dimension_values.data[dimension])
            dimension++;
        if (dimension == rank) {
            ok = false;
            break;
        }
        dimension_values.data[dimension] = extent;
    }
    core_list_free(arguments);
    if (!ok) {
        core_list_free(type_dimensions);
        core_list_free(dimension_values);
        core_list_free(dimension_expressions);
        return lower_failed_after(context, span,
            CORE_STRING_STATIC("could not compute the array dimensions"));
    }

    ir_shape_extent_t *extents = core_alloc_array(ir_shape_extent_t, rank);
    for (uint32_t i = 0; i < rank; i++) {
        if (dimension_expressions.data[i]) {
            extents[i] = array_dimension_extent(visit->grammar,
                dimension_expressions.data[i]);
            extents[i].value = dimension_values.data[i];
        }
        else if (dimension_values.data[i])
            extents[i] = (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_RUNTIME,
                .value = dimension_values.data[i],
            };
        else extents[i] = (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_UNRESOLVED};
    }
    ir_value_t *array = n00bb_emit_array_allocation(context, leaf, rank,
        extents, result_type, span);
    core_free(extents);
    core_list_free(type_dimensions);
    core_list_free(dimension_values);
    core_list_free(dimension_expressions);
    if (!array)
        return lower_internal(context, span,
                           "could not emit semantic array construction");
    return (ir_lower_result_t){IR_LOWER_VALUE, array};
}
