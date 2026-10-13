#include <limits.h>
#include <string.h>

#include "compiler_tu.h"
#include "compiler_link.h"

static core_csp_var_id_t
n00bb_range_new_var(core_csp_store_t *store, parse_tree_t *node,
                   int64_t minimum, int64_t maximum)
{
    int64_t type_minimum = 0, type_maximum = 0;
    if (!store || !node || !is_integer_type(
            core_tree_node_value(node).type)
        || !n00bb_integer_interval_type(core_tree_node_value(node).type,
            &type_minimum, &type_maximum)) return -1;
    if (minimum < type_minimum) minimum = type_minimum;
    if (maximum > type_maximum) maximum = type_maximum;
    if (minimum > maximum) return -1;
    return core_csp_new_var(store, CORE_STRING_STATIC("integer-expression"),
                            core_csp_dom_range(minimum, maximum));
}

static bool
n00bb_range_store_fact(core_csp_store_t *store, parse_tree_t *node,
                       core_csp_var_id_t variable)
{
    if (variable < 0) return false;
    const core_csp_domain_t *domain =
        core_result_get_ok(core_csp_var_domain(store, variable));
    return domain && !core_csp_dom_is_empty(domain)
        && parse_tree_set_integer_range(node, core_csp_dom_min(domain),
                                        core_csp_dom_max(domain));
}

bool
n00bb_integer_range_wrapper(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const char *const wrappers[] = {
        "expression", "or-expr", "and-expr", "or-rhs", "and-rhs", "equality-expr",
        "compare-expr", "bitor-expr", "bitxor-expr", "bitand-expr",
        "shift-expr", "add-expr", "mul-expr", "unary-expr",
        "postfix-expr", "primary", "tuple-or-paren",
    };
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
        if (nt_is(grammar, node, wrappers[i])) return true;
    return false;
}


static core_csp_var_id_t
n00bb_range_variable_for_node(n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!state || !state->integer_range_store) return -1;
    uintptr_t slot = (uintptr_t)core_dict_get(
        &state->integer_range_variables, node, nullptr);
    return slot ? (core_csp_var_id_t)(slot - 1) : -1;
}

static const n00bb_symbol_range_t *
n00bb_range_for_symbol(const n00bb_tu_state_t *state,
                      const parse_sym_entry_t *symbol)
{
    for (size_t i = state ? state->integer_symbol_ranges.len : 0; i > 0;
         i--) {
        const n00bb_symbol_range_t *range =
            &state->integer_symbol_ranges.data[i - 1];
        if (range->symbol == symbol) return range;
    }
    return nullptr;
}

static bool
n00bb_range_symbol_invalidated(const n00bb_tu_state_t *state,
                               const parse_sym_entry_t *symbol)
{
    for (size_t i = 0; state && symbol
            && i < state->integer_range_invalidated_symbols.len; i++)
        if (state->integer_range_invalidated_symbols.data[i] == symbol)
            return true;
    return false;
}

static void
n00bb_range_invalidate_symbol(n00bb_tu_state_t *state,
                             parse_sym_entry_t *symbol)
{
    if (!state || !symbol
        || n00bb_range_symbol_invalidated(state, symbol)) return;
    core_list_push(state->integer_range_invalidated_symbols, symbol);
}

static void
n00bb_range_forget_symbol(n00bb_tu_state_t *state,
                          const parse_sym_entry_t *symbol)
{
    if (!state || !symbol) return;
    for (size_t i = 0; i < state->integer_symbol_ranges.len; i++) {
        if (state->integer_symbol_ranges.data[i].symbol != symbol) continue;
        memmove(&state->integer_symbol_ranges.data[i],
                &state->integer_symbol_ranges.data[i + 1],
                (state->integer_symbol_ranges.len - i - 1)
                    * sizeof(state->integer_symbol_ranges.data[0]));
        state->integer_symbol_ranges.len--;
        return;
    }
}

static void
n00bb_range_set_symbol(n00bb_tu_state_t *state, parse_sym_entry_t *symbol,
                       int64_t minimum, int64_t maximum,
                       bool proven_above_int64)
{
    if (!state || !symbol) return;
    for (size_t i = 0; i < state->integer_symbol_ranges.len; i++) {
        n00bb_symbol_range_t *range =
            &state->integer_symbol_ranges.data[i];
        if (range->symbol != symbol) continue;
        range->minimum = minimum;
        range->maximum = maximum;
        range->proven_above_int64 = proven_above_int64;
        return;
    }
    core_list_push(state->integer_symbol_ranges,
        ((n00bb_symbol_range_t){.symbol = symbol,
                                .minimum = minimum,
                                .maximum = maximum,
                                .proven_above_int64 = proven_above_int64}));
}

static core_list_t(n00bb_symbol_range_t)
n00bb_range_copy(const core_list_t(n00bb_symbol_range_t) *source)
{
    core_list_t(n00bb_symbol_range_t) copy = core_list_new_cap(
        n00bb_symbol_range_t, source ? source->len : 0);
    for (size_t i = 0; source && i < source->len; i++)
        core_list_push(copy, source->data[i]);
    return copy;
}

