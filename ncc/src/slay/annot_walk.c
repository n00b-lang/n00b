// annot_walk.c - Post-parse reclassify walk
//
// Walks the parse tree in DFS post-order, firing @reclassify and
// @reclassify_list annotations to mutate IDENTIFIER tokens into
// new token types (e.g., TYPEDEF_NAME).
//
// Scope-aware: @scope annotations push/pop scope on the symbol
// table, and reclassification is limited to the enclosing scope's
// token range.

#include "slay/annot_walk.h"
#include "slay/symtab.h"
#include "internal/slay/grammar_internal.h"
#include "core/alloc.h"
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

// Find the IDENTIFIER token ID in the grammar's terminal map.
static int64_t
find_identifier_tid(ncc_grammar_t *g)
{
    ncc_string_t name = NCC_STRING_STATIC("IDENTIFIER");
    bool          found = false;
    void         *val   = _ncc_dict_get(g->terminal_map,
                                                  (void *)name.data, &found);
    if (found) {
        return (int64_t)(intptr_t)val;
    }

    return NCC_TOK_IDENTIFIER;
}

// Check if any leaf token in the subtree rooted at `node` has text
// matching `text`.
static bool
subtree_contains_token_text(ncc_parse_tree_t *node, ncc_string_t text)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);
            if (ncc_string_eq(val, text)) {
                return true;
            }
        }
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_contains_token_text(ncc_tree_child(node, i), text)) {
            return true;
        }
    }

    return false;
}

// Extract the first IDENTIFIER token from a subtree, optionally
// excluding a specific child index.
static ncc_token_info_t *
extract_first_identifier(ncc_parse_tree_t *node, int64_t id_tid,
                         int32_t exclude_child)
{
    if (!node) {
        return NULL;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && tok->tid == (int32_t)id_tid) {
            return tok;
        }
        return NULL;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if ((int32_t)i == exclude_child) {
            continue;
        }
        ncc_token_info_t *found
            = extract_first_identifier(ncc_tree_child(node, i), id_tid, -1);
        if (found) {
            return found;
        }
    }

    return NULL;
}

// Resolve a child reference against a parse tree node.
// Returns the child subtree, or NULL if unresolvable.
static ncc_parse_tree_t *
resolve_child(ncc_grammar_t *g __attribute__((unused)),
              ncc_parse_tree_t *node,
              ncc_child_ref_t ref)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return NULL;
    }

    size_t nc = ncc_tree_num_children(node);

    if (ref.kind == NCC_ROLE_BY_INDEX) {
        if (ref.index < 0 || (size_t)ref.index >= nc) {
            return NULL;
        }
        return ncc_tree_child(node, ref.index);
    }

    // By name: find a child NT whose name matches.
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (child && !ncc_tree_is_leaf(child)) {
            ncc_nt_node_t *pn = &ncc_tree_node_value(child);
            if (pn->name.data && ref.name.data
                && ncc_string_eq(pn->name, ref.name)) {
                return child;
            }
        }
    }

    return NULL;
}

// Get the child index that corresponds to a child ref (for exclusion).
static int32_t
resolve_child_index(ncc_grammar_t *g __attribute__((unused)),
                    ncc_parse_tree_t *node,
                    ncc_child_ref_t ref)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return -1;
    }

    if (ref.kind == NCC_ROLE_BY_INDEX) {
        return ref.index;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (child && !ncc_tree_is_leaf(child)) {
            ncc_nt_node_t *pn = &ncc_tree_node_value(child);
            if (pn->name.data && ref.name.data
                && ncc_string_eq(pn->name, ref.name)) {
                return (int32_t)i;
            }
        }
    }

    return -1;
}

