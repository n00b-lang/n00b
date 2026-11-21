#include "lower_internal.h"
#include "sequence_transfer.h"

static bool
n00bb_lower_shaped_assignment(ir_lower_context_t *context,
                              parse_visit_t *visit, parse_sym_entry_t *symbol,
                              parse_tree_t *source);
static ir_value_t *
n00bb_materialize_source(ir_lower_context_t *context, parse_visit_t *visit,
                         parse_tree_t *source, th_type *target);
static bool
n00bb_materialize_assignment_source(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *source, th_type *target,
                                    bool entering);

bool
n00bb_shape_has_constraints(ir_shape_t *shape)
{
    for (uint32_t i = 0; shape && i < ir_shape_rank(shape); i++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, i, &extent)) return false;
        if (extent.kind != IR_SHAPE_EXTENT_UNRESOLVED) return true;
    }
    return false;
}

static bool
n00bb_emit_fixed_array_store(ir_lower_context_t *context, ir_value_t *target,
                             ir_value_t *value, bool initializes,
                             ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    bool global = target->kind == IR_VAL_GLOBAL;
    ir_instr_t *store = ir_lower_emit_instruction(context,
        global ? IR_OP_GLOBAL_STORE : IR_OP_STORE, span);
    return store
        && ir_instr_set_binding_initialization(store, initializes)
        && ir_instr_operand_value(store, target)
        && ir_instr_operand_value(store, value)
        && ir_annot_set(lower->ir_context, &store->annots,
               (ir_annot_t){.key = N00BB_FIXED_ARRAY_STORE_KEY,
                            .kind = IR_ANNOT_BOOL, .b = true});
}

static bool
n00bb_lower_shaped_assignment(ir_lower_context_t *context,
                              parse_visit_t *visit, parse_sym_entry_t *symbol,
                              parse_tree_t *source)
{
    ir_local_t *local = symbol ? ir_lower_bound_local(context, symbol) : nullptr;
    ir_value_t *binding = symbol
        ? ir_lower_bound_value(context, symbol) : nullptr;
    ir_value_t *address = symbol
        ? ir_lower_bound_address(context, symbol) : nullptr;
    ir_shape_t *required = local ? ir_local_shape(local)
        : binding ? ir_value_shape(binding) : nullptr;
    if (!required && symbol)
        required = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (!n00bb_shape_has_constraints(required)) return false;
    ir_value_t *value = source ? ir_lower_node_value(context, source) : nullptr;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    if (!value || !n00bb_attach_expression_shape(context, visit->grammar,
            source, value))
        return false;
    bool fixed = true;
    for (uint32_t d = 0; fixed && d < ir_shape_rank(required); d++) {
        ir_shape_extent_t extent;
        fixed = ir_shape_extent(required, d, &extent)
            && extent.kind == IR_SHAPE_EXTENT_STATIC;
    }
    ir_value_t *local_address = local && fixed
        ? ir_lower_local_address(context, local) : nullptr;
    if (local && !local_address)
        return n00bb_emit_local_write(context, local, value, span);
    if (!n00bb_emit_sequence_shape_check(context, value, required, span))
        return false;
    if (local_address || (address && fixed))
        return n00bb_emit_fixed_array_store(context,
            local_address ? local_address : address, value, true, span);
    if (address)
        return n00bb_emit_indirect_write(context, address, value, span);
    if (binding && binding->kind == IR_VAL_GLOBAL && binding->global)
        return n00bb_emit_fixed_array_store(context, binding, value,
            binding->global->mutability != IR_MUT_MUTABLE, span);
    lower_internal(context, span,
                   "shaped assignment target has no writable storage");
    return false;
}


static ir_value_t *
n00bb_materialize_source(ir_lower_context_t *context, parse_visit_t *visit,
                         parse_tree_t *source, th_type *target)
{
    (void)visit;
    ir_span_t span = lower_span_for_tree(context, source);
    ir_value_t *value = nullptr;
    (void)target;
    (void)span;
    value = ir_lower_node_value(context, source);
    return value && ir_lower_set_node_value(context, source, value)
        ? value : nullptr;
}

static bool
n00bb_materialize_assignment_source(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *source, th_type *target,
                                    bool entering)
{
    if (!source || !target) return true;
    if (entering)
        return true;
    if (!n00bb_materialize_source(context, visit, source, target)) {
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not materialize the assigned value");
        return false;
    }
    return true;
}






