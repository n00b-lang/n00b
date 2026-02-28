// tree_util.c - Shared parse-tree helpers for annotation walk and type inference.

#include "slay/tree_util.h"
#include "internal/slay/grammar_internal.h"
#include "text/strings/string_ops.h"

// ============================================================================
// extract_first_identifier
// ============================================================================

n00b_string_t *
n00b_tree_extract_first_identifier(n00b_parse_tree_t *node)
{
    if (!node) {
        return n00b_string_empty();
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t *val = n00b_option_get(tok->value);

            if (val->u8_bytes > 0) {
                return val;
            }
        }

        return n00b_string_empty();
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_string_t *s = n00b_tree_extract_first_identifier(
            n00b_tree_child(node, i));

        if (s->u8_bytes > 0) {
            return s;
        }
    }

    return n00b_string_empty();
}

// ============================================================================
// find_child_by_nt_name
// ============================================================================

n00b_parse_tree_t *
n00b_tree_find_child_by_nt_name(n00b_grammar_t *g, n00b_parse_tree_t *parent,
                                n00b_string_t *name)
{
    size_t nc = n00b_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(parent, i);

        if (n00b_tree_is_leaf(child)) {
            continue;
        }

        n00b_nt_node_t *cpn = &n00b_tree_node_value(child);

        if (cpn->group_top) {
            n00b_parse_tree_t *found
                = n00b_tree_find_child_by_nt_name(g, child, name);

            if (found) {
                return found;
            }

            continue;
        }

        if (cpn->id >= 0) {
            n00b_nonterm_t *nt = n00b_get_nonterm(g, cpn->id);

            if (nt && n00b_unicode_str_eq(nt->name, name)) {
                return child;
            }
        }
    }

    return NULL;
}

// ============================================================================
// find_first_terminal
// ============================================================================

n00b_token_info_t *
n00b_tree_find_first_terminal(n00b_parse_tree_t *node)
{
    if (!node) {
        return NULL;
    }

    if (n00b_tree_is_leaf(node)) {
        return n00b_tree_leaf_value(node);
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        n00b_token_info_t *tok
            = n00b_tree_find_first_terminal(n00b_tree_child(node, i));

        if (tok) {
            return tok;
        }
    }

    return NULL;
}

// ============================================================================
// resolve_child_ref
// ============================================================================

n00b_parse_tree_t *
n00b_tree_resolve_child_ref(n00b_grammar_t *g, n00b_parse_tree_t *parent,
                            n00b_child_ref_t ref)
{
    switch (ref.kind) {
    case N00B_ROLE_BY_INDEX:
        if (ref.index < 0) {
            return NULL;
        }

        if ((size_t)ref.index >= n00b_tree_num_children(parent)) {
            return NULL;
        }

        return n00b_tree_child(parent, ref.index);

    case N00B_ROLE_BY_NAME:
        return n00b_tree_find_child_by_nt_name(g, parent, ref.name);
    }

    return NULL;
}

// ============================================================================
// extract_member_chain
// ============================================================================

// Recursive helper: appends identifiers from the member-chain tree to buf.
// Returns number of bytes written, or -1 on error.
static int32_t
member_chain_recurse(n00b_parse_tree_t *node, char *buf, int32_t cap,
                     int32_t pos)
{
    if (!node) {
        return pos;
    }

    // Leaf node: extract the identifier text.
    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t *val = n00b_option_get(tok->value);

            if (val->u8_bytes > 0) {
                // Add dot separator if not first identifier.
                if (pos > 0 && pos < cap - 1) {
                    buf[pos++] = '.';
                }

                int32_t len = (int32_t)val->u8_bytes;

                if (pos + len >= cap) {
                    return -1;
                }

                memcpy(buf + pos, val->data, (size_t)len);
                pos += len;
            }
        }

        return pos;
    }

    size_t nc = n00b_tree_num_children(node);

    // For a member-chain interior node, children are:
    //   [0] = <member-chain> (recursive) or IDENTIFIER
    //   [1] = "." terminal (skipped)
    //   [2] = IDENTIFIER terminal
    // For a single IDENTIFIER, nc == 1 and child[0] is the leaf.
    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_tree_is_leaf(child)) {
            n00b_token_info_t *tok = n00b_tree_leaf_value(child);

            if (!tok || !n00b_option_is_set(tok->value)) {
                continue;
            }

            n00b_string_t *val = n00b_option_get(tok->value);

            // Skip the "." separator token.
            if (val->u8_bytes == 1 && val->data[0] == '.') {
                continue;
            }

            if (val->u8_bytes > 0) {
                if (pos > 0 && pos < cap - 1) {
                    buf[pos++] = '.';
                }

                int32_t len = (int32_t)val->u8_bytes;

                if (pos + len >= cap) {
                    return -1;
                }

                memcpy(buf + pos, val->data, (size_t)len);
                pos += len;
            }
        }
        else {
            // Recurse into nested <member-chain>.
            pos = member_chain_recurse(child, buf, cap, pos);

            if (pos < 0) {
                return -1;
            }
        }
    }

    return pos;
}

int32_t
n00b_tree_extract_member_chain(n00b_parse_tree_t *node, char *buf, int32_t cap)
{
    if (!node || !buf || cap <= 0) {
        return -1;
    }

    int32_t pos = member_chain_recurse(node, buf, cap, 0);

    if (pos < 0 || pos >= cap) {
        return -1;
    }

    buf[pos] = '\0';

    return pos;
}

// ============================================================================
// get_nth_nt_child
// ============================================================================

n00b_parse_tree_t *
n00b_tree_get_nth_nt_child(n00b_parse_tree_t *node, int32_t n)
{
    size_t  nc   = n00b_tree_num_children(node);
    int32_t seen = 0;

    for (size_t i = 0; i < nc; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_tree_is_leaf(child)) {
            continue;
        }

        n00b_nt_node_t *cpn = &n00b_tree_node_value(child);

        if (cpn->group_top) {
            size_t gnc = n00b_tree_num_children(child);

            for (size_t j = 0; j < gnc; j++) {
                n00b_parse_tree_t *gchild = n00b_tree_child(child, j);

                if (!n00b_tree_is_leaf(gchild)) {
                    if (seen == n) {
                        return gchild;
                    }

                    seen++;
                }
            }

            continue;
        }

        if (seen == n) {
            return child;
        }

        seen++;
    }

    return NULL;
}