// Mutate all IDENTIFIER tokens in [after_index+1, scope_end) whose
// text matches `name` to `new_tid`.  scope_end < 0 means no limit.
static int32_t
reclassify_tokens(ncc_token_info_t **tokens, int32_t ntokens,
                  int32_t after_index, int32_t scope_end,
                  ncc_string_t name,
                  int64_t id_tid, int64_t new_tid)
{
    int32_t count = 0;
    int32_t limit = (scope_end >= 0 && scope_end < ntokens)
                        ? scope_end
                        : ntokens;

    for (int32_t i = after_index + 1; i < limit; i++) {
        ncc_token_info_t *tok = tokens[i];

        if (!tok || tok->tid != (int32_t)id_tid) {
            continue;
        }

        if (!ncc_option_is_set(tok->value)) {
            continue;
        }

        ncc_string_t val = ncc_option_get(tok->value);
        if (ncc_string_eq(val, name)) {
            tok->tid = (int32_t)new_tid;
            count++;
        }
    }

    return count;
}

// ============================================================================
// Scope helpers
// ============================================================================

// Check if an NT has a @scope annotation in its pending_annotations.
static bool
nt_has_scope(ncc_nonterm_t *nt)
{
    if (!nt || !nt->pending_annotations.data) {
        return false;
    }

    size_t na = ncc_list_len(nt->pending_annotations);

    for (size_t i = 0; i < na; i++) {
        ncc_annotation_t *a = ncc_list_get(nt->pending_annotations, i);
        if (a && a->kind == NCC_ANNOT_SCOPE_OPEN) {
            return true;
        }
    }

    return false;
}

// Get the scope tag from the first @scope annotation on an NT.
static ncc_string_t
nt_scope_tag(ncc_nonterm_t *nt)
{
    if (!nt || !nt->pending_annotations.data) {
        return ncc_string_empty();
    }

    size_t na = ncc_list_len(nt->pending_annotations);

    for (size_t i = 0; i < na; i++) {
        ncc_annotation_t *a = ncc_list_get(nt->pending_annotations, i);
        if (a && a->kind == NCC_ANNOT_SCOPE_OPEN && a->scope_tag.data) {
            return a->scope_tag;
        }
    }

    return ncc_string_empty();
}

// ============================================================================
// Fire reclassify for one annotation on one node
// ============================================================================

// Add a typedef name to the symtab if provided.
static void
symtab_add_typedef(ncc_symtab_t *st, ncc_string_t name,
                   ncc_parse_tree_t *decl_node)
{
    if (!st) {
        return;
    }

    ncc_symtab_add(st, ncc_string_empty(), name,
                     NCC_SYM_TYPEDEF, decl_node);
}