static const n00bb_symbol_range_t *
n00bb_range_find_in(const core_list_t(n00bb_symbol_range_t) *ranges,
                    const parse_sym_entry_t *symbol)
{
    for (size_t i = ranges ? ranges->len : 0; i > 0; i--)
        if (ranges->data[i - 1].symbol == symbol)
            return &ranges->data[i - 1];
    return nullptr;
}

static void
n00bb_range_merge_branch(n00bb_range_branch_frame_t *frame,
                         const core_list_t(n00bb_symbol_range_t) *branch)
{
    if (!frame || !branch) return;
    if (!frame->has_branch) {
        frame->merged = n00bb_range_copy(branch);
        frame->has_branch = true;
        return;
    }
    for (size_t i = frame->merged.len; i > 0; i--) {
        n00bb_symbol_range_t *merged = &frame->merged.data[i - 1];
        const n00bb_symbol_range_t *other = n00bb_range_find_in(
            branch, merged->symbol);
        if (!other) {
            memmove(merged, merged + 1,
                (frame->merged.len - i) * sizeof(*merged));
            frame->merged.len--;
            continue;
        }
        if (other->proven_above_int64
            != merged->proven_above_int64) {
            memmove(merged, merged + 1,
                (frame->merged.len - i) * sizeof(*merged));
            frame->merged.len--;
            continue;
        }
        if (merged->proven_above_int64) continue;
        if (other->minimum < merged->minimum)
            merged->minimum = other->minimum;
        if (other->maximum > merged->maximum)
            merged->maximum = other->maximum;
    }
}

static void
n00bb_range_restore(n00bb_tu_state_t *state,
                    const core_list_t(n00bb_symbol_range_t) *ranges)
{
    if (!state || !ranges) return;
    core_list_free(state->integer_symbol_ranges);
    state->integer_symbol_ranges = n00bb_range_copy(ranges);
}

static n00bb_range_branch_frame_t *
n00bb_range_branch_top(n00bb_tu_state_t *state, parse_tree_t *if_node)
{
    if (!state || !state->integer_range_branches.len) return nullptr;
    n00bb_range_branch_frame_t *frame =
        &state->integer_range_branches.data[
            state->integer_range_branches.len - 1];
    return !if_node || frame->node == if_node ? frame : nullptr;
}

static parse_tree_t *
n00bb_range_if_owner(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at;
         at = core_tree_node_value(at).parent)
        if (nt_is(grammar, at, "if-stmt")) return at;
    return nullptr;
}