static bool
record_write_site(ir_lower_context_t *context, parse_tree_t *target,
                  core_string_t field)
{
    n00bb_lower_context_t *lower = context->user;
    ir_block_t *block = context->block;
    ir_instr_t *store = block && block->n_instrs
        ? block->instrs[block->n_instrs - 1] : nullptr;
    ir_value_t *address = store && store->op == IR_OP_STORE
            && store->n_operands == 2
        ? ir_operand_value(&store->operands[0]) : nullptr;
    for (size_t depth = 0; address && address->kind == IR_VAL_INSTR
         && address->def && address->def->op == IR_OP_FIELD
         && address->def->n_operands && depth < 64; depth++)
        address = ir_operand_value(&address->def->operands[0]);
    ir_local_t *local = nullptr;
    ir_value_t *storage = nullptr;
    if (!address || !n00bb_place_root(address, &local, &storage)) return true;
    parse_sym_entry_t *symbol = ir_lower_binding_symbol(context, local,
                                                        storage);
    if (symbol)
        core_list_push(tu_state(lower->unit)->write_sites,
            ((n00bb_write_site_t){target, symbol, store, field}));
    return true;
}

static bool
check_initialized_let(ir_lower_context_t *context, parse_grammar_t *grammar,
                      parse_sym_entry_t *symbol, parse_tree_t *target)
{
    parse_tree_t *initializer = symbol && symbol->kind == PARSE_SYM_VARIABLE
        && symbol->mutability == PARSE_SYM_IMMUTABLE && symbol->decl_node
        ? child_nt(grammar, symbol->decl_node,
                   CORE_STRING_STATIC("expression")) : nullptr;
    if (!initializer) return true;
    lower_report(context, N00B_DIAG_LET_ASSIGNED_TWICE,
        lower_span_for_tree(context, target), N00B_TEXT_IS_A_LET_SO_IT,
        N00BB_ARGS(ARG_TEXT(symbol->name)));
    lower_declared_secondary(context, symbol, N00B_TEXT_IS_DECLARED_HERE,
        N00BB_ARGS(ARG_TEXT(symbol->name)));
    lower_secondary(context, lower_span_for_tree(context, initializer),
        N00B_TEXT_IT_IS_FIRST_INITIALIZED_HERE, N00BB_NO_ARGS);
    lower_help(context, N00B_TEXT_DECLARE_WITH_VAR_TO_ALLOW_ASSIGNMENT,
        N00BB_ARGS(ARG_TEXT(symbol->name)));
    return false;
}

static void
check_loop_initialization(ir_lower_context_t *context,
                          parse_grammar_t *grammar, parse_tree_t *statement,
                          parse_sym_entry_t *symbol, parse_tree_t *target)
{
    if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
        || symbol->mutability != PARSE_SYM_IMMUTABLE || !symbol->decl_node
        || child_nt(grammar, symbol->decl_node,
                    CORE_STRING_STATIC("expression")))
        return;
    (void)statement;
    n00bb_lower_context_t *lower = context->user;
    n00bb_loop_frame_t *frame = lower && lower->loops.len
        ? &lower->loops.data[lower->loops.len - 1] : nullptr;
    if (!frame || !frame->active
        || ir_lower_bound_since(context, symbol, frame->entry_bindings))
        return;
    parse_tree_t *loop = frame->node;
    lower_report(context, N00B_DIAG_LET_INITIALIZED_IN_LOOP,
        lower_span_for_tree(context, target),
        N00B_TEXT_LET_INITIALIZED_IN_LOOP, N00BB_ARGS(ARG_TEXT(symbol->name)));
    lower_declared_secondary(context, symbol, N00B_TEXT_IS_DECLARED_HERE,
        N00BB_ARGS(ARG_TEXT(symbol->name)));
    lower_secondary(context, lower_span_for_tree(context, loop),
        N00B_TEXT_LOOP_REPEATS_ASSIGNMENT, N00BB_NO_ARGS);
    lower_help(context, N00B_TEXT_DECLARE_VAR_OR_IN_LOOP,
        N00BB_ARGS(ARG_TEXT(symbol->name)));
}


