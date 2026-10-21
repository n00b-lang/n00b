#include "compiler_tu.h"
#include "compiler_link.h"
#include "ir/interval.h"

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
    parse_tree_t *lvalue = n00bb_simple_lvalue(node);
    parse_node_resolution_t resolution = lvalue
        ? parse_tree_resolution(lvalue) : (parse_node_resolution_t){0};
    parse_sym_entry_t *symbol = resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    return symbol && symbol->kind == PARSE_SYM_VARIABLE
        && is_integer_type(symbol->type) ? symbol : nullptr;
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
    for (parse_tree_t *at = node; at && !core_tree_is_leaf(at);) {
        if (core_dict_contains(&state->integer_range_nodes, at))
            return (n00bb_integer_term_t){N00BB_INTEGER_TERM_NODE,
                                          parse_tree_type(at), at};
        size_t count = core_tree_num_children(at);
        at = count == 1 ? core_tree_child(at, 0)
            : count == 3 && parse_tree_leaf_is(core_tree_child(at, 0),
                                               CORE_STRING_STATIC("("))
            ? core_tree_child(at, 1) : nullptr;
    }
    return (n00bb_integer_term_t){
        type ? N00BB_INTEGER_TERM_TYPE : N00BB_INTEGER_TERM_NONE, type,
        nullptr};
}

static parse_tree_t *
guard_unwrap(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        size_t count = core_tree_num_children(node);
        if (count == 1) node = core_tree_child(node, 0);
        else if (count == 3 && parse_tree_leaf_is(core_tree_child(node, 0),
                                                  CORE_STRING_STATIC("(")))
            node = core_tree_child(node, 1);
        else break;
    }
    return node;
}

static ir_op_t
guard_comparison(parse_tree_t *operator, bool flipped)
{
    core_string_t text = operator ? first_token_text(operator)
                                  : (core_string_t){0};
    if (core_string_eq(text, CORE_STRING_STATIC("<")))
        return flipped ? IR_OP_GT : IR_OP_LT;
    if (core_string_eq(text, CORE_STRING_STATIC("<=")))
        return flipped ? IR_OP_GE : IR_OP_LE;
    if (core_string_eq(text, CORE_STRING_STATIC(">")))
        return flipped ? IR_OP_LT : IR_OP_GT;
    if (core_string_eq(text, CORE_STRING_STATIC(">=")))
        return flipped ? IR_OP_LE : IR_OP_GE;
    return IR_OP_COUNT;
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
        || edge->guard_count >= N00BB_INTEGER_GUARD_LIMIT
        || (symbol != edge->left_symbol && symbol != edge->right_symbol))
        return;
    n00bb_integer_term_t term = n00bb_integer_term_of(state, bound);
    if (term.kind == N00BB_INTEGER_TERM_NONE) return;
    edge->guards[edge->guard_count++] = (n00bb_integer_guard_t){
        .symbol = symbol, .bound = term, .op = op,
    };
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
tree_contains(parse_tree_t *outer, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at))
        if (at == outer) return true;
    return false;
}

static bool
assigns_before(parse_grammar_t *grammar, parse_tree_t *node,
               parse_tree_t *site, parse_sym_entry_t *symbol, bool *reached)
{
    if (*reached || !node || core_tree_is_leaf(node)) return false;
    if (node == site) {
        *reached = true;
        return false;
    }
    bool contains = tree_contains(node, site);
    if (!contains
        && ((nt_is(grammar, node, CORE_STRING_STATIC("assign-stmt"))
                || nt_is(grammar, node,
                         CORE_STRING_STATIC("binop-assign-stmt")))
            && simple_expression_symbol(core_tree_child(node, 0)) == symbol))
        return true;
    if (!contains && nt_is(grammar, node, CORE_STRING_STATIC("sym-decl"))
        && parse_tree_sym(node) == symbol)
        return true;
    if (!contains && nt_is(grammar, node, CORE_STRING_STATIC("call-args")))
        for (size_t i = 0; i < core_tree_num_children(node); i++) {
            parse_tree_t *group = core_tree_child(node, i);
            for (size_t a = 0; !core_tree_is_leaf(group)
                 && a < core_tree_num_children(group); a++) {
                parse_tree_t *argument = core_tree_child(group, a);
                if (!core_tree_is_leaf(argument)
                    && core_tree_num_children(argument)
                    && simple_expression_symbol(core_tree_child(argument,
                        core_tree_num_children(argument) - 1)) == symbol)
                    return true;
                if (simple_expression_symbol(argument) == symbol)
                    return true;
            }
        }
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        if (assigns_before(grammar, core_tree_child(node, i), site, symbol,
                           reached)) return true;
        if (*reached) return false;
    }
    return false;
}