static parse_tree_t *
n00bb_range_direct_comparison(parse_grammar_t *grammar,
                              parse_tree_t *condition)
{
    for (parse_tree_t *node = condition; node && !core_tree_is_leaf(node);) {
        if ((nt_is(grammar, node, "equality-expr")
                || nt_is(grammar, node, "compare-expr"))
            && core_tree_num_children(node) == 3) return node;
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

bool
n00bb_range_simple_integer_literal(parse_grammar_t *grammar,
                                   parse_tree_t *node, int64_t *value)
{
    while (node && !core_tree_is_leaf(node)) {
        if (nt_is(grammar, node, "simple-lit")) {
            ir_lit_t literal = {0};
            if (!n00bb_parse_switch_integer(first_token_text(node), &literal)
                || literal.mag > (ir_uint_t)INT64_MAX) return false;
            if (value) *value = (int64_t)literal.mag;
            return true;
        }
        if (core_tree_num_children(node) != 1) return false;
        node = core_tree_child(node, 0);
    }
    return false;
}

static void
n00bb_range_refine_comparison(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state,
                              parse_tree_t *condition, bool true_branch)
{
    parse_tree_t *comparison = n00bb_range_direct_comparison(grammar,
                                                              condition);
    if (!comparison || core_tree_num_children(comparison) != 3) return;
    parse_tree_t *left = core_tree_child(comparison, 0);
    parse_tree_t *right = core_tree_child(comparison, 2);
    core_string_t op = first_token_text(core_tree_child(comparison, 1));
    parse_tree_t *lvalue = n00bb_simple_lvalue(left);
    parse_sym_entry_t *symbol = nullptr;
    int64_t constant = 0;
    bool variable_left = false;
    if (lvalue) {
        parse_node_resolution_t resolution = parse_tree_resolution(lvalue);
        if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
            && resolution.target.symbol
            && resolution.target.symbol->kind == PARSE_SYM_VARIABLE) {
            symbol = resolution.target.symbol;
            variable_left = true;
        }
    }
    parse_tree_t *constant_node = variable_left ? right : left;
    if (!symbol) {
        lvalue = n00bb_simple_lvalue(right);
        if (lvalue) {
            parse_node_resolution_t resolution = parse_tree_resolution(lvalue);
            if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
                && resolution.target.symbol
                && resolution.target.symbol->kind == PARSE_SYM_VARIABLE) {
                symbol = resolution.target.symbol;
                constant_node = left;
            }
        }
    }
    if (!symbol || !is_integer_type(symbol->type)) return;
    if (!n00bb_range_simple_integer_literal(grammar, constant_node,
                                             &constant)) return;
    if (!variable_left) {
        if (core_string_eq(op, CORE_STRING_STATIC("<")))
            op = CORE_STRING_STATIC(">");
        else if (core_string_eq(op, CORE_STRING_STATIC("<=")))
            op = CORE_STRING_STATIC(">=");
        else if (core_string_eq(op, CORE_STRING_STATIC(">")))
            op = CORE_STRING_STATIC("<");
        else if (core_string_eq(op, CORE_STRING_STATIC(">=")))
            op = CORE_STRING_STATIC("<=");
    }
    int64_t minimum = 0, maximum = 0;
    const n00bb_symbol_range_t *known = n00bb_range_for_symbol(state,
                                                               symbol);
    if (known && known->proven_above_int64) return;
    if (known) {
        minimum = known->minimum;
        maximum = known->maximum;
    }
    else if (!n00bb_integer_interval_type(symbol->type, &minimum,
                                           &maximum)) return;
    if (core_string_eq(op, CORE_STRING_STATIC(">"))) {
        if (true_branch && constant < INT64_MAX && minimum < constant + 1)
            minimum = constant + 1;
        if (!true_branch && maximum > constant) maximum = constant;
    }
    else if (core_string_eq(op, CORE_STRING_STATIC(">="))) {
        if (true_branch && minimum < constant) minimum = constant;
        if (!true_branch && constant > INT64_MIN
            && maximum >= constant) maximum = constant - 1;
    }
    else if (core_string_eq(op, CORE_STRING_STATIC("<"))) {
        if (true_branch && constant > INT64_MIN
            && maximum >= constant) maximum = constant - 1;
        if (!true_branch && minimum < constant) minimum = constant;
    }
    else if (core_string_eq(op, CORE_STRING_STATIC("<="))) {
        if (true_branch && maximum > constant) maximum = constant;
        if (!true_branch && constant < INT64_MAX
            && minimum <= constant) minimum = constant + 1;
    }
    else if (core_string_eq(op, CORE_STRING_STATIC("=="))) {
        if (true_branch) minimum = maximum = constant;
    }
    else if (core_string_eq(op, CORE_STRING_STATIC("!="))) {
        if (!true_branch) minimum = maximum = constant;
    }
    if (minimum <= maximum)
        n00bb_range_set_symbol(state, symbol, minimum, maximum, false);
}

static void
n00bb_range_enter_if(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                     parse_tree_t *node)
{
    if (!state || !nt_is(grammar, node, "if-stmt")) return;
    n00bb_range_branch_frame_t frame = {
        .node = node,
        .incoming = n00bb_range_copy(&state->integer_symbol_ranges),
    };
    core_list_push(state->integer_range_branches, frame);
}

static bool
n00bb_range_begin_branch(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *parent)
{
    if (!state || !parent) return false;
    bool is_if = nt_is(grammar, parent, "if-stmt");
    bool is_elif = nt_is(grammar, parent, "elif-clause");
    bool is_else = nt_is(grammar, parent, "else-clause");
    if (!is_if && !is_elif && !is_else) return false;
    parse_tree_t *owner = n00bb_range_if_owner(grammar, parent);
    n00bb_range_branch_frame_t *frame = n00bb_range_branch_top(state, owner);
    if (!frame) return false;
    n00bb_range_restore(state, &frame->incoming);
    frame->branch_active = true;
    if (is_else) {
        frame->has_else = true;
        parse_tree_t *chain = child_nt(grammar, owner, "elif-chain");
        if (!child_nt(grammar, chain, "elif-clause"))
            n00bb_range_refine_comparison(grammar, state,
                child_nt(grammar, owner, "expression"), false);
    }
    else {
        n00bb_range_refine_comparison(grammar, state,
            child_nt(grammar, parent, "expression"), true);
    }
    return true;
}

static bool
n00bb_range_end_branch(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                      parse_tree_t *body, parse_tree_t *parent)
{
    (void)body;
    if (!state || !parent) return false;
    if (!nt_is(grammar, parent, "if-stmt")
        && !nt_is(grammar, parent, "elif-clause")
        && !nt_is(grammar, parent, "else-clause")) return false;
    parse_tree_t *owner = n00bb_range_if_owner(grammar, parent);
    n00bb_range_branch_frame_t *frame = n00bb_range_branch_top(state, owner);
    if (!frame || !frame->branch_active) return false;
    n00bb_range_merge_branch(frame, &state->integer_symbol_ranges);
    frame->branch_active = false;
    n00bb_range_restore(state, &frame->incoming);
    return true;
}

