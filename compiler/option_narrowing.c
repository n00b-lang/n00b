#include "option_narrowing.h"

#include "compiler_tu.h"
#include "lib/list.h"

#define NT_IS(g, n, lit) nt_is((g), (n), CORE_STRING_STATIC(lit))
#define CHILD_NT(g, n, lit) child_nt((g), (n), CORE_STRING_STATIC(lit))

typedef struct {
    bool when_true;
    bool when_false;
} nil_facts_t;

typedef struct {
    parse_grammar_t *grammar;
    parse_sym_entry_t *symbol;
    parse_tree_t *use;
    size_t use_start;
    parse_tree_t *invalidator;
} assignment_scan_t;

static parse_tree_t *
unwrap(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (core_tree_num_children(node) == 1) {
            node = core_tree_child(node, 0);
            continue;
        }
        if (NT_IS(grammar, node, "tuple-or-paren")
            && core_tree_num_children(node) == 3) {
            node = core_tree_child(node, 1);
            continue;
        }
        break;
    }
    return node;
}

static parse_sym_entry_t *
referenced_symbol(parse_tree_t *node)
{
    parse_node_resolution_t resolution = parse_tree_resolution(node);
    if (resolution.kind == PARSE_NODE_RESOLUTION_SYMBOL)
        return resolution.target.symbol;
    return parse_tree_sym(node);
}

static bool
names_symbol(parse_grammar_t *grammar, parse_tree_t *node,
             parse_sym_entry_t *symbol)
{
    for (parse_tree_t *at = node; at; ) {
        if (n00bb_primary_node(grammar, at)) return referenced_symbol(at) == symbol;
        if (core_tree_is_leaf(at) || core_tree_num_children(at) != 1) {
            if (NT_IS(grammar, at, "tuple-or-paren")
                && core_tree_num_children(at) == 3) {
                at = core_tree_child(at, 1);
                continue;
            }
            return false;
        }
        at = core_tree_child(at, 0);
    }
    return false;
}

static bool
is_nil(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *at = node; at; ) {
        if (NT_IS(grammar, at, "nil-keyword")) return true;
        if (core_tree_is_leaf(at) || core_tree_num_children(at) != 1)
            return false;
        at = core_tree_child(at, 0);
    }
    return false;
}

static core_string_t
operator_text(parse_tree_t *node)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    core_option_t(core_string_t) value = token ? parse_token_value(token)
                                               : core_option_none(core_string_t);
    return core_option_is_set(value) ? core_option_get(value)
                                     : (core_string_t){0};
}

static nil_facts_t
nil_facts(parse_grammar_t *grammar, parse_tree_t *condition,
          parse_sym_entry_t *symbol)
{
    parse_tree_t *node = unwrap(grammar, condition);
    if (!node || core_tree_is_leaf(node)) return (nil_facts_t){0};
    size_t children = core_tree_num_children(node);
    if (NT_IS(grammar, node, "equality-expr") && children == 3) {
        parse_tree_t *left = core_tree_child(node, 0);
        parse_tree_t *right = core_tree_child(node, 2);
        bool tests = (names_symbol(grammar, left, symbol) && is_nil(grammar, right))
            || (names_symbol(grammar, right, symbol) && is_nil(grammar, left));
        if (!tests) return (nil_facts_t){0};
        core_string_t op = operator_text(core_tree_child(node, 1));
        if (core_string_eq(op, CORE_STRING_STATIC("!=")))
            return (nil_facts_t){.when_true = true};
        if (core_string_eq(op, CORE_STRING_STATIC("==")))
            return (nil_facts_t){.when_false = true};
        return (nil_facts_t){0};
    }
    if (NT_IS(grammar, node, "and-expr") && children == 3) {
        nil_facts_t left = nil_facts(grammar, core_tree_child(node, 0), symbol);
        nil_facts_t right = nil_facts(grammar, core_tree_child(node, 2), symbol);
        return (nil_facts_t){
            .when_true = left.when_true || right.when_true,
            .when_false = left.when_false && right.when_false,
        };
    }
    if (NT_IS(grammar, node, "or-expr") && children == 3) {
        nil_facts_t left = nil_facts(grammar, core_tree_child(node, 0), symbol);
        nil_facts_t right = nil_facts(grammar, core_tree_child(node, 2), symbol);
        return (nil_facts_t){
            .when_true = left.when_true && right.when_true,
            .when_false = left.when_false || right.when_false,
        };
    }
    if (NT_IS(grammar, node, "unary-expr") && children == 2
        && NT_IS(grammar, core_tree_child(node, 0), "not-op")) {
        nil_facts_t inner = nil_facts(grammar, core_tree_child(node, 1), symbol);
        return (nil_facts_t){.when_true = inner.when_false,
                             .when_false = inner.when_true};
    }
    return (nil_facts_t){0};
}

