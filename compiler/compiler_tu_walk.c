#include "compiler_tu.h"
#include "compiler_link.h"
#include "flagset_value.h"

typedef struct {
    parse_grammar_t *grammar;
    th_ctx *types;
} annotation_context_t;

static const parse_annot_kind_t semantic_kinds[] = {
    PARSE_ANNOT_SCOPE_OPEN, PARSE_ANNOT_ADT, PARSE_ANNOT_NAMESPACE,
    PARSE_ANNOT_DECLARES, PARSE_ANNOT_TYPE_DECL, PARSE_ANNOT_FIELD,
    PARSE_ANNOT_METHOD, PARSE_ANNOT_TYPE, PARSE_ANNOT_EXPOSES,
    PARSE_ANNOT_INHERITS, PARSE_ANNOT_VISIBILITY, PARSE_ANNOT_STATIC,
    PARSE_ANNOT_ABSTRACT, PARSE_ANNOT_LITERAL, PARSE_ANNOT_RECORD,
    PARSE_ANNOT_MUTABILITY, PARSE_ANNOT_DIRECTION, PARSE_ANNOT_PASS_BY,
    PARSE_ANNOT_CONSTRAIN, PARSE_ANNOT_INFER, PARSE_ANNOT_SIZE,
    PARSE_ANNOT_ALIGNMENT, PARSE_ANNOT_OFFSET, PARSE_ANNOT_WIDTH,
};

static const parse_annot_kind_t semantic_reference_kinds[] = {
    PARSE_ANNOT_BRANCH, PARSE_ANNOT_LOOP, PARSE_ANNOT_SWITCH,
    PARSE_ANNOT_JUMP, PARSE_ANNOT_CAPTURE, PARSE_ANNOT_ASSIGNS,
    PARSE_ANNOT_VARREF, PARSE_ANNOT_OPERATOR, PARSE_ANNOT_NOTRIVIA,
    PARSE_ANNOT_CALL,
};

typedef struct {
    bool type_reference;
    bool modified_numeric;
    parse_token_info_t *literal_token;
    core_string_t literal_terminal;
} semantic_annotation_cache_t;

typedef enum {
    SEMANTIC_PREPARE_ENTER,
    SEMANTIC_AFTER_ENTER,
    SEMANTIC_PREPARE_EXIT,
    SEMANTIC_AFTER_REFERENCE_EXIT,
    SEMANTIC_AFTER_EXIT,
} semantic_step_t;

static bool
untyped_module_name(n00bb_tu_state_t *state, parse_tree_t *import)
{
    parse_node_resolution_t resolution = parse_tree_resolution(import);
    parse_sym_entry_t *module = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : core_tree_node_value(import).sym;
    if (!module || module->kind != PARSE_SYM_MODULE
        || (module->type && th_is_scheme(module->type)))
        return true;
    th_type *any = th_var(state->types);
    module->type = any ? th_generalize(state->types, any) : nullptr;
    return module->type != nullptr;
}

static th_type *
range_binding_type(parse_grammar_t *grammar, parse_tree_t *parts)
{
    parse_tree_t *loop = core_tree_node_value(parts).parent;
    parse_tree_t *statement = nt_is(grammar, loop, "range-loop")
        ? core_tree_node_value(loop).parent : nullptr;
    parse_tree_t *binding = child_nt(grammar,
        child_nt(grammar, statement, "for-range-var"), "for-binding");
    parse_tree_t *spec = child_nt(grammar, binding, "type-spec");
    return spec ? core_tree_node_value(spec).type : nullptr;
}

static void
report_literal_failure(core_translation_unit_t *unit, parse_tree_t *node,
                       size_t before, const char *code, const char *message)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (core_diagnostics_count(diagnostics) == before)
        core_diagnostics_report_at(diagnostics, unit, CORE_DIAG_ERROR,
            core_string_from_cstr(code), core_string_from_cstr(message),
            link_source_location(node));
}

static bool
infer_literal_reporting(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node,
                        const char *code, const char *message)
{
    size_t before = core_diagnostics_count(
        core_translation_unit_diagnostics(unit));
    if (infer_user_literal_type(grammar, unit, node)) return true;
    report_literal_failure(unit, node, before, code, message);
    return false;
}