static void
n00bb_range_end_if(n00bb_tu_state_t *state, parse_tree_t *node)
{
    n00bb_range_branch_frame_t *frame = n00bb_range_branch_top(state, node);
    if (!frame) return;
    if (!frame->has_else)
        n00bb_range_merge_branch(frame, &frame->incoming);
    core_list_free(frame->incoming);
    core_list_free(state->integer_symbol_ranges);
    state->integer_symbol_ranges = frame->merged;
    frame->merged = (core_list_t(n00bb_symbol_range_t)){0};
    state->integer_range_branches.len--;
}

static bool
n00bb_range_in_active_branch(parse_grammar_t *grammar,
                             n00bb_tu_state_t *state, parse_tree_t *node)
{
    parse_tree_t *owner = nullptr;
    for (parse_tree_t *at = node
             ? core_tree_node_value(node).parent : nullptr;
         at; at = core_tree_node_value(at).parent) {
        if (nt_is(grammar, at, "while-stmt")
            || nt_is(grammar, at, "for-stmt")
            || nt_is(grammar, at, "labeled-while")
            || nt_is(grammar, at, "labeled-for")
            || nt_is(grammar, at, "switch-stmt")
            || nt_is(grammar, at, "typecase-stmt")
            || nt_is(grammar, at, "typecase-value-expr")
            || nt_is(grammar, at, "try-stmt")
            || nt_is(grammar, at, "with-stmt")
            || nt_is(grammar, at, "if-value-expr")) return false;
        if (nt_is(grammar, at, "if-stmt")) {
            owner = at;
            break;
        }
    }
    n00bb_range_branch_frame_t *frame = n00bb_range_branch_top(state, owner);
    return frame && frame->branch_active;
}

static bool
n00bb_range_control_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const char *const controls[] = {
        "if-stmt", "if-value-expr", "elif-clause", "else-clause",
        "while-stmt", "for-stmt", "labeled-while", "labeled-for",
        "switch-stmt", "typecase-stmt", "typecase-value-expr",
        "try-stmt", "with-stmt",
    };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
        if (nt_is(grammar, node, controls[i])) return true;
    return false;
}

static bool
n00bb_range_inside_control(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *parent = node
             ? core_tree_node_value(node).parent : nullptr;
         parent; parent = core_tree_node_value(parent).parent)
        if (n00bb_range_control_node(grammar, parent)) return true;
    return false;
}

static bool
n00bb_range_inside_function(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *parent = node
             ? core_tree_node_value(node).parent : nullptr;
         parent; parent = core_tree_node_value(parent).parent)
        if (nt_is(grammar, parent, "func-def")
            || nt_is(grammar, parent, "context-def")) return true;
    return false;
}

static parse_sym_phase_t
n00bb_range_execution_phase(parse_grammar_t *grammar, parse_tree_t *node)
{
    return lexical_execution_phase(grammar, node);
}

static void
n00bb_range_invalidate_symtab(n00bb_tu_state_t *state,
                              parse_symtab_t *symtab)
{
    for (parse_scope_t *scope = symtab ? symtab->all_scopes : nullptr;
         scope; scope = scope->all_next)
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope)
            if (symbol->kind == PARSE_SYM_VARIABLE
                && symbol->mutability == PARSE_SYM_CONST)
                n00bb_range_invalidate_symbol(state, symbol);
}

static parse_sym_entry_t *
n00bb_integer_reference_symbol(parse_tree_t *node)
{
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
        return resolution.target.symbol;
    return core_tree_node_value(node).sym;
}

static bool
n00bb_range_constant_uint64(parse_grammar_t *grammar, parse_tree_t *node,
                            uint64_t *value)
{
    if (!node || core_tree_is_leaf(node)) return false;
    size_t count = core_tree_num_children(node);
    if (nt_is(grammar, node, "simple-lit")) {
        ir_lit_t literal = {0};
        if (!n00bb_parse_switch_integer(first_token_text(node), &literal)
            || literal.mag > UINT64_MAX) return false;
        *value = (uint64_t)literal.mag;
        return true;
    }
    if (count == 1)
        return n00bb_range_constant_uint64(grammar, core_tree_child(node, 0),
                                           value);
    if (count == 3
        && parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("("))
        && parse_tree_leaf_is(core_tree_child(node, 2),
                              CORE_STRING_STATIC(")")))
        return n00bb_range_constant_uint64(grammar, core_tree_child(node, 1),
                                           value);
    uint64_t width = 0;
    bool is_signed = true;
    bool unsigned64 = n00bb_integer_representation(
        core_tree_node_value(node).type, &width, &is_signed)
        && width == 64 && !is_signed;
    if (unsigned64 && nt_is(grammar, node, "unary-expr") && count == 2
        && parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("~"))
        && n00bb_range_constant_uint64(grammar, core_tree_child(node, 1),
                                       value)) {
        *value = ~*value;
        return true;
    }
    if (unsigned64 && nt_is(grammar, node, "unary-expr") && count == 5
        && parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("cast")))
        return n00bb_range_constant_uint64(grammar, core_tree_child(node, 4),
                                           value);
    return false;
}

