#include "lower_internal.h"

static bool
collect_sequence_literal_values(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *node,
                                core_list_t(n00bb_ir_value_ptr_t) *values);
static n00bb_tuple_parts_t *
n00bb_tuple_parts(n00bb_lower_context_t *lower, parse_tree_t *node,
                 bool create);
static ir_lower_result_t
n00bb_lower_literal_constructor(ir_lower_context_t *context,
                                th_type *result_type, ir_value_t *seed,
                                ir_span_t span);
static ir_lower_result_t
n00bb_lower_open_sequence_literal(ir_lower_context_t *context,
                                  parse_visit_t *visit, th_type *result_type,
                                  ir_span_t span);
static ir_lower_result_t
n00bb_lower_sequence_aggregate(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state);
static ir_lower_result_t
n00bb_lower_open_brace_literal(ir_lower_context_t *context,
                               parse_visit_t *visit, th_type *result_type,
                               ir_span_t span);
static ir_lower_result_t
n00bb_lower_brace_payload(ir_lower_context_t *context,
                          parse_visit_t *visit, bool dictionary,
                          th_type *payload_type, ir_span_t span);
static ir_lower_result_t
n00bb_lower_user_brace_payload(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               n00bb_litmod_site_t *site, ir_span_t span);
static ir_lower_result_t
n00bb_lower_user_parenthesized_payload(ir_lower_context_t *context,
                                       parse_visit_t *visit,
                                       n00bb_litmod_site_t *site,
                                       ir_span_t span);
static ir_value_t *
n00bb_lower_literal_site_member(ir_lower_context_t *context,
                                const th_member *member, core_string_t path,
                                uint64_t line, uint64_t column, ir_span_t span);
static ir_value_t *
n00bb_lower_literal_site(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_span_t span);

static bool
collect_sequence_literal_values(ir_lower_context_t *context,
                                parse_visit_t *visit, parse_tree_t *node,
                                core_list_t(n00bb_ir_value_ptr_t) *values)
{
    if (!node) return true;
    parse_tree_t *entries = nullptr;
    if (nt_is(visit->grammar, node, CORE_STRING_STATIC("list-lit"))) {
        entries = child_nt(visit->grammar, node, CORE_STRING_STATIC("list-elems"));
    }
    else if (nt_is(visit->grammar, node, CORE_STRING_STATIC("dict-or-set-lit"))) {
        entries = child_nt(visit->grammar, node, CORE_STRING_STATIC("set-entries"));
        if (!entries)
            entries = child_nt(visit->grammar, node, CORE_STRING_STATIC("dict-entries"));
    }
    else return false;
    if (!entries) return true;
    n00bb_lower_context_t *lower = context ? context->user : nullptr;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_ordered_nodes_t *cached = tu
        ? n00bb_node_ordered_nodes(entries) : nullptr;
    if (!cached) return false;
    for (size_t i = 0; i < cached->count; i++) {
        ir_value_t *value = ir_lower_node_value(context, cached->nodes[i]);
        if (!value || !n00bb_move_source(context, visit->grammar,
                                         cached->nodes[i])) return false;
        core_list_push(*values, value);
    }
    return true;
}

static n00bb_tuple_parts_t *
n00bb_tuple_parts(n00bb_lower_context_t *lower, parse_tree_t *node,
                 bool create)
{
    if (!lower || !node) return nullptr;
    for (size_t i = 0; i < lower->tuple_parts.len; i++)
        if (lower->tuple_parts.data[i].node == node)
            return &lower->tuple_parts.data[i];
    if (!create) return nullptr;
    if (!lower->tuple_parts.data)
        lower->tuple_parts = core_list_new(n00bb_tuple_parts_t);
    core_list_push(lower->tuple_parts, ((n00bb_tuple_parts_t){
        .node = node,
        .expressions = core_list_new(parse_tree_ptr_t),
        .values = core_list_new(n00bb_ir_value_ptr_t),
    }));
    return &lower->tuple_parts.data[lower->tuple_parts.len - 1];
}

bool
n00bb_lower_tuple_parts(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("expr-comma-list")))
        return true;
    n00bb_lower_context_t *lower = context->user;
    size_t child_count = core_tree_num_children(visit->node);
    if (child_count < 1 || child_count > 3)
        return lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "tuple expression list has an unexpected shape").status
            != IR_LOWER_FAILED;
    parse_tree_t *head = core_tree_child(visit->node, 0);
    ir_value_t *head_value = ir_lower_node_value(context, head);
    n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, visit->node, true);
    if (!parts) return false;
    core_list_push(parts->expressions, head);
    core_list_push(parts->values, head_value);
    if (child_count == 3) {
        parse_tree_t *tail_node = core_tree_child(visit->node, 2);
        n00bb_tuple_parts_t *tail = n00bb_tuple_parts(lower, tail_node, false);
        if (!tail)
            return lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "tuple expression list lost its trailing elements").status
                != IR_LOWER_FAILED;
        for (size_t i = 0; i < tail->expressions.len; i++) {
            core_list_push(parts->expressions, tail->expressions.data[i]);
            core_list_push(parts->values, tail->values.data[i]);
        }
    }
    return true;
}

bool
n00bb_lower_named_field_parts(ir_lower_context_t *context,
                              parse_visit_t *visit, bool entering,
                              void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node,
                           CORE_STRING_STATIC("named-field-list"))) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, visit->node, true);
    if (!parts) return false;
    for (size_t i = 0; i < core_tree_num_children(visit->node); i++) {
        parse_tree_t *child = core_tree_child(visit->node, i);
        if (nt_is(visit->grammar, child, CORE_STRING_STATIC("named-field-list"))) {
            n00bb_tuple_parts_t *tail = n00bb_tuple_parts(lower, child, false);
            if (!tail) return false;
            for (size_t j = 0; j < tail->expressions.len; j++) {
                core_list_push(parts->expressions, tail->expressions.data[j]);
                core_list_push(parts->values, tail->values.data[j]);
            }
        }
        else if (nt_is(visit->grammar, child, CORE_STRING_STATIC("named-field"))) {
            parse_tree_t *expression = nullptr;
            for (size_t j = 0; j < core_tree_num_children(child); j++) {
                parse_tree_t *part = core_tree_child(child, j);
                if (nt_is(visit->grammar, part, CORE_STRING_STATIC("expression"))) {
                    expression = part;
                    break;
                }
            }
            ir_value_t *value = expression
                ? ir_lower_node_value(context, expression) : nullptr;
            if (!expression || !value
                || !n00bb_move_source(context, visit->grammar, expression))
                return false;
            core_list_push(parts->expressions, expression);
            core_list_push(parts->values, value);
        }
    }
    return parts->values.len != 0;
}

