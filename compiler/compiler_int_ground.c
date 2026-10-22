#include "compiler_tu.h"
#include "compiler_link.h"
#include "ir/interval.h"
#include "ir/operands.h"

enum { GROWTH_LIMIT = 8, ROUND_LIMIT = 256, NARROW_LIMIT = 4 };
enum { TRIP_NONE, TRIP_BOUNDED, TRIP_FAILED };

typedef struct {
    n00bb_integer_term_kind_t kind;
    void *key;
    th_type *type;
    parse_tree_t *site;
    core_translation_unit_t *unit;
    n00bb_tu_state_t *state;
    ir_int_t lo;
    ir_int_t hi;
    bool known;
    bool wide;
    bool frozen;
    bool fed;
    bool unbounded;
    bool grown;
    bool seeded;
    ir_int_t seed_lo;
    ir_int_t seed_hi;
    uint32_t growth;
} range_node_t;
core_list_decl(range_node_t);

typedef struct {
    core_dict_t index;
    core_list_t(range_node_t) nodes;
} range_graph_t;

enum { INCREMENT_UNKNOWN, INCREMENT_NONE, INCREMENT_LEFT, INCREMENT_RIGHT };

typedef struct {
    n00bb_integer_edge_t *edge;
    n00bb_tu_state_t *state;
    range_node_t *result;
    range_node_t *left;
    range_node_t *right;
    uint8_t increment;
} flow_edge_t;
core_list_decl(flow_edge_t);

typedef struct {
    uint32_t node;
    uint32_t edge;
} flow_read_t;
core_list_decl(flow_read_t);

typedef struct {
    core_list_t(flow_edge_t) edges;
    uint32_t *reader_start;
    uint32_t *readers;
} flow_t;

static th_integer_class
integer_class_of(th_ctx *ctx, th_type *type)
{
    (void)ctx;
    uint64_t width = 0;
    bool is_signed = false;
    if (n00bb_integer_representation(type, &width, &is_signed))
        return is_signed ? TH_INTEGER_SIGNED : TH_INTEGER_UNSIGNED_TYPE;
    return n00bb_scalar_role_is(type, IR_ROLE_BYTE)
        ? TH_INTEGER_UNSIGNED_TYPE : TH_INTEGER_NONE;
}

static bool
integer_method_result(th_ctx *ctx, th_type *receiver,
                      const th_constraint *method, void *user)
{
    n00bb_tu_state_t *state = user;
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(method->method.name);
    th_type *call = method->method.call;
    th_type *result = th_resolve(method->method.result);
    const th_param *argument = call && th_func_nparams(call) == 1
        ? th_func_param(call, 0) : nullptr;
    if (!state || state->types != ctx || !operator || !argument || !result
        || th_kind_of(result) != TH_VAR || n00bb_open_integer(result))
        return true;
    bool bitwise = operator->unsigned_only || operator->shift;
    th_type *class = n00bb_integer_variable(state,
        bitwise ? TH_INTEGER_UNSIGNED : 0);
    if (!class || !th_unify_in_place(ctx, result, class)) return false;
    if (bitwise && (!n00bb_require_unsigned_integer(state, receiver)
            || (n00bb_open_integer(argument->type)
                && !n00bb_require_unsigned_integer(state, argument->type))))
        return false;
    n00bb_integer_flow(state, nullptr, operator->op,
        (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, class, nullptr},
        (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, receiver, nullptr},
        (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, argument->type,
                               nullptr});
    return true;
}

void
n00bb_register_integer_class(n00bb_tu_state_t *state)
{
    th_register_integer_class(state->types, integer_class_of);
    th_register_integer_method(state->types, integer_method_result, state);
}

bool
n00bb_open_integer(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    return type && th_kind_of(type) == TH_VAR
        && th_var_is_integer(type, nullptr);
}

bool
n00bb_require_unsigned_integer(n00bb_tu_state_t *state, th_type *type)
{
    return th_var_constrain(state->types, th_resolve(type),
        (th_constraint){.kind = TH_CON_INTEGER,
            .integer = {.flags = TH_INTEGER_UNSIGNED}});
}

th_type *
n00bb_integer_variable(n00bb_tu_state_t *state, uint8_t flags)
{
    th_type *type = th_var(state->types);
    return type && th_var_constrain(state->types, type,
        (th_constraint){.kind = TH_CON_INTEGER, .integer = {.flags = flags}})
        ? type : nullptr;
}

th_type *
n00bb_integer_literal_variable(n00bb_tu_state_t *state, parse_tree_t *site,
                               ir_lit_t literal)
{
    th_type *type = n00bb_integer_variable(state, 0);
    if (!type) return nullptr;
    bool wide = !literal.neg && literal.mag > (ir_uint_t)ir_int_max;
    ir_int_t value = wide ? ir_int_max
        : literal.neg ? -(ir_int_t)(literal.mag - 1) - 1
        : (ir_int_t)literal.mag;
    core_list_push(state->integer_seeds, ((n00bb_integer_seed_t){
        .site = site, .type = type, .lo = value, .hi = value, .wide = wide,
    }));
    return type;
}

static parse_sym_entry_t *
declared_integer_symbol(parse_tree_t *node)
{
    parse_sym_entry_t *symbol = n00bb_range_variable_of(node);
    return symbol && is_integer_type(symbol->type) ? symbol : nullptr;
}

static parse_tree_t *
unwrapped_once(parse_tree_t *node)
{
    size_t count = node && !core_tree_is_leaf(node)
        ? core_tree_num_children(node) : 0;
    return count == 1 ? core_tree_child(node, 0)
        : count == 3 && parse_tree_leaf_is(core_tree_child(node, 0),
                                           CORE_STRING_STATIC("("))
        ? core_tree_child(node, 1) : nullptr;
}

n00bb_integer_term_t
n00bb_integer_term_of(n00bb_tu_state_t *state, parse_tree_t *node)
{
    th_type *type = node ? parse_tree_type(node) : nullptr;
    if (n00bb_open_integer(type))
        return (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, type, nullptr};
    parse_sym_entry_t *symbol = declared_integer_symbol(node);
    if (symbol)
        return (n00bb_integer_term_t){N00BB_INTEGER_TERM_SYMBOL,
                                      symbol->type, symbol};
    for (parse_tree_t *at = node; at && !core_tree_is_leaf(at);
         at = unwrapped_once(at))
        if (core_dict_contains(&state->integer_range_nodes, at))
            return (n00bb_integer_term_t){N00BB_INTEGER_TERM_NODE,
                                          parse_tree_type(at), at};
    return (n00bb_integer_term_t){
        type ? N00BB_INTEGER_TERM_TYPE : N00BB_INTEGER_TERM_NONE, type,
        nullptr};
}

static parse_tree_t *
guard_unwrap(parse_tree_t *node)
{
    for (parse_tree_t *next = unwrapped_once(node); next;
         next = unwrapped_once(node))
        node = next;
    return node;
}

static ir_op_t
guard_comparison(parse_tree_t *operator, bool flipped)
{
    ir_op_t op = operator ? n00bb_comparison_op(first_token_text(operator))
                          : IR_OP_COUNT;
    if (op == IR_OP_EQ || op == IR_OP_NE) return IR_OP_COUNT;
    return flipped ? ir_comparison_swapped(op) : op;
}