static bool
reference_taken(parse_tree_t *node, parse_sym_entry_t *symbol)
{
    if (!node || core_tree_is_leaf(node)) return false;
    if (core_tree_num_children(node) == 2
        && parse_tree_leaf_is(core_tree_child(node, 0),
                              CORE_STRING_STATIC("ref"))
        && simple_expression_symbol(core_tree_child(node, 1)) == symbol)
        return true;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        if (reference_taken(core_tree_child(node, i), symbol)) return true;
    return false;
}

static parse_tree_t *
enclosing_function(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; at = parse_tree_parent(at))
        if (nt_is(grammar, at, CORE_STRING_STATIC("func-def"))
            || nt_is(grammar, at, CORE_STRING_STATIC("context-def")))
            return at;
    return nullptr;
}

static bool
reference_taken_cached(n00bb_tu_state_t *state, parse_tree_t *function,
                       parse_sym_entry_t *symbol)
{
    bool found = false;
    uintptr_t known = (uintptr_t)core_dict_get(
        &state->integer_reference_taken, symbol, &found);
    if (found) return known == 2;
    bool taken = reference_taken(function, symbol);
    core_dict_put(&state->integer_reference_taken, symbol,
                  (void *)(uintptr_t)(taken ? 2 : 1));
    return taken;
}