bool
n00bb_range_proven_above_int64(parse_grammar_t *grammar,
                               n00bb_tu_state_t *state,
                               parse_tree_t *node)
{
    if (!grammar || !state || !node || core_tree_is_leaf(node)) return false;
    if (core_dict_contains(&state->integer_proven_above_int64_nodes, node))
        return true;
    uint64_t constant = 0;
    if (n00bb_range_constant_uint64(grammar, node, &constant))
        return constant > INT64_MAX;
    if (nt_is(grammar, node, "primary")) {
        parse_sym_entry_t *symbol = n00bb_integer_reference_symbol(node);
        const n00bb_symbol_range_t *known = n00bb_range_for_symbol(state,
                                                                   symbol);
        return known && known->proven_above_int64;
    }
    if (core_tree_num_children(node) == 1)
        return n00bb_range_proven_above_int64(grammar, state,
                                              core_tree_child(node, 0));
    return false;
}

static parse_sym_entry_t *
n00bb_range_binding_symbol(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!nt_is(grammar, node, "primary")
        || core_tree_num_children(node) != 1) return nullptr;
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    return token && token->tid == PARSE_TOK_IDENTIFIER
        ? n00bb_integer_reference_symbol(node) : nullptr;
}

static void
n00bb_range_invalidate_static_symbols(n00bb_tu_state_t *state)
{
    if (!state) return;
    for (size_t i = 0; i < state->integer_symbol_ranges.len; i++)
        n00bb_range_invalidate_symbol(state,
            state->integer_symbol_ranges.data[i].symbol);
    n00bb_range_invalidate_symtab(state, state->annotations
        ? state->annotations->symtab : nullptr);
    const core_list_t(core_translation_unit_ptr_t) *units =
        n00bb_link_analysis_units(state->link_analysis);
    for (size_t i = 0; units && i < units->len; i++) {
        n00bb_tu_state_t *linked = tu_state(units->data[i]);
        n00bb_range_invalidate_symtab(state, linked && linked->annotations
            ? linked->annotations->symtab : nullptr);
    }
}

static void
n00bb_range_invalidate_call_mutations(parse_grammar_t *grammar,
                                      n00bb_tu_state_t *state)
{
    if (!state) return;
    core_dict_foreach(&state->integer_range_variables, binding) {
        parse_sym_entry_t *symbol = n00bb_range_binding_symbol(
            grammar, binding->key);
        if (symbol && symbol->mutability != PARSE_SYM_IMMUTABLE
            && symbol->mutability != PARSE_SYM_CONST)
            n00bb_range_invalidate_symbol(state, symbol);
    }
    for (size_t i = state->integer_symbol_ranges.len; i > 0; i--) {
        parse_sym_entry_t *symbol =
            state->integer_symbol_ranges.data[i - 1].symbol;
        if (symbol && symbol->mutability != PARSE_SYM_IMMUTABLE
            && symbol->mutability != PARSE_SYM_CONST)
            n00bb_range_forget_symbol(state, symbol);
    }
}

static void
n00bb_range_update_symbol(parse_grammar_t *grammar, parse_tree_t *node,
                          n00bb_tu_state_t *state, bool declaration,
                          bool compound)
{
    if (!state) return;
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    parse_annotation_t *assign = rule
        ? parse_rule_find_annot(grammar, rule, PARSE_ANNOT_ASSIGNS) : nullptr;
    if (!assign) return;

    parse_sym_entry_t *symbol = declaration
        ? core_tree_node_value(node).sym : nullptr;
    if (!declaration) {
        parse_tree_t *target = parse_tree_resolve_child_ref(grammar, node,
                                                            assign->name_ref);
        parse_tree_t *lvalue = n00bb_simple_lvalue(target);
        parse_node_resolution_t resolution = lvalue
            ? parse_tree_resolution(lvalue)
            : (parse_node_resolution_t){0};
        if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
            symbol = resolution.target.symbol;
    }
    if (!symbol) return;
    if (!declaration)
        n00bb_range_invalidate_symbol(state, symbol);
    if (n00bb_range_inside_control(grammar, node)
        && !n00bb_range_in_active_branch(grammar, state, node)) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }

    bool in_function = n00bb_range_inside_function(grammar, node);
    if ((in_function && !scope_is_within_function(symbol->scope))
        || (!in_function && scope_is_within_function(symbol->scope))) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }
    if (symbol->kind != PARSE_SYM_VARIABLE
        || !(declaration
            ? parse_sym_can_initialize(symbol,
                n00bb_range_execution_phase(grammar, node))
            : parse_sym_can_write(symbol,
                n00bb_range_execution_phase(grammar, node)))
        || !is_integer_type(symbol->type) || compound) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }

    parse_tree_t *value = parse_tree_resolve_child_ref(grammar, node,
                                                        assign->value_ref);
    int64_t minimum = 0, maximum = 0;
    bool proven_above_int64 = n00bb_range_proven_above_int64(grammar, state,
                                                              value);
    if (!proven_above_int64
        && (!value || !parse_tree_integer_range(value,
                                                &minimum, &maximum))) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }
    n00bb_range_invalidate_symbol(state, symbol);
    n00bb_range_set_symbol(state, symbol, minimum, maximum,
                           proven_above_int64);
}

