#include "lower_internal.h"

static bool
n00bb_bind_runtime_array_shape(ir_lower_context_t *context,
                               ir_shape_t *shape);
static bool
n00bb_allocate_declared_array(ir_lower_context_t *context, ir_local_t *local,
                              ir_shape_t *shape, ir_span_t span);
static ir_value_t *
n00bb_lower_slice_bound(ir_lower_context_t *context, parse_tree_t *node,
                        th_type *bound_type, ir_span_t span);

bool
n00bb_attach_symbol_array_shape(ir_lower_context_t *context,
                                parse_grammar_t *grammar,
                                parse_sym_entry_t *symbol, th_type *type,
                                ir_value_t *value, ir_local_t *local)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    bool complete = false;
    ir_shape_t *shape = lower ? array_shape_from_declaration(grammar,
        lower->unit, lower->ir_context, symbol, type, &complete) : nullptr;
    if (!complete || (shape && value && !ir_value_set_shape(value, shape))
        || (shape && local && !ir_local_set_shape(local, shape))) {
        lower_internal(context,
            symbol ? lower_symbol_name_span(context, symbol)
                   : IR_SPAN_UNKNOWN,
            "could not attach the declaration's array shape");
        return false;
    }
    return true;
}

static bool
n00bb_bind_runtime_array_shape(ir_lower_context_t *context,
                               ir_shape_t *shape)
{
    if (!shape) return true;
    ir_shape_storage_t storage;
    if (!ir_shape_storage(shape, &storage)) return false;
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    ir_value_t *one = nullptr;
    bool has_runtime = false;
    for (uint32_t i = 0; i < storage.flat_prefix_rank; i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_RUNTIME)
            continue;
        has_runtime = true;
        if (extent.value) continue;
        if (!one) {
            ir_instr_t *constant = uint_type
                ? n00bb_emit_uint_constant(context, uint_type, 1,
                    IR_SPAN_UNKNOWN, CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
            one = constant ? constant->dest : nullptr;
            if (!one) return false;
        }
        parse_tree_t *expression = (parse_tree_t *)extent.expression;
        extent.value = expression
            ? n00bb_lower_array_dimension_value(context, expression,
                uint_type, one,
                lower_span_for_tree(context, expression), true) : nullptr;
        if (!extent.value || !ir_shape_set_extent(shape, i, extent))
            return false;
    }
    if (!has_runtime) return true;
    ir_shape_measure_t count;
    if (!ir_shape_element_count(shape, &count)) return false;
    if (count.kind != IR_SHAPE_MEASURE_UNKNOWN) return true;
    if (!one) {
        ir_instr_t *constant = uint_type
            ? n00bb_emit_uint_constant(context, uint_type, 1,
                IR_SPAN_UNKNOWN, CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
        one = constant ? constant->dest : nullptr;
        if (!one) return false;
    }
    ir_value_t *total = one;
    for (uint32_t i = 0; i < storage.flat_prefix_rank; i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) return false;
        ir_value_t *value = extent.value;
        if (extent.kind == IR_SHAPE_EXTENT_STATIC) {
            ir_instr_t *constant = n00bb_emit_uint_constant(context,
                uint_type, extent.static_value, IR_SPAN_UNKNOWN,
                CORE_STRING_STATIC("__n00bb_array_extent"));
            value = constant ? constant->dest : nullptr;
        }
        if (!value) return false;
        ir_instr_t *product = n00bb_emit_checked_product(context, total,
            value, uint_type, IR_SPAN_UNKNOWN);
        if (!product) return false;
        total = product->dest;
    }
    return ir_shape_set_element_count(shape, (ir_shape_measure_t){
        .kind = IR_SHAPE_MEASURE_RUNTIME,
        .value = total,
    });
}

bool
n00bb_lower_signature_array_shape(ir_lowerer_t *lowerer,
                                  ir_lower_context_t *context,
                                  parse_grammar_t *grammar,
                                  ir_shape_t *shape)
{
    th_type *uint_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("uint"));
    ir_value_t *one = nullptr;
    for (uint32_t d = 0; shape && d < ir_shape_rank(shape); d++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, d, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_RUNTIME || extent.value)
            continue;
        parse_tree_t *expression = (parse_tree_t *)extent.expression;
        if (!expression) return false;
        ir_lower_result_t bound = ir_lower_tree(lowerer, context, grammar,
                                               expression);
        if (bound.status != IR_LOWER_VALUE || !bound.value)
            return false;
        if (!one) {
            ir_instr_t *constant = uint_type
                ? n00bb_emit_uint_constant(context, uint_type, 1,
                    IR_SPAN_UNKNOWN, CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
            one = constant ? constant->dest : nullptr;
            if (!one) return false;
        }
        extent.value = n00bb_coerce_array_dimension_value(context,
            expression, bound.value, uint_type, one,
            lower_span_for_tree(context, expression), true);
        if (!extent.value || !ir_shape_set_extent(shape, d, extent))
            return false;
    }
    return n00bb_bind_runtime_array_shape(context, shape);
}