static int32_t
fire_reclassify(ncc_grammar_t     *g,
                ncc_parse_tree_t  *node,
                ncc_annotation_t  *annot,
                ncc_token_info_t **tokens,
                int32_t             ntokens,
                int64_t             id_tid,
                int32_t             scope_end,
                ncc_symtab_t      *st)
{
    // Check guard: type_ref points to the guard child.
    ncc_parse_tree_t *guard = resolve_child(g, node, annot->type_ref);
    if (!guard) {
        return 0;
    }

    // Guard text must appear in the guard subtree.
    if (annot->scope_tag.data) {
        if (!subtree_contains_token_text(guard, annot->scope_tag)) {
            return 0;
        }
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(node);
    int32_t count = 0;

    if (annot->kind == NCC_ANNOT_RECLASSIFY) {
        // Extract the first IDENTIFIER from this node, excluding the
        // guard child.
        int32_t guard_ix = resolve_child_index(g, node, annot->type_ref);
        ncc_token_info_t *id_tok
            = extract_first_identifier(node, id_tid, guard_ix);

        if (id_tok && ncc_option_is_set(id_tok->value)) {
            ncc_string_t name = ncc_option_get(id_tok->value);
            count = reclassify_tokens(tokens, ntokens, pn->end,
                                      scope_end, name, id_tid,
                                      annot->reclassify_tid);
            if (count > 0) {
                symtab_add_typedef(st, name, node);
            }
        }
    }
    else {
        // RECLASSIFY_LIST: name_ref points to the list child.
        ncc_parse_tree_t *list = resolve_child(g, node, annot->name_ref);
        if (!list || ncc_tree_is_leaf(list)) {
            return 0;
        }

        // Each child of the list node may contain an IDENTIFIER to
        // reclassify.
        size_t lnc = ncc_tree_num_children(list);
        for (size_t i = 0; i < lnc; i++) {
            ncc_parse_tree_t *item = ncc_tree_child(list, i);
            ncc_token_info_t *id_tok
                = extract_first_identifier(item, id_tid, -1);

            if (id_tok && ncc_option_is_set(id_tok->value)) {
                ncc_string_t name = ncc_option_get(id_tok->value);
                int32_t rc = reclassify_tokens(tokens, ntokens, pn->end,
                                               scope_end, name, id_tid,
                                               annot->reclassify_tid);
                count += rc;
                if (rc > 0) {
                    symtab_add_typedef(st, name, node);
                }
            }
        }
    }

    return count;
}

// ============================================================================
// DFS walk
// ============================================================================

static int32_t
reclassify_walk_node(ncc_grammar_t     *g,
                     ncc_parse_tree_t  *node,
                     ncc_token_info_t **tokens,
                     int32_t             ntokens,
                     int64_t             id_tid,
                     int32_t             scope_end,
                     ncc_symtab_t      *st)
{
    if (!node) {
        return 0;
    }

    if (ncc_tree_is_leaf(node)) {
        return 0;
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(node);

    if (pn->group_top) {
        // Recurse through group nodes transparently.
        int32_t count = 0;
        size_t  nc    = ncc_tree_num_children(node);
        for (size_t i = 0; i < nc; i++) {
            count += reclassify_walk_node(g, ncc_tree_child(node, i),
                                          tokens, ntokens, id_tid,
                                          scope_end, st);
        }
        return count;
    }

    // Check for @scope annotation on this NT.
    ncc_nonterm_t *nt       = ncc_get_nonterm(g, pn->id);
    bool            is_scope = nt_has_scope(nt);

    // If this NT opens a scope, push scope on symtab and limit
    // reclassification to this NT's token range.
    int32_t child_scope_end = scope_end;

    if (is_scope) {
        if (st) {
            ncc_string_t tag = nt_scope_tag(nt);
            ncc_symtab_push_scope(st, ncc_string_empty(), tag);
        }
        child_scope_end = pn->end;
    }

    int32_t count = 0;

    // Post-order: recurse into children first.
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        count += reclassify_walk_node(g, ncc_tree_child(node, i),
                                      tokens, ntokens, id_tid,
                                      child_scope_end, st);
    }

    // Fire reclassify annotations on this node.
    if (nt && nt->has_reclassify) {
        ncc_parse_rule_t *rule = ncc_get_node_rule(g, pn);
        if (rule && rule->annotations.data) {
            size_t na = ncc_list_len(rule->annotations);
            for (size_t i = 0; i < na; i++) {
                ncc_annotation_t *annot = ncc_list_get(rule->annotations, i);

                if (annot->kind == NCC_ANNOT_RECLASSIFY
                    || annot->kind == NCC_ANNOT_RECLASSIFY_LIST) {
                    count += fire_reclassify(g, node, annot, tokens, ntokens,
                                             id_tid, child_scope_end, st);
                }
            }
        }
    }

    // Pop scope if we pushed one.
    if (is_scope && st) {
        ncc_symtab_pop_scope(st, ncc_string_empty());
    }

    return count;
}

// ============================================================================
// Public API
// ============================================================================

int32_t
ncc_annot_reclassify_walk(ncc_grammar_t    *g,
                           ncc_parse_tree_t *tree,
                           ncc_token_info_t **tokens,
                           int32_t            ntokens)
{
    return ncc_annot_reclassify_walk_with_symtab(g, tree, tokens,
                                                    ntokens, NULL);
}

int32_t
ncc_annot_reclassify_walk_with_symtab(ncc_grammar_t    *g,
                                        ncc_parse_tree_t *tree,
                                        ncc_token_info_t **tokens,
                                        int32_t            ntokens,
                                        ncc_symtab_t     *st)
{
    if (!g || !tree || !tokens || ntokens <= 0) {
        return 0;
    }

    int64_t id_tid = find_identifier_tid(g);
    return reclassify_walk_node(g, tree, tokens, ntokens, id_tid, -1, st);
}