static bool
guard_symbol(parse_sym_entry_t *symbol)
{
    return symbol && symbol->scope && scope_is_within_function(symbol->scope);
}

static void
add_guard(n00bb_tu_state_t *state, n00bb_integer_edge_t *edge,
          parse_sym_entry_t *symbol, ir_op_t op, parse_tree_t *bound)
{
    if (!guard_symbol(symbol) || op == IR_OP_COUNT
        || (symbol != edge->left_symbol && symbol != edge->right_symbol))
        return;
    n00bb_integer_term_t term = n00bb_integer_term_of(state, bound);
    if (term.kind == N00BB_INTEGER_TERM_NONE) return;
    if (!edge->guard_count)
        edge->guard_start = (uint32_t)state->integer_guards.len;
    core_list_push(state->integer_guards, ((n00bb_integer_guard_t){
        .symbol = symbol, .bound = term, .op = op,
    }));
    edge->guard_count++;
}

typedef struct {
    parse_sym_entry_t *symbol;
    parse_tree_t *bound;
    ir_op_t op;
} guard_candidate_t;
core_list_decl(guard_candidate_t);

static void
collect_guards(parse_tree_t *condition,
               core_list_t(guard_candidate_t) *found)
{
    condition = guard_unwrap(condition);
    if (!condition || core_tree_is_leaf(condition)
        || core_tree_num_children(condition) != 3) return;
    parse_tree_t *left = core_tree_child(condition, 0);
    parse_tree_t *middle = core_tree_child(condition, 1);
    parse_tree_t *right = core_tree_child(condition, 2);
    if (parse_tree_leaf_is(middle, CORE_STRING_STATIC("and"))) {
        collect_guards(left, found);
        collect_guards(right, found);
        return;
    }
    if (core_tree_is_leaf(middle)) return;
    parse_sym_entry_t *left_symbol = simple_expression_symbol(left);
    parse_sym_entry_t *right_symbol = simple_expression_symbol(right);
    ir_op_t forward = guard_comparison(middle, false);
    ir_op_t backward = guard_comparison(middle, true);
    if (left_symbol && forward != IR_OP_COUNT)
        core_list_push(*found, ((guard_candidate_t){
            .symbol = left_symbol, .bound = right, .op = forward}));
    if (right_symbol && backward != IR_OP_COUNT)
        core_list_push(*found, ((guard_candidate_t){
            .symbol = right_symbol, .bound = left, .op = backward}));
}

static bool
argument_writes(parse_tree_t *arguments, parse_sym_entry_t *symbol)
{
    for (size_t i = 0; i < core_tree_num_children(arguments); i++) {
        parse_tree_t *group = core_tree_child(arguments, i);
        for (size_t a = 0; !core_tree_is_leaf(group)
             && a < core_tree_num_children(group); a++) {
            parse_tree_t *argument = core_tree_child(group, a);
            if (!core_tree_is_leaf(argument)
                && core_tree_num_children(argument)
                && simple_expression_symbol(core_tree_child(argument,
                    core_tree_num_children(argument) - 1)) == symbol)
                return true;
            if (simple_expression_symbol(argument) == symbol) return true;
        }
    }
    return false;
}

static bool
assigned_in(parse_grammar_t *grammar, parse_tree_t *node,
            parse_sym_entry_t *symbol)
{
    if (!node || core_tree_is_leaf(node)) return false;
    if ((nt_is(grammar, node, CORE_STRING_STATIC("assign-stmt"))
            || nt_is(grammar, node, CORE_STRING_STATIC("binop-assign-stmt")))
        && simple_expression_symbol(core_tree_child(node, 0)) == symbol)
        return true;
    if (nt_is(grammar, node, CORE_STRING_STATIC("sym-decl"))
        && parse_tree_sym(node) == symbol)
        return true;
    if (nt_is(grammar, node, CORE_STRING_STATIC("call-args"))
        && argument_writes(node, symbol))
        return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (assigned_in(grammar, core_tree_child(node, i), symbol))
            return true;
    return false;
}

static bool
assigns_before(parse_grammar_t *grammar, parse_tree_t *scope,
               parse_tree_t *site, parse_sym_entry_t *symbol)
{
    for (parse_tree_t *on_path = site; on_path && on_path != scope;
         on_path = parse_tree_parent(on_path)) {
        parse_tree_t *parent = parse_tree_parent(on_path);
        for (size_t i = 0; parent && i < core_tree_num_children(parent)
             && core_tree_child(parent, i) != on_path; i++)
            if (assigned_in(grammar, core_tree_child(parent, i), symbol))
                return true;
    }
    return false;
}

static void
note_references(n00bb_tu_state_t *state, parse_tree_t *node)
{
    if (!node || core_tree_is_leaf(node)) return;
    parse_sym_entry_t *symbol = core_tree_num_children(node) == 2
            && parse_tree_leaf_is(core_tree_child(node, 0),
                                  CORE_STRING_STATIC("ref"))
        ? simple_expression_symbol(core_tree_child(node, 1)) : nullptr;
    if (symbol) core_dict_put(&state->integer_reference_taken, symbol, 2);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        note_references(state, core_tree_child(node, i));
}

static bool
reference_taken_cached(n00bb_tu_state_t *state, parse_tree_t *function,
                       parse_sym_entry_t *symbol)
{
    if (core_dict_add(&state->integer_reference_taken, function, 1))
        note_references(state, function);
    return (uintptr_t)core_dict_get(&state->integer_reference_taken, symbol,
                                    nullptr) == 2;
}

static void
site_guards(n00bb_tu_state_t *state, parse_tree_t *site,
            core_list_t(guard_candidate_t) *valid)
{
    parse_grammar_t *grammar = state->grammar;
    parse_tree_t *function = n00bb_enclosing_function_node(grammar, site);
    core_list_t(guard_candidate_t) found = core_list_new(guard_candidate_t);
    for (parse_tree_t *at = site, *parent = at ? parse_tree_parent(at)
                                               : nullptr;
         parent; at = parent, parent = parse_tree_parent(parent)) {
        bool loop = parse_tree_leaf_is(core_tree_child(parent, 0),
                                       CORE_STRING_STATIC("while"));
        bool branch = parse_tree_leaf_is(core_tree_child(parent, 0),
                                         CORE_STRING_STATIC("if"));
        if ((loop || branch) && core_tree_num_children(parent) >= 3
            && core_tree_child(parent, 2) == at) {
            found.len = 0;
            collect_guards(core_tree_child(parent, 1), &found);
            for (size_t g = 0; g < found.len; g++)
                if (guard_symbol(found.data[g].symbol) && function
                    && !reference_taken_cached(state, function,
                                               found.data[g].symbol)
                    && !assigns_before(grammar, at, site,
                                       found.data[g].symbol))
                    core_list_push(*valid, found.data[g]);
        }
        if (loop || nt_is(grammar, parent, CORE_STRING_STATIC("for-stmt"))
            || n00bb_function_node(grammar, parent))
            break;
    }
    core_list_free(found);
}

