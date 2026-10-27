#include "compiler_tu.h"
#include "compiler_link.h"
#include "flagset_value.h"

#define NT_IS(g, n, lit) nt_is((g), (n), CORE_STRING_STATIC(lit))
#define CHILD_NT(g, n, lit) child_nt((g), (n), CORE_STRING_STATIC(lit))

typedef struct {
    parse_grammar_t *grammar;
    th_ctx *types;
    core_list_t(parse_tree_ptr_t) assignment_targets;
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

static parse_tree_t *
last_leaf(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        size_t count = core_tree_num_children(node);
        if (!count) return nullptr;
        node = core_tree_child(node, count - 1);
    }
    return node;
}

core_string_t
n00bb_node_source_text(core_translation_unit_t *unit, parse_tree_t *node,
                       core_string_t fallback)
{
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_tree_t *leaf = last_leaf(node);
    parse_token_info_t *last = leaf ? parse_node_token(leaf) : nullptr;
    if (!unit || !first || !last) return fallback;
    core_string_t contents = core_source_file_contents(&unit->source);
    size_t start = parse_token_start_byte(first);
    size_t end = parse_token_end_byte(last);
    if (end <= start || end > contents.u8_bytes) return fallback;
    return core_string_slice(contents, start, end - start);
}

static bool
untyped_module_name(n00bb_tu_state_t *state, parse_tree_t *import)
{
    parse_node_resolution_t resolution = parse_tree_resolution(import);
    parse_sym_entry_t *module = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : parse_tree_sym(import);
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
    parse_tree_t *loop = parse_tree_parent(parts);
    parse_tree_t *statement = NT_IS(grammar, loop, "range-loop")
        ? parse_tree_parent(loop) : nullptr;
    parse_tree_t *binding = CHILD_NT(grammar,
        CHILD_NT(grammar, statement, "for-range-var"), "for-binding");
    parse_tree_t *spec = CHILD_NT(grammar, binding, "type-spec");
    return spec ? parse_tree_type(spec) : nullptr;
}

static void
report_view_write(parse_grammar_t *grammar, core_translation_unit_t *unit,
                  parse_tree_t *target)
{
    parse_tree_t *root = target
        ? n00bb_addressable_root_lvalue(grammar, target) : nullptr;
    if (!root || root == n00bb_simple_lvalue(target)) return;
    parse_sym_entry_t *origin = n00bb_frozen_origin(grammar, unit, target);
    if (!origin) return;
    n00bb_tu_state_t *state = tu_state(unit);
    bool through_shared = attached_symbol(grammar, unit, root) == origin;
    for (parse_tree_t *at = target; !through_shared;) {
        parse_tree_t *member = member_access_node(grammar, at);
        parse_tree_t *step = member ? member
            : n00bb_index_expression_node(grammar, at);
        if (!step || !core_tree_num_children(step)) break;
        at = core_tree_child(step, 0);
        th_type *base = th_resolve(parse_tree_type(at));
        through_shared = base && state
            && (th_kind_of(base) == TH_INDIRECTION
                || n00bb_reference_definition(state->types, base));
    }
    if (!through_shared) return;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    bool parameter = origin->kind == PARSE_SYM_PARAM;
    if (!n00bb_report(diagnostics, unit, N00B_DIAG_WRITE_THROUGH_READ_ONLY,
            link_source_location(target),
            parameter ? N00B_TEXT_CANNOT_WRITE_THROUGH_VIEW
                      : N00B_TEXT_CANNOT_WRITE_THROUGH_CONSTANT,
            N00BB_ARGS(ARG_TEXT(origin->name))))
        return;
    n00bb_report_label(diagnostics, N00B_TEXT_WRITTEN_HERE, N00BB_NO_ARGS);
    if (origin->decl_node)
        n00bb_report_secondary(diagnostics, nullptr,
            link_source_location(origin->decl_node),
            N00B_TEXT_IS_DECLARED_HERE, N00BB_ARGS(ARG_TEXT(origin->name)));
    n00bb_report_help(diagnostics, parameter
            ? N00B_TEXT_DECLARE_AS_A_MODIFY_PARAMETER
            : N00B_TEXT_DECLARE_WITH_VAR_TO_MAKE,
        N00BB_ARGS(ARG_TEXT(origin->name)));
}

static void
report_untyped_container_literal(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *node, size_t before)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (core_diagnostics_count(diagnostics) != before) return;
    bool list = NT_IS(grammar, node, "list-lit");
    if (!n00bb_report(diagnostics, unit, N00B_DIAG_CONTAINER_LITERAL_UNTYPED,
            link_source_location(node),
            list ? N00B_TEXT_CANNOT_INFER_LIST_ITEM
                 : N00B_TEXT_CANNOT_INFER_DICT_ITEM, N00BB_NO_ARGS))
        return;
    n00bb_report_help(diagnostics, list ? N00B_TEXT_GIVE_LIST_VARIABLE_A_TYPE
                                        : N00B_TEXT_GIVE_DICT_VARIABLE_A_TYPE,
                      N00BB_NO_ARGS);
}

static void
report_modifier_without_literal(core_translation_unit_t *unit,
                                parse_tree_t *node, core_string_t modifier,
                                bool parenthesized)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (!n00bb_report(diagnostics, unit,
            N00B_DIAG_LITERAL_MODIFIER_NOT_ON_LITERAL,
            link_source_location(node),
            parenthesized ? N00B_TEXT_MODIFIER_AFTER_PARENTHESES
                          : N00B_TEXT_MODIFIER_MUST_FOLLOW_LITERAL,
            N00BB_ARGS(ARG_TEXT(modifier))))
        return;
    n00bb_report_help(diagnostics, N00B_TEXT_PUT_THE_MODIFIER_DIRECTLY_AFTER,
        N00BB_NO_ARGS);
}

static void
report_type_declaration_form(core_translation_unit_t *unit,
                             parse_tree_t *node)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_sym_entry_t *declared = parse_tree_sym(node);
    if (!declared || !declared->name.data) {
        if (n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_DECLARATION_FORM,
            link_source_location(node), N00B_TEXT_TYPE_IS_NOT_A_DECLARATION,
            N00BB_NO_ARGS))
            n00bb_report_help(diagnostics, N00B_TEXT_USE_RECORD_FOR_A_VALUE,
                N00BB_NO_ARGS);
        return;
    }
    int len = (int)declared->name.u8_bytes;
    const char *name = declared->name.data;
    if (n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_DECLARATION_FORM,
        link_source_location(node), N00B_TEXT_TYPE_IS_NOT_A_DECLARATION_2,
        N00BB_ARGS(ARG_TEXT(core_string_borrow(name, (size_t)(len))))))
        n00bb_report_help(diagnostics, N00B_TEXT_USE_RECORD_FOR_A_VALUE_2,
            N00BB_ARGS(ARG_TEXT(core_string_borrow(name, (size_t)(len))),
                       ARG_TEXT(core_string_borrow(name, (size_t)(len)))));
}

static bool
infer_literal_reporting(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *node)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    size_t before = core_diagnostics_count(diagnostics);
    if (infer_user_literal_type(grammar, unit, node, nullptr)) return true;
    if (core_diagnostics_count(diagnostics) != before) return false;
    core_string_t modifier = literal_modifier(grammar, node);
    n00bb_report(diagnostics, unit, N00B_DIAG_LITERAL_MODIFIER_PAYLOAD,
        link_source_location(node), N00B_TEXT_LITERAL_MODIFIER_DOES_NOT_ACCEPT,
        N00BB_ARGS(ARG_TEXT(modifier)));
    return false;
}

static bool
integer_copy_declaration(parse_grammar_t *grammar, parse_tree_t *node,
                         bool entering)
{
    if (entering || !NT_IS(grammar, node, "sym-decl")
        || CHILD_NT(grammar, node, "type-spec")) return false;
    parse_tree_t *initializer = CHILD_NT(grammar, node, "expression");
    return initializer && is_integer_type(parse_tree_type(initializer));
}

