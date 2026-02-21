/**
 * @file xform_flatten.c
 * @brief Tree flattening transformation for left-recursive structures.
 *
 * Left-recursive grammar rules produce deeply nested trees that are hard
 * to debug and expensive to walk. This transformation flattens them into
 * single nodes with multiple children.
 *
 * Example - before flattening:
 *   translation_unit_0
 *     translation_unit_0
 *       translation_unit_1
 *         external_declaration (first)
 *       external_declaration (second)
 *     external_declaration (third)
 *
 * After flattening:
 *   translation_unit
 *     external_declaration (first)
 *     external_declaration (second)
 *     external_declaration (third)
 *
 * This applies to any left-recursive structure: parameter_list,
 * argument_expression_list, block_item_list, etc.
 */

#include <stdio.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include "ncc_limits.h"
#include <string.h>

#include "transform.h"
#include "rewrite.h"
#include "nt_types.h"
#include "types.h" // For MAX_TYPE_ELEMENTS
#include "token.h" // For TT_PUNCT

// Check if a node is valid (not null and not elided)
static inline bool
is_valid_node(tnode_t *node)
{
    extern const tnode_t elided_node;
    return node && node != (tnode_t *)&elided_node;
}

/**
 * @brief Check if a node is a separator token (comma or semicolon).
 *
 * Separator tokens are removed during flattening; the emitter will
 * re-insert them based on the NT type being emitted.
 */
static bool
is_separator_token(tnode_t *node, ncc_buf_t *input)
{
    if (!is_valid_node(node) || !node->tptr) {
        return false;
    }

    tok_t *tok = node->tptr;
    if (tok->type != TT_PUNCT) {
        return false;
    }

    // Get token text
    const char *text;
    int         len;
    if (tok->replacement) {
        text = tok->replacement->data;
        len  = tok->replacement->len;
    }
    else {
        text = input->data + tok->offset;
        len  = tok->len;
    }

    // Check for comma (the main separator we remove)
    if (len == 1 && text[0] == ',') {
        return true;
    }

    return false;
}

/**
 * @brief Check if a node has a child with the same NT type (recursive structure).
 *
 * Only returns true for list structures defined in NT_SET_FLATTENABLE_LISTS,
 * NOT binary expression chains which have specific associativity.
 */