static void
guard_last_edge(n00bb_tu_state_t *state, parse_tree_t *left,
                parse_tree_t *right)
{
    parse_grammar_t *grammar = state->grammar;
    if (!grammar || !state->integer_edges.len) return;
    n00bb_integer_edge_t *edge =
        &state->integer_edges.data[state->integer_edges.len - 1];
    edge->left_symbol = left ? simple_expression_symbol(left) : nullptr;
    edge->right_symbol = right ? simple_expression_symbol(right) : nullptr;
    if ((!guard_symbol(edge->left_symbol)
            && !guard_symbol(edge->right_symbol)) || !edge->site) return;
    core_list_t(guard_candidate_t) valid = core_list_new(guard_candidate_t);
    site_guards(state, edge->site, &valid);
    for (size_t g = 0; g < valid.len; g++)
        add_guard(state, edge, valid.data[g].symbol, valid.data[g].op,
                  valid.data[g].bound);
    core_list_free(valid);
}

static ir_interval_t
guard_limit(ir_op_t op, ir_interval_t bound)
{
    ir_interval_t limit = {ir_int_min, ir_int_max};
    if (op == IR_OP_LT) limit.hi = bound.hi - 1;
    else if (op == IR_OP_LE) limit.hi = bound.hi;
    else if (op == IR_OP_GT) limit.lo = bound.lo + 1;
    else if (op == IR_OP_GE) limit.lo = bound.lo;
    return limit;
}

static void
guard_site_range(n00bb_tu_state_t *state, parse_tree_t *site,
                 ir_interval_t *range)
{
    parse_grammar_t *grammar = state->grammar;
    parse_sym_entry_t *symbol = simple_expression_symbol(site);
    if (!grammar || !guard_symbol(symbol)) return;
    core_list_t(guard_candidate_t) valid = core_list_new(guard_candidate_t);
    site_guards(state, site, &valid);
    for (size_t g = 0; g < valid.len; g++) {
        ir_interval_t bound = {0}, narrowed = {0};
        if (valid.data[g].symbol == symbol
            && n00bb_current_integer_range(valid.data[g].bound, &bound)
            && ir_interval_meet(*range,
                   guard_limit(valid.data[g].op, bound), &narrowed))
            *range = narrowed;
    }
    core_list_free(valid);
}

static bool
loop_node(parse_grammar_t *grammar, parse_tree_t *node)
{
    return parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("while"))
        || nt_is(grammar, node, CORE_STRING_STATIC("for-stmt"));
}

static bool
range_loop(parse_grammar_t *grammar, parse_tree_t *node)
{
    return nt_is(grammar, node, CORE_STRING_STATIC("for-stmt"))
        && core_tree_num_children(node) == 5
        && parse_tree_leaf_is(core_tree_child(node, 2),
                              CORE_STRING_STATIC("from"));
}

static parse_tree_t *
bounded_loop(parse_grammar_t *grammar, parse_tree_t *site)
{
    if (!grammar || !site) return nullptr;
    parse_tree_t *found = nullptr;
    for (parse_tree_t *at = site, *parent = parse_tree_parent(at); parent;
         at = parent, parent = parse_tree_parent(parent)) {
        if (n00bb_function_node(grammar, parent)) return found;
        if (!loop_node(grammar, parent)) continue;
        if (found || !range_loop(grammar, parent)
            || core_tree_child(parent, 4) != at)
            return nullptr;
        found = parent;
    }
    return found;
}

bool
n00bb_integer_flow(n00bb_tu_state_t *state, parse_tree_t *site, ir_op_t op,
                   n00bb_integer_term_t result, n00bb_integer_term_t left,
                   n00bb_integer_term_t right)
{
    if (result.kind == N00BB_INTEGER_TERM_NONE
        || left.kind == N00BB_INTEGER_TERM_NONE) return false;
    core_list_push(state->integer_edges, ((n00bb_integer_edge_t){
        .site = site, .result = result, .left = left, .right = right,
        .loop = bounded_loop(state->grammar, site), .op = op,
    }));
    return true;
}

th_type *
n00bb_integer_result_variable(n00bb_tu_state_t *state, parse_tree_t *site,
                              ir_op_t op, parse_tree_t *left,
                              parse_tree_t *right)
{
    uint8_t flags = 0, other = 0;
    bool bitwise = op == IR_OP_AND || op == IR_OP_OR || op == IR_OP_XOR;
    bool shift = op == IR_OP_SHL || op == IR_OP_SHR;
    if (bitwise || shift)
        (void)th_var_is_integer(th_resolve(parse_tree_type(left)), &flags);
    if (bitwise && right)
        (void)th_var_is_integer(th_resolve(parse_tree_type(right)), &other);
    th_type *type = n00bb_integer_variable(state, (uint8_t)(flags | other));
    if (!type) return nullptr;
    if (n00bb_integer_flow(state, site, op,
            (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, type, nullptr},
            n00bb_integer_term_of(state, left),
            right ? n00bb_integer_term_of(state, right)
                  : (n00bb_integer_term_t){0}))
        guard_last_edge(state, left, right);
    core_dict_put(&state->integer_edge_sites, site, site);
    return type;
}

void
n00bb_integer_node_result(n00bb_tu_state_t *state, parse_tree_t *site,
                          ir_op_t op, parse_tree_t *left, parse_tree_t *right)
{
    if (core_dict_contains(&state->integer_range_nodes, site)) return;
    bool flowed = n00bb_integer_flow(state, site, op,
        (n00bb_integer_term_t){N00BB_INTEGER_TERM_NODE,
                               parse_tree_type(site), site},
        n00bb_integer_term_of(state, left),
        right ? n00bb_integer_term_of(state, right)
              : (n00bb_integer_term_t){0});
    core_dict_put(&state->integer_range_nodes, site,
                  flowed ? state->integer_edges.len : 0);
    if (flowed) guard_last_edge(state, left, right);
}

void
n00bb_integer_growth(n00bb_tu_state_t *state, parse_tree_t *site, ir_op_t op,
                     parse_tree_t *target, parse_tree_t *value)
{
    n00bb_integer_term_t term = n00bb_integer_term_of(state, target);
    if (n00bb_integer_flow(state, site, op, term, term,
                           n00bb_integer_term_of(state, value)))
        guard_last_edge(state, target, value);
}

static bool
same_term(n00bb_integer_term_t left, n00bb_integer_term_t right)
{
    return left.kind != N00BB_INTEGER_TERM_NONE && left.kind == right.kind
        && left.key == right.key && left.type == right.type;
}

static const n00bb_integer_edge_t *
value_edge(n00bb_tu_state_t *state, parse_tree_t *value)
{
    size_t slot = (uintptr_t)core_dict_get(&state->integer_range_nodes, value,
                                           nullptr);
    return slot ? &state->integer_edges.data[slot - 1] : nullptr;
}