static void
place_record_literal(parse_grammar_t *grammar, core_translation_unit_t *unit,
                     n00bb_tu_state_t *state, parse_tree_t *expression,
                     th_type *expected,
                     const n00bb_value_destination_t *destination)
{
    if (!expression || !expected) return;
    bool placed = contextualize_record_literal(grammar, unit, state,
                                               expression, expected);
    if (!n00bb_node_record_literal_origin(expression)) return;
    th_type *actual = parse_tree_type(expression);
    th_type *target = expected;
    th_type *payload = nullptr;
    if (n00bb_option_type_element(expected, &payload) && payload)
        target = payload;
    if (placed && actual && th_unify_probe(state->types, actual, target))
        return;
    n00bb_report_value_type_mismatch(unit, expression, actual, expected,
                                     destination);
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
    if (entering && NT_IS(grammar, node, "assign-stmt")
        && core_tree_num_children(node) == 3
        && n00bb_index_expression_node(grammar,
            core_tree_child(node, 0))) {
        parse_tree_t *value = core_tree_child(node, 2);
        if (!core_tree_is_leaf(value) && !parse_tree_argument_type(value))
            parse_tree_set_argument_type(value, th_var(state->types));
    }
    if (entering && NT_IS(grammar, node, "postfix-expr")
        && core_tree_num_children(node) >= 3
        && parse_tree_leaf_is(core_tree_child(node, 1),
                              CORE_STRING_STATIC("("))
        && member_access_node(grammar, core_tree_child(node, 0))) {
        parse_tree_t *args = core_tree_child(node, 2);
        parse_tree_t *positional = CHILD_NT(grammar, args,
                                            "positional-args");
        parse_tree_t *keywords = CHILD_NT(grammar, args, "keyword-args");
        for (size_t i = 0; positional
                && i < core_tree_num_children(positional); i++) {
            parse_tree_t *arg = core_tree_child(positional, i);
            if (NT_IS(grammar, arg, "expression")
                && !parse_tree_argument_type(arg)) {
                parse_tree_set_argument_type(arg, th_var(state->types));
            }
        }
        for (size_t i = 0; keywords
                && i < core_tree_num_children(keywords); i++) {
            parse_tree_t *keyword = core_tree_child(keywords, i);
            parse_tree_t *value = CHILD_NT(grammar, keyword, "expression");
            if (value && !parse_tree_argument_type(value))
                parse_tree_set_argument_type(value, th_var(state->types));
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
    parse_token_info_t *literal_token = NT_IS(grammar, node, "simple-lit")
        ? parse_tree_find_first_terminal(node) : nullptr;
    core_string_t literal_terminal = literal_token
        ? parse_terminal_name(grammar, parse_token_tid(literal_token))
        : (core_string_t){0};
    bool modified_numeric = literal_token
        && core_option_is_set(parse_token_postfix_modifier(literal_token))
        && (core_string_eq(literal_terminal, CORE_STRING_STATIC("INT_LIT"))
            || core_string_eq(literal_terminal, CORE_STRING_STATIC("HEX_LIT"))
            || core_string_eq(literal_terminal,
                              CORE_STRING_STATIC("FLOAT_LIT"))
            || core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("true"))
            || core_string_eq(first_token_text(node),
                              CORE_STRING_STATIC("false"))
            || n00bb_nil_spelling(first_token_text(node)));
    operator_site.dispatch_trait = operator_trait;
    operator_site.dispatch_method = operator_method;
    parse_tree_t *numeric_receiver = intrinsic_binary
        ? operator_receiver_node(&operator_site) : nullptr;
    bool numeric_comparison = numeric_receiver
        && !core_tree_is_leaf(numeric_receiver)
        && (core_string_eq(operator_trait, CORE_STRING_STATIC("Equal"))
            || core_string_eq(operator_trait,
                              CORE_STRING_STATIC("Comparable")))
        && is_integer_type(parse_tree_type(numeric_receiver));
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
    th_type *compound_target = !entering
        && NT_IS(grammar, node, "binop-assign-stmt")
        && core_tree_num_children(node) == 3
        ? parse_tree_type(core_tree_child(node, 0)) : nullptr;
    bool integer_compound = compound_target
        && (is_integer_type(compound_target)
            || n00bb_open_integer(compound_target));
    bool integer_copy = integer_copy_declaration(grammar, node, entering);
    bool path_root_past_fields = resolve_type_path_root_past_fields(grammar,
        unit, node, parent);
    if (variant_payload || flagset_field || type_reference
        || path_root_past_fields
        || invalid_field_access || intrinsic_binary || intrinsic_unary
        || integer_compound || integer_copy
        || (modified_numeric && n00bb_nil_spelling(first_token_text(node))))
        (void)parse_visit_suppress_annotation(visit, PARSE_ANNOT_INFER);
    if (modified_numeric
        || core_string_eq(literal_terminal, CORE_STRING_STATIC("INT_LIT"))
        || core_string_eq(literal_terminal, CORE_STRING_STATIC("HEX_LIT")))
        (void)parse_visit_suppress_annotation(visit, PARSE_ANNOT_LITERAL);
    if (type_reference || path_root_past_fields)
        (void)parse_visit_suppress_annotation(visit, PARSE_ANNOT_VARREF);
    if (step == SEMANTIC_PREPARE_EXIT && NT_IS(grammar, node, "tspec-tvar")
        && !signature_typevar_for_node(grammar, unit, node)) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        core_string_t spelling = n00bb_node_source_text(unit, node,
            n00bb_text_form(N00B_TEXT_THIS_TYPE_VARIABLE));
        if (n00bb_report(diagnostics, unit,
            N00B_DIAG_TYPE_VARIABLE_OUTSIDE_SIGNATURE,
            link_source_location(node), N00B_TEXT_TYPE_VARIABLE_DOES_NOT_BELONG,
            N00BB_ARGS(ARG_TEXT(spelling))))
            n00bb_report_help(diagnostics,
                N00B_TEXT_TYPE_VARIABLES_ARE_DECLARED_BY,
                N00BB_NO_ARGS);
    }
    return;
    }
    bool type_reference = cache && cache->type_reference;
    bool modified_numeric = cache && cache->modified_numeric;
    parse_token_info_t *literal_token = cache ? cache->literal_token : nullptr;
    core_string_t literal_terminal = cache
        ? cache->literal_terminal : (core_string_t){0};
    if (entering) {
        if (NT_IS(grammar, node, "postfix-expr"))
            parse_tree_set_index_address_access(node,
                n00bb_index_reference_storage(grammar, node));
        if (field_record_declaration(grammar, node)) {
            parse_sym_entry_t *symbol = parse_tree_sym(node);
            if (!symbol || symbol->kind != PARSE_SYM_TYPEDEF) {
                n00bb_tu_internal_error(unit, node, "checking declarations",
                    "record declaration has no type symbol");
                return;
            }
            th_type *named = symbol->type ? th_resolve(symbol->type)
                                          : nullptr;
            if (!named || th_kind_of(named) != TH_NAMED) {
                n00bb_tu_internal_error(unit, node, "checking declarations",
                    "record type name conflicts with its symbol");
                return;
            }
            parse_tree_set_type(node, named);
        }
    }
    else {
        if (step == SEMANTIC_AFTER_EXIT) {
            n00bb_defer_overloaded_callee(grammar, unit, node);
            n00bb_check_clobbered_import(grammar, unit, node);
        }
        if (step == SEMANTIC_AFTER_REFERENCE_EXIT) {
        if (NT_IS(grammar, node, "tuple-or-paren")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *inner = core_tree_child(node, 1);
            parse_sym_entry_t *symbol = attached_symbol(grammar, unit, inner);
            if (symbol && !parse_tree_set_resolution_symbol(node, symbol)) {
                n00bb_tu_internal_error(unit, node, "resolving names",
                    "could not retain parenthesized symbol resolution");
                return;
            }
        }
        if (!apply_typecase_narrowing(grammar, node)) {
            n00bb_tu_internal_error(unit, node, "checking types",
                "could not bind a reference to its narrowed typecase subject");
            return;
        }
        n00bb_apply_atomic_load(grammar, unit, node);
        if (!bind_builtin_reference(grammar, state, node)) {
            n00bb_tu_internal_error(unit, node, "resolving names",
                "could not bind the builtin reference");
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
                parse_tree_set_type(receiver, element);
                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);
                if (n00bb_report(diagnostics, unit,
                    N00B_DIAG_ATOMIC_FIELD_UPDATE, link_source_location(node),
                    N00B_TEXT_IS_ATOMIC_SO_ITS_FIELDS,
                    N00BB_ARGS(ARG_TEXT(atomic_root->name)))) {
                    if (atomic_root->decl_node)
                        n00bb_report_secondary(diagnostics, nullptr,
                            link_source_location(atomic_root->decl_node),
                            N00B_TEXT_IS_DECLARED_HERE,
                            N00BB_ARGS(ARG_TEXT(atomic_root->name)));
                    n00bb_report_help(diagnostics,
                        N00B_TEXT_BUILD_THE_UPDATED_VALUE_AND,
                        N00BB_ARGS(ARG_TEXT(atomic_root->name)));
                }
            }
        }
        bind_declared_type_from_node(grammar, state->types, node);
        if (!n00bb_cache_contextual_integer(grammar, state, node)) {
            n00bb_tu_internal_error(unit, node, "checking types",
                "could not constrain integer literal");
            return;
        }
        if (NT_IS(grammar, node, "simple-lit")) {
            th_type *expected = n00bb_integer_literal_context(grammar,
                state, node);
            th_type *element = nullptr;
            if (n00bb_option_type_element(expected, &element))
                expected = element;
            if (expected && is_float_type(expected))
                n00bb_contextual_float_literal(node, expected);
        }
        if (NT_IS(grammar, node, "range-loop")) {
            parse_tree_t *parts = CHILD_NT(grammar, node, "range-parts");
            parse_tree_t *step = CHILD_NT(grammar,
                CHILD_NT(grammar, node, "range-step"), "expression");
            th_type *bound = parts ? parse_tree_type(parts) : nullptr;
            if (step && bound && (is_integer_type(bound)
                                  || n00bb_open_integer(bound)))
                (void)n00bb_integer_literal_joins_type(state, step, bound);
        }
        th_type *declared = NT_IS(grammar, node, "range-parts")
            ? range_binding_type(grammar, node) : nullptr;
        if (declared && is_integer_type(th_resolve(declared))
            && core_tree_num_children(node) == 3) {
            (void)n00bb_integer_literal_joins_type(state,
                core_tree_child(node, 0), declared);
            (void)n00bb_integer_literal_joins_type(state,
                core_tree_child(node, 2), declared);
        }
        if (NT_IS(grammar, node, "range-parts")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *start = core_tree_child(node, 0);
            parse_tree_t *bound = core_tree_child(node, 2);
            th_type *start_type = parse_tree_type(start);
            th_type *bound_type = parse_tree_type(bound);
            if (bound_type && th_kind_of(th_resolve(bound_type)) == TH_VAR)
                (void)n00bb_literal_adopts_open_type(grammar, state, start,
                                                     bound_type);
            else if (start_type
                     && th_kind_of(th_resolve(start_type)) == TH_VAR)
                (void)n00bb_literal_adopts_open_type(grammar, state, bound,
                                                     start_type);
            th_type *parts = n00bb_open_integer(start_type) ? start_type
                : n00bb_open_integer(bound_type) ? bound_type : nullptr;
            if (parts) {
                n00bb_integer_term_t term = {N00BB_INTEGER_TERM_TYPE, parts,
                                             nullptr};
                n00bb_integer_copy(state, node, term, start);
                n00bb_integer_copy(state, node, term, bound);
            }
        }
        if (core_tree_num_children(node) == 3
            && (NT_IS(grammar, node, "range-parts")
                || (NT_IS(grammar, node, "compare-expr")
                    && NT_IS(grammar, core_tree_child(node, 1),
                             "compare-op"))
                || (NT_IS(grammar, node, "equality-expr")
                    && NT_IS(grammar, core_tree_child(node, 1),
                             "equality-op")))) {
            parse_tree_t *left = core_tree_child(node, 0);
            parse_tree_t *right = core_tree_child(node, 2);
            th_type *left_type = parse_tree_type(left);
            th_type *right_type = parse_tree_type(right);
            bool integer_comparison = !NT_IS(grammar, node, "range-parts")
                && (is_integer_type(left_type) || n00bb_open_integer(left_type))
                && (is_integer_type(right_type)
                    || n00bb_open_integer(right_type));
            if (left_type && right_type && left_type != right_type
                && !integer_comparison
                && !n00bb_integer_literal_joins_type(state, right,
                                                     left_type)
                && !n00bb_float_literal_adopts(right, left_type)
                && !n00bb_integer_literal_joins_type(state, left, right_type))
                n00bb_float_literal_adopts(left, right_type);
            parse_tree_t *sides[] = {left, right};
            th_type *declared[] = {right_type, left_type};
            for (size_t s = 0; integer_comparison && s < 2; s++) {
                ir_interval_t range = {0}, bounds = {0};
                if (parse_tree_integer_literal_origin(sides[s])
                    && is_integer_type(declared[s])
                    && !n00bb_open_integer(declared[s])
                    && n00bb_current_integer_range(sides[s], &range)
                    && n00bb_type_interval(declared[s], &bounds)
                    && !ir_interval_meet(range, bounds, nullptr)) {
                    n00bb_report_integer_misfit(state, node,
                        false, range, declared[s]);
                    break;
                }
            }
        }
        if (NT_IS(grammar, node, "sym-decl")) {
            parse_tree_t *type_spec = CHILD_NT(grammar, node, "type-spec");
            parse_tree_t *initializer = CHILD_NT(grammar, node, "expression");
            parse_sym_entry_t *declared = parse_tree_sym(node);
            n00bb_value_destination_t variable = {
                .kind = N00BB_DESTINATION_VARIABLE,
                .name = declared ? declared->name : (core_string_t){0},
                .declared_at = type_spec,
            };
            place_record_literal(grammar, unit, state, initializer,
                type_spec ? parse_tree_type(type_spec) : nullptr, &variable);
            if (declared && !type_spec && initializer
                && is_integer_type(parse_tree_type(initializer))) {
                th_type *class = n00bb_integer_variable(state, 0);
                if (class && !declared->type) declared->type = class;
                if (class && th_unify_in_place(state->types, declared->type,
                                               class))
                    n00bb_integer_copy(state, node,
                        (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE,
                                               class, nullptr},
                        initializer);
            }
            if (type_spec && initializer && declared
                && is_integer_type(declared->type)
                && scope_is_within_function(declared->scope)) {
                core_dict_put(&state->integer_tracked_symbols, declared,
                              declared);
                n00bb_integer_copy(state, node,
                    (n00bb_integer_term_t){N00BB_INTEGER_TERM_SYMBOL,
                                           declared->type, declared},
                    initializer);
            }
        }
        if (NT_IS(grammar, node, "default-param")) {
            parse_tree_t *type_spec = CHILD_NT(grammar, node, "type-spec");
            parse_sym_entry_t *parameter = parse_tree_sym(node);
            n00bb_value_destination_t variable = {
                .kind = N00BB_DESTINATION_VARIABLE,
                .name = parameter ? parameter->name : (core_string_t){0},
                .declared_at = type_spec,
            };
            if (type_spec)
                place_record_literal(grammar, unit, state,
                    CHILD_NT(grammar, node, "expression"),
                    parse_tree_type(type_spec), &variable);
        }
        if (NT_IS(grammar, node, "assign-stmt")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *value = core_tree_child(node, 2);
            th_type *expected = record_literal_expected_type(grammar,
                state->types, value);
            n00bb_value_destination_t target = {
                .kind = N00BB_DESTINATION_EXPECTED,
            };
            place_record_literal(grammar, unit, state, value,
                expected ? expected
                         : parse_tree_type(core_tree_child(node, 0)),
                &target);
        }
        if ((NT_IS(grammar, node, "assign-stmt")
             || NT_IS(grammar, node, "binop-assign-stmt"))
            && core_tree_num_children(node) == 3)
            core_list_push(annotation_context->assignment_targets,
                           core_tree_child(node, 0));
        if (NT_IS(grammar, node, "return-stmt")) {
            parse_tree_t *function = node;
            while (function && !NT_IS(grammar, function, "func-def"))
                function = parse_tree_parent(function);
            parse_sym_entry_t *symbol = function
                ? parse_tree_sym(function) : nullptr;
            th_type *declared = symbol ? parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.declared-function-signature"))
                : nullptr;
            if (declared)
                contextualize_record_literal(grammar, unit, state,
                    CHILD_NT(grammar, node, "expression"),
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
                    n00bb_tu_internal_error(unit, node, "checking types",
                        "could not retain record literal origin");
                    return;
                }
                facts->record_literal_origin = origin;
            }
        }
        if (NT_IS(grammar, node, "binop-assign-stmt")
            && core_tree_num_children(node) == 3) {
            const n00bb_binary_operator_t *operator =
                n00bb_binary_operator_assigned(
                    first_token_text(core_tree_child(node, 1)));
            th_type *target_type = parse_tree_type(core_tree_child(node, 0));
            th_type *value_type = parse_tree_type(core_tree_child(node, 2));
            bool open_target = n00bb_open_integer(target_type);
            bool unresolved_value = value_type
                && th_kind_of(th_resolve(value_type)) == TH_VAR
                && !n00bb_open_integer(value_type);
            if (operator && !operator->shift && value_type
                && !n00bb_open_integer(value_type)
                && (open_target || is_integer_type(target_type))) {
                if (unresolved_value)
                    core_list_push(state->deferred_conversions,
                        ((n00bb_deferred_conversion_t){
                            core_tree_child(node, 2), value_type,
                            target_type}));
                else (void)th_converts(state->types, value_type, target_type,
                                       core_tree_child(node, 2), grammar);
            }
            if (operator && operator->unsigned_only && open_target)
                (void)n00bb_require_unsigned_integer(state, target_type);
            if (operator && !operator->shift
                && (open_target || is_integer_type(target_type)))
                n00bb_integer_growth(state, node, operator->op,
                                     core_tree_child(node, 0),
                                     core_tree_child(node, 2));
        }
        if (NT_IS(grammar, node, "assign-stmt")
            && core_tree_num_children(node) == 3) {
            parse_tree_t *target = core_tree_child(node, 0);
            parse_tree_t *value = core_tree_child(node, 2);
            n00bb_integer_literal_joins_type(state, value,
                parse_tree_type(target));
            if (n00bb_open_integer(parse_tree_type(value))
                && n00bb_open_integer(parse_tree_type(target)))
                (void)th_unify_in_place(state->types,
                    parse_tree_type(value), parse_tree_type(target));
            n00bb_integer_term_t written = n00bb_integer_term_of(state,
                                                                 target);
            if (written.kind == N00BB_INTEGER_TERM_SYMBOL)
                n00bb_integer_assign(state, node, written, target, value);
            th_type *target_type = parse_tree_type(target)
                ? th_resolve(parse_tree_type(target)) : nullptr;
            th_type *value_type = parse_tree_type(value)
                ? th_resolve(parse_tree_type(value)) : nullptr;
            bool open_integer = n00bb_open_integer(value_type);
            if (target_type && value_type
                && th_kind_of(target_type) == TH_VAR
                && th_var_name(target_type).data
                && (open_integer
                    || (th_kind_of(value_type) != TH_VAR
                        && th_is_closed(state->types, value_type)))) {
                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);
                core_string_t variable = n00bb_print_type_brief(state->types,
                                                                target_type);
                core_string_t value_text = n00bb_node_source_text(unit,
                    value, n00bb_text_form(N00B_TEXT_THIS_VALUE));
                core_string_t target_text = n00bb_node_source_text(unit,
                    target, n00bb_text_form(N00B_TEXT_THIS_TARGET));
                if (n00bb_report(diagnostics, unit,
                    N00B_DIAG_ASSIGN_TO_TYPE_PARAMETER,
                    link_source_location(value),
                    N00B_TEXT_CANNOT_ASSIGN_TO_ITS_TYPE,
                    N00BB_ARGS(ARG_TEXT(value_text), ARG_TEXT(target_text),
                               ARG_TEXT(variable)))) {
                    if (open_integer)
                        n00bb_report_label(diagnostics, N00B_TEXT_AN_INTEGER,
                                           N00BB_NO_ARGS);
                    else
                        n00bb_report_label(diagnostics, N00B_TEXT_QUOTED_NAME,
                            N00BB_ARGS(ARG_TEXT(n00bb_print_type(
                                state->types, value_type))));
                    n00bb_report_help(diagnostics,
                        N00B_TEXT_ONLY_A_VALUE_OF_TYPE,
                        N00BB_ARGS(ARG_TEXT(variable), ARG_TEXT(target_text)));
                }
            }
        }
        cache_wrapped_symbol_resolution(grammar, unit, node);
        if (NT_IS(grammar, node, "unary-expr")
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
        if (NT_IS(grammar, node, "alias-stmt")
            && alias_declaration_is_transparent(grammar, node)) {
            parse_sym_entry_t *alias = parse_tree_sym(node);
            if (alias && !alias->type) alias->type = th_var(state->types);
            th_type *target = alias
                ? declared_type_from_node(grammar, unit, node) : nullptr;
            if (target && th_is_closed(state->types, th_resolve(target))
                && th_unify_probe(state->types, alias->type, target))
                alias->type = th_unify_in_place(state->types, alias->type,
                                                target);
            if (alias) parse_tree_set_type(node, alias->type);
        }
        else if (NT_IS(grammar, node, "alias-stmt")) {
            parse_sym_entry_t *named = parse_tree_sym(node);
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
                    : qualified.data
                        ? th_name(state->types, core_buffer_string(&qualified))
                        : nullptr;
                if (type) th_type_define(state->types, type, representation);
                core_buffer_release(&qualified);
            }
        }
        if (NT_IS(grammar, node, "module-import")
            && !untyped_module_name(state, node)) {
            n00bb_tu_internal_error(unit, node, "checking types",
                "could not keep the imported module name out of value "
                "inference");
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
                n00bb_tu_internal_error(unit, node, "resolving names",
                    "could not hold the imported type reference until its "
                    "module is typed");
                return;
            }
            if (typed) {
                parse_tree_set_type(node, resolution.target.symbol->type);
                if (!parse_sym_add_use(resolution.target.symbol, node))
                    n00bb_tu_internal_error(unit, node, "resolving names",
                        "could not bind the imported type reference node");
                else if (!bind_array_shape_alias_source(grammar, node,
                        resolution.target.symbol))
                    n00bb_tu_internal_error(unit, node, "resolving names",
                        "could not bind the imported alias shape source");
            }
        }
        resolve_local_type_reference(grammar, unit, node, parent);
        if (type_reference) {
            parse_node_resolution_t resolution = parse_tree_resolution(node);
            if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                && parse_tree_type(node))
                propagate_resolved_type_reference(grammar, unit, node,
                    parse_tree_type(node));
        }
    }
    if (!entering && NT_IS(grammar, node, "enum-item")
        && !cache_enum_item_value(grammar, state, node)) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        parse_tree_t *value = CHILD_NT(grammar,
            CHILD_NT(grammar, node, "enum-value"), "simple-lit");
        parse_sym_entry_t *item = parse_tree_sym(node);
        core_string_t name = item ? item->name : CORE_STRING_STATIC("item");
        core_string_t value_text = n00bb_node_source_text(unit, value,
            n00bb_text_form(N00B_TEXT_THIS_VALUE));
        if (n00bb_report(diagnostics, unit, N00B_DIAG_ENUM_VALUE_NOT_INTEGER,
            link_source_location(value ? value : node),
            N00B_TEXT_THE_VALUE_OF_ENUM_ITEM,
            N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(value_text))))
            n00bb_report_help(diagnostics,
                N00B_TEXT_WRITE_A_DECIMAL_OR_HEXADECIMAL,
                N00BB_ARGS(ARG_TEXT(name)));
        return;
    }
    if (!entering && state->flagset_scope
        && !cache_flagset_semantics(grammar, unit, state, node)) {
        parse_tree_t *declaration = state->flagset_scope;
        parse_sym_entry_t *owner = declaration
            ? parse_tree_sym(declaration) : nullptr;
        if (n00bb_flagset_first_problem(owner))
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking declarations",
                "could not compute the value of a flagset member");
        return;
    }
    if (!entering && NT_IS(grammar, node, "simple-lit")
        && !modified_numeric
        && !refine_integer_literal_type(grammar, unit, node, state))
        return;
    if (!entering && !cache_ordered_array_dimensions(grammar, state, node)) {
        n00bb_tu_internal_error(unit, node, "checking types",
            "could not cache array type dimensions");
        return;
    }
    if (!entering && !cache_typecase_arm_types(grammar, state, node)) {
        n00bb_tu_internal_error(unit, node, "checking types",
            "could not cache typecase arm types");
        return;
    }
    if (entering
        && (NT_IS(grammar, node, "body")
            || NT_IS(grammar, node, "value-body")) && parent
        && (NT_IS(grammar, parent, "typecase-case-block")
            || NT_IS(grammar, parent, "typecase-value-case-block")))
        prepare_typecase_arm(grammar, unit, parent);
    if (!entering) {
        cache_type_syntax_node(grammar, node);
        th_type *composed = compose_type_syntax_node(grammar, unit, node);
        if (composed) parse_tree_set_type(node, composed);
        else if (NT_IS(grammar, node, "tspec-application"))
            parse_tree_set_type(node, nullptr);
        if (NT_IS(grammar, node, "variant-decl-one")) {
            parse_sym_entry_t *arm = parse_tree_sym(node);
            parse_tree_t *type_node = CHILD_NT(grammar, node, "type-spec");
            th_type *type = type_node ? parse_tree_type(type_node) : nullptr;
            if (!arm || !type) {
                n00bb_tu_internal_error_unless_reported(unit, node,
                    "checking declarations",
                    "variant arm has no payload type");
                return;
            }
            if (arm->type) {
                type = th_unify_in_place(state->types, arm->type, type);
                if (!type) {
                    n00bb_tu_internal_error_unless_reported(unit, node,
                        "checking declarations",
                        "variant arm payload conflicts with its cached type");
                    return;
                }
            }
            arm->type = type;
            parse_tree_set_type(node, type);
        }
    }
    if (!entering && (NT_IS(grammar, node, "simple-lit")
        || NT_IS(grammar, node, "list-lit")
        || NT_IS(grammar, node, "dict-or-set-lit")
        || NT_IS(grammar, node, "embed-lit"))) {
        core_string_t modifier = literal_modifier(grammar, node);
        bool string_literal = NT_IS(grammar, node, "simple-lit")
            && parse_tree_find_first_terminal(node)
            && core_string_eq(parse_terminal_name(grammar,
                parse_token_tid(parse_tree_find_first_terminal(node))),
                CORE_STRING_STATIC("STRING_LIT"));
        if (!modifier.data) {
            th_type *default_type = nullptr;
            if (string_literal)
                default_type = th_name(state->types,
                                       CORE_STRING_STATIC("string"));
            else if (NT_IS(grammar, node, "list-lit")
                     || NT_IS(grammar, node, "dict-or-set-lit")) {
                size_t before = core_diagnostics_count(
                    core_translation_unit_diagnostics(unit));
                if (!n00bb_type_shape_literal(grammar, unit, node)) {
                    report_untyped_container_literal(grammar, unit, node,
                                                     before);
                    return;
                }
            }
            if (default_type) {
                th_type *literal_type = parse_tree_type(node);
                parse_tree_set_type(node, literal_type
                    ? th_unify_in_place(state->types, literal_type,
                                         default_type) : default_type);
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
                    || n00bb_nil_spelling(first_token_text(node)));
            if (NT_IS(grammar, node, "simple-lit")
                && !string_literal && !numeric_literal) {
                return;
            }
            bool sequence = NT_IS(grammar, node, "list-lit");
            bool braces = NT_IS(grammar, node, "dict-or-set-lit");
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
                    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                        N00B_DIAG_LITERAL_MODIFIER_PAYLOAD,
                        link_source_location(node),
                        N00B_TEXT_LITERAL_MODIFIER_DOES_NOT_ACCEPT,
                        N00BB_ARGS(ARG_TEXT(modifier)));
                    return;
                }
            }
            else if (!infer_literal_reporting(grammar, unit, node))
                return;
        }
    }
    if (!entering && NT_IS(grammar, node, "new-expr")) {
        th_type *expected = parse_tree_type(node);
        if (!infer_new_expression_type(grammar, node, state)) {
            parse_tree_t *type_node = CHILD_NT(grammar, node, "type-spec");
            th_type *created = type_node ? parse_tree_type(type_node)
                                         : nullptr;
            if (created && expected)
                n00bb_report_value_type_mismatch(unit, node, created,
                                                 expected, nullptr);
            else
                n00bb_tu_internal_error_unless_reported(unit, node,
                    "checking types", "`new` expression has no type");
            return;
        }
    }
    if (!entering && NT_IS(grammar, node, "callback-expr")
        && !infer_callback_expression_type(grammar, unit, node, state))
        return;
    if (!entering && (NT_IS(grammar, node, "pos-param")
        || NT_IS(grammar, node, "default-param")
        || NT_IS(grammar, node, "vargs-param"))) {
        bind_declared_parameter(grammar, unit, node, state);
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        if (symbol) validate_builtin_generic_type_arity(grammar, node, unit,
                                                       symbol->type);
        if (!record_function_parameter_fact(grammar, node, state)) {
            n00bb_tu_internal_error(unit, node, "checking declarations",
                "could not retain the function parameter type fact");
            return;
        }
    }
    if (entering && (NT_IS(grammar, node, "func-def")
                     || NT_IS(grammar, node, "callback-role")
                     || NT_IS(grammar, node, "trait-spec")
                     || NT_IS(grammar, node, "context-def"))) {
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        if (symbol) {
            n00bb_function_parameter_facts_t *parameter_facts =
                core_arena_alloc(&state->semantic_values,
                                 sizeof(*parameter_facts));
            if (!parameter_facts
                || !parse_sym_set_binding(symbol, N00BB_PARAMETER_FACTS_KEY,
                                          parameter_facts)) {
                n00bb_tu_internal_error(unit, node, "checking declarations",
                    "could not initialize parameter facts");
                return;
            }
        }
    }

    if (!entering && NT_IS(grammar, node, "trait-spec")) {
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        if (symbol) {
            th_type *failure = nullptr;
            th_type *signature = explicit_trait_method_signature(
                grammar, unit, node, state->types, &failure);
            if (signature) {
                symbol->type = signature;
                symbol->error_type = failure
                    ? failure
                    : th_name(state->types, CORE_STRING_STATIC("nil"));
                th_type *contract = th_env_signature_contract(
                    state->types, symbol->type, symbol->error_type);
                symbol->callable_contract = contract;
            }
        }
    }
    if (!entering && NT_IS(grammar, node, "trait-decl")) {
        parse_sym_entry_t *symbol = parse_tree_sym(node);
        core_list_t(n00bb_typevar_binding_t) variables =
            core_list_new(n00bb_typevar_binding_t);
        core_list_t(th_trait_requirement) declared =
            core_list_new(th_trait_requirement);
        signature_typevars_for_symbol(symbol, &variables);
        bool valid = apply_signature_constraints(grammar, unit,
            CHILD_NT(grammar, node, "where-clause"), state->types,
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
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking declarations",
                "could not apply the trait's where clause");
            return;
        }
    }

    if (!entering && NT_IS(grammar, node, "callback-role"))
        record_callback_role_signature(grammar, unit, node, state);
    if (!entering && NT_IS(grammar, node, "callback-decl"))
        finalize_callback_contract(grammar, unit, node, state);

    if (!entering && NT_IS(grammar, node, "func-def"))
        record_function_signature(grammar, unit, node, state, true);
    if (entering && NT_IS(grammar, node, "context-enter"))
        record_context_hook_signature(grammar, unit, node, state);

    if (!entering) {
        if (!cache_ordered_literal_children(grammar, state, node)
            || !cache_static_literal_node(grammar, state, node)) {
            n00bb_tu_internal_error(unit, node, "checking types",
                "could not cache ordered literal child references");
            return;
        }
        if (NT_IS(grammar, node, "top-level-stmt")) {
            parse_tree_t *statement = top_level_statement_child(grammar, node);
            if (!statement) {
                n00bb_tu_internal_error(unit, node, "checking declarations",
                    "could not cache a top-level statement node");
                return;
            }
            core_list_push(state->module_top_level_statements, statement);
        }
        if (NT_IS(grammar, node, "sym-decl")) {
            parse_tree_t *type_spec = CHILD_NT(grammar, node, "type-spec");
            parse_sym_entry_t *symbol = parse_tree_sym(node);
            parse_tree_t *initializer = CHILD_NT(grammar, node, "expression");
            if (symbol && initializer
                && !parse_sym_set_binding(symbol,
                    CORE_STRING_STATIC("n00bb.initializer-expression"),
                    initializer)) {
                n00bb_tu_internal_error(unit, node, "checking declarations",
                    "could not retain symbol initializer");
                return;
            }
            th_type *declared = type_spec
                ? declared_type_from_node(grammar, unit, type_spec) : nullptr;
            if (symbol && declared) {
                th_type *unified = unify_declared_symbol_type(state->types,
                    symbol, declared);
                symbol->type = unified ? unified : declared;
                parse_tree_set_type(node, symbol->type);
                parse_tree_set_type(type_spec, declared);
                validate_builtin_generic_type_arity(grammar, type_spec, unit,
                                                    declared);
            }
        }
        if (NT_IS(grammar, node, "variable-decl")) {
            parse_tree_t *decl_list = CHILD_NT(grammar, node,
                                               "sym-decl-list");
            for (size_t i = 0; decl_list
                    && i < core_tree_num_children(decl_list); i++) {
                parse_tree_t *declaration = core_tree_child(decl_list, i);
                parse_sym_entry_t *symbol = parse_tree_sym(declaration);
                n00bb_static_value_t value;
                if (!symbol || !semantic_static_literal_value(
                        grammar, declaration, state, &value)) continue;
                n00bb_static_value_t *stored = core_arena_alloc(
                    &state->semantic_values, sizeof(*stored));
                if (!stored) {
                    n00bb_tu_internal_error(unit, declaration,
                        "checking declarations",
                        "could not retain a semantic static value");
                    return;
                }
                *stored = value;
                symbol->comptime_value = stored;
                symbol->has_comptime_value = true;
            }
        }
        if (NT_IS(grammar, node, "tuple-or-paren")
            && literal_modifier(grammar, node).data) {
            bool shape = CHILD_NT(grammar, node, "named-field-list")
                || core_tree_num_children(node) == 5;
            if (shape)
                (void)infer_literal_reporting(grammar, unit, node);
            else
                report_modifier_without_literal(unit, node,
                    literal_modifier(grammar, node), true);
        }
        else if (NT_IS(grammar, node, "tuple-or-paren")
            && CHILD_NT(grammar, node, "named-field-list")
            && (!contextualize_record_fields_before_inference(grammar,
                    unit, state, node)
                || !infer_anonymous_record(grammar, node, state))) {
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking types",
                "could not construct the anonymous record type");
            return;
        }
        if (NT_IS(grammar, node, "tuple-or-paren")
            && !literal_modifier(grammar, node).data
            && !CHILD_NT(grammar, node, "named-field-list")
            && !infer_tuple_literal_type(grammar, node, state)) {
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking types", "could not infer the tuple literal type");
            return;
        }
        if (NT_IS(grammar, node, "type-decl")) {
            report_type_declaration_form(unit, node);
            return;
        }
        if (field_record_declaration(grammar, node)) {
            if (!cache_record_layout_request(grammar, unit, node)) {
                n00bb_tu_internal_error_unless_reported(unit, node,
                    "checking declarations",
                    "could not retain the record layout request");
                return;
            }
            if (!cache_record_field_extents(grammar, unit, node)) {
                n00bb_tu_internal_error_unless_reported(unit, node,
                    "checking declarations",
                    "could not retain the record field extents");
                return;
            }
            if (!register_record_semantic_shape(grammar, unit, node)) {
                n00bb_tu_internal_error_unless_reported(unit, node,
                    "checking declarations",
                    "could not register the record shape");
                return;
            }
        }
        if (NT_IS(grammar, node, "variant-decl")
            && !finalize_variant_symbol_type(grammar, node, unit, state)) {
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking declarations",
                "could not resolve the declared variant type");
            return;
        }
        if (NT_IS(grammar, node, "enum-stmt")
            && !finalize_enum_symbol_type(grammar, node, unit, state)) {
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking declarations",
                "could not resolve the declared enum type");
            return;
        }
        if (NT_IS(grammar, node, "flagset-stmt")
            && !finalize_flagset_symbol_type(grammar, node, unit, state)) {
            n00bb_tu_internal_error_unless_reported(unit, node,
                "checking declarations",
                "could not resolve the declared flagset type");
            return;
        }
        remember_link_use_site(grammar, unit, state, node);
        if (!record_generic_operator_failure(grammar, state, node)) {
            n00bb_tu_internal_error(unit, node, "checking types",
                "could not retain operator failure constraints");
            return;
        }
        if (!cache_return_source_fact(grammar, node, state)) {
            n00bb_tu_internal_error(unit, node, "checking types",
                "could not retain return-source facts");
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
                        parse_tree_set_type(symbol->decl_node, field_type);
                    }
                }
                if (!symbol->type && symbol->kind != PARSE_SYM_MODULE) {
                    parse_sym_mark_type_inferred(symbol);
                }
            }
        }
    }
    if (!entering) n00bb_range_after_children(grammar, state, node);
    if (!entering && NT_IS(grammar, node, "expression")) {
        th_type *argument = parse_tree_argument_type(node);
        if (argument) {
            ir_interval_t range = {0};
            th_type *source = parse_tree_type(node);
            bool integer = source && is_integer_type(source);
            bool ranged = integer && n00bb_current_integer_range(node,
                                                                 &range);
            if (!(integer
                    ? n00bb_constrain_integer_fit(state, argument, source,
                        ranged, range)
                    : source && th_unify_in_place(state->types, argument,
                                                   source))) {
                parse_tree_t *statement = parse_tree_parent(node);
                parse_tree_t *target = statement
                    && core_tree_num_children(statement) == 3
                    ? core_tree_child(statement, 0) : nullptr;
                parse_tree_t *indexing = target
                    ? n00bb_index_expression_node(grammar, target) : nullptr;
                parse_tree_t *container = indexing
                    && core_tree_num_children(indexing)
                    ? core_tree_child(indexing, 0) : nullptr;
                n00bb_value_destination_t destination = {
                    .kind = N00BB_DESTINATION_ELEMENT,
                    .name = container ? n00bb_value_spelling(unit, container)
                                      : (core_string_t){0},
                };
                n00bb_report_value_type_mismatch(unit, node, source,
                                                 argument, &destination);
                return;
            }
        }
    }
    if (!entering) n00bb_range_after_annotation(grammar, state, node, parent);
}

