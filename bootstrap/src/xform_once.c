/**
 * @file xform_once.c
 * @brief Tree-based transformation for the `once` function specifier.
 *
 * Transforms `once` qualified functions:
 * - Prototypes: remove the `once` specifier from the tree
 * - Definitions: generate wrapper with futex-based synchronization
 *
 * Uses proper tree node operations - NO token replacement.
 */

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include "ncc_limits.h"

#include "branch_symbols.h"
#include "transform.h"
#include "xform_helpers.h"
#include "nt_types.h"

// ---------------------------------------------------------------------------
// Helper: Node Validation
// ---------------------------------------------------------------------------

static inline bool
is_valid_node(tnode_t *node)
{
    extern const tnode_t elided_node;
    return node && node != (tnode_t *)&elided_node;
}

#define for_each_valid_kid(node, kid, i)       \
    for (int i = 0; i < (node)->num_kids; i++) \
        if ((kid = tnode_get_kid((node), i)), is_valid_node(kid))

// ---------------------------------------------------------------------------
// Helper: Node Navigation
// ---------------------------------------------------------------------------

/**
 * @brief Find 'once' specifier in declaration_specifiers.
 */
static tnode_t *
find_once_specifier(ncc_buf_t *input, tnode_t *decl_specs)
{
    if (!is_valid_node(decl_specs)) {
        return nullptr;
    }

    if (decl_specs->nt_id == NT_struct_or_union_specifier ||
        decl_specs->nt_id == NT_enum_specifier) {
        return nullptr;
    }

    if (decl_specs->tptr) {
        char *text = extract(input, decl_specs->tptr);
        if (text && strcmp(text, "once") == 0) {
            base_dealloc(text);
            return decl_specs;
        }
        base_dealloc(text);
        return nullptr;
    }

    tnode_t *kid;
    for_each_valid_kid(decl_specs, kid, i)
    {
        tnode_t *result = find_once_specifier(input, kid);
        if (result) {
            return result;
        }
    }

    return nullptr;
}

/**
 * @brief Extract identifier name from a declarator subtree.
 */
static char *
extract_declarator_name(ncc_buf_t *input, tnode_t *node)
{
    if (!is_valid_node(node)) {
        return nullptr;
    }

    if (node->nt_id == NT_identifier) {
        tok_t *tok = identifier_tok(node);
        if (tok) {
            return extract(input, tok);
        }
    }

    if (NT_IN_SET(node->nt_id, NT_SET_DECLARATORS) || node->nt_id == NT_direct_declarator) {
        tnode_t *kid;
        for_each_valid_kid(node, kid, i)
        {
            char *name = extract_declarator_name(input, kid);
            if (name) {
                return name;
            }
        }
    }

    return nullptr;
}

/**
 * @brief Check if declaration_specifiers contains "void" as return type.
 */
