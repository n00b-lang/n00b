#include "lower_internal.h"

static bool
n00bb_lower_shaped_assignment(ir_lower_context_t *context,
                              parse_visit_t *visit, parse_tree_t *target,
                              parse_tree_t *source);
static bool
n00bb_materialized_nil_source(parse_grammar_t *grammar, parse_tree_t *source,
                              th_type *target);
static ir_value_t *
n00bb_materialize_source(ir_lower_context_t *context, parse_visit_t *visit,
                         parse_tree_t *source, th_type *target);
static bool
n00bb_materialize_assignment_source(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *source, th_type *target,
                                    bool entering);
static parse_sym_entry_t *
n00bb_assignment_symbol(ir_lower_context_t *context, parse_visit_t *visit,
                        parse_tree_t *target);
static bool
n00bb_lower_flagset_field_assignment(ir_lower_context_t *context,
                                     parse_visit_t *visit,
                                     parse_tree_t *target,
                                     parse_tree_t *source,
                                     bool entering);
static bool
n00bb_suppress_dereferences(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *member);
static parse_sym_entry_t *
n00bb_initialized_self(ir_lower_context_t *context, parse_grammar_t *grammar,
                       parse_tree_t *target);
static ir_local_t *
n00bb_init_field_flag(ir_lower_context_t *context, core_string_t name,
                      ir_span_t span);
static bool
n00bb_finalize_replaced(ir_lower_context_t *context, ir_value_t *address,
                        ir_local_t *initialized, ir_span_t span);
static bool
n00bb_lower_open_field_assignment(ir_lower_context_t *context,
                                  parse_visit_t *visit, parse_tree_t *target,
                                  parse_tree_t *source, parse_tree_t *member,
                                  bool entering);
static bool
n00bb_lower_record_field_assignment(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *target,
                                    parse_tree_t *source,
                                    bool entering);
static bool
n00bb_lower_referent_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *target,
                                parse_tree_t *source, bool entering);
static bool
n00bb_lower_variant_arm_assignment(ir_lower_context_t *context,
                                   parse_visit_t *visit,
                                   parse_tree_t *target,
                                   parse_tree_t *source, bool entering);
static parse_sym_entry_t *
n00bb_compound_target_symbol(parse_tree_t *target);

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
n00bb_lower_shaped_assignment(ir_lower_context_t *context,
                              parse_visit_t *visit, parse_tree_t *target,
                              parse_tree_t *source)
{
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    parse_sym_entry_t *symbol = lvalue
        ? attached_symbol(visit->grammar,
            ((n00bb_lower_context_t *)context->user)->unit, lvalue) : nullptr;
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
    if (local) return n00bb_emit_local_write(context, local, value, span);
    if (!n00bb_emit_sequence_shape_check(context, value, required, span))
        return false;
    if (address)
        return n00bb_emit_indirect_write(context, address, value, span);
    if (binding && binding->kind == IR_VAL_GLOBAL
        && ir_type_eq(context->module, binding->type, value->type)) {
        ir_instr_t *store = ir_lower_emit_instruction(context,
            IR_OP_GLOBAL_STORE, span);
        if (!store || !ir_instr_set_binding_initialization(store, true)
            || !ir_instr_operand_value(store, binding)
            || !ir_instr_operand_value(store, value)) return false;
        return true;
    }
    lower_internal(context, span,
                   "shaped assignment target has no writable storage");
    return false;
}

static bool
n00bb_materialized_nil_source(parse_grammar_t *grammar, parse_tree_t *source,
                              th_type *target)
{
    return source && target && !th_is_nil(target)
        && n00bb_is_nil_literal(grammar, source);
}

static ir_value_t *
n00bb_materialize_source(ir_lower_context_t *context, parse_visit_t *visit,
                         parse_tree_t *source, th_type *target)
{
    ir_span_t span = lower_span_for_tree(context, source);
    ir_value_t *value = nullptr;
    if (n00bb_materialized_nil_source(visit->grammar, source, target))
        value = n00bb_emit_wrapping(context, IR_OP_NIL, nullptr,
            th_nil(ir_module_types(context->module)), span);
    else value = ir_lower_node_value(context, source);
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
        return !n00bb_materialized_nil_source(visit->grammar, source, target)
            || ir_lower_skip_value(context, visit->node, source);
    if (!n00bb_materialize_source(context, visit, source, target)) {
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not materialize the assigned value");
        return false;
    }
    return n00bb_move_source(context, visit->grammar, source);
}