static bool
has_same_type_child(tnode_t *node)
{
    if (!node || node->nt_id == NT_NONE) {
        return false;
    }

    // Only flatten known list structures (not binary expression chains)
    if (!NT_IN_SET(node->nt_id, NT_SET_FLATTENABLE_LISTS)) {
        return false;
    }

    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (is_valid_node(kid) && kid->nt_id == node->nt_id) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Flatten the same-type chain into an array of chain nodes (iterative).
 *
 * Follows same-type children to collect all nodes in the recursive chain
 * in left-to-right (depth-first) order. Returns the count.
 */
static int
collect_chain_nodes(tnode_t *node, nt_type_t root_type,
                    tnode_t ***out_nodes, int *out_cap)
{
    int       cnt = 0;
    int       cap = NCC_CAP_LARGE;
    tnode_t **arr = base_alloc(cap * sizeof(tnode_t *));
    if (!arr) {
        *out_nodes = nullptr;
        return 0;
    }

    // Use a stack to DFS through same-type children, pushing in reverse
    // so leftmost is popped first.
    int       stk_cap = NCC_CAP_LARGE;
    int       stk_top = 0;
    tnode_t **stk     = base_alloc(stk_cap * sizeof(tnode_t *));
    if (!stk) {
        base_dealloc(arr);
        *out_nodes = nullptr;
        return 0;
    }

    stk[stk_top++] = node;

    while (stk_top > 0) {
        tnode_t *n = stk[--stk_top];

        // Record this chain node
        if (cnt >= cap) {
            cap *= 2;
            arr = base_realloc(arr, cap * sizeof(tnode_t *));
        }
        arr[cnt++] = n;

        // Push same-type children in reverse order for correct left-to-right DFS
        for (int i = n->num_kids - 1; i >= 0; i--) {
            tnode_t *kid = tnode_get_kid(n, i);
            if (is_valid_node(kid) && kid->nt_id == root_type) {
                if (stk_top >= stk_cap) {
                    stk_cap *= 2;
                    stk = base_realloc(stk, stk_cap * sizeof(tnode_t *));
                }
                stk[stk_top++] = kid;
            }
        }
    }

    base_dealloc(stk);

    // Reverse: the recursive version visits deepest same-type children first
    // (recurses before collecting), so chain nodes must be in reverse DFS order.
    for (int i = 0, j = cnt - 1; i < j; i++, j--) {
        tnode_t *tmp = arr[i];
        arr[i]       = arr[j];
        arr[j]       = tmp;
    }

    *out_nodes = arr;
    *out_cap   = cap;
    return cnt;
}

/**
 * @brief Count total payload children in a recursive chain (iterative).
 */
static int
count_payload_children(tnode_t *node, nt_type_t root_type, ncc_buf_t *input)
{
    if (!is_valid_node(node)) {
        return 0;
    }

    tnode_t **chain = nullptr;
    int       cap   = 0;
    int       n     = collect_chain_nodes(node, root_type, &chain, &cap);
    int       count = 0;

    for (int ci = 0; ci < n; ci++) {
        tnode_t *cn = chain[ci];
        for (int i = 0; i < cn->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(cn, i);
            if (!is_valid_node(kid)) {
                continue;
            }
            if (kid->nt_id != root_type && !is_separator_token(kid, input)) {
                count++;
            }
        }
    }

    base_dealloc(chain);
    return count;
}

/**
 * @brief Collect payload children from a recursive chain (iterative).
 *
 * Walks the chain in left-to-right order, collecting non-same-type,
 * non-separator children. Returns the next available index.
 */
static int
collect_payload_children(tnode_t *node, nt_type_t root_type,
                         tnode_t **out, int idx, ncc_buf_t *input)
{
    if (!is_valid_node(node)) {
        return idx;
    }

    tnode_t **chain = nullptr;
    int       cap   = 0;
    int       n     = collect_chain_nodes(node, root_type, &chain, &cap);

    for (int ci = 0; ci < n; ci++) {
        tnode_t *cn = chain[ci];
        for (int i = 0; i < cn->num_kids; i++) {
            tnode_t *kid = tnode_get_kid(cn, i);
            if (is_valid_node(kid) && kid->nt_id != root_type
                && !is_separator_token(kid, input)) {
                out[idx++] = kid;
            }
        }
    }

    base_dealloc(chain);
    return idx;
}

/**
 * @brief Get the base name of an NT (strip branch suffix).
 *
 * E.g., "translation_unit_0" -> "translation_unit"
 */
static char *
get_base_nt_name(const char *nt)
{
    if (!nt) {
        return nullptr;
    }

    // Find the last underscore followed by digits
    const char *last_underscore = nullptr;
    const char *p               = nt;

    while (*p) {
        if (*p == '_') {
            // Check if rest is all digits
            const char *q       = p + 1;
            bool        all_dig = (*q != '\0');
            while (*q) {
                if (*q < '0' || *q > '9') {
                    all_dig = false;
                    break;
                }
                q++;
            }
            if (all_dig) {
                last_underscore = p;
            }
        }
        p++;
    }

    if (last_underscore) {
        size_t len    = last_underscore - nt;
        char  *result = base_alloc(len + 1);
        memcpy(result, nt, len);
        result[len] = '\0';
        return result;
    }

    return base_strdup(nt);
}

/**
 * @brief Flatten a recursive node structure.
 *
 * Creates a new node with all payload children collected from the chain.
 * Separator tokens (commas) are removed; the emitter will re-insert them.
 */
static tnode_t *
xform_flatten_recursive(tree_xform_t *ctx, tnode_t *node)
{
    // Only process nodes that have same-type children
    if (!has_same_type_child(node)) {
        return nullptr;
    }

    nt_type_t root_type = node->nt_id;
    ncc_buf_t    *input     = ctx->input;

    // Count how many payload children we'll have (excluding separators)
    int total = count_payload_children(node, root_type, input);
    if (total == 0) {
        return nullptr;
    }

    // Allocate list for collecting children
    ncc_list_t *children = ncc_list_alloc(total);
    if (!children) {
        return nullptr;
    }

    // Collect all payload children in order (excluding separators)
    int collected = collect_payload_children(node, root_type, (tnode_t **)children->data, 0, input);

    // Create new flattened node
    char    *base_name = get_base_nt_name(node->nt);
    tnode_t *flat      = synth_nonterminal(base_name ? base_name : node->nt);
    base_dealloc(base_name);

    // Set the nt_id to match the original
    flat->nt_id = root_type;

    // Pre-allocate kids list and add all collected children in one go
    // (avoids repeated reallocation from add_child loop)
    flat->kids     = ncc_list_alloc(collected);
    flat->num_kids = collected;
    // ncc_list_alloc already sets len = collected, so kids->len is in sync
    for (int i = 0; i < collected; i++) {
        tnode_t *child         = (tnode_t *)children->data[i];
        flat->kids->data[i]    = child;
        if (child) {
            child->parent = flat;
        }
    }

    base_dealloc(children);

    // Tag flattened node so the emitter knows to re-insert commas
    rewrite_origin_t *origin = base_calloc(1, sizeof(rewrite_origin_t));
    origin->rewrite_name     = "flatten";
    flat->origin             = origin;

    // Replace the old node with the flattened one (fast path - no origin tracking)
    replace_node_fast(node, flat);

    return flat;
}

/**
 * @brief Register the tree flattening transformation.
 *
 * Registers as a wildcard post-order transformer so it runs after
 * children are processed and applies to all node types.
 */
void
register_flatten_xform(xform_registry_t *reg)
{
    // Use NT_NONE as wildcard to match all node types
    xform_register_post(reg, NT_NONE, xform_flatten_recursive, "flatten");
}
