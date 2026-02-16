/**
 * @file xform_bang.c
 * @brief Transforms postfix `!` (error propagation) into Result-handling code.
 *
 * The postfix `!` operator is inspired by Rust's `?` operator. It:
 *   1. Evaluates the expression (which must return a Result type)
 *   2. If `.is_ok` is true, yields the `.ok` value
 *   3. If `.is_ok` is false, early-returns from the function with the error
 *
 * Example transformation (inside `result_t foo(void)`):
 *   bar()!
 * Becomes:
 *   ({
 *       __auto_type _ncc_try = (bar());
 *       if (!_ncc_try.is_ok) {
 *           return (result_t){ .is_ok = false, .err = _ncc_try.err };
 *       }
 *       _ncc_try.ok;
 *   })
 *
 * The compound literal in the return statement uses the enclosing function's
 * return type (not typeof on the expression), so that cross-Result-type
 * propagation works correctly.
 *
 * Grammar: see postfix_expression branch 10 in src/parse.c
 */

#include "branch_symbols.h"
#include "transform.h"
#include "rewrite.h"
#include "types.h"
#include "nt_types.h"

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

// Maximum length for generated variable names
#define VAR_NAME_SIZE 32

// ---------------------------------------------------------------------------
// Node Validation
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
// Helper: Check if declaration_specifiers contains "void" as return type.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Helper: Extract identifier name from a declarator subtree.
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Tree Building Helpers
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
 * @brief Build a simple primary_expression containing an identifier.
 * primary_expression_7 -> identifier
 */
static tnode_t *
build_primary_id(const char *name, int line)
{
    tnode_t *primary = synth_nonterminal("primary_expression_7");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 7;
    add_child(primary, build_identifier(name, line));
    return primary;
}

/**
 * @brief Build a parenthesized expression.
 * primary_expression_3 -> ( expression )
 */
static tnode_t *
build_paren_expr(tnode_t *inner, int line)
{
    tnode_t *primary = synth_nonterminal("primary_expression_3");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 3;

    // Need to wrap inner in expression hierarchy if it isn't already
    tnode_t *expr = synth_nonterminal("expression_1");
    expr->nt_id   = NT_expression;
    expr->branch  = 1;

    tnode_t *assign = synth_nonterminal("assignment_expression_1");
    assign->nt_id   = NT_assignment_expression;
    assign->branch  = 1;
    add_child(assign, inner);
    add_child(expr, assign);

    add_child(primary, synth_terminal("(", TT_PUNCT, line));
    add_child(primary, expr);
    add_child(primary, synth_terminal(")", TT_PUNCT, line));

    return primary;
}

/**
 * @brief Build postfix member access: expr.member
 * postfix_expression_3 -> postfix_expression . identifier
 */
static tnode_t *
build_member_access(tnode_t *base, const char *member, int line)
{
    tnode_t *postfix = synth_nonterminal("postfix_expression_3");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = 3;

    add_child(postfix, base);
    add_child(postfix, synth_terminal(".", TT_PUNCT, line));
    add_child(postfix, build_identifier(member, line));

    return postfix;
}

/**
 * @brief Build unary not: !expr
 * unary_expression_1 -> unary_operator cast_expression
 */
static tnode_t *
build_unary_not(tnode_t *operand, int line)
{
    tnode_t *unary_op = synth_nonterminal("unary_operator");
    unary_op->nt_id   = NT_unary_operator;
    add_child(unary_op, synth_terminal("!", TT_PUNCT, line));

    tnode_t *cast = synth_nonterminal("cast_expression_1");
    cast->nt_id   = NT_cast_expression;
    cast->branch  = 1;
    add_child(cast, operand);

    tnode_t *unary = synth_nonterminal("unary_expression_1");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 1;
    add_child(unary, unary_op);
    add_child(unary, cast);

    return unary;
}

/**
 * @brief Wrap an expression in the full expression hierarchy up to assignment_expression.
 * This is needed because many places in the grammar expect assignment_expression.
 */