static parse_sym_entry_t *
n00bb_assignment_symbol(ir_lower_context_t *context, parse_visit_t *visit,
                        parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("sym-decl")))
        return parse_tree_sym(visit->node);
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    return lvalue && lower
        ? attached_symbol(visit->grammar, lower->unit, lvalue) : nullptr;
}

parse_sym_entry_t *
n00bb_assignment_root_symbol(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             parse_tree_t *target)
{
    parse_tree_t *lvalue = n00bb_addressable_root_lvalue(grammar, target);
    return lvalue ? attached_symbol(grammar, unit, lvalue) : nullptr;
}

static bool
n00bb_lower_flagset_field_assignment(ir_lower_context_t *context,
                                     parse_visit_t *visit,
                                     parse_tree_t *target,
                                     parse_tree_t *source,
                                     bool entering)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, target);
    parse_sym_entry_t *flag = member && lower
        ? attached_symbol(visit->grammar, lower->unit, member) : nullptr;
    if (!lower || !n00bb_flagset_member(flag)
        || !n00bb_flagset_instance_access(visit->grammar, lower->unit,
                                           member, flag)) return true;
    if (entering)
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_skip_value(context, visit->node, target);

    (void)source;
    lower_report(context, N00B_DIAG_FLAGSET_FIELD_READ_ONLY,
                 lower_span_for_tree(context, target),
                 N00B_TEXT_FLAGSET_FIELD_IS_READ_ONLY,
                 N00BB_ARGS(ARG_TEXT(flag->name),
                            ARG_TEXT(flag->owner_type->name)));
    lower_help(context, N00B_TEXT_FLAGSET_FIELD_ASSIGN_WHOLE_HELP,
               N00BB_ARGS(ARG_TEXT(flag->name),
                          ARG_TEXT(flag->owner_type->name)));
    return true;
}

static bool
n00bb_suppress_dereferences(ir_lower_context_t *context,
                            parse_grammar_t *grammar, parse_tree_t *member)
{
    parse_tree_t *at = member && core_tree_num_children(member) >= 3
        ? core_tree_child(member, 0) : nullptr;
    for (parse_tree_t *inner = at ? member_access_node(grammar, at) : nullptr;
         inner && core_tree_num_children(inner) >= 3;
         inner = member_access_node(grammar, core_tree_child(inner, 0))) {
        parse_tree_t *name = core_tree_child(inner,
            core_tree_num_children(inner) - 1);
        if (!core_string_eq(first_token_text(name),
                            CORE_STRING_STATIC("__value"))) break;
        if (!ir_lower_suppress_emit(context, inner)) return false;
    }
    return true;
}

bool
n00bb_lower_field_receiver(ir_lower_context_t *context, parse_visit_t *visit,
                           bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    if (!entering || !lower || core_tree_num_children(visit->node) != 3)
        return true;
    parse_node_resolution_t resolution = parse_tree_resolution(visit->node);
    parse_sym_entry_t *field = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    parse_tree_t *receiver = core_tree_child(visit->node, 0);
    if (!field || !field->is_field
        || !referent_access_node(visit->grammar, receiver)) return true;
    return n00bb_suppress_dereferences(context, visit->grammar, visit->node);
}

static parse_sym_entry_t *
n00bb_initialized_self(ir_lower_context_t *context, parse_grammar_t *grammar,
                       parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    if (!lower || !lower->function_symbol
        || !core_string_eq(lower->function_symbol->name,
                           CORE_STRING_STATIC("__init"))) return nullptr;
    parse_tree_t *member = member_access_node(grammar, target);
    parse_tree_t *lvalue = member && core_tree_num_children(member)
        ? n00bb_simple_lvalue(core_tree_child(member, 0)) : nullptr;
    parse_sym_entry_t *symbol = lvalue
        ? attached_symbol(grammar, lower->unit, lvalue) : nullptr;
    return symbol && symbol->kind == PARSE_SYM_PARAM
        && core_string_eq(symbol->name, CORE_STRING_STATIC("self"))
        ? symbol : nullptr;
}