static bool
n00bb_allocate_declared_array(ir_lower_context_t *context, ir_local_t *local,
                              ir_shape_t *shape, ir_span_t span)
{
    ir_shape_storage_t storage;
    if (!local || !shape || !ir_shape_storage(shape, &storage)
        || !storage.flat_prefix_rank) return true;
    uint32_t rank = ir_shape_rank(shape);
    ir_shape_extent_t *extents = core_alloc_array(ir_shape_extent_t, rank);
    bool ok = true;
    for (uint32_t i = 0; ok && i < rank; i++) {
        ok = ir_shape_extent(shape, i, &extents[i]);
        if (ok && i < storage.flat_prefix_rank && !extents[i].value
            && extents[i].kind != IR_SHAPE_EXTENT_STATIC) {
            lower_report(context, N00B_DIAG_ARRAY_EXTENT_NOT_CONSTANT, span,
                N00B_TEXT_THE_SIZE_OF_THIS_ARRAY,
                N00BB_NO_ARGS);
            lower_help(context, N00B_TEXT_USE_A_CONSTANT_SIZE_OR,
                N00BB_NO_ARGS);
            core_free(extents);
            return false;
        }
    }
    ir_value_t *array = ok ? n00bb_emit_array_allocation(context,
        ir_shape_element_type(shape), rank, extents, local->type, span)
        : nullptr;
    core_free(extents);
    if (array && n00bb_emit_local_write(context, local, array, span))
        return true;
    lower_internal(context, span,
                "could not allocate the declared array");
    return false;
}

bool
n00bb_lower_declare_local(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state)
{
    (void)state;
    bool pattern = nt_is(visit->grammar,
        n00bb_pattern_owner(visit->grammar, visit->node),
        CORE_STRING_STATIC("variable-decl"));
    if (!pattern
        && !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("sym-decl")))
        return true;
    parse_sym_entry_t *symbol = parse_tree_sym(visit->node);
    if (!entering && pattern) return true;
    if (!entering) {
        parse_tree_t *expression = child_nt(visit->grammar, visit->node,
                                            CORE_STRING_STATIC("expression"));
        ir_local_t *local = symbol
            ? ir_lower_bound_local(context, symbol) : nullptr;
        ir_shape_t *shape = local ? ir_local_shape(local) : nullptr;
        if (!n00bb_bind_runtime_array_shape(context, shape)) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not bind runtime array dimensions");
            return false;
        }
        if (!expression)
            return !symbol || symbol->mutability != PARSE_SYM_MUTABLE
                || n00bb_allocate_declared_array(context, local, shape,
                       lower_span_for_tree(context, visit->node));
        if (!n00bb_shape_has_constraints(shape)) return true;
        ir_value_t *value = ir_lower_node_value(context, expression);
        if (!value || !n00bb_attach_expression_shape(context, visit->grammar,
                expression, value)
            || !n00bb_emit_sequence_shape_check(context, value, shape,
                lower_span_for_tree(context, visit->node))) return false;
        return true;
    }
    if (symbol && symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("module")))
        return true;
    if (!symbol || !symbol->type) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
                       "local declaration has no resolved type");
        return false;
    }
    char name_buffer[48];
    core_string_t name = n00bb_format_name(name_buffer, sizeof(name_buffer),
        "__n00bb_local_%llu", (unsigned long long)context->next_value_id++);
    th_type *type = th_apply_subst(ir_module_types(context->module),
                                   symbol->type);
    th_comb combination;
    if (type && th_sum_comb(th_resolve(type), &combination)
        && combination == TH_UNION) {
        core_string_t type_text = lower_type_text(context, type);
        lower_report(context, N00B_DIAG_UNION_VARIABLE,
            lower_span_for_tree(context, visit->node),
            N00B_TEXT_WOULD_HOLD_THE_ANONYMOUS_UNION,
            N00BB_ARGS(ARG_TEXT(symbol->name), ARG_TEXT(type_text)));
        lower_help(context, N00B_TEXT_GIVE_A_CONCRETE_TYPE_OR,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
        core_free(type_text.data);
        return false;
    }
    ir_local_t *local = type
        ? ir_local_add(context->function, name, type) : nullptr;
    if (local) (void)ir_local_set_source_name(context->function, local,
                                              symbol->name);
    if (local) local->mutability = local_binding_mutability(symbol);
    if (!local || !n00bb_set_reference_access(
            ((n00bb_lower_context_t *)context->user)->ir_context,
            ir_local_value(context->function, local), type)
        || !n00bb_bind_local(context, symbol, local,
                                     lower_span_for_tree(context,
                                                         visit->node))) {
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not bind local declaration");
        return false;
    }
    if (!n00bb_attach_symbol_array_shape(context, visit->grammar,
                                          symbol, type, nullptr, local))
        return false;
    return true;
}