static void
semantic_annotation_node(void *context, core_translation_unit_t *unit,
                         parse_tree_t *node, parse_tree_t *parent,
                         semantic_step_t step,
                         parse_visit_t *visit,
                         semantic_annotation_cache_t *cache)
{
    bool entering = step <= SEMANTIC_AFTER_ENTER;
    bool before_annotations = step == SEMANTIC_PREPARE_ENTER
        || step == SEMANTIC_PREPARE_EXIT;
    annotation_context_t *annotation_context = context;
    parse_grammar_t *grammar = annotation_context->grammar;
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !node) return;
    if (before_annotations) {
    if (entering && nt_is(grammar, node, "assign-stmt")
        && core_tree_num_children(node) == 3
        && n00bb_index_expression_node(grammar,
            core_tree_child(node, 0))) {
        parse_tree_t *value = core_tree_child(node, 2);
        if (!core_tree_is_leaf(value)
            && !core_tree_node_value(value).argument_type)
            core_tree_node_value(value).argument_type = th_var(state->types);
    }
    if (entering && nt_is(grammar, node, "postfix-expr")
        && core_tree_num_children(node) >= 3
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC("("))
        && member_access_node(grammar, core_tree_child(node, 0))) {
        parse_tree_t *args = core_tree_child(node, 2);
        parse_tree_t *positional = child_nt(grammar, args,
                                            "positional-args");
        parse_tree_t *keywords = child_nt(grammar, args, "keyword-args");
        for (size_t i = 0; positional
                && i < core_tree_num_children(positional); i++) {
            parse_tree_t *arg = core_tree_child(positional, i);
            if (nt_is(grammar, arg, "expression")
                && !core_tree_node_value(arg).argument_type) {
                core_tree_node_value(arg).argument_type = th_var(state->types);
            }
        }
        for (size_t i = 0; keywords
                && i < core_tree_num_children(keywords); i++) {
            parse_tree_t *keyword = core_tree_child(keywords, i);
            parse_tree_t *value = child_nt(grammar, keyword, "expression");
            if (value && !core_tree_node_value(value).argument_type)
                core_tree_node_value(value).argument_type = th_var(state->types);
        }
    }
    if (entering && !validate_package_body_statement(
            grammar, unit, node, parent))
        return;
    if (entering) n00bb_range_enter_node(grammar, state, node, parent);
    bool invalid_field_access = !entering
        && check_local_record_field_reference(grammar, unit, node);
    bool variant_payload = !entering
        && prepare_variant_payload_type(grammar, unit, node);
    parse_node_resolution_t member_resolution = parse_tree_resolution(node);
    bool flagset_field = !entering
        && member_resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && n00bb_flagset_instance_access(grammar, unit, node,
                                          member_resolution.target.symbol);
    bool type_reference = is_type_reference_node_at(grammar, node, parent);
    core_string_t operator_trait = {0}, operator_method = {0};
    n00bb_link_use_site_t operator_site = {
        .node = node,
        .operator_dispatch = true,
    };
    bool intrinsic_binary = !entering
        && binary_operator_dispatch_for_node(grammar, node,
            &operator_trait, &operator_method);
    bool intrinsic_unary = !entering
        && unary_operator_dispatch_for_node(grammar, node,
            &operator_trait, &operator_method);
    parse_token_info_t *literal_token = nt_is(grammar, node, "simple-lit")
        ? parse_tree_find_first_terminal(node) : nullptr;
    core_string_t literal_terminal = literal_token
        ? parse_terminal_name(grammar, literal_token->tid)
        : (core_string_t){0};
    bool modified_numeric = literal_token
        && core_option_is_set(literal_token->postfix_modifier)
        && (core_string_eq(literal_terminal, CORE_STRING_STATIC("INT_LIT"))
            || core_string_eq(literal_terminal, CORE_STRING_STATIC("HEX_LIT"))
            || core_string_eq(literal_terminal,
                              CORE_STRING_STATIC("FLOAT_LIT"))
            || core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("true"))
            || core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("false"))
            || core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("nil")));
    operator_site.dispatch_trait = operator_trait;
    operator_site.dispatch_method = operator_method;
    parse_tree_t *numeric_receiver = intrinsic_binary
        ? operator_receiver_node(&operator_site) : nullptr;
    bool numeric_comparison = numeric_receiver
        && !core_tree_is_leaf(numeric_receiver)
        && (core_string_eq(operator_trait, CORE_STRING_STATIC("Equal"))
            || core_string_eq(operator_trait,
                              CORE_STRING_STATIC("Comparable")))
        && is_integer_type(core_tree_node_value(numeric_receiver).type);
    intrinsic_binary = intrinsic_binary
        && ((primitive_operator_site(state, &operator_site)
                && (numeric_comparison
                    || (!core_string_eq(operator_trait,
                                         CORE_STRING_STATIC("Equal"))
                        && !core_string_eq(operator_trait,
                                            CORE_STRING_STATIC("Comparable")))))
            || n00bb_flagset_operator_site(grammar, unit, state, node));
    intrinsic_unary = intrinsic_unary
        && (primitive_operator_site(state, &operator_site)
            || n00bb_flagset_operator_site(grammar, unit, state, node));
    if (cache) *cache = (semantic_annotation_cache_t){
        .type_reference = type_reference,
        .modified_numeric = modified_numeric,
        .literal_token = literal_token,
        .literal_terminal = literal_terminal,
    };
    if (variant_payload || flagset_field || type_reference
        || invalid_field_access || intrinsic_binary || intrinsic_unary
        || (modified_numeric
            && core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("nil"))))
        (void)parse_visit_suppress_annotation(visit, PARSE_ANNOT_INFER);
    if (modified_numeric)
        (void)parse_visit_suppress_annotation(visit, PARSE_ANNOT_LITERAL);
    if (type_reference)
        (void)parse_visit_suppress_annotation(visit, PARSE_ANNOT_VARREF);
    if (step == SEMANTIC_PREPARE_EXIT && nt_is(grammar, node, "tspec-tvar")
        && !signature_typevar_for_node(grammar, unit, node))
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
            CORE_STRING_STATIC(
                "could not bind this type variable to its callable signature"),
            link_source_location(node));
    return;
    }
    bool type_reference = cache && cache->type_reference;
    bool modified_numeric = cache && cache->modified_numeric;
    parse_token_info_t *literal_token = cache ? cache->literal_token : nullptr;
    core_string_t literal_terminal = cache
        ? cache->literal_terminal : (core_string_t){0};
    if (entering) {
        if (nt_is(grammar, node, "postfix-expr"))
            core_tree_node_value(node).index_address_access =
                n00bb_index_reference_storage(grammar, node);
        if (field_record_declaration(grammar, node)) {
            parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
            if (!symbol || symbol->kind != PARSE_SYM_TYPEDEF) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0005"),
                    CORE_STRING_STATIC("record declaration has no type symbol"),
                    link_source_location(node));
                return;
            }
            th_type *named = symbol->type ? th_resolve(symbol->type)
                                          : nullptr;
            if (!named || th_kind_of(named) != TH_NAMED) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0005"),
                    CORE_STRING_STATIC("record type name conflicts with its symbol"),
                    link_source_location(node));
                return;
            }
            core_tree_node_value(node).type = named;
        }
        if (nt_is(grammar, node, "body")
            && !n00bb_bind_option_branch_shadow(grammar, unit, node, parent))
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0015"),
                CORE_STRING_STATIC(
                    "could not bind the option refinement in this branch"),
                link_source_location(node));
    }
    else {
        if (step == SEMANTIC_AFTER_EXIT)
            n00bb_defer_overloaded_callee(grammar, unit, node);
        if (step == SEMANTIC_AFTER_REFERENCE_EXIT) {
        if (nt_is(grammar, node, "tuple-or-paren")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *inner = core_tree_child(node, 1);
            parse_sym_entry_t *symbol = attached_symbol(grammar, unit, inner);
            if (symbol && !parse_tree_set_resolution_symbol(node, symbol)) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0013"),
                    CORE_STRING_STATIC(
                        "could not retain parenthesized symbol resolution"),
                    link_source_location(node));
                return;
            }
        }
        if (!n00bb_apply_option_branch_shadow(grammar, unit, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0015"),
                CORE_STRING_STATIC(
                    "could not bind a reference to its option refinement"),
                link_source_location(node));
            return;
        }
        if (!apply_typecase_narrowing(grammar, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-TYPECASE"),
                CORE_STRING_STATIC(
                    "could not bind a reference to its narrowed typecase subject"),
                link_source_location(node));
            return;
        }
        n00bb_apply_atomic_load(grammar, unit, node);
        if (!bind_builtin_reference(grammar, state, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not bind the builtin reference"),
                link_source_location(node));
            return;
        }
        if (member_access_node(grammar, node) == node
            && n00bb_assignment_target(grammar, node, nullptr)) {
            parse_sym_entry_t *atomic_root =
                n00bb_atomic_member_lvalue_root(grammar, unit, node);
            th_type *element = atomic_root
                ? n00bb_atomic_element_type(atomic_root->type) : nullptr;
            parse_tree_t *access = member_access_node(grammar, node);
            parse_tree_t *receiver = access
                ? core_tree_child(access, 0) : nullptr;
            if (atomic_root && element && receiver) {
                core_tree_node_value(receiver).type = element;
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0017"),
                    CORE_STRING_STATIC(
                        "atomic values cannot be updated through a field; assign the whole value"),
                link_source_location(node));
            }
        }
        bind_declared_type_from_node(grammar, node);
        if (!n00bb_cache_contextual_integer(grammar, state, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not constrain integer literal"),
                link_source_location(node));
            return;
        }
        if (nt_is(grammar, node, "simple-lit")) {
            th_type *expected = n00bb_integer_literal_context(grammar,
                state, node);
            th_type *element = nullptr;
            if (n00bb_option_type_element(expected, &element))
                expected = element;
            if (expected && is_integer_type(expected)
                && core_tree_node_value(node).integer_literal_origin == node)
                n00bb_contextual_integer_literal(state, node, expected);
            if (expected && is_float_type(expected))
                n00bb_contextual_float_literal(node, expected);
        }
        if (nt_is(grammar, node, "range-loop")) {
            parse_tree_t *parts = child_nt(grammar, node, "range-parts");
            parse_tree_t *step = child_nt(grammar,
                child_nt(grammar, node, "range-step"), "expression");
            th_type *bound = parts ? core_tree_node_value(parts).type
                                   : nullptr;
            if (step && bound && is_integer_type(th_resolve(bound)))
                (void)n00bb_contextual_integer_literal(state, step, bound);
        }
        th_type *declared = nt_is(grammar, node, "range-parts")
            ? range_binding_type(grammar, node) : nullptr;
        if (declared && is_integer_type(th_resolve(declared))
            && core_tree_num_children(node) == 3) {
            (void)n00bb_contextual_integer_literal(state,
                core_tree_child(node, 0), declared);
            (void)n00bb_contextual_integer_literal(state,
                core_tree_child(node, 2), declared);
        }
        if (nt_is(grammar, node, "range-parts")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *start = core_tree_child(node, 0);
            parse_tree_t *bound = core_tree_child(node, 2);
            th_type *start_type = core_tree_node_value(start).type;
            th_type *bound_type = core_tree_node_value(bound).type;
            if (bound_type && th_kind_of(th_resolve(bound_type)) == TH_VAR)
                (void)n00bb_literal_adopts_open_type(grammar, state, start,
                                                     bound_type);
            else if (start_type
                     && th_kind_of(th_resolve(start_type)) == TH_VAR)
                (void)n00bb_literal_adopts_open_type(grammar, state, bound,
                                                     start_type);
        }
        if (core_tree_num_children(node) == 3
            && (nt_is(grammar, node, "range-parts")
                || (nt_is(grammar, node, "compare-expr")
                    && nt_is(grammar, core_tree_child(node, 1),
                             "compare-op"))
                || (nt_is(grammar, node, "equality-expr")
                    && nt_is(grammar, core_tree_child(node, 1),
                             "equality-op")))) {
            parse_tree_t *left = core_tree_child(node, 0);
            parse_tree_t *right = core_tree_child(node, 2);
            th_type *left_type = core_tree_node_value(left).type;
            th_type *right_type = core_tree_node_value(right).type;
            if (left_type && right_type && left_type != right_type
                && !n00bb_contextual_integer_literal(state, right,
                                                     left_type)
                && !n00bb_float_literal_adopts(right, left_type)
                && !n00bb_contextual_integer_literal(state, left, right_type))
                n00bb_float_literal_adopts(left, right_type);
        }
        if (nt_is(grammar, node, "sym-decl")) {
            parse_tree_t *type_spec = child_nt(grammar, node, "type-spec");
            contextualize_record_literal(grammar, unit, state,
                child_nt(grammar, node, "expression"),
                type_spec ? core_tree_node_value(type_spec).type : nullptr);
        }
        if (nt_is(grammar, node, "assign-stmt")
            && core_tree_num_children(node) == 3) {
            contextualize_record_literal(grammar, unit, state,
                core_tree_child(node, 2),
                record_literal_expected_type(grammar, state->types,
                    core_tree_child(node, 2)));
        }
        if (nt_is(grammar, node, "return-stmt")) {
            parse_tree_t *function = node;
            while (function && !nt_is(grammar, function, "func-def"))
                function = core_tree_node_value(function).parent;
            parse_sym_entry_t *symbol = function
                ? core_tree_node_value(function).sym : nullptr;
            th_type *declared = symbol ? parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.declared-function-signature"))
                : nullptr;
            if (declared)
                contextualize_record_literal(grammar, unit, state,
                    child_nt(grammar, node, "expression"),
                    th_func_ret(declared));
        }
        return;
        }
        if (core_tree_num_children(node) == 1) {
            parse_tree_t *child = core_tree_child(node, 0);
            parse_tree_t *origin = n00bb_node_record_literal_origin(child);
            if (origin) {
                n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state,
                                                                     node);
                if (!facts) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                        CORE_STRING_STATIC("could not retain record literal origin"),
                        link_source_location(node));
                    return;
                }
                facts->record_literal_origin = origin;
            }
        }
        if (nt_is(grammar, node, "assign-stmt")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *target = core_tree_child(node, 0);
            parse_tree_t *value = core_tree_child(node, 2);
            n00bb_contextual_integer_literal(state, value,
                core_tree_node_value(target).type);
            th_type *target_type = core_tree_node_value(target).type
                ? th_resolve(core_tree_node_value(target).type) : nullptr;
            th_type *value_type = core_tree_node_value(value).type
                ? th_resolve(core_tree_node_value(value).type) : nullptr;
            if (target_type && value_type
                && th_kind_of(target_type) == TH_VAR
                && th_var_name(target_type)
                && th_kind_of(value_type) != TH_VAR
                && th_is_closed(state->types, value_type)) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "cannot assign %s to a value of type parameter `%s; a declared type parameter stays generic",
                    n00bb_print_type(state->types, value_type),
                    th_var_name(target_type));
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0036"),
                    core_string_borrow(message.data, message.byte_len),
                    link_source_location(node));
                core_buffer_release(&message);
            }
        }
        cache_wrapped_symbol_resolution(grammar, unit, node);
        if (nt_is(grammar, node, "unary-expr")
            && core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("ref"))) {
            parse_tree_t *operand = nullptr;
            for (size_t i = 0; i < core_tree_num_children(node); i++) {
                parse_tree_t *child = core_tree_child(node, i);
                if (core_tree_is_leaf(child)) continue;
                if (operand) { operand = nullptr; break; }
                operand = child;
            }
            parse_node_resolution_t resolution =
                parse_tree_resolution(operand);
            if (operand && parse_tree_is_direct_symbol_reference(operand)
                && resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
                (void)parse_tree_set_reference_target(
                    node, resolution.target.symbol);
        }
        if (!refine_flagset_operator_result(grammar, unit, state, node)
            || !refine_intrinsic_binary_result(grammar, unit, state, node)
            || !refine_intrinsic_unary_result(grammar, unit, state, node))
            return;
        if (nt_is(grammar, node, "alias-stmt")
            && alias_declaration_is_transparent(node)) {
            parse_sym_entry_t *alias = core_tree_node_value(node).sym;
            if (alias && !alias->type) alias->type = th_var(state->types);
            th_type *target = alias
                ? declared_type_from_node(grammar, unit, node) : nullptr;
            if (target && th_is_closed(state->types, th_resolve(target))
                && th_unify_probe(state->types, alias->type, target))
                alias->type = th_unify_in_place(state->types, alias->type,
                                                target);
            if (alias) core_tree_node_value(node).type = alias->type;
        }
        else if (nt_is(grammar, node, "alias-stmt")) {
            parse_sym_entry_t *named = core_tree_node_value(node).sym;
            th_type *representation = declared_type_from_node(grammar, unit,
                                                              node);
            representation = representation ? th_resolve(representation)
                                             : nullptr;
            if (named && representation
                && th_kind_of(representation) == TH_INDIRECTION
                && th_is_closed(state->types, representation)) {
                core_buffer_t qualified = {0};
                append_module_type_name(unit, named->name, &qualified);
                th_type *type = named->type ? named->type
                    : qualified.data ? th_name(state->types, qualified.data)
                                     : nullptr;
                if (type) th_type_define(state->types, type, representation);
                core_buffer_release(&qualified);
            }
        }
        if (nt_is(grammar, node, "import-stmt")
            && !untyped_module_name(state, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "could not keep the imported module name out of value inference"),
                link_source_location(node));
            return;
        }
        record_function_exit_fact(grammar, unit, node, state);
        if (type_reference || dotted_symbol_path_node(grammar, node))
            remember_link_use_site(grammar, unit, state, node);
        if (dotted_symbol_path_node(grammar, node)
            && !n00bb_assignment_target(grammar, node, nullptr)
            && !resolve_module_member_site(grammar, unit, node)) return;
        if (type_reference && dotted_symbol_path_node(grammar, node)) {
            parse_node_resolution_t resolution = parse_tree_resolution(node);
            bool typed = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                && resolution.target.symbol->type;
            if (!typed && !n00bb_pending_type_reference(state, node)) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                    CORE_STRING_STATIC(
                        "could not hold the imported type reference until its module is typed"),
                    link_source_location(node));
                return;
            }
            if (typed) {
                core_tree_node_value(node).type =
                    resolution.target.symbol->type;
                if (!parse_sym_add_use(resolution.target.symbol, node)) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0013"),
                        CORE_STRING_STATIC(
                            "could not bind the imported type reference node"),
                        link_source_location(node));
                }
                else if (!bind_array_shape_alias_source(grammar, node,
                        resolution.target.symbol)) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0013"),
                        CORE_STRING_STATIC(
                            "could not bind the imported alias shape source"),
                        link_source_location(node));
                }
            }
        }
        resolve_local_type_reference(grammar, unit, node, parent);
        if (type_reference) {
            parse_node_resolution_t resolution = parse_tree_resolution(node);
            if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                && core_tree_node_value(node).type)
                propagate_resolved_type_reference(grammar, unit, node,
                    core_tree_node_value(node).type);
        }
    }
    if (!entering && nt_is(grammar, node, "enum-item")
        && !cache_enum_item_value(grammar, state, node)) {
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0018"),
            CORE_STRING_STATIC("enum item value must be an integer literal"),
            link_source_location(node));
        return;
    }
    if (!entering && state->flagset_scope
        && !cache_flagset_semantics(grammar, unit, state, node)) {
        parse_tree_t *declaration = state->flagset_scope;
        parse_sym_entry_t *owner = declaration
            ? core_tree_node_value(declaration).sym : nullptr;
        if (owner && !parse_sym_binding(owner,
                CORE_STRING_STATIC("n00bb.flagset-invalid"))) {
            parse_sym_set_binding(owner,
                CORE_STRING_STATIC("n00bb.flagset-invalid"), owner);
            n00bb_flagset_info_t *info = parse_sym_binding(owner,
                CORE_STRING_STATIC("n00bb.flagset-info"));
            bool missing_primitive = node == declaration && info
                && !info->atomic_count;
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0019"),
                missing_primitive
                    ? CORE_STRING_STATIC(
                        "flagset requires at least one primitive flag")
                    : CORE_STRING_STATIC(
                        "invalid flagset declaration; values must be unique combinations of earlier flags and no more than 128 atomic flags are allowed"),
                link_source_location(node));
        }
        return;
    }
    if (!entering && nt_is(grammar, node, "simple-lit")
        && !modified_numeric
        && !refine_integer_literal_type(grammar, unit, node, state))
        return;
    if (!entering && !cache_ordered_array_dimensions(grammar, state, node)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0007"),
            CORE_STRING_STATIC("could not cache array type dimensions"),
            link_source_location(node));
        return;
    }
    if (!entering && !cache_typecase_arm_types(grammar, state, node)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-TYPECASE"),
            CORE_STRING_STATIC("could not cache typecase arm types"),
            link_source_location(node));
        return;
    }
    if (entering
        && (nt_is(grammar, node, "body")
            || nt_is(grammar, node, "value-body")) && parent
        && (nt_is(grammar, parent, "typecase-case-block")
            || nt_is(grammar, parent, "typecase-value-case-block")))
        prepare_typecase_arm(grammar, unit, parent);
    if (!entering) {
        cache_type_syntax_node(grammar, node);
        th_type *composed = compose_type_syntax_node(grammar, unit, node);
        if (composed) core_tree_node_value(node).type = composed;
        else if (nt_is(grammar, node, "tspec-application"))
            core_tree_node_value(node).type = nullptr;
        if (nt_is(grammar, node, "variant-decl-one")) {
            parse_sym_entry_t *arm = core_tree_node_value(node).sym;
            parse_tree_t *type_node = child_nt(grammar, node, "type-spec");
            th_type *type = type_node
                ? core_tree_node_value(type_node).type : nullptr;
            if (!arm || !type) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                    CORE_STRING_STATIC(
                        "could not bind the variant arm's cached payload type"),
                    link_source_location(node));
                return;
            }
            if (arm->type) {
                type = th_unify_in_place(state->types, arm->type, type);
                if (!type) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                        CORE_STRING_STATIC(
                            "variant arm payload conflicts with its cached type"),
                        link_source_location(node));
                    return;
                }
            }
            arm->type = type;
            core_tree_node_value(node).type = type;
        }
    }
    if (!entering && (nt_is(grammar, node, "simple-lit")
        || nt_is(grammar, node, "list-lit")
        || nt_is(grammar, node, "dict-or-set-lit")
        || nt_is(grammar, node, "embed-lit"))) {
        core_string_t modifier = literal_modifier(grammar, node);
        bool string_literal = nt_is(grammar, node, "simple-lit")
            && parse_tree_find_first_terminal(node)
            && core_string_eq(parse_terminal_name(grammar,
                parse_tree_find_first_terminal(node)->tid),
                CORE_STRING_STATIC("STRING_LIT"));
        if (!modifier.data) {
            th_type *default_type = nullptr;
            if (string_literal)
                default_type = th_name(state->types, "string");
            else if (nt_is(grammar, node, "list-lit")) {
                parse_nt_node_t *parsed = &core_tree_node_value(node);
                bool array_context = parsed->type
                    && th_kind_of(th_resolve(parsed->type)) == TH_SEQUENCE;
                if (!array_context) {
                    th_type *expected = record_literal_expected_type(grammar,
                        state->types, node);
                    th_type *unified = expected
                        && th_kind_of(th_resolve(expected)) == TH_SEQUENCE
                        ? (parsed->type ? th_unify_in_place(state->types,
                               parsed->type, expected) : expected)
                        : nullptr;
                    if (unified) {
                        parsed->type = unified;
                        array_context = true;
                    }
                }
                bool user_literal = !array_context
                    && user_litmod(state, CORE_STRING_STATIC("list"),
                                   CORE_STRING_STATIC("array"), nullptr);
                if (user_literal && !infer_literal_reporting(grammar, unit,
                        node, "N00BB-T0010",
                        "could not infer the bracket literal element type"))
                    return;
                size_t before = core_diagnostics_count(
                    core_translation_unit_diagnostics(unit));
                if (!user_literal && !infer_sequence_modifier_type(grammar,
                        node, unit, state)) {
                    report_literal_failure(unit, node, before, "N00BB-T0010",
                        "could not infer the bracket literal element type");
                    return;
                }
            }
            else if (nt_is(grammar, node, "dict-or-set-lit")) {
                bool entries = child_nt(grammar, node, "dict-entries")
                    || child_nt(grammar, node, "set-entries");
                if (entries && !infer_literal_reporting(grammar, unit, node,
                        "N00BB-T0010",
                        "could not infer the brace literal element types"))
                    return;
                if (!entries && dict_only_empty_literal(grammar, node)) {
                    th_arg args[] = {
                        {.kind = TH_ARG_TYPE, .type = th_var(state->types)},
                        {.kind = TH_ARG_TYPE, .type = th_var(state->types)},
                    };
                    default_type = th_apply(state->types,
                        "builtin.containers.dict.Dict", args, 2);
                }
            }
            if (default_type) {
                parse_nt_node_t *parsed = &core_tree_node_value(node);
                parsed->type = parsed->type
                    ? th_unify_in_place(state->types, parsed->type,
                                         default_type) : default_type;
            }
        }
        else {
            bool numeric_literal = literal_token
                && (core_string_eq(literal_terminal,
                    CORE_STRING_STATIC("INT_LIT"))
                    || core_string_eq(literal_terminal,
                        CORE_STRING_STATIC("HEX_LIT"))
                    || core_string_eq(literal_terminal,
                        CORE_STRING_STATIC("FLOAT_LIT"))
                    || core_string_eq(first_token_text(node),
                        CORE_STRING_STATIC("true"))
                    || core_string_eq(first_token_text(node),
                        CORE_STRING_STATIC("false"))
                    || core_string_eq(first_token_text(node),
                        CORE_STRING_STATIC("nil")));
            if (nt_is(grammar, node, "simple-lit")
                && !string_literal && !numeric_literal) {
                return;
            }
            bool sequence = nt_is(grammar, node, "list-lit");
            bool braces = nt_is(grammar, node, "dict-or-set-lit");
            const n00bb_literal_registration_t *builtin =
                sequence ? n00bb_literal_handler(modifier,
                             N00BB_LITERAL_SEQUENCE_SHAPE)
                : braces ? n00bb_literal_handler(modifier,
                             N00BB_LITERAL_BRACE_SHAPE)
                : string_literal ? n00bb_literal_handler(modifier,
                             N00BB_LITERAL_STRING_SHAPE) : nullptr;
            if (builtin) {
                bool inferred = sequence
                    ? infer_sequence_modifier_type(grammar, node, unit,
                                                   state)
                    : braces ? infer_brace_modifier_type(grammar, node, state)
                    : infer_string_modifier_type(grammar, node, state);
                if (!inferred) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0010"),
                        CORE_STRING_STATIC("literal modifier conflicts with its payload"),
                        link_source_location(node));
                    return;
                }
            }
            else if (!infer_literal_reporting(grammar, unit, node,
                    "N00BB-T0030",
                    "literal handler cannot specialize for this payload"))
                return;
        }
    }
    if (!entering && nt_is(grammar, node, "new-expr")
        && !infer_new_expression_type(grammar, node, state)) {
        core_diagnostics_report_at(
            core_translation_unit_diagnostics(unit), unit,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0011"),
            CORE_STRING_STATIC("could not resolve the new-expression type"),
            link_source_location(node));
        return;
    }
    if (!entering && nt_is(grammar, node, "callback-expr")
        && !infer_callback_expression_type(grammar, unit, node, state))
        return;
    if (!entering && (nt_is(grammar, node, "pos-param")
        || nt_is(grammar, node, "default-param")
        || nt_is(grammar, node, "vargs-param"))) {
        bind_declared_parameter(grammar, unit, node, state);
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) validate_builtin_generic_type_arity(grammar, node, unit,
                                                       symbol->type);
        if (!record_function_parameter_fact(grammar, node, state)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC(
                    "could not retain the function parameter type fact"),
                link_source_location(node));
            return;
        }
    }
    if (entering && (nt_is(grammar, node, "func-def")
                     || nt_is(grammar, node, "callback-role")
                     || nt_is(grammar, node, "trait-spec")
                     || nt_is(grammar, node, "context-def"))) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) {
            n00bb_function_parameter_facts_t *parameter_facts =
                core_arena_alloc(&state->semantic_values,
                                 sizeof(*parameter_facts));
            if (!parameter_facts
                || !parse_sym_set_binding(symbol, N00BB_PARAMETER_FACTS_KEY,
                                          parameter_facts)) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                    CORE_STRING_STATIC(
                        "could not initialize parameter facts"),
                    link_source_location(node));
                return;
            }
        }
    }

    if (!entering && nt_is(grammar, node, "trait-spec")) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) {
            th_type *failure = nullptr;
            th_type *signature = explicit_trait_method_signature(
                grammar, unit, node, state->types, &failure);
            if (signature) {
                symbol->type = signature;
                symbol->error_type = failure
                    ? failure
                    : th_name(state->types, "nil");
                th_type *contract = th_env_signature_contract(
                    state->types, symbol->type, symbol->error_type);
                symbol->callable_contract = contract;
            }
        }
    }
    if (!entering && nt_is(grammar, node, "trait-decl")) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        core_list_t(n00bb_typevar_binding_t) variables =
            core_list_new(n00bb_typevar_binding_t);
        core_list_t(th_trait_requirement) declared =
            core_list_new(th_trait_requirement);
        signature_typevars_for_symbol(symbol, &variables);
        bool valid = apply_signature_constraints(grammar, unit,
            child_nt(grammar, node, "where-clause"), state->types,
            &variables, &declared);
        core_list_free(variables);
        if (valid && symbol) {
            n00bb_declared_trait_requirements_t *facts = core_arena_alloc(
                &state->semantic_values, sizeof(*facts));
            th_trait_requirement *items = declared.len
                ? core_arena_alloc(&state->semantic_values,
                    declared.len * sizeof(*items)) : nullptr;
            valid = facts && (!declared.len || items);
            if (valid) {
                if (declared.len)
                    memcpy(items, declared.data,
                        declared.len * sizeof(*items));
                *facts = (n00bb_declared_trait_requirements_t){
                    .data = items, .len = declared.len,
                };
                valid = parse_sym_set_binding(symbol,
                    CORE_STRING_STATIC("n00bb.trait.requirements"), facts);
            }
        }
        core_list_free(declared);
        if (!valid) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0002"),
                CORE_STRING_STATIC("could not apply trait constraints"),
                link_source_location(node));
            return;
        }
    }

    if (!entering && nt_is(grammar, node, "callback-role"))
        record_callback_role_signature(grammar, unit, node, state);
    if (!entering && nt_is(grammar, node, "callback-decl"))
        finalize_callback_contract(grammar, unit, node, state);

    if (!entering && nt_is(grammar, node, "func-def"))
        record_function_signature(grammar, unit, node, state, true);
    if (entering && nt_is(grammar, node, "context-enter"))
        record_context_hook_signature(grammar, unit, node, state);

    if (!entering) {
        if (!cache_ordered_literal_children(grammar, state, node)
            || !cache_static_literal_node(grammar, state, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0007"),
                CORE_STRING_STATIC(
                    "could not cache ordered literal child references"),
                link_source_location(node));
            return;
        }
        if (nt_is(grammar, node, "top-level-stmt")) {
            parse_tree_t *statement = top_level_statement_child(grammar, node);
            if (!statement) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0010"),
                    CORE_STRING_STATIC(
                        "could not cache a top-level statement node"),
                    link_source_location(node));
                return;
            }
            core_list_push(state->module_top_level_statements, statement);
        }
        if (nt_is(grammar, node, "sym-decl")) {
            parse_tree_t *type_spec = child_nt(grammar, node, "type-spec");
            parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
            parse_tree_t *initializer = child_nt(grammar, node, "expression");
            if (symbol && initializer
                && !parse_sym_set_binding(symbol,
                    CORE_STRING_STATIC("n00bb.initializer-expression"),
                    initializer)) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                    CORE_STRING_STATIC("could not retain symbol initializer"),
                    link_source_location(node));
                return;
            }
            th_type *declared = type_spec
                ? declared_type_from_node(grammar, unit, type_spec) : nullptr;
            if (symbol && declared) {
                symbol->type = declared;
                core_tree_node_value(node).type = declared;
                core_tree_node_value(type_spec).type = declared;
                validate_builtin_generic_type_arity(grammar, type_spec, unit,
                                                    declared);
            }
        }
        if (nt_is(grammar, node, "variable-decl")) {
            parse_tree_t *decl_list = child_nt(grammar, node,
                                               "sym-decl-list");
            for (size_t i = 0; decl_list
                    && i < core_tree_num_children(decl_list); i++) {
                parse_tree_t *declaration = core_tree_child(decl_list, i);
                parse_sym_entry_t *symbol =
                    core_tree_node_value(declaration).sym;
                n00bb_static_value_t value;
                if (!symbol || !semantic_static_literal_value(
                        grammar, declaration, state, &value)) continue;
                n00bb_static_value_t *stored = core_arena_alloc(
                    &state->semantic_values, sizeof(*stored));
                if (!stored) {
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0007"),
                        CORE_STRING_STATIC(
                            "could not retain a semantic static value"),
                        link_source_location(declaration));
                    return;
                }
                *stored = value;
                symbol->comptime_value = stored;
                symbol->has_comptime_value = true;
            }
        }
        if (nt_is(grammar, node, "tuple-or-paren")
            && literal_modifier(grammar, node).data) {
            bool shape = child_nt(grammar, node, "named-field-list")
                || core_tree_num_children(node) == 5;
            if (shape)
                (void)infer_literal_reporting(grammar, unit, node,
                    "N00BB-T0030",
                    "literal handler cannot specialize for this payload");
            else
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0030"),
                    CORE_STRING_STATIC(
                        "literal modifier requires a tuple or record literal"),
                    link_source_location(node));
        }
        else if (nt_is(grammar, node, "tuple-or-paren")
            && child_nt(grammar, node, "named-field-list")
            && (!contextualize_record_fields_before_inference(grammar,
                    unit, state, node)
                || !infer_anonymous_record(grammar, node, state))) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0006"),
                CORE_STRING_STATIC(
                    "could not construct the anonymous record type"),
                link_source_location(node));
            return;
        }
        if (nt_is(grammar, node, "tuple-or-paren")
            && !literal_modifier(grammar, node).data
            && !child_nt(grammar, node, "named-field-list")
            && !infer_tuple_literal_type(grammar, node, state)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0008"),
                CORE_STRING_STATIC("could not infer the tuple literal type"),
                link_source_location(node));
            return;
        }
        if (field_record_declaration(grammar, node)) {
            if (!cache_record_layout_request(grammar, unit, node)) {
                if (!core_diagnostics_failed(
                        core_translation_unit_diagnostics(unit)))
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0069"),
                        CORE_STRING_STATIC(
                            "could not retain the record layout request"),
                        link_source_location(node));
                return;
            }
            if (!cache_record_field_extents(grammar, unit, node)) {
                if (!core_diagnostics_failed(
                        core_translation_unit_diagnostics(unit)))
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0037"),
                        CORE_STRING_STATIC(
                            "could not retain the record field extents"),
                        link_source_location(node));
                return;
            }
            if (!register_record_semantic_shape(grammar, unit, node)) {
                if (!core_translation_unit_has_fatal_diagnostics(unit))
                    core_diagnostics_report_at(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0005"),
                        CORE_STRING_STATIC(
                            "could not register the record shape for semantic analysis"),
                        link_source_location(node));
                return;
            }
        }
        if (nt_is(grammar, node, "variant-decl")
            && !finalize_variant_symbol_type(grammar, node, unit, state)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0012"),
                CORE_STRING_STATIC(
                    "could not resolve the declared variant type"),
                link_source_location(node));
            return;
        }
        if (nt_is(grammar, node, "enum-stmt")
            && !finalize_enum_symbol_type(grammar, node, unit, state)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0012"),
                CORE_STRING_STATIC(
                    "could not resolve the declared enum type"),
                link_source_location(node));
            return;
        }
        if (nt_is(grammar, node, "flagset-stmt")
            && !finalize_flagset_symbol_type(grammar, node, unit, state)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0019"),
                CORE_STRING_STATIC("could not resolve the declared flagset type"),
                link_source_location(node));
            return;
        }
        remember_link_use_site(grammar, unit, state, node);
        if (!record_generic_operator_failure(grammar, state, node)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not retain operator failure constraints"),
                link_source_location(node));
            return;
        }
        if (!cache_return_source_fact(grammar, node, state)) {
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
                CORE_STRING_STATIC("could not retain return-source facts"),
                link_source_location(node));
            return;
        }
        record_reference_facts(grammar, unit, node);
    }

    if (!entering && node == n00bb_tu_parsed_tree(state)) {
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->is_field && !symbol->type && symbol->decl_node) {
                    th_type *field_type = declared_type_from_node(
                        grammar, unit, symbol->decl_node);
                    if (field_type) {
                        symbol->type = field_type;
                        core_tree_node_value(symbol->decl_node).type =
                            field_type;
                    }
                }
                if (!symbol->type && symbol->kind != PARSE_SYM_MODULE) {
                    parse_sym_mark_type_inferred(symbol);
                }
            }
        }
    }
    if (!entering) n00bb_range_after_children(grammar, state, node);
    if (!entering && nt_is(grammar, node, "expression")) {
        th_type *argument = core_tree_node_value(node).argument_type;
        if (argument) {
            int64_t minimum = 0, maximum = 0;
            th_type *source = core_tree_node_value(node).type;
            bool integer = source && is_integer_type(source);
            bool ranged = integer && n00bb_current_integer_range(state,
                node, &minimum, &maximum);
            if (!(integer
                    ? n00bb_constrain_integer_fit(state, argument, source,
                        ranged, minimum, maximum)
                    : source && th_unify_in_place(state->types, argument,
                                                   source))) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
                    CORE_STRING_STATIC(
                        "indexed assignment value has an incompatible type"),
                    link_source_location(node));
                return;
            }
        }
    }
    if (!entering) n00bb_range_after_annotation(grammar, state, node, parent);
}

