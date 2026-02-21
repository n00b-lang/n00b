/**
 * @file xform_keyword.c
 * @brief Transforms keyword argument syntax into inline struct parameters.
 *
 * Transforms:
 *   void foo(int x) _kargs { bool opt = true; };
 * Into:
 *   struct _foo__kargs { unsigned _has_opt:1; bool opt; };
 *   void foo(int x, struct _foo__kargs *kargs);
 *
 * Uses proper tree node operations - NO token replacement.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

#include "branch_symbols.h"
#include "ncc_limits.h"
#include "transform.h"
#include "xform_helpers.h"
#include "types.h"
#include "nt_types.h"
#include "st.h"

// ---------------------------------------------------------------------------
// Helper: Node Navigation
// ---------------------------------------------------------------------------

/**
 * @brief Collect keyword_param nodes from keyword_param_list.
 * After flattening, keyword_param nodes are direct children.
 */
static ncc_list_t *
collect_params(tnode_t *node, ncc_list_t *params)
{
    if (!node) {
        return params;
    }
    if (node->nt_id == NT_keyword_param) {
        return ncc_list_append(params, node);
    }
    if (node->nt_id == NT_keyword_param_list) {
        for (int i = 0; i < node->num_kids; i++) {
            params = collect_params(tnode_get_kid(node, i), params);
        }
    }
    return params;
}

/**
 * @brief Find function_declarator in a declarator tree.
 */