static bool
record_write_site(ir_lower_context_t *context, parse_grammar_t *grammar,
                  parse_tree_t *target, core_string_t field)
{
    n00bb_lower_context_t *lower = context->user;
    ir_block_t *block = context->block;
    ir_instr_t *store = block && block->n_instrs
        ? block->instrs[block->n_instrs - 1] : nullptr;
    for (parse_tree_t *step = target; step;) {
        if (n00bb_index_expression_node(grammar, step)) return true;
        parse_tree_t *member = member_access_node(grammar, step);
        step = member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
    }
    parse_tree_t *root = n00bb_addressable_root_lvalue(grammar, target);
    parse_sym_entry_t *symbol = root
        ? attached_symbol(grammar, lower->unit, root) : nullptr;
    if (store && store->op == IR_OP_STORE && symbol)
        core_list_push(tu_state(lower->unit)->write_sites,
            ((n00bb_write_site_t){target, symbol, store, field}));
    return true;
}

static bool
check_initialized_let(ir_lower_context_t *context, parse_grammar_t *grammar,
                      parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *symbol = attached_symbol(grammar, lower->unit, target);
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
                          parse_tree_t *target)
{
    n00bb_lower_context_t *lower = context->user;
    parse_sym_entry_t *symbol = attached_symbol(grammar, lower->unit, target);
    if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
        || symbol->mutability != PARSE_SYM_IMMUTABLE || !symbol->decl_node
        || child_nt(grammar, symbol->decl_node,
                    CORE_STRING_STATIC("expression")))
        return;
    parse_tree_t *loop = parse_tree_parent(statement);
    while (loop && !nt_is(grammar, loop, CORE_STRING_STATIC("for-stmt"))
           && !nt_is(grammar, loop, CORE_STRING_STATIC("while-stmt")))
        loop = parse_tree_parent(loop);
    for (parse_tree_t *at = symbol->decl_node; loop && at;
         at = parse_tree_parent(at))
        if (at == loop) return;
    if (!loop) return;
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


static ir_local_t *
n00bb_init_field_flag(ir_lower_context_t *context, core_string_t name,
                      ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    for (size_t i = 0; i < lower->init_fields.len; i++)
        if (core_string_eq(lower->init_fields.data[i].name, name))
            return lower->init_fields.data[i].flag;
    th_type *bool_type = n00bb_ir_builtin_type(context->module,
                                               CORE_STRING_STATIC("bool"));
    char buffer[48];
    core_string_t local_name = n00bb_format_name(buffer, sizeof(buffer),
        "__n00bb_initialized_%llu",
        (unsigned long long)context->next_value_id++);
    ir_local_t *flag = bool_type
        ? ir_local_add(context->function, local_name, bool_type) : nullptr;
    ir_block_t *entry = ir_block_entry(context->function);
    ir_instr_t *constant = flag && entry
        ? ir_instr_insert(entry, IR_OP_CONST, 0) : nullptr;
    ir_instr_t *clear = constant
        && ir_instr_set_fresh_dest(constant, "__n00bb_", bool_type)
        && ir_instr_operand_int(constant, 0)
        ? ir_instr_insert(entry, IR_OP_COPY, 1) : nullptr;
    if (!clear || !ir_instr_set_dest_local(clear, flag)
        || !ir_instr_operand_value(clear, constant->dest)) return nullptr;
    constant->span = clear->span = span;
    core_list_push(lower->init_fields, ((n00bb_init_field_t){name, flag}));
    return flag;
}

static bool
n00bb_finalize_replaced(ir_lower_context_t *context, ir_value_t *address,
                        ir_local_t *initialized, ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_type *type = th_ind_referent(th_resolve(address->type));
    if (!type || !n00bb_type_may_finalize(lower->unit, type)) return true;
    ir_block_t *join = nullptr;
    if (initialized) {
        ir_block_t *replace = n00bb_branch_block(context, lower, "replace");
        join = n00bb_branch_block(context, lower, "replaced");
        ir_value_t *flag = ir_local_value(context->function, initialized);
        if (!replace || !join || !flag
            || !n00bb_emit_branch(context, flag, replace, join, span))
            return false;
        context->block = replace;
    }
    ir_value_t *old = n00bb_emit_temp(context, IR_OP_COPY, address->type,
                                      address, span);
    if (!old || !n00bb_emit_finalize_value(context, old, type, span, 0))
        return false;
    if (!join) return true;
    if (!n00bb_emit_jump(context, join, span)) return false;
    context->block = join;
    return true;
}