typedef struct {
    th_type *expected;
    parse_scope_t *flow_scope;
    parse_scope_t *previous_scope;
    semantic_annotation_cache_t annotation_cache;
    bool in_expression;
} semantic_walk_frame_t;
core_list_decl(semantic_walk_frame_t);

typedef struct {
    annotation_context_t *context;
    core_translation_unit_t *unit;
    core_list_t(semantic_walk_frame_t) frames;
    bool skip_typed_ahead;
} semantic_walk_context_t;

static bool
type_declaration_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return field_record_declaration(grammar, node)
        || nt_is(grammar, node, "variant-decl")
        || nt_is(grammar, node, "enum-stmt")
        || nt_is(grammar, node, "flagset-stmt")
        || nt_is(grammar, node, "alias-stmt");
}

static bool
executable_scope_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const char *const executable[] = {
        "block", "if", "for", "switch", "try", "try-error", "typecase",
        "while", "with",
    };
    const parse_rule_t *rule = node && !core_tree_is_leaf(node)
        ? parse_tree_rule(grammar, node) : nullptr;
    parse_annotation_t *scope = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_SCOPE_OPEN)
        : nullptr;
    for (size_t i = 0; scope
         && i < sizeof(executable) / sizeof(executable[0]); i++)
        if (core_string_eq(scope->scope_tag,
                           core_string_from_cstr(executable[i])))
            return true;
    return false;
}