void
n00bb_integer_assign(n00bb_tu_state_t *state, parse_tree_t *site,
                     n00bb_integer_term_t written, parse_tree_t *target,
                     parse_tree_t *value)
{
    n00bb_integer_term_t computed = n00bb_integer_term_of(state, value);
    parse_tree_t *binary = computed.kind == N00BB_INTEGER_TERM_NODE
        ? computed.key : nullptr;
    const n00bb_integer_edge_t *produced = binary
        ? value_edge(state, binary) : nullptr;
    bool additive = produced && (produced->op == IR_OP_ADD
                                 || produced->op == IR_OP_SUB)
        && core_tree_num_children(binary) == 3;
    parse_tree_t *left = additive ? core_tree_child(binary, 0) : nullptr;
    parse_tree_t *right = additive ? core_tree_child(binary, 2) : nullptr;
    parse_tree_t *other = nullptr;
    if (additive && same_term(n00bb_integer_term_of(state, left), written))
        other = right;
    else if (additive && produced->op == IR_OP_ADD
             && same_term(n00bb_integer_term_of(state, right), written))
        other = left;
    if (other && n00bb_integer_flow(state, site, produced->op, written,
                                    written,
                                    n00bb_integer_term_of(state, other))) {
        guard_last_edge(state, target, other);
        return;
    }
    n00bb_integer_copy(state, site, written, value);
}

void
n00bb_integer_copy(n00bb_tu_state_t *state, parse_tree_t *site,
                   n00bb_integer_term_t target, parse_tree_t *value)
{
    if (n00bb_integer_flow(state, site, IR_OP_COPY, target,
                           n00bb_integer_term_of(state, value),
                           (n00bb_integer_term_t){0}))
        guard_last_edge(state, value, nullptr);
}

static th_type *
best_fit(th_ctx *types, const range_node_t *class, bool unsigned_only)
{
    bool is_signed = class->lo < 0 && !class->wide;
    if (is_signed && unsigned_only) return nullptr;
    for (size_t i = 0; i < N00BB_INTEGER_WIDTHS; i++) {
        uint64_t width = (uint64_t)8 << i;
        if (!class->wide && !ir_interval_fits(
                (ir_interval_t){class->lo, class->hi}, width, is_signed))
            continue;
        if (class->wide && width < 128) continue;
        return n00bb_integer_type_of_width(types, width, is_signed);
    }
    return nullptr;
}

static th_type *
allowed_fit(th_type *variable, const range_node_t *class)
{
    th_type *fitting = nullptr, *widest = nullptr;
    ir_interval_t fitting_bounds = {0}, widest_bounds = {0};
    ir_interval_t range = {class->lo, class->hi};
    for (size_t i = 0; i < th_var_constraint_count(variable); i++) {
        th_constraint constraint;
        if (!th_var_constraint_at(variable, i, &constraint)
            || constraint.kind != TH_CON_ONE_OF) continue;
        for (size_t t = 0; t < constraint.one_of.n; t++) {
            th_type *candidate = constraint.one_of.types[t];
            ir_interval_t bounds = {0};
            if (!n00bb_type_interval(candidate, &bounds)) continue;
            bool fits = !class->wide && ir_interval_meet(range, bounds,
                                                         nullptr)
                && bounds.lo <= range.lo && range.hi <= bounds.hi;
            if (fits && (!fitting || bounds.hi - bounds.lo
                                     < fitting_bounds.hi - fitting_bounds.lo)) {
                fitting = candidate;
                fitting_bounds = bounds;
            }
            bool sign_matches = (range.lo < 0) == (bounds.lo < 0);
            if (sign_matches && (!widest || bounds.hi - bounds.lo
                                 > widest_bounds.hi - widest_bounds.lo)) {
                widest = candidate;
                widest_bounds = bounds;
            }
        }
    }
    return fitting ? fitting : widest;
}

static void
report(const range_node_t *class, core_string_t message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(class->unit),
        class->unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
        message, link_source_location(class->site));
}

static bool
check_seed_fits(n00bb_tu_state_t *state, const n00bb_integer_seed_t *seed,
                th_type *type)
{
    uint64_t width = 0;
    bool is_signed = false;
    if (!n00bb_integer_representation(type, &width, &is_signed)) {
        if (!n00bb_scalar_role_is(type, IR_ROLE_BYTE)) return true;
        width = 8;
    }
    if (!width || (seed->wide ? width >= 128 && !is_signed
                              : ir_interval_fits((ir_interval_t){seed->lo,
                                    seed->hi}, width, is_signed)))
        return true;
    n00bb_report_integer_misfit(state, seed->site,
        CORE_STRING_STATIC("constant"), ir_interval_point(seed->lo), type);
    return false;
}

static void
collect_interface_integers(n00bb_tu_state_t *state, th_type *type,
                           core_dict_t *interface)
{
    th_type **vars = nullptr;
    size_t count = type ? th_free_vars(state->types, type, &vars) : 0;
    for (size_t i = 0; i < count; i++)
        if (n00bb_open_integer(vars[i]))
            core_dict_put(interface, th_resolve(vars[i]), vars[i]);
}

static void
collect_interfaces(core_translation_unit_t *const *units, size_t unit_count,
                   core_dict_t *interface)
{
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state || !state->annotations || !state->annotations->symtab)
            continue;
        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);
        for (size_t s = 0; s < scopes.len; s++)
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                collect_interface_integers(state, symbol->type, interface);
                collect_interface_integers(state, parse_sym_binding(symbol,
                    CORE_STRING_STATIC("n00bb.body-function-signature")),
                    interface);
            }
        core_list_free(scopes);
    }
}

static void *
term_key(n00bb_integer_term_t term)
{
    if (term.kind == N00BB_INTEGER_TERM_TYPE)
        return n00bb_open_integer(term.type) ? th_resolve(term.type)
                                             : nullptr;
    return term.kind == N00BB_INTEGER_TERM_NONE ? nullptr : term.key;
}

static range_node_t *
find_node(range_graph_t *graph, n00bb_integer_term_t term)
{
    void *key = term_key(term);
    uintptr_t slot = key ? (uintptr_t)core_dict_get(&graph->index, key,
                                                    nullptr)
                         : 0;
    return slot ? &graph->nodes.data[slot - 1] : nullptr;
}

static range_node_t *
node_for(range_graph_t *graph, n00bb_integer_term_t term,
         core_translation_unit_t *unit, n00bb_tu_state_t *state,
         parse_tree_t *site)
{
    range_node_t *found = find_node(graph, term);
    void *key = term_key(term);
    if (found || !key) return found;
    core_list_push(graph->nodes, ((range_node_t){
        .kind = term.kind, .key = key,
        .type = term.kind == N00BB_INTEGER_TERM_TYPE ? th_resolve(term.type)
                                                     : term.type,
        .site = site, .unit = unit, .state = state,
    }));
    core_dict_put(&graph->index, key, (void *)(uintptr_t)graph->nodes.len);
    return &graph->nodes.data[graph->nodes.len - 1];
}

static th_type *
seed_type(const n00bb_integer_seed_t *seed)
{
    th_type *current = seed->site ? parse_tree_type(seed->site) : nullptr;
    if (current && th_kind_of(th_resolve(current)) == TH_VAR
        && !n00bb_open_integer(current))
        current = nullptr;
    return current ? current : seed->type;
}