ir_value_t *
n00bb_union_filler(ir_lower_context_t *context, th_type *type, ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *resolved = th_resolve(type);
    if (th_option_item(resolved))
        return n00bb_emit_wrapping(context, IR_OP_OPTION_NONE, nullptr, type,
                                   span);
    ir_machine_t machine = ir_machine_of(context->module, resolved);
    if (machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT) {
        ir_instr_t *zero = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
        return zero && ir_lower_set_temp_dest(context, zero, CORE_STRING_STATIC("__n00bb_"), type)
            && ir_instr_operand_int_lit(zero, ir_lit_of(0))
            ? zero->dest : nullptr;
    }
    th_type *definition = th_kind_of(resolved) == TH_RECORD ? resolved
        : th_type_definition(types, type);
    if (!definition || th_kind_of(th_resolve(definition)) != TH_RECORD)
        return nullptr;
    definition = th_resolve(definition);
    size_t count = th_record_len(definition);
    ir_value_t **members = count
        ? core_alloc_array(ir_value_t *, count) : nullptr;
    bool ok = !count || members;
    for (size_t i = 0; ok && i < count; i++) {
        const th_member *member = th_record_member(definition, i);
        members[i] = member
            ? n00bb_union_filler(context, member->type, span) : nullptr;
        ok = members[i] != nullptr;
    }
    ir_instr_t *aggregate = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = aggregate && ir_lower_set_temp_dest(context, aggregate, CORE_STRING_STATIC("__n00bb_"),
                                             type);
    for (size_t i = 0; ok && i < count; i++)
        ok = ir_instr_operand_value(aggregate, members[i]);
    core_free(members);
    return ok ? aggregate->dest : nullptr;
}

ir_value_t *
n00bb_object_allocate(ir_lower_context_t *context, th_type *object_type,
                      ir_value_t **storage, ir_span_t span)
{
    th_type *reference = n00bb_reference_definition(
        ir_module_types(context->module), object_type);
    th_type *shape = reference ? th_ind_referent(reference) : nullptr;
    ir_instr_t *allocation = shape
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    if (!allocation
        || !ir_lower_set_temp_dest(context, allocation,
                                   CORE_STRING_STATIC("__n00bb_object_"),
                                   reference)
        || !ir_instr_operand_type(allocation, shape)
        || !ir_instr_set_fresh(allocation)) return nullptr;
    ir_instr_t *conversion = ir_lower_emit_instruction(context,
        IR_OP_NOMINAL_CAST, span);
    if (!conversion || !ir_lower_set_temp_dest(context, conversion,
                                               CORE_STRING_STATIC("__n00bb_"),
                                               object_type)
        || !ir_instr_operand_value(conversion, allocation->dest))
        return nullptr;
    if (storage) *storage = allocation->dest;
    return conversion->dest;
}

ir_value_t *
n00bb_emit_fresh_object(ir_lower_context_t *context, ir_value_t *storage,
                        th_type *object_type, ir_span_t span)
{
    ir_value_t *object = n00bb_object_allocate(context, object_type, nullptr,
                                               span);
    return object && n00bb_emit_indirect_write(context, storage, object, span)
        ? object : nullptr;
}

ir_value_t *
n00bb_object_from_fields(ir_lower_context_t *context, ir_value_t *fields,
                         th_type *object_type, ir_span_t span)
{
    ir_value_t *storage = nullptr;
    ir_value_t *object = fields
        ? n00bb_object_allocate(context, object_type, &storage, span)
        : nullptr;
    return object && n00bb_emit_indirect_write(context, storage, fields, span)
        ? object : nullptr;
}

