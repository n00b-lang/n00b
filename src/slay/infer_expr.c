/**
 * @file infer_expr.c
 * @brief Recursive-descent interpreter for `@infer` expression strings.
 *
 * A small (~300-line) interpreter that parses `@infer("...")` mini-language
 * expressions and executes them against the annotation walk context.
 *
 * Grammar (informal):
 * ```
 * infer_expr := type_expr
 * type_expr  := type_primary ("unify" type_primary | "|" type_primary)*
 * type_primary
 *     := "$" INT                        # child type ref
 *      | "$" INT "[" param_list "]"     # parameterized: name from child text
 *      | "$" "return"                   # current function's return type
 *      | "`" IDENT                      # fresh type variable
 *      | "lookup" "(" "$" INT ")"       # symtab lookup
 *      | "return_of" "(" "$" INT ")"    # callee return type
 *      | IDENT "[" param_list "]"       # parameterized type
 *      | IDENT                          # primitive
 *      | "(" type_expr ")"             # grouping
 * param_list := param_elem ("," param_elem)*
 * param_elem := "..." "$" INT           # spread: translate children of NT-child
 *             | type_expr
 * ```
 */

#include "slay/infer_expr.h"
#include "slay/annot_walk.h"
#include "slay/tree_util.h"
#include "slay/annotation.h"
#include "slay/cf_label.h"
#include "internal/slay/grammar_internal.h"
#include "core/alloc.h"
#include "adt/list.h"
#include "text/strings/string_ops.h"

#include "typecheck/types.h"
#include "typecheck/context.h"
#include "typecheck/unify.h"

// Forward-declare construct functions (same pattern as infer.c).
extern n00b_tc_type_t *n00b_tc_var(n00b_tc_ctx_t *ctx, n00b_string_t name);
extern n00b_tc_type_t *n00b_tc_fresh_var(n00b_tc_ctx_t *ctx);
extern n00b_tc_type_t *n00b_tc_prim(n00b_tc_ctx_t *ctx, n00b_string_t name);
extern void n00b_tc_ctx_register(n00b_tc_ctx_t *ctx, n00b_tc_type_t *type);


// ============================================================================
// Parser state
// ============================================================================

// Small name→type_var map for type variables within one @infer expression.
#define INFER_MAX_TVARS 16

typedef struct {
    n00b_string_t   name;
    n00b_tc_type_t *var;
} infer_tvar_entry_t;

typedef struct {
    n00b_tc_ctx_t              *tc_ctx;
    n00b_symtab_t              *symtab;
    n00b_grammar_t             *grammar;
    n00b_parse_tree_t          *node;
    n00b_node_types_t          *node_types;
    n00b_translate_type_spec_fn translate_type_spec;

    const char *src;    // Expression string bytes.
    int32_t     pos;    // Current position.
    int32_t     len;    // Total length.
    bool        error;  // Set on parse error.

    // Per-expression type variable map: same `x in one expression = same var.
    infer_tvar_entry_t tvars[INFER_MAX_TVARS];
    int32_t            tvar_count;
} infer_ctx_t;

// ============================================================================
// Lexer helpers
// ============================================================================

static void
skip_ws(infer_ctx_t *ctx)
{
    while (ctx->pos < ctx->len
           && (ctx->src[ctx->pos] == ' ' || ctx->src[ctx->pos] == '\t')) {
        ctx->pos++;
    }
}

static bool
at_end(infer_ctx_t *ctx)
{
    return ctx->pos >= ctx->len || ctx->error;
}

static bool
peek_char(infer_ctx_t *ctx, char c)
{
    skip_ws(ctx);
    return !at_end(ctx) && ctx->src[ctx->pos] == c;
}

static bool
match_char(infer_ctx_t *ctx, char c)
{
    skip_ws(ctx);

    if (!at_end(ctx) && ctx->src[ctx->pos] == c) {
        ctx->pos++;
        return true;
    }

    return false;
}