static size_t
start_of(parse_tree_t *node)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(node);
    return token ? parse_token_start_byte(token) : 0;
}

static bool
is_loop(parse_grammar_t *grammar, parse_tree_t *node)
{
    return NT_IS(grammar, node, "while-stmt") || NT_IS(grammar, node, "for-stmt");
}

static bool
encloses(parse_tree_t *outer, parse_tree_t *inner)
{
    for (parse_tree_t *at = inner; at; at = parse_tree_parent(at))
        if (at == outer) return true;
    return false;
}

static parse_tree_t *
owning_if(parse_grammar_t *grammar, parse_tree_t *clause)
{
    parse_tree_t *at = parse_tree_parent(clause);
    while (at && !NT_IS(grammar, at, "if-stmt")) at = parse_tree_parent(at);
    return at;
}

static parse_tree_t *
branch_of(parse_grammar_t *grammar, parse_tree_t *node, parse_tree_t *if_stmt)
{
    parse_tree_t *child = node;
    for (parse_tree_t *at = parse_tree_parent(node); at;
         child = at, at = parse_tree_parent(at)) {
        if (at == if_stmt)
            return NT_IS(grammar, child, "body") ? child : nullptr;
        if ((NT_IS(grammar, at, "elif-clause")
             || NT_IS(grammar, at, "else-clause"))
            && owning_if(grammar, at) == if_stmt)
            return NT_IS(grammar, child, "body") ? child : nullptr;
    }
    return nullptr;
}

static bool
same_statement(parse_grammar_t *grammar, parse_tree_t *assignment,
               parse_tree_t *use)
{
    for (parse_tree_t *at = parse_tree_parent(assignment); at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "assign-stmt")
            || NT_IS(grammar, at, "binop-assign-stmt"))
            return encloses(at, use);
        if (NT_IS(grammar, at, "body-stmt")) return false;
    }
    return false;
}

static bool
on_other_branch(parse_grammar_t *grammar, parse_tree_t *region,
                parse_tree_t *assignment, parse_tree_t *use)
{
    for (parse_tree_t *at = parse_tree_parent(assignment); at && at != region;
         at = parse_tree_parent(at)) {
        if (!NT_IS(grammar, at, "if-stmt") || !encloses(at, use)) continue;
        parse_tree_t *assigned = branch_of(grammar, assignment, at);
        parse_tree_t *used = branch_of(grammar, use, at);
        if (assigned && used && assigned != used) return true;
    }
    return false;
}

static bool
loop_between(parse_grammar_t *grammar, parse_tree_t *region,
             parse_tree_t *assignment, parse_tree_t *use)
{
    for (parse_tree_t *at = parse_tree_parent(assignment); at && at != region;
         at = parse_tree_parent(at))
        if (is_loop(grammar, at) && encloses(at, use)) return true;
    return false;
}

static bool
is_whole_assignment_target(parse_grammar_t *grammar, parse_tree_t *use)
{
    for (parse_tree_t *at = parse_tree_parent(use); at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "assign-stmt")
            || NT_IS(grammar, at, "binop-assign-stmt"))
            return n00bb_assignment_target(grammar, use, nullptr);
        if (core_tree_is_leaf(at) || core_tree_num_children(at) != 1)
            return false;
    }
    return false;
}

