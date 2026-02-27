/**
 * @file infer.c
 * @brief Type-spec translator: slay parse tree → typecheck types.
 *
 * Walks `<type-spec>` parse subtrees and produces `n00b_tc_type_t *` nodes
 * by calling the constructors in typecheck/construct.h.
 */

#include "slay/infer.h"
#include "slay/annotation.h"
#include "internal/slay/grammar_internal.h"
#include "core/alloc.h"
#include "core/list.h"
#include "core/vargs.h"
#include "strings/string_ops.h"

// Now that n00b_option_decl(n00b_string_t) is centralized in core/string.h,
// we can include the typecheck headers directly without collision.
#include "typecheck/types.h"
#include "typecheck/context.h"

// Forward-declare construct functions (can't include construct.h because
// it uses typed varargs which produce kargs structs we don't need here).
extern n00b_tc_type_t *n00b_tc_var(n00b_tc_ctx_t *ctx, n00b_string_t name);
extern n00b_tc_type_t *n00b_tc_fresh_var(n00b_tc_ctx_t *ctx);
extern n00b_tc_type_t *n00b_tc_prim(n00b_tc_ctx_t *ctx, n00b_string_t name);

// Build param types manually (can't use the typed vargs constructors from here).
extern void n00b_tc_ctx_register(n00b_tc_ctx_t *ctx, n00b_tc_type_t *type);

// ============================================================================
// Helpers
// ============================================================================

// Extract the text of the leftmost terminal in a subtree.
// Identical to the one in annot_walk.c but duplicated here to keep
// infer.c self-contained (avoids coupling to annot_walk internals).
static n00b_string_t
extract_first_identifier(n00b_parse_tree_t *node)
{
    if (!node) {
        return n00b_string_empty();
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t val = n00b_option_get(tok->value);

            if (val.u8_bytes > 0) {
                return val;
            }
        }

        return n00b_string_empty();
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_string_t s = extract_first_identifier(n00b_tree_child(node, i));

        if (s.u8_bytes > 0) {
            return s;
        }
    }

    return n00b_string_empty();
}

// Check if a non-terminal node's name matches a string.
static bool
nt_name_is(n00b_grammar_t *g, n00b_parse_tree_t *node, n00b_string_t name)
{
    if (!node || n00b_tree_is_leaf(node)) {
        return false;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    if (pn->id < 0) {
        return false;
    }

    n00b_nonterm_t *nt = n00b_get_nonterm(g, pn->id);

    return nt && n00b_unicode_str_eq(nt->name, name);
}

// Find the first child whose NT name matches.
static n00b_parse_tree_t *
find_child_nt(n00b_grammar_t *g, n00b_parse_tree_t *parent, n00b_string_t name)
{
    size_t nc = n00b_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(parent, i);

        if (nt_name_is(g, child, name)) {
            return child;
        }

        // Recurse through group nodes.
        if (!n00b_tree_is_leaf(child)) {
            n00b_nt_node_t *cpn = &n00b_tree_node_value(child);

            if (cpn->group_top) {
                n00b_parse_tree_t *found = find_child_nt(g, child, name);

                if (found) {
                    return found;
                }
            }
        }
    }

    return NULL;
}

// Forward declaration for recursive translation.
static n00b_tc_type_t *translate(n00b_tc_ctx_t *ctx, n00b_grammar_t *g,
                                   n00b_parse_tree_t *node);

// ============================================================================
// Collect type list from <tspec-list>
// ============================================================================