// Match a keyword (must be followed by non-alnum or end).
static bool
match_kw(infer_ctx_t *ctx, const char *kw)
{
    skip_ws(ctx);
    int32_t klen = (int32_t)strlen(kw);

    if (ctx->pos + klen > ctx->len) {
        return false;
    }

    if (memcmp(ctx->src + ctx->pos, kw, klen) != 0) {
        return false;
    }

    // Check word boundary.
    if (ctx->pos + klen < ctx->len) {
        char next = ctx->src[ctx->pos + klen];

        if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')
            || (next >= '0' && next <= '9') || next == '_') {
            return false;
        }
    }

    ctx->pos += klen;
    return true;
}

// Parse an identifier (alnum + _).
static n00b_string_t
parse_ident(infer_ctx_t *ctx)
{
    skip_ws(ctx);
    int32_t start = ctx->pos;

    while (ctx->pos < ctx->len) {
        char c = ctx->src[ctx->pos];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            ctx->pos++;
        }
        else {
            break;
        }
    }

    if (ctx->pos == start) {
        return n00b_string_empty();
    }

    return n00b_string_from_raw(ctx->src + start, ctx->pos - start);
}

// Parse a non-negative integer.
static int32_t
parse_int(infer_ctx_t *ctx)
{
    skip_ws(ctx);
    int32_t val = 0;
    bool    got = false;

    while (ctx->pos < ctx->len) {
        char c = ctx->src[ctx->pos];

        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            ctx->pos++;
            got = true;
        }
        else {
            break;
        }
    }

    if (!got) {
        ctx->error = true;
        return -1;
    }

    return val;
}

// ============================================================================
// Child type resolution
// ============================================================================

// Look up a child node's type in the node_types dict.
static n00b_tc_type_t *
get_child_type(infer_ctx_t *ctx, int32_t index)
{
    n00b_parse_tree_t *child = n00b_tree_get_nth_nt_child(ctx->node, index);

    if (!child || !ctx->node_types) {
        return NULL;
    }

    bool           found = false;
    uintptr_t      key   = (uintptr_t)child;
    n00b_tc_type_t *t     = n00b_dict_get(ctx->node_types, key, &found);

    return found ? t : NULL;
}

// ============================================================================
// Recursive-descent parser
// ============================================================================

static n00b_tc_type_t *parse_type_expr(infer_ctx_t *ctx);

