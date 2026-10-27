#include <limits.h>
#include <string.h>

#include "compiler_tu.h"
#include "compiler_link.h"
#include "ir/operands.h"

bool
n00bb_type_interval(th_type *type, ir_interval_t *range)
{
    uint64_t width = 0;
    bool is_signed = false;
    if (!type) return false;
    if (!n00bb_integer_representation(type, &width, &is_signed)) {
        if (!n00bb_scalar_role_is(type, IR_ROLE_BYTE)) return false;
        width = 8;
    }
    if (!width) return false;
    *range = ir_interval_of_width(width, is_signed);
    return true;
}

bool
n00bb_current_integer_range(parse_tree_t *node, ir_interval_t *range)
{
    n00bb_node_facts_t *facts = n00bb_node_facts(node);
    if (!facts || !facts->integer_range_known) return false;
    if (range) *range = facts->integer_range;
    return true;
}

bool
n00bb_integer_point(parse_tree_t *node, int64_t *value)
{
    ir_interval_t range = {0};
    if (!n00bb_current_integer_range(node, &range)
        || range.lo != range.hi || range.lo < INT64_MIN
        || range.lo > INT64_MAX) return false;
    if (value) *value = (int64_t)range.lo;
    return true;
}

bool
n00bb_set_integer_range(n00bb_tu_state_t *state, parse_tree_t *node,
                        ir_interval_t range)
{
    if (range.lo > range.hi) return false;
    n00bb_node_facts_t *facts = n00bb_ensure_node_facts(state, node);
    if (!facts) return false;
    facts->integer_range = range;
    facts->integer_range_known = true;
    return true;
}

enum {
    RANGE_SIMPLE_LIT = 1u << 0,
    RANGE_UNARY = 1u << 1,
    RANGE_PRIMARY = 1u << 2,
    RANGE_WRAPPER = 1u << 3,
    RANGE_ADD = 1u << 4,
    RANGE_MUL = 1u << 5,
    RANGE_SHIFT = 1u << 6,
    RANGE_BITAND = 1u << 7,
    RANGE_BITOR = 1u << 8,
    RANGE_BITXOR = 1u << 9,
    RANGE_IF = 1u << 10,
    RANGE_CLAUSE = 1u << 11,
    RANGE_BODY = 1u << 12,
    RANGE_EXPRESSION = 1u << 13,
    RANGE_SYM_DECL = 1u << 14,
    RANGE_ASSIGN = 1u << 15,
    RANGE_BINOP_ASSIGN = 1u << 16,
    RANGE_CONTROL = 1u << 17,
    RANGE_FUNCTION = 1u << 18,
};