bool
n00bb_lower_tuple_literal(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state)
{
    (void)state;
    if (entering || !nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("tuple-or-paren")))
        return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_explicit_literal_site(visit->grammar, lower->unit, visit->node) : nullptr;
    if (site) {
        if ((lower->has_active_literal
                && lower->active_literal.node == visit->node)
            || ir_lower_node_value(context, visit->node)) return true;
        ir_lower_result_t result = n00bb_lower_user_literal(context, visit,
            site, n00bb_node_type(context, visit->node),
            lower_span_for_tree(context, visit->node));
        return result.status == IR_LOWER_VALUE && result.value
            && ir_lower_set_node_value(context, visit->node, result.value);
    }
    size_t child_count = core_tree_num_children(visit->node);
    parse_tree_t *named_fields = child_nt(visit->grammar, visit->node,
                                          CORE_STRING_STATIC("named-field-list"));
    if (named_fields) {
        bool compound = false;
        if (n00bb_assignment_target(visit->grammar, visit->node, &compound))
            return true;
        n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, named_fields,
                                                       false);
        th_type *type = n00bb_node_type(context, visit->node);
        if (parts && parts->values.len
            && (!type || th_kind_of(th_resolve(type)) == TH_VAR)) {
            ir_value_t *built = n00bb_open_record_literal(context, parts,
                lower_span_for_tree(context, visit->node));
            return built
                ? ir_lower_set_node_value(context, visit->node, built)
                : lower_internal(context,
                      lower_span_for_tree(context, visit->node),
                      "could not lower an open record literal")
                      .status != IR_LOWER_FAILED;
        }
        if (!parts || !parts->values.len || !type)
            return lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "named-field literal is missing its inferred type or values")
                .status != IR_LOWER_FAILED;
        ir_span_t span = lower_span_for_tree(context, visit->node);
        th_type *object_type = n00bb_object_type(
            ir_module_types(context->module), type) ? type : nullptr;
        th_type *built_type = object_type ? object_type : type;
        if (object_type)
            type = th_auto_deref(ir_module_types(context->module), type);
        bool variant = false;
        (void)th_named_info(th_resolve(type), nullptr, &variant, nullptr);
        if (!variant && th_kind_of(th_resolve(built_type)) == TH_NAMED
            && n00bb_type_has_constructor(lower->unit, built_type)) {
            core_string_t type_text = lower_type_text(context, built_type);
            lower_report(context, N00B_DIAG_RECORD_HAS_CONSTRUCTOR, span,
                N00B_TEXT_HAS_AN_INIT_SO_A,
                N00BB_ARGS(ARG_TEXT(type_text)));
            lower_help(context, N00B_TEXT_TUPLE_CONSTRUCTOR_HELP,
                       N00BB_ARGS(ARG_TEXT(type_text)));
            core_free(type_text.data);
            return false;
        }
        if (variant) {
            n00bb_ordered_nodes_t *ordered = tu
                ? n00bb_node_ordered_nodes(named_fields) : nullptr;
            bool valid = parts->values.len == 1 && ordered
                && ordered->count == 1;
            core_string_t arm = valid
                ? first_token_text(ordered->nodes[0]) : (core_string_t){0};
            if (!arm.data)
                return lower_internal(context, span,
                    "variant literal does not name a single arm")
                    .status != IR_LOWER_FAILED;
            ir_instr_t *make = ir_lower_emit_instruction(context,
                                                          IR_OP_VARIANT_MAKE,
                                                          span);
            bool ok = make && ir_lower_set_temp_dest(context, make,
                CORE_STRING_STATIC("__n00bb_variant_"), type)
                && ir_instr_operand_name(make, arm)
                && ir_instr_operand_value(make, parts->values.data[0]);
            if (!ok)
                return lower_internal(context, span,
                    "could not lower the selected variant arm")
                    .status != IR_LOWER_FAILED;
            return ir_lower_set_node_value(context, visit->node, make->dest);
        }
        th_type *definition = th_kind_of(th_resolve(type)) == TH_RECORD
            ? th_resolve(type)
            : th_type_definition(ir_module_types(context->module), type);
        ir_instr_t *aggregate = ir_lower_emit_instruction(context,
            IR_OP_AGGREGATE, span);
        bool ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
            CORE_STRING_STATIC("__n00bb_"), type);
        bool by_name = definition
            && th_record_len(definition) == parts->values.len;
        for (size_t j = 0; ok && j < parts->values.len; j++) {
            size_t i = j;
            if (by_name) {
                core_string_t member = th_record_member(definition, j)->name;
                for (i = 0; i < parts->values.len; i++) {
                    parse_tree_t *field = parse_tree_parent(
                        parts->expressions.data[i]);
                    if (field && core_string_eq(first_token_text(field),
                                                member)) break;
                }
            }
            ok = i < parts->values.len
                && ir_instr_operand_value(aggregate, parts->values.data[i]);
        }
        if (!ok)
            return lower_internal(context,
                lower_span_for_tree(context, visit->node),
                "could not emit named-field aggregate")
                .status != IR_LOWER_FAILED;
        ir_value_t *built = aggregate->dest;
        if (object_type) {
            built = n00bb_object_from_fields(context, built, object_type,
                                             span);
            if (!built)
                return lower_internal(context, span,
                    "could not allocate the object")
                    .status != IR_LOWER_FAILED;
        }
        return built && ir_lower_set_node_value(context, visit->node, built);
    }
    if (child_count == 3
        && nt_is(visit->grammar, core_tree_child(visit->node, 1),
                 CORE_STRING_STATIC("expression"))) {
        ir_value_t *value = ir_lower_node_value(context,
                                                core_tree_child(visit->node, 1));
        if (value) return ir_lower_set_node_value(context, visit->node, value);
        bool compound = false;
        return n00bb_assignment_target(visit->grammar, visit->node, &compound);
    }
    if (child_count != 5
        || !nt_is(visit->grammar, core_tree_child(visit->node, 1),
                  CORE_STRING_STATIC("expression"))) return true;
    bool compound = false;
    if (n00bb_assignment_target(visit->grammar, visit->node, &compound))
        return true;
    n00bb_tuple_parts_t *parts = n00bb_tuple_parts(lower, visit->node, true);
    parse_tree_t *first = core_tree_child(visit->node, 1);
    parse_tree_t *rest_node = core_tree_child(visit->node, 3);
    n00bb_tuple_parts_t *rest = n00bb_tuple_parts(lower, rest_node, false);
    ir_value_t *first_value = ir_lower_node_value(context, first);
    th_type *type = n00bb_node_type(context, visit->node);
    if (!parts || !rest || !first_value)
        return lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "tuple literal is missing an inferred type or element")
            .status != IR_LOWER_FAILED;
    core_list_push(parts->expressions, first);
    core_list_push(parts->values, first_value);
    for (size_t i = 0; i < rest->expressions.len; i++) {
        core_list_push(parts->expressions, rest->expressions.data[i]);
        core_list_push(parts->values, rest->values.data[i]);
    }
    if (!type || th_kind_of(th_resolve(type)) == TH_VAR) {
        th_type **elements = core_alloc_array(th_type *, parts->values.len);
        bool complete = true;
        for (size_t i = 0; i < parts->values.len; i++) {
            complete &= parts->values.data[i] != nullptr;
            elements[i] = th_var(ir_module_types(context->module));
        }
        type = complete ? th_tuple(ir_module_types(context->module), elements,
                                   parts->values.len)
                        : nullptr;
        core_free(elements);
    }
    for (size_t i = 0; i < parts->expressions.len; i++)
        if (!n00bb_move_source(context, visit->grammar,
                               parts->expressions.data[i])) return false;
    ir_span_t span = lower_span_for_tree(context, visit->node);
    ir_instr_t *aggregate = ir_lower_emit_instruction(context,
                                                       IR_OP_AGGREGATE, span);
    bool ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
                                                   CORE_STRING_STATIC("__n00bb_"), type);
    for (size_t i = 0; ok && i < parts->values.len; i++) {
        if (!parts->values.data[i]) ok = false;
        else ok = ir_instr_operand_value(aggregate, parts->values.data[i]);
    }
    if (!ok)
        return lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not emit tuple aggregate").status != IR_LOWER_FAILED;
    return ir_lower_set_node_value(context, visit->node, aggregate->dest);
}

ir_lower_result_t
n00bb_lower_char_literal(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_value_t *const *operands, size_t operand_count,
                         th_type *result_type, ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    parse_token_info_t *token = visit && visit->node
        ? parse_tree_find_first_terminal(visit->node) : nullptr;
    uint32_t scalar = 0;
    core_option_t(core_string_t) literal = token
        ? parse_token_value(token) : core_option_none(core_string_t);
    if (operand_count || !result_type || !token
        || !core_option_is_set(literal)
        || !n00bb_decode_char_literal(core_option_get(literal), &scalar))
        return lower_internal(context, span,
                              "character literal has no valid scalar value");
    if (n00bb_open_type(context, result_type))
        result_type = n00bb_ir_builtin_type(context->module,
                                            CORE_STRING_STATIC("char"));
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant || !ir_lower_set_temp_dest(context, constant, CORE_STRING_STATIC("__n00bb_"),
                                              result_type)
        || !ir_instr_operand_uint(constant, scalar))
        return lower_internal(context, span,
                           "could not emit character literal");
    return (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
}

ir_lower_result_t
n00bb_emit_char_array(ir_lower_context_t *context, const uint8_t *bytes,
                      size_t length, th_type *result_type, ir_span_t span)
{
    th_type *resolved = th_resolve(result_type);
    th_type *element_type = th_kind_of(resolved) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(resolved)) : nullptr;
    bool ok = n00bb_scalar_role_is(element_type, IR_ROLE_CHAR);
    if (length > UINT32_MAX) {
        return lower_report(context, N00B_DIAG_NOT_SUPPORTED_YET, span,
            N00B_TEXT_NOT_SUPPORTED_YET_A_CHARACTER,
            N00BB_NO_ARGS);
    }
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    uint32_t position = 0;
    while (ok && position < length) {
        int32_t codepoint = unicode_utf8_decode((const char *)bytes,
            (uint32_t)length, &position);
        if (codepoint < 0) {
            ok = false;
            break;
        }
        ir_instr_t *constant = ir_lower_emit_instruction(context,
                                                          IR_OP_CONST,
                                                          span);
        ok = constant
            && ir_lower_set_temp_dest(context, constant, CORE_STRING_STATIC("__n00bb_"),
                                       element_type)
            && ir_instr_operand_uint(constant, (uint32_t)codepoint);
        if (ok) {
            core_list_push(values, constant->dest);
        }
    }
    ir_instr_t *aggregate = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span)
        : nullptr;
    ok = ok && aggregate
        && ir_lower_set_temp_dest(context, aggregate, CORE_STRING_STATIC("__n00bb_"),
                                  result_type);
    for (size_t i = 0; ok && i < values.len; i++)
        ok = ir_instr_operand_value(aggregate, values.data[i]);
    ir_shape_t *shape = ok
        ? ir_shape_new(((n00bb_lower_context_t *)context->user)->ir_context,
                       element_type, 1) : nullptr;
    ok = ok && shape && ir_shape_set_extent(shape, 0,
        (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = values.len,
        }) && ir_value_set_shape(aggregate->dest, shape);
    core_list_free(values);
    if (!ok)
        return lower_internal(context, span,
            "character-array literal has invalid UTF-8 or could not be "
            "built");
    return (ir_lower_result_t){IR_LOWER_VALUE, aggregate->dest};
}