static bool
node_term_interval(range_node_t *node, n00bb_integer_term_t term,
                   ir_interval_t *out, bool *unbounded)
{
    if (node) {
        *out = (ir_interval_t){node->lo, node->hi};
        *unbounded = *unbounded || node->unbounded;
        return node->known;
    }
    *unbounded = true;
    if (n00bb_type_interval(term.type ? th_resolve(term.type) : nullptr, out))
        return true;
    if (term.type && th_kind_of(th_resolve(term.type)) == TH_VAR)
        return false;
    *out = (ir_interval_t){INT64_MIN, INT64_MAX};
    return term.kind != N00BB_INTEGER_TERM_NONE;
}

static bool
term_interval(range_graph_t *graph, n00bb_integer_term_t term,
              ir_interval_t *out, bool *unbounded)
{
    return node_term_interval(find_node(graph, term), term, out, unbounded);
}

static bool
widen(range_node_t *node, ir_interval_t range, bool unbounded)
{
    if (node->frozen) return false;
    bool flagged = unbounded && !node->unbounded;
    node->unbounded = node->unbounded || unbounded;
    ir_interval_t bounds = {0};
    if (node->kind != N00BB_INTEGER_TERM_TYPE
        && n00bb_type_interval(node->type, &bounds)
        && !ir_interval_meet(range, bounds, &range)) return false;
    if (!node->known) {
        node->lo = range.lo;
        node->hi = range.hi;
        node->known = true;
        return true;
    }
    if (range.lo >= node->lo && range.hi <= node->hi) return flagged;
    if (++node->growth > GROWTH_LIMIT) {
        bool negative = node->lo < 0 || range.lo < 0;
        ir_interval_t grown = {
            range.lo < node->lo ? (negative ? INT64_MIN : 0) : node->lo,
            range.hi > node->hi
                ? (negative ? INT64_MAX : (ir_int_t)UINT64_MAX) : node->hi};
        if (node->kind != N00BB_INTEGER_TERM_TYPE
            && n00bb_type_interval(node->type, &bounds))
            (void)ir_interval_meet(grown, bounds, &grown);
        node->lo = grown.lo;
        node->hi = grown.hi;
        node->frozen = true;
        node->unbounded = true;
        node->grown = true;
        return true;
    }
    if (range.lo < node->lo) node->lo = range.lo;
    if (range.hi > node->hi) node->hi = range.hi;
    return true;
}

static const n00bb_integer_guard_t *
edge_guard(const n00bb_tu_state_t *state, const n00bb_integer_edge_t *edge,
           uint32_t index)
{
    return &state->integer_guards.data[edge->guard_start + index];
}

static bool
guarded(range_graph_t *graph, const flow_edge_t *flow,
        parse_sym_entry_t *symbol, ir_interval_t *range)
{
    if (!symbol) return true;
    const n00bb_integer_edge_t *edge = flow->edge;
    for (uint32_t g = 0; g < edge->guard_count; g++) {
        const n00bb_integer_guard_t *guard = edge_guard(flow->state, edge, g);
        bool unbounded = false;
        ir_interval_t bound = {0};
        if (guard->symbol != symbol
            || !term_interval(graph, guard->bound, &bound, &unbounded))
            continue;
        if (!ir_interval_meet(*range, guard_limit(guard->op, bound), range))
            return false;
    }
    return true;
}

static ir_interval_t
unbounded_cap(ir_interval_t out, ir_interval_t left, ir_interval_t right)
{
    bool negative = out.lo < 0;
    ir_interval_t cap = ir_interval_join(ir_interval_join(left, right),
        (ir_interval_t){negative ? INT64_MIN : 0,
                        negative ? INT64_MAX : (ir_int_t)UINT64_MAX});
    return ir_interval_meet(out, cap, &out) ? out : cap;
}

static bool
edge_output(range_graph_t *graph, const flow_edge_t *flow,
            ir_interval_t *out, bool *unbounded)
{
    const n00bb_integer_edge_t *edge = flow->edge;
    bool binary = edge->right.kind != N00BB_INTEGER_TERM_NONE;
    ir_interval_t left = {0}, right = {0};
    if (!node_term_interval(flow->left, edge->left, &left, unbounded)
        || !guarded(graph, flow, edge->left_symbol, &left))
        return false;
    if (!binary) right = left;
    else if (!node_term_interval(flow->right, edge->right, &right, unbounded)
             || !guarded(graph, flow, edge->right_symbol, &right))
        return false;
    *out = edge->op == IR_OP_COPY ? left
        : ir_interval_apply(edge->op, left, right);
    if (*unbounded && edge->op != IR_OP_COPY && edge->op != IR_OP_MUL)
        *out = unbounded_cap(*out, left, right);
    return true;
}

static bool
node_interval(range_graph_t *graph, n00bb_tu_state_t *state,
              parse_tree_t *node, ir_interval_t *out)
{
    if (node && n00bb_current_integer_range(node, out)) return true;
    bool unbounded = false;
    n00bb_integer_term_t term = node ? n00bb_integer_term_of(state, node)
                                     : (n00bb_integer_term_t){0};
    return term.kind != N00BB_INTEGER_TERM_NONE
        && term_interval(graph, term, out, &unbounded);
}

static bool
trip_count(range_graph_t *graph, n00bb_tu_state_t *state,
           parse_tree_t *loop, ir_int_t *count)
{
    parse_tree_t *range = core_tree_child(loop, 3);
    parse_tree_t *parts = range ? core_tree_child(range, 0) : nullptr;
    if (!parts || core_tree_is_leaf(parts)
        || core_tree_num_children(parts) != 3) return false;
    ir_interval_t start = {0}, end = {0}, step = {1, 1};
    if (!node_interval(graph, state, core_tree_child(parts, 0), &start)
        || !node_interval(graph, state, core_tree_child(parts, 2), &end))
        return false;
    parse_tree_t *clause = core_tree_num_children(range) == 2
        ? core_tree_child(range, 1) : nullptr;
    while (clause && !core_tree_is_leaf(clause)
           && core_tree_num_children(clause) == 1)
        clause = core_tree_child(clause, 0);
    if (clause && !core_tree_is_leaf(clause)
        && core_tree_num_children(clause) != 0
        && (core_tree_num_children(clause) != 2
            || !node_interval(graph, state, core_tree_child(clause, 1),
                              &step))) return false;
    if (step.lo <= 0 && step.hi >= 0) return false;
    ir_int_t smallest = step.lo > 0 ? step.lo : -step.hi;
    ir_interval_t forward = ir_interval_apply(IR_OP_SUB, end, start);
    ir_interval_t backward = ir_interval_apply(IR_OP_SUB, start, end);
    ir_int_t span = forward.hi > backward.hi ? forward.hi : backward.hi;
    if (span < 0) span = 0;
    if (span == ir_int_max) return false;
    *count = span / smallest + 1;
    return true;
}

static bool
inside_loop(parse_grammar_t *grammar, parse_tree_t *site)
{
    for (parse_tree_t *at = parse_tree_parent(site); at;
         at = parse_tree_parent(at)) {
        if (n00bb_function_node(grammar, at)) return false;
        if (loop_node(grammar, at)) return true;
    }
    return false;
}