static parse_visit_outcome_t
semantic_walk_status(semantic_walk_context_t *walk)
{
    return core_diagnostics_failed(
        core_translation_unit_diagnostics(walk->unit))
        ? PARSE_VISIT_ERROR : PARSE_VISIT_CONTINUE;
}

static th_type *
enclosing_array_element_type(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *child = node;
    for (parse_tree_t *parent = core_tree_node_value(child).parent; parent;
         child = parent, parent = core_tree_node_value(parent).parent) {
        if (nt_is(grammar, parent, "list-lit")) {
            if (child != child_nt(grammar, parent, "list-elems"))
                return nullptr;
            th_type *type = core_tree_node_value(parent).type;
            type = type ? th_resolve(type) : nullptr;
            return type && th_kind_of(type) == TH_SEQUENCE
                ? th_seq_elem(type) : nullptr;
        }
        bool grouped = core_tree_node_value(parent).group_top
            && nt_is(grammar, core_tree_node_value(parent).parent,
                     "list-elems");
        if (grouped || nt_is(grammar, parent, "list-elems")) {
            if (!nt_is(grammar, child, "expression")
                && !core_tree_node_value(child).group_top) return nullptr;
            continue;
        }
        if (core_tree_num_children(parent) != 1
            || core_tree_child(parent, 0) != child) return nullptr;
    }
    return nullptr;
}