static bool
n00bb_range_singleton(core_csp_store_t *store, core_csp_var_id_t variable,
                      int64_t *value)
{
    if (!store || variable < 0) return false;
    core_csp_domain_result_t result = core_csp_var_domain(store, variable);
    const core_csp_domain_t *domain = core_result_get_ok(result);
    if (!domain || core_csp_dom_is_empty(domain)
        || core_csp_dom_min(domain) != core_csp_dom_max(domain)) return false;
    if (value) *value = core_csp_dom_min(domain);
    return true;
}

static void
n00bb_propagate_integer_range_node(parse_grammar_t *grammar,
                                  n00bb_tu_state_t *state,
                                  parse_tree_t *node)
{
    core_csp_store_t *store = state ? state->integer_range_store : nullptr;
    if (!grammar || !state || !node || core_tree_is_leaf(node) || !store)
        return;
    size_t child_count = core_tree_num_children(node);
    core_list_t(int32_t) child_ranges = core_list_new(int32_t);
    for (size_t i = 0; i < child_count; i++)
        core_list_push(child_ranges, n00bb_range_variable_for_node(state,
            core_tree_child(node, i)));

    parse_nt_node_t *parsed = &core_tree_node_value(node);
    int64_t type_minimum = 0, type_maximum = 0;
    core_csp_var_id_t result = -1;
    parse_sym_entry_t *symbol = nullptr;
    if (nt_is(grammar, node, "simple-lit")) {
        ir_lit_t literal = {0};
        if (n00bb_parse_switch_integer(first_token_text(node), &literal)
            && literal.mag <= (ir_uint_t)INT64_MAX
            && (!parsed->type
                || th_kind_of(th_resolve(parsed->type)) == TH_VAR
                || is_integer_type(parsed->type))) {
            parsed->integer_literal_origin = node;
            int64_t value = (int64_t)literal.mag;
            if (!n00bb_integer_interval_type(parsed->type,
                    &type_minimum, &type_maximum)
                || (value >= type_minimum && value <= type_maximum))
                result = core_csp_new_var(store,
                    CORE_STRING_STATIC("integer-literal"),
                    core_csp_dom_singleton(value));
        }
        goto record;
    }
    if (!is_integer_type(parsed->type)
        || !n00bb_integer_interval_type(parsed->type,
            &type_minimum, &type_maximum)) {
        int64_t minimum = 0, maximum = 0;
        if (is_integer_type(parsed->type)
            && state->integer_range_use_symbol_facts
            && nt_is(grammar, node, "primary")) {
            const n00bb_symbol_range_t *known = n00bb_range_for_symbol(
                state, n00bb_integer_reference_symbol(node));
            if (known && !known->proven_above_int64)
                parse_tree_set_integer_range(node, known->minimum,
                                             known->maximum);
        }
        else if (is_integer_type(parsed->type) && child_count == 1
                 && n00bb_integer_range_wrapper(grammar, node)
                 && parse_tree_integer_range(core_tree_child(node, 0),
                                             &minimum, &maximum))
            parse_tree_set_integer_range(node, minimum, maximum);
        goto done;
    }

    if (nt_is(grammar, node, "add-expr") && child_count == 3
        && child_ranges.data[0] >= 0 && child_ranges.data[2] >= 0) {
        core_string_t op = first_token_text(core_tree_child(node, 1));
        if (core_string_eq(op, CORE_STRING_STATIC("+"))
            || core_string_eq(op, CORE_STRING_STATIC("-"))) {
            result = n00bb_range_new_var(store, node, type_minimum,
                                         type_maximum);
            core_csp_var_id_t vars[] = {result, child_ranges.data[0],
                                        child_ranges.data[2]};
            int64_t coeffs[] = {1, -1,
                core_string_eq(op, CORE_STRING_STATIC("+")) ? -1 : 1};
            if (result < 0
                || !core_csp_post_linear(store, vars, coeffs, 3, 0))
                result = -1;
        }
    }
    else if (nt_is(grammar, node, "shift-expr") && child_count == 3
        && child_ranges.data[0] >= 0 && child_ranges.data[2] >= 0
        && core_string_eq(first_token_text(core_tree_child(node, 1)),
                          CORE_STRING_STATIC("<<"))) {
        int64_t left = 0, count = 0;
        if (n00bb_range_singleton(store, child_ranges.data[0], &left)
            && n00bb_range_singleton(store, child_ranges.data[2], &count)
            && left >= 0 && count >= 0
            && count < (int64_t)(sizeof(int64_t) * CHAR_BIT - 1)
            && left <= (INT64_MAX >> count)) {
            int64_t value = left << count;
            if (value >= type_minimum && value <= type_maximum)
                result = core_csp_new_var(store,
                    CORE_STRING_STATIC("constant-left-shift"),
                    core_csp_dom_singleton(value));
        }
    }
    else if (nt_is(grammar, node, "mul-expr") && child_count == 3
        && child_ranges.data[0] >= 0 && child_ranges.data[2] >= 0
        && core_string_eq(first_token_text(core_tree_child(node, 1)),
                          CORE_STRING_STATIC("*"))) {
        int64_t factor = 0;
        core_csp_var_id_t variable = child_ranges.data[0];
        if (!n00bb_range_singleton(store, child_ranges.data[2], &factor)) {
            if (!n00bb_range_singleton(store, child_ranges.data[0],
                                       &factor)) goto done;
            variable = child_ranges.data[2];
        }
        if (factor != INT64_MIN) {
            result = n00bb_range_new_var(store, node, type_minimum,
                                         type_maximum);
            core_csp_var_id_t vars[] = {result, variable};
            int64_t coeffs[] = {1, -factor};
            if (result < 0
                || !core_csp_post_linear(store, vars, coeffs, 2, 0))
                result = -1;
        }
    }
    else if (nt_is(grammar, node, "unary-expr") && child_count == 2
        && child_ranges.data[1] >= 0) {
        core_string_t op = parse_tree_leaf_text(core_tree_child(node, 0));
        parse_tree_t *operand = core_tree_child(node, 1);
        if (core_tree_node_value(operand).integer_literal_origin
            && (core_string_eq(op, CORE_STRING_STATIC("-"))
                || core_string_eq(op, CORE_STRING_STATIC("+"))))
            parsed->integer_literal_origin =
                core_tree_node_value(operand).integer_literal_origin;
        if (core_string_eq(op, CORE_STRING_STATIC("-"))) {
            result = n00bb_range_new_var(store, node, type_minimum,
                                         type_maximum);
            core_csp_var_id_t vars[] = {result, child_ranges.data[1]};
            int64_t coeffs[] = {1, 1};
            if (result < 0
                || !core_csp_post_linear(store, vars, coeffs, 2, 0))
                result = -1;
        }
        else if (core_string_eq(op, CORE_STRING_STATIC("+"))) {
            result = n00bb_range_new_var(store, node, type_minimum,
                                         type_maximum);
            if (result < 0 || !core_csp_post_eq(store, result,
                    child_ranges.data[1])) result = -1;
        }
    }
    else if (nt_is(grammar, node, "primary")
        && child_count == 1 && child_ranges.data[0] < 0) {
        parse_token_info_t *token = parse_tree_find_first_terminal(node);
        if (token && token->tid == PARSE_TOK_IDENTIFIER) {
            symbol = n00bb_integer_reference_symbol(node);
            result = n00bb_range_new_var(store, node, type_minimum,
                                         type_maximum);
            const n00bb_symbol_range_t *known_range =
                state->integer_range_use_symbol_facts
                    ? n00bb_range_for_symbol(state, symbol) : nullptr;
            n00bb_static_value_t *static_value = symbol
                ? symbol->comptime_value : nullptr;
            if (result >= 0 && symbol
                && symbol->kind == PARSE_SYM_VARIABLE
                && symbol->mutability == PARSE_SYM_CONST
                && state->integer_range_use_symbol_facts
                && !n00bb_range_symbol_invalidated(state, symbol)
                && !known_range
                && symbol->has_comptime_value
                && static_value
                && static_value->kind == N00BB_STATIC_VALUE_INT) {
                __int128 value = static_value->as.integer;
                if (value >= type_minimum && value <= type_maximum
                    && value >= INT64_MIN && value <= INT64_MAX
                    && !core_csp_post_eq_const(store, result,
                                               (int64_t)value))
                    result = -1;
            }
            if (result >= 0 && known_range
                && !known_range->proven_above_int64
                && !core_csp_post_in(store, result,
                    core_csp_dom_range(known_range->minimum,
                                       known_range->maximum)))
                result = -1;
            core_dict_foreach(&state->integer_range_variables, binding) {
                if (result < 0 || !symbol
                    || n00bb_range_symbol_invalidated(state, symbol)) break;
                if (n00bb_range_binding_symbol(grammar, binding->key)
                    != symbol)
                    continue;
                core_csp_var_id_t previous =
                    (core_csp_var_id_t)((uintptr_t)binding->value - 1);
                if (!core_csp_post_eq(store, result, previous))
                    result = -1;
                break;
            }
        }
    }
    else if (n00bb_integer_range_wrapper(grammar, node)) {
        int32_t child = -1;
        size_t ranged = 0;
        parse_tree_t *literal_child = nullptr;
        bool literal_shape = true;
        for (size_t i = 0; i < child_ranges.len; i++) {
            parse_tree_t *member = core_tree_child(node, i);
            if (core_tree_is_leaf(member)) {
                core_string_t text = parse_tree_leaf_text(member);
                if (!core_string_eq(text, CORE_STRING_STATIC("+"))
                    && !core_string_eq(text, CORE_STRING_STATIC("-"))
                    && !core_string_eq(text, CORE_STRING_STATIC("("))
                    && !core_string_eq(text, CORE_STRING_STATIC(")")))
                    literal_shape = false;
            }
            else if (core_tree_node_value(member).integer_literal_origin) {
                if (literal_child) literal_shape = false;
                literal_child = member;
            }
            else literal_shape = false;
            if (child_ranges.data[i] < 0) continue;
            child = child_ranges.data[i];
            ranged++;
        }
        if (ranged == 1) {
            if (literal_shape && literal_child)
                parsed->integer_literal_origin =
                    core_tree_node_value(literal_child).integer_literal_origin;
            result = n00bb_range_new_var(store, node, type_minimum,
                                         type_maximum);
            if (result < 0 || !core_csp_post_eq(store, result, child))
                result = -1;
        }
    }

record:
    if (result >= 0)
        core_dict_put(&state->integer_range_variables, node, result + 1);
    goto done;

done:
    core_list_free(child_ranges);
}

