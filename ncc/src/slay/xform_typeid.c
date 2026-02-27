// xform_typeid.c — Transform: typeid(T) -> mangled identifier.
//
// Registered as post-order on "synthetic_identifier".
// typeid(int *) becomes __aB3cD5eF... (SHA256-based identifier).

#include "slay/xform_helpers.h"

#include <stdlib.h>

// ============================================================================
// Transform callback
// ============================================================================

static n00b_parse_tree_t *
xform_typeid(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *node)
{
    // synthetic_identifier alternatives:
    //   1: <_named_id_kw_typeid> "(" <typeid_atom> <typeid_continuation>? ")"
    //   2: "constexpr_paste" "(" <argument_expression_list> ")"
    //
    // For alt 1, child[0] is the <_named_id_kw_typeid> NT.
    // We need to find the "typeid" keyword leaf inside it.

    size_t nc = n00b_tree_num_children(node);
    if (nc < 4) {
        return NULL;
    }

    // Check that first child is the _named_id_kw_typeid NT containing "typeid".
    n00b_parse_tree_t *kw_nt = n00b_tree_child(node, 0);
    if (!kw_nt) {
        return NULL;
    }

    // The kw NT has one leaf child with the keyword text.
    const char *kw_text = NULL;
    if (n00b_tree_is_leaf(kw_nt)) {
        kw_text = n00b_xform_leaf_text(kw_nt);
    }
    else if (n00b_tree_num_children(kw_nt) > 0) {
        kw_text = n00b_xform_leaf_text(n00b_tree_child(kw_nt, 0));
    }

    if (!kw_text || strcmp(kw_text, "typeid") != 0) {
        return NULL;
    }

    // Find <typeid_atom> and optional <typeid_continuation> by name,
    // since group wrapper nodes may alter child indices.
    n00b_parse_tree_t *atom = n00b_xform_find_child_nt(node, "typeid_atom");
    n00b_parse_tree_t *cont = n00b_xform_find_child_nt(node,
                                                        "typeid_continuation");

    if (!atom) {
        return NULL;
    }

    char *type_str = n00b_xform_extract_type_string(ctx, atom, cont);
    char *mangled  = n00b_type_mangle(type_str);

    uint32_t line, col;
    n00b_xform_first_leaf_pos(node, &line, &col);

    n00b_parse_tree_t *replacement = n00b_xform_make_token_node(
        N00B_TOK_IDENTIFIER, mangled, line, col);

    free(type_str);
    free(mangled);
    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
n00b_register_typeid_xform(n00b_xform_registry_t *reg)
{
    n00b_xform_register(reg, "synthetic_identifier", xform_typeid,
                         "typeid");
}