ir_lower_result_t
n00bb_lower_string_literal(ir_lower_context_t *context,
                           parse_visit_t *visit,
                           ir_value_t *const *operands,
                           size_t operand_count, th_type *result_type,
                           ir_span_t span, void *state)
{
    (void)operands;
    (void)state;
    parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
    core_option_t(core_string_t) literal = token
        ? parse_token_value(token) : core_option_none(core_string_t);
    if (operand_count || !token || !core_option_is_set(literal))
        return lower_internal(context, span,
                              "string literal has no decoded token value");
    core_string_t spelling = core_option_get(literal);
    core_string_t modifier = literal_modifier(visit->grammar, visit->node);
    const n00bb_literal_registration_t *registration = modifier.data
        ? n00bb_literal_handler(modifier, N00BB_LITERAL_STRING_SHAPE)
        : nullptr;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *user_site = tu
        ? n00bb_explicit_literal_site(visit->grammar, lower->unit, visit->node) : nullptr;
    if (user_site)
        return n00bb_lower_user_literal(context, visit, user_site,
                                        result_type, span);
    if (modifier.data && !registration
        && !n00bb_literal_prefix_registered(modifier))
        return lower_report(context, N00B_DIAG_UNKNOWN_LITERAL_MODIFIER, span,
            N00B_TEXT_NO_LITERAL_MODIFIER_IS_DEFINED,
            N00BB_ARGS(ARG_TEXT(modifier)));
    if (!result_type)
        return lower_internal(context, span,
                              "string literal has no inferred result type");
    core_buffer_t *decoded = core_buffer_empty();
    bool ok = decoded != nullptr
        && n00bb_decode_string_literal(spelling, decoded);
    if (!ok) {
        core_buffer_free(decoded);
        return lower_internal(context, span,
                              "string literal escape sequence did not decode");
    }
    if (registration && registration->handler == N00BB_LITERAL_U32) {
        th_ctx *types = ir_module_types(context->module);
        result_type = th_array(types, th_name(types,
                                              CORE_STRING_STATIC("char")));
    }
    th_type *resolved = th_resolve(result_type);
    th_type *element_type = th_kind_of(resolved) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(resolved)) : nullptr;
    if (n00bb_scalar_role_is(element_type, IR_ROLE_CHAR)) {
        ir_lower_result_t array = n00bb_emit_char_array(context,
            (const uint8_t *)decoded->data, decoded->byte_len, result_type,
            span);
        core_buffer_free(decoded);
        return array;
    }
    ir_instr_t *instruction = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                        span);
    ok = instruction && ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), result_type)
        && ir_instr_set_const_bytes(instruction,
            (const uint8_t *)decoded->data, decoded->byte_len);
    if (ok && !registration && n00bb_open_type(context, result_type))
        n00bb_rewrite_later(context, instruction);
    if (ok && registration && lower && lower->ir_context)
        ok = ir_annot_set(lower->ir_context, &instruction->annots,
            (ir_annot_t){
                .key = CORE_STRING_STATIC("n00bb.literal.modifier"),
                .kind = IR_ANNOT_STR,
                .s = ir_strdup(lower->ir_context,
                               registration->canonical_name),
            });
    core_buffer_free(decoded);
    if (!ok)
        return lower_internal(context, span,
                           "could not emit a byte-exact string constant");
    return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

static ir_lower_result_t
n00bb_lower_literal_constructor(ir_lower_context_t *context,
                                th_type *result_type, ir_value_t *seed,
                                ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    th_type *address_type = result_type && seed
        ? n00bb_indirect_type(types, result_type) : nullptr;
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, span) : nullptr;
    bool ok = allocation
        && ir_lower_set_temp_dest(context, allocation,
               CORE_STRING_STATIC("__n00bb_literal_storage_"), address_type)
        && ir_instr_operand_type(allocation, result_type);
    ir_instr_t *init = ok
        ? ir_lower_emit_instruction(context, IR_OP_DISPATCH, span) : nullptr;
    ok = init
        && ir_lower_set_temp_dest(context, init,
               CORE_STRING_STATIC("__n00bb_literal_init_"), th_nil(types))
        && ir_instr_operand_name(init, CORE_STRING_STATIC("__init"))
        && ir_instr_operand_fresh(init, allocation->dest)
        && ir_instr_operand_value(init, seed);
    if (!ok)
        return lower_internal(context, span,
                           "could not emit the literal constructor dispatch");
    n00bb_rewrite_later(context, init);
    ir_value_t *value = n00bb_emit_indirect_read(context, allocation->dest,
                                                 result_type, span);
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
        : lower_internal(context, span,
                      "could not read the constructed literal");
}

static ir_lower_result_t
n00bb_lower_open_sequence_literal(ir_lower_context_t *context,
                                  parse_visit_t *visit, th_type *result_type,
                                  ir_span_t span)
{
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = collect_sequence_literal_values(context, visit, visit->node,
                                              &values);
    ir_instr_t *instruction = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span)
        : nullptr;
    ok = instruction
        && ir_lower_set_temp_dest(context, instruction,
               CORE_STRING_STATIC("__n00bb_literal_"), result_type)
        && ir_instr_set_aggregate_form(instruction, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < values.len; i++)
        ok = ir_instr_operand_value(instruction, values.data[i]);
    core_list_free(values);
    if (ok) n00bb_rewrite_later(context, instruction);
    return ok ? (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest}
        : lower_internal(context, span,
                         "could not lower an open sequence literal");
}