static void
site_guards(n00bb_tu_state_t *state, parse_tree_t *site,
            core_list_t(guard_candidate_t) *valid)
{
    parse_grammar_t *grammar = state->grammar;
    parse_tree_t *function = enclosing_function(grammar, site);
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
            for (size_t g = 0; g < found.len; g++) {
                bool reached = false;
                if (guard_symbol(found.data[g].symbol) && function
                    && !reference_taken_cached(state, function,
                                               found.data[g].symbol)
                    && !assigns_before(grammar, at, site,
                                       found.data[g].symbol, &reached))
                    core_list_push(*valid, found.data[g]);
            }
        }
        if (loop || nt_is(grammar, parent, CORE_STRING_STATIC("for-stmt"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("func-def"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("context-def")))
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
            && n00bb_current_integer_range(state, valid.data[g].bound, &bound)
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
        if (nt_is(grammar, parent, CORE_STRING_STATIC("func-def"))
            || nt_is(grammar, parent, CORE_STRING_STATIC("context-def")))
            return found;
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
    core_dict_put(&state->integer_range_nodes, site, site);
    if (n00bb_integer_flow(state, site, op,
            (n00bb_integer_term_t){N00BB_INTEGER_TERM_NODE,
                                   parse_tree_type(site), site},
            n00bb_integer_term_of(state, left),
            right ? n00bb_integer_term_of(state, right)
                  : (n00bb_integer_term_t){0}))
        guard_last_edge(state, left, right);
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
    for (size_t e = state->integer_edges.len, seen = 0; e > 0 && seen < 16;
         e--, seen++) {
        const n00bb_integer_edge_t *edge = &state->integer_edges.data[e - 1];
        if (edge->site == value && edge->result.kind == N00BB_INTEGER_TERM_NODE
            && edge->result.key == value) return edge;
    }
    return nullptr;
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

static bool
type_interval(th_type *type, ir_interval_t *out)
{
    return type && n00bb_type_interval(type, out);
}

static th_type *
best_fit(th_ctx *types, const range_node_t *class, bool unsigned_only)
{
    const core_string_t unsigned_names[] = {
        CORE_STRING_STATIC("u8"), CORE_STRING_STATIC("u16"),
        CORE_STRING_STATIC("u32"), CORE_STRING_STATIC("u64"),
        CORE_STRING_STATIC("u128"),
    };
    const core_string_t signed_names[] = {
        CORE_STRING_STATIC("i8"), CORE_STRING_STATIC("i16"),
        CORE_STRING_STATIC("i32"), CORE_STRING_STATIC("i64"),
        CORE_STRING_STATIC("i128"),
    };
    bool is_signed = class->lo < 0 && !class->wide;
    if (is_signed && unsigned_only) return nullptr;
    for (size_t i = 0; i < 5; i++) {
        uint64_t width = (uint64_t)8 << i;
        if (!class->wide && !ir_interval_fits(
                (ir_interval_t){class->lo, class->hi}, width, is_signed))
            continue;
        if (class->wide && width < 128) continue;
        return th_name(types, canonical_builtin_type_name(
            is_signed ? signed_names[i] : unsigned_names[i]));
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
            if (!type_interval(candidate, &bounds)) continue;
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

static void
append_integer(core_buffer_t *buffer, ir_int_t value, bool wide)
{
    char digits[48];
    size_t length = 0;
    ir_uint_t magnitude = wide ? ~(ir_uint_t)0
        : value < 0 ? (ir_uint_t)(-(value + 1)) + 1 : (ir_uint_t)value;
    do {
        digits[length++] = (char)('0' + (int)(magnitude % 10));
        magnitude /= 10;
    } while (magnitude);
    if (!wide && value < 0) core_buffer_append(buffer, "-", 1);
    while (length) core_buffer_append(buffer, &digits[--length], 1);
}

static bool
check_seed_fits(n00bb_tu_state_t *state, core_translation_unit_t *unit,
                const n00bb_integer_seed_t *seed, th_type *type)
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
    core_string_t name = th_print(state->types, type);
    core_buffer_t message = {0};
    core_buffer_append(&message, "integer constant ", 17);
    append_integer(&message, seed->lo, seed->wide);
    core_buffer_printf(&message, " does not fit %.*s",
                       (int)name.u8_bytes, name.data);
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0041"),
        core_buffer_string(&message), link_source_location(seed->site));
    core_buffer_release(&message);
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
node_for(range_graph_t *graph, n00bb_integer_term_t term,
         core_translation_unit_t *unit, n00bb_tu_state_t *state,
         parse_tree_t *site)
{
    void *key = term_key(term);
    if (!key) return nullptr;
    bool found = false;
    uintptr_t slot = (uintptr_t)core_dict_get(&graph->index, key, &found);
    if (found) return &graph->nodes.data[slot - 1];
    if (!unit) return nullptr;
    core_list_push(graph->nodes, ((range_node_t){
        .kind = term.kind, .key = key,
        .type = term.kind == N00BB_INTEGER_TERM_TYPE ? th_resolve(term.type)
                                                     : term.type,
        .site = site, .unit = unit, .state = state,
    }));
    core_dict_put(&graph->index, key, (void *)(uintptr_t)graph->nodes.len);
    return &graph->nodes.data[graph->nodes.len - 1];
}

static range_node_t *
class_for_type(range_graph_t *graph, th_type *type)
{
    return node_for(graph,
        (n00bb_integer_term_t){N00BB_INTEGER_TERM_TYPE, type, nullptr},
        nullptr, nullptr, nullptr);
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
term_interval(range_graph_t *graph, n00bb_integer_term_t term,
              ir_interval_t *out, bool *unbounded)
{
    range_node_t *node = node_for(graph, term, nullptr, nullptr, nullptr);
    if (node) {
        *out = (ir_interval_t){node->lo, node->hi};
        *unbounded = *unbounded || node->unbounded;
        return node->known;
    }
    *unbounded = true;
    if (type_interval(term.type ? th_resolve(term.type) : nullptr, out))
        return true;
    if (term.type && th_kind_of(th_resolve(term.type)) == TH_VAR)
        return false;
    *out = (ir_interval_t){INT64_MIN, INT64_MAX};
    return term.kind != N00BB_INTEGER_TERM_NONE;
}

static bool
widen(range_node_t *node, ir_interval_t range, bool unbounded)
{
    if (node->frozen) return false;
    bool flagged = unbounded && !node->unbounded;
    node->unbounded = node->unbounded || unbounded;
    ir_interval_t bounds = {0};
    if (node->kind != N00BB_INTEGER_TERM_TYPE
        && type_interval(node->type, &bounds)
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
            && type_interval(node->type, &bounds))
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

static bool
guarded(range_graph_t *graph, const n00bb_integer_edge_t *edge,
        parse_sym_entry_t *symbol, ir_interval_t *range)
{
    if (!symbol) return true;
    for (uint8_t g = 0; g < edge->guard_count; g++) {
        const n00bb_integer_guard_t *guard = &edge->guards[g];
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
edge_output(range_graph_t *graph, const n00bb_integer_edge_t *edge,
            ir_interval_t *out, bool *unbounded)
{
    bool binary = edge->right.kind != N00BB_INTEGER_TERM_NONE;
    ir_interval_t left = {0}, right = {0};
    if (!term_interval(graph, edge->left, &left, unbounded)
        || !guarded(graph, edge, edge->left_symbol, &left))
        return false;
    if (!binary) right = left;
    else if (!term_interval(graph, edge->right, &right, unbounded)
             || !guarded(graph, edge, edge->right_symbol, &right))
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
    if (node && n00bb_current_integer_range(state, node, out)) return true;
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
        if (nt_is(grammar, at, CORE_STRING_STATIC("func-def"))
            || nt_is(grammar, at, CORE_STRING_STATIC("context-def")))
            return false;
        if (loop_node(grammar, at)) return true;
    }
    return false;
}

static bool
activation_local(n00bb_tu_state_t *state, parse_tree_t *site,
                 parse_sym_entry_t *symbol)
{
    parse_tree_t *function = enclosing_function(state->grammar, site);
    return symbol && function && symbol->decl_node
        && enclosing_function(state->grammar, symbol->decl_node) == function
        && !reference_taken_cached(state, function, symbol);
}

static bool
self_increment(range_graph_t *graph, n00bb_tu_state_t *state,
               const n00bb_integer_edge_t *edge, range_node_t *node,
               ir_interval_t *step)
{
    range_node_t *left = node_for(graph, edge->left, nullptr, nullptr,
                                  nullptr);
    range_node_t *right = node_for(graph, edge->right, nullptr, nullptr,
                                   nullptr);
    bool add = edge->op == IR_OP_ADD;
    bool from_left = left == node && right != node
        && (add || edge->op == IR_OP_SUB);
    bool from_right = add && right == node && left != node;
    if ((!from_left && !from_right)
        || !activation_local(state, edge->site,
                             from_left ? edge->left_symbol
                                       : edge->right_symbol)) return false;
    ir_int_t count = 1;
    if (edge->loop ? !trip_count(graph, state, edge->loop, &count)
                   : inside_loop(state->grammar, edge->site))
        return false;
    bool unbounded = false;
    ir_interval_t other = {0};
    if (!term_interval(graph, from_left ? edge->right : edge->left, &other,
                       &unbounded)
        || !guarded(graph, edge, from_left ? edge->right_symbol
                                           : edge->left_symbol, &other))
        return false;
    if (!add) other = ir_interval_apply(IR_OP_NEG, other, other);
    ir_interval_t per_iteration = {other.lo < 0 ? other.lo : 0,
                                   other.hi > 0 ? other.hi : 0};
    *step = ir_interval_apply(IR_OP_MUL, ir_interval_point(count),
                              per_iteration);
    return true;
}

static void
narrow(range_graph_t *graph, core_translation_unit_t *const *units,
       size_t unit_count)
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
        for (size_t u = 0; u < unit_count; u++) {
            n00bb_tu_state_t *state = tu_state(units[u]);
            for (size_t e = 0; state && e < state->integer_edges.len; e++) {
                n00bb_integer_edge_t *edge = &state->integer_edges.data[e];
                range_node_t *result = node_for(graph, edge->result,
                    nullptr, nullptr, nullptr);
                if (!result || !result->grown) continue;
                size_t slot = (size_t)(result - graph->nodes.data);
                ir_interval_t out = {0}, step = {0};
                bool unbounded = false;
                bool self = node_for(graph, edge->left, nullptr, nullptr,
                                     nullptr) == result
                    || node_for(graph, edge->right, nullptr, nullptr,
                                nullptr) == result;
                if (self && trip[slot] != TRIP_FAILED) {
                    if (self_increment(graph, state, edge, result, &step)) {
                        growth[slot] = ir_interval_apply(IR_OP_ADD,
                                                         growth[slot], step);
                        trip[slot] = TRIP_BOUNDED;
                    }
                    else trip[slot] = TRIP_FAILED;
                }
                if (!edge_output(graph, edge, &out, &unbounded)) continue;
                fresh[slot] = reached[slot]
                    ? ir_interval_join(fresh[slot], out) : out;
                reached[slot] = true;
                if (self) continue;
                base[slot] = based[slot]
                    ? ir_interval_join(base[slot], out) : out;
                based[slot] = true;
            }
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
solve(range_graph_t *graph, core_translation_unit_t *const *units,
      size_t unit_count)
{
    for (int pass = 0; pass < 3; pass++) {
        bool changed = true;
        for (size_t round = 0; changed && round < ROUND_LIMIT; round++) {
            changed = false;
            for (size_t u = 0; u < unit_count; u++) {
                n00bb_tu_state_t *state = tu_state(units[u]);
                for (size_t e = 0; state && e < state->integer_edges.len;
                     e++) {
                    n00bb_integer_edge_t *edge = &state->integer_edges.data[e];
                    range_node_t *result = node_for(graph, edge->result,
                        nullptr, nullptr, nullptr);
                    ir_interval_t out = {0};
                    bool unbounded = false;
                    if (!result || !edge_output(graph, edge, &out,
                                                &unbounded)) continue;
                    changed = widen(result, out, unbounded) || changed;
                }
            }
        }
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
            if (typed) (void)type_interval(node->type, &fallback);
            node->lo = fallback.lo;
            node->hi = fallback.hi;
            node->known = true;
            node->unbounded = true;
        }
    }
    narrow(graph, units, unit_count);
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
            if (!class->known) {
                class->lo = seed->lo;
                class->hi = seed->hi;
                class->known = true;
            }
            if (seed->lo < class->lo) class->lo = seed->lo;
            if (seed->hi > class->hi) class->hi = seed->hi;
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
            || !type_interval(node->type, &bounds)) continue;
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
    range_node_t *entry = class_for_type(graph, type);
    if (entry && interface_only
        && !core_dict_contains(interface, th_resolve(type))) return;
    if (!entry && core_dict_contains(&state->integer_range_nodes, node))
        entry = node_for(graph, (n00bb_integer_term_t){
            N00BB_INTEGER_TERM_NODE, type, node}, nullptr, nullptr, nullptr);
    if (!entry) {
        parse_sym_entry_t *symbol = declared_integer_symbol(node);
        if (symbol)
            entry = node_for(graph, (n00bb_integer_term_t){
                N00BB_INTEGER_TERM_SYMBOL, symbol->type, symbol},
                nullptr, nullptr, nullptr);
    }
    if (!entry || !entry->known) return;
    ir_interval_t solved = {entry->lo, entry->hi};
    n00bb_settle_symbol_range(state, node, solved);
    if (entry->kind == N00BB_INTEGER_TERM_SYMBOL)
        guard_site_range(state, node, &solved);
    ir_interval_t current = {0};
    if (n00bb_current_integer_range(state, node, &current)
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

static void
check_boundaries(n00bb_tu_state_t *state)
{
    core_dict_foreach(&state->integer_boundary_sites, entry) {
        ir_interval_t range = {0}, bounds = {0};
        th_type *target = th_resolve(entry->value);
        if (!n00bb_type_interval(target, &bounds)
            || !n00bb_current_integer_range(state, entry->key, &range))
            continue;
        ir_interval_t guarded_range = range;
        guard_site_range(state, entry->key, &guarded_range);
        if (guarded_range.lo != range.lo || guarded_range.hi != range.hi) {
            range = guarded_range;
            (void)n00bb_set_integer_range(state, entry->key, range);
        }
        if (!ir_interval_meet(range, bounds, nullptr))
            n00bb_report_integer_misfit(state, entry->key, range, target);
        else if (range.lo < bounds.lo || range.hi > bounds.hi)
            n00bb_report_integer_narrowing(state, entry->key, range, target);
    }
    core_dict_free(&state->integer_boundary_sites);
    core_dict_init(&state->integer_boundary_sites, nullptr, nullptr);
}

static void
check_shift_counts(core_translation_unit_t *unit, n00bb_tu_state_t *state)
{
    for (size_t i = 0; i < state->integer_shift_sites.len; i++) {
        parse_tree_t *site = state->integer_shift_sites.data[i];
        uint64_t width = n00bb_shift_receiver_width(
            parse_tree_type(core_tree_child(site, 0)));
        uint64_t result = n00bb_shift_receiver_width(parse_tree_type(site));
        if (result > width) width = result;
        ir_interval_t count = {0};
        if (!width || !n00bb_current_integer_range(state,
                core_tree_child(site, 2), &count)
            || (count.lo >= 0 && count.lo < (ir_int_t)width)) continue;
        core_diagnostics_report_at(core_translation_unit_diagnostics(unit),
            unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0047"),
            CORE_STRING_STATIC("shift count is outside the receiver width"),
            link_source_location(core_tree_child(site, 2)));
    }
    state->integer_shift_sites.len = 0;
}

static bool
term_resolved(n00bb_integer_term_t term)
{
    return term.kind != N00BB_INTEGER_TERM_TYPE
        || !n00bb_open_integer(term.type);
}

static void
retire_resolved(core_translation_unit_t *const *units, size_t unit_count,
                bool interface_only)
{
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
                (void)check_seed_fits(state, units[u], &seed, type);
        }
        state->integer_seeds.len = kept;
        if (interface_only) continue;
        kept = 0;
        for (size_t e = 0; e < state->integer_edges.len; e++) {
            n00bb_integer_edge_t edge = state->integer_edges.data[e];
            if (!term_resolved(edge.result) || !term_resolved(edge.left)
                || !term_resolved(edge.right))
                state->integer_edges.data[kept++] = edge;
        }
        state->integer_edges.len = kept;
    }
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
        for (size_t u = 0; !interface_only && u < unit_count; u++) {
            n00bb_tu_state_t *state = tu_state(units[u]);
            if (state) check_boundaries(state);
        }
        return true;
    }
    core_dict_t interface;
    core_dict_init(&interface, nullptr, nullptr);
    if (interface_only) collect_interfaces(units, unit_count, &interface);
    range_graph_t graph = {.nodes = core_list_new(range_node_t)};
    core_dict_init(&graph.index, nullptr, nullptr);
    collect_nodes(&graph, units, unit_count);
    bool ok = true;
    solve(&graph, units, unit_count);
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
    retire_resolved(units, unit_count, interface_only);
    for (size_t u = 0; !interface_only && u < unit_count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);
        if (!state) continue;
        check_shift_counts(units[u], state);
        check_boundaries(state);
    }
    core_list_free(graph.nodes);
    core_dict_free(&graph.index);
    core_dict_free(&interface);
    return ok;
}
