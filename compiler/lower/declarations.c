#include "lower_internal.h"

static bool
n00bb_bind_runtime_array_shape(ir_lower_context_t *context,
                               ir_shape_t *shape);
static bool
n00bb_allocate_declared_array(ir_lower_context_t *context, ir_local_t *local,
                              ir_shape_t *shape, bool initializes,
                              ir_span_t span);

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
write_declaration_default(ir_lower_context_t *context, ir_local_t *local,
                          ir_value_t *array, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !n00bb_emit_local_write(context, local, array, span))
        return false;
    ir_block_t *block = context->block;
    ir_instr_t *write = block && block->n_instrs
        ? block->instrs[block->n_instrs - 1] : nullptr;
    return write && ir_annot_set(lower->ir_context, &write->annots,
        (ir_annot_t){.key = CORE_STRING_STATIC("ir.declaration_default"),
                     .kind = IR_ANNOT_BOOL, .b = true});
}

static bool
n00bb_allocate_declared_array(ir_lower_context_t *context, ir_local_t *local,
                              ir_shape_t *shape, bool initializes,
                              ir_span_t span)
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
    bool written = array && (initializes
        ? n00bb_emit_local_write(context, local, array, span)
        : write_declaration_default(context, local, array, span));
    if (written) return true;
    lower_internal(context, span,
                "could not allocate the declared array");
    return false;
}

bool
n00bb_allocate_runtime_global(ir_lower_context_t *context,
                              parse_sym_entry_t *symbol, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    ir_global_t *global = state
        ? link_ir_global_for_symbol(state->link_analysis, symbol) : nullptr;
    if (!n00bb_global_needs_runtime_array(global)) return true;
    ir_shape_t *declared = parse_sym_binding(symbol,
        CORE_STRING_STATIC("ir.array-shape"));
    ir_shape_t *shape = declared
        && ir_shape_rank(declared) == ir_shape_rank(global->shape)
        ? declared : global->shape;
    ir_shape_storage_t storage;
    uint32_t rank = ir_shape_rank(shape);
    ir_shape_extent_t *extents = core_alloc_array(ir_shape_extent_t, rank);
    th_type *uint_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("uint"));
    ir_instr_t *one = uint_type ? n00bb_emit_uint_constant(context, uint_type,
        1, span, CORE_STRING_STATIC("__n00bb_array_one")) : nullptr;
    bool ok = one && ir_shape_storage(shape, &storage);
    for (uint32_t i = 0; ok && i < rank; i++) {
        ok = ir_shape_extent(shape, i, &extents[i]);
        if (!ok || i >= storage.flat_prefix_rank
            || extents[i].kind == IR_SHAPE_EXTENT_STATIC) continue;
        parse_tree_t *expression = (parse_tree_t *)extents[i].expression;
        extents[i].kind = IR_SHAPE_EXTENT_RUNTIME;
        extents[i].value = expression
            ? n00bb_lower_array_dimension_value(context, expression,
                uint_type, one->dest,
                lower_span_for_tree(context, expression), true)
            : nullptr;
        ok = extents[i].value != nullptr;
    }
    ir_value_t *array = ok ? n00bb_emit_array_allocation(context,
        ir_shape_element_type(shape), rank, extents, global->type, span)
        : nullptr;
    core_free(extents);
    ir_value_t *target = array ? ir_global_address(context->module, global)
                               : nullptr;
    ir_instr_t *store = target
        ? ir_lower_emit_instruction(context, IR_OP_GLOBAL_STORE, span)
        : nullptr;
    ok = store && ir_instr_operand_value(store, target)
        && ir_instr_operand_value(store, array)
        && ir_instr_set_binding_initialization(store, true)
        && (symbol->mutability != PARSE_SYM_IMMUTABLE
            || ir_annot_set(lower->ir_context, &store->annots, (ir_annot_t){
                   .key = CORE_STRING_STATIC("ir.declaration_default"),
                   .kind = IR_ANNOT_BOOL, .b = true}));
    if (!ok)
        lower_internal(context, span,
                       "could not allocate the declared module array");
    return ok;
}