static void
report_unassignable(ir_lower_context_t *context, parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    lower_report(context, N00B_DIAG_NOT_WRITABLE,
        lower_span_for_tree(context, target), N00B_TEXT_ASSIGNS_A_TEMPORARY,
        N00BB_ARGS(ARG_TEXT(source_text_of(lower->unit, target))));
    lower_help(context, N00B_TEXT_ASSIGNS_A_TEMPORARY_HELP, N00BB_NO_ARGS);
}

static bool
n00bb_lower_variable_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *target,
                                parse_tree_t *source,
                                parse_sym_entry_t *variable, ir_value_t *read)
{
    ir_span_t span = lower_span_for_tree(context, visit->node);
    if (ir_func_is_initializer(context->function)
        && !check_initialized_let(context, visit->grammar, variable, target))
        return false;
    check_loop_initialization(context, visit->grammar, visit->node, variable,
                              target);
    if (!ir_value_has_uses(context->function, read)
        && read->def->op != IR_OP_TYPE_NARROW)
        (void)ir_instr_erase(read->def);
    ir_local_t *local = ir_lower_bound_local(context, variable);
    ir_value_t *binding = ir_lower_bound_value(context, variable);
    ir_shape_t *shape = local ? ir_local_shape(local)
        : binding ? ir_value_shape(binding) : nullptr;
    if (!shape)
        shape = parse_sym_binding(variable,
                                  CORE_STRING_STATIC("ir.array-shape"));
    if (n00bb_shape_has_constraints(shape)) {
        size_t failures = lower_failures(context);
        return n00bb_lower_shaped_assignment(context, visit, variable, source)
            || lower_failures(context) == failures;
    }
    ir_value_t *value = source
        ? n00bb_materialize_source(context, visit, source, variable->type)
        : nullptr;
    bool initializes = variable->decl_node == visit->node
        || ir_lower_symbol_defined_at(variable, visit->node);
    return value && ir_lower_assign_symbol(context, variable, value,
                                           initializes, span).status
        != IR_LOWER_FAILED;
}

static bool
n00bb_lower_place_assignment(ir_lower_context_t *context,
                             parse_visit_t *visit, parse_tree_t *target,
                             parse_tree_t *source, bool entering)
{
    if (entering)
        return ir_lower_handle_assignment(context, visit->node);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *read = ir_lower_node_value(context, target);
    if (!read) {
        lower_internal(context, span, "assignment target was not lowered");
        return false;
    }
    parse_sym_entry_t *variable = ir_lower_read_variable(context, read);
    if (variable)
        return n00bb_lower_variable_assignment(context, visit, target,
                                               source, variable, read);
    ir_value_t *value = source
        ? n00bb_materialize_source(context, visit, source, read->type)
        : nullptr;
    if (!value) {
        lower_internal(context, span, "assignment has no value");
        return false;
    }
    if (!n00bb_emit_sequence_shape_check(context, value,
                                         ir_value_shape(read), span))
        return false;
    size_t failures = lower_failures(context);
    if (!n00bb_store_to(context, read, value, false, span)) {
        if (lower_failures(context) == failures)
            report_unassignable(context, target);
        return false;
    }
    ir_block_t *block = context->block;
    ir_instr_t *store = block->n_instrs
        ? block->instrs[block->n_instrs - 1] : nullptr;
    if (store && store->op == IR_OP_STORE)
        n00bb_rewrite_later(context, store);
    ir_value_t *address = store && store->op == IR_OP_STORE
            && store->n_operands == 2
        ? ir_operand_value(&store->operands[0]) : nullptr;
    core_string_t name = address && address->def
            && address->def->op == IR_OP_FIELD && address->def->n_operands == 2
            && address->def->operands[1].kind == IR_OPD_NAME
        ? address->def->operands[1].name : (core_string_t){0};
    return !name.data || record_write_site(context, target, name);
}