static ir_value_t *
assigned_field_address(ir_lower_context_t *context, parse_grammar_t *grammar,
                       core_translation_unit_t *unit, parse_tree_t *target,
                       ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    bool saved = lower->write_back_open_index;
    lower->write_back_open_index = true;
    ir_value_t *address = n00bb_record_field_address(context, grammar, unit,
                                                     target, span);
    lower->write_back_open_index = saved;
    return address;
}

static bool
n00bb_lower_open_field_assignment(ir_lower_context_t *context,
                                  parse_visit_t *visit, parse_tree_t *target,
                                  parse_tree_t *source, parse_tree_t *member,
                                  bool entering)
{
    n00bb_lower_context_t *lower = context->user;
    if (entering)
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_suppress_emit(context, member)
            && n00bb_suppress_dereferences(context, visit->grammar, member);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    size_t failures = lower_failures(context);
    ir_value_t *address = assigned_field_address(context, visit->grammar,
        lower->unit, target, span);
    if (!address && lower_failures(context) > failures) return false;
    ir_value_t *value = source ? ir_lower_node_value(context, source)
                               : nullptr;
    n00bb_index_writeback_t writeback = {0};
    if (!address && value)
        address = n00bb_index_writeback_place(context, visit->grammar,
            lower->unit, target, &writeback, span);
    ir_instr_t *field = address && address->def
            && address->def->op == IR_OP_FIELD && address->def->n_operands == 2
            && address->def->operands[1].kind == IR_OPD_NAME
        ? address->def : nullptr;
    bool self = n00bb_initialized_self(context, visit->grammar, target);
    ir_local_t *initialized = self && field
        ? n00bb_init_field_flag(context, field->operands[1].name, span)
        : nullptr;
    if (!address || !value || (self && !initialized)
        || !n00bb_finalize_replaced(context, address, initialized, span)
        || !n00bb_emit_indirect_write(context, address, value, span)) {
        lower_internal(context, span,
            "could not lower an open field assignment");
        return false;
    }
    ir_block_t *block = context->block;
    ir_instr_t *store = block->n_instrs
        ? block->instrs[block->n_instrs - 1] : nullptr;
    if (store && store->op == IR_OP_STORE) n00bb_rewrite_later(context, store);
    if (field && !record_write_site(context, visit->grammar, target,
                                    field->operands[1].name))
        return false;
    if (initialized && !n00bb_set_flag(context, initialized, true, span))
        return false;
    if (n00bb_index_writeback_finish(context, visit->grammar, visit->node,
                                     &writeback, span))
        return n00bb_move_source(context, visit->grammar, source);
    lower_internal(context, span, "could not write back an indexed element");
    return false;
}

static bool
n00bb_lower_record_field_assignment(ir_lower_context_t *context,
                                    parse_visit_t *visit,
                                    parse_tree_t *target,
                                    parse_tree_t *source,
                                    bool entering)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, target);
    parse_sym_entry_t *field = member && lower
        ? attached_symbol(visit->grammar, lower->unit, member) : nullptr;
    if (member && lower && !field)
        return n00bb_lower_open_field_assignment(context, visit, target,
                                                 source, member, entering);
    if (!field || !field->is_field) return true;
    bool is_nil = n00bb_materialized_nil_source(visit->grammar, source,
                                                field->type);
    if (entering)
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_suppress_emit(context, member)
            && n00bb_suppress_dereferences(context, visit->grammar, member)
            && (!is_nil || ir_lower_skip_value(context, visit->node, source));

    ir_span_t span = lower_span_for_tree(context, visit->node);
    size_t failures = lower_failures(context);
    ir_value_t *address = assigned_field_address(context, visit->grammar,
        lower->unit, target, span);
    if (!address && lower_failures(context) > failures) return false;
    n00bb_index_writeback_t writeback = {0};
    if (!address)
        address = n00bb_index_writeback_place(context, visit->grammar,
            lower->unit, target, &writeback, span);
    th_type *resolved_field_type = address
        ? th_ind_referent(address->type) : field->type;
    ir_value_t *value = source
        ? n00bb_materialize_source(context, visit, source,
                                   resolved_field_type)
        : nullptr;
    if (!value || !address) {
        lower_internal(context, span,
            "record field assignment has no value or address");
        return false;
    }
    if (!n00bb_emit_sequence_shape_check(context, value,
            parse_sym_binding(field, CORE_STRING_STATIC("ir.array-shape")),
            span)) {
        lower_internal(context, span,
            "record field assignment does not match its array shape");
        return false;
    }
    parse_sym_entry_t *self = n00bb_initialized_self(context, visit->grammar,
                                                    target);
    ir_local_t *initialized = self && n00bb_type_may_finalize(lower->unit,
            th_ind_referent(th_resolve(address->type)))
        ? n00bb_init_field_flag(context, field->name, span) : nullptr;
    if ((self && n00bb_type_may_finalize(lower->unit,
                th_ind_referent(th_resolve(address->type))) && !initialized)
        || !n00bb_finalize_replaced(context, address, initialized, span)
        || !n00bb_emit_indirect_write(context, address, value, span)
        || !record_write_site(context, visit->grammar, target, field->name)
        || (initialized && !n00bb_set_flag(context, initialized, true, span))) {
        lower_internal(context, span,
            "could not emit the record field store");
        return false;
    }
    return n00bb_index_writeback_finish(context, visit->grammar, visit->node,
                                        &writeback, span)
        && n00bb_move_source(context, visit->grammar, source);
}