typedef struct {
    th_type *expected;
    semantic_annotation_cache_t annotation_cache;
    bool in_expression;
} semantic_walk_frame_t;
core_list_decl(semantic_walk_frame_t);

typedef struct {
    annotation_context_t *context;
    core_translation_unit_t *unit;
    core_list_t(semantic_walk_frame_t) frames;
    bool skip_typed_ahead;
    size_t errors_seen;
    parse_tree_t *failed_statement;
} semantic_walk_context_t;

static bool
type_declaration_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return field_record_declaration(grammar, node)
        || NT_IS(grammar, node, "variant-decl")
        || NT_IS(grammar, node, "enum-stmt")
        || NT_IS(grammar, node, "flagset-stmt")
        || NT_IS(grammar, node, "alias-stmt");
}

static bool
executable_scope_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const core_string_t executable[] = {
        CORE_STRING_STATIC("block"), CORE_STRING_STATIC("if"),
        CORE_STRING_STATIC("for"), CORE_STRING_STATIC("switch"),
        CORE_STRING_STATIC("try"), CORE_STRING_STATIC("try-error"),
        CORE_STRING_STATIC("typecase"), CORE_STRING_STATIC("while"),
        CORE_STRING_STATIC("with"),
    };
    const parse_rule_t *rule = node && !core_tree_is_leaf(node)
        ? parse_tree_rule(grammar, node) : nullptr;
    parse_annotation_t *scope = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_SCOPE_OPEN)
        : nullptr;
    for (size_t i = 0; scope
         && i < sizeof(executable) / sizeof(executable[0]); i++)
        if (core_string_eq(scope->scope_tag, executable[i]))
            return true;
    return false;
}