static ir_lower_result_t
n00bb_lower_sequence_aggregate(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    if (!result_type)
        return lower_internal(context, span,
                              "sequence literal has no inferred type");
    th_type *resolved_type = th_resolve(result_type);
    if (th_kind_of(resolved_type) == TH_VAR) {
        const n00bb_literal_registration_t *handler = n00bb_literal_handler(
            literal_modifier(visit->grammar, visit->node),
            N00BB_LITERAL_SEQUENCE_SHAPE);
        th_ctx *types = ir_module_types(context->module);
        return n00bb_lower_open_sequence_literal(context, visit,
            handler && handler->handler == N00BB_LITERAL_ARRAY
                ? th_array(types, th_var(types)) : result_type,
            span);
    }
    bool is_list = th_builtin_applied(resolved_type, TH_BUILTIN_CONTAINER_LIST, 1)
        != nullptr;
    bool is_array = th_kind_of(resolved_type) == TH_SEQUENCE;
    if (!is_array && !is_list)
        return lower_internal(context, span,
            "bracket literal has no resolved array or list type");
    const th_arg *list_element = is_list
        ? th_named_arg(resolved_type, 0) : nullptr;
    th_type *element_type = is_array ? th_seq_elem(resolved_type)
        : list_element && list_element->kind == TH_ARG_TYPE
            ? list_element->type : nullptr;
    th_type *aggregate_type = is_list && element_type
        ? th_array(ir_module_types(context->module), element_type)
        : result_type;
    if (!aggregate_type || !element_type)
        return lower_internal(context, span,
                              "sequence literal element type is unresolved");
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool collected = collect_sequence_literal_values(context, visit,
                                                      visit->node, &values);
    if (!collected) {
        core_list_free(values);
        return lower_internal(context, span,
                              "sequence literal element has no value");
    }
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
                                                        IR_OP_AGGREGATE, span);
    bool ok = instruction && ir_lower_set_temp_dest(context, instruction,
                                                CORE_STRING_STATIC("__n00bb_"), aggregate_type);
    if (ok) ok = ir_instr_set_aggregate_form(instruction,
                                             IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < values.len; i++)
        ok = ir_instr_operand_value(instruction, values.data[i]);
    th_type *element = th_resolve(aggregate_type);
    uint32_t rank = 0;
    while (ok && th_kind_of(element) == TH_SEQUENCE) {
        element = th_resolve(th_seq_elem(element));
        if (!element || rank == UINT32_MAX) ok = false;
        else rank++;
    }
    ir_shape_t *shape = ok && rank
        ? ir_shape_new(((n00bb_lower_context_t *)context->user)->ir_context,
                       element, rank) : nullptr;
    ok = ok && (!rank || shape);
    for (uint32_t d = 0; ok && d < rank; d++) {
        ir_shape_extent_t extent = {.kind = IR_SHAPE_EXTENT_UNRESOLVED};
        if (!d) {
            extent.kind = IR_SHAPE_EXTENT_STATIC;
            extent.static_value = values.len;
        }
        else if (values.len) {
            ir_shape_t *first_shape = ir_value_shape(values.data[0]);
            bool common_static = first_shape != nullptr;
            ir_shape_extent_t first_extent = {0};
            common_static &= ir_shape_extent(first_shape, d - 1,
                                               &first_extent)
                && first_extent.kind == IR_SHAPE_EXTENT_STATIC;
            for (size_t i = 1; common_static && i < values.len; i++) {
                ir_shape_extent_t candidate = {0};
                ir_shape_t *child_shape = ir_value_shape(values.data[i]);
                common_static = child_shape
                    && ir_shape_extent(child_shape, d - 1, &candidate)
                    && candidate.kind == IR_SHAPE_EXTENT_STATIC
                    && candidate.static_value == first_extent.static_value;
            }
            if (common_static) extent = first_extent;
        }
        ok = ir_shape_set_extent(shape, d, extent);
    }
    if (ok && shape) ok = ir_value_set_shape(instruction->dest, shape);
    core_list_free(values);
    if (!ok)
        return lower_internal(context, span,
                           "could not emit a sequence aggregate");
    instruction->span = span;
    return is_list
        ? n00bb_lower_literal_constructor(context, result_type,
                                          instruction->dest, span)
        : (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
}

ir_lower_result_t
n00bb_lower_sequence_literal(ir_lower_context_t *context,
                             parse_visit_t *visit,
                             ir_value_t *const *operands,
                             size_t operand_count, th_type *result_type,
                             ir_span_t span, void *state)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_explicit_literal_site(visit->grammar, lower->unit, visit->node) : nullptr;
    return site
        ? n00bb_lower_user_literal(context, visit, site, result_type, span)
        : n00bb_lower_sequence_aggregate(context, visit, operands,
                                         operand_count, result_type, span,
                                         state);
}

static ir_lower_result_t
n00bb_lower_open_brace_literal(ir_lower_context_t *context,
                               parse_visit_t *visit, th_type *result_type,
                               ir_span_t span)
{
    th_ctx *types = ir_module_types(context->module);
    bool set = child_nt(visit->grammar, visit->node,
                        CORE_STRING_STATIC("set-entries")) != nullptr;
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = collect_sequence_literal_values(context, visit, visit->node,
                                              &values)
        && (set || !(values.len & 1u));
    core_list_t(n00bb_ir_value_ptr_t) items =
        core_list_new(n00bb_ir_value_ptr_t);
    for (size_t i = 0; ok && i < values.len; i++) {
        core_list_push(items, values.data[i]);
        if (!set) continue;
        ir_instr_t *nil = ir_lower_emit_instruction(context, IR_OP_NIL, span);
        ok = nil && ir_lower_set_temp_dest(context, nil,
            CORE_STRING_STATIC("__n00bb_"), th_nil(types));
        if (ok) core_list_push(items, nil->dest);
    }
    ir_instr_t *instruction = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span)
        : nullptr;
    ok = instruction
        && ir_lower_set_temp_dest(context, instruction,
               CORE_STRING_STATIC("__n00bb_literal_"), result_type)
        && ir_instr_set_aggregate_form(instruction, IR_AGGREGATE_PAIRS);
    for (size_t i = 0; ok && i < items.len; i++)
        ok = ir_instr_operand_value(instruction, items.data[i]);
    core_list_free(values);
    core_list_free(items);
    if (ok) n00bb_rewrite_later(context, instruction);
    return ok ? (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest}
        : lower_internal(context, span, "could not lower an open brace literal");
}

ir_lower_result_t
n00bb_lower_brace_literal(ir_lower_context_t *context,
                          parse_visit_t *visit,
                          ir_value_t *const *operands,
                          size_t operand_count, th_type *result_type,
                          ir_span_t span, void *state)
{
    (void)operands;
    (void)operand_count;
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_litmod_site_t *site = tu
        ? n00bb_explicit_literal_site(visit->grammar, lower->unit, visit->node) : nullptr;
    if (site)
        return n00bb_lower_user_literal(context, visit, site,
                                        result_type, span);
    return n00bb_lower_open_brace_literal(context, visit, result_type, span);
}

static ir_lower_result_t
n00bb_lower_brace_payload(ir_lower_context_t *context,
                          parse_visit_t *visit, bool dictionary,
                          th_type *payload_type, ir_span_t span)
{
    parse_tree_t *braces = visit->node;
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = braces && collect_sequence_literal_values(context, visit,
                                                          braces, &values)
        && (!dictionary || !(values.len & 1u));
    th_type *element_type = payload_type
        ? th_seq_elem(th_resolve(payload_type)) : nullptr;
    core_list_t(n00bb_ir_value_ptr_t) items =
        core_list_new(n00bb_ir_value_ptr_t);
    ok = ok && element_type;
    for (size_t i = 0; ok && i < values.len; i += dictionary ? 2 : 1) {
        ir_value_t *item = values.data[i];
        if (dictionary) {
            ir_instr_t *pair = ir_lower_emit_instruction(context,
                IR_OP_AGGREGATE, span);
            ok = pair && ir_lower_set_temp_dest(context, pair,
                                                 CORE_STRING_STATIC("__n00bb_pair_"), element_type)
                && ir_instr_operand_value(pair, item)
                && ir_instr_operand_value(pair, values.data[i + 1]);
            if (!ok) break;
            item = pair->dest;
        }
        core_list_push(items, item);
    }
    core_list_free(values);
    ir_instr_t *array = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = array && ir_lower_set_temp_dest(context, array,
        CORE_STRING_STATIC("__n00bb_brace_payload_"), payload_type)
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < items.len; i++)
        ok = ir_instr_operand_value(array, items.data[i]);
    core_list_free(items);
    if (!ok)
        return lower_internal(context, span,
                           "could not construct the brace handler payload");
    return (ir_lower_result_t){IR_LOWER_VALUE, array->dest};
}