static bool
n00bb_lower_referent_assignment(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *target,
                                parse_tree_t *source, bool entering)
{
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *member = referent_access_node(visit->grammar, target);
    if (entering)
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_suppress_emit(context, member);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    size_t failures = lower_failures(context);
    ir_value_t *address = n00bb_record_field_address(context, visit->grammar,
        lower->unit, target, span);
    if (!address && lower_failures(context) > failures) return false;
    ir_value_t *value = address && source
        ? n00bb_materialize_source(context, visit, source,
                                   th_ind_referent(address->type))
        : nullptr;
    if (!value || !address) {
        if (!address)
            lower_report(context, N00B_DIAG_VALUE_NEEDS_REFERENCE,
                lower_span_for_tree(context, target),
                N00B_TEXT_VALUE_WRITES_THROUGH_A_REFERENCE,
                N00BB_NO_ARGS);
        else
            lower_internal(context, span, "`__value` assignment has no value");
        return false;
    }
    if (!n00bb_finalize_replaced(context, address, nullptr, span)
        || !n00bb_emit_indirect_write(context, address, value, span)) {
        lower_internal(context, span,
                    "could not emit the store through a reference");
        return false;
    }
    return n00bb_move_source(context, visit->grammar, source);
}

static bool
n00bb_lower_variant_arm_assignment(ir_lower_context_t *context,
                                   parse_visit_t *visit,
                                   parse_tree_t *target,
                                   parse_tree_t *source, bool entering)
{
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    parse_tree_t *member = member_access_node(visit->grammar, target);
    parse_sym_entry_t *arm = member && lower
        ? attached_symbol(visit->grammar, lower->unit, member) : nullptr;
    if (!lower || !n00bb_variant_arm_symbol(visit->grammar, arm)) return true;
    parse_tree_t *receiver_node = core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    parse_sym_entry_t *receiver = attached_symbol(visit->grammar, lower->unit,
                                                   receiver_node);
    if (!receiver || !receiver->type || !arm->type) {
        lower_internal(context, lower_span_for_tree(context, visit->node),
            "variant arm assignment has no resolved variant value or "
            "payload type");
        return false;
    }
    if (entering)
        return ir_lower_handle_assignment(context, visit->node)
            && ir_lower_skip_value(context, visit->node, target);

    ir_span_t span = lower_span_for_tree(context, visit->node);
    th_ctx *types = ir_module_types(context->module);
    th_type *variant_type = th_apply_subst(types, receiver->type);
    bool is_variant = false;
    if (!variant_type
        || !th_named_info(th_resolve(variant_type), nullptr, &is_variant,
                          nullptr)
        || !is_variant) {
        lower_internal(context, span,
            "variant arm assignment target does not have a variant type");
        return false;
    }
    ir_value_t *payload = source ? ir_lower_node_value(context, source) : nullptr;
    th_type *arm_type = n00bb_variant_arm_payload_type(types, variant_type,
                                                        arm);
    if (!payload || !arm_type) {
        lower_internal(context, span,
                       "variant arm assignment has no payload or type");
        return false;
    }

    ir_instr_t *make = ir_lower_emit_instruction(context, IR_OP_VARIANT_MAKE,
                                                 span);
    if (!make || !ir_lower_set_temp_dest(context, make,
        CORE_STRING_STATIC("__n00bb_variant_"), variant_type)
        || !ir_instr_operand_name(make, arm->name)
        || !ir_instr_operand_value(make, payload)) {
        lower_internal(context, span,
            "could not construct the selected variant arm");
        return false;
    }

    ir_value_t *address = n00bb_record_field_address(context, visit->grammar,
        lower->unit, receiver_node, span);
    if (!address
        || !n00bb_emit_indirect_write(context, address, make->dest, span)) {
        lower_internal(context, span,
            "could not store the selected variant arm");
        return false;
    }
    return true;
}