static bool
activation_local(n00bb_tu_state_t *state, parse_tree_t *site,
                 parse_sym_entry_t *symbol)
{
    parse_tree_t *function = n00bb_enclosing_function_node(state->grammar, site);
    return symbol && function && symbol->decl_node
        && n00bb_enclosing_function_node(state->grammar, symbol->decl_node) == function
        && !reference_taken_cached(state, function, symbol);
}

static uint8_t
increment_side(const flow_edge_t *flow)
{
    const n00bb_integer_edge_t *edge = flow->edge;
    bool add = edge->op == IR_OP_ADD;
    bool from_left = flow->left == flow->result
        && flow->right != flow->result && (add || edge->op == IR_OP_SUB);
    bool from_right = add && flow->right == flow->result
        && flow->left != flow->result;
    if ((!from_left && !from_right)
        || !activation_local(flow->state, edge->site,
                             from_left ? edge->left_symbol
                                       : edge->right_symbol)
        || (!edge->loop && inside_loop(flow->state->grammar, edge->site)))
        return INCREMENT_NONE;
    return from_left ? INCREMENT_LEFT : INCREMENT_RIGHT;
}

static bool
self_increment(range_graph_t *graph, flow_edge_t *flow, ir_interval_t *step)
{
    if (flow->increment == INCREMENT_UNKNOWN)
        flow->increment = increment_side(flow);
    if (flow->increment == INCREMENT_NONE) return false;
    const n00bb_integer_edge_t *edge = flow->edge;
    bool from_left = flow->increment == INCREMENT_LEFT;
    ir_int_t count = 1;
    if (edge->loop && !trip_count(graph, flow->state, edge->loop, &count))
        return false;
    bool unbounded = false;
    ir_interval_t other = {0};
    if (!node_term_interval(from_left ? flow->right : flow->left,
                            from_left ? edge->right : edge->left, &other,
                            &unbounded)
        || !guarded(graph, flow, from_left ? edge->right_symbol
                                           : edge->left_symbol, &other))
        return false;
    if (edge->op != IR_OP_ADD)
        other = ir_interval_apply(IR_OP_NEG, other, other);
    ir_interval_t per_iteration = {other.lo < 0 ? other.lo : 0,
                                   other.hi > 0 ? other.hi : 0};
    *step = ir_interval_apply(IR_OP_MUL, ir_interval_point(count),
                              per_iteration);
    return true;
}

static void
narrow(range_graph_t *graph, flow_t *flow)
{
    size_t count = graph->nodes.len;
    ir_interval_t *fresh = count ? core_alloc_array(ir_interval_t, count)
                                 : nullptr;
    ir_interval_t *base = count ? core_alloc_array(ir_interval_t, count)
                                : nullptr;
    ir_interval_t *growth = count ? core_alloc_array(ir_interval_t, count)
                                  : nullptr;
    bool *reached = count ? core_alloc_array(bool, count) : nullptr;
    bool *based = count ? core_alloc_array(bool, count) : nullptr;
    uint8_t *trip = count ? core_alloc_array(uint8_t, count) : nullptr;
    bool allocated = fresh && base && growth && reached && based && trip;
    for (int round = 0; allocated && round < NARROW_LIMIT; round++) {
        for (size_t i = 0; i < count; i++) {
            range_node_t *node = &graph->nodes.data[i];
            reached[i] = based[i] = node->seeded;
            fresh[i] = base[i] = (ir_interval_t){node->seed_lo,
                                                 node->seed_hi};
            growth[i] = (ir_interval_t){0, 0};
            trip[i] = TRIP_NONE;
        }
        for (size_t e = 0; e < flow->edges.len; e++) {
            flow_edge_t *edge = &flow->edges.data[e];
            range_node_t *result = edge->result;
            if (!result || !result->grown) continue;
            size_t slot = (size_t)(result - graph->nodes.data);
            ir_interval_t out = {0}, step = {0};
            bool unbounded = false;
            bool self = edge->left == result || edge->right == result;
            if (self && trip[slot] != TRIP_FAILED) {
                if (self_increment(graph, edge, &step)) {
                    growth[slot] = ir_interval_apply(IR_OP_ADD, growth[slot],
                                                     step);
                    trip[slot] = TRIP_BOUNDED;
                }
                else trip[slot] = TRIP_FAILED;
            }
            if (!edge_output(graph, edge, &out, &unbounded)) continue;
            fresh[slot] = reached[slot] ? ir_interval_join(fresh[slot], out)
                                        : out;
            reached[slot] = true;
            if (self) continue;
            base[slot] = based[slot] ? ir_interval_join(base[slot], out) : out;
            based[slot] = true;
        }
        for (size_t i = 0; i < count; i++)
            if (trip[i] == TRIP_BOUNDED && based[i]) {
                fresh[i] = ir_interval_apply(IR_OP_ADD, base[i], growth[i]);
                reached[i] = true;
            }
        bool changed = false;
        for (size_t i = 0; i < count; i++) {
            range_node_t *node = &graph->nodes.data[i];
            ir_interval_t narrowed = {0};
            if (!node->grown || !reached[i]
                || !ir_interval_meet((ir_interval_t){node->lo, node->hi},
                                     fresh[i], &narrowed)
                || (narrowed.lo == node->lo && narrowed.hi == node->hi))
                continue;
            node->lo = narrowed.lo;
            node->hi = narrowed.hi;
            changed = true;
        }
        if (!changed) break;
    }
    core_free(fresh);
    core_free(base);
    core_free(growth);
    core_free(reached);
    core_free(based);
    core_free(trip);
}

static void
note_read(core_list_t(flow_read_t) *reads, range_graph_t *graph,
          const range_node_t *node, size_t edge)
{
    if (node)
        core_list_push(*reads, ((flow_read_t){
            (uint32_t)(node - graph->nodes.data), (uint32_t)edge}));
}

static flow_t
flow_build(range_graph_t *graph, core_translation_unit_t *const *units,
           size_t unit_count)
{
    flow_t flow = {.edges = core_list_new(flow_edge_t)};
    core_list_t(flow_read_t) reads = core_list_new(flow_read_t);
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        for (size_t e = 0; state && e < state->integer_edges.len; e++) {
            n00bb_integer_edge_t *edge = &state->integer_edges.data[e];
            flow_edge_t entry = {
                .edge = edge, .state = state,
                .result = find_node(graph, edge->result),
                .left = find_node(graph, edge->left),
                .right = find_node(graph, edge->right),
            };
            size_t id = flow.edges.len;
            core_list_push(flow.edges, entry);
            note_read(&reads, graph, entry.left, id);
            if (entry.right != entry.left)
                note_read(&reads, graph, entry.right, id);
            for (uint32_t g = 0; g < edge->guard_count; g++)
                note_read(&reads, graph,
                          find_node(graph, edge_guard(state, edge, g)->bound),
                          id);
        }
    }
    size_t count = graph->nodes.len;
    flow.reader_start = core_alloc_array(uint32_t, count + 1);
    flow.readers = core_alloc_array(uint32_t, reads.len + 1);
    uint32_t *fill = core_alloc_array(uint32_t, count + 1);
    for (size_t r = 0; r < reads.len; r++)
        flow.reader_start[reads.data[r].node + 1]++;
    for (size_t n = 0; n < count; n++)
        flow.reader_start[n + 1] += flow.reader_start[n];
    for (size_t r = 0; r < reads.len; r++) {
        uint32_t node = reads.data[r].node;
        flow.readers[flow.reader_start[node] + fill[node]++] =
            reads.data[r].edge;
    }
    core_free(fill);
    core_list_free(reads);
    return flow;
}