static bool
record_holds_array(th_ctx *types, th_type *type, uint32_t depth)
{
    th_type *record = depth < 8 && type ? th_fields_record(types, type)
                                        : nullptr;
    for (size_t i = 0; record && i < th_record_len(record); i++) {
        th_type *field = th_resolve(th_record_member(record, i)->type);
        if (field && (th_kind_of(field) == TH_SEQUENCE
                      || record_holds_array(types, field, depth + 1)))
            return true;
    }
    return false;
}

static bool
zero_declared_record(ir_lower_context_t *context, ir_local_t *local,
                     ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    if (!local || !record_holds_array(types, local->type, 0)) return true;
    n00bb_lower_context_t *lower = context->user;
    th_type *address_type = th_ind(types, 0, local->type);
    th_type *byte = n00bb_ir_builtin_type(context->module,
                                          CORE_STRING_STATIC("byte"));
    th_type *uint_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("uint"));
    ir_instr_t *address = address_type && byte && uint_type
        ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
        : nullptr;
    bool ok = address
        && ir_lower_set_temp_dest(context, address,
               CORE_STRING_STATIC("__n00bb_zeroed_"), address_type)
        && ir_instr_operand_local(address, local);
    ir_instr_t *zero = ok ? n00bb_emit_uint_constant(context, byte, 0, span,
        CORE_STRING_STATIC("__n00bb_zero")) : nullptr;
    ir_instr_t *size = zero
        ? ir_lower_emit_instruction(context, IR_OP_SIZEOF, span) : nullptr;
    ok = size && ir_lower_set_temp_dest(context, size,
            CORE_STRING_STATIC("__n00bb_size_"), uint_type)
        && ir_instr_operand_type(size, local->type);
    ir_instr_t *fill = ok
        ? ir_lower_emit_instruction(context, IR_OP_MEMSET, span) : nullptr;
    ok = fill && ir_instr_operand_value(fill, address->dest)
        && ir_instr_operand_value(fill, zero->dest)
        && ir_instr_operand_value(fill, size->dest)
        && ir_annot_set(lower->ir_context, &fill->annots, (ir_annot_t){
               .key = CORE_STRING_STATIC("ir.declaration_zero"),
               .kind = IR_ANNOT_BOOL, .b = true});
    if (!ok) lower_internal(context, span, "could not zero the declared record");
    return ok;
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
        if (!expression) {
            ir_span_t span = lower_span_for_tree(context, visit->node);
            if (!local)
                return !symbol || symbol->mutability == PARSE_SYM_CONST
                    || n00bb_allocate_runtime_global(context, symbol, span);
            ir_shape_storage_t storage;
            bool sized = shape && ir_shape_storage(shape, &storage)
                && storage.flat_prefix_rank;
            if (symbol && symbol->mutability == PARSE_SYM_IMMUTABLE && sized)
                return n00bb_allocate_declared_array(context, local, shape,
                                                     false, span);
            if (symbol && symbol->mutability == PARSE_SYM_IMMUTABLE) {
                if (!local
                    || th_kind_of(th_resolve(local->type)) != TH_SEQUENCE)
                    return true;
                ir_value_t *array = n00bb_emit_let_default_array(context,
                    local->type, span);
                if (array && write_declaration_default(context, local, array,
                                                       span)) return true;
                lower_internal(context, span,
                               "could not give a `let` array its default");
                return false;
            }
            if (!symbol || symbol->mutability != PARSE_SYM_MUTABLE)
                return true;
            return n00bb_allocate_declared_array(context, local, shape, true,
                                                 span)
                && zero_declared_record(context, local, span);
        }
        if (!local && symbol && expression) {
            ir_value_t *value = ir_lower_node_value(context, expression);
            if (value)
                n00bb_report_static_shape_mismatch(context, value,
                    parse_sym_binding(symbol,
                        CORE_STRING_STATIC("ir.array-shape")),
                    lower_span_for_tree(context, visit->node));
        }
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