static void
n00bb_finish_integer_range_expression(n00bb_tu_state_t *state)
{
    if (!state || !state->integer_range_store) return;
    core_dict_foreach(&state->integer_range_variables, binding) {
        if (!n00bb_range_store_fact(state->integer_range_store,
                binding->key,
                (core_csp_var_id_t)((uintptr_t)binding->value - 1)))
            continue;
    }
    core_csp_store_free(state->integer_range_store);
    state->integer_range_store = nullptr;
    state->integer_range_root = nullptr;
    state->integer_range_use_symbol_facts = false;
    core_dict_free(&state->integer_range_variables);
}

void
n00bb_range_enter_node(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                       parse_tree_t *node, parse_tree_t *parent)
{
    if (nt_is(grammar, node, "func-def")
        || nt_is(grammar, node, "context-def"))
        state->integer_symbol_ranges.len = 0;
    if (nt_is(grammar, node, "if-stmt"))
        n00bb_range_enter_if(grammar, state, node);
    if (nt_is(grammar, node, "body"))
        (void)n00bb_range_begin_branch(grammar, state, parent);
    if (!state->integer_range_store
        && nt_is(grammar, node, "expression")) {
        state->integer_range_store = core_csp_store_new();
        state->integer_range_root = state->integer_range_store ? node : nullptr;
        state->integer_range_use_symbol_facts =
            (!n00bb_range_inside_control(grammar, node)
                || n00bb_range_in_active_branch(grammar, state, node));
        if (state->integer_range_store)
            core_dict_init(&state->integer_range_variables, nullptr, nullptr);
    }
}