static void
flow_free(flow_t *flow)
{
    core_list_free(flow->edges);
    core_free(flow->reader_start);
    core_free(flow->readers);
}

static void
mark_edge(uint64_t *words, uint32_t edge)
{
    words[edge / 64] |= UINT64_C(1) << (edge % 64);
}

static bool
settle(range_graph_t *graph, flow_t *flow, uint64_t *now, uint64_t *next,
       size_t words)
{
    bool changed = false;
    for (size_t w = 0; w < words; w++)
        for (uint64_t bits; (bits = now[w]) != 0;) {
            uint32_t e = (uint32_t)(w * 64 + (size_t)__builtin_ctzll(bits));
            now[w] &= now[w] - 1;
            flow_edge_t *edge = &flow->edges.data[e];
            ir_interval_t out = {0};
            bool unbounded = false;
            if (!edge->result || !edge_output(graph, edge, &out, &unbounded)
                || !widen(edge->result, out, unbounded)) continue;
            changed = true;
            size_t slot = (size_t)(edge->result - graph->nodes.data);
            for (uint32_t r = flow->reader_start[slot];
                 r < flow->reader_start[slot + 1]; r++)
                mark_edge(flow->readers[r] > e ? now : next,
                          flow->readers[r]);
        }
    return changed;
}

static void
solve(range_graph_t *graph, flow_t *flow)
{
    size_t words = flow->edges.len / 64 + 1;
    uint64_t *now = core_alloc_array(uint64_t, words);
    uint64_t *next = core_alloc_array(uint64_t, words);
    for (int pass = 0; pass < 3; pass++) {
        for (uint32_t e = 0; e < flow->edges.len; e++) mark_edge(now, e);
        bool changed = true;
        for (size_t round = 0; changed && round < ROUND_LIMIT; round++) {
            changed = settle(graph, flow, now, next, words);
            uint64_t *swap = now;
            now = next;
            next = swap;
        }
        for (size_t w = 0; w < words; w++) now[w] = next[w] = 0;
        for (size_t i = 0; pass < 2 && i < graph->nodes.len; i++) {
            range_node_t *node = &graph->nodes.data[i];
            bool typed = node->kind != N00BB_INTEGER_TERM_TYPE;
            if (node->known || (pass == 0 && !typed && node->fed)) continue;
            uint8_t flags = 0;
            bool unsigned_class = !typed
                && th_var_is_integer(th_resolve(node->type), &flags)
                && (flags & TH_INTEGER_UNSIGNED);
            ir_interval_t fallback = unsigned_class
                ? (ir_interval_t){0, (ir_int_t)UINT64_MAX}
                : (ir_interval_t){INT64_MIN, INT64_MAX};
            if (typed) (void)n00bb_type_interval(node->type, &fallback);
            node->lo = fallback.lo;
            node->hi = fallback.hi;
            node->known = true;
            node->unbounded = true;
        }
    }
    core_free(now);
    core_free(next);
    narrow(graph, flow);
}

static void
collect_nodes(range_graph_t *graph, core_translation_unit_t *const *units,
              size_t unit_count)
{
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        for (size_t i = 0; i < state->integer_seeds.len; i++) {
            n00bb_integer_seed_t *seed = &state->integer_seeds.data[i];
            th_type *current = seed_type(seed);
            if (current != seed->type && n00bb_open_integer(seed->type)
                && th_resolve(current) != th_resolve(seed->type))
                (void)th_unify_in_place(state->types, seed->type, current);
            range_node_t *class = node_for(graph,
                (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, current,
                                       nullptr},
                units[u], state, seed->site);
            if (!class) continue;
            class->lo = class->known ? ir_int_lesser(class->lo, seed->lo)
                                     : seed->lo;
            class->hi = class->known ? ir_int_greater(class->hi, seed->hi)
                                     : seed->hi;
            class->known = true;
            class->wide = class->wide || seed->wide;
            class->seed_lo = class->seeded && class->seed_lo < seed->lo
                ? class->seed_lo : seed->lo;
            class->seed_hi = class->seeded && class->seed_hi > seed->hi
                ? class->seed_hi : seed->hi;
            class->seeded = true;
        }
        for (size_t e = 0; e < state->integer_edges.len; e++) {
            n00bb_integer_edge_t *edge = &state->integer_edges.data[e];
            range_node_t *result = node_for(graph, edge->result, units[u],
                                            state, edge->site);
            if (result) result->fed = true;
            (void)node_for(graph, edge->left, units[u], state, edge->site);
            (void)node_for(graph, edge->right, units[u], state, edge->site);
        }
    }
    for (size_t i = 0; i < graph->nodes.len; i++) {
        range_node_t *node = &graph->nodes.data[i];
        parse_sym_entry_t *symbol = node->kind == N00BB_INTEGER_TERM_SYMBOL
            ? node->key : nullptr;
        ir_interval_t bounds = {0};
        if (!symbol || (core_dict_contains(
                &node->state->integer_tracked_symbols, symbol)
                && !symbol->aliased_by_count && !symbol->alias_unresolved)
            || !n00bb_type_interval(node->type, &bounds)) continue;
        node->lo = bounds.lo;
        node->hi = bounds.hi;
        node->known = true;
        node->frozen = true;
        node->unbounded = true;
    }
}

static void
annotate_ranges(range_graph_t *graph, n00bb_tu_state_t *state,
                parse_tree_t *node, core_dict_t *interface,
                bool interface_only)
{
    if (!node || core_tree_is_leaf(node)) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        annotate_ranges(graph, state, core_tree_child(node, i), interface,
                        interface_only);
    th_type *type = parse_tree_type(node);
    range_node_t *entry = find_node(graph, (n00bb_integer_term_t){
        N00BB_INTEGER_TERM_TYPE, type, nullptr});
    if (entry && interface_only
        && !core_dict_contains(interface, th_resolve(type))) return;
    if (!entry && core_dict_contains(&state->integer_range_nodes, node))
        entry = find_node(graph, (n00bb_integer_term_t){
            N00BB_INTEGER_TERM_NODE, type, node});
    if (!entry) {
        parse_sym_entry_t *symbol = declared_integer_symbol(node);
        if (symbol)
            entry = find_node(graph, (n00bb_integer_term_t){
                N00BB_INTEGER_TERM_SYMBOL, symbol->type, symbol});
    }
    if (!entry || !entry->known) return;
    ir_interval_t solved = {entry->lo, entry->hi};
    n00bb_settle_symbol_range(state, node, solved);
    if (entry->kind == N00BB_INTEGER_TERM_SYMBOL)
        guard_site_range(state, node, &solved);
    ir_interval_t current = {0};
    if (n00bb_current_integer_range(node, &current)
        && !ir_interval_meet(current, solved, &solved)) return;
    (void)n00bb_set_integer_range(state, node, solved);
}