static ir_lower_result_t
n00bb_lower_user_brace_payload(ir_lower_context_t *context,
                               parse_visit_t *visit,
                               n00bb_litmod_site_t *site, ir_span_t span)
{
    bool dictionary = core_string_eq(site->registration->kind,
                                      CORE_STRING_STATIC("dict"));
    return n00bb_lower_brace_payload(context, visit, dictionary,
                                     site->payload_type, span);
}

static ir_lower_result_t
n00bb_lower_user_parenthesized_payload(ir_lower_context_t *context,
                                       parse_visit_t *visit,
                                       n00bb_litmod_site_t *site,
                                       ir_span_t span)
{
    bool record = core_string_eq(site->registration->kind,
                                 CORE_STRING_STATIC("record"));
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    parse_tree_t *fields = record
        ? child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("named-field-list"))
        : visit->node;
    core_list_t(parse_tree_ptr_t) expressions =
        core_list_new(parse_tree_ptr_t);
    core_list_t(n00bb_ir_value_ptr_t) values =
        core_list_new(n00bb_ir_value_ptr_t);
    bool ok = collect_ordered_children(fields, &expressions);
    th_type *element_type = site->payload_type
        ? th_seq_elem(th_resolve(site->payload_type)) : nullptr;
    th_type *string_type = record && tu
        ? th_name(tu->types, CORE_STRING_STATIC("string")) : nullptr;
    ok = ok && element_type && (!record || string_type);
    for (size_t i = 0; ok && i < expressions.len; i++) {
        parse_tree_t *item = expressions.data[i];
        parse_tree_t *expression = record
            ? child_nt(visit->grammar, item, CORE_STRING_STATIC("expression")) : item;
        ir_value_t *value = expression
            ? ir_lower_node_value(context, expression) : nullptr;
        if (!value) { ok = false; break; }
        if (record) {
            core_string_t field = named_field_identifier(visit->grammar,
                                                         item);
            ir_instr_t *name = field.data
                ? ir_lower_emit_instruction(context, IR_OP_CONST, span)
                : nullptr;
            ok = name && ir_lower_set_temp_dest(context, name,
                                                 CORE_STRING_STATIC("__n00bb_field_"), string_type)
                && ir_instr_set_const_bytes(name,
                    (const uint8_t *)field.data, field.u8_bytes);
            if (!ok) break;
            ir_instr_t *pair = ir_lower_emit_instruction(context,
                IR_OP_AGGREGATE, span);
            ok = pair && ir_lower_set_temp_dest(context, pair,
                CORE_STRING_STATIC("__n00bb_field_pair_"), element_type)
                && ir_instr_operand_value(pair, name->dest)
                && ir_instr_operand_value(pair, value);
            if (!ok) break;
            value = pair->dest;
        }
        core_list_push(values, value);
    }
    ir_instr_t *array = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = array && ir_lower_set_temp_dest(context, array,
        CORE_STRING_STATIC("__n00bb_parenthesized_"), site->payload_type)
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < values.len; i++)
        ok = ir_instr_operand_value(array, values.data[i]);
    core_list_free(values);
    core_list_free(expressions);
    if (!ok) return lower_internal(context, span,
        "could not construct the tuple or record handler payload");
    return (ir_lower_result_t){IR_LOWER_VALUE, array->dest};
}

static ir_value_t *
n00bb_lower_literal_site_member(ir_lower_context_t *context,
                                const th_member *member, core_string_t path,
                                uint64_t line, uint64_t column, ir_span_t span)
{
    bool text = core_string_eq(member->name, CORE_STRING_STATIC("path"));
    bool at_line = core_string_eq(member->name, CORE_STRING_STATIC("line"));
    if (!text && !at_line
        && !core_string_eq(member->name, CORE_STRING_STATIC("column")))
        return nullptr;
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    bool ok = constant && ir_lower_set_temp_dest(context, constant,
        CORE_STRING_STATIC("__n00bb_literal_site_"), member->type)
        && (text
            ? ir_instr_set_const_bytes(constant, (const uint8_t *)path.data,
                                       path.u8_bytes)
            : ir_instr_operand_int(constant,
                                   (int64_t)(at_line ? line : column)));
    return ok ? constant->dest : nullptr;
}

static ir_value_t *
n00bb_lower_literal_site(ir_lower_context_t *context, parse_visit_t *visit,
                         ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    th_ctx *types = ir_module_types(context->module);
    th_type *site_type = n00bb_literal_site_type(types);
    th_type *definition = site_type
        ? th_resolve(th_type_definition(types, site_type)) : nullptr;
    parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
    if (!lower || !token || !definition
        || th_kind_of(definition) != TH_RECORD) return nullptr;
    core_string_t path = core_os_real_path(
        core_source_file_path(&lower->unit->source));
    size_t count = th_record_len(definition);
    ir_value_t **members = core_alloc_array(ir_value_t *, count ? count : 1);
    bool ok = path.data && members;
    for (size_t i = 0; ok && i < count; i++) {
        const th_member *member = th_record_member(definition, i);
        members[i] = member ? n00bb_lower_literal_site_member(context, member,
            path, parse_token_line(token), parse_token_column(token), span)
            : nullptr;
        ok = members[i] != nullptr;
    }
    ir_instr_t *aggregate = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, span) : nullptr;
    ok = aggregate && ir_lower_set_temp_dest(context, aggregate,
        CORE_STRING_STATIC("__n00bb_literal_site_"), site_type);
    for (size_t i = 0; ok && i < count; i++)
        ok = ir_instr_operand_value(aggregate, members[i]);
    core_free(members);
    core_free(path.data);
    return ok ? aggregate->dest : nullptr;
}