bool
n00bb_has_index_store_target(parse_grammar_t *grammar, parse_tree_t *node)
{
    core_string_t trait = {0}, method = {0};
    if (index_dispatch_for_node(grammar, node, &trait, &method)
        && (core_string_eq(trait, CORE_STRING_STATIC("Index_Write"))
            || core_string_eq(trait,
                              CORE_STRING_STATIC("Index_Slice_Write"))
            || index_is_compound_target(grammar, node)))
        return true;
    for (size_t i = 0; node && !core_tree_is_leaf(node)
            && i < core_tree_num_children(node); i++)
        if (n00bb_has_index_store_target(grammar,
                                         core_tree_child(node, i)))
            return true;
    return false;
}

static ir_value_t *
n00bb_lower_slice_bound(ir_lower_context_t *context, parse_tree_t *node,
                        th_type *bound_type, ir_span_t span)
{
    if (!bound_type) return nullptr;
    if (!node)
        return n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr,
                                   bound_type, span);
    return ir_lower_node_value(context, node);
}

bool
n00bb_lower_index_store_at(ir_lower_context_t *context,
                           parse_grammar_t *grammar,
                           parse_tree_t *site_node,
                           parse_tree_t *target, parse_tree_t *source,
                           ir_value_t *source_value)
{
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *index_node = n00bb_index_expression_node(grammar, target);
    bool slice = nt_is(grammar, index_node
        && core_tree_num_children(index_node) > 2
            ? core_tree_child(index_node, 2) : nullptr, CORE_STRING_STATIC("slice-contents"));
    parse_tree_t *contents = index_node
        && core_tree_num_children(index_node) > 2
            ? core_tree_child(index_node, 2) : nullptr;
    parse_tree_t *receiver_node = index_node
        && core_tree_num_children(index_node)
            ? core_tree_child(index_node, 0) : nullptr;
    parse_tree_t *index_arg = child_nt(grammar, contents,
                                       CORE_STRING_STATIC("expression"));
    ir_value_t *arguments[5] = {
        receiver_node ? ir_lower_node_value(context, receiver_node) : nullptr,
        nullptr, nullptr, nullptr,
        source_value ? source_value
                     : source ? ir_lower_node_value(context, source) : nullptr,
    };
    if (!slice) {
        arguments[1] = index_arg
            ? ir_lower_node_value(context, index_arg) : nullptr;
        arguments[2] = arguments[4];
    }
    else {
        parse_tree_t *bounds[2] = {0};
        size_t position = 0;
        bool inclusive = false, separator_seen = false;
        for (size_t i = 0; contents && i < core_tree_num_children(contents);
             i++) {
            parse_tree_t *part = core_tree_child(contents, i);
            if (nt_is(grammar, part, CORE_STRING_STATIC("range-sep"))
                || parse_tree_leaf_is(part, CORE_STRING_STATIC(":"))) {
                position = 1;
                separator_seen = true;
                inclusive = core_string_eq(first_token_text(part),
                                           CORE_STRING_STATIC("to"));
            }
            else if (nt_is(grammar, part, CORE_STRING_STATIC("expression")))
                bounds[position] = part;
        }
        th_type *integer = n00bb_ir_builtin_type(context->module,
                                                 CORE_STRING_STATIC("int"));
        th_type *bound_type = integer
            ? th_option(ir_module_types(context->module), integer) : nullptr;
        ir_span_t span = lower_span_for_tree(context, contents);
        arguments[1] = separator_seen
            ? n00bb_lower_slice_bound(context, bounds[0], bound_type, span)
            : nullptr;
        arguments[2] = separator_seen
            ? n00bb_lower_slice_bound(context, bounds[1], bound_type, span)
            : nullptr;
        th_type *bool_type = n00bb_ir_builtin_type(context->module, CORE_STRING_STATIC("bool"));
        ir_instr_t *flag = separator_seen && bool_type
            ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
        if (flag && ir_lower_set_temp_dest(context, flag,
            CORE_STRING_STATIC("__n00bb_slice_inclusive_"), bool_type)
            && ir_instr_operand_int(flag, inclusive ? 1 : 0)) {
            arguments[3] = flag->dest;
        }
    }
    ir_value_t *operands[4] = {arguments[0], arguments[1], arguments[2],
                               arguments[3]};
    if (!slice) operands[2] = arguments[4];
    else operands[3] = arguments[4];
    size_t count = slice ? 4 : 3;
    bool complete = lower != nullptr;
    for (size_t i = 0; i < count; i++) complete &= operands[i] != nullptr;
    ir_value_t *written = complete
        ? n00bb_dispatch(context,
              slice ? CORE_STRING_STATIC("__write_slice")
                    : CORE_STRING_STATIC("__write"),
              operands, count,
              th_name(ir_module_types(context->module),
                      CORE_STRING_STATIC("nil")),
              lower_span_for_tree(context, site_node))
        : nullptr;
    if (!written || !ir_annot_set(lower->ir_context, &written->def->annots,
            (ir_annot_t){.key = N00BB_WRITE_BACK_KEY,
                         .kind = IR_ANNOT_BOOL, .b = true})) {
        lower_internal(context, lower_span_for_tree(context, site_node),
            "could not lower a dispatched indexed assignment");
        return false;
    }
    return n00bb_move_source(context, grammar, source);
}
