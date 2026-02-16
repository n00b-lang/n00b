/**
 * @file xform_vargs.c
 * @brief Transforms n00b-style variadic arguments.
 *
 * Declaration transformation:
 *   void foo(int x, +);               -> void foo(int x, n00b_vargs_t *vargs);
 *   void foo(int x, int +);           -> void foo(int x, n00b_vargs_t *vargs);
 *   void bar(const char *fmt, ...);   -> unchanged (standard C varargs)
 *
 * Call site transformation (n00b style only):
 *   foo(1, "a", "b", "c");  -> foo(1, &(n00b_vargs_t){.nargs=3, .cur_ix=0,
 *                                    .args=(void*[]){"a", "b", "c"}});
 *   foo(1);                 -> foo(1, nullptr);
 *
 * For typed n00b varargs (e.g. int +), call sites are wrapped in a statement
 * expression that includes _Static_assert checks for each variadic argument.
 *
 * Uses proper tree node operations - NO token replacement.
 */

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

#include "branch_symbols.h"
#include "lex.h"
#include "transform.h"
#include "rewrite.h"
#include "types.h"
#include "nt_types.h"
#include "st.h"

// ---------------------------------------------------------------------------
// Helper: Node Navigation
// ---------------------------------------------------------------------------

/**
 * @brief Find identifier node in a declarator subtree.
 */
static tnode_t *
find_identifier_node(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->nt_id == NT_identifier && identifier_tok(node)) {
        return node;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *found = find_identifier_node(tnode_get_kid(node, i));
        if (found) {
            return found;
        }
    }
    return nullptr;
}

/**
 * @brief Get identifier text from a declarator.
 */
static const char *
get_identifier_text(ncc_buf_t *input, tnode_t *declarator)
{
    tnode_t *id_node = find_identifier_node(declarator);
    if (id_node) {
        tok_t *tok = identifier_tok(id_node);
        if (tok) {
            if (tok->replacement) {
                return tok->replacement->data;
            }
            return extract(input, tok);
        }
    }
    return nullptr;
}

/**
 * @brief Find parameter_type_list in a declarator.
 */
static tnode_t *
find_param_type_list(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->nt_id == NT_parameter_type_list) {
        return node;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *found = find_param_type_list(tnode_get_kid(node, i));
        if (found) {
            return found;
        }
    }
    return nullptr;
}

/**
 * @brief Find the '+' operator node in parameter_type_list.
 */
static tnode_t *
find_plus_node(tnode_t *param_type_list)
{
    if (!param_type_list) {
        return nullptr;
    }
    for (int i = 0; i < param_type_list->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(param_type_list, i);
        if (kid && kid->tptr && kid->nt && strcmp(kid->nt, "+") == 0) {
            return kid;
        }
    }
    return nullptr;
}

/**
 * @brief Insert a child node at a given index, shifting later children right.
 */
static void
insert_child_at(tnode_t *parent, int idx, tnode_t *child)
{
    if (!parent || !child || !parent->kids || idx < 0 || idx > parent->num_kids) {
        return;
    }
    for (int i = parent->num_kids; i > idx; i--) {
        parent->kids->items[i] = parent->kids->items[i - 1];
    }
    parent->kids->items[idx] = child;
    parent->num_kids++;
    child->parent = parent;
}

/**
 * @brief Iterative DFS search for a node with specific nt_id.
 */
static tnode_t *
find_node_recursive(tnode_t *node, nt_type_t nt_id)
{
    if (!node) {
        return nullptr;
    }
    if (node->nt_id == nt_id) {
        return node;
    }

    int       cap = 32;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return nullptr;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid) {
                continue;
            }
            if (kid->nt_id == nt_id) {
                base_dealloc(stk);
                return kid;
            }
            if (top >= cap) {
                cap *= 2;
                stk = base_realloc(stk, cap * sizeof(tnode_t *));
            }
            stk[top++] = kid;
        }
    }

    base_dealloc(stk);
    return nullptr;
}

/**
 * @brief Find index of child in parent.
 */