static parse_visit_outcome_t
semantic_walk_prepare(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    annotation_context_t *context = walk->context;
    core_translation_unit_t *unit = walk->unit;
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state->annotations->symtab;
    while (walk->frames.len <= (size_t)visit->depth)
        core_list_push(walk->frames, ((semantic_walk_frame_t){0}));
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    *frame = (semantic_walk_frame_t){0};
    bool typed_ahead = false;
    if (walk->skip_typed_ahead)
        core_dict_get(&state->declarations_typed_ahead, visit->node,
                      &typed_ahead);
    if (typed_ahead) return PARSE_VISIT_PRUNE;
    if (visit->depth > 0) {
        semantic_walk_frame_t *parent = &walk->frames.data[visit->depth - 1];
        frame->expected = expected_expression_child_type(context->grammar,
            visit->parent, visit->node, parent->expected);
        frame->in_expression = parent->in_expression;
        if (parent->flow_scope) {
            frame->previous_scope = parse_symtab_current_scope(symtab,
                CORE_STRING_STATIC(""));
            parse_symtab_enter_scope_all(symtab, parent->flow_scope);
        }
    }
    if (core_tree_is_leaf(visit->node)) {
        parse_token_info_t *token = parse_node_token(visit->node);
        if (token && core_option_is_set(token->postfix_modifier)
            && !literal_modifier_terminal(context->grammar, visit->node,
                                          visit->parent))
            core_diagnostics_report_at(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0010"),
                CORE_STRING_STATIC("literal modifier requires a supported literal shape"),
                link_source_location(visit->parent ? visit->parent
                                                   : visit->node));
        return semantic_walk_status(walk);
    }
    if (nt_is(context->grammar, visit->node, "list-lit")
        && !core_tree_node_value(visit->node).type) {
        th_type *expected = frame->expected ? frame->expected
            : enclosing_array_element_type(context->grammar, visit->node);
        if (expected && th_kind_of(th_resolve(expected)) == TH_SEQUENCE)
            core_tree_node_value(visit->node).type = expected;
    }
    frame->in_expression |= nt_is(context->grammar, visit->node,
                                   "expression");
    if (nt_is(context->grammar, visit->node, "flagset-stmt")
        && tu_state(unit))
        tu_state(unit)->flagset_scope = visit->node;
    semantic_annotation_node(context, unit, visit->node, visit->parent,
                             SEMANTIC_PREPARE_ENTER, visit,
                             &frame->annotation_cache);
    return semantic_walk_status(walk);
}