static size_t
unit_error_count(core_translation_unit_t *unit)
{
    const core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(unit);
    return core_diagnostics_count_severity(diagnostics, CORE_DIAG_ERROR)
        + core_diagnostics_count_severity(diagnostics, CORE_DIAG_FATAL)
        + core_diagnostics_count_severity(diagnostics, CORE_DIAG_INTERNAL);
}

static bool
declared_within(parse_sym_entry_t *symbol, parse_tree_t *statement)
{
    for (parse_tree_t *at = symbol->decl_node; at; at = parse_tree_parent(at))
        if (at == statement) return true;
    return false;
}

static parse_tree_t *
declared_type_spec(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                   parse_tree_t *statement)
{
    for (parse_tree_t *at = symbol->decl_node; at && at != statement;
         at = parse_tree_parent(at)) {
        parse_tree_t *type_spec = CHILD_NT(grammar, at, "type-spec");
        if (type_spec) return type_spec;
    }
    return nullptr;
}

static void
poison_statement_exits(semantic_walk_context_t *walk, parse_tree_t *node,
                       th_type *poison)
{
    parse_grammar_t *grammar = walk->context->grammar;
    if (!node || core_tree_is_leaf(node) || NT_IS(grammar, node, "func-def"))
        return;
    if (NT_IS(grammar, node, "return-stmt")
        || NT_IS(grammar, node, "fail-stmt")) {
        parse_tree_t *value = CHILD_NT(grammar, node, "expression");
        if (value) parse_tree_set_type(value, poison);
        record_function_exit_fact(grammar, walk->unit, node,
                                  tu_state(walk->unit));
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        poison_statement_exits(walk, core_tree_child(node, i), poison);
}

static void
poison_failed_statement(semantic_walk_context_t *walk,
                          parse_tree_t *statement)
{
    n00bb_tu_state_t *state = tu_state(walk->unit);
    th_ctx *types = walk->context->types;
    th_type *poison = th_poison(types);
    if (NT_IS(walk->context->grammar, statement, "body-stmt"))
        poison_statement_exits(walk, statement, poison);
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_VARIABLE
                || !declared_within(symbol, statement)) continue;
            parse_tree_t *type_spec = declared_type_spec(
                walk->context->grammar, symbol, statement);
            th_type *declared = type_spec ? parse_tree_type(type_spec)
                                          : nullptr;
            if (type_spec && declared) {
                if (!symbol->type) symbol->type = declared;
                continue;
            }
            th_type *current = symbol->type ? th_resolve(symbol->type)
                                            : nullptr;
            if (current && th_kind_of(current) == TH_VAR)
                (void)th_unify_in_place(types, current, poison);
            else symbol->type = poison;
        }
    }
    for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
         scope; scope = scope->all_next) {
        if (!scope->node || !n00bb_field_scope_tag(scope->scope_tag))
            continue;
        bool inside = false;
        for (parse_tree_t *at = scope->node; at && !inside;
             at = parse_tree_parent(at))
            inside = at == statement;
        parse_sym_entry_t *record = inside ? parse_tree_sym(scope->node)
                                           : nullptr;
        if (record && field_record_declaration(walk->context->grammar,
                                               scope->node)
            && (!record->type || !th_type_definition(types, record->type)))
            (void)register_record_semantic_shape(walk->context->grammar,
                                                 walk->unit, scope->node);
    }
}