void
n00bb_range_after_children(parse_grammar_t *grammar,
                           n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!state->integer_range_store) return;
    n00bb_propagate_integer_range_node(grammar, state, node);
    if (n00bb_range_proven_above_int64(grammar, state, node))
        core_dict_put(&state->integer_proven_above_int64_nodes, node,
                      (void *)1);
    if (node == state->integer_range_root)
        n00bb_finish_integer_range_expression(state);
}

void
n00bb_range_after_annotation(parse_grammar_t *grammar,
                             n00bb_tu_state_t *state,
                             parse_tree_t *node, parse_tree_t *parent)
{
    if (nt_is(grammar, node, "sym-decl")
        || nt_is(grammar, node, "assign-stmt"))
        n00bb_range_update_symbol(grammar, node, state,
            nt_is(grammar, node, "sym-decl"), false);
    if (nt_is(grammar, node, "binop-assign-stmt"))
        n00bb_range_update_symbol(grammar, node, state, false, true);
    if (nt_is(grammar, node, "body"))
        (void)n00bb_range_end_branch(grammar, state, node, parent);
    if (nt_is(grammar, node, "if-stmt"))
        n00bb_range_end_if(state, node);
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    if (rule && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)) {
        n00bb_range_invalidate_call_mutations(grammar, state);
        if (!n00bb_range_inside_function(grammar, node)
            && n00bb_range_execution_phase(grammar, node)
                == PARSE_SYM_PHASE_COMPTIME)
            n00bb_range_invalidate_static_symbols(state);
    }
    if ((n00bb_range_control_node(grammar, node)
            && !nt_is(grammar, node, "if-stmt")
            && !nt_is(grammar, node, "elif-clause")
            && !nt_is(grammar, node, "else-clause"))
        || nt_is(grammar, node, "func-def")
        || nt_is(grammar, node, "context-def"))
        state->integer_symbol_ranges.len = 0;
}