// Collects types from a <tspec-list> node into a pre-allocated list.
static void
collect_tspec_list(n00b_tc_ctx_t *ctx, n00b_grammar_t *g,
                   n00b_parse_tree_t *node,
                   n00b_list_t(n00b_tc_type_t *) *out)
{
    if (!node) {
        return;
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_tree_is_leaf(child)) {
            // Skip punctuation tokens (commas, brackets).
            continue;
        }

        n00b_nt_node_t *cpn = &n00b_tree_node_value(child);

        if (cpn->group_top) {
            // Recurse into groups.
            collect_tspec_list(ctx, g, child, out);
            continue;
        }

        if (cpn->id >= 0) {
            n00b_nonterm_t *nt = n00b_get_nonterm(g, cpn->id);

            if (nt && (n00b_unicode_str_eq(nt->name, *r"one-tspec")
                    || n00b_unicode_str_eq(nt->name, *r"tspec-list"))) {
                if (n00b_unicode_str_eq(nt->name, *r"tspec-list")) {
                    collect_tspec_list(ctx, g, child, out);
                } else {
                    n00b_tc_type_t *t = translate(ctx, g, child);

                    if (t) {
                        n00b_list_push(*out, t);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Core translation
// ============================================================================

static n00b_tc_type_t *
translate(n00b_tc_ctx_t *ctx, n00b_grammar_t *g, n00b_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    // Leaf (terminal) — must be a bare IDENTIFIER used as a type name.
    if (n00b_tree_is_leaf(node)) {
        n00b_string_t name = extract_first_identifier(node);

        if (name.u8_bytes == 0) {
            return nullptr;
        }

        // Try built-in first, fall back to creating a new prim.
        n00b_tc_type_t *prim = n00b_tc_lookup_prim(ctx, name);

        return prim ? prim : n00b_tc_prim(ctx, name);
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Group nodes: recurse into first child.
    if (pn->group_top || pn->id < 0 || pn->id == N00B_EMPTY_STRING) {
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_tc_type_t *t = translate(ctx, g, n00b_tree_child(node, i));

            if (t) {
                return t;
            }
        }

        return nullptr;
    }

    n00b_nonterm_t *nt = n00b_get_nonterm(g, pn->id);

    if (!nt) {
        return nullptr;
    }

    // <type-spec> → <one-tspec> — just recurse.
    if (n00b_unicode_str_eq(nt->name, *r"type-spec")) {
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_tc_type_t *t = translate(ctx, g, n00b_tree_child(node, i));

            if (t) {
                return t;
            }
        }

        return nullptr;
    }

    // <one-tspec> — dispatch to the child.
    if (n00b_unicode_str_eq(nt->name, *r"one-tspec")) {
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_tc_type_t *t = translate(ctx, g, n00b_tree_child(node, i));

            if (t) {
                return t;
            }
        }

        return nullptr;
    }

    // <tspec-tvar> — ` IDENTIFIER → type variable.
    if (n00b_unicode_str_eq(nt->name, *r"tspec-tvar")) {
        // Children: %"`" %IDENTIFIER
        n00b_string_t name = n00b_string_empty();
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *child = n00b_tree_child(node, i);

            if (n00b_tree_is_leaf(child)) {
                n00b_string_t s = extract_first_identifier(child);

                if (s.u8_bytes > 0 && !n00b_unicode_str_eq(s, *r"`")) {
                    name = s;
                    break;
                }
            }
        }

        if (name.u8_bytes > 0) {
            return n00b_tc_var(ctx, name);
        }

        return n00b_tc_fresh_var(ctx);
    }

    // <tspec-parameterized> — name [ tspec-list ] or ref [ tspec-list ]
    if (n00b_unicode_str_eq(nt->name, *r"tspec-parameterized")) {
        // Get the constructor name from the first identifier.
        n00b_string_t name = n00b_string_empty();
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_parse_tree_t *child = n00b_tree_child(node, i);

            if (n00b_tree_is_leaf(child)) {
                n00b_string_t s = extract_first_identifier(child);

                // Skip punctuation.
                if (s.u8_bytes > 0
                    && !n00b_unicode_str_eq(s, *r"[")
                    && !n00b_unicode_str_eq(s, *r"]")) {
                    if (name.u8_bytes == 0) {
                        name = s;
                    }
                }
            } else if (nt_name_is(g, child, *r"member-chain")) {
                name = extract_first_identifier(child);
            }
        }

        if (name.u8_bytes == 0) {
            return nullptr;
        }

        // Find the <tspec-list> child.
        n00b_parse_tree_t *tspec_list = find_child_nt(g, node, *r"tspec-list");

        if (!tspec_list) {
            // No parameters: bare identifier (not really parameterized).
            n00b_tc_type_t *prim = n00b_tc_lookup_prim(ctx, name);

            return prim ? prim : n00b_tc_prim(ctx, name);
        }

        // Collect parameters.
        n00b_list_t(n00b_tc_type_t *) params = n00b_list_new_private(n00b_tc_type_t *);
        n00b_list_t(n00b_tc_type_t *) *params_ptr =
            n00b_alloc(n00b_list_t(n00b_tc_type_t *));
        *params_ptr = params;

        collect_tspec_list(ctx, g, tspec_list, params_ptr);

        // Build the Param type manually (can't use typed vargs from here).
        n00b_tc_type_t *t = n00b_alloc(n00b_tc_type_t);
        t->forward = nullptr;
        n00b_tc_ctx_register(ctx, t);

        // We need to include the variant payload types.
        // Use the _n00b_variant_set_ptr macro directly.
        n00b_tc_param_t param = {
            .name   = name,
            .params = params_ptr,
        };

        _n00b_variant_set_ptr(&t->kind, n00b_tc_param_t, param);
        return t;
    }

    // <tspec-func> — ( params ) -> ret
    if (n00b_unicode_str_eq(nt->name, *r"tspec-func")) {
        // Collect positional params from <tspec-func-params>.
        n00b_list_t(n00b_tc_type_t *) positional = n00b_list_new_private(n00b_tc_type_t *);
        n00b_list_t(n00b_tc_type_t *) *pos_ptr =
            n00b_alloc(n00b_list_t(n00b_tc_type_t *));
        *pos_ptr = positional;

        n00b_parse_tree_t *func_params = find_child_nt(g, node, *r"tspec-func-params");

        if (func_params) {
            collect_tspec_list(ctx, g, func_params, pos_ptr);
        }

        // Get return type from <opt-return-type> or <return-type>.
        n00b_tc_type_t *ret_type = nullptr;
        n00b_parse_tree_t *ret_node = find_child_nt(g, node, *r"opt-return-type");

        if (!ret_node) {
            ret_node = find_child_nt(g, node, *r"return-type");
        }

        if (ret_node) {
            n00b_parse_tree_t *type_spec = find_child_nt(g, ret_node, *r"type-spec");

            if (!type_spec) {
                type_spec = find_child_nt(g, ret_node, *r"one-tspec");
            }

            if (type_spec) {
                ret_type = translate(ctx, g, type_spec);
            }
        }

        // Build function type manually.
        n00b_tc_type_t *t = n00b_alloc(n00b_tc_type_t);
        t->forward = nullptr;
        n00b_tc_ctx_register(ctx, t);

        n00b_tc_fn_t fn = {
            .positional  = pos_ptr,
            .vargs_type  = nullptr,
            .kargs_type  = nullptr,
            .return_type = ret_type,
        };

        _n00b_variant_set_ptr(&t->kind, n00b_tc_fn_t, fn);
        return t;
    }

    // <member-chain> — resolve to a type name.
    if (n00b_unicode_str_eq(nt->name, *r"member-chain")) {
        n00b_string_t name = extract_first_identifier(node);

        if (name.u8_bytes > 0) {
            n00b_tc_type_t *prim = n00b_tc_lookup_prim(ctx, name);

            return prim ? prim : n00b_tc_prim(ctx, name);
        }

        return nullptr;
    }

    // Fallback: try recursing into children.
    {
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            n00b_tc_type_t *t = translate(ctx, g, n00b_tree_child(node, i));

            if (t) {
                return t;
            }
        }
    }

    return nullptr;
}

// ============================================================================
// Public API
// ============================================================================

n00b_tc_type_t *
n00b_tc_translate_type_spec(n00b_tc_ctx_t     *ctx,
                              n00b_grammar_t    *g,
                              n00b_parse_tree_t *type_node)
{
    if (!ctx || !g || !type_node) {
        return nullptr;
    }

    return translate(ctx, g, type_node);
}