static parse_visit_outcome_t
semantic_walk_after_enter(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_AFTER_ENTER, nullptr,
                             &frame->annotation_cache);
    return semantic_walk_status(walk);
}

static parse_visit_outcome_t
semantic_walk_before_exit(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    if (core_tree_is_leaf(visit->node)) {
        if (frame->previous_scope)
            parse_symtab_enter_scope_all(tu_state(walk->unit)->annotations->symtab,
                                         frame->previous_scope);
        return PARSE_VISIT_CONTINUE;
    }
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_PREPARE_EXIT, visit,
                             &frame->annotation_cache);
    return semantic_walk_status(walk);
}

static parse_visit_outcome_t
semantic_walk_after_reference_exit(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_AFTER_REFERENCE_EXIT,
                             nullptr, &frame->annotation_cache);
    return semantic_walk_status(walk);
}

static parse_visit_outcome_t
semantic_walk_after_exit(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_AFTER_EXIT, nullptr,
                             &frame->annotation_cache);
    n00bb_tu_state_t *exiting_state = tu_state(walk->unit);
    if (exiting_state && exiting_state->flagset_scope == visit->node)
        exiting_state->flagset_scope = nullptr;
    if (frame->in_expression
        && !cache_alias_edges(walk->context->grammar, visit->node,
                              tu_state(walk->unit)))
        core_diagnostics_report_at(core_translation_unit_diagnostics(walk->unit),
            walk->unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not retain alias edges"),
            link_source_location(visit->node));
    if (frame->in_expression
        && !cache_escape_edges(walk->context->grammar, visit->node,
                               tu_state(walk->unit)))
        core_diagnostics_report_at(core_translation_unit_diagnostics(walk->unit),
            walk->unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0004"),
            CORE_STRING_STATIC("could not retain escape edges"),
            link_source_location(visit->node));
    if (frame->previous_scope)
        parse_symtab_enter_scope_all(tu_state(walk->unit)->annotations->symtab,
                                     frame->previous_scope);
    return semantic_walk_status(walk);
}

static bool
switch_variant_case_names(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(core_string_t) *names)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (nt_is(grammar, node, "body") || nt_is(grammar, node, "value-body"))
        return true;
    if (nt_is(grammar, node, "case-expr-item")) {
        core_string_t name = {0};
        parse_tree_t *expression = child_nt(grammar, node, "expression");
        if (child_nt(grammar, node, "range-sep")
            || !simple_identifier_expression(grammar, expression, &name))
            return false;
        core_list_push(*names, name);
        return true;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (!switch_variant_case_names(grammar, core_tree_child(node, i),
                                       names)) return false;
    return true;
}

static void
find_switch_variant_candidate(parse_grammar_t *grammar,
    core_translation_unit_t *unit, core_translation_unit_t *root,
    const core_list_t(core_string_t) *names,
    core_list_t(core_translation_unit_ptr_t) *visited,
    th_type **candidate, bool *ambiguous)
{
    if (*ambiguous) return;
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == unit) return;
    core_list_push(*visited, unit);
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->types && state->annotations
        ? state->annotations->symtab : nullptr;
    for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
         scope && !*ambiguous; scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope;
             symbol && !*ambiguous; symbol = symbol->next_in_scope) {
            parse_scope_t *arms = symbol->member_scope;
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type || !arms
                || !core_string_eq(arms->scope_tag,
                                   CORE_STRING_STATIC("variant"))
                || (unit != root && !core_string_eq(symbol->visibility,
                                                    CORE_STRING_STATIC("public")))
                || !th_is_closed(state->types, symbol->type)) continue;
            bool matches = true;
            for (size_t i = 0; i < names->len; i++) {
                parse_sym_entry_t *arm = parse_symtab_lookup_in(
                    arms, names->data[i]);
                if (!n00bb_variant_arm_symbol(grammar, arm)) {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;
            th_type *type = th_resolve(symbol->type);
            if (*candidate && *candidate != type) *ambiguous = true;
            else *candidate = type;
        }
    }
    for (size_t i = 0; !*ambiguous
            && i < core_translation_unit_import_count(unit); i++)
        find_switch_variant_candidate(grammar,
            core_translation_unit_import(unit, i), root, names, visited,
            candidate, ambiguous);
}

static bool
infer_indexed_switch_variant_subject(parse_grammar_t *grammar,
                                     core_translation_unit_t *unit,
                                     parse_tree_t *switch_node,
                                     parse_tree_t *subject_expression)
{
    if ((!nt_is(grammar, switch_node, "switch-stmt")
         && !nt_is(grammar, switch_node, "switch-value-expr"))
        || !nt_is(grammar, subject_expression, "expression")) return true;
    parse_tree_t *subject = n00bb_simple_lvalue(subject_expression);
    parse_sym_entry_t *symbol = subject
        ? attached_symbol(grammar, unit, subject) : nullptr;
    parse_tree_t *initializer = symbol ? parse_sym_binding(symbol,
        CORE_STRING_STATIC("n00bb.initializer-expression")) : nullptr;
    if (!symbol || symbol->kind != PARSE_SYM_VARIABLE
        || !symbol->type_is_inferred || !initializer
        || !n00bb_index_expression_node(grammar, initializer)
        || !symbol->type
        || th_kind_of(th_resolve(symbol->type)) != TH_VAR) return true;
    parse_tree_t *cases = child_nt(grammar, switch_node,
        nt_is(grammar, switch_node, "switch-stmt")
            ? "switch-cases" : "switch-value-cases");
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    bool named_cases = switch_variant_case_names(grammar, cases, &names);
    th_type *candidate = nullptr;
    bool ambiguous = false;
    if (named_cases && names.len) {
        core_list_t(core_translation_unit_ptr_t) visited =
            core_list_new(core_translation_unit_ptr_t);
        find_switch_variant_candidate(grammar, unit, unit, &names,
                                      &visited, &candidate, &ambiguous);
        core_list_free(visited);
    }
    core_list_free(names);
    if (!candidate || ambiguous) return true;
    th_type *unified = th_unify_in_place(tu_state(unit)->types,
                                        symbol->type, candidate);
    if (unified) return true;
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0001"),
        CORE_STRING_STATIC(
            "switch cases conflict with the inferred subject type"),
        link_source_location(switch_node));
    return false;
}