static parse_visit_outcome_t
semantic_walk_status(semantic_walk_context_t *walk, parse_tree_t *node)
{
    size_t errors = unit_error_count(walk->unit);
    if (errors > walk->errors_seen) {
        walk->errors_seen = errors;
        parse_tree_t *statement = node;
        while (statement
               && !NT_IS(walk->context->grammar, statement, "top-level-stmt")
               && !NT_IS(walk->context->grammar, statement, "body-stmt"))
            statement = parse_tree_parent(statement);
        if (!statement) return PARSE_VISIT_ERROR;
        walk->failed_statement = statement;
        poison_failed_statement(walk, statement);
    }
    if (!walk->failed_statement) return PARSE_VISIT_CONTINUE;
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at))
        if (at == walk->failed_statement) return PARSE_VISIT_PRUNE;
    walk->failed_statement = nullptr;
    return PARSE_VISIT_CONTINUE;
}

static th_type *
enclosing_array_element_type(parse_grammar_t *grammar, parse_tree_t *node)
{
    parse_tree_t *child = node;
    for (parse_tree_t *parent = parse_tree_parent(child); parent;
         child = parent, parent = parse_tree_parent(parent)) {
        if (NT_IS(grammar, parent, "list-lit")) {
            if (child != CHILD_NT(grammar, parent, "list-elems"))
                return nullptr;
            th_type *type = parse_tree_type(parent);
            type = type ? th_resolve(type) : nullptr;
            return type && th_kind_of(type) == TH_SEQUENCE
                ? th_seq_elem(type) : nullptr;
        }
        bool grouped = parse_tree_is_group(parent)
            && NT_IS(grammar, parse_tree_parent(parent), "list-elems");
        if (grouped || NT_IS(grammar, parent, "list-elems")) {
            if (!NT_IS(grammar, child, "expression")
                && !parse_tree_is_group(child)) return nullptr;
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
    }
    if (core_tree_is_leaf(visit->node)) {
        parse_token_info_t *token = parse_node_token(visit->node);
        if (token && core_option_is_set(parse_token_postfix_modifier(token))
            && !literal_modifier_terminal(context->grammar, visit->node,
                                          visit->parent))
            report_modifier_without_literal(unit, visit->node,
                core_option_get(parse_token_postfix_modifier(token)), false);
        return semantic_walk_status(walk, visit->node);
    }
    if (NT_IS(context->grammar, visit->node, "list-lit")
        && !parse_tree_type(visit->node)) {
        th_type *expected = frame->expected ? frame->expected
            : enclosing_array_element_type(context->grammar, visit->node);
        if (expected && th_kind_of(th_resolve(expected)) == TH_SEQUENCE)
            parse_tree_set_type(visit->node, expected);
    }
    frame->in_expression |= NT_IS(context->grammar, visit->node,
                                   "expression");
    if (NT_IS(context->grammar, visit->node, "flagset-stmt")
        && tu_state(unit))
        tu_state(unit)->flagset_scope = visit->node;
    semantic_annotation_node(context, unit, visit->node, visit->parent,
                             SEMANTIC_PREPARE_ENTER, visit,
                             &frame->annotation_cache);
    return semantic_walk_status(walk, visit->node);
}

static parse_visit_outcome_t
semantic_walk_after_enter(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_AFTER_ENTER, nullptr,
                             &frame->annotation_cache);
    return semantic_walk_status(walk, visit->node);
}