static n00b_tc_type_t *
parse_type_primary(infer_ctx_t *ctx)
{
    if (at_end(ctx)) {
        ctx->error = true;
        return NULL;
    }

    skip_ws(ctx);

    // $return — current function's return type variable.
    // Looks up the "$return" symbol inserted by annot_walk.c when entering
    // a function scope.
    if (match_kw(ctx, "$return")) {
        if (ctx->symtab) {
            n00b_sym_entry_t *sym = n00b_symtab_lookup_any(
                ctx->symtab, n00b_string_empty(), *r"$return");

            if (sym && sym->type_var) {
                return sym->type_var;
            }
        }

        // Fallback: no enclosing function — return a fresh var.
        return n00b_tc_fresh_var(ctx->tc_ctx);
    }

    // $N — child type reference, or $N[...] — parameterized type with
    // dynamic constructor name extracted from child N's token text.
    //
    // Inside $N[...], a spread form ...$M translates each type-relevant
    // child of NT-child M (e.g., each <tspec-tvar> inside <type-params>)
    // and adds them all as type parameters.
    if (match_char(ctx, '$')) {
        int32_t index = parse_int(ctx);

        if (ctx->error) {
            return NULL;
        }

        // $N[type_list] — parameterized type with name from child N.
        if (match_char(ctx, '[')) {
            // Get the child node (any child, including terminals).
            n00b_parse_tree_t *name_child = NULL;

            if (ctx->node && index >= 0
                && (size_t)index < n00b_tree_num_children(ctx->node)) {
                name_child = n00b_tree_child(ctx->node, index);
            }

            n00b_string_t name = n00b_tree_extract_first_identifier(name_child);

            if (name.u8_bytes == 0) {
                ctx->error = true;
                return NULL;
            }

            // Collect type parameters.
            n00b_list_t(n00b_tc_type_t *) params
                = n00b_list_new_private(n00b_tc_type_t *);
            n00b_list_t(n00b_tc_type_t *) *params_ptr
                = n00b_alloc(n00b_list_t(n00b_tc_type_t *));
            *params_ptr = params;

            bool first = true;

            while (!at_end(ctx) && !peek_char(ctx, ']')) {
                if (!first && !match_char(ctx, ',')) {
                    ctx->error = true;
                    return NULL;
                }

                first = false;
                skip_ws(ctx);

                // ...$M — spread: translate children of NT-child M.
                if (ctx->pos + 3 <= ctx->len
                    && ctx->src[ctx->pos] == '.'
                    && ctx->src[ctx->pos + 1] == '.'
                    && ctx->src[ctx->pos + 2] == '.') {
                    ctx->pos += 3;

                    if (!match_char(ctx, '$')) {
                        ctx->error = true;
                        return NULL;
                    }

                    int32_t spread_ix = parse_int(ctx);

                    if (ctx->error) {
                        return NULL;
                    }

                    n00b_parse_tree_t *spread_node
                        = n00b_tree_get_nth_nt_child(ctx->node, spread_ix);

                    if (spread_node) {
                        size_t snc = n00b_tree_num_children(spread_node);

                        for (size_t si = 0; si < snc; si++) {
                            n00b_parse_tree_t *sc
                                = n00b_tree_child(spread_node, si);

                            if (n00b_tree_is_leaf(sc)) {
                                continue;
                            }

                            n00b_nt_node_t *spn = &n00b_tree_node_value(sc);

                            // Recurse into groups.
                            if (spn->group_top) {
                                size_t gnc = n00b_tree_num_children(sc);

                                for (size_t gi = 0; gi < gnc; gi++) {
                                    n00b_parse_tree_t *gc
                                        = n00b_tree_child(sc, gi);

                                    if (!n00b_tree_is_leaf(gc)
                                        && ctx->translate_type_spec) {
                                        n00b_tc_type_t *gt
                                            = ctx->translate_type_spec(
                                                ctx->tc_ctx,
                                                ctx->grammar,
                                                gc);

                                        if (gt) {
                                            n00b_list_push(*params_ptr, gt);
                                        }
                                    }
                                }

                                continue;
                            }

                            if (ctx->translate_type_spec) {
                                n00b_tc_type_t *st = ctx->translate_type_spec(
                                    ctx->tc_ctx, ctx->grammar, sc);

                                if (st) {
                                    n00b_list_push(*params_ptr, st);
                                }
                            }
                        }
                    }
                }
                else {
                    // Regular type expression.
                    n00b_tc_type_t *p = parse_type_expr(ctx);

                    if (p) {
                        n00b_list_push(*params_ptr, p);
                    }
                }
            }

            if (!match_char(ctx, ']')) {
                ctx->error = true;
                return NULL;
            }

            // Build parameterized type.
            n00b_tc_type_t *t = n00b_alloc(n00b_tc_type_t);
            t->forward = nullptr;
            n00b_tc_ctx_register(ctx->tc_ctx, t);

            n00b_tc_param_t param = {
                .name   = name,
                .params = params_ptr,
            };

            _n00b_variant_set_ptr(&t->kind, n00b_tc_param_t, param);
            return t;
        }

        n00b_tc_type_t *ct = get_child_type(ctx, index);

        if (!ct) {
            // Child has no type yet — return a fresh variable.
            ct = n00b_tc_fresh_var(ctx->tc_ctx);

            // Store it so future references get the same var.
            n00b_parse_tree_t *child = n00b_tree_get_nth_nt_child(ctx->node, index);

            if (child && ctx->node_types) {
                uintptr_t key = (uintptr_t)child;
                n00b_dict_put(ctx->node_types, key, ct);
            }
        }

        return ct;
    }

    // `x — type variable (same name within one expression = same var).
    if (match_char(ctx, '`')) {
        n00b_string_t name = parse_ident(ctx);

        if (name.u8_bytes == 0) {
            ctx->error = true;
            return NULL;
        }

        // Look up in per-expression map first.
        for (int32_t i = 0; i < ctx->tvar_count; i++) {
            if (n00b_unicode_str_eq(ctx->tvars[i].name, name)) {
                return ctx->tvars[i].var;
            }
        }

        // Create a new var and cache it.
        n00b_tc_type_t *tv = n00b_tc_var(ctx->tc_ctx, name);

        if (ctx->tvar_count < INFER_MAX_TVARS) {
            ctx->tvars[ctx->tvar_count].name = name;
            ctx->tvars[ctx->tvar_count].var  = tv;
            ctx->tvar_count++;
        }

        return tv;
    }

    // ( expr ) — grouping.
    if (match_char(ctx, '(')) {
        n00b_tc_type_t *t = parse_type_expr(ctx);

        if (!match_char(ctx, ')')) {
            ctx->error = true;
        }

        return t;
    }

    // lookup($N) — symtab lookup.
    if (match_kw(ctx, "lookup")) {
        if (!match_char(ctx, '(') || !match_char(ctx, '$')) {
            ctx->error = true;
            return NULL;
        }

        int32_t index = parse_int(ctx);

        if (ctx->error || !match_char(ctx, ')')) {
            ctx->error = true;
            return NULL;
        }

        // Get the child node, extract its identifier text, look up in symtab.
        n00b_parse_tree_t *child = n00b_tree_get_nth_nt_child(ctx->node, index);

        if (!child) {
            // Try treating it as a direct child index (including terminals).
            if (index >= 0
                && (size_t)index < n00b_tree_num_children(ctx->node)) {
                child = n00b_tree_child(ctx->node, index);
            }
        }

        n00b_string_t ident = n00b_tree_extract_first_identifier(child);

        if (ident.u8_bytes == 0) {
            return n00b_tc_fresh_var(ctx->tc_ctx);
        }

        n00b_sym_entry_t *sym = n00b_symtab_lookup_any(ctx->symtab,
                                                       n00b_string_empty(),
                                                       ident);

        if (sym && sym->type_var) {
            return sym->type_var;
        }

        return n00b_tc_fresh_var(ctx->tc_ctx);
    }

    // return_of($N) — return type of a function-typed child.
    if (match_kw(ctx, "return_of")) {
        if (!match_char(ctx, '(') || !match_char(ctx, '$')) {
            ctx->error = true;
            return NULL;
        }

        int32_t index = parse_int(ctx);

        if (ctx->error || !match_char(ctx, ')')) {
            ctx->error = true;
            return NULL;
        }

        n00b_tc_type_t *callee_type = get_child_type(ctx, index);

        if (!callee_type) {
            return n00b_tc_fresh_var(ctx->tc_ctx);
        }

        // Follow union-find to canonical type.
        n00b_tc_type_t *resolved = callee_type;

        while (resolved->forward) {
            resolved = resolved->forward;
        }

        // If it's a Fn type, extract the return_type.
        if (n00b_variant_is_type(resolved->kind, n00b_tc_fn_t)) {
            auto fn = n00b_variant_get(resolved->kind, n00b_tc_fn_t);

            if (fn.return_type) {
                return fn.return_type;
            }
        }

        // Not a function type (yet?) — return a fresh variable.
        return n00b_tc_fresh_var(ctx->tc_ctx);
    }

    // IDENT — could be primitive or parameterized (IDENT[...]).
    n00b_string_t name = parse_ident(ctx);

    if (name.u8_bytes == 0) {
        ctx->error = true;
        return NULL;
    }

    // Check for parameterized: name[type_list].
    if (match_char(ctx, '[')) {
        // Collect type parameters.
        n00b_list_t(n00b_tc_type_t *) params
            = n00b_list_new_private(n00b_tc_type_t *);
        n00b_list_t(n00b_tc_type_t *) *params_ptr
            = n00b_alloc(n00b_list_t(n00b_tc_type_t *));
        *params_ptr = params;

        n00b_tc_type_t *first = parse_type_expr(ctx);

        if (first) {
            n00b_list_push(*params_ptr, first);
        }

        while (match_char(ctx, ',')) {
            n00b_tc_type_t *p = parse_type_expr(ctx);

            if (p) {
                n00b_list_push(*params_ptr, p);
            }
        }

        if (!match_char(ctx, ']')) {
            ctx->error = true;
            return NULL;
        }

        // Build parameterized type manually.
        n00b_tc_type_t *t = n00b_alloc(n00b_tc_type_t);
        t->forward = nullptr;
        n00b_tc_ctx_register(ctx->tc_ctx, t);

        n00b_tc_param_t param = {
            .name   = name,
            .params = params_ptr,
        };

        _n00b_variant_set_ptr(&t->kind, n00b_tc_param_t, param);
        return t;
    }

    // Bare identifier — look up as primitive.
    n00b_tc_type_t *prim = n00b_tc_lookup_prim(ctx->tc_ctx, name);

    return prim ? prim : n00b_tc_prim(ctx->tc_ctx, name);
}