static ir_lower_result_t
user_literal_call(ir_lower_context_t *context, parse_visit_t *visit,
                  n00bb_litmod_site_t *site, th_type *result_type,
                  ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    parse_sym_entry_t *symbol = site && site->registration
        ? site->registration->function : nullptr;
    n00bb_link_analysis_t *analysis = state ? state->link_analysis : nullptr;
    ir_func_t *function = symbol && analysis
        ? link_ir_function_for_symbol(analysis, symbol) : nullptr;
    ir_value_t *callee = function
        ? ir_func_address(function->owner, function) : nullptr;
    bool no_payload = site && core_string_eq(site->registration->kind,
                                             CORE_STRING_STATIC("nil"));
    if (!symbol)
        return lower_internal(context, span,
                              "literal handler has no function symbol");
    if (!function)
        return lower_internal(context, span,
            "literal handler function is absent from this phase");
    if (!callee)
        return lower_internal(context, span,
                              "literal handler has no callable address");
    (void)result_type;
    if (!site || (!no_payload && !site->payload_type) || !site->signature)
        return lower_internal(context, span,
                              "literal handler call has no signature");
    if (!parse_sym_add_use(symbol, visit->node))
        return lower_internal(context, span,
                           "could not record the literal handler use");
    ir_lower_result_t payload = {IR_LOWER_FAILED, nullptr};
    if (core_string_eq(site->registration->kind, CORE_STRING_STATIC("sequence")))
        payload = n00bb_lower_sequence_aggregate(context, visit, nullptr, 0,
            site->payload_type, span, nullptr);
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("dict"))
        || core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("set")))
        payload = n00bb_lower_user_brace_payload(context, visit, site, span);
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("tuple"))
        || core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("record")))
        payload = n00bb_lower_user_parenthesized_payload(context, visit,
                                                           site, span);
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("bool"))) {
        parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
        core_option_t(core_string_t) literal = token
            ? parse_token_value(token) : core_option_none(core_string_t);
        core_string_t spelling = core_option_is_set(literal)
            ? core_option_get(literal) : (core_string_t){0};
        bool value = core_string_eq(spelling, CORE_STRING_STATIC("true"));
        if (value || core_string_eq(spelling, CORE_STRING_STATIC("false"))) {
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                IR_OP_CONST, span);
            if (constant && ir_lower_set_temp_dest(context, constant,
                CORE_STRING_STATIC("__n00bb_litmod_bool_"), site->payload_type)
                && ir_instr_operand_int(constant, value ? 1 : 0)) {
                payload = (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
            }
        }
    }
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("quotes"))
        || core_string_eq(site->registration->kind,
                          CORE_STRING_STATIC("int"))
        || core_string_eq(site->registration->kind,
                          CORE_STRING_STATIC("hex"))
        || core_string_eq(site->registration->kind,
                          CORE_STRING_STATIC("float"))) {
        parse_token_info_t *token =
            parse_tree_find_first_terminal(visit->node);
        core_buffer_t *decoded = core_buffer_empty();
        bool quotes = core_string_eq(site->registration->kind,
                                     CORE_STRING_STATIC("quotes"));
        core_option_t(core_string_t) literal = token
            ? parse_token_value(token) : core_option_none(core_string_t);
        core_string_t spelling = core_option_is_set(literal)
            ? core_option_get(literal) : (core_string_t){0};
        bool valid = token && decoded && spelling.data;
        if (valid && quotes) {
            valid = spelling.u8_bytes >= 2 && spelling.data[0] == '"'
                && spelling.data[spelling.u8_bytes - 1] == '"';
            if (valid)
                core_buffer_append(decoded, spelling.data + 1,
                                   spelling.u8_bytes - 2);
        }
        else if (valid) {
            bool hex = core_string_eq(site->registration->kind,
                                      CORE_STRING_STATIC("hex"));
            valid = !hex || (spelling.u8_bytes >= 2
                && spelling.data[0] == '0' && spelling.data[1] == 'x');
            if (valid)
                core_buffer_append(decoded,
                    spelling.data + (hex ? 2 : 0),
                    spelling.u8_bytes - (hex ? 2 : 0));
        }
        ir_instr_t *constant = valid
            ? ir_lower_emit_instruction(context, IR_OP_CONST, span) : nullptr;
        if (constant && ir_lower_set_temp_dest(context, constant,
            CORE_STRING_STATIC("__n00bb_litmod_raw_"), site->payload_type)
            && ir_instr_set_const_bytes(constant,
                (const uint8_t *)decoded->data, decoded->byte_len)) {
            payload = (ir_lower_result_t){IR_LOWER_VALUE, constant->dest};
        }
        core_buffer_free(decoded);
    }
    else if (core_string_eq(site->registration->kind,
                            CORE_STRING_STATIC("fence"))) {
        parse_token_info_t *token =
            parse_tree_find_first_terminal(visit->node);
        size_t content_start = 0;
        size_t content_end = 0;
        core_option_t(core_string_t) literal = token
            ? parse_token_value(token) : core_option_none(core_string_t);
        if (token && parse_token_content_span(token, &content_start,
                                              &content_end)
            && core_option_is_set(literal)) {
            core_string_t spelling = core_option_get(literal);
            size_t token_start = parse_token_start_byte(token);
            if (content_start < token_start || content_end < content_start
                || content_end - token_start > spelling.u8_bytes)
                return lower_internal(context, span,
                    "fence literal has invalid content offsets");
            size_t start = content_start - token_start;
            size_t end = content_end - token_start;
            ir_instr_t *constant = ir_lower_emit_instruction(context,
                IR_OP_CONST, span);
            if (constant && ir_lower_set_temp_dest(context, constant,
                CORE_STRING_STATIC("__n00bb_fence_"), site->payload_type)
                && ir_instr_set_const_bytes(constant,
                    (const uint8_t *)spelling.data + start, end - start)) {
                payload = (ir_lower_result_t){IR_LOWER_VALUE,
                                               constant->dest};
            }
        }
    }
    if (!no_payload
        && (payload.status != IR_LOWER_VALUE || !payload.value))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    ir_value_t *literal_site = n00bb_litmod_takes_site(
            ir_module_types(context->module), site->signature)
        ? n00bb_lower_literal_site(context, visit, span) : nullptr;
    if (n00bb_litmod_takes_site(ir_module_types(context->module),
                                site->signature) && !literal_site)
        return lower_internal(context, span,
                           "could not describe the literal's source site");
    th_type *outcome_type = ir_call_outcome_type(context->module,
                                                site->signature);
    ir_instr_t *call = outcome_type
        ? ir_lower_emit_instruction(context, IR_OP_CALL, span) : nullptr;
    if (!call || !ir_lower_set_temp_dest(context, call,
                                        CORE_STRING_STATIC("__n00bb_litmod_call_"), outcome_type)
        || !ir_instr_operand_value(call, callee)
        || (!no_payload && !ir_instr_operand_value(call, payload.value))
        || (literal_site && !ir_instr_operand_value(call, literal_site))
        || !ir_instr_set_call_signature(call, site->signature))
        return lower_internal(context, span,
                           "could not emit the literal handler call");
    ir_value_t *value = nullptr;
    if (!n00bb_unwrap_call_outcome(context, visit, call->dest, span,
                                   &value, true))
        return (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
    if (ir_machine_of(context->module, th_func_ret(site->signature)).class
            == IR_CLASS_VOID)
        return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    return (ir_lower_result_t){IR_LOWER_VALUE, value};
}

static n00bb_hoisted_literal_t *
hoisted_literal(n00bb_tu_state_t *state, parse_tree_t *node)
{
    for (size_t i = 0; i < state->hoisted_literals.len; i++)
        if (state->hoisted_literals.data[i].node == node)
            return &state->hoisted_literals.data[i];
    return nullptr;
}

static bool
producing_constants(ir_lower_context_t *context)
{
    return n00bb_const_initializer(context->function);
}

static size_t
modified_tokens(parse_tree_t *node)
{
    if (!node) return 0;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        return token
            && core_option_is_set(parse_token_postfix_modifier(token));
    }
    size_t count = 0;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        count += modified_tokens(core_tree_child(node, i));
    return count;
}