static bool
is_void_return(ncc_buf_t *input, tnode_t *decl_specs)
{
    if (!is_valid_node(decl_specs)) {
        return false;
    }

    if (decl_specs->tptr) {
        char *text = extract(input, decl_specs->tptr);
        if (text && strcmp(text, "void") == 0) {
            base_dealloc(text);
            return true;
        }
        base_dealloc(text);
        return false;
    }

    tnode_t *kid;
    for_each_valid_kid(decl_specs, kid, i)
    {
        if (is_void_return(input, kid)) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Find parameter_type_list in a declarator.
 */
static tnode_t *
find_param_list(tnode_t *node)
{
    if (!is_valid_node(node)) {
        return nullptr;
    }

    if (node->nt_id == NT_parameter_type_list) {
        return node;
    }

    tnode_t *kid;
    for_each_valid_kid(node, kid, i)
    {
        tnode_t *result = find_param_list(kid);
        if (result) {
            return result;
        }
    }

    return nullptr;
}

/**
 * @brief Find direct_declarator containing identifier in a declarator.
 */
static tnode_t *
find_name_direct_declarator(tnode_t *node)
{
    if (!is_valid_node(node)) {
        return nullptr;
    }

    if (node->nt_id == NT_direct_declarator) {
        for (int i = 0; i < node->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(node, i);
            if (kid && kid->nt_id == NT_identifier) {
                return node;
            }
        }
    }

    tnode_t *kid;
    for_each_valid_kid(node, kid, i)
    {
        tnode_t *result = find_name_direct_declarator(kid);
        if (result) {
            return result;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: Tree Building
// ---------------------------------------------------------------------------

/**
 * @brief Build a constant primary expression.
 */
static tnode_t *
build_constant(const char *value, int line)
{
    tnode_t *constant = synth_nonterminal("constant");
    constant->nt_id   = NT_constant;
    add_child(constant, synth_terminal(value, TT_NUM, line));

    tnode_t *primary = synth_nonterminal("primary_expression_1");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 1;
    add_child(primary, constant);

    return primary;
}

/**
 * @brief Build a function call expression: func(args...).
 */
static tnode_t *
build_function_call(const char *func_name, ncc_list_t *args, int line)
{
    tnode_t *primary = build_primary_id(func_name, line);

    tnode_t *postfix = synth_nonterminal("postfix_expression_9");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = 9;
    add_child(postfix, primary);

    tnode_t *call = synth_nonterminal("postfix_expression_1");
    call->nt_id   = NT_postfix_expression;
    call->branch  = 1;
    add_child(call, postfix);
    add_child(call, synth_terminal("(", TT_PUNCT, line));

    if (args && ncc_list_len(args) > 0) {
        tnode_t *arg_list = synth_nonterminal("argument_expression_list");
        arg_list->nt_id   = NT_argument_expression_list;

        int count = ncc_list_len(args);
        for (int i = 0; i < count; i++) {
            if (i > 0) {
                add_child(arg_list, synth_terminal(",", TT_PUNCT, line));
            }
            tnode_t *arg = ncc_list_get(args, i);
            add_child(arg_list, arg);
        }
        add_child(call, arg_list);
    }

    add_child(call, synth_terminal(")", TT_PUNCT, line));

    return call;
}

/**
 * @brief Build address-of expression: &operand
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

    tnode_t *unary = synth_nonterminal("unary_expression_2");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 2;
    add_child(unary, unary_op);
    add_child(unary, cast);

    return unary;
}

/**
 * @brief Build a cast expression: (type)expr
 */
static tnode_t *
build_cast(const char *type, tnode_t *operand, int line)
{
    tnode_t *type_name = synth_nonterminal("type_name_0");
    type_name->nt_id   = NT_type_name;
    type_name->branch  = 0;

    tnode_t *sql = synth_nonterminal("specifier_qualifier_list_1");
    sql->nt_id   = NT_specifier_qualifier_list;
    sql->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *ts = synth_nonterminal("type_specifier_5");
    ts->nt_id   = NT_type_specifier;
    ts->branch  = 5;

    tnode_t *typedef_name = synth_nonterminal("typedef_name_0");
    typedef_name->nt_id   = NT_typedef_name;
    typedef_name->branch  = 0;
    add_child(typedef_name, synth_terminal(type, TT_ID, line));

    add_child(ts, typedef_name);
    add_child(tsq, ts);
    add_child(sql, tsq);
    add_child(type_name, sql);

    tnode_t *cast = synth_nonterminal("cast_expression_0");
    cast->nt_id   = NT_cast_expression;
    cast->branch  = 0;
    add_child(cast, synth_terminal("(", TT_PUNCT, line));
    add_child(cast, type_name);
    add_child(cast, synth_terminal(")", TT_PUNCT, line));
    add_child(cast, operand);

    return cast;
}

/**
 * @brief Build a return statement: return expr;
 */
static tnode_t *
build_return_stmt(tnode_t *expr, int line)
{
    tnode_t *jump = synth_nonterminal("jump_statement_3");
    jump->nt_id   = NT_jump_statement;
    jump->branch  = 3;
    add_child(jump, synth_terminal("return", TT_KEYWORD, line));
    if (expr) {
        tnode_t *expression = synth_nonterminal("expression_1");
        expression->nt_id   = NT_expression;
        expression->branch  = 1;
        add_child(expression, expr);
        add_child(jump, expression);
    }
    add_child(jump, synth_terminal(";", TT_PUNCT, line));

    tnode_t *unlabeled = synth_nonterminal("unlabeled_statement_2");
    unlabeled->nt_id   = NT_unlabeled_statement;
    unlabeled->branch  = 2;
    add_child(unlabeled, jump);

    return unlabeled;
}

/**
 * @brief Build a case label: case N:
 */
static tnode_t *
build_case_label(const char *value, int line)
{
    tnode_t *const_expr = synth_nonterminal("constant_expression_0");
    const_expr->nt_id   = NT_constant_expression;
    const_expr->branch  = 0;
    add_child(const_expr, wrap_in_expr_hierarchy(build_constant(value, line), line));

    tnode_t *label = synth_nonterminal("label_2");
    label->nt_id   = NT_label;
    label->branch  = 2;
    add_child(label, synth_terminal("case", TT_KEYWORD, line));
    add_child(label, const_expr);
    add_child(label, synth_terminal(":", TT_PUNCT, line));

    return label;
}

/**
 * @brief Build a default label: default:
 */
static tnode_t *
build_default_label(int line)
{
    tnode_t *label = synth_nonterminal("label_3");
    label->nt_id   = NT_label;
    label->branch  = 3;
    add_child(label, synth_terminal("default", TT_KEYWORD, line));
    add_child(label, synth_terminal(":", TT_PUNCT, line));

    return label;
}

/**
 * @brief Build a block_item from a label.
 */
static tnode_t *
build_label_block_item(tnode_t *label)
{
    tnode_t *block_item = synth_nonterminal("block_item_1");
    block_item->nt_id   = NT_block_item;
    block_item->branch  = 1;
    add_child(block_item, label);
    return block_item;
}

/**
 * @brief Build a block_item from an unlabeled_statement.
 */
static tnode_t *
build_stmt_block_item(tnode_t *stmt)
{
    tnode_t *block_item = synth_nonterminal("block_item_2");
    block_item->nt_id   = NT_block_item;
    block_item->branch  = 2;
    add_child(block_item, stmt);
    return block_item;
}

/**
 * @brief Build expression statement from an expression.
 */
static tnode_t *
build_expr_stmt(tnode_t *expr, int line)
{
    tnode_t *expression = synth_nonterminal("expression_1");
    expression->nt_id   = NT_expression;
    expression->branch  = 1;
    add_child(expression, expr);

    tnode_t *expr_stmt = synth_nonterminal("expression_statement_1");
    expr_stmt->nt_id   = NT_expression_statement;
    expr_stmt->branch  = 1;
    add_child(expr_stmt, expression);
    add_child(expr_stmt, synth_terminal(";", TT_PUNCT, line));

    tnode_t *unlabeled = synth_nonterminal("unlabeled_statement_0");
    unlabeled->nt_id   = NT_unlabeled_statement;
    unlabeled->branch  = 0;
    add_child(unlabeled, expr_stmt);

    return unlabeled;
}

/**
 * @brief Build assignment expression: lhs = rhs
 */
static tnode_t *
build_assignment(tnode_t *lhs, tnode_t *rhs, int line)
{
    tnode_t *assign_op = synth_nonterminal("assignment_operator_0");
    assign_op->nt_id   = NT_assignment_operator;
    assign_op->branch  = 0;
    add_child(assign_op, synth_terminal("=", TT_PUNCT, line));

    tnode_t *unary = synth_nonterminal("unary_expression_9");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 9;
    add_child(unary, lhs);

    tnode_t *assign = synth_nonterminal("assignment_expression_0");
    assign->nt_id   = NT_assignment_expression;
    assign->branch  = 0;
    add_child(assign, unary);
    add_child(assign, assign_op);
    add_child(assign, rhs);

    return assign;
}

/**
 * @brief Build static variable declaration: static type name = init;
 */
static tnode_t *
build_static_var_decl(const char *type, const char *name, const char *init, int line)
{
    tnode_t *ext_decl = synth_nonterminal("external_declaration_1");
    ext_decl->nt_id   = NT_external_declaration;
    ext_decl->branch  = 1;

    tnode_t *decl = synth_nonterminal("declaration_1");
    decl->nt_id   = NT_declaration;
    decl->branch  = 1;

    // declaration_specifiers: static type
    tnode_t *decl_specs = synth_nonterminal("declaration_specifiers");
    decl_specs->nt_id   = NT_declaration_specifiers;

    tnode_t *storage_spec = synth_nonterminal("declaration_specifier_0");
    storage_spec->nt_id   = NT_declaration_specifier;
    storage_spec->branch  = 0;

    tnode_t *scs = synth_nonterminal("storage_class_specifier_0");
    scs->nt_id   = NT_storage_class_specifier;
    scs->branch  = 0;
    add_child(scs, synth_terminal("static", TT_KEYWORD, line));
    add_child(storage_spec, scs);
    add_child(decl_specs, storage_spec);

    tnode_t *type_spec_ds = synth_nonterminal("declaration_specifier_1");
    type_spec_ds->nt_id   = NT_declaration_specifier;
    type_spec_ds->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *ts = synth_nonterminal("type_specifier_5");
    ts->nt_id   = NT_type_specifier;
    ts->branch  = 5;

    tnode_t *typedef_name = synth_nonterminal("typedef_name_0");
    typedef_name->nt_id   = NT_typedef_name;
    typedef_name->branch  = 0;
    add_child(typedef_name, synth_terminal(type, TT_ID, line));

    add_child(ts, typedef_name);
    add_child(tsq, ts);
    add_child(type_spec_ds, tsq);
    add_child(decl_specs, type_spec_ds);
    add_child(decl, decl_specs);

    // init_declarator_list
    tnode_t *init_list = synth_nonterminal("init_declarator_list_0");
    init_list->nt_id   = NT_init_declarator_list;
    init_list->branch  = 0;

    tnode_t *init_decl = synth_nonterminal("init_declarator_0");
    init_decl->nt_id   = NT_init_declarator;
    init_decl->branch  = 0;

    tnode_t *declarator = synth_nonterminal("declarator_0");
    declarator->nt_id   = NT_declarator;
    declarator->branch  = 0;

    tnode_t *direct_decl = synth_nonterminal("direct_declarator_3");
    direct_decl->nt_id   = NT_direct_declarator;
    direct_decl->branch  = 3;
    add_child(direct_decl, build_identifier(name, line));

    add_child(declarator, direct_decl);
    add_child(init_decl, declarator);

    if (init) {
        add_child(init_decl, synth_terminal("=", TT_PUNCT, line));

        tnode_t *initializer = synth_nonterminal("initializer_1");
        initializer->nt_id   = NT_initializer;
        initializer->branch  = 1;
        add_child(initializer, wrap_in_expr_hierarchy(build_constant(init, line), line));
        add_child(init_decl, initializer);
    }

    add_child(init_list, init_decl);
    add_child(decl, init_list);
    add_child(decl, synth_terminal(";", TT_PUNCT, line));

    add_child(ext_decl, decl);

    return ext_decl;
}

/**
 * @brief Build static void pointer declaration: static void *name;
 */
static tnode_t *
build_static_void_ptr_decl(const char *name, int line)
{
    tnode_t *ext_decl = synth_nonterminal("external_declaration_1");
    ext_decl->nt_id   = NT_external_declaration;
    ext_decl->branch  = 1;

    tnode_t *decl = synth_nonterminal("declaration_1");
    decl->nt_id   = NT_declaration;
    decl->branch  = 1;

    // declaration_specifiers: static void
    tnode_t *decl_specs = synth_nonterminal("declaration_specifiers");
    decl_specs->nt_id   = NT_declaration_specifiers;

    tnode_t *storage_spec = synth_nonterminal("declaration_specifier_0");
    storage_spec->nt_id   = NT_declaration_specifier;
    storage_spec->branch  = 0;

    tnode_t *scs = synth_nonterminal("storage_class_specifier_0");
    scs->nt_id   = NT_storage_class_specifier;
    scs->branch  = 0;
    add_child(scs, synth_terminal("static", TT_KEYWORD, line));
    add_child(storage_spec, scs);
    add_child(decl_specs, storage_spec);

    tnode_t *type_spec_ds = synth_nonterminal("declaration_specifier_1");
    type_spec_ds->nt_id   = NT_declaration_specifier;
    type_spec_ds->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *ts = synth_nonterminal("type_specifier_0");
    ts->nt_id   = NT_type_specifier;
    ts->branch  = 0;
    add_child(ts, synth_terminal("void", TT_KEYWORD, line));

    add_child(tsq, ts);
    add_child(type_spec_ds, tsq);
    add_child(decl_specs, type_spec_ds);
    add_child(decl, decl_specs);

    // init_declarator_list with pointer
    tnode_t *init_list = synth_nonterminal("init_declarator_list_0");
    init_list->nt_id   = NT_init_declarator_list;
    init_list->branch  = 0;

    tnode_t *init_decl = synth_nonterminal("init_declarator_1");
    init_decl->nt_id   = NT_init_declarator;
    init_decl->branch  = 1;

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
    add_child(direct_decl, build_identifier(name, line));

    add_child(declarator, pointer);
    add_child(declarator, direct_decl);
    add_child(init_decl, declarator);

    add_child(init_list, init_decl);
    add_child(decl, init_list);
    add_child(decl, synth_terminal(";", TT_PUNCT, line));

    add_child(ext_decl, decl);

    return ext_decl;
}

// ---------------------------------------------------------------------------
// Once Transform
// ---------------------------------------------------------------------------

/**
 * @brief Remove 'once' specifier from the tree.
 */
static void
remove_once_specifier(tnode_t *once_node)
{
    if (!once_node || !once_node->parent) {
        return;
    }

    tnode_t *parent = once_node->parent;
    int      idx    = find_child_index(parent, once_node);

    if (idx >= 0) {
        remove_child_at(parent, idx);

        // If parent is now empty and is a declaration_specifier, remove it too
        if (parent->num_kids == 0 && parent->parent &&
            parent->nt_id == NT_declaration_specifier) {
            tnode_t *grandparent = parent->parent;
            int gp_idx = find_child_index(grandparent, parent);
            if (gp_idx >= 0) {
                remove_child_at(grandparent, gp_idx);
            }
        }
    }
}

/**
 * @brief Transform a declaration (prototype) with `once` specifier.
 */
static tnode_t *
xform_once_declaration(tree_xform_t *ctx, tnode_t *node)
{
    tnode_t *decl_specs = nullptr;
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
            break;
        }
    }

    if (!decl_specs) {
        return nullptr;
    }

    tnode_t *once_node = find_once_specifier(ctx->input, decl_specs);
    if (!once_node) {
        return nullptr;
    }

    // Remove the once specifier from the tree
    remove_once_specifier(once_node);

    return nullptr;
}

/**
 * @brief Build the wrapper function body with switch statement.
 */
static tnode_t *
build_wrapper_body(const char *func_name, const char *impl_name, tnode_t *param_list,
                   bool is_void, int line)
{
    // Build block_item_list for switch body
    tnode_t *switch_body_list = synth_nonterminal("block_item_list");
    switch_body_list->nt_id   = NT_block_item_list;

    char lock_name[NCC_IDENT_BUF];
    char cached_name[NCC_IDENT_BUF];
    int  lret = snprintf(lock_name, sizeof(lock_name), "__n00b_%s_once_lock", func_name);
    NCC_CHECK_SNPRINTF(lret, lock_name);
    int  cret = snprintf(cached_name, sizeof(cached_name), "__n00b_%s_cached_result", func_name);
    NCC_CHECK_SNPRINTF(cret, cached_name);

    // Collect parameter names for calling the implementation
    ncc_list_t *call_args = ncc_list_alloc(0);
    if (param_list) {
        for (int i = 0; i < param_list->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(param_list, i);
            if (kid && kid->nt_id == NT_parameter_declaration) {
                for (int j = 0; j < kid->num_kids; j++) {
                    tnode_t *decl = tnode_get_kid(kid, j);
                    if (decl && (decl->nt_id == NT_declarator || decl->nt_id == NT_direct_declarator)) {
                        // Find identifier in declarator
                        tnode_t *id = nullptr;
                        for (int k = 0; k < decl->num_kids; k++) {
                            tnode_t *c = tnode_get_kid(decl, k);
                            if (c && c->nt_id == NT_identifier) {
                                id = c;
                                break;
                            }
                            if (c && c->nt_id == NT_direct_declarator) {
                                for (int m = 0; m < c->num_kids; m++) {
                                    tnode_t *d = tnode_get_kid(c, m);
                                    if (d && d->nt_id == NT_identifier) {
                                        id = d;
                                        break;
                                    }
                                }
                            }
                        }
                        if (id) {
                            tnode_t *arg = wrap_in_expr_hierarchy(copy_tree(id), line);
                            call_args = ncc_list_append(call_args, arg);
                        }
                        break;
                    }
                }
            }
        }
    }

    // case 0: - first call, execute and cache
    add_child(switch_body_list, build_label_block_item(build_case_label("0", line)));

    // Build the implementation call
    tnode_t *impl_call = build_function_call(impl_name, call_args, line);

    if (is_void) {
        // Just call: impl();
        add_child(switch_body_list, build_stmt_block_item(build_expr_stmt(wrap_in_expr_hierarchy(impl_call, line), line)));
    }
    else {
        // Store result: cached = (int64_t)impl();
        tnode_t *call_wrapped = wrap_in_expr_hierarchy(impl_call, line);
        tnode_t *cast_i64     = build_cast("int64_t", call_wrapped, line);
        tnode_t *lhs = build_primary_id(cached_name, line);

        tnode_t *postfix_lhs = synth_nonterminal("postfix_expression_9");
        postfix_lhs->nt_id   = NT_postfix_expression;
        postfix_lhs->branch  = 9;
        add_child(postfix_lhs, lhs);

        tnode_t *assignment = build_assignment(postfix_lhs, wrap_in_expr_hierarchy(cast_i64, line), line);
        add_child(switch_body_list, build_stmt_block_item(build_expr_stmt(assignment, line)));
    }

    // Atomic store and wake
    {
        // __atomic_store_n(&lock, ~0, __ATOMIC_RELEASE)
        ncc_list_t *store_args = ncc_list_alloc(3);
        tnode_t *lock_addr = build_address_of(build_primary_id(lock_name, line), line);
        store_args->data[0] = wrap_in_expr_hierarchy(lock_addr, line);
        store_args->data[1] = wrap_in_expr_hierarchy(build_constant("0xFFFFFFFF", line), line);
        store_args->data[2] = wrap_in_expr_hierarchy(build_primary_id("__ATOMIC_RELEASE", line), line);
        tnode_t *store_call = build_function_call("__atomic_store_n", store_args, line);
        add_child(switch_body_list, build_stmt_block_item(build_expr_stmt(wrap_in_expr_hierarchy(store_call, line), line)));
        base_dealloc(store_args);

        // base_futex_wake
        ncc_list_t *wake_args = ncc_list_alloc(2);
        tnode_t *futex_addr = build_address_of(build_primary_id(lock_name, line), line);
        // Cast to base_futex_t*
        tnode_t *futex_cast = build_cast("base_futex_t", wrap_in_expr_hierarchy(futex_addr, line), line);
        wake_args->data[0] = wrap_in_expr_hierarchy(futex_cast, line);
        wake_args->data[1] = wrap_in_expr_hierarchy(build_primary_id("true", line), line);
        tnode_t *wake_call = build_function_call("base_futex_wake", wake_args, line);
        add_child(switch_body_list, build_stmt_block_item(build_expr_stmt(wrap_in_expr_hierarchy(wake_call, line), line)));
        base_dealloc(wake_args);
    }

    // Return
    if (is_void) {
        tnode_t *ret = build_return_stmt(nullptr, line);
        add_child(switch_body_list, build_stmt_block_item(ret));
    }
    else {
        tnode_t *cached_expr = wrap_in_expr_hierarchy(build_primary_id(cached_name, line), line);
        tnode_t *ret         = build_return_stmt(cached_expr, line);
        add_child(switch_body_list, build_stmt_block_item(ret));
    }

    // case 1: - waiting
    add_child(switch_body_list, build_label_block_item(build_case_label("1", line)));

    // Note: Simplified - in real implementation would have futex_wait loop
    // For now, just fall through to return cached

    // default: - already done
    add_child(switch_body_list, build_label_block_item(build_default_label(line)));

    if (is_void) {
        tnode_t *ret = build_return_stmt(nullptr, line);
        add_child(switch_body_list, build_stmt_block_item(ret));
    }
    else {
        tnode_t *cached_expr = wrap_in_expr_hierarchy(build_primary_id(cached_name, line), line);
        tnode_t *ret         = build_return_stmt(cached_expr, line);
        add_child(switch_body_list, build_stmt_block_item(ret));
    }

    // Build switch compound statement
    tnode_t *switch_compound = synth_nonterminal("compound_statement_0");
    switch_compound->nt_id   = NT_compound_statement;
    switch_compound->branch  = BRANCH(compound_statement, WITH_ITEMS);
    add_child(switch_compound, synth_terminal("{", TT_PUNCT, line));
    add_child(switch_compound, switch_body_list);
    add_child(switch_compound, synth_terminal("}", TT_PUNCT, line));

    // Build switch statement
    tnode_t *switch_stmt = synth_nonterminal("selection_statement_2");
    switch_stmt->nt_id   = NT_selection_statement;
    switch_stmt->branch  = BRANCH(selection_statement, SWITCH);
    add_child(switch_stmt, synth_terminal("switch", TT_KEYWORD, line));
    add_child(switch_stmt, synth_terminal("(", TT_PUNCT, line));

    // Switch expression: __atomic_fetch_or(&lock, 1, __ATOMIC_ACQ_REL)
    ncc_list_t *fetch_args = ncc_list_alloc(3);
    tnode_t *lock_addr = build_address_of(build_primary_id(lock_name, line), line);
    fetch_args->data[0] = wrap_in_expr_hierarchy(lock_addr, line);
    fetch_args->data[1] = wrap_in_expr_hierarchy(build_constant("1", line), line);
    fetch_args->data[2] = wrap_in_expr_hierarchy(build_primary_id("__ATOMIC_ACQ_REL", line), line);
    tnode_t *fetch_call = build_function_call("__atomic_fetch_or", fetch_args, line);

    tnode_t *sel_header = synth_nonterminal("selection_header_2");
    sel_header->nt_id   = NT_selection_header;
    sel_header->branch  = 2;

    tnode_t *sel_expr = synth_nonterminal("expression_1");
    sel_expr->nt_id   = NT_expression;
    sel_expr->branch  = 1;
    add_child(sel_expr, wrap_in_expr_hierarchy(fetch_call, line));
    add_child(sel_header, sel_expr);

    add_child(switch_stmt, sel_header);
    add_child(switch_stmt, synth_terminal(")", TT_PUNCT, line));

    // Secondary block with switch body
    tnode_t *sec_block = synth_nonterminal("secondary_block_0");
    sec_block->nt_id   = NT_secondary_block;
    sec_block->branch  = 0;

    tnode_t *statement = synth_nonterminal("statement_1");
    statement->nt_id   = NT_statement;
    statement->branch  = 1;

    tnode_t *unlabeled = synth_nonterminal("unlabeled_statement_1");
    unlabeled->nt_id   = NT_unlabeled_statement;
    unlabeled->branch  = 1;

    tnode_t *prim_block = synth_nonterminal("primary_block_0");
    prim_block->nt_id   = NT_primary_block;
    prim_block->branch  = 0;
    add_child(prim_block, switch_compound);

    add_child(unlabeled, prim_block);
    add_child(statement, unlabeled);
    add_child(sec_block, statement);
    add_child(switch_stmt, sec_block);

    base_dealloc(fetch_args);
    base_dealloc(call_args);

    // Build primary_block with selection_statement
    tnode_t *wrapper_prim = synth_nonterminal("primary_block_1");
    wrapper_prim->nt_id   = NT_primary_block;
    wrapper_prim->branch  = 1;
    add_child(wrapper_prim, switch_stmt);

    // Build unlabeled_statement
    tnode_t *wrapper_unlabeled = synth_nonterminal("unlabeled_statement_1");
    wrapper_unlabeled->nt_id   = NT_unlabeled_statement;
    wrapper_unlabeled->branch  = 1;
    add_child(wrapper_unlabeled, wrapper_prim);

    // Build block_item
    tnode_t *block_item = synth_nonterminal("block_item_2");
    block_item->nt_id   = NT_block_item;
    block_item->branch  = 2;
    add_child(block_item, wrapper_unlabeled);

    // Build block_item_list
    tnode_t *block_list = synth_nonterminal("block_item_list_1");
    block_list->nt_id   = NT_block_item_list;
    block_list->branch  = 1;
    add_child(block_list, block_item);

    // Build compound_statement
    tnode_t *compound = synth_nonterminal("compound_statement_0");
    compound->nt_id   = NT_compound_statement;
    compound->branch  = BRANCH(compound_statement, WITH_ITEMS);
    add_child(compound, synth_terminal("{", TT_PUNCT, line));
    add_child(compound, block_list);
    add_child(compound, synth_terminal("}", TT_PUNCT, line));

    // Build function_body
    tnode_t *func_body = synth_nonterminal("function_body_0");
    func_body->nt_id   = NT_function_body;
    func_body->branch  = 0;
    add_child(func_body, compound);

    return func_body;
}

/**
 * @brief Transform a function definition with `once` specifier.
 */
static tnode_t *
xform_once_definition(tree_xform_t *ctx, tnode_t *node)
{
    // Find children
    tnode_t *decl_specs = nullptr;
    tnode_t *declarator = nullptr;
    tnode_t *func_body  = nullptr;

    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (!is_valid_node(kid)) {
            continue;
        }
        if (kid->nt_id == NT_declaration_specifiers) {
            decl_specs = kid;
        }
        else if (kid->nt_id == NT_declarator) {
            declarator = kid;
        }
        else if (kid->nt_id == NT_function_body) {
            func_body = kid;
        }
    }

    if (!decl_specs || !declarator || !func_body) {
        return nullptr;
    }

    // Check for 'once' specifier
    tnode_t *once_node = find_once_specifier(ctx->input, decl_specs);
    if (!once_node) {
        return nullptr;
    }

    // Extract function information
    char *func_name = extract_declarator_name(ctx->input, declarator);
    bool is_void    = is_void_return(ctx->input, decl_specs);
    int line        = get_node_line(node);

    if (!func_name) {
        return nullptr;
    }

    // Build names for generated code
    char impl_name[NCC_IDENT_BUF];
    char lock_name[NCC_IDENT_BUF];
    char cached_name[NCC_IDENT_BUF];
    int  iret = snprintf(impl_name, sizeof(impl_name), "__n00b_once_func_%s", func_name);
    NCC_CHECK_SNPRINTF(iret, impl_name);
    int  lret = snprintf(lock_name, sizeof(lock_name), "__n00b_%s_once_lock", func_name);
    NCC_CHECK_SNPRINTF(lret, lock_name);
    int  cret = snprintf(cached_name, sizeof(cached_name), "__n00b_%s_cached_result", func_name);
    NCC_CHECK_SNPRINTF(cret, cached_name);

    // Find the translation_unit (grandparent of function_definition)
    tnode_t *ext_decl = node->parent;
    tnode_t *trans_unit = ext_decl ? ext_decl->parent : nullptr;

    if (!trans_unit || trans_unit->nt_id != NT_translation_unit) {
        base_dealloc(func_name);
        return nullptr;
    }

    int func_idx = find_child_index(trans_unit, ext_decl);
    if (func_idx < 0) {
        base_dealloc(func_name);
        return nullptr;
    }

    // 1. Insert static lock variable before the function
    tnode_t *lock_decl = build_static_var_decl("uint32_t", lock_name, "0", line);
    insert_child_at(trans_unit, func_idx, lock_decl);
    func_idx++;

    // 2. Insert cached result variable (non-void only)
    if (!is_void) {
        tnode_t *cached_decl = build_static_void_ptr_decl(cached_name, line);
        insert_child_at(trans_unit, func_idx, cached_decl);
        func_idx++;
    }

    // 3. Remove 'once' from declaration_specifiers
    remove_once_specifier(once_node);

    // 4. Find the param_list for the wrapper
    tnode_t *param_list = find_param_list(declarator);

    // 5. Build wrapper function body (replaces original body)
    tnode_t *wrapper_body = build_wrapper_body(func_name, impl_name, param_list, is_void, line);

    // 6. Clone the original function as the implementation
    tnode_t *impl_ext_decl = synth_nonterminal("external_declaration_0");
    impl_ext_decl->nt_id   = NT_external_declaration;
    impl_ext_decl->branch  = 0;

    tnode_t *impl_def = synth_nonterminal("function_definition_1");
    impl_def->nt_id   = NT_function_definition;
    impl_def->branch  = 1;

    // Copy and modify declaration_specifiers (add static)
    tnode_t *impl_specs = copy_tree(decl_specs);

    // Add static specifier at the beginning
    tnode_t *static_spec = synth_nonterminal("declaration_specifier_0");
    static_spec->nt_id   = NT_declaration_specifier;
    static_spec->branch  = 0;

    tnode_t *scs = synth_nonterminal("storage_class_specifier_0");
    scs->nt_id   = NT_storage_class_specifier;
    scs->branch  = 0;
    add_child(scs, synth_terminal("static", TT_KEYWORD, line));
    add_child(static_spec, scs);

    // Insert at beginning of impl_specs
    insert_child_at(impl_specs, 0, static_spec);

    add_child(impl_def, impl_specs);

    // Copy declarator and rename function
    tnode_t *impl_declarator = copy_tree(declarator);

    // Find and rename the identifier in the declarator
    tnode_t *name_dd = find_name_direct_declarator(impl_declarator);
    if (name_dd) {
        for (int i = 0; i < name_dd->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(name_dd, i);
            if (kid && kid->nt_id == NT_identifier) {
                // Replace with new identifier
                name_dd->kids->data[i] = build_identifier(impl_name, line);
                break;
            }
        }
    }

    add_child(impl_def, impl_declarator);

    // Copy the function body to the implementation (don't reparent the
    // original — it stays in `node` until replaced by wrapper_body below).
    add_child(impl_def, copy_tree(func_body));

    add_child(impl_ext_decl, impl_def);

    // 7. Replace original function body with wrapper body
    int body_idx = find_child_index(node, func_body);
    if (body_idx >= 0) {
        node->kids->data[body_idx] = wrapper_body;
        wrapper_body->parent = node;
    }

    // 8. Insert implementation function after the wrapper
    insert_child_at(trans_unit, func_idx + 1, impl_ext_decl);

    base_dealloc(func_name);

    return nullptr;
}

/**
 * @brief Register the once transformation.
 */
void
register_once_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_function_definition, xform_once_definition, "once_def");
    xform_register_post(reg, NT_declaration, xform_once_declaration, "once_decl");
}