bool
n00bb_check_assignment(ir_lower_context_t *context, parse_visit_t *visit,
                       bool entering, void *state)
{
    (void)state;
    if (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("sym-decl"))) {
        parse_tree_t *target = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->name_ref);
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        parse_sym_entry_t *declared = parse_tree_sym(visit->node);
        th_type *expected = declared ? declared->type : nullptr;
        if (!n00bb_materialize_assignment_source(context, visit, source,
                                                 expected, entering))
            return false;
        parse_tree_t *lvalue = n00bb_simple_lvalue(target);
        n00bb_lower_context_t *lower = context->user;
        parse_sym_entry_t *symbol = lvalue
            ? attached_symbol(visit->grammar,
                lower ? lower->unit : nullptr, lvalue) : nullptr;
        ir_shape_t *shape = symbol ? parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape")) : nullptr;
        if (!n00bb_shape_has_constraints(shape)) return true;
        if (entering)
            return ir_lower_handle_assignment(context, visit->node);
        size_t failures = lower_failures(context);
        return n00bb_lower_shaped_assignment(context, visit, target, source)
            || lower_failures(context) == failures;
    }
    bool compound = nt_is(visit->grammar, visit->node,
                          CORE_STRING_STATIC("binop-assign-stmt"));
    if (!compound && !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("assign-stmt")))
        return true;
    if (!entering && ir_lower_emit_suppressed(context, visit->node))
        return true;
    parse_tree_t *target = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, visit->annot->name_ref);
    parse_tree_t *source = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, visit->annot->value_ref);
    n00bb_lower_context_t *lower = context->user;
    parse_tree_t *discard = n00bb_simple_lvalue(target);
    if (!compound && discard
        && core_string_eq(first_token_text(discard), CORE_STRING_STATIC("_")))
        return !entering || ir_lower_handle_assignment(context, visit->node);
    if (entering && discard && lower
        && ir_func_is_initializer(context->function)
        && !check_initialized_let(context, visit->grammar, discard))
        return false;
    if (entering && !compound && discard && lower)
        check_loop_initialization(context, visit->grammar, visit->node,
                                  discard);
    if (!compound && referent_access_node(visit->grammar, target))
        return n00bb_lower_referent_assignment(context, visit, target, source,
                                               entering);
    parse_tree_t *field_access = member_access_node(visit->grammar, target);
    parse_sym_entry_t *field = field_access && lower
        ? attached_symbol(visit->grammar, lower->unit, field_access) : nullptr;
    if (!compound && field && n00bb_flagset_member(field))
        return n00bb_lower_flagset_field_assignment(context, visit, target,
                                                     source, entering);
    if (!compound && field
        && n00bb_variant_arm_symbol(visit->grammar, field))
        return n00bb_lower_variant_arm_assignment(context, visit, target,
                                                   source, entering);
    if (!compound && field_access && (!field || field->is_field))
        return n00bb_lower_record_field_assignment(context, visit, target,
                                                    source, entering);
    if (entering && n00bb_atomic_member_lvalue_root(visit->grammar,
            lower ? lower->unit : nullptr, target)) {
        lower_report(context, N00B_DIAG_ATOMIC_PART_UPDATE,
            lower_span_for_tree(context, target),
            N00B_TEXT_AN_ATOMIC_VALUE_CANNOT_BE,
            N00BB_NO_ARGS);
        lower_help(context, N00B_TEXT_BUILD_THE_NEW_VALUE_AND, N00BB_NO_ARGS);
        return false;
    }
    if (!entering) {
        if (!compound && n00bb_has_index_store_target(visit->grammar, target)) {
            parse_tree_t *source = parse_tree_resolve_child_ref(
                visit->grammar, visit->node, visit->annot->value_ref);
            return n00bb_lower_index_store_at(context, visit->grammar,
                visit->node, target, source, nullptr);
        }
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        parse_sym_entry_t *assigned = n00bb_assignment_symbol(context, visit,
                                                               target);
        th_type *target_type = assigned ? assigned->type
            : target && !core_tree_is_leaf(target)
                ? n00bb_node_type(context, target) : nullptr;
        if (!compound && !n00bb_materialize_assignment_source(context, visit,
                source, target_type, false))
            return false;
        size_t failures = lower_failures(context);
        return n00bb_lower_shaped_assignment(context, visit, target, source)
            || lower_failures(context) == failures;
    }
    if (!compound && n00bb_has_index_store_target(visit->grammar, target)) {
        if (ir_lower_handle_assignment(context, visit->node)) return true;
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not claim indexed assignment lowering");
        return false;
    }
    if (!compound) {
        parse_tree_t *source = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, visit->annot->value_ref);
        parse_sym_entry_t *assigned = n00bb_assignment_symbol(context, visit,
                                                               target);
        if (!n00bb_materialize_assignment_source(context, visit, source,
                assigned ? assigned->type : nullptr, true)) return false;
    }
    parse_tree_t *lvalue = n00bb_simple_lvalue(target);
    parse_sym_entry_t *symbol = lvalue ? parse_tree_sym(lvalue) : nullptr;
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
    if (compound && entering
        && !ir_lower_evaluate_assignment_target(context, visit->node)) {
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not evaluate compound assignment target");
        return false;
    }
    ir_local_t *target_local = symbol
        ? ir_lower_bound_local(context, symbol) : nullptr;
    ir_value_t *target_binding = symbol
        ? ir_lower_bound_value(context, symbol) : nullptr;
    ir_shape_t *target_shape = target_local
        ? ir_local_shape(target_local)
        : target_binding ? ir_value_shape(target_binding) : nullptr;
    if (!target_shape && symbol)
        target_shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
    if (!compound && n00bb_shape_has_constraints(target_shape)) {
        if (!ir_lower_handle_assignment(context, visit->node)
            || !ir_lower_skip_value(context, visit->node, target)) {
            lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not claim shaped assignment lowering");
            return false;
        }
    }
    return true;
}