static int
find_child_index(tnode_t *parent, tnode_t *child)
{
    if (!parent || !child) {
        return -1;
    }
    for (int i = 0; i < parent->num_kids; i++) {
        if (tnode_get_kid(parent, i) == child) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Remove a child from parent at given index.
 */
static void
remove_child_at(tnode_t *parent, int idx)
{
    if (!parent || !parent->kids || idx < 0 || idx >= parent->num_kids) {
        return;
    }
    for (int i = idx; i < parent->num_kids - 1; i++) {
        parent->kids->items[i] = parent->kids->items[i + 1];
    }
    parent->num_kids--;
}

/**
 * @brief Replace a child at given index.
 */
static void
replace_child_at(tnode_t *parent, int idx, tnode_t *new_child)
{
    if (!parent || !parent->kids || !new_child || idx < 0 || idx >= parent->num_kids) {
        return;
    }
    parent->kids->items[idx] = new_child;
    new_child->parent = parent;
}

// ---------------------------------------------------------------------------
// Helper: Tree Building
// ---------------------------------------------------------------------------

/**
 * @brief Build an identifier node.
 */
static tnode_t *
build_identifier(const char *name, int line)
{
    tnode_t *id = synth_nonterminal("identifier");
    id->nt_id   = NT_identifier;
    add_child(id, synth_terminal(name, TT_ID, line));
    return id;
}

/**
 * @brief Wrap an expression in the full expression hierarchy.
 */
static tnode_t *
wrap_in_expr_hierarchy(tnode_t *inner, int line)
{
    (void)line;

    tnode_t *unary = synth_nonterminal("unary_expression_9");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 9;
    add_child(unary, inner);

    tnode_t *cast = synth_nonterminal("cast_expression_1");
    cast->nt_id   = NT_cast_expression;
    cast->branch  = 1;
    add_child(cast, unary);

    tnode_t *mult = synth_nonterminal("multiplicative_expression_3");
    mult->nt_id   = NT_multiplicative_expression;
    mult->branch  = 3;
    add_child(mult, cast);

    tnode_t *add = synth_nonterminal("additive_expression_2");
    add->nt_id   = NT_additive_expression;
    add->branch  = 2;
    add_child(add, mult);

    tnode_t *shift = synth_nonterminal("shift_expression_2");
    shift->nt_id   = NT_shift_expression;
    shift->branch  = 2;
    add_child(shift, add);

    tnode_t *rel = synth_nonterminal("relational_expression_4");
    rel->nt_id   = NT_relational_expression;
    rel->branch  = 4;
    add_child(rel, shift);

    tnode_t *eq = synth_nonterminal("equality_expression_2");
    eq->nt_id   = NT_equality_expression;
    eq->branch  = 2;
    add_child(eq, rel);

    tnode_t *and_ = synth_nonterminal("AND_expression_1");
    and_->nt_id   = NT_AND_expression;
    and_->branch  = 1;
    add_child(and_, eq);

    tnode_t *xor_ = synth_nonterminal("exclusive_OR_expression_1");
    xor_->nt_id   = NT_exclusive_OR_expression;
    xor_->branch  = 1;
    add_child(xor_, and_);

    tnode_t *or_ = synth_nonterminal("inclusive_OR_expression_1");
    or_->nt_id   = NT_inclusive_OR_expression;
    or_->branch  = 1;
    add_child(or_, xor_);

    tnode_t *land = synth_nonterminal("logical_AND_expression_1");
    land->nt_id   = NT_logical_AND_expression;
    land->branch  = 1;
    add_child(land, or_);

    tnode_t *lor = synth_nonterminal("logical_OR_expression_1");
    lor->nt_id   = NT_logical_OR_expression;
    lor->branch  = 1;
    add_child(lor, land);

    tnode_t *cond = synth_nonterminal("conditional_expression_1");
    cond->nt_id   = NT_conditional_expression;
    cond->branch  = 1;
    add_child(cond, lor);

    tnode_t *assign = synth_nonterminal("assignment_expression_1");
    assign->nt_id   = NT_assignment_expression;
    assign->branch  = 1;
    add_child(assign, cond);

    return assign;
}

/**
 * @brief Build a designation for designated initializer: .member =
 */
static tnode_t *
build_designation(const char *member, int line)
{
    tnode_t *desig_list = synth_nonterminal("designator_list_1");
    desig_list->nt_id   = NT_designator_list;
    desig_list->branch  = 1;

    tnode_t *desig = synth_nonterminal("designator_0");
    desig->nt_id   = NT_designator;
    desig->branch  = 0;
    add_child(desig, synth_terminal(".", TT_PUNCT, line));
    add_child(desig, build_identifier(member, line));
    add_child(desig_list, desig);

    tnode_t *designation = synth_nonterminal("designation_0");
    designation->nt_id   = NT_designation;
    designation->branch  = 0;
    add_child(designation, desig_list);
    add_child(designation, synth_terminal("=", TT_PUNCT, line));

    return designation;
}

/**
 * @brief Build parameter_declaration for n00b_vargs_t *vargs
 */
static tnode_t *
build_vargs_param(int line)
{
    tnode_t *param_decl = synth_nonterminal("parameter_declaration_0");
    param_decl->nt_id   = NT_parameter_declaration;
    param_decl->branch  = 0;

    // declaration_specifiers: n00b_vargs_t
    tnode_t *decl_specs = synth_nonterminal("declaration_specifiers_1");
    decl_specs->nt_id   = NT_declaration_specifiers;
    decl_specs->branch  = 1;

    tnode_t *decl_spec = synth_nonterminal("declaration_specifier_1");
    decl_spec->nt_id   = NT_declaration_specifier;
    decl_spec->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_5");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 5;

    tnode_t *typedef_name = synth_nonterminal("typedef_name_0");
    typedef_name->nt_id   = NT_typedef_name;
    typedef_name->branch  = 0;
    add_child(typedef_name, synth_terminal("n00b_vargs_t", TT_ID, line));

    add_child(type_spec, typedef_name);
    add_child(tsq, type_spec);
    add_child(decl_spec, tsq);
    add_child(decl_specs, decl_spec);
    add_child(param_decl, decl_specs);

    // declarator: *vargs
    tnode_t *declarator = synth_nonterminal("declarator_0");
    declarator->nt_id   = NT_declarator;
    declarator->branch  = 0;

    tnode_t *pointer = synth_nonterminal("pointer_0");
    pointer->nt_id   = NT_pointer;
    pointer->branch  = 0;
    add_child(pointer, synth_terminal("*", TT_PUNCT, line));

    tnode_t *direct_decl = synth_nonterminal("direct_declarator_3");
    direct_decl->nt_id   = NT_direct_declarator;
    direct_decl->branch  = 3;
    add_child(direct_decl, build_identifier("vargs", line));

    add_child(declarator, pointer);
    add_child(declarator, direct_decl);
    add_child(param_decl, declarator);

    return param_decl;
}

/**
 * @brief Build a nullptr primary expression.
 */
static tnode_t *
build_nullptr_expr(int line)
{
    tnode_t *primary = synth_nonterminal("primary_expression_7");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 7;
    add_child(primary, build_identifier("nullptr", line));
    return primary;
}

/**
 * @brief Wrap an assignment_expression in parentheses to make it a primary_expression.
 * This is needed when the expression needs to be used as a cast operand.
 */
static tnode_t *
wrap_in_parens(tnode_t *assign_expr, int line)
{
    // primary_expression_6: '(' expression ')'
    tnode_t *primary = synth_nonterminal("primary_expression_6");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 6;

    add_child(primary, synth_terminal("(", TT_PUNCT, line));

    // expression wrapping assignment_expression
    tnode_t *expr = synth_nonterminal("expression_1");
    expr->nt_id   = NT_expression;
    expr->branch  = 1;
    add_child(expr, assign_expr);

    add_child(primary, expr);
    add_child(primary, synth_terminal(")", TT_PUNCT, line));

    return primary;
}

/**
 * @brief Build a cast expression: (void *)(expr)
 * Takes an assignment_expression and wraps it in parentheses, then casts to void*.
 */
static tnode_t *
build_void_ptr_cast(tnode_t *assign_expr, int line)
{
    // First wrap the expression in parentheses
    tnode_t *paren_expr = wrap_in_parens(assign_expr, line);

    // Build cast_expression_0: ( type_name ) cast_expression
    tnode_t *cast = synth_nonterminal("cast_expression_0");
    cast->nt_id   = NT_cast_expression;
    cast->branch  = 0;

    add_child(cast, synth_terminal("(", TT_PUNCT, line));

    // type_name for void*
    tnode_t *type_name = synth_nonterminal("type_name_1");
    type_name->nt_id   = NT_type_name;
    type_name->branch  = 1;

    tnode_t *spec_qual_list = synth_nonterminal("specifier_qualifier_list_1");
    spec_qual_list->nt_id   = NT_specifier_qualifier_list;
    spec_qual_list->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *ts = synth_nonterminal("type_specifier_0");
    ts->nt_id   = NT_type_specifier;
    ts->branch  = 0;
    add_child(ts, synth_terminal("void", TT_KEYWORD, line));

    add_child(tsq, ts);
    add_child(spec_qual_list, tsq);
    add_child(type_name, spec_qual_list);

    // abstract_declarator: *
    tnode_t *abs_decl = synth_nonterminal("abstract_declarator_1");
    abs_decl->nt_id   = NT_abstract_declarator;
    abs_decl->branch  = 1;

    tnode_t *pointer = synth_nonterminal("pointer_0");
    pointer->nt_id   = NT_pointer;
    pointer->branch  = 0;
    add_child(pointer, synth_terminal("*", TT_PUNCT, line));

    add_child(abs_decl, pointer);
    add_child(type_name, abs_decl);

    add_child(cast, type_name);
    add_child(cast, synth_terminal(")", TT_PUNCT, line));

    // Wrap the parenthesized expression in hierarchy to get to cast_expression
    tnode_t *postfix = synth_nonterminal("postfix_expression_0");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = 0;
    add_child(postfix, paren_expr);

    tnode_t *unary = synth_nonterminal("unary_expression_9");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 9;
    add_child(unary, postfix);

    tnode_t *inner_cast = synth_nonterminal("cast_expression_1");
    inner_cast->nt_id   = NT_cast_expression;
    inner_cast->branch  = 1;
    add_child(inner_cast, unary);

    add_child(cast, inner_cast);

    // Now wrap cast in expression hierarchy up to assignment_expression
    tnode_t *mult = synth_nonterminal("multiplicative_expression_3");
    mult->nt_id   = NT_multiplicative_expression;
    mult->branch  = 3;
    add_child(mult, cast);

    tnode_t *add = synth_nonterminal("additive_expression_2");
    add->nt_id   = NT_additive_expression;
    add->branch  = 2;
    add_child(add, mult);

    tnode_t *shift = synth_nonterminal("shift_expression_2");
    shift->nt_id   = NT_shift_expression;
    shift->branch  = 2;
    add_child(shift, add);

    tnode_t *rel = synth_nonterminal("relational_expression_4");
    rel->nt_id   = NT_relational_expression;
    rel->branch  = 4;
    add_child(rel, shift);

    tnode_t *eq = synth_nonterminal("equality_expression_2");
    eq->nt_id   = NT_equality_expression;
    eq->branch  = 2;
    add_child(eq, rel);

    tnode_t *and_ = synth_nonterminal("AND_expression_1");
    and_->nt_id   = NT_AND_expression;
    and_->branch  = 1;
    add_child(and_, eq);

    tnode_t *xor_ = synth_nonterminal("exclusive_OR_expression_1");
    xor_->nt_id   = NT_exclusive_OR_expression;
    xor_->branch  = 1;
    add_child(xor_, and_);

    tnode_t *or_ = synth_nonterminal("inclusive_OR_expression_1");
    or_->nt_id   = NT_inclusive_OR_expression;
    or_->branch  = 1;
    add_child(or_, xor_);

    tnode_t *land = synth_nonterminal("logical_AND_expression_1");
    land->nt_id   = NT_logical_AND_expression;
    land->branch  = 1;
    add_child(land, or_);

    tnode_t *lor = synth_nonterminal("logical_OR_expression_1");
    lor->nt_id   = NT_logical_OR_expression;
    lor->branch  = 1;
    add_child(lor, land);

    tnode_t *cond = synth_nonterminal("conditional_expression_1");
    cond->nt_id   = NT_conditional_expression;
    cond->branch  = 1;
    add_child(cond, lor);

    tnode_t *assign = synth_nonterminal("assignment_expression_1");
    assign->nt_id   = NT_assignment_expression;
    assign->branch  = 1;
    add_child(assign, cond);

    return assign;
}

/**
 * @brief Build unary & (address-of) expression.
 */
static tnode_t *
build_address_of(tnode_t *operand, int line)
{
    tnode_t *unary_op = synth_nonterminal("unary_operator");
    unary_op->nt_id   = NT_unary_operator;
    add_child(unary_op, synth_terminal("&", TT_PUNCT, line));

    tnode_t *cast = synth_nonterminal("cast_expression_1");
    cast->nt_id   = NT_cast_expression;
    cast->branch  = 1;

    tnode_t *inner_unary = synth_nonterminal("unary_expression_9");
    inner_unary->nt_id   = NT_unary_expression;
    inner_unary->branch  = 9;
    add_child(inner_unary, operand);
    add_child(cast, inner_unary);

    tnode_t *unary = synth_nonterminal("unary_expression_1");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 1;
    add_child(unary, unary_op);
    add_child(unary, cast);

    return unary;
}

/**
 * @brief Build a compound literal for vargs.
 * (n00b_vargs_t){.nargs=N, .cur_ix=0, .args=(void*[]){...}}
 */
static tnode_t *
build_vargs_compound_literal(list_t *varg_exprs, int line)
{
    int nargs = list_len(varg_exprs);

    tnode_t *literal = synth_nonterminal("compound_literal_1");
    literal->nt_id   = NT_compound_literal;
    literal->branch  = 1;

    add_child(literal, synth_terminal("(", TT_PUNCT, line));

    // type_name: n00b_vargs_t
    tnode_t *type_name = synth_nonterminal("type_name_0");
    type_name->nt_id   = NT_type_name;
    type_name->branch  = 0;

    tnode_t *spec_qual_list = synth_nonterminal("specifier_qualifier_list_1");
    spec_qual_list->nt_id   = NT_specifier_qualifier_list;
    spec_qual_list->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_5");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 5;

    tnode_t *typedef_name = synth_nonterminal("typedef_name_0");
    typedef_name->nt_id   = NT_typedef_name;
    typedef_name->branch  = 0;
    add_child(typedef_name, synth_terminal("n00b_vargs_t", TT_ID, line));

    add_child(type_spec, typedef_name);
    add_child(tsq, type_spec);
    add_child(spec_qual_list, tsq);
    add_child(type_name, spec_qual_list);

    add_child(literal, type_name);
    add_child(literal, synth_terminal(")", TT_PUNCT, line));

    // Braced initializer
    tnode_t *braced = synth_nonterminal("braced_initializer_0");
    braced->nt_id   = NT_braced_initializer;
    braced->branch  = 0;

    add_child(braced, synth_terminal("{", TT_PUNCT, line));

    // Build initializer_list with .nargs, .cur_ix, .args
    tnode_t *init_list = synth_nonterminal("initializer_list");
    init_list->nt_id   = NT_initializer_list;

    // .nargs = N
    add_child(init_list, build_designation("nargs", line));
    tnode_t *nargs_init = synth_nonterminal("initializer_1");
    nargs_init->nt_id   = NT_initializer;
    nargs_init->branch  = 1;

    char nargs_str[16];
    snprintf(nargs_str, sizeof(nargs_str), "%d", nargs);
    tnode_t *nargs_const = synth_nonterminal("constant");
    nargs_const->nt_id   = NT_constant;
    add_child(nargs_const, synth_terminal(nargs_str, TT_NUM, line));
    tnode_t *nargs_prim = synth_nonterminal("primary_expression_1");
    nargs_prim->nt_id   = NT_primary_expression;
    nargs_prim->branch  = 1;
    add_child(nargs_prim, nargs_const);
    add_child(nargs_init, wrap_in_expr_hierarchy(nargs_prim, line));
    add_child(init_list, nargs_init);

    add_child(init_list, synth_terminal(",", TT_PUNCT, line));

    // .cur_ix = 0
    add_child(init_list, build_designation("cur_ix", line));
    tnode_t *curix_init = synth_nonterminal("initializer_1");
    curix_init->nt_id   = NT_initializer;
    curix_init->branch  = 1;

    tnode_t *zero_const = synth_nonterminal("constant");
    zero_const->nt_id   = NT_constant;
    add_child(zero_const, synth_terminal("0", TT_NUM, line));
    tnode_t *zero_prim = synth_nonterminal("primary_expression_1");
    zero_prim->nt_id   = NT_primary_expression;
    zero_prim->branch  = 1;
    add_child(zero_prim, zero_const);
    add_child(curix_init, wrap_in_expr_hierarchy(zero_prim, line));
    add_child(init_list, curix_init);

    add_child(init_list, synth_terminal(",", TT_PUNCT, line));

    // .args = (void*[]){...}
    add_child(init_list, build_designation("args", line));

    // Build compound literal for args: (void*[]){arg1, arg2, ...}
    tnode_t *args_init = synth_nonterminal("initializer_1");
    args_init->nt_id   = NT_initializer;
    args_init->branch  = 1;

    tnode_t *args_literal = synth_nonterminal("compound_literal_1");
    args_literal->nt_id   = NT_compound_literal;
    args_literal->branch  = 1;

    add_child(args_literal, synth_terminal("(", TT_PUNCT, line));

    // type_name: void*[]
    tnode_t *args_type = synth_nonterminal("type_name_1");
    args_type->nt_id   = NT_type_name;
    args_type->branch  = 1;

    tnode_t *args_sql = synth_nonterminal("specifier_qualifier_list_1");
    args_sql->nt_id   = NT_specifier_qualifier_list;
    args_sql->branch  = 1;

    tnode_t *args_tsq = synth_nonterminal("type_specifier_qualifier_0");
    args_tsq->nt_id   = NT_type_specifier_qualifier;
    args_tsq->branch  = 0;

    tnode_t *args_ts = synth_nonterminal("type_specifier_0");
    args_ts->nt_id   = NT_type_specifier;
    args_ts->branch  = 0;
    add_child(args_ts, synth_terminal("void", TT_KEYWORD, line));

    add_child(args_tsq, args_ts);
    add_child(args_sql, args_tsq);
    add_child(args_type, args_sql);

    // abstract_declarator: *[]
    tnode_t *abs_decl = synth_nonterminal("abstract_declarator_0");
    abs_decl->nt_id   = NT_abstract_declarator;
    abs_decl->branch  = 0;

    tnode_t *abs_pointer = synth_nonterminal("pointer_0");
    abs_pointer->nt_id   = NT_pointer;
    abs_pointer->branch  = 0;
    add_child(abs_pointer, synth_terminal("*", TT_PUNCT, line));

    tnode_t *dabs = synth_nonterminal("direct_abstract_declarator_1");
    dabs->nt_id   = NT_direct_abstract_declarator;
    dabs->branch  = 1;

    tnode_t *array_abs = synth_nonterminal("array_abstract_declarator_0");
    array_abs->nt_id   = NT_array_abstract_declarator;
    array_abs->branch  = 0;
    add_child(array_abs, synth_terminal("[", TT_PUNCT, line));
    add_child(array_abs, synth_terminal("]", TT_PUNCT, line));

    add_child(dabs, array_abs);
    add_child(abs_decl, abs_pointer);
    add_child(abs_decl, dabs);
    add_child(args_type, abs_decl);

    add_child(args_literal, args_type);
    add_child(args_literal, synth_terminal(")", TT_PUNCT, line));

    // Inner braced initializer for args
    tnode_t *inner_braced = synth_nonterminal("braced_initializer_0");
    inner_braced->nt_id   = NT_braced_initializer;
    inner_braced->branch  = 0;

    add_child(inner_braced, synth_terminal("{", TT_PUNCT, line));

    tnode_t *inner_init_list = synth_nonterminal("initializer_list");
    inner_init_list->nt_id   = NT_initializer_list;

    // Add each variadic argument, cast to (void*)
    for (int i = 0; i < nargs; i++) {
        if (i > 0) {
            add_child(inner_init_list, synth_terminal(",", TT_PUNCT, line));
        }
        tnode_t *arg = list_get(varg_exprs, i);
        // Cast the argument to (void*) for storage in the void*[] array
        tnode_t *casted_arg = build_void_ptr_cast(arg, line);
        tnode_t *arg_init = synth_nonterminal("initializer_1");
        arg_init->nt_id   = NT_initializer;
        arg_init->branch  = 1;
        add_child(arg_init, casted_arg);
        add_child(inner_init_list, arg_init);
    }

    add_child(inner_braced, inner_init_list);
    add_child(inner_braced, synth_terminal("}", TT_PUNCT, line));
    add_child(args_literal, inner_braced);

    // Wrap args_literal in expression hierarchy
    add_child(args_init, wrap_in_expr_hierarchy(args_literal, line));
    add_child(init_list, args_init);

    add_child(braced, init_list);
    add_child(braced, synth_terminal("}", TT_PUNCT, line));

    add_child(literal, braced);

    return literal;
}

// ---------------------------------------------------------------------------
// Declaration Transform
// ---------------------------------------------------------------------------

/**
 * @brief Transform function declaration/definition with variadic arguments.
 */
static tnode_t *
xform_vargs_decl(xform_ctx_t *ctx, tnode_t *node, bool is_definition)
{
    (void)is_definition;

    tnode_t *declarator = find_node_recursive(node, NT_declarator);
    if (!declarator) {
        return nullptr;
    }

    const char *func_name = get_identifier_text(ctx->input, declarator);
    if (!func_name) {
        return nullptr;
    }

    vargs_info_t *vargs_info = nullptr;
    if (ctx->symtab) {
        vargs_info = st_get_vargs_info(ctx->symtab, (char *)func_name);
    }

    if (!vargs_info) {
        return nullptr;
    }

    tnode_t *param_type_list = find_param_type_list(declarator);
    if (!param_type_list) {
        return nullptr;
    }

    int line = get_node_line(node);

    if (vargs_info->style == VARGS_N00B) {
        // Find and replace '+' with n00b_vargs_t *vargs parameter
        tnode_t *plus = find_plus_node(param_type_list);
        if (!plus) {
            return nullptr;
        }

        // Skip transformation if plus is from a system header (ncc_off=true)
        if (plus->tptr && lex_tok_is_ncc_off(ctx->lex, plus->tptr)) {
            return nullptr;
        }

        tnode_t *vargs_param = build_vargs_param(line);

        if (param_type_list->branch == BRANCH(parameter_type_list, N00B_VA_PLUS)) {
            // Branch 0: parameter_list '+' (typed varargs)
            // The last parameter_declaration in the parameter_list is the type.
            // Remove it, insert comma, and replace '+' with vargs param.
            tnode_t *param_list = find_node_recursive(param_type_list,
                                                      NT_parameter_list);
            if (param_list && vargs_info->type_node) {
                // Find and remove the last parameter_declaration (it's the type)
                for (int i = param_list->num_kids - 1; i >= 0; i--) {
                    tnode_t *kid = tnode_get_kid(param_list, i);
                    if (kid && kid->nt_id == NT_parameter_declaration) {
                        remove_child_at(param_list, i);
                        break;
                    }
                }
            }

            // Replace '+' with vargs param
            int plus_idx = find_child_index(param_type_list, plus);
            if (plus_idx < 0) {
                return nullptr;
            }
            replace_child_at(param_type_list, plus_idx, vargs_param);

            if (param_list && vargs_info->positional_before == 0) {
                // No positional params — remove the empty param_list
                int pl_idx = find_child_index(param_type_list, param_list);
                if (pl_idx >= 0) {
                    remove_child_at(param_type_list, pl_idx);
                }
            }
            else {
                // Insert comma between parameter_list and vargs_param
                int vp_idx = find_child_index(param_type_list, vargs_param);
                if (vp_idx > 0) {
                    insert_child_at(param_type_list, vp_idx,
                                    synth_terminal(",", TT_PUNCT, line));
                }
            }
        }
        else if (param_type_list->branch == BRANCH(parameter_type_list, N00B_VA_COMMA_PLUS)) {
            // Branch 1: parameter_list ',' '+'
            // Keep comma, replace '+' with vargs param
            int plus_idx = find_child_index(param_type_list, plus);
            if (plus_idx < 0) {
                return nullptr;
            }
            replace_child_at(param_type_list, plus_idx, vargs_param);
        }
        else {
            // Branch 2: '+' only
            int plus_idx = find_child_index(param_type_list, plus);
            if (plus_idx < 0) {
                return nullptr;
            }
            replace_child_at(param_type_list, plus_idx, vargs_param);
        }
    }
    // VARGS_CSTD: bare '...' passes through unchanged — no transform needed

    return nullptr;
}

/**
 * @brief Transform function_definition with variadic arguments.
 */
static tnode_t *
xform_vargs_func_def(xform_ctx_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(function_definition, WITH_KEYWORDS)
        && node->branch != BRANCH(function_definition, WITHOUT_KEYWORDS)) {
        return nullptr;
    }
    return xform_vargs_decl(ctx, node, true);
}

/**
 * @brief Transform declaration with variadic arguments.
 */
static tnode_t *
xform_vargs_declaration(xform_ctx_t *ctx, tnode_t *node)
{
    return xform_vargs_decl(ctx, node, false);
}

// ---------------------------------------------------------------------------
// Typed Varargs: Static Assert Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Build a _Static_assert declaration checking type compatibility.
 *
 * Produces: _Static_assert(__builtin_types_compatible_p(typeof(expr), T), "msg");
 *
 * where expr is a copy of the vararg expression and T is a copy of the type_name.
 */
static tnode_t *
build_static_assert_type_check(tnode_t *arg_expr, tnode_t *type_node, int line)
{
    // static_assert_declaration_0:
    //   _Static_assert ( constant_expression , string_literal ) ;
    tnode_t *sa = synth_nonterminal("static_assert_declaration_0");
    sa->nt_id   = NT_static_assert_declaration;
    sa->branch  = 0;

    add_child(sa, synth_terminal("_Static_assert", TT_KEYWORD, line));
    add_child(sa, synth_terminal("(", TT_PUNCT, line));

    // primary_expression_8: __builtin_types_compatible_p ( type_name , type_name )
    tnode_t *compat = synth_nonterminal("primary_expression_8");
    compat->nt_id   = NT_primary_expression;
    compat->branch  = 8;

    add_child(compat, synth_terminal("__builtin_types_compatible_p", TT_ID, line));
    add_child(compat, synth_terminal("(", TT_PUNCT, line));

    // First type_name: typeof(expr)
    // type_name_0: specifier_qualifier_list [abstract_declarator]
    tnode_t *typeof_type = synth_nonterminal("type_name_0");
    typeof_type->nt_id   = NT_type_name;
    typeof_type->branch  = 0;

    tnode_t *sql1 = synth_nonterminal("specifier_qualifier_list_1");
    sql1->nt_id   = NT_specifier_qualifier_list;
    sql1->branch  = 1;

    tnode_t *tsq1 = synth_nonterminal("type_specifier_qualifier_0");
    tsq1->nt_id   = NT_type_specifier_qualifier;
    tsq1->branch  = 0;

    // typeof_specifier_0: typeof ( expression )
    tnode_t *typeof_spec = synth_nonterminal("typeof_specifier_0");
    typeof_spec->nt_id   = NT_typeof_specifier;
    typeof_spec->branch  = 0;

    // typeof_specifier_argument_1: expression
    tnode_t *typeof_arg = synth_nonterminal("typeof_specifier_argument_1");
    typeof_arg->nt_id   = NT_typeof_specifier_argument;
    typeof_arg->branch  = 1;

    // Wrap arg_expr (a copy) in expression hierarchy
    tnode_t *expr_wrap = synth_nonterminal("expression_1");
    expr_wrap->nt_id   = NT_expression;
    expr_wrap->branch  = 1;
    add_child(expr_wrap, wrap_in_expr_hierarchy(arg_expr, line));
    add_child(typeof_arg, expr_wrap);

    add_child(typeof_spec, synth_terminal("typeof", TT_KEYWORD, line));
    add_child(typeof_spec, synth_terminal("(", TT_PUNCT, line));
    add_child(typeof_spec, typeof_arg);
    add_child(typeof_spec, synth_terminal(")", TT_PUNCT, line));

    tnode_t *ts1 = synth_nonterminal("type_specifier_6");
    ts1->nt_id   = NT_type_specifier;
    ts1->branch  = 6;
    add_child(ts1, typeof_spec);

    add_child(tsq1, ts1);
    add_child(sql1, tsq1);
    add_child(typeof_type, sql1);

    add_child(compat, typeof_type);
    add_child(compat, synth_terminal(",", TT_PUNCT, line));

    // Second type_name: the declared type (deep copy)
    add_child(compat, copy_tree(type_node));

    add_child(compat, synth_terminal(")", TT_PUNCT, line));

    // Now wrap compat in expression hierarchy to make it a constant_expression
    add_child(sa, wrap_in_expr_hierarchy(compat, line));

    // Comma and message string
    add_child(sa, synth_terminal(",", TT_PUNCT, line));

    tnode_t *msg = synth_nonterminal("string_literal");
    msg->nt_id   = NT_string_literal;
    add_child(msg, synth_terminal("\"vararg type mismatch\"", TT_STR, line));
    add_child(sa, msg);

    add_child(sa, synth_terminal(")", TT_PUNCT, line));
    add_child(sa, synth_terminal(";", TT_PUNCT, line));

    return sa;
}

/**
 * @brief Wrap a call expression in a statement expression with _Static_assert checks.
 *
 * Produces: ({ _Static_assert(...); ...; original_call; })
 */
static tnode_t *
wrap_call_with_type_checks(tnode_t *call_node, list_t *varg_copies, tnode_t *type_node, int line)
{
    int nvargs = list_len(varg_copies);

    // Build block_item_list
    tnode_t *block_list = synth_nonterminal("block_item_list");
    block_list->nt_id   = NT_block_item_list;

    // Add _Static_assert for each vararg
    for (int i = 0; i < nvargs; i++) {
        tnode_t *arg_copy = list_get(varg_copies, i);
        tnode_t *sa_decl  = build_static_assert_type_check(arg_copy, type_node, line);

        // Wrap in declaration_2 (static_assert_declaration)
        tnode_t *decl = synth_nonterminal("declaration_2");
        decl->nt_id   = NT_declaration;
        decl->branch  = 2;
        add_child(decl, sa_decl);

        // block_item_0: declaration
        tnode_t *item = synth_nonterminal("block_item_0");
        item->nt_id   = NT_block_item;
        item->branch  = 0;
        add_child(item, decl);

        add_child(block_list, item);
    }

    // Add the original call expression as final expression statement
    // expression_statement_1: expression ;
    tnode_t *expr_stmt = synth_nonterminal("expression_statement_1");
    expr_stmt->nt_id   = NT_expression_statement;
    expr_stmt->branch  = 1;

    tnode_t *expr_wrap = synth_nonterminal("expression_1");
    expr_wrap->nt_id   = NT_expression;
    expr_wrap->branch  = 1;
    add_child(expr_wrap, wrap_in_expr_hierarchy(call_node, line));
    add_child(expr_stmt, expr_wrap);
    add_child(expr_stmt, synth_terminal(";", TT_PUNCT, line));

    // unlabeled_statement_0: expression_statement
    tnode_t *unlabeled = synth_nonterminal("unlabeled_statement_0");
    unlabeled->nt_id   = NT_unlabeled_statement;
    unlabeled->branch  = 0;
    add_child(unlabeled, expr_stmt);

    // block_item_2: unlabeled_statement
    tnode_t *final_item = synth_nonterminal("block_item_2");
    final_item->nt_id   = NT_block_item;
    final_item->branch  = 2;
    add_child(final_item, unlabeled);
    add_child(block_list, final_item);

    // compound_statement_0: { block_item_list }
    tnode_t *compound = synth_nonterminal("compound_statement_0");
    compound->nt_id   = NT_compound_statement;
    compound->branch  = 0;
    add_child(compound, synth_terminal("{", TT_PUNCT, line));
    add_child(compound, block_list);
    add_child(compound, synth_terminal("}", TT_PUNCT, line));

    // primary_expression_5: ( compound_statement )  -- statement expression
    tnode_t *result = synth_nonterminal("primary_expression_5");
    result->nt_id   = NT_primary_expression;
    result->branch  = 5;
    add_child(result, synth_terminal("(", TT_PUNCT, line));
    add_child(result, compound);
    add_child(result, synth_terminal(")", TT_PUNCT, line));

    return result;
}

// ---------------------------------------------------------------------------
// Call Site Transform
// ---------------------------------------------------------------------------

/**
 * @brief Find the function name from a postfix_expression (the callee).
 */
static tnode_t *
find_func_identifier(tnode_t *callee)
{
    if (!callee) {
        return nullptr;
    }

    if (callee->nt_id == NT_identifier) {
        return callee;
    }

    if (callee->nt_id == NT_primary_expression || callee->nt_id == NT_postfix_expression) {
        for (int i = 0; i < callee->num_kids; i++) {
            tnode_t *found = find_func_identifier(tnode_get_kid(callee, i));
            if (found) {
                return found;
            }
        }
    }

    for (int i = 0; i < callee->num_kids; i++) {
        tnode_t *found = find_func_identifier(tnode_get_kid(callee, i));
        if (found) {
            return found;
        }
    }

    return nullptr;
}

/**
 * @brief Get function name as string from identifier node.
 */
static const char *
get_func_name(ncc_buf_t *input, tnode_t *id_node)
{
    tok_t *tok = id_node ? identifier_tok(id_node) : nullptr;
    if (!tok) {
        return nullptr;
    }

    if (tok->replacement) {
        return tok->replacement->data;
    }

    return extract(input, tok);
}

/**
 * @brief Check if a node contains a kw_func() call (iterative DFS).
 */
static bool
contains_kw_func_call(ncc_buf_t *input, tnode_t *node)
{
    if (!node) {
        return false;
    }

    int       cap = 32;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return false;
    }

    stk[top++] = node;

    while (top > 0) {
        tnode_t *n = stk[--top];

        if (n->nt_id == NT_postfix_expression
            && n->branch == BRANCH(postfix_expression, CALL)) {
            tnode_t *callee  = tnode_get_kid(n, 0);
            tnode_t *func_id = find_func_identifier(callee);
            if (func_id) {
                const char *name = get_func_name(input, func_id);
                if (name && strcmp(name, "kw_func") == 0) {
                    base_dealloc(stk);
                    return true;
                }
            }
        }

        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid) {
                continue;
            }
            if (top >= cap) {
                cap *= 2;
                stk = base_realloc(stk, cap * sizeof(tnode_t *));
            }
            stk[top++] = kid;
        }
    }

    base_dealloc(stk);
    return false;
}