static bool
constant_slice_bound(const ir_value_t *bound, int64_t *value)
{
    const ir_instr_t *def = bound && bound->kind == IR_VAL_INSTR
        ? bound->def : nullptr;
    if (!def || def->op != IR_OP_CONST || def->n_operands != 1
        || def->operands[0].kind != IR_OPD_CONST
        || def->operands[0].lit.i.mag > (ir_uint_t)INT64_MAX)
        return false;
    *value = (int64_t)ir_lit_signed(def->operands[0].lit.i);
    return true;
}

bool
n00bb_lower_slice_arguments(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *contents,
                            ir_value_t *arguments[3])
{
    parse_tree_t *bounds[2] = {0};
    size_t position = 0;
    bool inclusive = false;
    for (size_t i = 0; contents && i < core_tree_num_children(contents);
         i++) {
        parse_tree_t *part = core_tree_child(contents, i);
        if (nt_is(grammar, part, CORE_STRING_STATIC("range-sep"))
            || parse_tree_leaf_is(part, CORE_STRING_STATIC(":"))) {
            position = 1;
            inclusive = core_string_eq(first_token_text(part),
                                       CORE_STRING_STATIC("to"));
        }
        else if (nt_is(grammar, part, CORE_STRING_STATIC("expression")))
            bounds[position] = part;
    }
    ir_span_t span = lower_span_for_tree(context, contents);
    th_type *integer = n00bb_ir_builtin_type(context->module,
                                             CORE_STRING_STATIC("int"));
    th_type *bound_type = integer
        ? th_option(ir_module_types(context->module), integer) : nullptr;
    arguments[0] = n00bb_lower_slice_bound(context, bounds[0], bound_type,
                                           span);
    arguments[1] = n00bb_lower_slice_bound(context, bounds[1], bound_type,
                                           span);
    int64_t start = 0, end = 0;
    if (bounds[0] && bounds[1]
        && constant_slice_bound(arguments[0], &start)
        && constant_slice_bound(arguments[1], &end)
        && (start < 0) == (end < 0)
        && start > (inclusive ? end + 1 : end)) {
        lower_report(context, N00B_DIAG_SLICE_REVERSED, span,
            N00B_TEXT_SLICE_REVERSED,
            N00BB_ARGS(ARG_NUMBER(start), ARG_NUMBER(end)));
        lower_help(context, N00B_TEXT_SLICE_REVERSED_HELP, N00BB_NO_ARGS);
        return false;
    }
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    ir_instr_t *flag = bool_type
        ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
    arguments[2] = flag && ir_lower_set_temp_dest(context, flag,
            CORE_STRING_STATIC("__n00bb_slice_inclusive_"), bool_type)
        && ir_instr_operand_int(flag, inclusive ? 1 : 0)
        ? flag->dest : nullptr;
    return arguments[0] && arguments[1] && arguments[2];
}

ir_value_t *
n00bb_lower_slice_read(ir_lower_context_t *context, parse_grammar_t *grammar,
                       parse_tree_t *index_node, ir_value_t *container,
                       th_type *result_type, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *contents = core_tree_num_children(index_node) > 2
        ? core_tree_child(index_node, 2) : nullptr;
    ir_value_t *operands[4] = {container};
    if (!lower || !n00bb_lower_slice_arguments(context, grammar, contents,
                                               &operands[1]))
        return nullptr;
    ir_value_t *read = n00bb_dispatch(context, CORE_STRING_STATIC("__slice"),
        operands, 4,
        result_type ? result_type : th_var(ir_module_types(context->module)),
        span);
    return read && ir_annot_set(lower->ir_context, &read->def->annots,
            (ir_annot_t){.key = N00BB_SLICE_TARGET_KEY,
                         .kind = IR_ANNOT_BOOL, .b = true})
        ? read : nullptr;
}