void
n00bb_integer_boundary(n00bb_tu_state_t *state, parse_tree_t *node,
                       th_type *target)
{
    if (state && node && target)
        core_dict_put(&state->integer_boundary_sites, node, target);
}

static bool
check_boundaries(n00bb_tu_state_t *state)
{
    bool ok = true;
    core_dict_foreach(&state->integer_boundary_sites, entry) {
        ir_interval_t range = {0}, bounds = {0};
        th_type *target = th_resolve(entry->value);
        if (!n00bb_type_interval(target, &bounds)
            || !n00bb_current_integer_range(entry->key, &range))
            continue;
        ir_interval_t guarded_range = range;
        guard_site_range(state, entry->key, &guarded_range);
        if (guarded_range.lo != range.lo || guarded_range.hi != range.hi) {
            range = guarded_range;
            (void)n00bb_set_integer_range(state, entry->key, range);
        }
        if (!ir_interval_meet(range, bounds, nullptr)) {
            n00bb_report_integer_misfit(state, entry->key,
                CORE_STRING_STATIC("value"), range, target);
            ok = false;
        }
        else if (range.lo < bounds.lo || range.hi > bounds.hi)
            n00bb_report_integer_narrowing(state, entry->key, range, target);
    }
    core_dict_free(&state->integer_boundary_sites);
    core_dict_init(&state->integer_boundary_sites, nullptr, nullptr);
    return ok;
}

static bool
check_shift_counts(core_translation_unit_t *unit, n00bb_tu_state_t *state)
{
    bool ok = true;
    for (size_t i = 0; i < state->integer_shift_sites.len; i++) {
        parse_tree_t *site = state->integer_shift_sites.data[i];
        uint64_t width = n00bb_shift_receiver_width(
            parse_tree_type(core_tree_child(site, 0)));
        uint64_t result = n00bb_shift_receiver_width(parse_tree_type(site));
        if (result > width) width = result;
        ir_interval_t count = {0};
        if (!width || !n00bb_current_integer_range(
                core_tree_child(site, 2), &count)
            || (count.lo >= 0 && count.lo < (ir_int_t)width)) continue;
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
            CORE_STRING_STATIC("shift count is outside the receiver width"),
            link_source_location(core_tree_child(site, 2)));
        ok = false;
    }
    state->integer_shift_sites.len = 0;
    core_dict_free(&state->integer_shift_seen);
    core_dict_init(&state->integer_shift_seen, nullptr, nullptr);
    return ok;
}

static bool
term_resolved(n00bb_integer_term_t term)
{
    return term.kind != N00BB_INTEGER_TERM_TYPE
        || !n00bb_open_integer(term.type);
}

static bool
retire_resolved(core_translation_unit_t *const *units, size_t unit_count,
                bool interface_only)
{
    bool ok = true;
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        size_t kept = 0;
        for (size_t i = 0; i < state->integer_seeds.len; i++) {
            n00bb_integer_seed_t seed = state->integer_seeds.data[i];
            th_type *type = th_resolve(seed_type(&seed));
            if (th_kind_of(type) == TH_VAR)
                state->integer_seeds.data[kept++] = seed;
            else if (th_is_closed(state->types, type))
                ok = check_seed_fits(state, &seed, type) && ok;
        }
        state->integer_seeds.len = kept;
        if (interface_only) continue;
        kept = 0;
        size_t guards = 0;
        for (size_t e = 0; e < state->integer_edges.len; e++) {
            n00bb_integer_edge_t edge = state->integer_edges.data[e];
            bool keep = !term_resolved(edge.result)
                || !term_resolved(edge.left) || !term_resolved(edge.right);
            if (edge.result.kind == N00BB_INTEGER_TERM_NODE
                && (uintptr_t)core_dict_get(&state->integer_range_nodes,
                                            edge.result.key, nullptr) == e + 1)
                core_dict_put(&state->integer_range_nodes, edge.result.key,
                              keep ? kept + 1 : 0);
            if (!keep) continue;
            for (uint32_t g = 0; g < edge.guard_count; g++)
                state->integer_guards.data[guards + g] =
                    state->integer_guards.data[edge.guard_start + g];
            edge.guard_start = (uint32_t)guards;
            guards += edge.guard_count;
            state->integer_edges.data[kept++] = edge;
        }
        state->integer_edges.len = kept;
        state->integer_guards.len = guards;
    }
    return ok;
}

bool
n00bb_ground_integers(core_translation_unit_t *const *units,
                      size_t unit_count, bool interface_only,
                      bool *grounded)
{
    *grounded = false;
    bool pending = false;
    for (size_t u = 0; !pending && u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        pending = state && (state->integer_seeds.len
                            || state->integer_edges.len
                            || state->integer_shift_sites.len);
    }
    if (!pending) {
        bool fits = true;
        for (size_t u = 0; !interface_only && u < unit_count; u++) {
            n00bb_tu_state_t *state = tu_state(units[u]);
            if (state) fits = check_boundaries(state) && fits;
        }
        return fits;
    }
    core_dict_t interface;
    core_dict_init(&interface, nullptr, nullptr);
    if (interface_only) collect_interfaces(units, unit_count, &interface);
    range_graph_t graph = {.nodes = core_list_new(range_node_t)};
    core_dict_init(&graph.index, nullptr, nullptr);
    collect_nodes(&graph, units, unit_count);
    bool ok = true;
    flow_t flow = flow_build(&graph, units, unit_count);
    solve(&graph, &flow);
    flow_free(&flow);
    for (size_t u = 0; u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (state)
            annotate_ranges(&graph, state, n00bb_tu_parsed_tree(state),
                            &interface, interface_only);
    }
    for (size_t i = 0; i < graph.nodes.len; i++) {
        range_node_t *class = &graph.nodes.data[i];
        uint8_t flags = 0;
        th_type *type = class->kind == N00BB_INTEGER_TERM_TYPE
            ? th_resolve(class->type) : nullptr;
        if (!class->state || !type || th_kind_of(type) != TH_VAR
            || !th_var_is_integer(type, &flags)
            || (interface_only && !core_dict_contains(&interface, type)))
            continue;
        th_type *chosen = best_fit(class->state->types, class,
                                   (flags & TH_INTEGER_UNSIGNED) != 0);
        th_type *allowed = allowed_fit(type, class);
        if (allowed) chosen = allowed;
        if (!chosen) {
            report(class, (flags & TH_INTEGER_UNSIGNED)
                ? CORE_STRING_STATIC(
                      "a bitwise integer value can be negative; give it an unsigned type")
                : CORE_STRING_STATIC("no integer type holds this value"));
            ok = false;
            continue;
        }
        if (!th_ground_integer(class->state->types, class->type, chosen)) {
            report(class, th_last_error(class->state->types));
            ok = false;
        }
        else *grounded = true;
    }
    ok = retire_resolved(units, unit_count, interface_only) && ok;
    for (size_t u = 0; !interface_only && u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        ok = check_shift_counts(units[u], state) && ok;
        ok = check_boundaries(state) && ok;
    }
    core_list_free(graph.nodes);
    core_dict_free(&graph.index);
    core_dict_free(&interface);
    return ok;
}