static parse_visit_outcome_t
semantic_walk_child_complete(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    annotation_context_t *context = walk->context;
    core_translation_unit_t *unit = walk->unit;
    parse_tree_t *node = visit->node;
    parse_tree_t *child = visit->completed_child;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    parse_symtab_t *symtab = tu_state(unit)->annotations->symtab;
    if (!infer_indexed_switch_variant_subject(context->grammar, unit,
                                               node, child))
        return PARSE_VISIT_ERROR;
    bool body_statement_sequence = nt_is(context->grammar, node,
            "body-stmt-list")
        || nt_is(context->grammar, node, "other-top-stmts")
        || (core_tree_node_value(node).group_top && visit->parent
            && (nt_is(context->grammar, visit->parent, "body-stmt-list")
                || nt_is(context->grammar, visit->parent,
                         "other-top-stmts")));
    bool top_level = nt_is(context->grammar, node, "other-top-stmts")
        || (core_tree_node_value(node).group_top && visit->parent
            && nt_is(context->grammar, visit->parent, "other-top-stmts"));
    bool short_circuit = core_tree_num_children(node) == 3
        && core_tree_child(node, 0) == child
        && (nt_is(context->grammar, node, "and-expr")
            || nt_is(context->grammar, node, "or-expr"));
    if (short_circuit || (body_statement_sequence
                          && (nt_is(context->grammar, child, "body-stmt")
                              || nt_is(context->grammar, child,
                                       "top-level-stmt")))) {
        bool condition_truth = short_circuit
            && nt_is(context->grammar, node, "and-expr");
        parse_tree_t *condition = short_circuit ? child
            : n00bb_exiting_guard_condition(context->grammar, child);
        if (!condition) {
            parse_tree_t *assertion = child_nt(context->grammar, child,
                                               "assert-stmt");
            if (assertion
                && core_string_eq(first_token_text(assertion),
                                  CORE_STRING_STATIC("require"))) {
                condition = child_nt(context->grammar, assertion,
                                     "expression");
                condition_truth = true;
            }
        }
        if (condition && top_level && !short_circuit) {
            if (!n00bb_bind_option_condition_shadows(context->grammar, unit,
                    symtab, nullptr, condition, condition_truth, node, child,
                    true)) return PARSE_VISIT_ERROR;
        }
        else if (condition) {
            parse_scope_t *saved = parse_symtab_current_scope(symtab,
                CORE_STRING_STATIC(""));
            if (frame->flow_scope)
                parse_symtab_enter_scope_all(symtab, frame->flow_scope);
            parse_symtab_push_scope_all(symtab, CORE_STRING_STATIC("flow"));
            parse_scope_t *new_scope = parse_symtab_current_scope(symtab,
                CORE_STRING_STATIC(""));
            bool bound = new_scope
                && n00bb_bind_option_condition_shadows(context->grammar,
                    unit, symtab, new_scope, condition, condition_truth,
                    node, child, false);
            parse_symtab_enter_scope_all(symtab, saved);
            if (!bound) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-T0015"),
                    CORE_STRING_STATIC(
                        "could not bind the option refinement after this guard"),
                    link_source_location(child));
                return PARSE_VISIT_ERROR;
            }
            frame->flow_scope = new_scope;
        }
    }
    if (nt_is(context->grammar, node, "func-def")
        && nt_is(context->grammar, child, "func-type-signature")) {
        record_cached_declared_function_signature(context->grammar,
            unit, child, tu_state(unit));
        bind_function_body_signature(unit, child, tu_state(unit));
    }
    if (nt_is(context->grammar, node, "context-def")
        && nt_is(context->grammar, child, "context-spec"))
        record_context_signature(context->grammar, unit, node,
                                 tu_state(unit));
    if ((nt_is(context->grammar, node, "context-exit")
         || nt_is(context->grammar, node, "context-error")
         || nt_is(context->grammar, node, "context-ensure"))
        && nt_is(context->grammar, child, "context-hook-params"))
        record_context_hook_signature(context->grammar, unit, node,
                                      tu_state(unit));
    return semantic_walk_status(walk);
}

static bool
run_semantic_walk(parse_grammar_t *grammar, th_ctx *types,
                  core_translation_unit_t *unit, parse_tree_t *root,
                  parse_tree_t *parent, int32_t depth, int32_t child_index,
                  bool skip_typed_ahead)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!root || !state || !state->annotations) return false;
    core_string_t package = n00bb_package_id(unit);
    if (!th_ctx_set_scope(types, unit->module.identifier.data, package.data))
        return false;
    annotation_context_t context = {
        .grammar = grammar,
        .types = types,
    };
    semantic_walk_context_t walk_context = {
        .context = &context,
        .unit = unit,
        .skip_typed_ahead = skip_typed_ahead,
    };
    parse_tree_walk_t *walk = parse_tree_walk_new(grammar,
                                                  &walk_context);
    parse_annot_kind_t order[
        sizeof(semantic_kinds) / sizeof(semantic_kinds[0])
        + sizeof(semantic_reference_kinds)
            / sizeof(semantic_reference_kinds[0])];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(semantic_kinds)
                            / sizeof(semantic_kinds[0]); i++)
        order[count++] = semantic_kinds[i];
    for (size_t i = 0; i < sizeof(semantic_reference_kinds)
                            / sizeof(semantic_reference_kinds[0]); i++)
        order[count++] = semantic_reference_kinds[i];
    bool configured = walk && parse_walk_set_annotation_order(walk, order,
                                                               count);
    if (configured)
        configured = parse_walk_on_annotation_boundary(walk,
            sizeof(semantic_kinds) / sizeof(semantic_kinds[0]),
            nullptr, semantic_walk_after_reference_exit, nullptr);
    if (configured) {
        parse_annot_register(walk, state->annotations, state->types, nullptr);
        parse_walk_on_stage(walk, PARSE_WALK_PREPARE,
                            semantic_walk_prepare, nullptr);
        parse_walk_on_stage(walk, PARSE_WALK_AFTER_ENTER,
                            semantic_walk_after_enter, nullptr);
        parse_walk_on_stage(walk, PARSE_WALK_BEFORE_EXIT,
                            semantic_walk_before_exit, nullptr);
        parse_walk_on_stage(walk, PARSE_WALK_AFTER_EXIT,
                            semantic_walk_after_exit, nullptr);
        parse_walk_on_stage(walk, PARSE_WALK_CHILD_COMPLETE,
                            semantic_walk_child_complete, nullptr);
        if (parent)
            parse_tree_walk_run_with_parent(walk, root, parent, depth,
                                            child_index);
        else parse_tree_walk_run(walk, root);
    }
    bool ok = configured && !parse_tree_walk_failed(walk)
        && !core_diagnostics_failed(core_translation_unit_diagnostics(unit));
    parse_tree_walk_free(walk);
    core_list_free(walk_context.frames);
    return ok;
}

static void
collect_import_statements(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(n00bb_tree_ptr_t) *imports)
{
    if (!node || core_tree_is_leaf(node) || nt_is(grammar, node, "func-def"))
        return;
    if (nt_is(grammar, node, "import-stmt")) {
        core_list_push(*imports, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_import_statements(grammar, core_tree_child(node, i), imports);
}

static void
report_imported_module_name(core_translation_unit_t *unit,
                            core_string_t name, parse_tree_t *import,
                            core_diag_location_t location)
{
    core_diag_location_t imported = link_source_location(import);
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "'%.*s' cannot be declared here because this module imports module '%.*s' (line %u)",
        (int)name.u8_bytes, name.data, (int)name.u8_bytes, name.data,
        imported.line);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-M0012"),
        core_string_borrow(message.data, message.byte_len), location);
    core_buffer_release(&message);
}

bool
n00bb_declares_litmod(n00bb_tu_state_t *state, parse_sym_entry_t *symbol)
{
    for (size_t m = 0; state && m < state->literal_modifiers.len; m++) {
        n00bb_user_litmod_t *registration = &state->literal_modifiers.data[m];
        parse_tree_t *declaration = registration->declaration;
        if ((symbol && registration->function == symbol)
            || (declaration && !core_tree_is_leaf(declaration)
                && core_tree_node_value(declaration).sym == symbol))
            return true;
    }
    return false;
}

static bool
reject_imported_module_names(parse_grammar_t *grammar,
                             core_translation_unit_t *unit,
                             n00bb_tu_state_t *state, parse_tree_t *tree)
{
    core_list_t(n00bb_tree_ptr_t) imports = core_list_new(n00bb_tree_ptr_t);
    collect_import_statements(grammar, tree, &imports);
    parse_scope_t *scopes[] = {module_scope(state), type_module_scope(state)};
    bool distinct = true;
    for (size_t i = 0; i < imports.len; i++) {
        core_string_t name = first_token_text(
            child_nt(grammar, imports.data[i], "member-chain"));
        if (!name.u8_bytes) continue;
        for (size_t s = 0; s < sizeof(scopes) / sizeof(scopes[0]); s++)
            for (parse_sym_entry_t *symbol = scopes[s]
                     ? scopes[s]->first_in_scope : nullptr;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind == PARSE_SYM_MODULE || !symbol->decl_node
                    || !core_string_eq(symbol->name, name)
                    || n00bb_declares_litmod(state, symbol)) continue;
                report_imported_module_name(unit, name, imports.data[i],
                    link_source_location(symbol->decl_node));
                distinct = false;
            }
    }
    core_list_free(imports);
    return distinct;
}

static parse_visit_outcome_t
declaration_walk_prepare(parse_visit_t *visit)
{
    return executable_scope_node(visit->grammar, visit->node)
        ? PARSE_VISIT_PRUNE : PARSE_VISIT_CONTINUE;
}

static bool
declare_unit(parse_grammar_t *grammar, th_ctx *types,
             core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *tree = state ? n00bb_tu_parsed_tree(state) : nullptr;
    core_string_t package = n00bb_package_id(unit);
    if (!tree || !th_ctx_set_scope(types, unit->module.identifier.data,
                                   package.data)
        || !prepare_initial_symbols(unit, types)) return false;
    state->semantic_annotation_started = true;
    parse_annot_result_set_node_callback(state->annotations,
                                         declared_symbol_node, nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(grammar, nullptr);
    if (walk) {
        parse_annot_register_declarations(walk, state->annotations);
        parse_walk_on_stage(walk, PARSE_WALK_PREPARE,
                            declaration_walk_prepare, nullptr);
        parse_tree_walk_run(walk, tree);
    }
    bool ok = walk && !parse_tree_walk_failed(walk);
    parse_tree_walk_free(walk);
    parse_annot_result_set_node_callback(state->annotations,
                                         initial_symbol_node,
                                         state->annotations);
    return ok && !core_diagnostics_failed(
        core_translation_unit_diagnostics(unit));
}

static th_type *
nominal_declaration_type(parse_grammar_t *grammar, th_ctx *types,
                         parse_tree_t *declaration, const char *qualified)
{
    if (nt_is(grammar, declaration, "variant-decl"))
        return th_variant(types, qualified, nullptr, 0);
    if (nt_is(grammar, declaration, "alias-stmt")
        && alias_declaration_is_transparent(declaration))
        return th_var(types);
    bool nominal = field_record_declaration(grammar, declaration)
        || nt_is(grammar, declaration, "enum-stmt")
        || nt_is(grammar, declaration, "flagset-stmt")
        || nt_is(grammar, declaration, "callback-decl")
        || nt_is(grammar, declaration, "alias-stmt");
    return nominal ? th_name(types, qualified) : nullptr;
}

static void
declare_nominal_types(parse_grammar_t *grammar, th_ctx *types,
                      core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next)
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->type || symbol->kind != PARSE_SYM_TYPEDEF
                || !symbol->decl_node) continue;
            core_buffer_t qualified = {0};
            append_module_type_name(unit, symbol->name, &qualified);
            if (qualified.data)
                symbol->type = nominal_declaration_type(grammar, types,
                    symbol->decl_node, qualified.data);
            core_buffer_release(&qualified);
        }
}