static void
scan_assignments(assignment_scan_t *scan, parse_tree_t *region,
                 parse_tree_t *node)
{
    if (!node || scan->invalidator) return;
    if (n00bb_primary_node(scan->grammar, node)
        && referenced_symbol(node) == scan->symbol && node != scan->use) {
        parse_tree_t *parent = parse_tree_parent(node);
        bool written = is_whole_assignment_target(scan->grammar, node)
            || (parent && NT_IS(scan->grammar, parent, "unary-expr")
                && core_string_eq(operator_text(core_tree_child(parent, 0)),
                                  CORE_STRING_STATIC("ref")));
        if (written && !same_statement(scan->grammar, node, scan->use)
            && ((start_of(node) < scan->use_start
                 && !on_other_branch(scan->grammar, region, node, scan->use))
                || loop_between(scan->grammar, region, node, scan->use))) {
            scan->invalidator = node;
            return;
        }
    }
    if (core_tree_is_leaf(node)) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        scan_assignments(scan, region, core_tree_child(node, i));
}

static bool
holds_until_use(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                parse_tree_t *region, parse_tree_t *use, parse_tree_t **lost)
{
    assignment_scan_t scan = {
        .grammar = grammar,
        .symbol = symbol,
        .use = use,
        .use_start = start_of(use),
    };
    scan_assignments(&scan, region, region);
    if (scan.invalidator && lost && !*lost) *lost = scan.invalidator;
    return !scan.invalidator;
}

static parse_tree_t *
find_descendant(parse_grammar_t *grammar, parse_tree_t *node,
                core_string_t name)
{
    for (parse_tree_t *at = node; at; ) {
        if (nt_is(grammar, at, name)) return at;
        if (core_tree_is_leaf(at) || core_tree_num_children(at) != 1)
            return nullptr;
        at = core_tree_child(at, 0);
    }
    return nullptr;
}

static void
collect_statements(parse_grammar_t *grammar, parse_tree_t *node,
                   core_list_t(parse_tree_ptr_t) *statements)
{
    if (!node || core_tree_is_leaf(node)) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "body-stmt"))
            core_list_push(*statements, child);
        else if (!NT_IS(grammar, child, "body"))
            collect_statements(grammar, child, statements);
    }
}

static bool
body_always_exits(parse_grammar_t *grammar, parse_tree_t *body)
{
    parse_tree_t *list = body ? CHILD_NT(grammar, body, "body-stmts") : nullptr;
    core_list_t(parse_tree_ptr_t) statements = core_list_new(parse_tree_ptr_t);
    collect_statements(grammar, list, &statements);
    parse_tree_t *last = statements.len ? statements.data[statements.len - 1]
                                        : nullptr;
    core_list_free(statements);
    if (!last) return false;
    return find_descendant(grammar, last, CORE_STRING_STATIC("return-stmt"))
        || find_descendant(grammar, last, CORE_STRING_STATIC("break-stmt"))
        || find_descendant(grammar, last, CORE_STRING_STATIC("continue-stmt"))
        || find_descendant(grammar, last, CORE_STRING_STATIC("fail-stmt"));
}

static void collect_clauses(parse_grammar_t *grammar, parse_tree_t *node,
                            core_list_t(parse_tree_ptr_t) *clauses);

static bool
if_has_alternatives(parse_grammar_t *grammar, parse_tree_t *if_stmt)
{
    core_list_t(parse_tree_ptr_t) clauses = core_list_new(parse_tree_ptr_t);
    collect_clauses(grammar, CHILD_NT(grammar, if_stmt, "elif-chain"), &clauses);
    bool any = clauses.len != 0;
    core_list_free(clauses);
    return any;
}