static tnode_t *
wrap_in_expr_hierarchy(tnode_t *inner, int line)
{
    (void)line;

    // Build chain: unary -> cast -> mult -> add -> shift -> rel -> eq -> and -> xor -> or -> land -> lor -> cond -> assign
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
 * @brief Build an initializer_list entry with designation.
 */
static tnode_t *
build_designated_init(const char *member, tnode_t *value, int line)
{
    tnode_t *init_item = synth_nonterminal("initializer_list_0");
    init_item->nt_id   = NT_initializer_list;
    init_item->branch  = 0;

    add_child(init_item, build_designation(member, line));

    tnode_t *init = synth_nonterminal("initializer_1");
    init->nt_id   = NT_initializer;
    init->branch  = 1;
    add_child(init, wrap_in_expr_hierarchy(value, line));
    add_child(init_item, init);

    return init_item;
}

/**
 * @brief Convert declaration_specifiers children into a specifier_qualifier_list.
 *
 * Walks the declaration_specifiers tree, deep-copies relevant type specifier
 * children, and wraps them in a specifier_qualifier_list. Skips
 * storage_class_specifier children (e.g., static, extern).
 */
static tnode_t *
decl_specs_to_spec_qual_list(tnode_t *decl_specs)
{
    tnode_t *sql = synth_nonterminal("specifier_qualifier_list");
    sql->nt_id   = NT_specifier_qualifier_list;

    for (int i = 0; i < decl_specs->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(decl_specs, i);
        if (!is_valid_node(kid)) {
            continue;
        }

        // declaration_specifier wraps either storage_class_specifier or
        // type_specifier_qualifier. We skip storage class specifiers.
        if (kid->nt_id == NT_declaration_specifier) {
            // Check if this wraps a storage_class_specifier — skip it
            tnode_t *inner = tnode_get_kid(kid, 0);
            if (inner && inner->nt_id == NT_storage_class_specifier) {
                continue;
            }
            // Otherwise it wraps a type_specifier_qualifier — copy and add
            if (inner && inner->nt_id == NT_type_specifier_qualifier) {
                add_child(sql, copy_tree(inner));
            }
        }
        else if (kid->nt_id == NT_type_specifier_qualifier) {
            // Already a type_specifier_qualifier — copy directly
            add_child(sql, copy_tree(kid));
        }
    }

    return sql;
}

/**
 * @brief Build a compound literal using the enclosing function's return type.
 *
 * compound_literal_1 -> ( type_name ) braced_initializer
 *
 * Constructs: (ReturnType){ .field1 = val1, .field2 = val2 }
 * where ReturnType comes from the function_definition's declaration_specifiers
 * and optional pointer from the declarator.
 */
static tnode_t *
build_return_compound_literal(tnode_t    *func_def,
                              const char *field1,
                              tnode_t    *val1,
                              const char *field2,
                              tnode_t    *val2,
                              int         line)
{
    tnode_t *literal = synth_nonterminal("compound_literal_1");
    literal->nt_id   = NT_compound_literal;
    literal->branch  = 1;

    add_child(literal, synth_terminal("(", TT_PUNCT, line));

    // Build type_name from function's return type
    tnode_t *decl_specs = find_child(func_def, NT_declaration_specifiers);
    tnode_t *declarator = find_child(func_def, NT_declarator);

    tnode_t *type_name = synth_nonterminal("type_name_0");
    type_name->nt_id   = NT_type_name;
    type_name->branch  = 0;

    // Convert declaration_specifiers -> specifier_qualifier_list
    tnode_t *sql = decl_specs_to_spec_qual_list(decl_specs);
    add_child(type_name, sql);

    // Check for pointer in declarator (e.g., int *foo -> need * in type_name)
    if (declarator) {
        tnode_t *pointer = find_child(declarator, NT_pointer);
        if (pointer) {
            tnode_t *abs_decl = synth_nonterminal("abstract_declarator_0");
            abs_decl->nt_id   = NT_abstract_declarator;
            abs_decl->branch  = 0;
            add_child(abs_decl, copy_tree(pointer));
            add_child(type_name, abs_decl);
        }
    }

    add_child(literal, type_name);
    add_child(literal, synth_terminal(")", TT_PUNCT, line));

    // Braced initializer
    tnode_t *braced = synth_nonterminal("braced_initializer_0");
    braced->nt_id   = NT_braced_initializer;
    braced->branch  = 0;

    add_child(braced, synth_terminal("{", TT_PUNCT, line));

    // Build initializer_list with two items
    tnode_t *init_list = synth_nonterminal("initializer_list");
    init_list->nt_id   = NT_initializer_list;

    // First: .field1 = val1
    tnode_t *item1 = build_designated_init(field1, val1, line);
    add_child(init_list, item1);

    add_child(init_list, synth_terminal(",", TT_PUNCT, line));

    // Second: .field2 = val2
    tnode_t *item2 = build_designated_init(field2, val2, line);
    add_child(init_list, item2);

    add_child(braced, init_list);
    add_child(braced, synth_terminal("}", TT_PUNCT, line));

    add_child(literal, braced);

    return literal;
}

/**
 * @brief Build expression_statement: expression ;
 */
static tnode_t *
build_expr_stmt(tnode_t *expr, int line)
{
    tnode_t *expr_stmt = synth_nonterminal("expression_statement_1");
    expr_stmt->nt_id   = NT_expression_statement;
    expr_stmt->branch  = 1;

    tnode_t *expr_wrap = synth_nonterminal("expression_1");
    expr_wrap->nt_id   = NT_expression;
    expr_wrap->branch  = 1;
    add_child(expr_wrap, wrap_in_expr_hierarchy(expr, line));

    add_child(expr_stmt, expr_wrap);
    add_child(expr_stmt, synth_terminal(";", TT_PUNCT, line));

    return expr_stmt;
}

/**
 * @brief Build jump_statement for return: return expression ;
 */
static tnode_t *
build_return_stmt(tnode_t *expr, int line)
{
    tnode_t *jump = synth_nonterminal("jump_statement_5");
    jump->nt_id   = NT_jump_statement;
    jump->branch  = 5;

    add_child(jump, synth_terminal("return", TT_KEYWORD, line));

    tnode_t *expr_wrap = synth_nonterminal("expression_1");
    expr_wrap->nt_id   = NT_expression;
    expr_wrap->branch  = 1;
    add_child(expr_wrap, wrap_in_expr_hierarchy(expr, line));

    add_child(jump, expr_wrap);
    add_child(jump, synth_terminal(";", TT_PUNCT, line));

    return jump;
}

/**
 * @brief Build compound_statement: { block_item_list }
 */
static tnode_t *
build_compound_stmt(tnode_t *block_items, int line)
{
    tnode_t *compound = synth_nonterminal("compound_statement_0");
    compound->nt_id   = NT_compound_statement;
    compound->branch  = 0;

    add_child(compound, synth_terminal("{", TT_PUNCT, line));
    add_child(compound, block_items);
    add_child(compound, synth_terminal("}", TT_PUNCT, line));

    return compound;
}

/**
 * @brief Build selection_statement_1: if ( expression ) secondary_block
 */
static tnode_t *
build_if_stmt(tnode_t *cond, tnode_t *body, int line)
{
    tnode_t *sel = synth_nonterminal("selection_statement_1");
    sel->nt_id   = NT_selection_statement;
    sel->branch  = 1;

    add_child(sel, synth_terminal("if", TT_KEYWORD, line));
    add_child(sel, synth_terminal("(", TT_PUNCT, line));

    // selection_header_0 -> expression
    tnode_t *header = synth_nonterminal("selection_header_0");
    header->nt_id   = NT_selection_header;
    header->branch  = 0;

    tnode_t *expr_wrap = synth_nonterminal("expression_1");
    expr_wrap->nt_id   = NT_expression;
    expr_wrap->branch  = 1;
    add_child(expr_wrap, wrap_in_expr_hierarchy(cond, line));
    add_child(header, expr_wrap);

    add_child(sel, header);
    add_child(sel, synth_terminal(")", TT_PUNCT, line));

    // secondary_block_0 -> statement
    tnode_t *secondary = synth_nonterminal("secondary_block_0");
    secondary->nt_id   = NT_secondary_block;
    secondary->branch  = 0;

    // statement_1 -> unlabeled_statement
    tnode_t *stmt = synth_nonterminal("statement_1");
    stmt->nt_id   = NT_statement;
    stmt->branch  = 1;

    // unlabeled_statement_3 -> compound_statement
    tnode_t *unlabeled = synth_nonterminal("unlabeled_statement_3");
    unlabeled->nt_id   = NT_unlabeled_statement;
    unlabeled->branch  = 3;
    add_child(unlabeled, body);

    add_child(stmt, unlabeled);
    add_child(secondary, stmt);
    add_child(sel, secondary);

    return sel;
}

/**
 * @brief Build a declaration: type_specifier declarator = initializer ;
 */
static tnode_t *
build_declaration(const char *type_text, const char *var_name, tnode_t *init_expr, int line)
{
    tnode_t *decl = synth_nonterminal("declaration_1");
    decl->nt_id   = NT_declaration;
    decl->branch  = 1;

    // declaration_specifiers
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
    add_child(type_spec, synth_terminal(type_text, TT_KEYWORD, line));

    add_child(tsq, type_spec);
    add_child(decl_spec, tsq);
    add_child(decl_specs, decl_spec);
    add_child(decl, decl_specs);

    // init_declarator_list
    tnode_t *init_decl_list = synth_nonterminal("init_declarator_list_0");
    init_decl_list->nt_id   = NT_init_declarator_list;
    init_decl_list->branch  = 0;

    tnode_t *init_decl = synth_nonterminal("init_declarator_0");
    init_decl->nt_id   = NT_init_declarator;
    init_decl->branch  = 0;

    // declarator -> direct_declarator -> identifier
    tnode_t *declarator = synth_nonterminal("declarator_0");
    declarator->nt_id   = NT_declarator;
    declarator->branch  = 0;

    tnode_t *direct_decl = synth_nonterminal("direct_declarator_3");
    direct_decl->nt_id   = NT_direct_declarator;
    direct_decl->branch  = 3;
    add_child(direct_decl, build_identifier(var_name, line));

    add_child(declarator, direct_decl);
    add_child(init_decl, declarator);

    add_child(init_decl, synth_terminal("=", TT_PUNCT, line));

    // initializer
    tnode_t *init = synth_nonterminal("initializer_1");
    init->nt_id   = NT_initializer;
    init->branch  = 1;
    add_child(init, wrap_in_expr_hierarchy(init_expr, line));

    add_child(init_decl, init);
    add_child(init_decl_list, init_decl);
    add_child(decl, init_decl_list);

    add_child(decl, synth_terminal(";", TT_PUNCT, line));

    return decl;
}

/**
 * @brief Build a block_item containing a declaration.
 */
static tnode_t *
build_block_item_decl(tnode_t *declaration)
{
    tnode_t *item = synth_nonterminal("block_item_0");
    item->nt_id   = NT_block_item;
    item->branch  = 0;
    add_child(item, declaration);
    return item;
}

/**
 * @brief Build a block_item containing an unlabeled_statement (expression or primary_block).
 */
static tnode_t *
build_block_item_stmt(tnode_t *stmt, int branch)
{
    tnode_t *item = synth_nonterminal("block_item_2");
    item->nt_id   = NT_block_item;
    item->branch  = 2;

    tnode_t *unlabeled = synth_nonterminal("unlabeled_statement");
    unlabeled->nt_id   = NT_unlabeled_statement;
    unlabeled->branch  = branch;
    add_child(unlabeled, stmt);

    add_child(item, unlabeled);
    return item;
}

/**
 * @brief Build primary_block containing selection_statement.
 */
static tnode_t *
build_primary_block_selection(tnode_t *sel_stmt)
{
    tnode_t *block = synth_nonterminal("primary_block_1");
    block->nt_id   = NT_primary_block;
    block->branch  = 1;
    add_child(block, sel_stmt);
    return block;
}

// ---------------------------------------------------------------------------
// Main Transform
// ---------------------------------------------------------------------------

/**
 * @brief Transform postfix_expression with `!` operator.
 *
 * Node structure for branch 10:
 *   kids[0] = postfix_expression (the expression before !)
 *   kids[1] = "!" token (unused in output)
 *
 * Builds a proper tree structure for:
 *   ({ __auto_type _ncc_try_N = (expr); if (!_ncc_try_N.is_ok) { return ...; } _ncc_try_N.ok; })
 *
 * @param ctx  Transform context
 * @param node The postfix_expression node to transform
 * @return Replacement node, or nullptr if not a `!` branch
 */
static tnode_t *
xform_postfix_bang(xform_ctx_t *ctx, tnode_t *node)
{
    if (node->branch != BRANCH(postfix_expression, BANG)) {
        return nullptr;
    }

    tnode_t *expr = tnode_get_kid(node, 0);
    if (!expr) {
        return nullptr;
    }

    int         line = get_node_line(node);
    const char *file = ctx->lex ? ctx->lex->in_file : nullptr;

    // Error: '!' used outside any function
    if (!ctx->current_func_def) {
        ncc_error("%s:%d: '!' operator used outside of function\n",
                  file ? file : "<unknown>",
                  line);
        exit(1);
    }

    // Error: '!' used in void function
    tnode_t *func_decl_specs = find_child(ctx->current_func_def, NT_declaration_specifiers);
    tnode_t *func_declarator = find_child(ctx->current_func_def, NT_declarator);
    bool     has_pointer     = func_declarator && find_child(func_declarator, NT_pointer);

    if (func_decl_specs && !has_pointer && is_void_return(ctx->input, func_decl_specs)) {
        char *func_name = nullptr;
        if (func_declarator) {
            func_name = extract_declarator_name(ctx->input, func_declarator);
        }
        ncc_error("%s:%d: '!' cannot be used in void function '%s'\n",
                  file ? file : "<unknown>",
                  line,
                  func_name ? func_name : "<unknown>");
        base_dealloc(func_name);
        exit(1);
    }

    char varname[VAR_NAME_SIZE];
    snprintf(varname, VAR_NAME_SIZE, "_ncc_try_%d", node->id);

    // Build the statement expression tree structure
    // primary_expression_6 -> ( compound_statement )

    tnode_t *result = synth_nonterminal("primary_expression_6");
    result->nt_id   = NT_primary_expression;
    result->branch  = 6;

    add_child(result, synth_terminal("(", TT_PUNCT, line));

    // Build block_item_list with 3 items:
    // 1. Declaration: __auto_type varname = (expr);
    // 2. If statement: if (!varname.is_ok) { return ...; }
    // 3. Final expression: varname.ok;

    tnode_t *block_list = synth_nonterminal("block_item_list");
    block_list->nt_id   = NT_block_item_list;

    // Item 1: Declaration
    // Wrap original expression in parentheses
    tnode_t *paren_expr = build_paren_expr(expr, line);
    tnode_t *decl       = build_declaration("__auto_type", varname, paren_expr, line);
    add_child(block_list, build_block_item_decl(decl));

    // Item 2: If statement
    // Condition: !varname.is_ok
    tnode_t *var_ref1    = build_primary_id(varname, line);
    tnode_t *is_ok_check = build_member_access(var_ref1, "is_ok", line);
    tnode_t *not_cond    = build_unary_not(is_ok_check, line);

    // Return expression: (typeof(varname)){ .is_ok = false, .err = varname.err }
    tnode_t *false_val   = synth_nonterminal("primary_expression_1");
    false_val->nt_id     = NT_primary_expression;
    false_val->branch    = 1;
    tnode_t *false_const = synth_nonterminal("constant");
    false_const->nt_id   = NT_constant;
    add_child(false_const, synth_terminal("false", TT_KEYWORD, line));
    add_child(false_val, false_const);

    tnode_t *var_ref2 = build_primary_id(varname, line);
    tnode_t *err_val  = build_member_access(var_ref2, "err", line);
    tnode_t *ret_expr = build_return_compound_literal(ctx->current_func_def, "is_ok", false_val, "err", err_val, line);
    tnode_t *ret_stmt = build_return_stmt(ret_expr, line);

    // Build if body: { return ...; }
    tnode_t *if_body_list = synth_nonterminal("block_item_list");
    if_body_list->nt_id   = NT_block_item_list;

    tnode_t *if_body_item = synth_nonterminal("block_item_2");
    if_body_item->nt_id   = NT_block_item;
    if_body_item->branch  = 2;

    tnode_t *if_body_unlabeled = synth_nonterminal("unlabeled_statement_2");
    if_body_unlabeled->nt_id   = NT_unlabeled_statement;
    if_body_unlabeled->branch  = 2;
    add_child(if_body_unlabeled, ret_stmt);
    add_child(if_body_item, if_body_unlabeled);
    add_child(if_body_list, if_body_item);

    tnode_t *if_body = build_compound_stmt(if_body_list, line);
    tnode_t *if_stmt = build_if_stmt(not_cond, if_body, line);

    // Wrap in primary_block for selection statement
    tnode_t *primary_block = build_primary_block_selection(if_stmt);
    tnode_t *if_block_item = build_block_item_stmt(primary_block, 1);
    add_child(block_list, if_block_item);

    // Item 3: Final expression statement: varname.ok;
    tnode_t *var_ref3   = build_primary_id(varname, line);
    tnode_t *ok_val     = build_member_access(var_ref3, "ok", line);
    tnode_t *final_expr = build_expr_stmt(ok_val, line);
    tnode_t *final_item = build_block_item_stmt(final_expr, 0);
    add_child(block_list, final_item);

    // Build compound_statement
    tnode_t *compound = build_compound_stmt(block_list, line);
    add_child(result, compound);

    add_child(result, synth_terminal(")", TT_PUNCT, line));

    replace_node(node, result, "bang");
    return result;
}

/**
 * @brief Register the postfix `!` transformation.
 *
 * @param reg Registry to add the transformation to
 */
void
register_bang_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_postfix_expression, xform_postfix_bang, "bang");
}