static parse_visit_outcome_t
semantic_walk_before_exit(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    if (core_tree_is_leaf(visit->node)) return PARSE_VISIT_CONTINUE;
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_PREPARE_EXIT, visit,
                             &frame->annotation_cache);
    return semantic_walk_status(walk, visit->node);
}

static parse_visit_outcome_t
semantic_walk_after_reference_exit(parse_visit_t *visit)
{
    semantic_walk_context_t *walk = visit->user;
    semantic_walk_frame_t *frame = &walk->frames.data[visit->depth];
    semantic_annotation_node(walk->context, walk->unit, visit->node,
                             visit->parent, SEMANTIC_AFTER_REFERENCE_EXIT,
                             nullptr, &frame->annotation_cache);
    return semantic_walk_status(walk, visit->node);
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
        n00bb_tu_internal_error(walk->unit, visit->node, "checking types",
            "could not retain alias edges");
    return semantic_walk_status(walk, visit->node);
}

static bool
switch_variant_case_names(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(core_string_t) *names)
{
    if (!node || core_tree_is_leaf(node)) return true;
    if (NT_IS(grammar, node, "body") || NT_IS(grammar, node, "value-body"))
        return true;
    if (NT_IS(grammar, node, "case-expr-item")) {
        core_string_t name = {0};
        parse_tree_t *expression = CHILD_NT(grammar, node, "expression");
        if (CHILD_NT(grammar, node, "range-sep")
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
                || !n00bb_member_visible_to(root, unit, symbol)
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
    if ((!NT_IS(grammar, switch_node, "switch-stmt")
         && !NT_IS(grammar, switch_node, "switch-value-expr"))
        || !NT_IS(grammar, subject_expression, "expression")) return true;
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
        NT_IS(grammar, switch_node, "switch-stmt")
            ? CORE_STRING_STATIC("switch-cases")
            : CORE_STRING_STATIC("switch-value-cases"));
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
    th_ctx *types = tu_state(unit)->types;
    core_string_t variant = n00bb_print_type(types, candidate);
    core_string_t actual = n00bb_print_type(types, symbol->type);
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (n00bb_report(diagnostics, unit, N00B_DIAG_SWITCH_CASE_SUBJECT_MISMATCH,
        link_source_location(subject_expression),
        N00B_TEXT_THE_CASES_OF_THIS_SWITCH,
        N00BB_ARGS(ARG_TEXT(variant), ARG_TEXT(symbol->name), ARG_TEXT(actual)))
        && symbol->decl_node)
        n00bb_report_secondary(diagnostics, nullptr,
            link_source_location(symbol->decl_node), N00B_TEXT_IS_DECLARED_HERE,
            N00BB_ARGS(ARG_TEXT(symbol->name)));
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
    if (!infer_indexed_switch_variant_subject(context->grammar, unit,
                                               node, child))
        return PARSE_VISIT_ERROR;
    if (NT_IS(context->grammar, node, "func-def")
        && NT_IS(context->grammar, child, "func-type-signature")) {
        record_cached_declared_function_signature(context->grammar,
            unit, child, tu_state(unit));
        bind_function_body_signature(unit, child, tu_state(unit));
    }
    if (NT_IS(context->grammar, node, "context-def")
        && NT_IS(context->grammar, child, "context-spec"))
        record_context_signature(context->grammar, unit, node,
                                 tu_state(unit));
    if ((NT_IS(context->grammar, node, "context-exit")
         || NT_IS(context->grammar, node, "context-fail")
         || NT_IS(context->grammar, node, "context-ensure"))
        && NT_IS(context->grammar, child, "context-hook-params"))
        record_context_hook_signature(context->grammar, unit, node,
                                      tu_state(unit));
    return semantic_walk_status(walk, visit->node);
}

static bool
run_semantic_walk(parse_grammar_t *grammar, th_ctx *types,
                  core_translation_unit_t *unit, parse_tree_t *root,
                  parse_tree_t *parent, int32_t depth, int32_t child_index,
                  bool skip_typed_ahead)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!root || !state || !state->annotations) return false;
    state->grammar = grammar;
    core_string_t package = n00bb_package_id(unit);
    if (!th_ctx_set_scope(types, unit->module.identifier, package))
        return false;
    annotation_context_t context = {
        .grammar = grammar,
        .types = types,
    };
    semantic_walk_context_t walk_context = {
        .context = &context,
        .unit = unit,
        .skip_typed_ahead = skip_typed_ahead,
        .errors_seen = unit_error_count(unit),
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
    for (size_t i = 0; i < context.assignment_targets.len; i++)
        report_view_write(grammar, unit, context.assignment_targets.data[i]);
    core_list_free(context.assignment_targets);
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
    if (!node || core_tree_is_leaf(node) || NT_IS(grammar, node, "func-def"))
        return;
    if (NT_IS(grammar, node, "module-import")) {
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
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    if (!n00bb_report(diagnostics, unit, N00B_DIAG_NAME_SHADOWS_PACKAGE,
        location, N00B_TEXT_CANNOT_BE_DECLARED_HERE_BECAUSE_2,
        N00BB_ARGS(ARG_TEXT(name),
                   ARG_TEXT(name))))
        return;
    n00bb_report_secondary(diagnostics, nullptr, link_source_location(import),
        N00B_TEXT_MODULE_IS_IMPORTED_HERE,
        N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_help(diagnostics, N00B_TEXT_RENAME_THIS_DECLARATION_OR_IMPORT,
        N00BB_ARGS(ARG_TEXT(name)));
}

bool
n00bb_declares_litmod(n00bb_tu_state_t *state, parse_sym_entry_t *symbol)
{
    for (size_t m = 0; state && m < state->literal_modifiers.len; m++) {
        n00bb_user_litmod_t *registration = &state->literal_modifiers.data[m];
        parse_tree_t *declaration = registration->declaration;
        if ((symbol && registration->function == symbol)
            || (declaration && !core_tree_is_leaf(declaration)
                && parse_tree_sym(declaration) == symbol))
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
        parse_tree_t *import = imports.data[i];
        core_string_t name = core_tree_num_children(import) == 3
            ? parse_tree_leaf_text(core_tree_child(import, 2))
            : first_token_text(import);
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
    if (!tree || !th_ctx_set_scope(types, unit->module.identifier, package)
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
                         parse_tree_t *declaration, core_string_t qualified)
{
    if (NT_IS(grammar, declaration, "variant-decl"))
        return th_variant(types, qualified, nullptr, 0);
    if (NT_IS(grammar, declaration, "alias-stmt")
        && alias_declaration_is_transparent(grammar, declaration))
        return th_var(types);
    bool nominal = field_record_declaration(grammar, declaration)
        || NT_IS(grammar, declaration, "enum-stmt")
        || NT_IS(grammar, declaration, "flagset-stmt")
        || NT_IS(grammar, declaration, "callback-decl")
        || NT_IS(grammar, declaration, "alias-stmt");
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
                    symbol->decl_node, core_buffer_string(&qualified));
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
        || !NT_IS(grammar, symbol->decl_node, "sym-decl")) return nullptr;
    parse_tree_t *declaration = symbol->decl_node;
    while (declaration && !NT_IS(grammar, declaration, "variable-decl"))
        declaration = parse_tree_parent(declaration);
    parse_tree_t *prefix = CHILD_NT(grammar, declaration,
                                    "declaration-prefix");
    if (!prefix || !core_string_eq(first_token_text(CHILD_NT(grammar, prefix,
            "binding-kind")), CORE_STRING_STATIC("const"))) return nullptr;
    for (parse_tree_t *at = parse_tree_parent(declaration); at;
         at = parse_tree_parent(at))
        if (executable_scope_node(grammar, at)
            || NT_IS(grammar, at, "func-def")) return nullptr;
    return declaration;
}

static bool
type_referenced_constants_ahead(parse_grammar_t *grammar, th_ctx *types,
                                core_translation_unit_t *unit,
                                parse_tree_t *declaration, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return true;
    parse_scope_t *scope = nullptr;
    for (parse_tree_t *at = node; at && !scope; at = parse_tree_parent(at))
        scope = parse_tree_scope(at);
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
    parse_tree_t *parent = parse_tree_parent(declaration);
    int32_t child_index = 0;
    int32_t depth = 0;
    for (size_t i = 0; parent && i < core_tree_num_children(parent); i++)
        if (core_tree_child(parent, i) == declaration)
            child_index = (int32_t)i;
    for (parse_tree_t *at = parent; at; at = parse_tree_parent(at))
        depth++;
    parse_scope_t *scope = parse_tree_scope(declaration);
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
    const core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(unit);
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child) || executable_scope_node(grammar, child))
            continue;
        size_t failures = core_diagnostics_failures(diagnostics);
        size_t internal = core_diagnostics_count_severity(diagnostics,
                                                          CORE_DIAG_INTERNAL);
        if (type_declaration_node(grammar, child)
            ? type_declaration_ahead(grammar, types, unit, child)
            : annotate_type_declarations(grammar, types, unit, child))
            continue;
        if (!field_record_declaration(grammar, child)
            || core_diagnostics_failures(diagnostics) == failures
            || core_diagnostics_count_severity(diagnostics,
                   CORE_DIAG_INTERNAL) != internal)
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
    if (!validate_litmod_aliases(unit)
        || !report_shadowed_litmods(unit, state)) return false;
    bool annotated = run_semantic_walk(grammar, types, unit, tree, nullptr,
                                       0, 0, true);
    const core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(unit);
    bool reported = core_diagnostics_failed(diagnostics);
    if (!reject_imported_module_names(grammar, unit, state, tree)
        || (!annotated && !reported)
        || !resolve_forward_literal_sites(unit, state)
        || core_translation_unit_has_fatal_diagnostics(unit)
        || core_diagnostics_count_severity(diagnostics, CORE_DIAG_INTERNAL)
        || (finish && !finish(context, unit))) return false;
    n00bb_index_type_symbols(state);
    state->semantic_errors = core_diagnostics_failed(diagnostics);
    state->semantic_annotation_complete = true;
    return true;
}