static bool
hoistable_literal(ir_lower_context_t *context, parse_visit_t *visit,
                  n00bb_litmod_site_t *site)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    if (state->interface_module || !context->function || !site
        || !site->registration || !site->signature
        || producing_constants(context)
        || th_kind_of(th_resolve(th_func_ret(site->signature))) == TH_VAR)
        return false;
    th_type **variables = nullptr;
    size_t    count     = th_free_vars(state->types, context->function->type,
                                       &variables);
    for (size_t i = 0; i < count; i++)
        if (th_var_name(variables[i]).data) return false;
    static const core_string_t fixed[] = {
        CORE_STRING_STATIC("quotes"), CORE_STRING_STATIC("int"),
        CORE_STRING_STATIC("hex"), CORE_STRING_STATIC("float"),
        CORE_STRING_STATIC("fence"), CORE_STRING_STATIC("bool"),
        CORE_STRING_STATIC("nil"),
    };
    core_string_t kind = site->registration->kind;
    for (size_t i = 0; i < sizeof(fixed) / sizeof(*fixed); i++)
        if (core_string_eq(kind, fixed[i])) return true;
    n00bb_static_value_t value = {0};
    return core_string_eq(kind, CORE_STRING_STATIC("sequence"))
        && modified_tokens(visit->node) == 1
        && n00bb_trivial_static_value(visit->grammar, state, visit->node,
                                      &value);
}

static n00bb_hoisted_literal_t *
hoist_literal(ir_lower_context_t *context, parse_visit_t *visit,
              th_type *type)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = tu_state(lower->unit);
    if (ir_machine_of(context->module, type).class == IR_CLASS_VOID) {
        core_list_push(state->hoisted_literals, ((n00bb_hoisted_literal_t){
            .node = visit->node}));
        return &state->hoisted_literals.data[state->hoisted_literals.len - 1];
    }
    parse_symtab_t *symtab = state->annotations
        ? state->annotations->symtab : nullptr;
    parse_scope_t *scope = module_scope(state);
    parse_token_info_t *token = parse_tree_find_first_terminal(visit->node);
    if (!symtab || !scope || !token) return nullptr;
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_litmod_%llu",
        (unsigned long long)parse_token_start_byte(token));
    ir_global_t *global = ir_global_add(state->ir_module,
                                        core_buffer_string(&name), type);
    parse_scope_t *previous = parse_symtab_current_scope(symtab,
        CORE_STRING_STATIC(""));
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC(""), scope);
    parse_sym_entry_t *symbol = global ? parse_symtab_add(symtab,
        CORE_STRING_STATIC(""), global->name, PARSE_SYM_VARIABLE, nullptr)
        : nullptr;
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC(""), previous);
    core_buffer_release(&name);
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.generated-literal"), global))
        return nullptr;
    symbol->type = type;
    symbol->mutability = PARSE_SYM_CONST;
    symbol->is_synthetic = true;
    parse_sym_set_storage_duration(symbol, PARSE_SYM_STORAGE_MODULE);
    global->mutability = IR_MUT_CONST;
    global->is_static = true;
    if (!ir_global_set_storage_duration(lower->ir_context, global,
                                        IR_STORAGE_DURATION_MODULE))
        return nullptr;
    core_list_push(state->hoisted_literals, ((n00bb_hoisted_literal_t){
        .node = visit->node, .global = global}));
    return &state->hoisted_literals.data[state->hoisted_literals.len - 1];
}

ir_lower_result_t
n00bb_lower_user_literal(ir_lower_context_t *context, parse_visit_t *visit,
                         n00bb_litmod_site_t *site, th_type *result_type,
                         ir_span_t span)
{
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *state = lower ? tu_state(lower->unit) : nullptr;
    n00bb_hoisted_literal_t *hoisted = state
        ? hoisted_literal(state, visit->node) : nullptr;
    if (hoisted && producing_constants(context)) {
        ir_lower_result_t produced = user_literal_call(context, visit, site,
                                                       result_type, span);
        if (produced.status != IR_LOWER_VALUE || !hoisted->global)
            return produced;
        ir_instr_t *store = ir_lower_emit_instruction(context,
            IR_OP_GLOBAL_STORE, span);
        if (!store || !ir_instr_operand_value(store,
                ir_global_address(context->module, hoisted->global))
            || !ir_instr_operand_value(store, produced.value)
            || !ir_instr_set_binding_initialization(store, true))
            return lower_internal(context, span,
                                  "could not store the literal's comptime value");
        return produced;
    }
    th_type *type = site && site->signature ? th_func_ret(site->signature)
                                            : nullptr;
    if (!hoisted && state && type && hoistable_literal(context, visit, site))
        hoisted = hoist_literal(context, visit, type);
    if (!hoisted)
        return user_literal_call(context, visit, site, result_type, span);
    if (!hoisted->global) return (ir_lower_result_t){IR_LOWER_NO_VALUE, nullptr};
    ir_instr_t *copy = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    if (!copy || !ir_lower_set_temp_dest(context, copy,
            CORE_STRING_STATIC("__n00bb_literal_"), type)
        || !ir_instr_operand_value(copy,
               ir_global_address(context->module, hoisted->global))
        || !ir_instr_set_fresh(copy))
        return lower_internal(context, span,
                              "could not copy the literal's comptime value");
    return (ir_lower_result_t){IR_LOWER_VALUE, copy->dest};
}

bool
n00bb_lower_scalar_litmod(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *state)
{
    (void)state;
    n00bb_lower_context_t *lower = context->user;
    n00bb_emit_t emit;
    bool scalar = nt_is(visit->grammar, visit->node,
                        CORE_STRING_STATIC("simple-lit"))
        && n00bb_node_emit(visit->grammar, visit->node, &emit)
        && core_string_eq(emit.action, CORE_STRING_STATIC("literal"));
    bool fence = nt_is(visit->grammar, visit->node,
                       CORE_STRING_STATIC("embed-lit"));
    n00bb_litmod_site_t *site = lower && !entering && visit->node
            && (scalar || fence)
        ? n00bb_explicit_literal_site(visit->grammar, lower->unit, visit->node)
        : nullptr;
    if (!site || !context->reachable
        || ir_lower_node_value(context, visit->node)) return true;
    ir_lower_result_t result = n00bb_lower_user_literal(context, visit, site,
        n00bb_node_type(context, visit->node),
        lower_span_for_tree(context, visit->node));
    if (result.status == IR_LOWER_NO_VALUE)
        return ir_lower_suppress_emit(context, visit->node);
    return result.status == IR_LOWER_VALUE && result.value
        && ir_lower_set_node_value(context, visit->node, result.value);
}

ir_instr_t *
n00bb_emit_uint_constant(ir_lower_context_t *context, th_type *uint_type,
                         uint64_t value, ir_span_t span, core_string_t prefix)
{
    char name_buffer[64];
    core_string_t name = n00bb_format_name(name_buffer, sizeof(name_buffer),
        "%.*s_%llu", (int)prefix.u8_bytes, prefix.data,
        (unsigned long long)context->next_value_id++);
    ir_instr_t *constant = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                     span);
    if (!constant || !ir_instr_set_dest(constant, name, uint_type)
        || !ir_instr_operand_uint(constant, value)) return nullptr;
    return constant;
}
