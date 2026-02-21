/**
 * @file xform_helpers.c
 * @brief Shared AST construction and navigation utilities for transforms.
 *
 * Consolidates identical helpers that were duplicated across up to six
 * xform_*.c files: child manipulation, identifier lookup, and the
 * expression-precedence hierarchy builder.
 */

#include <string.h>
#include "base_alloc_shim.h"
#include "ncc_limits.h"

#include "branch_symbols.h"
#include "emit.h"
#include "lex.h"
#include "token.h"
#include "transform.h"
#include "xform_helpers.h"

// ====================================================================
// Child manipulation
// ====================================================================

int
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

void
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

void
insert_child_at(tnode_t *parent, int idx, tnode_t *child)
{
    if (!parent || !child || idx < 0 || idx > parent->num_kids) {
        return;
    }

    // Grow the kids list if at capacity (or if it doesn't exist yet).
    if (!parent->kids || parent->num_kids >= parent->kids->nitems) {
        int         new_cap  = parent->num_kids + 4;
        ncc_list_t *new_kids = ncc_list_alloc(new_cap);
        if (parent->kids) {
            for (int i = 0; i < parent->num_kids; i++) {
                new_kids->items[i] = parent->kids->items[i];
            }
            base_dealloc(parent->kids);
        }
        parent->kids = new_kids;
    }

    for (int i = parent->num_kids; i > idx; i--) {
        parent->kids->items[i] = parent->kids->items[i - 1];
    }
    parent->kids->items[idx] = child;
    parent->num_kids++;
    child->parent = parent;
}

void
replace_child_at(tnode_t *parent, int idx, tnode_t *new_child)
{
    if (!parent || !parent->kids || !new_child || idx < 0
        || idx >= parent->num_kids) {
        return;
    }
    parent->kids->items[idx] = new_child;
    new_child->parent        = parent;
}

// ====================================================================
// Node search
// ====================================================================

tnode_t *
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

const char *
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

tnode_t *
find_node_by_type(tnode_t *node, nt_type_t nt_id)
{
    if (!node) {
        return nullptr;
    }
    if (node->nt_id == nt_id) {
        return node;
    }

    int       cap = NCC_CAP_MEDIUM;
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

// ====================================================================
// AST construction
// ====================================================================

tnode_t *
build_identifier(const char *name, int line)
{
    tnode_t *id = synth_nonterminal("identifier");
    id->nt_id   = NT_identifier;
    add_child(id, synth_terminal(name, TT_ID, line));
    return id;
}

tnode_t *
build_primary_id(const char *name, int line)
{
    tnode_t *primary = synth_nonterminal("primary_expression_7");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 7;
    add_child(primary, build_identifier(name, line));
    return primary;
}

tnode_t *
wrap_in_expr_hierarchy(tnode_t *inner, int line)
{
    (void)line;

    // If caller passes a primary_expression, wrap it in postfix first.
    tnode_t *postfix = inner;
    if (inner->nt_id == NT_primary_expression) {
        postfix          = synth_nonterminal("postfix_expression_9");
        postfix->nt_id   = NT_postfix_expression;
        postfix->branch  = 9;
        add_child(postfix, inner);
    }

    tnode_t *unary = synth_nonterminal("unary_expression_9");
    unary->nt_id   = NT_unary_expression;
    unary->branch  = 9;
    add_child(unary, postfix);

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

// ====================================================================
// Emit / callee / numeric helpers (moved from xform_constexpr.c)
// ====================================================================

tok_t *
find_identifier_tok(tnode_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (node->tptr) {
        return node->tptr;
    }
    if (node->nt_id == NT_postfix_expression
        && node->branch != BRANCH(postfix_expression, PRIMARY)) {
        return nullptr;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tok_t *tok = find_identifier_tok(tnode_get_kid(node, i));
        if (tok) {
            return tok;
        }
    }
    return nullptr;
}

char *
get_callee_name(tree_xform_t *ctx, tnode_t *node)
{
    tnode_t *callee = tnode_get_kid(node, 0);
    if (!callee) {
        return nullptr;
    }

    tok_t *tok = find_identifier_tok(callee);
    if (!tok) {
        return nullptr;
    }

    return extract(ctx->input, tok);
}

char *
emit_node_to_string(tree_xform_t *ctx, tnode_t *node)
{
    char  *output = nullptr;
    size_t size;
    FILE  *f = open_memstream(&output, &size);

    emit_ctx_t ectx;
    emit_init(&ectx, ctx->lex, f);
    emit_tree(&ectx, node);
    emit_finish(&ectx);

    fclose(f);
    return output;
}

char *
strip_line_directives(const char *src)
{
    char  *out = nullptr;
    size_t size;
    FILE  *f = open_memstream(&out, &size);

    const char *p = src;
    while (*p) {
        if (*p == '#') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') {
                q++;
            }
            bool is_line_directive = false;
            if (*q >= '0' && *q <= '9') {
                is_line_directive = true;
            }
            else if (strncmp(q, "line", 4) == 0
                     && (q[4] == ' ' || q[4] == '\t')) {
                is_line_directive = true;
            }

            if (is_line_directive) {
                while (*p && *p != '\n') {
                    p++;
                }
                if (*p == '\n') {
                    p++;
                }
                continue;
            }
        }

        while (*p && *p != '\n') {
            fputc(*p, f);
            p++;
        }
        if (*p == '\n') {
            fputc('\n', f);
            p++;
        }
    }

    fclose(f);
    return out;
}

tnode_t *
build_numeric_literal(const char *value_str, int line)
{
    tnode_t *const_node = synth_nonterminal("constant");
    const_node->nt_id   = NT_constant;
    add_child(const_node, synth_terminal(value_str, TT_NUM, line));

    tnode_t *primary = synth_nonterminal("primary_expression_1");
    primary->nt_id   = NT_primary_expression;
    primary->branch  = 1;
    add_child(primary, const_node);

    tnode_t *postfix = synth_nonterminal("postfix_expression_9");
    postfix->nt_id   = NT_postfix_expression;
    postfix->branch  = BRANCH(postfix_expression, PRIMARY);
    add_child(postfix, primary);

    return postfix;
}