static bool
component_member(core_list_t(core_translation_unit_ptr_t) *members,
                 core_translation_unit_t *unit)
{
    for (size_t i = 0; i < members->len; i++)
        if (members->data[i] == unit) return true;
    return false;
}

static void
order_component_member(core_list_t(core_translation_unit_ptr_t) *members,
                       core_translation_unit_t *unit,
                       core_list_t(core_translation_unit_ptr_t) *ordered)
{
    if (component_member(ordered, unit)) return;
    core_list_push(*ordered, unit);
    size_t at = ordered->len - 1;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_translation_unit_t *imported =
            core_translation_unit_import(unit, i);
        if (component_member(members, imported))
            order_component_member(members, imported, ordered);
    }
    for (size_t i = at; i + 1 < ordered->len; i++)
        ordered->data[i] = ordered->data[i + 1];
    ordered->data[ordered->len - 1] = unit;
}

static void
order_component_bodies(core_list_t(core_translation_unit_ptr_t) *members)
{
    core_list_t(core_translation_unit_ptr_t) ordered =
        core_list_new(core_translation_unit_ptr_t);
    for (size_t i = 0; i < members->len; i++)
        order_component_member(members, members->data[i], &ordered);
    for (size_t i = 0; i < members->len && i < ordered.len; i++)
        members->data[i] = ordered.data[i];
    core_list_free(ordered);
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
    if (members->len > 1 && !prepopulate_component_package_interfaces(
            members->data, members->len)) return false;
    if (members->len > 1) order_component_bodies(members);
    for (size_t i = 0; i < members->len; i++) {
        if (!annotate_unit_bodies(grammar, types, members->data[i], finish,
                                  context)) return false;
        if (members->len > 1 && !refresh_component_export_bindings(
                members->data, members->len)) return false;
    }
    return true;
}