bool
n00bb_simple_assignment_field_target(parse_grammar_t *grammar,
                                     parse_tree_t *node)
{
    for (parse_tree_t *parent = parse_tree_parent(node);
         parent; parent = parse_tree_parent(parent)) {
        if (nt_is(grammar, parent, CORE_STRING_STATIC("binop-assign-stmt"))) return false;
        if (!nt_is(grammar, parent, CORE_STRING_STATIC("assign-stmt"))) continue;
        const parse_rule_t *rule = parse_tree_rule(grammar, parent);
        parse_annotation_t *assign = rule
            ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS)
            : nullptr;
        parse_tree_t *target = assign
            ? parse_tree_resolve_child_ref(grammar, parent,
                                           assign->name_ref) : nullptr;
        return member_access_node(grammar, target) == node;
    }
    return false;
}

static parse_sym_entry_t *
n00bb_compound_target_symbol(parse_tree_t *target)
{
    parse_node_resolution_t resolution = parse_tree_resolution(target);
    return resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol
        : target ? parse_tree_sym(target) : nullptr;
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
    parse_annotation_t *assigns = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_ASSIGNS) : nullptr;
    if (!assigns && visit->nt)
        assigns = parse_nt_find_direct_annot(visit->nt,
                                             PARSE_ANNOT_ASSIGNS);
    parse_tree_t *target = assigns
        ? parse_tree_resolve_child_ref(visit->grammar, visit->node,
                                       assigns->name_ref) : nullptr;
    parse_sym_entry_t *symbol = n00bb_compound_target_symbol(target);
    n00bb_lower_context_t *lower = context->user;
    bool field_target = member_access_node(visit->grammar, target) != nullptr;
    bool index_target = !field_target
        && n00bb_has_index_store_target(visit->grammar, target);
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    bool global = binding && binding->kind == IR_VAL_GLOBAL;
    th_type *target_type = !core_tree_is_leaf(target)
        ? n00bb_node_type(context, target) : nullptr;
    if (!target_type)
        target_type = local ? local->type
            : binding ? binding->type : symbol ? symbol->type : nullptr;
    if (!local && !address && !global && !field_target && !index_target)
        return lower_internal(context, span,
                           "compound assignment target has no storage");
    if (!target_type)
        return lower_internal(context, span,
                           "compound assignment target has no resolved type");

    const n00bb_binary_operator_t *operator = n00bb_binary_operator_assigned(
        first_token_text(core_tree_num_children(visit->node) > 1
            ? core_tree_child(visit->node, 1) : nullptr));
    if (!operator)
        return lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET, span,
            N00B_TEXT_NOT_SUPPORTED_YET_THIS_COMPOUND,
            N00BB_NO_ARGS);

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
    bool stored = false;
    if (updated && local)
        stored = n00bb_emit_local_write(context, local, updated, span);
    else if (updated && global) {
        ir_instr_t *store = ir_lower_emit_instruction(context,
            IR_OP_GLOBAL_STORE, span);
        stored = store && ir_instr_operand_value(store, binding)
            && ir_instr_operand_value(store, updated);
    }
    else if (updated && index_target)
        stored = n00bb_lower_index_store_at(context, visit->grammar,
            visit->node, target, core_tree_child(visit->node, 2), updated);
    else if (updated && field_target) {
        core_translation_unit_t *unit = lower ? lower->unit : nullptr;
        ir_value_t *field_address = assigned_field_address(context,
            visit->grammar, unit, target, span);
        n00bb_index_writeback_t writeback = {0};
        if (!field_address)
            field_address = n00bb_index_writeback_place(context,
                visit->grammar, unit, target, &writeback, span);
        stored = field_address
            && n00bb_emit_indirect_write(context, field_address, updated,
                                         span)
            && n00bb_index_writeback_finish(context, visit->grammar,
                                            visit->node, &writeback, span);
    }
    else if (updated)
        stored = n00bb_emit_indirect_write(context, address, updated, span);
    if (!stored)
        return lower_internal(context, span,
                           "could not emit compound assignment store");
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

