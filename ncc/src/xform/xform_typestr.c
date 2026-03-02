// xform_typestr.c — Transform: typestr(T) -> string literal.
//
// Registered as post-order on "synthetic_string_literal".
// typestr(int *) becomes "int *".

#include "xform/xform_helpers.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Transform callback
// ============================================================================

static n00b_parse_tree_t *
xform_typestr(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *node)
{
    // synthetic_string_literal alternatives:
    //   1: <_named_id_kw_typestr> "(" <typeid_atom> <typeid_continuation>? ")"
    //   2: <string_literal>
    //
    // For alt 2, we pass through (return NULL).
    // For alt 1, we transform.

    size_t nc = n00b_tree_num_children(node);
    if (nc < 4) {
        return NULL; // Probably alt 2 (string_literal).
    }

    // Check that first child contains the "typestr" keyword.
    n00b_parse_tree_t *kw_nt = n00b_tree_child(node, 0);
    if (!kw_nt) {
        return NULL;
    }

    const char *kw_text = NULL;
    if (n00b_tree_is_leaf(kw_nt)) {
        kw_text = n00b_xform_leaf_text(kw_nt);
    }
    else if (n00b_tree_num_children(kw_nt) > 0) {
        kw_text = n00b_xform_leaf_text(n00b_tree_child(kw_nt, 0));
    }

    if (!kw_text || strcmp(kw_text, "typestr") != 0) {
        return NULL;
    }

    n00b_parse_tree_t *atom = n00b_xform_find_child_nt(node, "typeid_atom");
    n00b_parse_tree_t *cont = n00b_xform_find_child_nt(node,
                                                        "typeid_continuation");
    if (!atom) {
        return NULL;
    }

    char *type_str = n00b_xform_extract_type_string(ctx, atom, cont);

    // Build quoted string: "\"type_str\""
    size_t len    = strlen(type_str);
    char  *quoted = malloc(len + 3);
    quoted[0]     = '"';
    memcpy(quoted + 1, type_str, len);
    quoted[len + 1] = '"';
    quoted[len + 2] = '\0';

    uint32_t line, col;
    n00b_xform_first_leaf_pos(node, &line, &col);

    n00b_parse_tree_t *replacement = n00b_xform_make_token_node(
        N00B_TOK_STRING_LIT, quoted, line, col);

    free(type_str);
    free(quoted);
    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
n00b_register_typestr_xform(n00b_xform_registry_t *reg)
{
    n00b_xform_register(reg, "synthetic_string_literal", xform_typestr,
                         "typestr");
}