typedef struct {
    size_t index;
    size_t low;
    bool on_stack;
    bool blocked;
} component_vertex_t;

typedef struct {
    parse_grammar_t *grammar;
    th_ctx *types;
    n00bb_semantic_unit_finish_t finish;
    void *context;
    core_dict_t vertices;
    core_list_t(core_translation_unit_ptr_t) stack;
    size_t next_index;
    bool failed;
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
        if (imported_state->semantic_annotation_failed) {
            vertex->blocked = true;
            continue;
        }
        bool found = false;
        component_vertex_t *next = core_dict_get(&search->vertices,
                                                 imported, &found);
        if (!found) {
            if (!annotate_components_from(search, imported, &next))
                return false;
            if (next->low < vertex->low) vertex->low = next->low;
            if (imported_state->semantic_annotation_failed)
                vertex->blocked = true;
        }
        else if (next->on_stack && next->index < vertex->low)
            vertex->low = next->index;
    }
    if (vertex->low != vertex->index) return true;
    core_list_t(core_translation_unit_ptr_t) members =
        core_list_new(core_translation_unit_ptr_t);
    core_translation_unit_t *member = nullptr;
    bool blocked = false;
    do {
        member = search->stack.data[--search->stack.len];
        component_vertex_t *popped = core_dict_get(&search->vertices,
                                                   member, nullptr);
        popped->on_stack = false;
        blocked = blocked || popped->blocked;
        core_list_push(members, member);
    } while (member != unit);
    bool annotated = !blocked
        && annotate_component(search->grammar, search->types, &members,
                              search->finish, search->context);
    if (!annotated) {
        for (size_t i = 0; i < members.len; i++)
            tu_state(members.data[i])->semantic_annotation_failed = true;
        search->failed = true;
    }
    core_list_free(members);
    return true;
}

bool
semantic_annotate_closure(parse_grammar_t *grammar, th_ctx *types,
    core_translation_unit_t *unit, n00bb_semantic_unit_finish_t finish,
    void *context)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || state->semantic_annotation_failed) return false;
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
    return annotated && !search.failed;
}