static tnode_t *
find_function_declarator(tnode_t *declarator)
{
    if (!declarator) {
        return nullptr;
    }
    if (declarator->nt_id == NT_function_declarator) {
        return declarator;
    }
    for (int i = 0; i < declarator->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(declarator, i);
        tnode_t *found = find_function_declarator(kid);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

/**
 * @brief Find parameter_type_list or parameter_list in function_declarator.
 */
static tnode_t *
find_param_list(tnode_t *func_decl)
{
    if (!func_decl) {
        return nullptr;
    }
    for (int i = 0; i < func_decl->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(func_decl, i);
        if (kid && (kid->nt_id == NT_parameter_type_list || kid->nt_id == NT_parameter_list)) {
            return kid;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: Tree Building (keyword-specific)
// ---------------------------------------------------------------------------

/**
 * @brief Build member access: base->member
 */
static tnode_t *
build_arrow_access(const char *base_name, const char *member, int line)
{
    tnode_t *base = build_primary_id(base_name, line);

    tnode_t *postfix = synth_nonterminal("postfix_expression_4");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = 4;

    add_child(postfix, base);
    add_child(postfix, synth_terminal("->", TT_PUNCT, line));
    add_child(postfix, build_identifier(member, line));

    return postfix;
}

/**
 * @brief Build a member_declaration for a bitfield: unsigned _has_name : 1;
 */
static tnode_t *
build_bitfield_member(const char *field_name, int line)
{
    char has_name[NCC_IDENT_BUF];
    int  ret = snprintf(has_name, sizeof(has_name), "_has_%s", field_name);
    NCC_CHECK_SNPRINTF(ret, has_name);

    tnode_t *member = synth_nonterminal("member_declaration_0");
    member->nt_id   = NT_member_declaration;
    member->branch  = 0;

    // specifier_qualifier_list: unsigned
    tnode_t *spec_qual_list = synth_nonterminal("specifier_qualifier_list_1");
    spec_qual_list->nt_id   = NT_specifier_qualifier_list;
    spec_qual_list->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_0");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 0;
    add_child(type_spec, synth_terminal("unsigned", TT_KEYWORD, line));

    add_child(tsq, type_spec);
    add_child(spec_qual_list, tsq);
    add_child(member, spec_qual_list);

    // member_declarator_list -> member_declarator_0 (bitfield)
    tnode_t *member_decl_list = synth_nonterminal("member_declarator_list_0");
    member_decl_list->nt_id   = NT_member_declarator_list;
    member_decl_list->branch  = 0;

    tnode_t *member_decl = synth_nonterminal("member_declarator_0");
    member_decl->nt_id   = NT_member_declarator;
    member_decl->branch  = 0;

    // declarator
    tnode_t *declarator = synth_nonterminal("declarator_0");
    declarator->nt_id   = NT_declarator;
    declarator->branch  = 0;

    tnode_t *direct_decl = synth_nonterminal("direct_declarator_3");
    direct_decl->nt_id   = NT_direct_declarator;
    direct_decl->branch  = 3;
    add_child(direct_decl, build_identifier(has_name, line));

    add_child(declarator, direct_decl);
    add_child(member_decl, declarator);

    // : 1
    add_child(member_decl, synth_terminal(":", TT_PUNCT, line));

    tnode_t *const_expr = synth_nonterminal("constant_expression_0");
    const_expr->nt_id   = NT_constant_expression;
    const_expr->branch  = 0;

    // Build the constant 1
    tnode_t *const_node = synth_nonterminal("constant");
    const_node->nt_id   = NT_constant;
    add_child(const_node, synth_terminal("1", TT_NUM, line));

    tnode_t *prim = synth_nonterminal("primary_expression_1");
    prim->nt_id   = NT_primary_expression;
    prim->branch  = 1;
    add_child(prim, const_node);

    add_child(const_expr, wrap_in_expr_hierarchy(prim, line));

    add_child(member_decl, const_expr);
    add_child(member_decl_list, member_decl);
    add_child(member, member_decl_list);

    add_child(member, synth_terminal(";", TT_PUNCT, line));

    return member;
}

/**
 * @brief Build a member_declaration from a keyword_param node.
 * Clones the type and name from the keyword_param.
 */
static tnode_t *
build_field_member(tnode_t *kw_param, int line)
{
    tnode_t *decl_specs = find_child(kw_param, NT_declaration_specifiers);
    tnode_t *declarator = find_child(kw_param, NT_declarator);

    if (!decl_specs || !declarator) {
        return nullptr;
    }

    tnode_t *member = synth_nonterminal("member_declaration_0");
    member->nt_id   = NT_member_declaration;
    member->branch  = 0;

    // Clone declaration_specifiers as specifier_qualifier_list
    // This is a simplification - we copy the subtree
    tnode_t *spec_qual_list = synth_nonterminal("specifier_qualifier_list");
    spec_qual_list->nt_id   = NT_specifier_qualifier_list;

    // Copy children from decl_specs
    for (int i = 0; i < decl_specs->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(decl_specs, i);
        if (kid && !IS_ELIDED(kid)) {
            tnode_t *copy = copy_tree(kid);
            // Convert declaration_specifier to type_specifier_qualifier if needed
            if (copy->nt_id == NT_declaration_specifier) {
                copy->nt_id = NT_type_specifier_qualifier;
            }
            add_child(spec_qual_list, copy);
        }
    }
    add_child(member, spec_qual_list);

    // member_declarator_list -> member_declarator_1 (simple declarator)
    tnode_t *member_decl_list = synth_nonterminal("member_declarator_list_0");
    member_decl_list->nt_id   = NT_member_declarator_list;
    member_decl_list->branch  = 0;

    tnode_t *member_decl = synth_nonterminal("member_declarator_1");
    member_decl->nt_id   = NT_member_declarator;
    member_decl->branch  = 1;

    // Clone the declarator
    add_child(member_decl, copy_tree(declarator));

    add_child(member_decl_list, member_decl);
    add_child(member, member_decl_list);

    add_child(member, synth_terminal(";", TT_PUNCT, line));

    return member;
}

/**
 * @brief Build a struct definition for keyword arguments.
 * struct _funcname__kargs { unsigned _has_x:1; type x; ... };
 */
static tnode_t *
build_kargs_struct(ncc_buf_t *input, const char *func_name, ncc_list_t *params, int line)
{
    char struct_name[NCC_IDENT_BUF];
    int  ret = snprintf(struct_name, sizeof(struct_name), "_%s__kargs", func_name);
    NCC_CHECK_SNPRINTF(ret, struct_name);

    // external_declaration_1 -> declaration_1
    tnode_t *ext_decl = synth_nonterminal("external_declaration_1");
    ext_decl->nt_id   = NT_external_declaration;
    ext_decl->branch  = 1;

    tnode_t *decl = synth_nonterminal("declaration_1");
    decl->nt_id   = NT_declaration;
    decl->branch  = 1;

    // declaration_specifiers with struct_or_union_specifier
    tnode_t *decl_specs = synth_nonterminal("declaration_specifiers_1");
    decl_specs->nt_id   = NT_declaration_specifiers;
    decl_specs->branch  = 1;

    tnode_t *decl_spec = synth_nonterminal("declaration_specifier_1");
    decl_spec->nt_id   = NT_declaration_specifier;
    decl_spec->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_3");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 3;

    // struct_or_union_specifier with body
    tnode_t *struct_spec = synth_nonterminal("struct_or_union_specifier_0");
    struct_spec->nt_id   = NT_struct_or_union_specifier;
    struct_spec->branch  = 0;

    add_child(struct_spec, synth_terminal("struct", TT_KEYWORD, line));
    add_child(struct_spec, build_identifier(struct_name, line));
    add_child(struct_spec, synth_terminal("{", TT_PUNCT, line));

    // member_declaration_list
    tnode_t *member_list = synth_nonterminal("member_declaration_list");
    member_list->nt_id   = NT_member_declaration_list;

    int count = ncc_list_len(params);

    // First: bitfields for _has_X
    for (int i = 0; i < count; i++) {
        tnode_t *param = ncc_list_get(params, i);
        tnode_t *param_declarator = find_child(param, NT_declarator);
        const char *name = get_identifier_text(input, param_declarator);
        if (name) {
            add_child(member_list, build_bitfield_member(name, line));
        }
    }

    // Second: actual fields
    for (int i = 0; i < count; i++) {
        tnode_t *param = ncc_list_get(params, i);
        tnode_t *field = build_field_member(param, line);
        if (field) {
            add_child(member_list, field);
        }
    }

    add_child(struct_spec, member_list);
    add_child(struct_spec, synth_terminal("}", TT_PUNCT, line));

    add_child(type_spec, struct_spec);
    add_child(tsq, type_spec);
    add_child(decl_spec, tsq);
    add_child(decl_specs, decl_spec);
    add_child(decl, decl_specs);

    add_child(decl, synth_terminal(";", TT_PUNCT, line));

    add_child(ext_decl, decl);

    return ext_decl;
}

/**
 * @brief Build parameter_declaration for kargs: struct _funcname__kargs *kargs
 */
static tnode_t *
build_kargs_param(const char *func_name, int line)
{
    char struct_name[NCC_IDENT_BUF];
    int  ret = snprintf(struct_name, sizeof(struct_name), "_%s__kargs", func_name);
    NCC_CHECK_SNPRINTF(ret, struct_name);

    tnode_t *param_decl = synth_nonterminal("parameter_declaration_0");
    param_decl->nt_id   = NT_parameter_declaration;
    param_decl->branch  = 0;

    // declaration_specifiers: struct _foo__kargs
    tnode_t *decl_specs = synth_nonterminal("declaration_specifiers_1");
    decl_specs->nt_id   = NT_declaration_specifiers;
    decl_specs->branch  = 1;

    tnode_t *decl_spec = synth_nonterminal("declaration_specifier_1");
    decl_spec->nt_id   = NT_declaration_specifier;
    decl_spec->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_3");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 3;

    // struct_or_union_specifier (reference only, no body)
    tnode_t *struct_spec = synth_nonterminal("struct_or_union_specifier_1");
    struct_spec->nt_id   = NT_struct_or_union_specifier;
    struct_spec->branch  = 1;

    add_child(struct_spec, synth_terminal("struct", TT_KEYWORD, line));
    add_child(struct_spec, build_identifier(struct_name, line));

    add_child(type_spec, struct_spec);
    add_child(tsq, type_spec);
    add_child(decl_spec, tsq);
    add_child(decl_specs, decl_spec);
    add_child(param_decl, decl_specs);

    // declarator: *kargs
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
    add_child(direct_decl, build_identifier("kargs", line));

    add_child(declarator, pointer);
    add_child(declarator, direct_decl);
    add_child(param_decl, declarator);

    return param_decl;
}

/**
 * @brief Build parameter_declaration for opaque kargs: void *kargs
 * Used for keyword passthrough functions that don't define their own keywords.
 */
static tnode_t *
build_opaque_kargs_param(int line)
{
    tnode_t *param_decl = synth_nonterminal("parameter_declaration_0");
    param_decl->nt_id   = NT_parameter_declaration;
    param_decl->branch  = 0;

    // declaration_specifiers: void
    tnode_t *decl_specs = synth_nonterminal("declaration_specifiers_1");
    decl_specs->nt_id   = NT_declaration_specifiers;
    decl_specs->branch  = 1;

    tnode_t *decl_spec = synth_nonterminal("declaration_specifier_1");
    decl_spec->nt_id   = NT_declaration_specifier;
    decl_spec->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_0");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 0;
    add_child(type_spec, synth_terminal("void", TT_KEYWORD, line));

    add_child(tsq, type_spec);
    add_child(decl_spec, tsq);
    add_child(decl_specs, decl_spec);
    add_child(param_decl, decl_specs);

    // declarator: *kargs
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
    add_child(direct_decl, build_identifier("kargs", line));

    add_child(declarator, pointer);
    add_child(declarator, direct_decl);
    add_child(param_decl, declarator);

    return param_decl;
}

/**
 * @brief Wrap a postfix_expression up through logical_OR_expression.
 * Used to build the condition part of a ternary expression.
 */
static tnode_t *
wrap_to_lor(tnode_t *inner, int line)
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

    return lor;
}

/**
 * @brief Build extraction declaration: type name = kargs->name;
 * When default_init is non-NULL, generates:
 *   type name = kargs->_has_name ? kargs->name : (default_value);
 */
static tnode_t *
build_extraction_decl(ncc_buf_t *input, tnode_t *kw_param,
                      tnode_t *default_init, int line)
{
    tnode_t *decl_specs = find_child(kw_param, NT_declaration_specifiers);
    tnode_t *declarator = find_child(kw_param, NT_declarator);

    if (!decl_specs || !declarator) {
        return nullptr;
    }

    const char *name = get_identifier_text(input, declarator);
    if (!name) {
        return nullptr;
    }

    // block_item_0 -> declaration_1
    tnode_t *block_item = synth_nonterminal("block_item_0");
    block_item->nt_id   = NT_block_item;
    block_item->branch  = 0;

    tnode_t *decl = synth_nonterminal("declaration_1");
    decl->nt_id   = NT_declaration;
    decl->branch  = 1;

    // Clone declaration_specifiers
    add_child(decl, copy_tree(decl_specs));

    // init_declarator_list -> init_declarator_0
    tnode_t *init_decl_list = synth_nonterminal("init_declarator_list_0");
    init_decl_list->nt_id   = NT_init_declarator_list;
    init_decl_list->branch  = 0;

    tnode_t *init_decl = synth_nonterminal("init_declarator_0");
    init_decl->nt_id   = NT_init_declarator;
    init_decl->branch  = 0;

    // Clone declarator
    add_child(init_decl, copy_tree(declarator));

    add_child(init_decl, synth_terminal("=", TT_PUNCT, line));

    // initializer
    tnode_t *init = synth_nonterminal("initializer_1");
    init->nt_id   = NT_initializer;
    init->branch  = 1;

    if (default_init) {
        // Build: kargs->_has_name ? kargs->name : (default_value)
        //
        // conditional_expression_0:
        //   logical_OR_expression   (condition: kargs->_has_name)
        //   "?"
        //   expression              (true: kargs->name)
        //   ":"
        //   conditional_expression  (false: default_value)

        // Condition: kargs->_has_name
        char has_name[NCC_IDENT_BUF];
        int  hret = snprintf(has_name, sizeof(has_name), "_has_%s", name);
        NCC_CHECK_SNPRINTF(hret, has_name);
        tnode_t *cond_access = build_arrow_access("kargs", has_name, line);
        tnode_t *cond_lor = wrap_to_lor(cond_access, line);

        // True branch: kargs->name wrapped in expression
        tnode_t *true_access = build_arrow_access("kargs", name, line);
        tnode_t *true_assign = wrap_in_expr_hierarchy(true_access, line);

        tnode_t *true_expr = synth_nonterminal("expression_0");
        true_expr->nt_id   = NT_expression;
        true_expr->branch  = 0;
        add_child(true_expr, true_assign);

        // False branch: default value wrapped in conditional_expression
        // The default_init is an NT_initializer node (branch 1) containing
        // an assignment_expression. We need the assignment_expression's
        // inner conditional_expression for the false branch.
        // Simplest: copy the initializer's child (assignment_expression),
        // then extract the conditional from it, or just re-wrap the
        // default expression.
        //
        // Since the default_init (branch 1) contains an assignment_expression,
        // and assignment_expression (branch 1) contains a conditional_expression,
        // we can copy the whole initializer's child and extract appropriately.
        // But it's safest to just copy the entire default expression tree and
        // wrap it in a fresh conditional_expression_1 (pass-through).
        tnode_t *default_copy = copy_tree(default_init);
        // default_copy is an NT_initializer; its first child is assignment_expression
        tnode_t *default_assign = tnode_get_kid(default_copy, 0);

        tnode_t *false_cond;
        if (default_assign && default_assign->nt_id == NT_assignment_expression) {
            // assignment_expression (branch 1) -> conditional_expression
            tnode_t *inner_cond = tnode_get_kid(default_assign, 0);
            if (inner_cond && inner_cond->nt_id == NT_conditional_expression) {
                false_cond = inner_cond;
            }
            else {
                // Wrap the whole default in a fresh conditional
                false_cond = synth_nonterminal("conditional_expression_1");
                false_cond->nt_id = NT_conditional_expression;
                false_cond->branch = 1;
                tnode_t *def_lor = wrap_to_lor(default_assign, line);
                add_child(false_cond, def_lor);
            }
        }
        else {
            // Fallback: wrap default_copy's first child
            false_cond = synth_nonterminal("conditional_expression_1");
            false_cond->nt_id = NT_conditional_expression;
            false_cond->branch = 1;
            tnode_t *fallback = default_copy->num_kids > 0
                              ? tnode_get_kid(default_copy, 0) : default_copy;
            tnode_t *def_lor = wrap_to_lor(fallback, line);
            add_child(false_cond, def_lor);
        }

        // Build the ternary: conditional_expression_0
        tnode_t *ternary = synth_nonterminal("conditional_expression_0");
        ternary->nt_id   = NT_conditional_expression;
        ternary->branch  = 0;
        add_child(ternary, cond_lor);
        add_child(ternary, synth_terminal("?", TT_PUNCT, line));
        add_child(ternary, true_expr);
        add_child(ternary, synth_terminal(":", TT_PUNCT, line));
        add_child(ternary, false_cond);

        // Wrap ternary in assignment_expression
        tnode_t *assign = synth_nonterminal("assignment_expression_1");
        assign->nt_id   = NT_assignment_expression;
        assign->branch  = 1;
        add_child(assign, ternary);

        add_child(init, assign);
    }
    else {
        // No default: unconditional load from kargs->name
        tnode_t *access = build_arrow_access("kargs", name, line);
        add_child(init, wrap_in_expr_hierarchy(access, line));
    }

    add_child(init_decl, init);
    add_child(init_decl_list, init_decl);
    add_child(decl, init_decl_list);

    add_child(decl, synth_terminal(";", TT_PUNCT, line));

    add_child(block_item, decl);

    return block_item;
}

// ---------------------------------------------------------------------------
// Main Transform Functions
// ---------------------------------------------------------------------------

/**
 * @brief Helper to add kargs parameter to a function declarator.
 * @param func_decl The function_declarator node.
 * @param kargs_param The parameter_declaration node for kargs.
 * @param line Source line for synthetic tokens.
 */
static void
add_kargs_to_func(tnode_t *func_decl, tnode_t *kargs_param, int line)
{
    tnode_t *func_param_list = find_param_list(func_decl);

    if (func_param_list) {
        // Always append kargs to the parameter_type_list (or the
        // outermost param node) so it comes AFTER any vargs param
        // that the vargs transform may have placed as a direct child
        // of parameter_type_list.
        tnode_t *append_target = func_param_list;

        // Check if node was flattened - emitter only adds commas for flattened nodes
        bool was_flattened = append_target->origin != nullptr
                          && append_target->origin->rewrite_name != nullptr
                          && strcmp(append_target->origin->rewrite_name, "flatten") == 0;

        // Add comma before kargs if there are existing params and node wasn't flattened
        if (!was_flattened && append_target->num_kids > 0) {
            add_child(append_target, synth_terminal(",", TT_PUNCT, line));
        }

        add_child(append_target, kargs_param);
    }
    else if (func_decl) {
        // No parameter list exists (e.g., func() with no params)
        // Create: parameter_type_list -> parameter_list -> kargs_param
        tnode_t *param_list = synth_nonterminal("parameter_list_0");
        param_list->nt_id   = NT_parameter_list;
        param_list->branch  = 0;

        add_child(param_list, kargs_param);

        tnode_t *param_type_list = synth_nonterminal("parameter_type_list_4");
        param_type_list->nt_id   = NT_parameter_type_list;
        param_type_list->branch  = 4;
        add_child(param_type_list, param_list);

        // Insert after "(" which is at index 1 (index 0 is direct_declarator)
        insert_child_at(func_decl, 2, param_type_list);
    }
}

/**
 * @brief Transform function_definition with keyword_clause.
 */
static tnode_t *
xform_kw_definition(tree_xform_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(function_definition, WITH_KEYWORDS)) {
        return nullptr;
    }

    tnode_t *kw_clause = find_child(node, NT_keyword_clause);
    if (!kw_clause) {
        return nullptr;
    }

    tnode_t *declarator = find_child(node, NT_declarator);
    if (!declarator) {
        return nullptr;
    }

    const char *func_name = get_identifier_text(ctx->input, declarator);
    if (!func_name) {
        return nullptr;
    }

    int line = get_node_line(node);
    tnode_t *func_decl = find_function_declarator(declarator);

    // Check if this is opaque keyword passthrough vs regular keywords
    bool is_opaque = (kw_clause->branch == BRANCH(keyword_clause, OPAQUE));

    if (is_opaque) {
        // Opaque passthrough: just add void *kargs, no struct, no extraction
        tnode_t *kargs_param = build_opaque_kargs_param(line);
        add_kargs_to_func(func_decl, kargs_param, line);

        // Record in symbol table that this function has opaque kargs
        if (ctx->symtab) {
            kw_info_t *kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);
            if (kw_info) {
                kw_info->is_opaque = true;
            }
        }
    }
    else {
        // Regular keywords: generate struct, add typed kargs param, extract in body

        // Collect keyword params
        tnode_t *param_list = find_child(kw_clause, NT_keyword_param_list);
        if (!param_list) {
            param_list = kw_clause;
        }

        ncc_list_t *params = nullptr;
        params = collect_params(param_list, params);

        int param_count = ncc_list_len(params);
        if (param_count == 0) {
            base_dealloc(params);
            // Remove keyword_clause and change branch
            int kw_idx = find_child_index(node, kw_clause);
            if (kw_idx >= 0) {
                remove_child_at(node, kw_idx);
            }
            node->branch = 1;
            return nullptr;
        }

        // Check if we've already emitted the struct
        bool need_struct = true;
        kw_info_t *kw_info = nullptr;
        if (ctx->symtab) {
            kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);

            // If kw_info wasn't registered during parsing (e.g. because
            // the function name used typeid() which hadn't been resolved
            // yet), build and register it now.
            if (!kw_info) {
                kw_info = base_calloc(1, sizeof(kw_info_t));
                kw_info->params = nullptr;
                kw_info->defaults_set = false;

                for (int i = 0; i < param_count; i++) {
                    tnode_t *p = ncc_list_get(params, i);
                    kw_param_info_t *kpi = base_calloc(1, sizeof(kw_param_info_t));

                    tnode_t *p_decl = find_child(p, NT_declarator);
                    if (p_decl) {
                        kpi->name = (char *)get_identifier_text(ctx->input, p_decl);
                    }
                    kpi->decl_specs = find_child(p, NT_declaration_specifiers);
                    kpi->declarator = p_decl;
                    kpi->default_val = find_child(p, NT_initializer);
                    if (kpi->default_val) {
                        kw_info->defaults_set = true;
                    }
                    kw_info->params = ncc_list_append(kw_info->params, kpi);
                }

                // Count positional params from the function declarator
                tnode_t *ptl = find_param_list(func_decl);
                if (ptl) {
                    int pos_count = 0;
                    for (int i = 0; i < ptl->num_kids; i++) {
                        tnode_t *kid = tnode_get_kid(ptl, i);
                        if (kid && kid->nt_id == NT_parameter_declaration) {
                            pos_count++;
                        }
                        else if (kid && kid->nt_id == NT_parameter_list) {
                            for (int j = 0; j < kid->num_kids; j++) {
                                tnode_t *gkid = tnode_get_kid(kid, j);
                                if (gkid && gkid->nt_id == NT_parameter_declaration) {
                                    pos_count++;
                                }
                            }
                        }
                    }
                    kw_info->num_positional_params = pos_count;
                }

                // Ensure symbol entry exists (may be missing if
                // name wasn't resolved during parsing, e.g. typeid())
                if (!st_get_entry(ctx->symtab, (char *)func_name)) {
                    st_add_variable(ctx->symtab, (char *)func_name,
                                    SYM_DEFINITION, node, nullptr);
                }
                st_set_kw_info(ctx->symtab, (char *)func_name, kw_info);
            }

            if (kw_info->struct_emitted) {
                need_struct = false;
            }
        }

        // 1. Build and insert struct definition before this node
        if (need_struct) {
            tnode_t *struct_decl = build_kargs_struct(ctx->input, func_name, params, line);
            if (struct_decl) {
                // Find parent (should be external_declaration) and grandparent (translation_unit)
                tnode_t *ext_decl = node->parent;
                if (ext_decl && ext_decl->parent) {
                    tnode_t *trans_unit = ext_decl->parent;
                    int idx = find_child_index(trans_unit, ext_decl);
                    if (idx >= 0) {
                        insert_child_at(trans_unit, idx, struct_decl);
                    }
                }
            }
            if (kw_info) {
                kw_info->struct_emitted = true;
            }
        }

        // 2. Add kargs parameter to function
        tnode_t *kargs_param = build_kargs_param(func_name, line);
        add_kargs_to_func(func_decl, kargs_param, line);

        // 3. Insert extraction code at function body start
        tnode_t *func_body = find_child(node, NT_function_body);
        if (func_body) {
            tnode_t *compound = find_child(func_body, NT_compound_statement);
            if (compound) {
                tnode_t *block_list = find_child(compound, NT_block_item_list);
                if (block_list) {
                    // Insert extraction declarations at the beginning
                    int insert_idx = 0;
                    for (int i = 0; i < param_count; i++) {
                        tnode_t *param = ncc_list_get(params, i);
                        tnode_t *default_init = find_child(param, NT_initializer);
                        tnode_t *extract_item = build_extraction_decl(ctx->input, param, default_init, line);
                        if (extract_item) {
                            insert_child_at(block_list, insert_idx++, extract_item);
                        }
                    }
                }
            }
        }

        base_dealloc(params);
    }

    // Remove keyword_clause from the tree (UNPARENT)
    int kw_idx = find_child_index(node, kw_clause);
    if (kw_idx >= 0) {
        remove_child_at(node, kw_idx);
    }

    // Change branch to 1 (standard function_definition without keywords)
    node->branch = 1;

    return nullptr;
}

/**
 * @brief Transform declaration with keyword_clause.
 */
static tnode_t *
xform_kw_declaration(tree_xform_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(declaration, WITH_KEYWORDS)) {
        return nullptr;
    }

    tnode_t *kw_clause = find_child(node, NT_keyword_clause);
    if (!kw_clause) {
        return nullptr;
    }

    tnode_t *declarator = find_child(node, NT_declarator);
    if (!declarator) {
        return nullptr;
    }

    const char *func_name = get_identifier_text(ctx->input, declarator);
    if (!func_name) {
        return nullptr;
    }

    int line = get_node_line(node);
    tnode_t *func_decl = find_function_declarator(declarator);

    // Check if this is opaque keyword passthrough vs regular keywords
    bool is_opaque = (kw_clause->branch == BRANCH(keyword_clause, OPAQUE));

    if (is_opaque) {
        // Opaque passthrough: just add void *kargs, no struct
        tnode_t *kargs_param = build_opaque_kargs_param(line);
        add_kargs_to_func(func_decl, kargs_param, line);

        // Record in symbol table that this function has opaque kargs
        if (ctx->symtab) {
            kw_info_t *kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);
            if (kw_info) {
                kw_info->is_opaque = true;
            }
        }
    }
    else {
        // Regular keywords: generate struct, add typed kargs param

        // Collect keyword params
        tnode_t *param_list = find_child(kw_clause, NT_keyword_param_list);
        if (!param_list) {
            param_list = kw_clause;
        }

        ncc_list_t *params = nullptr;
        params = collect_params(param_list, params);

        int param_count = ncc_list_len(params);
        if (param_count == 0) {
            base_dealloc(params);
            // Remove keyword_clause and change branch
            int kw_idx = find_child_index(node, kw_clause);
            if (kw_idx >= 0) {
                remove_child_at(node, kw_idx);
            }
            node->branch = 1;
            return nullptr;
        }

        // Check if we've already emitted the struct
        bool need_struct = true;
        kw_info_t *kw_info = nullptr;
        if (ctx->symtab) {
            kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);

            // If kw_info wasn't registered during parsing (e.g. because
            // the function name used typeid()), build and register it now.
            if (!kw_info) {
                kw_info = base_calloc(1, sizeof(kw_info_t));
                kw_info->params = nullptr;
                kw_info->defaults_set = false;

                for (int i = 0; i < param_count; i++) {
                    tnode_t *p = ncc_list_get(params, i);
                    kw_param_info_t *kpi = base_calloc(1, sizeof(kw_param_info_t));

                    tnode_t *p_decl = find_child(p, NT_declarator);
                    if (p_decl) {
                        kpi->name = (char *)get_identifier_text(ctx->input, p_decl);
                    }
                    kpi->decl_specs = find_child(p, NT_declaration_specifiers);
                    kpi->declarator = p_decl;
                    kpi->default_val = find_child(p, NT_initializer);
                    if (kpi->default_val) {
                        kw_info->defaults_set = true;
                    }
                    kw_info->params = ncc_list_append(kw_info->params, kpi);
                }

                // Count positional params from the function declarator
                tnode_t *ptl = find_param_list(func_decl);
                if (ptl) {
                    int pos_count = 0;
                    for (int i = 0; i < ptl->num_kids; i++) {
                        tnode_t *kid = tnode_get_kid(ptl, i);
                        if (kid && kid->nt_id == NT_parameter_declaration) {
                            pos_count++;
                        }
                        else if (kid && kid->nt_id == NT_parameter_list) {
                            for (int j = 0; j < kid->num_kids; j++) {
                                tnode_t *gkid = tnode_get_kid(kid, j);
                                if (gkid && gkid->nt_id == NT_parameter_declaration) {
                                    pos_count++;
                                }
                            }
                        }
                    }
                    kw_info->num_positional_params = pos_count;
                }

                // Ensure symbol entry exists (may be missing if
                // name wasn't resolved during parsing, e.g. typeid())
                if (!st_get_entry(ctx->symtab, (char *)func_name)) {
                    st_add_variable(ctx->symtab, (char *)func_name,
                                    SYM_DEFINITION, node, nullptr);
                }
                st_set_kw_info(ctx->symtab, (char *)func_name, kw_info);
            }

            if (kw_info->struct_emitted) {
                need_struct = false;
            }
        }

        // 1. Build and insert struct definition before this node
        if (need_struct) {
            tnode_t *struct_decl = build_kargs_struct(ctx->input, func_name, params, line);
            if (struct_decl) {
                // Find parent (should be external_declaration) and grandparent (translation_unit)
                tnode_t *ext_decl = node->parent;
                if (ext_decl && ext_decl->parent) {
                    tnode_t *trans_unit = ext_decl->parent;
                    int idx = find_child_index(trans_unit, ext_decl);
                    if (idx >= 0) {
                        insert_child_at(trans_unit, idx, struct_decl);
                    }
                }
            }
            if (kw_info) {
                kw_info->struct_emitted = true;
            }
        }

        // 2. Add kargs parameter to function
        tnode_t *kargs_param = build_kargs_param(func_name, line);
        add_kargs_to_func(func_decl, kargs_param, line);

        base_dealloc(params);
    }

    // Remove keyword_clause from the tree (UNPARENT)
    int kw_idx = find_child_index(node, kw_clause);
    if (kw_idx >= 0) {
        remove_child_at(node, kw_idx);
    }

    // Change branch to 1 (standard declaration without keywords)
    node->branch = 1;

    return nullptr;
}

/**
 * @brief Register the keyword argument transformations.
 */
void
register_keyword_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_function_definition, xform_kw_definition, "kw_def");
    xform_register_post(reg, NT_declaration, xform_kw_declaration, "kw_decl");
}
