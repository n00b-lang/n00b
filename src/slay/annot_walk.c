// annot_walk.c - Post-parse DFS annotation walk (slim dispatcher).
//
// Walks the parse tree depth-first. For each non-terminal node, checks
// the grammar for annotations and dispatches to phase handlers:
//
//   Phase 1: scope open       (annot_scope.c)
//   Phase 2-3: symbol reg     (annot_symtab.c) — two-pass for ordering fix
//   Phase 4: CF labels        (annot_cf.c)
//   Phase 5: child recursion  (here)
//   Phase 6: post-order types (annot_types.c)
//   Phase 7: scope close      (annot_scope.c)

#include "internal/slay/annot_phases.h"
#include "slay/n00b_parse.h"

// ============================================================================
// DFS walk
// ============================================================================

static void
walk_node(n00b_annot_walk_ctx_t *ctx, n00b_parse_tree_t *node)
{
    if (!node) {
        return;
    }

    // Terminals (leaves) have no annotations.
    if (n00b_tree_is_leaf(node)) {
        return;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    // Empty-string nodes and negative IDs have no annotations.
    if (pn->id == N00B_EMPTY_STRING || pn->id < 0) {
        return;
    }

    // Group wrapper nodes are internal BNF artifacts (from ?, *, +
    // quantifiers). They carry no annotations — just recurse through them.
    if (pn->group_top) {
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            walk_node(ctx, n00b_tree_child(node, i));
        }

        return;
    }

    n00b_nonterm_t *nt = n00b_get_nonterm(ctx->grammar, pn->id);

    if (!nt) {
        // Unknown NT — just recurse.
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            walk_node(ctx, n00b_tree_child(node, i));
        }

        return;
    }

    // --- Populate per-node context ---

    n00b_parse_rule_t *rule = n00b_get_node_rule(ctx->grammar, pn);

    n00b_list_t(n00b_annotation_t *) *annot_list = NULL;

    if (rule && rule->annotations.data) {
        annot_list = &rule->annotations;
    }

    size_t annot_count = annot_list ? n00b_list_len(*annot_list) : 0;

    // Build a flat pointer array for the phase handlers so they don't
    // need to know about the n00b_list_t internals.
    n00b_annotation_t *annot_stack[annot_count > 0 ? annot_count : 1];

    for (size_t ai = 0; ai < annot_count; ai++) {
        annot_stack[ai] = n00b_list_get(*annot_list, ai);
    }

    annot_node_ctx_t nc_ctx = {
        .node         = node,
        .pn           = pn,
        .nt           = nt,
        .rule         = rule,
        .annots       = annot_stack,
        .annot_count  = annot_count,
        .last_sym     = NULL,
        .opened_scope = false,
        .scope_ns     = n00b_string_empty(),
    };

    // --- Phase 1: scope open ---
    annot_phase_scope_open(ctx, &nc_ctx);

    // --- Phases 2-3: two-pass symbol registration + type binding ---
    annot_phase_symtab(ctx, &nc_ctx);

    // --- Phase 4: CF labels ---
    annot_phase_cf(ctx, &nc_ctx);

    // --- Phase 5: recurse into children ---
    size_t num_children = n00b_tree_num_children(node);

    for (size_t i = 0; i < num_children; i++) {
        walk_node(ctx, n00b_tree_child(node, i));
    }

    // --- Phase 6: post-order types ---
    annot_phase_types_post(ctx, &nc_ctx);

    // --- Phase 7: scope close ---
    annot_phase_scope_close(ctx, &nc_ctx);
}

// ============================================================================
// Public API — full result (symtab + control flow labels)
// ============================================================================

n00b_annot_result_t *
n00b_annot_walk_tree_full(n00b_grammar_t *g, n00b_parse_tree_t *tree)
{
    if (!g || !tree) {
        return NULL;
    }

    n00b_cf_labels_t *labels = n00b_alloc(n00b_cf_labels_t);
    n00b_dict_init(labels, .hash = n00b_hash_word, .skip_obj_hash = true);

    n00b_list_t(n00b_sym_entry_t *) *params
        = n00b_alloc(n00b_list_t(n00b_sym_entry_t *));
    *params = n00b_list_new_private(n00b_sym_entry_t *);

    n00b_node_types_t *node_types = n00b_alloc(n00b_node_types_t);
    n00b_dict_init(node_types, .hash = n00b_hash_word, .skip_obj_hash = true);

    n00b_list_t(n00b_sym_entry_t *) *shadowed_entries
        = n00b_alloc(n00b_list_t(n00b_sym_entry_t *));
    *shadowed_entries = n00b_list_new_private(n00b_sym_entry_t *);

    n00b_annot_walk_ctx_t ctx = {
        .symtab           = n00b_symtab_new(),
        .grammar          = g,
        .cf_labels        = labels,
        .tc_ctx           = n00b_tc_ctx_new(),
        .params           = params,
        .node_types       = node_types,
        .shadowed_entries = shadowed_entries,
    };

    walk_node(&ctx, tree);

    n00b_annot_result_t *result = n00b_alloc(n00b_annot_result_t);
    result->symtab           = ctx.symtab;
    result->cf_labels        = ctx.cf_labels;
    result->tc_ctx           = ctx.tc_ctx;
    result->params           = ctx.params;
    result->node_types       = ctx.node_types;
    result->shadowed_entries = ctx.shadowed_entries;

    return result;
}

n00b_annot_result_t *
n00b_annot_walk_full(n00b_parse_result_t *result)
{
    if (!result || !n00b_parse_result_ok(result)) {
        return NULL;
    }

    n00b_grammar_t    *g    = n00b_parse_result_grammar(result);
    n00b_parse_tree_t *tree = n00b_parse_result_tree(result);

    return n00b_annot_walk_tree_full(g, tree);
}

// ============================================================================
// Public API — symtab only (convenience wrappers)
// ============================================================================

n00b_symtab_t *
n00b_annot_walk_tree(n00b_grammar_t *g, n00b_parse_tree_t *tree)
{
    n00b_annot_result_t *r = n00b_annot_walk_tree_full(g, tree);

    return r ? r->symtab : NULL;
}

n00b_symtab_t *
n00b_annot_walk(n00b_parse_result_t *result)
{
    n00b_annot_result_t *r = n00b_annot_walk_full(result);

    return r ? r->symtab : NULL;
}