static bool
earlier_exit_narrows(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                     parse_tree_t *list, parse_tree_t *statement,
                     parse_tree_t *use, parse_tree_t **lost)
{
    core_list_t(parse_tree_ptr_t) statements = core_list_new(parse_tree_ptr_t);
    collect_statements(grammar, list, &statements);
    bool narrowed = false;
    for (size_t i = 0; i < statements.len && !narrowed; i++) {
        parse_tree_t *candidate = statements.data[i];
        if (candidate == statement) break;
        parse_tree_t *if_stmt = find_descendant(grammar, candidate,
                                                CORE_STRING_STATIC("if-stmt"));
        if (!if_stmt || if_has_alternatives(grammar, if_stmt)) continue;
        parse_tree_t *condition = CHILD_NT(grammar, if_stmt, "expression");
        parse_tree_t *body = CHILD_NT(grammar, if_stmt, "body");
        narrowed = nil_facts(grammar, condition, symbol).when_false
            && body_always_exits(grammar, body)
            && holds_until_use(grammar, symbol, list, use, lost);
    }
    core_list_free(statements);
    return narrowed;
}

static void
collect_clauses(parse_grammar_t *grammar, parse_tree_t *node,
                core_list_t(parse_tree_ptr_t) *clauses)
{
    if (!node || core_tree_is_leaf(node)) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (NT_IS(grammar, child, "elif-clause")
            || NT_IS(grammar, child, "else-clause"))
            core_list_push(*clauses, child);
        else
            collect_clauses(grammar, child, clauses);
    }
}

static bool
alternative_narrows(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                    parse_tree_t *clause, parse_tree_t *body,
                    parse_tree_t *use, parse_tree_t **lost)
{
    parse_tree_t *if_stmt = parse_tree_parent(clause);
    while (if_stmt && !NT_IS(grammar, if_stmt, "if-stmt"))
        if_stmt = parse_tree_parent(if_stmt);
    if (!if_stmt) return false;
    bool narrowed = nil_facts(grammar, CHILD_NT(grammar, if_stmt, "expression"),
                              symbol).when_false;
    core_list_t(parse_tree_ptr_t) clauses = core_list_new(parse_tree_ptr_t);
    collect_clauses(grammar, CHILD_NT(grammar, if_stmt, "elif-chain"), &clauses);
    for (size_t i = 0; i < clauses.len && !narrowed; i++) {
        parse_tree_t *other = clauses.data[i];
        parse_tree_t *condition = CHILD_NT(grammar, other, "expression");
        if (other == clause) {
            narrowed = condition
                && nil_facts(grammar, condition, symbol).when_true;
            break;
        }
        narrowed = condition && nil_facts(grammar, condition, symbol).when_false;
    }
    core_list_free(clauses);
    return narrowed && holds_until_use(grammar, symbol, body, use, lost);
}

static bool
use_is_narrowed(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                parse_tree_t *use, parse_tree_t **lost)
{
    parse_tree_t *child = use;
    for (parse_tree_t *parent = parse_tree_parent(use); parent;
         child = parent, parent = parse_tree_parent(parent)) {
        if (NT_IS(grammar, parent, "func-def")) return false;
        size_t children = core_tree_is_leaf(parent)
            ? 0 : core_tree_num_children(parent);
        if ((NT_IS(grammar, parent, "if-stmt")
             || NT_IS(grammar, parent, "while-stmt"))
            && NT_IS(grammar, child, "body")
            && nil_facts(grammar, CHILD_NT(grammar, parent, "expression"),
                         symbol).when_true
            && holds_until_use(grammar, symbol, child, use, lost))
            return true;
        if ((NT_IS(grammar, parent, "elif-clause")
             || NT_IS(grammar, parent, "else-clause"))
            && NT_IS(grammar, child, "body")
            && alternative_narrows(grammar, symbol, parent, child, use, lost))
            return true;
        if (NT_IS(grammar, parent, "and-expr") && children == 3
            && child == core_tree_child(parent, 2)
            && nil_facts(grammar, core_tree_child(parent, 0),
                         symbol).when_true)
            return true;
        if (NT_IS(grammar, parent, "or-expr") && children == 3
            && child == core_tree_child(parent, 2)
            && nil_facts(grammar, core_tree_child(parent, 0),
                         symbol).when_false)
            return true;
        if (NT_IS(grammar, child, "body-stmt")) {
            parse_tree_t *list = parent;
            while (list && !NT_IS(grammar, list, "body-stmt-list"))
                list = parse_tree_parent(list);
            if (list && earlier_exit_narrows(grammar, symbol, list, child, use,
                                             lost))
                return true;
        }
    }
    return false;
}