static bool type_declaration_ahead(parse_grammar_t *grammar, th_ctx *types,
                                   core_translation_unit_t *unit,
                                   parse_tree_t *declaration);

static parse_tree_t *
module_constant_declaration(parse_grammar_t *grammar, parse_sym_entry_t *symbol)
{
    if (!symbol || symbol->kind != PARSE_SYM_VARIABLE || !symbol->decl_node
        || !nt_is(grammar, symbol->decl_node, "sym-decl")) return nullptr;
    parse_tree_t *declaration = symbol->decl_node;
    while (declaration && !nt_is(grammar, declaration, "variable-decl"))
        declaration = core_tree_node_value(declaration).parent;
    parse_tree_t *prefix = child_nt(grammar, declaration,
                                    "declaration-prefix");
    if (!prefix || !core_string_eq(first_token_text(child_nt(grammar, prefix,
            "binding-kind")), CORE_STRING_STATIC("const"))) return nullptr;
    for (parse_tree_t *at = core_tree_node_value(declaration).parent; at;
         at = core_tree_node_value(at).parent)
        if (executable_scope_node(grammar, at)
            || nt_is(grammar, at, "func-def")) return nullptr;
    return declaration;
}

static bool
type_referenced_constants_ahead(parse_grammar_t *grammar, th_ctx *types,
                                core_translation_unit_t *unit,
                                parse_tree_t *declaration, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return true;
    parse_scope_t *scope = nullptr;
    for (parse_tree_t *at = node; at && !scope;
         at = core_tree_node_value(at).parent)
        scope = core_tree_node_value(at).scope;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (!core_tree_is_leaf(child)) {
            if (!type_referenced_constants_ahead(grammar, types, unit,
                    declaration, child)) return false;
            continue;
        }
        parse_sym_entry_t *symbol = scope
            ? parse_symtab_lookup_scoped(tu_state(unit)->annotations->symtab,
                scope, core_string_empty(), parse_tree_leaf_text(child))
            : nullptr;
        parse_tree_t *constant = module_constant_declaration(grammar, symbol);
        if (constant && constant != declaration
            && !type_declaration_ahead(grammar, types, unit, constant))
            return false;
    }
    return true;
}

static bool
type_declaration_ahead(parse_grammar_t *grammar, th_ctx *types,
                       core_translation_unit_t *unit,
                       parse_tree_t *declaration)
{
    n00bb_tu_state_t *state = tu_state(unit);
    bool typed = false;
    core_dict_get(&state->declarations_typed_ahead, declaration, &typed);
    if (typed) return true;
    core_dict_put(&state->declarations_typed_ahead, declaration, declaration);
    if (!type_referenced_constants_ahead(grammar, types, unit, declaration,
                                         declaration)) return false;
    parse_tree_t *parent = core_tree_node_value(declaration).parent;
    int32_t child_index = 0;
    int32_t depth = 0;
    for (size_t i = 0; parent && i < core_tree_num_children(parent); i++)
        if (core_tree_child(parent, i) == declaration)
            child_index = (int32_t)i;
    for (parse_tree_t *at = parent; at; at = core_tree_node_value(at).parent)
        depth++;
    parse_scope_t *scope = core_tree_node_value(declaration).scope;
    if (scope && scope->node == declaration) scope = scope->parent;
    if (scope)
        parse_symtab_enter_scope_all(state->annotations->symtab, scope);
    return run_semantic_walk(grammar, types, unit, declaration, parent, depth,
                             child_index, false);
}

static bool
annotate_type_declarations(parse_grammar_t *grammar, th_ctx *types,
                           core_translation_unit_t *unit, parse_tree_t *node)
{
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child) || executable_scope_node(grammar, child))
            continue;
        if (!(type_declaration_node(grammar, child)
              ? type_declaration_ahead(grammar, types, unit, child)
              : annotate_type_declarations(grammar, types, unit, child)))
            return false;
    }
    return true;
}

static bool
annotate_unit_bodies(parse_grammar_t *grammar, th_ctx *types,
                     core_translation_unit_t *unit,
                     n00bb_semantic_unit_finish_t finish, void *context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_tree_t *tree = n00bb_tu_parsed_tree(state);
    if (!report_shadowed_litmods(unit, state)) return false;
    bool annotated = run_semantic_walk(grammar, types, unit, tree, nullptr,
                                       0, 0, true);
    if (!reject_imported_module_names(grammar, unit, state, tree)
        || !annotated
        || !resolve_forward_literal_sites(unit, state)
        || core_translation_unit_has_fatal_diagnostics(unit)
        || core_diagnostics_failed(core_translation_unit_diagnostics(unit))
        || (finish && !finish(context, unit))) return false;
    state->semantic_annotation_complete = true;
    return true;
}

static bool
annotate_component(parse_grammar_t *grammar, th_ctx *types,
                   core_list_t(core_translation_unit_ptr_t) *members,
                   n00bb_semantic_unit_finish_t finish, void *context)
{
    for (size_t i = 0; i < members->len; i++)
        if (!declare_unit(grammar, types, members->data[i])) return false;
    if (!bind_imported_module_scopes(members)) return false;
    for (size_t i = 0; i < members->len; i++)
        declare_nominal_types(grammar, types, members->data[i]);
    for (size_t i = 0; i < members->len; i++) {
        core_translation_unit_t *unit = members->data[i];
        if (!annotate_type_declarations(grammar, types, unit,
                n00bb_tu_parsed_tree(tu_state(unit)))) return false;
    }
    for (size_t i = 0; i < members->len; i++)
        if (!annotate_unit_bodies(grammar, types, members->data[i], finish,
                                  context)) return false;
    return true;
}

typedef struct {
    size_t index;
    size_t low;
    bool on_stack;
} component_vertex_t;

typedef struct {
    parse_grammar_t *grammar;
    th_ctx *types;
    n00bb_semantic_unit_finish_t finish;
    void *context;
    core_dict_t vertices;
    core_list_t(core_translation_unit_ptr_t) stack;
    size_t next_index;
} component_search_t;

static bool
annotate_components_from(component_search_t *search,
                         core_translation_unit_t *unit,
                         component_vertex_t **out)
{
    component_vertex_t *vertex = core_alloc(component_vertex_t);
    if (!vertex) return false;
    *vertex = (component_vertex_t){
        .index = search->next_index,
        .low = search->next_index,
        .on_stack = true,
    };
    search->next_index++;
    core_dict_put(&search->vertices, unit, vertex);
    core_list_push(search->stack, unit);
    *out = vertex;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        n00bb_tu_state_t *imported_state = tu_state(imported);
        if (!imported_state) return false;
        if (imported_state->semantic_annotation_complete) continue;
        bool found = false;
        component_vertex_t *next = core_dict_get(&search->vertices,
                                                 imported, &found);
        if (!found) {
            if (!annotate_components_from(search, imported, &next))
                return false;
            if (next->low < vertex->low) vertex->low = next->low;
        }
        else if (next->on_stack && next->index < vertex->low)
            vertex->low = next->index;
    }
    if (vertex->low != vertex->index) return true;
    core_list_t(core_translation_unit_ptr_t) members =
        core_list_new(core_translation_unit_ptr_t);
    core_translation_unit_t *member = nullptr;
    do {
        member = search->stack.data[--search->stack.len];
        component_vertex_t *popped = core_dict_get(&search->vertices,
                                                   member, nullptr);
        popped->on_stack = false;
        core_list_push(members, member);
    } while (member != unit);
    bool annotated = annotate_component(search->grammar, search->types,
                                        &members, search->finish,
                                        search->context);
    core_list_free(members);
    return annotated;
}

bool
semantic_annotate_closure(parse_grammar_t *grammar, th_ctx *types,
    core_translation_unit_t *unit, n00bb_semantic_unit_finish_t finish,
    void *context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state) return false;
    if (state->semantic_annotation_complete) return true;
    component_search_t search = {
        .grammar = grammar,
        .types = types,
        .finish = finish,
        .context = context,
        .stack = core_list_new(core_translation_unit_ptr_t),
    };
    core_dict_init(&search.vertices, nullptr, nullptr);
    component_vertex_t *root = nullptr;
    bool annotated = annotate_components_from(&search, unit, &root);
    core_dict_foreach(&search.vertices, item) core_free(item->value);
    core_dict_free(&search.vertices);
    core_list_free(search.stack);
    return annotated;
}