static n00b_tc_type_t *
parse_type_expr(infer_ctx_t *ctx)
{
    n00b_tc_type_t *lhs = parse_type_primary(ctx);

    if (!lhs || ctx->error) {
        return lhs;
    }

    while (!at_end(ctx)) {
        skip_ws(ctx);

        // "unify" infix operator.
        if (match_kw(ctx, "unify")) {
            n00b_tc_type_t *rhs = parse_type_primary(ctx);

            if (!rhs) {
                ctx->error = true;
                return lhs;
            }

            n00b_tc_unify(ctx->tc_ctx, lhs, rhs);
            // After unification, lhs and rhs point to the same canonical type.
            continue;
        }

        // "|" infix operator — sum type.
        if (match_char(ctx, '|')) {
            n00b_tc_type_t *rhs = parse_type_primary(ctx);

            if (!rhs) {
                ctx->error = true;
                return lhs;
            }

            // Build a sum type with lhs and rhs.
            n00b_list_t(n00b_tc_type_t *) variants
                = n00b_list_new_private(n00b_tc_type_t *);
            n00b_list_t(n00b_tc_type_t *) *var_ptr
                = n00b_alloc(n00b_list_t(n00b_tc_type_t *));
            *var_ptr = variants;

            n00b_list_push(*var_ptr, lhs);
            n00b_list_push(*var_ptr, rhs);

            n00b_tc_type_t *t = n00b_alloc(n00b_tc_type_t);
            t->forward = nullptr;
            n00b_tc_ctx_register(ctx->tc_ctx, t);

            n00b_tc_sum_t sum = {.variants = var_ptr};
            _n00b_variant_set_ptr(&t->kind, n00b_tc_sum_t, sum);

            lhs = t;
            continue;
        }

        // No more infix operators.
        break;
    }

    return lhs;
}