static bool
compared_with_nil(parse_grammar_t *grammar, parse_tree_t *use)
{
    for (parse_tree_t *at = parse_tree_parent(use); at;
         at = parse_tree_parent(at)) {
        if (NT_IS(grammar, at, "equality-expr")
            && core_tree_num_children(at) == 3)
            return is_nil(grammar, core_tree_child(at, 0))
                || is_nil(grammar, core_tree_child(at, 2));
        if (core_tree_is_leaf(at) || core_tree_num_children(at) != 1)
            return false;
    }
    return false;
}

static bool
receives_option_method(parse_grammar_t *grammar, parse_tree_t *use)
{
    parse_tree_t *child = use;
    for (parse_tree_t *at = parse_tree_parent(use); at;
         child = at, at = parse_tree_parent(at)) {
        if (core_tree_is_leaf(at)) return false;
        size_t children = core_tree_num_children(at);
        if (children == 1) continue;
        if (!n00bb_postfix_node(grammar, at) || children != 3
            || core_tree_child(at, 0) != child)
            return false;
        core_string_t member = parse_tree_leaf_text(core_tree_child(at, 2));
        return core_string_eq(member, CORE_STRING_STATIC("get"))
            || core_string_eq(member, CORE_STRING_STATIC("get_or_else"))
            || core_string_eq(member, CORE_STRING_STATIC("is_nil"));
    }
    return false;
}

static bool
narrowable_symbol(parse_sym_entry_t *symbol)
{
    if (!symbol || symbol->is_field || symbol->owner_type) return false;
    if (symbol->kind == PARSE_SYM_PARAM) return true;
    return symbol->kind == PARSE_SYM_VARIABLE
        && (symbol->storage_duration == PARSE_SYM_STORAGE_AUTOMATIC
            || symbol->mutability == PARSE_SYM_IMMUTABLE
            || symbol->mutability == PARSE_SYM_CONST);
}

th_type *
n00bb_narrowed_symbol_type(th_ctx *types, parse_sym_entry_t *symbol,
                           parse_tree_t *use, th_type *declared, void *context)
{
    core_translation_unit_t *unit = context;
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    parse_grammar_t *grammar = state ? state->grammar : nullptr;
    if (!types || !grammar || !use || !declared || !narrowable_symbol(symbol))
        return nullptr;
    th_type *item = th_option_item(th_resolve(declared));
    if (!item) return nullptr;
    parse_tree_t *parent = parse_tree_parent(use);
    if (is_whole_assignment_target(grammar, use)
        || compared_with_nil(grammar, use)
        || receives_option_method(grammar, use)
        || (parent && NT_IS(grammar, parent, "unary-expr")
            && core_string_eq(operator_text(core_tree_child(parent, 0)),
                              CORE_STRING_STATIC("ref"))))
        return nullptr;
    return use_is_narrowed(grammar, symbol, use, nullptr) ? item : nullptr;
}

parse_tree_t *
n00bb_narrowing_ended_by(parse_grammar_t *grammar, parse_tree_t *use)
{
    parse_tree_t *reference = use;
    while (reference && !n00bb_primary_node(grammar, reference)
           && !core_tree_is_leaf(reference)
           && core_tree_num_children(reference) == 1)
        reference = core_tree_child(reference, 0);
    if (!reference || !n00bb_primary_node(grammar, reference)) return nullptr;
    parse_sym_entry_t *symbol = referenced_symbol(reference);
    if (!narrowable_symbol(symbol)) return nullptr;
    parse_tree_t *lost = nullptr;
    return use_is_narrowed(grammar, symbol, reference, &lost) ? nullptr : lost;
}