bool
n00bb_capture_compound_target(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering, void *state)
{
    (void)state;
    parse_tree_t *parent = parse_tree_parent(visit->node);
    if (entering || !context->reachable || !parent
        || !nt_is(visit->grammar, parent, CORE_STRING_STATIC("binop-assign-stmt"))
        || core_tree_child(parent, 0) != visit->node
        || (nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("place"))
            && !n00bb_name_place(visit->grammar, visit->node))) return true;
    parse_sym_entry_t *symbol = n00bb_compound_target_symbol(visit->node);
    ir_local_t *local = ir_lower_bound_local(context, symbol);
    ir_value_t *address = ir_lower_bound_address(context, symbol);
    ir_value_t *binding = ir_lower_bound_value(context, symbol);
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_value_t *value = ir_lower_node_value(context, visit->node);
    if (!value && core_tree_num_children(visit->node) == 1)
        value = ir_lower_node_value(context,
                                    core_tree_child(visit->node, 0));
    if (value && value->kind != IR_VAL_GLOBAL) return true;
    if (value && value->kind == IR_VAL_GLOBAL) binding = value;
    value = nullptr;
    value = local ? n00bb_emit_local_read(context, local, span)
        : address && symbol
            ? n00bb_emit_indirect_read(context, address, symbol->type, span)
            : nullptr;
    if (!value && binding && binding->kind == IR_VAL_GLOBAL) {
        ir_instr_t *snapshot = ir_lower_emit_instruction(context, IR_OP_COPY,
                                                          span);
        if (snapshot && ir_lower_set_temp_dest(context, snapshot,
            CORE_STRING_STATIC("__n00bb_"), binding->type)
            && ir_instr_operand_value(snapshot, binding)) {
            value = snapshot->dest;
        }
    }
    if (value && ir_lower_set_node_value(context, visit->node, value))
        return true;
    lower_internal(context, span,
                   "compound assignment target has no storage");
    return false;
}