// ============================================================================
// Public API
// ============================================================================

n00b_tc_type_t *
n00b_infer_eval_ex(n00b_tc_ctx_t              *tc_ctx,
                   n00b_symtab_t              *symtab,
                   n00b_grammar_t             *grammar,
                   n00b_parse_tree_t          *node,
                   n00b_node_types_t          *node_types,
                   n00b_translate_type_spec_fn ts_fn,
                   n00b_string_t               expr)
{
    if (!tc_ctx || expr.u8_bytes == 0) {
        return NULL;
    }

    infer_ctx_t ctx = {
        .tc_ctx              = tc_ctx,
        .symtab              = symtab,
        .grammar             = grammar,
        .node                = node,
        .node_types          = node_types,
        .translate_type_spec = ts_fn,
        .src                 = expr.data,
        .pos                 = 0,
        .len                 = (int32_t)expr.u8_bytes,
        .error               = false,
    };

    n00b_tc_type_t *result = parse_type_expr(&ctx);

    if (ctx.error) {
        return NULL;
    }

    return result;
}

n00b_tc_type_t *
n00b_infer_eval(n00b_tc_ctx_t     *tc_ctx,
                n00b_symtab_t     *symtab,
                n00b_grammar_t    *grammar,
                n00b_parse_tree_t *node,
                n00b_node_types_t *node_types,
                n00b_string_t      expr)
{
    return n00b_infer_eval_ex(tc_ctx, symtab, grammar, node, node_types,
                              NULL, expr);
}