/**
 * @brief Count total arguments in an argument_expression_list.
 * Does not count keyword arguments or kw_func() calls.
 */
static int
count_args(ncc_buf_t *input, tnode_t *arg_list)
{
    if (!arg_list) {
        return 0;
    }

    if (arg_list->nt_id == NT_assignment_expression) {
        // Don't count if this expression contains a kw_func() call
        if (contains_kw_func_call(input, arg_list)) {
            return 0;
        }
        return 1;
    }

    if (arg_list->nt_id == NT_keyword_argument) {
        return 0;
    }

    if (arg_list->nt_id == NT_argument_expression_list) {
        int count = 0;
        for (int i = 0; i < arg_list->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(arg_list, i);
            if (kid) {
                count += count_args(input, kid);
            }
        }
        return count;
    }

    return 0;
}

/**
 * @brief Collect variadic arguments into a list.
 * Stops when it hits a keyword argument or kw_func() call.
 * @note list_append frees the old list and returns a new one,
 *       so we take a pointer-to-pointer to update the caller's list.
 */
static void
collect_vargs(ncc_buf_t *input, tnode_t *node, int *arg_index, int positional_count, list_t **vargs_ptr, bool *hit_keyword)
{
    if (!node || *hit_keyword) {
        return;
    }

    if (node->nt_id == NT_assignment_expression) {
        // Check if this is a kw_func() call - treat like keyword argument
        if (contains_kw_func_call(input, node)) {
            *hit_keyword = true;
            return;
        }
        if (*arg_index >= positional_count) {
            *vargs_ptr = list_append(*vargs_ptr, node);
        }
        (*arg_index)++;
        return;
    }

    if (node->nt_id == NT_keyword_argument) {
        *hit_keyword = true;
        return;
    }

    if (node->nt_id == NT_argument_expression_list) {
        for (int i = 0; i < node->num_kids; i++) {
            collect_vargs(input, tnode_get_kid(node, i), arg_index, positional_count, vargs_ptr, hit_keyword);
            if (*hit_keyword) {
                return;
            }
        }
    }
}