static const struct {
    core_string_t name;
    uint32_t roles;
} range_role_names[] = {
    {CORE_STRING_STATIC("simple-lit"), RANGE_SIMPLE_LIT},
    {CORE_STRING_STATIC("expression"), RANGE_WRAPPER | RANGE_EXPRESSION},
    {CORE_STRING_STATIC("or-expr"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("and-expr"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("or-rhs"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("and-rhs"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("equality-expr"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("compare-expr"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("bitor-expr"), RANGE_WRAPPER | RANGE_BITOR},
    {CORE_STRING_STATIC("bitxor-expr"), RANGE_WRAPPER | RANGE_BITXOR},
    {CORE_STRING_STATIC("bitand-expr"), RANGE_WRAPPER | RANGE_BITAND},
    {CORE_STRING_STATIC("shift-expr"), RANGE_WRAPPER | RANGE_SHIFT},
    {CORE_STRING_STATIC("add-expr"), RANGE_WRAPPER | RANGE_ADD},
    {CORE_STRING_STATIC("mul-expr"), RANGE_WRAPPER | RANGE_MUL},
    {CORE_STRING_STATIC("unary-expr"), RANGE_WRAPPER | RANGE_UNARY},
    {CORE_STRING_STATIC("postfix-expr"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("primary"), RANGE_WRAPPER | RANGE_PRIMARY},
    {CORE_STRING_STATIC("tuple-or-paren"), RANGE_WRAPPER},
    {CORE_STRING_STATIC("if-stmt"), RANGE_IF | RANGE_CONTROL},
    {CORE_STRING_STATIC("if-value-expr"), RANGE_CONTROL},
    {CORE_STRING_STATIC("elif-clause"), RANGE_CLAUSE | RANGE_CONTROL},
    {CORE_STRING_STATIC("else-clause"), RANGE_CLAUSE | RANGE_CONTROL},
    {CORE_STRING_STATIC("while-stmt"), RANGE_CONTROL},
    {CORE_STRING_STATIC("for-stmt"), RANGE_CONTROL},
    {CORE_STRING_STATIC("labeled-while"), RANGE_CONTROL},
    {CORE_STRING_STATIC("labeled-for"), RANGE_CONTROL},
    {CORE_STRING_STATIC("switch-stmt"), RANGE_CONTROL},
    {CORE_STRING_STATIC("typecase-stmt"), RANGE_CONTROL},
    {CORE_STRING_STATIC("typecase-value-expr"), RANGE_CONTROL},
    {CORE_STRING_STATIC("try-stmt"), RANGE_CONTROL},
    {CORE_STRING_STATIC("with-stmt"), RANGE_CONTROL},
    {CORE_STRING_STATIC("body"), RANGE_BODY},
    {CORE_STRING_STATIC("sym-decl"), RANGE_SYM_DECL},
    {CORE_STRING_STATIC("assign-stmt"), RANGE_ASSIGN},
    {CORE_STRING_STATIC("binop-assign-stmt"), RANGE_BINOP_ASSIGN},
    {CORE_STRING_STATIC("func-def"), RANGE_FUNCTION},
    {CORE_STRING_STATIC("context-def"), RANGE_FUNCTION},
};

static uint32_t
range_roles(parse_grammar_t *grammar, n00bb_tu_state_t *state,
            parse_tree_t *node)
{
    if (!state->integer_range_roles_ready) {
        for (size_t i = 0; i < sizeof(range_role_names)
                                   / sizeof(range_role_names[0]); i++) {
            parse_nonterm_t *nt = parse_find_nonterm(grammar,
                                                     range_role_names[i].name);
            if (nt)
                parse_nonterm_set_user_data(nt,
                    (void *)(uintptr_t)range_role_names[i].roles);
        }
        state->integer_range_roles_ready = true;
    }
    parse_nonterm_t *nt = parse_tree_nonterm(grammar, node);
    return nt ? (uint32_t)(uintptr_t)parse_nonterm_get_user_data(nt) : 0;
}

bool
n00bb_integer_range_wrapper(parse_grammar_t *grammar, parse_tree_t *node)
{
    static const core_string_t wrappers[] = {
        CORE_STRING_STATIC("expression"), CORE_STRING_STATIC("or-expr"),
        CORE_STRING_STATIC("and-expr"), CORE_STRING_STATIC("or-rhs"),
        CORE_STRING_STATIC("and-rhs"), CORE_STRING_STATIC("equality-expr"),
        CORE_STRING_STATIC("compare-expr"), CORE_STRING_STATIC("bitor-expr"),
        CORE_STRING_STATIC("bitxor-expr"), CORE_STRING_STATIC("bitand-expr"),
        CORE_STRING_STATIC("shift-expr"), CORE_STRING_STATIC("add-expr"),
        CORE_STRING_STATIC("mul-expr"), CORE_STRING_STATIC("unary-expr"),
        CORE_STRING_STATIC("postfix-expr"), CORE_STRING_STATIC("primary"),
        CORE_STRING_STATIC("tuple-or-paren"),
    };
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++)
        if (nt_is(grammar, node, wrappers[i])) return true;
    return false;
}

static n00bb_symbol_range_t *
range_table_get(const n00bb_range_table_t *table,
                const parse_sym_entry_t *symbol)
{
    size_t slot = (uintptr_t)core_dict_get((core_dict_t *)&table->index,
                                           (void *)symbol, nullptr);
    return slot ? &table->entries.data[slot - 1] : nullptr;
}

static void
range_table_remove(n00bb_range_table_t *table,
                   const parse_sym_entry_t *symbol)
{
    size_t slot = (uintptr_t)core_dict_get(&table->index, (void *)symbol,
                                           nullptr);
    if (!slot) return;
    size_t last = table->entries.len - 1;
    if (slot - 1 != last) {
        table->entries.data[slot - 1] = table->entries.data[last];
        core_dict_put(&table->index, table->entries.data[slot - 1].symbol,
                      slot);
    }
    table->entries.len = last;
    core_dict_remove(&table->index, (void *)symbol);
}

static void
range_table_put(n00bb_range_table_t *table, parse_sym_entry_t *symbol,
                ir_interval_t value)
{
    n00bb_symbol_range_t *range = range_table_get(table, symbol);
    if (range) {
        range->range = value;
        return;
    }
    core_list_push(table->entries,
                   ((n00bb_symbol_range_t){.symbol = symbol, .range = value}));
    core_dict_put(&table->index, symbol, table->entries.len);
}

static n00bb_range_table_t
range_table_copy(const n00bb_range_table_t *source)
{
    n00bb_range_table_t copy = {.entries = core_list_new(n00bb_symbol_range_t)};
    core_dict_init(&copy.index, nullptr, nullptr);
    for (size_t i = 0; source && i < source->entries.len; i++)
        range_table_put(&copy, source->entries.data[i].symbol,
                        source->entries.data[i].range);
    return copy;
}

static void
range_table_release(n00bb_range_table_t *table)
{
    core_list_free(table->entries);
    core_dict_free(&table->index);
}

static void
range_table_clear(n00bb_range_table_t *table)
{
    table->entries.len = 0;
    core_dict_free(&table->index);
    core_dict_init(&table->index, nullptr, nullptr);
}

static const n00bb_symbol_range_t *
n00bb_range_for_symbol(const n00bb_tu_state_t *state,
                      const parse_sym_entry_t *symbol)
{
    return state && symbol
        ? range_table_get(&state->integer_symbol_ranges, symbol) : nullptr;
}

static bool
n00bb_range_symbol_invalidated(const n00bb_tu_state_t *state,
                               const parse_sym_entry_t *symbol)
{
    return state && symbol && core_dict_contains(
        (core_dict_t *)&state->integer_range_invalidated_symbols,
        (void *)symbol);
}

static void
n00bb_range_invalidate_symbol(n00bb_tu_state_t *state,
                             parse_sym_entry_t *symbol)
{
    if (!state || !symbol
        || n00bb_range_symbol_invalidated(state, symbol)) return;
    core_dict_put(&state->integer_range_invalidated_symbols, symbol, symbol);
}

static void
n00bb_range_forget_symbol(n00bb_tu_state_t *state,
                          const parse_sym_entry_t *symbol)
{
    if (state && symbol)
        range_table_remove(&state->integer_symbol_ranges, symbol);
}

static void
n00bb_range_set_symbol(n00bb_tu_state_t *state, parse_sym_entry_t *symbol,
                       ir_interval_t value)
{
    if (state && symbol)
        range_table_put(&state->integer_symbol_ranges, symbol, value);
}

static void
solved_ranges_release(core_dict_t *ranges)
{
    core_dict_foreach(ranges, entry)
        core_free(entry->value);
    core_dict_free(ranges);
}

static void
n00bb_range_merge_branch(n00bb_range_branch_frame_t *frame,
                         const n00bb_range_table_t *branch)
{
    if (!frame || !branch) return;
    if (!frame->has_branch) {
        frame->merged = range_table_copy(branch);
        frame->has_branch = true;
        return;
    }
    for (size_t i = frame->merged.entries.len; i > 0; i--) {
        n00bb_symbol_range_t *merged = &frame->merged.entries.data[i - 1];
        const n00bb_symbol_range_t *other = range_table_get(branch,
                                                            merged->symbol);
        if (other)
            merged->range = ir_interval_join(merged->range, other->range);
        else range_table_remove(&frame->merged, merged->symbol);
    }
}

static void
n00bb_range_restore(n00bb_tu_state_t *state,
                    const n00bb_range_table_t *ranges)
{
    if (!state || !ranges) return;
    range_table_release(&state->integer_symbol_ranges);
    state->integer_symbol_ranges = range_table_copy(ranges);
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
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at))
        if (nt_is(grammar, at, CORE_STRING_STATIC("if-stmt"))) return at;
    return nullptr;
}

static parse_tree_t *
n00bb_range_direct_comparison(parse_grammar_t *grammar,
                              parse_tree_t *condition)
{
    for (parse_tree_t *node = condition; node && !core_tree_is_leaf(node);) {
        if ((nt_is(grammar, node, CORE_STRING_STATIC("equality-expr"))
                || nt_is(grammar, node, CORE_STRING_STATIC("compare-expr")))
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
        if (nt_is(grammar, node, CORE_STRING_STATIC("simple-lit"))) {
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

ir_op_t
n00bb_comparison_op(core_string_t op)
{
    static const struct {
        core_string_t spelling;
        ir_op_t op;
    } ops[] = {
        {CORE_STRING_STATIC("<"), IR_OP_LT},
        {CORE_STRING_STATIC("<="), IR_OP_LE},
        {CORE_STRING_STATIC(">"), IR_OP_GT},
        {CORE_STRING_STATIC(">="), IR_OP_GE},
        {CORE_STRING_STATIC("=="), IR_OP_EQ},
        {CORE_STRING_STATIC("!="), IR_OP_NE},
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (core_string_eq(op, ops[i].spelling)) return ops[i].op;
    return IR_OP_COUNT;
}

parse_sym_entry_t *
n00bb_range_variable_of(parse_tree_t *node)
{
    parse_tree_t *lvalue = n00bb_simple_lvalue(node);
    parse_node_resolution_t resolution = lvalue
        ? parse_tree_resolution(lvalue) : (parse_node_resolution_t){0};
    parse_sym_entry_t *symbol = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    return symbol && symbol->kind == PARSE_SYM_VARIABLE ? symbol : nullptr;
}

static void
n00bb_range_refine_comparison(parse_grammar_t *grammar,
                              n00bb_tu_state_t *state,
                              parse_tree_t *condition, bool true_branch)
{
    parse_tree_t *comparison = n00bb_range_direct_comparison(grammar,
                                                              condition);
    if (!comparison) return;
    ir_op_t op = n00bb_comparison_op(first_token_text(
        core_tree_child(comparison, 1)));
    parse_tree_t *constant_node = core_tree_child(comparison, 2);
    parse_sym_entry_t *symbol = n00bb_range_variable_of(
        core_tree_child(comparison, 0));
    if (!symbol) {
        symbol = n00bb_range_variable_of(core_tree_child(comparison, 2));
        constant_node = core_tree_child(comparison, 0);
        op = ir_comparison_swapped(op);
    }
    int64_t constant = 0;
    if (!symbol || op == IR_OP_COUNT
        || !n00bb_range_simple_integer_literal(grammar, constant_node,
                                               &constant)) return;
    ir_interval_t known = {0};
    const n00bb_symbol_range_t *fact = n00bb_range_for_symbol(state, symbol);
    if (fact) known = fact->range;
    else if (!n00bb_type_interval(symbol->type, &known)) return;
    ir_interval_t refined = ir_interval_constrain(known,
        true_branch ? op : ir_comparison_negated(op),
        ir_interval_point(constant));
    if (refined.lo <= refined.hi)
        n00bb_range_set_symbol(state, symbol, refined);
}

static void
n00bb_range_enter_if(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                     parse_tree_t *node)
{
    if (!state || !nt_is(grammar, node, CORE_STRING_STATIC("if-stmt"))) return;
    n00bb_range_branch_frame_t frame = {
        .node = node,
        .incoming = range_table_copy(&state->integer_symbol_ranges),
    };
    core_list_push(state->integer_range_branches, frame);
}

static bool
n00bb_range_begin_branch(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                        parse_tree_t *parent)
{
    if (!state || !parent) return false;
    bool is_if = nt_is(grammar, parent, CORE_STRING_STATIC("if-stmt"));
    bool is_elif = nt_is(grammar, parent, CORE_STRING_STATIC("elif-clause"));
    bool is_else = nt_is(grammar, parent, CORE_STRING_STATIC("else-clause"));
    if (!is_if && !is_elif && !is_else) return false;
    parse_tree_t *owner = n00bb_range_if_owner(grammar, parent);
    n00bb_range_branch_frame_t *frame = n00bb_range_branch_top(state, owner);
    if (!frame) return false;
    n00bb_range_restore(state, &frame->incoming);
    frame->branch_active = true;
    if (is_else) {
        frame->has_else = true;
        parse_tree_t *chain = child_nt(grammar, owner,
                                       CORE_STRING_STATIC("elif-chain"));
        if (!child_nt(grammar, chain, CORE_STRING_STATIC("elif-clause")))
            n00bb_range_refine_comparison(grammar, state,
                child_nt(grammar, owner, CORE_STRING_STATIC("expression")),
                false);
    }
    else {
        n00bb_range_refine_comparison(grammar, state,
            child_nt(grammar, parent, CORE_STRING_STATIC("expression")),
            true);
    }
    return true;
}

static bool
n00bb_range_end_branch(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                      parse_tree_t *parent)
{
    if (!state || !parent) return false;
    if (!nt_is(grammar, parent, CORE_STRING_STATIC("if-stmt"))
        && !nt_is(grammar, parent, CORE_STRING_STATIC("elif-clause"))
        && !nt_is(grammar, parent, CORE_STRING_STATIC("else-clause")))
        return false;
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
    if (!frame->has_branch) frame->merged = range_table_copy(nullptr);
    range_table_release(&frame->incoming);
    range_table_release(&state->integer_symbol_ranges);
    state->integer_symbol_ranges = frame->merged;
    frame->merged = (n00bb_range_table_t){0};
    state->integer_range_branches.len--;
}

static bool
n00bb_range_in_active_branch(parse_grammar_t *grammar,
                             n00bb_tu_state_t *state, parse_tree_t *node)
{
    parse_tree_t *owner = nullptr;
    for (parse_tree_t *at = node ? parse_tree_parent(node) : nullptr;
         at; at = parse_tree_parent(at)) {
        uint32_t roles = range_roles(grammar, state, at);
        if (roles & RANGE_IF) {
            owner = at;
            break;
        }
        if ((roles & RANGE_CONTROL) && !(roles & RANGE_CLAUSE)) return false;
    }
    n00bb_range_branch_frame_t *frame = n00bb_range_branch_top(state, owner);
    return frame && frame->branch_active;
}

static bool
n00bb_range_inside_control(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                           parse_tree_t *node)
{
    for (parse_tree_t *parent = node ? parse_tree_parent(node) : nullptr;
         parent; parent = parse_tree_parent(parent))
        if (range_roles(grammar, state, parent) & RANGE_CONTROL) return true;
    return false;
}

bool
n00bb_function_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("func-def"))
        || nt_is(grammar, node, CORE_STRING_STATIC("context-def"));
}

parse_tree_t *
n00bb_enclosing_function_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at))
        if (n00bb_function_node(grammar, at)) return at;
    return nullptr;
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
    return parse_tree_sym(node);
}

void
n00bb_settle_symbol_range(n00bb_tu_state_t *state, parse_tree_t *node,
                          ir_interval_t range)
{
    parse_sym_entry_t *symbol = node ? n00bb_integer_reference_symbol(node)
                                     : nullptr;
    if (!state || !symbol || symbol->kind != PARSE_SYM_VARIABLE) return;
    n00bb_symbol_range_t *settled = core_dict_get(
        &state->integer_solved_symbols, symbol, nullptr);
    if (!settled) {
        settled = core_alloc(n00bb_symbol_range_t);
        *settled = (n00bb_symbol_range_t){.symbol = symbol, .range = range};
        core_dict_put(&state->integer_solved_symbols, symbol, settled);
        return;
    }
    settled->range = ir_interval_join(settled->range, range);
}

static bool
n00bb_range_constant_uint64(parse_grammar_t *grammar, parse_tree_t *node,
                            uint64_t *value)
{
    if (!node || core_tree_is_leaf(node)) return false;
    size_t count = core_tree_num_children(node);
    if (nt_is(grammar, node, CORE_STRING_STATIC("simple-lit"))) {
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
        parse_tree_type(node), &width, &is_signed)
        && width == 64 && !is_signed;
    if (unsigned64 && nt_is(grammar, node, CORE_STRING_STATIC("unary-expr"))
        && count == 2
        && parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("~"))
        && n00bb_range_constant_uint64(grammar, core_tree_child(node, 1),
                                       value)) {
        *value = ~*value;
        return true;
    }
    if (unsigned64 && nt_is(grammar, node, CORE_STRING_STATIC("unary-expr"))
        && count == 5
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
    ir_interval_t range = {0};
    if (n00bb_current_integer_range(node, &range))
        return range.lo > INT64_MAX;
    uint64_t constant = 0;
    if (n00bb_range_constant_uint64(grammar, node, &constant))
        return constant > INT64_MAX;
    if ((nt_is(grammar, node, CORE_STRING_STATIC("primary")) || n00bb_name_place(grammar, node))) {
        const n00bb_symbol_range_t *known = n00bb_range_for_symbol(state,
            n00bb_integer_reference_symbol(node));
        return known && known->range.lo > INT64_MAX;
    }
    if (core_tree_num_children(node) == 1)
        return n00bb_range_proven_above_int64(grammar, state,
                                              core_tree_child(node, 0));
    return false;
}

static void
n00bb_range_invalidate_static_symbols(n00bb_tu_state_t *state)
{
    if (!state) return;
    for (size_t i = 0; i < state->integer_symbol_ranges.entries.len; i++)
        n00bb_range_invalidate_symbol(state,
            state->integer_symbol_ranges.entries.data[i].symbol);
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

static bool
n00bb_range_mutable(const parse_sym_entry_t *symbol)
{
    return symbol && symbol->mutability != PARSE_SYM_IMMUTABLE
        && symbol->mutability != PARSE_SYM_CONST;
}

static void
n00bb_range_invalidate_call_mutations(n00bb_tu_state_t *state)
{
    if (!state) return;
    if (state->integer_range_root) {
        core_dict_foreach(&state->integer_range_references, entry)
            if (n00bb_range_mutable(entry->key))
                n00bb_range_invalidate_symbol(state, entry->key);
    }
    for (size_t i = state->integer_symbol_ranges.entries.len; i > 0; i--) {
        parse_sym_entry_t *symbol =
            state->integer_symbol_ranges.entries.data[i - 1].symbol;
        if (n00bb_range_mutable(symbol))
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

    parse_sym_entry_t *symbol = declaration ? parse_tree_sym(node) : nullptr;
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
    if (n00bb_range_inside_control(grammar, state, node)
        && !n00bb_range_in_active_branch(grammar, state, node)) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }

    bool in_function = n00bb_enclosing_function_node(grammar,
        parse_tree_parent(node)) != nullptr;
    if ((in_function && !scope_is_within_function(symbol->scope))
        || (!in_function && scope_is_within_function(symbol->scope))) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }
    if (symbol->kind != PARSE_SYM_VARIABLE
        || !(declaration
            ? parse_sym_can_initialize(symbol, n00bb_compile_phase())
            : parse_sym_can_write(symbol, n00bb_compile_phase()))
        || compound) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }

    parse_tree_t *value = parse_tree_resolve_child_ref(grammar, node,
                                                        assign->value_ref);
    ir_interval_t range = {0};
    if (!value || !n00bb_current_integer_range(value, &range)) {
        n00bb_range_forget_symbol(state, symbol);
        return;
    }
    n00bb_range_invalidate_symbol(state, symbol);
    n00bb_range_set_symbol(state, symbol, range);
}

static bool
child_range(parse_tree_t *node, size_t index, ir_interval_t *range)
{
    return n00bb_current_integer_range(core_tree_child(node, index), range);
}

static ir_op_t
n00bb_range_binary_op(uint32_t roles, parse_tree_t *node)
{
    if (core_tree_num_children(node) != 3) return IR_OP_COUNT;
    core_string_t op = first_token_text(core_tree_child(node, 1));
    if (roles & RANGE_ADD)
        return core_string_eq(op, CORE_STRING_STATIC("+")) ? IR_OP_ADD
            : core_string_eq(op, CORE_STRING_STATIC("-")) ? IR_OP_SUB
            : IR_OP_COUNT;
    if (roles & RANGE_MUL)
        return core_string_eq(op, CORE_STRING_STATIC("*")) ? IR_OP_MUL
            : core_string_eq(op, CORE_STRING_STATIC("/")) ? IR_OP_DIV
            : core_string_eq(op, CORE_STRING_STATIC("%")) ? IR_OP_REM
            : IR_OP_COUNT;
    if (roles & RANGE_SHIFT)
        return core_string_eq(op, CORE_STRING_STATIC("<<")) ? IR_OP_SHL
            : core_string_eq(op, CORE_STRING_STATIC(">>")) ? IR_OP_SHR
            : IR_OP_COUNT;
    if (roles & RANGE_BITAND) return IR_OP_AND;
    if (roles & RANGE_BITOR) return IR_OP_OR;
    if (roles & RANGE_BITXOR) return IR_OP_XOR;
    return IR_OP_COUNT;
}

static bool
n00bb_range_primary(n00bb_tu_state_t *state, parse_tree_t *node,
                    ir_interval_t *range)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    if (!token || parse_token_tid(token) != PARSE_TOK_IDENTIFIER)
        return false;
    parse_sym_entry_t *symbol = n00bb_integer_reference_symbol(node);
    if (!symbol) return false;
    if (state->integer_range_root)
        core_dict_put(&state->integer_range_references, symbol, symbol);
    const n00bb_symbol_range_t *known = state->integer_range_use_symbol_facts
        ? n00bb_range_for_symbol(state, symbol) : nullptr;
    if (known) {
        *range = known->range;
        return true;
    }
    n00bb_static_value_t *static_value = symbol->comptime_value;
    if (symbol->kind == PARSE_SYM_VARIABLE
        && symbol->mutability == PARSE_SYM_CONST
        && state->integer_range_use_symbol_facts
        && !n00bb_range_symbol_invalidated(state, symbol)
        && symbol->has_comptime_value && static_value
        && static_value->kind == N00BB_STATIC_VALUE_INT) {
        *range = ir_interval_point(static_value->as.integer);
        return true;
    }
    bool typed = n00bb_type_interval(parse_tree_type(node), range);
    const n00bb_symbol_range_t *settled = core_dict_get(
        &state->integer_solved_symbols, symbol, nullptr);
    if (!settled) return typed;
    if (!typed || !ir_interval_meet(*range, settled->range, range))
        *range = settled->range;
    return true;
}

static void
n00bb_propagate_integer_range_node(parse_grammar_t *grammar,
                                  n00bb_tu_state_t *state,
                                  parse_tree_t *node)
{
    if (!grammar || !state || !node || core_tree_is_leaf(node)) return;
    size_t child_count = core_tree_num_children(node);
    th_type *node_type = parse_tree_type(node);
    bool open = !node_type || n00bb_open_integer(node_type)
        || th_kind_of(th_resolve(node_type)) == TH_VAR;
    if (!open && !is_integer_type(node_type)) return;
    ir_interval_t range = {0};
    bool known = false;
    int64_t constant = 0;
    uint32_t roles = range_roles(grammar, state, node);
    ir_op_t binary = n00bb_range_binary_op(roles, node);
    if (roles & RANGE_SIMPLE_LIT) {
        ir_lit_t literal = {0};
        if (!n00bb_parse_switch_integer(first_token_text(node), &literal)
            || literal.mag > (ir_uint_t)ir_int_max) return;
        parse_tree_set_integer_literal_origin(node, node);
        range = ir_interval_point((ir_int_t)literal.mag);
        known = true;
    }
    else if (n00bb_integer_constant_reference(grammar, node, &constant)) {
        parse_tree_set_integer_literal_origin(node, node);
        range = ir_interval_point(constant);
        known = true;
    }
    else if (binary != IR_OP_COUNT) {
        ir_interval_t left = {0}, right = {0};
        known = child_range(node, 0, &left)
            && child_range(node, 2, &right);
        if (known) range = ir_interval_apply(binary, left, right);
    }
    else if ((roles & RANGE_UNARY) && child_count == 2) {
        core_string_t op = parse_tree_leaf_text(core_tree_child(node, 0));
        parse_tree_t *operand = core_tree_child(node, 1);
        bool sign = core_string_eq(op, CORE_STRING_STATIC("-"))
            || core_string_eq(op, CORE_STRING_STATIC("+"));
        if (sign && parse_tree_integer_literal_origin(operand))
            parse_tree_set_integer_literal_origin(node,
                parse_tree_integer_literal_origin(operand));
        known = sign && child_range(node, 1, &range);
        if (known && core_string_eq(op, CORE_STRING_STATIC("-")))
            range = ir_interval_apply(IR_OP_NEG, range, range);
    }
    else if ((roles & RANGE_PRIMARY) && child_count == 1
             && !n00bb_current_integer_range(core_tree_child(node, 0),
                                             nullptr))
        known = n00bb_range_primary(state, node, &range);
    else if (roles & RANGE_WRAPPER) {
        size_t ranged = 0;
        parse_tree_t *literal_child = nullptr;
        bool literal_shape = true;
        for (size_t i = 0; i < child_count; i++) {
            parse_tree_t *member = core_tree_child(node, i);
            if (core_tree_is_leaf(member)) {
                core_string_t text = parse_tree_leaf_text(member);
                if (!core_string_eq(text, CORE_STRING_STATIC("+"))
                    && !core_string_eq(text, CORE_STRING_STATIC("-"))
                    && !core_string_eq(text, CORE_STRING_STATIC("("))
                    && !core_string_eq(text, CORE_STRING_STATIC(")")))
                    literal_shape = false;
            }
            else if (parse_tree_integer_literal_origin(member)) {
                if (literal_child) literal_shape = false;
                literal_child = member;
            }
            else literal_shape = false;
            ir_interval_t child = {0};
            if (!n00bb_current_integer_range(member, &child)) continue;
            range = child;
            ranged++;
        }
        known = ranged == 1;
        if (known && literal_shape && literal_child)
            parse_tree_set_integer_literal_origin(node,
                parse_tree_integer_literal_origin(literal_child));
    }
    ir_interval_t bounds = {0};
    if (known && !open && n00bb_type_interval(node_type, &bounds)
        && !ir_interval_meet(range, bounds, &range)) return;
    if (known) (void)n00bb_set_integer_range(state, node, range);
}

void
n00bb_range_enter_node(parse_grammar_t *grammar, n00bb_tu_state_t *state,
                       parse_tree_t *node, parse_tree_t *parent)
{
    uint32_t roles = range_roles(grammar, state, node);
    if (roles & RANGE_FUNCTION)
        range_table_clear(&state->integer_symbol_ranges);
    if (roles & RANGE_IF) n00bb_range_enter_if(grammar, state, node);
    if (roles & RANGE_BODY)
        (void)n00bb_range_begin_branch(grammar, state, parent);
    if (!state->integer_range_root && (roles & RANGE_EXPRESSION)) {
        state->integer_range_root = node;
        state->integer_range_use_symbol_facts =
            !n00bb_range_inside_control(grammar, state, node)
            || n00bb_range_in_active_branch(grammar, state, node);
        core_dict_init(&state->integer_range_references, nullptr, nullptr);
    }
}

void
n00bb_range_after_children(parse_grammar_t *grammar,
                           n00bb_tu_state_t *state, parse_tree_t *node)
{
    n00bb_propagate_integer_range_node(grammar, state, node);
    if (!state->integer_range_root || node != state->integer_range_root)
        return;
    state->integer_range_root = nullptr;
    state->integer_range_use_symbol_facts = false;
    core_dict_free(&state->integer_range_references);
}

void
n00bb_range_after_annotation(parse_grammar_t *grammar,
                             n00bb_tu_state_t *state,
                             parse_tree_t *node, parse_tree_t *parent)
{
    uint32_t roles = range_roles(grammar, state, node);
    if (roles & (RANGE_SYM_DECL | RANGE_ASSIGN))
        n00bb_range_update_symbol(grammar, node, state,
                                  (roles & RANGE_SYM_DECL) != 0, false);
    if (roles & RANGE_BINOP_ASSIGN)
        n00bb_range_update_symbol(grammar, node, state, false, true);
    if (roles & RANGE_BODY)
        (void)n00bb_range_end_branch(grammar, state, parent);
    if (roles & RANGE_IF) n00bb_range_end_if(state, node);
    const parse_rule_t *rule = parse_tree_rule(grammar, node);
    if (rule && parse_rule_find_annot(grammar, rule, PARSE_ANNOT_CALL)) {
        n00bb_range_invalidate_call_mutations(state);
        if (!n00bb_enclosing_function_node(grammar, parse_tree_parent(node))
            && n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME)
            n00bb_range_invalidate_static_symbols(state);
    }
    if (((roles & RANGE_CONTROL) && !(roles & (RANGE_IF | RANGE_CLAUSE)))
        || (roles & RANGE_FUNCTION))
        range_table_clear(&state->integer_symbol_ranges);
}

void
n00bb_range_state_release(n00bb_tu_state_t *state)
{
    if (!state) return;
    range_table_release(&state->integer_symbol_ranges);
    core_dict_free(&state->integer_range_invalidated_symbols);
    solved_ranges_release(&state->integer_solved_symbols);
    core_dict_free(&state->integer_boundary_sites);
    core_dict_free(&state->integer_narrowing_sites);
    core_dict_free(&state->integer_reference_taken);
    for (size_t i = 0; i < state->integer_range_branches.len; i++) {
        range_table_release(&state->integer_range_branches.data[i].incoming);
        range_table_release(&state->integer_range_branches.data[i].merged);
    }
    core_list_free(state->integer_range_branches);
}