bool
n00bb_check_assignment(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("sym-decl"))) {
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->node, visit->annot->value_ref);
        parse_sym_entry_t *declared = parse_tree_sym(visit->node);
        th_type *expected = declared ? declared->type : nullptr;
        if (!n00bb_materialize_assignment_source(context, visit, source,
                                                 expected, entering))
            return false;
        ir_shape_t *shape = declared ? parse_sym_binding(declared,
            CORE_STRING_STATIC("ir.array-shape")) : nullptr;
        ir_shape_storage_t storage;
        if (!n00bb_shape_has_constraints(shape)
            || (ir_shape_storage(shape, &storage)
                && storage.classification_known
                && storage.flat_prefix_rank < ir_shape_rank(shape)))
            return true;
        if (entering)
            return ir_lower_handle_assignment(context, visit->node);
        size_t failures = lower_failures(context);
        return n00bb_lower_shaped_assignment(context, visit, declared, source)
            || lower_failures(context) == failures;
    }
    bool compound = nt_is(visit->grammar, visit->node,
                          CORE_STRING_STATIC("binop-assign-stmt"));
    if (!compound && !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("assign-stmt")))
        return true;
    if (!entering && ir_lower_emit_suppressed(context, visit->node))
        return true;
    parse_tree_t *target = parse_tree_resolve_child_ref(
        visit->node, visit->annot->name_ref);
    parse_tree_t *source = parse_tree_resolve_child_ref(
        visit->node, visit->annot->value_ref);
    if (!compound && parse_tree_discards_value(visit->node))
        return !entering || ir_lower_handle_assignment(context, visit->node);
    if (compound) {
        if (!entering
            || ir_lower_evaluate_assignment_target(context, visit->node))
            return true;
        lower_internal(context, lower_span_for_tree(context, visit->node),
            "could not evaluate compound assignment target");
        return false;
    }
    if (!parse_tree_declares_binding(visit->node))
        return n00bb_lower_place_assignment(context, visit, target, source,
                                            entering);

    parse_sym_entry_t *symbol = parse_tree_sym(visit->node);
    if (!entering) {
        if (!n00bb_materialize_assignment_source(context, visit, source,
                symbol ? symbol->type : nullptr, false))
            return false;
        size_t failures = lower_failures(context);
        return !symbol
            || n00bb_lower_shaped_assignment(context, visit, symbol, source)
            || lower_failures(context) == failures;
    }
    if (!n00bb_materialize_assignment_source(context, visit, source,
            symbol ? symbol->type : nullptr, true)) return false;
    if (symbol && symbol->kind == PARSE_SYM_VARIABLE
        && scope_is_within_function(symbol->scope)
        && !ir_lower_bound_value(context, symbol)
        && !ir_lower_bound_local(context, symbol)
        && !ir_lower_bound_address(context, symbol)) {
        if (!symbol->type) {
            lower_internal(context, lower_span_for_tree(context, visit->node),
                           "inferred assignment has no resolved type");
            return false;
        }
        char name_buffer[48];
        core_string_t name = n00bb_format_name(name_buffer,
            sizeof(name_buffer), "__n00bb_local_%llu",
            (unsigned long long)context->next_value_id++);
        th_type *type = th_apply_subst(ir_module_types(context->module),
                                       symbol->type);
        ir_local_t *local = type
            ? ir_local_add(context->function, name, type) : nullptr;
        if (local) (void)ir_local_set_source_name(context->function, local,
                                                  symbol->name);
        if (local) local->mutability = local_binding_mutability(symbol);
        if (!local || !n00bb_bind_local(context, symbol, local,
                                         lower_span_for_tree(context,
                                                             visit->node))) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not bind inferred assignment destination");
            return false;
        }
        if (!n00bb_attach_symbol_array_shape(context, visit->grammar,
                symbol, type, nullptr, local)) return false;
    }
    ir_local_t *target_local = symbol
        ? ir_lower_bound_local(context, symbol) : nullptr;
    ir_shape_t *target_shape = target_local ? ir_local_shape(target_local)
                                            : nullptr;
    if (!target_shape && symbol)
        target_shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (n00bb_shape_has_constraints(target_shape)
        && !ir_lower_handle_assignment(context, visit->node)) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
            "could not claim shaped assignment lowering");
        return false;
    }
    return true;
}


