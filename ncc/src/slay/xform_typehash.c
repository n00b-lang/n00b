// xform_typehash.c — Transform: typehash(T) -> numeric literal.
//
// Registered as post-order on "primary_expression".
// typehash(int *) becomes 12345678901234ULL.

#include "slay/xform_helpers.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Transform callback
// ============================================================================

static n00b_parse_tree_t *
xform_typehash(n00b_xform_ctx_t *ctx, n00b_parse_tree_t *node)
{
    // primary_expression has many alternatives. The typehash one is:
    //   "typehash" "(" <typeid_atom> <typeid_continuation>? ")"
    //
    // Check that first child is the "typehash" keyword token.

    size_t nc = n00b_tree_num_children(node);
    if (nc < 4) {
        return NULL;
    }

    n00b_parse_tree_t *first = n00b_tree_child(node, 0);
    if (!n00b_xform_leaf_text_eq(first, "typehash")) {
        return NULL;
    }

    n00b_parse_tree_t *atom = n00b_xform_find_child_nt(node, "typeid_atom");
    n00b_parse_tree_t *cont = n00b_xform_find_child_nt(node,
                                                        "typeid_continuation");
    if (!atom) {
        return NULL;
    }

    char    *type_str = n00b_xform_extract_type_string(ctx, atom, cont);
    uint64_t hash     = n00b_type_hash_u64(type_str);

    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIu64 "ULL", hash);

    uint32_t line, col;
    n00b_xform_first_leaf_pos(node, &line, &col);

    n00b_parse_tree_t *replacement = n00b_xform_make_token_node(
        N00B_TOK_INTEGER, buf, line, col);

    free(type_str);
    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
n00b_register_typehash_xform(n00b_xform_registry_t *reg)
{
    n00b_xform_register(reg, "primary_expression", xform_typehash,
                         "typehash");
}
