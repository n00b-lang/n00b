/**
 * @file xform_kw_call.c
 * @brief Transforms keyword argument call sites into struct initializers.
 *
 * Transforms:
 *   foo(x, name: value, other: val2)
 * Into:
 *   foo(x, &(struct _foo__kargs){._has_name = 1, .name = value,
 *                                 ._has_other = 1, .other = val2})
 *
 * Also handles kw_func(target, name: value) which produces just the struct
 * initializer without the function call wrapper.
 *
 * Uses proper tree node operations - NO token replacement.
 */

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

#include "branch_symbols.h"
#include "transform.h"
#include "rewrite.h"
#include "types.h"
#include "nt_types.h"
#include "st.h"

// Maximum keyword arguments we support in a single call
#define MAX_KW_ARGS 32

// ---------------------------------------------------------------------------
// Collected keyword argument info
// ---------------------------------------------------------------------------

typedef struct {
    tnode_t    *name_node;  // The identifier node (keyword name)
    tnode_t    *value_node; // The assignment_expression node (value)
    const char *name;       // Extracted name string
} kw_arg_t;

typedef struct {
    kw_arg_t args[MAX_KW_ARGS];
    int      count;
} kw_arg_collector_t;

// ---------------------------------------------------------------------------
// Helper: Node Navigation
// ---------------------------------------------------------------------------

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
    new_child->parent        = parent;
}

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

    if (callee->nt_id == NT_primary_expression) {
        for (int i = 0; i < callee->num_kids; i++) {
            tnode_t *found = find_func_identifier(tnode_get_kid(callee, i));
            if (found) {
                return found;
            }
        }
    }

    if (callee->nt_id == NT_postfix_expression) {
        if (callee->num_kids > 0) {
            return find_func_identifier(tnode_get_kid(callee, 0));
        }
    }

    if (callee->num_kids > 0) {
        for (int i = 0; i < callee->num_kids; i++) {
            tnode_t *found = find_func_identifier(tnode_get_kid(callee, i));
            if (found) {
                return found;
            }
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
 * @brief Get identifier text from node's token.
 */
static const char *
get_id_text(ncc_buf_t *input, tnode_t *id_node)
{
    if (!id_node) {
        return nullptr;
    }
    if (id_node->tptr) {
        tok_t *tok = id_node->tptr;
        if (tok->replacement) {
            return tok->replacement->data;
        }
        return extract(input, tok);
    }
    // Check children for terminal
    for (int i = 0; i < id_node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(id_node, i);
        if (kid && kid->tptr) {
            tok_t *tok = kid->tptr;
            if (tok->replacement) {
                return tok->replacement->data;
            }
            return extract(input, tok);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Collecting keyword arguments
// ---------------------------------------------------------------------------

/**
 * @brief Recursively collect keyword_argument nodes from argument_expression_list.
 */
static void
collect_kw_args(ncc_buf_t *input, tnode_t *node, kw_arg_collector_t *collector)
{
    if (!node || collector->count >= MAX_KW_ARGS) {
        return;
    }

    if (node->nt_id == NT_keyword_argument) {
        // keyword_argument: '.' identifier '=' assignment_expression
        // kids[0] = '.', kids[1] = identifier, kids[2] = '=', kids[3] = assignment_expression
        if (node->num_kids >= 4) {
            kw_arg_t *arg   = &collector->args[collector->count++];
            arg->name_node  = tnode_get_kid(node, 1);
            arg->value_node = tnode_get_kid(node, 3);
            arg->name       = get_id_text(input, arg->name_node);
        }
        return;
    }

    if (node->nt_id == NT_argument_expression_list) {
        for (int i = 0; i < node->num_kids; i++) {
            collect_kw_args(input, tnode_get_kid(node, i), collector);
        }
    }
}

/**
 * @brief Check if argument_expression_list contains any keyword_argument nodes (iterative).
 */
static bool
has_kw_args(tnode_t *arg_list)
{
    if (!arg_list) {
        return false;
    }
    if (arg_list->nt_id == NT_keyword_argument) {
        return true;
    }

    int       cap = 16;
    int       top = 0;
    tnode_t **stk = base_alloc(cap * sizeof(tnode_t *));
    if (!stk) {
        return false;
    }

    stk[top++] = arg_list;

    while (top > 0) {
        tnode_t *n = stk[--top];
        for (int i = 0; i < n->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (!kid) {
                continue;
            }
            if (kid->nt_id == NT_keyword_argument) {
                base_dealloc(stk);
                return true;
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
 * @brief Count regular (non-keyword) arguments in an argument_expression_list.
 */
static int
count_regular_args(tnode_t *arg_list)
{
    if (!arg_list) {
        return 0;
    }

    if (arg_list->nt_id == NT_assignment_expression) {
        return 1;
    }

    if (arg_list->nt_id == NT_keyword_argument) {
        return 0;
    }

    if (arg_list->nt_id == NT_argument_expression_list) {
        int count = 0;
        for (int i = 0; i < arg_list->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(arg_list, i);
            if (!kid) {
                continue;
            }
            if (kid->nt_id == NT_assignment_expression) {
                count++;
            }
            else if (kid->nt_id == NT_argument_expression_list) {
                count += count_regular_args(kid);
            }
        }
        return count;
    }

    return 0;
}

/**
 * @brief Collect regular (non-keyword) arguments into a list.
 * @note list_append frees the old list and returns a new one,
 *       so we take a pointer-to-pointer to update the caller's list.
 */
static void
collect_regular_args(tnode_t *node, list_t **args_ptr)
{
    if (!node) {
        return;
    }

    if (node->nt_id == NT_assignment_expression) {
        *args_ptr = list_append(*args_ptr, node);
        return;
    }

    if (node->nt_id == NT_keyword_argument) {
        return;
    }

    if (node->nt_id == NT_argument_expression_list) {
        for (int i = 0; i < node->num_kids; i++) {
            collect_regular_args(tnode_get_kid(node, i), args_ptr);
        }
    }
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

    tnode_t *desig = synth_nonterminal("designator_1");
    desig->nt_id   = NT_designator;
    desig->branch  = 1;
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
 * @brief Build an initializer containing an integer constant.
 */
static tnode_t *
build_int_initializer(int value, int line)
{
    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%d", value);

    tnode_t *init = synth_nonterminal("initializer_1");
    init->nt_id   = NT_initializer;
    init->branch  = 1;

    tnode_t *const_node = synth_nonterminal("constant");
    const_node->nt_id   = NT_constant;
    add_child(const_node, synth_terminal(val_str, TT_NUM, line));

    tnode_t *prim = synth_nonterminal("primary_expression_1");
    prim->nt_id   = NT_primary_expression;
    prim->branch  = 1;
    add_child(prim, const_node);

    add_child(init, wrap_in_expr_hierarchy(prim, line));

    return init;
}

/**
 * @brief Build an initializer containing an expression (reparent).
 */
static tnode_t *
build_expr_initializer(tnode_t *expr, int line)
{
    (void)line;

    tnode_t *init = synth_nonterminal("initializer_1");
    init->nt_id   = NT_initializer;
    init->branch  = 1;
    add_child(init, expr);

    return init;
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

    tnode_t *unary = synth_nonterminal("unary_expression_2");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 2;
    add_child(unary, unary_op);
    add_child(unary, cast);

    return unary;
}

/**
 * @brief Build a compound literal for keyword arguments.
 * (struct _funcname__kargs){._has_name=1, .name=value, ...}
 */
static tnode_t *
build_kargs_compound_literal(const char *func_name, kw_arg_collector_t *kw_args, int line)
{
    char struct_name[256];
    snprintf(struct_name, sizeof(struct_name), "_%s__kargs", func_name);

    tnode_t *literal = synth_nonterminal("compound_literal_0");
    literal->nt_id   = NT_compound_literal;
    literal->branch  = 0;

    add_child(literal, synth_terminal("(", TT_PUNCT, line));

    // type_name: struct _funcname__kargs
    tnode_t *type_name = synth_nonterminal("type_name_0");
    type_name->nt_id   = NT_type_name;
    type_name->branch  = 0;

    tnode_t *spec_qual_list = synth_nonterminal("specifier_qualifier_list_1");
    spec_qual_list->nt_id   = NT_specifier_qualifier_list;
    spec_qual_list->branch  = 1;

    tnode_t *tsq = synth_nonterminal("type_specifier_qualifier_0");
    tsq->nt_id   = NT_type_specifier_qualifier;
    tsq->branch  = 0;

    tnode_t *type_spec = synth_nonterminal("type_specifier_3");
    type_spec->nt_id   = NT_type_specifier;
    type_spec->branch  = 3;

    // struct_or_union_specifier (reference, no body)
    tnode_t *struct_spec = synth_nonterminal("struct_or_union_specifier_2");
    struct_spec->nt_id   = NT_struct_or_union_specifier;
    struct_spec->branch  = 2;

    add_child(struct_spec, synth_terminal("struct", TT_KEYWORD, line));
    add_child(struct_spec, build_identifier(struct_name, line));

    add_child(type_spec, struct_spec);
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

    // Build initializer_list with ._has_name=1, .name=value pairs
    tnode_t *init_list = synth_nonterminal("initializer_list");
    init_list->nt_id   = NT_initializer_list;

    for (int i = 0; i < kw_args->count; i++) {
        kw_arg_t *arg = &kw_args->args[i];

        if (!arg->name) {
            continue;
        }

        // Add comma separator (except for first item)
        if (i > 0) {
            add_child(init_list, synth_terminal(",", TT_PUNCT, line));
        }

        // ._has_name = 1
        char has_name[256];
        snprintf(has_name, sizeof(has_name), "_has_%s", arg->name);
        add_child(init_list, build_designation(has_name, line));
        add_child(init_list, build_int_initializer(1, line));

        add_child(init_list, synth_terminal(",", TT_PUNCT, line));

        // .name = value (reparent the value expression)
        add_child(init_list, build_designation(arg->name, line));
        add_child(init_list, build_expr_initializer(arg->value_node, line));
    }

    add_child(braced, init_list);
    add_child(braced, synth_terminal("}", TT_PUNCT, line));

    add_child(literal, braced);

    return literal;
}

// ---------------------------------------------------------------------------
// Main Transform: xform_kw_call
// ---------------------------------------------------------------------------

/**
 * @brief Transform a function call with keyword arguments.
 * postfix_expression branch 1: postfix_expression '(' argument_expression_list? ')'
 */
static tnode_t *
xform_kw_call(xform_ctx_t *ctx, tnode_t *node)
{
    // Only handle function calls
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

    // Skip kw_func calls - handled by xform_kw_func
    if (strcmp(func_name, "kw_func") == 0) {
        return nullptr;
    }

    // Find the argument list
    tnode_t *arg_list     = nullptr;
    int      arg_list_idx = -1;
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->nt_id == NT_argument_expression_list) {
            arg_list     = kid;
            arg_list_idx = i;
            break;
        }
    }

    // Check if this function has a keywords block
    kw_info_t *kw_info = nullptr;
    if (ctx->symtab) {
        kw_info = st_get_kw_info(ctx->symtab, (char *)func_name);
    }

    bool call_has_kw_args = arg_list && has_kw_args(arg_list);

    // If function doesn't take kargs and call has no kw args, nothing to do
    if (!kw_info && !call_has_kw_args) {
        return nullptr;
    }

    int line = get_node_line(node);

    // If the function takes kargs but the call has no keyword arguments,
    // pass a zero-initialized kargs struct instead of nullptr so that
    // the function body can safely dereference kargs to read defaults.
    if (kw_info && !call_has_kw_args) {
        int num_call_args      = count_regular_args(arg_list);
        int expected_positional = kw_info->num_positional_params;

        // If the call already has all args (positional + kargs), don't add another
        if (num_call_args > expected_positional) {
            return nullptr;
        }

        // Build &(struct _funcname__kargs){} — zero-initialized compound literal
        kw_arg_collector_t empty_kw = {0};
        tnode_t *literal       = build_kargs_compound_literal(func_name, &empty_kw, line);
        tnode_t *addr_of       = build_address_of(literal, line);
        tnode_t *postfix       = synth_nonterminal("postfix_expression_9");
        postfix->nt_id         = NT_postfix_expression;
        postfix->branch        = 9;
        add_child(postfix, addr_of);
        tnode_t *kargs_assign  = wrap_in_expr_hierarchy(postfix, line);

        if (arg_list) {
            // Check if arg_list was flattened - emitter handles commas for flattened lists
            bool was_flattened = arg_list->origin != nullptr
                              && arg_list->origin->rewrite_name != nullptr
                              && strcmp(arg_list->origin->rewrite_name, "flatten") == 0;

            // Only add explicit comma if not flattened (emitter handles flattened lists)
            if (!was_flattened && arg_list->num_kids > 0) {
                add_child(arg_list, synth_terminal(",", TT_PUNCT, line));
            }
            add_child(arg_list, kargs_assign);
        }
        else {
            // Create new argument list with just the kargs struct
            tnode_t *new_arg_list = synth_nonterminal("argument_expression_list_1");
            new_arg_list->nt_id   = NT_argument_expression_list;
            new_arg_list->branch  = 1;
            add_child(new_arg_list, kargs_assign);

            // Insert before the closing paren
            for (int i = node->num_kids - 1; i >= 0; i--) {
                tnode_t *kid = tnode_get_kid(node, i);
                if (kid && kid->tptr && kid->nt && strcmp(kid->nt, ")") == 0) {
                    // Insert before closing paren
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

    // Collect keyword arguments
    kw_arg_collector_t kw_args = {0};
    collect_kw_args(ctx->input, arg_list, &kw_args);

    if (kw_args.count == 0) {
        return nullptr;
    }

    // Collect regular (non-keyword) arguments
    list_t *regular_args = nullptr;
    collect_regular_args(arg_list, &regular_args);
    int regular_count = list_len(regular_args);

    // Build the kargs compound literal with address-of
    tnode_t *literal       = build_kargs_compound_literal(func_name, &kw_args, line);
    tnode_t *addr_of       = build_address_of(literal, line);
    tnode_t *postfix       = synth_nonterminal("postfix_expression_9");
    postfix->nt_id         = NT_postfix_expression;
    postfix->branch        = 9;
    add_child(postfix, addr_of);
    tnode_t *kargs_assign  = wrap_in_expr_hierarchy(postfix, line);

    // Rebuild the argument list: regular args + kargs compound literal
    tnode_t *new_arg_list = synth_nonterminal("argument_expression_list");
    new_arg_list->nt_id   = NT_argument_expression_list;

    // Add regular arguments with commas
    for (int i = 0; i < regular_count; i++) {
        if (i > 0) {
            add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
        }
        tnode_t *arg = list_get(regular_args, i);
        add_child(new_arg_list, arg);
    }

    // Add comma and kargs struct
    if (regular_count > 0) {
        add_child(new_arg_list, synth_terminal(",", TT_PUNCT, line));
    }
    add_child(new_arg_list, kargs_assign);

    // Replace old arg_list with new one
    if (arg_list_idx >= 0) {
        replace_child_at(node, arg_list_idx, new_arg_list);
    }

    base_dealloc(regular_args);

    return nullptr;
}

// ---------------------------------------------------------------------------
// Main Transform: xform_kw_func
// ---------------------------------------------------------------------------

/**
 * @brief Check if a postfix_expression is a call to kw_func.
 */
static bool
is_kw_func_call(ncc_buf_t *input, tnode_t *node)
{
    if (!node || node->nt_id != NT_postfix_expression
        || node->branch != BRANCH(postfix_expression, CALL)) {
        return false;
    }

    tnode_t *callee  = tnode_get_kid(node, 0);
    tnode_t *func_id = find_func_identifier(callee);
    if (!func_id) {
        return false;
    }

    const char *name = get_func_name(input, func_id);
    if (!name) {
        return false;
    }

    return strcmp(name, "kw_func") == 0;
}

/**
 * @brief Find the first regular argument from an argument list.
 */
static tnode_t *
find_first_regular_arg(tnode_t *arg_list)
{
    if (!arg_list) {
        return nullptr;
    }

    if (arg_list->nt_id == NT_assignment_expression) {
        return arg_list;
    }

    if (arg_list->nt_id == NT_argument_expression_list) {
        for (int i = 0; i < arg_list->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(arg_list, i);
            if (!kid) {
                continue;
            }
            if (kid->nt_id == NT_assignment_expression) {
                return kid;
            }
            else if (kid->nt_id == NT_argument_expression_list) {
                tnode_t *found = find_first_regular_arg(kid);
                if (found) {
                    return found;
                }
            }
        }
    }

    return nullptr;
}

/**
 * @brief Transform kw_func(target, kw_args...) into struct initializer.
 *
 * Transforms:
 *   kw_func(target_func, name: value, other: val2)
 * Into:
 *   &(struct _target_func__kargs){._has_name = 1, .name = value, ...}
 *
 * This replaces the entire postfix_expression.
 */
static tnode_t *
xform_kw_func(xform_ctx_t *ctx, tnode_t *node)
{
    // Only handle function calls
    if (node->branch != BRANCH(postfix_expression, CALL)) {
        return nullptr;
    }

    // Check if this is a kw_func call
    if (!is_kw_func_call(ctx->input, node)) {
        return nullptr;
    }

    // Find the argument list
    tnode_t *arg_list = nullptr;
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->nt_id == NT_argument_expression_list) {
            arg_list = kid;
            break;
        }
    }

    if (!arg_list) {
        return nullptr;
    }

    // First argument should be the target function name (an identifier)
    tnode_t *first_arg = find_first_regular_arg(arg_list);
    if (!first_arg) {
        return nullptr;
    }

    // Extract the target function name from the first argument
    tnode_t *target_id = find_func_identifier(first_arg);
    if (!target_id) {
        return nullptr;
    }

    const char *target_name = get_func_name(ctx->input, target_id);
    if (!target_name) {
        return nullptr;
    }

    // Collect keyword arguments (skip the first regular arg)
    kw_arg_collector_t kw_args = {0};
    collect_kw_args(ctx->input, arg_list, &kw_args);

    if (kw_args.count == 0) {
        return nullptr;
    }

    int line = get_node_line(node);

    // Build the kargs compound literal with address-of
    tnode_t *literal = build_kargs_compound_literal(target_name, &kw_args, line);
    tnode_t *addr_of = build_address_of(literal, line);

    // Wrap in postfix_expression_9 (to match expression level)
    tnode_t *postfix = synth_nonterminal("postfix_expression_9");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = 9;
    add_child(postfix, addr_of);

    // Replace the entire kw_func(...) call with the struct expression
    // The return value replaces the node
    return postfix;
}

/**
 * @brief Register the keyword call transformation.
 */
void
register_kw_call_xform(xform_registry_t *reg)
{
    xform_register_post(reg, NT_postfix_expression, xform_kw_call, "kw_call");
    xform_register_post(reg, NT_postfix_expression, xform_kw_func, "kw_func");
}