static ir_lower_result_t
lower_atomic_update(ir_lower_context_t *context,
                    const n00bb_binary_operator_t *operator,
                    ir_value_t *place, th_type *element,
                    ir_value_t *const *operands, ir_span_t span)
{
    core_string_t kind = operator->op == IR_OP_ADD ? CORE_STRING_STATIC("add")
        : operator->op == IR_OP_SUB ? CORE_STRING_STATIC("sub")
        : operator->op == IR_OP_AND ? CORE_STRING_STATIC("and")
        : operator->op == IR_OP_OR ? CORE_STRING_STATIC("or")
        : (core_string_t){0};
    if (!kind.data) {
        lower_report(context, N00B_DIAG_ATOMIC_PART_UPDATE, span,
                     N00B_TEXT_NO_ATOMIC_FORM,
                     N00BB_ARGS(ARG_TEXT(operator->assign_spelling)));
        lower_help(context, N00B_TEXT_UPDATE_ATOMIC_WITH_COMPARE_EXCHANGE,
                   N00BB_NO_ARGS);
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    }
    ir_value_t *value = n00bb_materialize(context, nullptr, operands[1],
                                          element, span);
    ir_instr_t *rmw = value
        ? ir_lower_emit_instruction(context, IR_OP_ATOMIC_RMW, span)
        : nullptr;
    bool ok = rmw && ir_lower_set_temp_dest(context, rmw,
            CORE_STRING_STATIC("__n00bb_atomic_"), element)
        && ir_instr_operand_value(rmw, place)
        && ir_instr_operand_name(rmw, kind)
        && ir_instr_operand_value(rmw, value);
    if (!ok)
        return lower_internal(context, span,
                              "could not emit an atomic update");
    if (operator->op == IR_OP_ADD || operator->op == IR_OP_SUB) {
        ir_value_t *checked[2] = {rmw->dest, value};
        ir_lower_result_t result = n00bb_lower_operator_method(context,
            operator->method, checked, 2, element, span);
        if (result.status != IR_LOWER_VALUE) return result;
    }
    ir_instr_t *read = operands[0]->def;
    if (read && read->op == IR_OP_LOAD && !ir_instr_erase(read))
        return lower_internal(context, span,
                              "could not drop an atomic target's read");
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

ir_lower_result_t
n00bb_lower_compound_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit,
                                ir_value_t *const *operands,
                                size_t operand_count,
                                th_type *result_type, ir_span_t span,
                                void *state)
{
    (void)result_type;
    (void)state;
    if (operand_count != 2 || !operands || !operands[0] || !operands[1])
        return lower_internal(context, span,
            "compound assignment has no captured target or right operand");
    n00bb_lower_context_t *lower = context->user;
    th_type *target_type = operands[0]->type;
    const n00bb_binary_operator_t *operator = n00bb_binary_operator_assigned(
        first_token_text(core_tree_num_children(visit->node) > 1
            ? core_tree_child(visit->node, 1) : nullptr));
    if (!operator)
        return lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET, span,
            N00B_TEXT_NOT_SUPPORTED_YET_THIS_COMPOUND,
            N00BB_NO_ARGS);

    ir_instr_t *target_read = operands[0]->def;
    ir_value_t *atomic_place = target_read && target_read->op == IR_OP_LOAD
        && target_read->n_operands == 1
        ? ir_operand_value(&target_read->operands[0]) : nullptr;
    th_type *atomic_held = atomic_place && atomic_place->type
        && th_kind_of(th_resolve(atomic_place->type)) == TH_INDIRECTION
        ? th_ind_referent(th_resolve(atomic_place->type)) : nullptr;
    th_type *atomic_element = atomic_held
        ? n00bb_atomic_element_type(atomic_held) : nullptr;
    if (atomic_element)
        return lower_atomic_update(context, operator, atomic_place,
                                   atomic_element, operands, span);

    ir_lower_result_t result = n00bb_lower_operator_method(context,
        operator->method, operands, operand_count, target_type, span);
    if (result.status != IR_LOWER_VALUE) return result;
    ir_value_t *updated = result.value;
    if (updated && updated->kind == IR_VAL_INSTR && updated->def
        && updated->def->op == IR_OP_DISPATCH
        && !ir_annot_set(lower->ir_context, &updated->def->annots,
               (ir_annot_t){.key = N00BB_COMPOUND_KEY, .kind = IR_ANNOT_BOOL,
                            .b = true}))
        return lower_internal(context, span,
                              "could not mark a compound assignment");
    size_t failures = lower_failures(context);
    bool stored = updated
        && n00bb_store_to(context, operands[0], updated, false, span);
    if (!stored && lower_failures(context) > failures)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (!stored)
        return lower_internal(context, span,
                           "could not emit compound assignment store");
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