/**
 * @brief Collect keyword arguments into a list.
 */
static void
collect_kw_args(tnode_t *node, list_t **kw_args_ptr)
{
    if (!node) {
        return;
    }

    if (node->nt_id == NT_keyword_argument) {
        *kw_args_ptr = list_append(*kw_args_ptr, node);
        return;
    }

    if (node->nt_id == NT_argument_expression_list) {
        for (int i = 0; i < node->num_kids; i++) {
            collect_kw_args(tnode_get_kid(node, i), kw_args_ptr);
        }
    }
}

/**
 * @brief Transform function call with variadic arguments.
 */
static tnode_t *
xform_vargs_call(xform_ctx_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(postfix_expression, CALL)) {
        return nullptr;
    }

    if (node->num_kids < 3) {
        return nullptr;
    }

    tnode_t *callee = tnode_get_kid(node, 0);
    tnode_t *func_id = find_func_identifier(callee);
    if (!func_id) {
        return nullptr;
    }

    const char *func_name = get_func_name(ctx->input, func_id);
    if (!func_name) {
        return nullptr;
    }

    vargs_info_t *vargs_info = nullptr;
    if (ctx->symtab) {
        vargs_info = st_get_vargs_info(ctx->symtab, (char *)func_name);
    }

    if (!vargs_info || vargs_info->style != VARGS_N00B) {
        return nullptr;
    }

    // Find argument list
    tnode_t *arg_list = nullptr;
    int arg_list_idx = -1;
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->nt_id == NT_argument_expression_list) {
            arg_list = kid;
            arg_list_idx = i;
            break;
        }
    }

    int total_args = count_args(ctx->input, arg_list);
    int positional_args = vargs_info->positional_before;
    int variadic_count = total_args - positional_args;
    int line = get_node_line(node);

    if (variadic_count <= 0) {
        // No variadic arguments - add nullptr for vargs
        tnode_t *nullptr_expr = build_nullptr_expr(line);
        tnode_t *nullptr_assign = wrap_in_expr_hierarchy(nullptr_expr, line);

        // Check if function also has opaque kargs
        bool has_opaque_kargs = false;
        if (ctx->symtab) {
            kw_info_t *kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);
            has_opaque_kargs = kw_info && kw_info->is_opaque;
        }

        if (arg_list) {
            // Check if node was flattened - emitter automatically adds commas for flattened nodes
            bool was_flattened = arg_list->origin != nullptr
                              && arg_list->origin->rewrite_name != nullptr
                              && strcmp(arg_list->origin->rewrite_name, "flatten") == 0;

            // Add comma before nullptr if there are existing args and node wasn't flattened
            // (flattened nodes get commas added by emitter between all children)
            if (!was_flattened && arg_list->num_kids > 0) {
                add_child(arg_list, synth_terminal(",", TT_PUNCT, line));
            }
            add_child(arg_list, nullptr_assign);

            // If function has opaque kargs, add nullptr for that too
            if (has_opaque_kargs) {
                // Only add comma if not flattened (emitter handles it otherwise)
                if (!was_flattened) {
                    add_child(arg_list, synth_terminal(",", TT_PUNCT, line));
                }
                tnode_t *kargs_nullptr = build_nullptr_expr(line);
                tnode_t *kargs_assign = wrap_in_expr_hierarchy(kargs_nullptr, line);
                add_child(arg_list, kargs_assign);
            }
        } else {
            // Create new argument list with nullptr(s)
            tnode_t *new_arg_list = synth_nonterminal("argument_expression_list_1");
            new_arg_list->nt_id   = NT_argument_expression_list;
            new_arg_list->branch  = 1;
            add_child(new_arg_list, nullptr_assign);

            // If function has opaque kargs, add nullptr for that too
            if (has_opaque_kargs) {
                add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
                tnode_t *kargs_nullptr = build_nullptr_expr(line);
                tnode_t *kargs_assign = wrap_in_expr_hierarchy(kargs_nullptr, line);
                add_child(new_arg_list, kargs_assign);
            }

            // Insert before the closing paren
            for (int i = node->num_kids - 1; i >= 0; i--) {
                tnode_t *kid = tnode_get_kid(node, i);
                if (kid && kid->tptr && kid->nt && strcmp(kid->nt, ")") == 0) {
                    // Found closing paren, insert before it
                    // Need to shift children
                    for (int j = node->num_kids; j > i; j--) {
                        node->kids->items[j] = node->kids->items[j - 1];
                    }
                    node->kids->items[i] = new_arg_list;
                    node->num_kids++;
                    new_arg_list->parent = node;
                    break;
                }
            }
        }
        return nullptr;
    }

    // Collect variadic arguments (stops at first keyword)
    list_t *vargs = nullptr;
    bool hit_keyword = false;
    int arg_idx = 0;
    collect_vargs(ctx->input, arg_list, &arg_idx, positional_args, &vargs, &hit_keyword);

    // Collect keyword arguments (preserved for keyword transform)
    list_t *kw_args = nullptr;
    collect_kw_args(arg_list, &kw_args);

    if (list_len(vargs) == 0) {
        base_dealloc(vargs);
        base_dealloc(kw_args);
        return nullptr;
    }

    // For typed varargs, copy each vararg expression before they get consumed
    // by the compound literal builder (which reparents them)
    list_t *varg_copies = nullptr;
    if (vargs_info->type_node) {
        int nvargs = list_len(vargs);
        for (int i = 0; i < nvargs; i++) {
            tnode_t *arg = list_get(vargs, i);
            varg_copies = list_append(varg_copies, copy_tree(arg));
        }
    }

    // Build the vargs compound literal with address-of
    tnode_t *literal = build_vargs_compound_literal(vargs, line);
    tnode_t *addr_of = build_address_of(literal, line);
    tnode_t *vargs_assign = wrap_in_expr_hierarchy(addr_of, line);

    // Rebuild the argument list:
    // 1. Keep positional args
    // 2. Replace variadic args with compound literal
    // 3. Preserve keyword args for keyword transform

    // Collect positional arguments
    list_t *pos_args = nullptr;
    arg_idx = 0;
    for (int i = 0; i < arg_list->num_kids && arg_idx < positional_args; i++) {
        tnode_t *kid = tnode_get_kid(arg_list, i);
        if (kid && kid->nt_id == NT_assignment_expression) {
            pos_args = list_append(pos_args, kid);
            arg_idx++;
        }
    }

    // Rebuild the argument list
    tnode_t *new_arg_list = synth_nonterminal("argument_expression_list");
    new_arg_list->nt_id   = NT_argument_expression_list;

    // Add positional args with commas
    int pos_count = list_len(pos_args);
    for (int i = 0; i < pos_count; i++) {
        if (i > 0) {
            add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
        }
        tnode_t *arg = list_get(pos_args, i);
        add_child(new_arg_list, arg);
    }

    // Add comma and vargs struct
    if (pos_count > 0) {
        add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
    }
    add_child(new_arg_list, vargs_assign);

    // Preserve keyword arguments (keyword transform will process them)
    int kw_count = list_len(kw_args);
    for (int i = 0; i < kw_count; i++) {
        add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
        tnode_t *kw_arg = list_get(kw_args, i);
        add_child(new_arg_list, kw_arg);
    }

    // If function has opaque kargs but no keyword arguments in call, add nullptr
    if (kw_count == 0 && ctx->symtab) {
        kw_info_t *kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);
        if (kw_info && kw_info->is_opaque) {
            add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
            tnode_t *nullptr_expr = build_nullptr_expr(line);
            tnode_t *nullptr_assign = wrap_in_expr_hierarchy(nullptr_expr, line);
            add_child(new_arg_list, nullptr_assign);
        }
    }

    // Replace old arg_list with new one
    if (arg_list_idx >= 0) {
        replace_child_at(node, arg_list_idx, new_arg_list);
    }

    base_dealloc(pos_args);
    base_dealloc(vargs);
    base_dealloc(kw_args);

    // For typed varargs, wrap the call in a statement expression with
    // _Static_assert checks for each variadic argument's type
    if (varg_copies) {
        tnode_t *wrapper = wrap_call_with_type_checks(
            node, varg_copies, vargs_info->type_node, line);
        base_dealloc(varg_copies);
        return wrapper;
    }

    return nullptr;
}

/**
 * @brief Register the variadic argument transformations.
 */
void
register_vargs_xform(xform_registry_t *reg)
{
    // Declaration transforms - rewrite + to n00b_vargs_t *vargs
    xform_register_post(reg, NT_function_definition, xform_vargs_func_def, "vargs_def");
    xform_register_post(reg, NT_declaration, xform_vargs_declaration, "vargs_decl");

    // Call site transform - packs varargs into compound literal
    xform_register_post(reg, NT_postfix_expression, xform_vargs_call, "vargs_call");
}
