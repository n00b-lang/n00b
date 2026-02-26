// parse_tree.c - Shared parse tree operations (engine-agnostic).

#include "slay/parse_tree.h"
#include "internal/slay/grammar_internal.h"
#include "core/alloc.h"

// ============================================================================
// Node inspection
// ============================================================================

bool
n00b_parse_node_is_token(n00b_parse_tree_t *t)
{
    return t && n00b_tree_is_leaf(t);
}

n00b_token_info_t *
n00b_parse_node_token(n00b_parse_tree_t *t)
{
    if (!t || !n00b_tree_is_leaf(t)) {
        return NULL;
    }

    return n00b_tree_leaf_value(t);
}

n00b_option_t(n00b_string_t)
n00b_parse_node_name(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return n00b_option_none(n00b_string_t);
    }

    return n00b_option_set(n00b_string_t, n00b_tree_node_value(t).name);
}

// ============================================================================
// Tree walking
// ============================================================================

void *
n00b_parse_tree_walk(n00b_grammar_t *g, n00b_parse_tree_t *node, void *thunk)
{
    if (!node) {
        return NULL;
    }

    if (n00b_tree_is_leaf(node)) {
        return n00b_tree_leaf_value(node);
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(node);

    if (pn->id == N00B_EMPTY_STRING) {
        return NULL;
    }

    size_t nc = n00b_tree_num_children(node);

    void **sub_results = NULL;

    if (nc) {
        sub_results = n00b_alloc_array(void *, nc);

        for (size_t i = 0; i < nc; i++) {
            sub_results[i] = n00b_parse_tree_walk(g,
                                                   n00b_tree_child(node, i),
                                                   thunk);
        }
    }

    // Group wrapper nodes (from BNF ?, *, +) have pn->id = N00B_GROUP_ID
    // which won't resolve to a real nonterm. Skip the action lookup.
    if (pn->group_top) {
        return sub_results;
    }

    n00b_nonterm_t    *nt     = n00b_get_nonterm(g, pn->id);
    n00b_walk_action_t action = (nt && nt->action) ? nt->action
                                                    : g->default_action;

    if (!action) {
        return sub_results;
    }

    return (*action)(pn, sub_results, thunk);
}

// ============================================================================
// Tree free
// ============================================================================

void
n00b_parse_tree_free(n00b_parse_tree_t *t)
{
    if (!t) {
        return;
    }

    if (!t->is_leaf) {
        for (size_t i = 0; i < t->node.num_children; i++) {
            n00b_parse_tree_free(t->node.children[i]);
        }
    }

    n00b_tree_free_node(t);
}

// ============================================================================
// Extended node inspection
// ============================================================================

const char *
n00b_pt_nt_name(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return NULL;
    }

    n00b_nt_node_t *pn = &n00b_tree_node_value(t);
    return pn->name.data;
}

size_t
n00b_pt_nt_name_len(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return 0;
    }

    return n00b_tree_node_value(t).name.u8_bytes;
}

bool
n00b_pt_is_nt(n00b_parse_tree_t *t, const char *nt_name)
{
    const char *name = n00b_pt_nt_name(t);

    if (!name || !nt_name) {
        return false;
    }

    size_t nlen = n00b_pt_nt_name_len(t);
    size_t clen = strlen(nt_name);

    return nlen == clen && memcmp(name, nt_name, nlen) == 0;
}

int64_t
n00b_pt_nt_id(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return -1;
    }

    return n00b_tree_node_value(t).id;
}

bool
n00b_pt_is_token(n00b_parse_tree_t *t)
{
    if (!t) {
        return false;
    }

    return n00b_tree_is_leaf(t);
}

const char *
n00b_pt_token_text(n00b_parse_tree_t *t)
{
    if (!t || !n00b_tree_is_leaf(t)) {
        return NULL;
    }

    n00b_token_info_t *tok = n00b_tree_leaf_value(t);

    if (!tok || !n00b_option_is_set(tok->value)) {
        return NULL;
    }

    return n00b_option_get(tok->value).data;
}

size_t
n00b_pt_token_text_len(n00b_parse_tree_t *t)
{
    if (!t || !n00b_tree_is_leaf(t)) {
        return 0;
    }

    n00b_token_info_t *tok = n00b_tree_leaf_value(t);

    if (!tok || !n00b_option_is_set(tok->value)) {
        return 0;
    }

    return n00b_option_get(tok->value).u8_bytes;
}

size_t
n00b_pt_num_children(n00b_parse_tree_t *t)
{
    if (!t) {
        return 0;
    }

    return n00b_tree_num_children(t);
}

n00b_parse_tree_t *
n00b_pt_get_child(n00b_parse_tree_t *t, size_t index)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return NULL;
    }

    if (index >= n00b_tree_num_children(t)) {
        return NULL;
    }

    return n00b_tree_child(t, index);
}

// ============================================================================
// Pattern matching / search helpers
// ============================================================================

n00b_parse_tree_t *
n00b_pt_find_child_by_nt(n00b_parse_tree_t *node, const char *nt_name)
{
    if (!node || n00b_tree_is_leaf(node) || !nt_name) {
        return NULL;
    }

    size_t nch = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_pt_is_nt(child, nt_name)) {
            return child;
        }
    }

    return NULL;
}

n00b_parse_tree_t *
n00b_pt_find_child_by_token(n00b_parse_tree_t *node, const char *token_text)
{
    if (!node || n00b_tree_is_leaf(node) || !token_text) {
        return NULL;
    }

    size_t tlen = strlen(token_text);
    size_t nch  = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);
        const char        *text  = n00b_pt_token_text(child);

        if (!text) {
            continue;
        }

        size_t clen = n00b_pt_token_text_len(child);

        if (clen == tlen && memcmp(text, token_text, tlen) == 0) {
            return child;
        }
    }

    return NULL;
}

int
n00b_pt_collect_children_by_nt(n00b_parse_tree_t  *node,
                                const char         *nt_name,
                                n00b_parse_tree_t **out,
                                int                 max)
{
    if (!node || n00b_tree_is_leaf(node) || !nt_name || !out || max <= 0) {
        return 0;
    }

    int    count = 0;
    size_t nch   = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch && count < max; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_pt_is_nt(child, nt_name)) {
            out[count++] = child;
        }
    }

    return count;
}

bool
n00b_pt_is_group(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return false;
    }

    // Group nodes have group_top set, or start with "$$".
    n00b_nt_node_t *pn = &n00b_tree_node_value(t);

    if (pn->group_top) {
        return true;
    }

    const char *name = pn->name.data;
    size_t      nlen = pn->name.u8_bytes;

    return name && nlen >= 2 && name[0] == '$' && name[1] == '$';
}

int
n00b_pt_collect_nt_deep(n00b_parse_tree_t  *node,
                          const char         *nt_name,
                          n00b_parse_tree_t **out,
                          int                 max)
{
    if (!node || n00b_tree_is_leaf(node) || !nt_name || !out || max <= 0) {
        return 0;
    }

    int    count = 0;
    size_t nch   = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch && count < max; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_pt_is_nt(child, nt_name)) {
            out[count++] = child;
        }
        else if (n00b_pt_is_group(child)) {
            count += n00b_pt_collect_nt_deep(
                child, nt_name, out + count, max - count);
        }
    }

    return count;
}

n00b_parse_tree_t *
n00b_pt_first_token(n00b_parse_tree_t *node)
{
    if (!node) {
        return NULL;
    }

    if (n00b_pt_is_token(node)) {
        return node;
    }

    if (n00b_tree_is_leaf(node)) {
        return NULL;
    }

    size_t nch = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch; i++) {
        n00b_parse_tree_t *found = n00b_pt_first_token(n00b_tree_child(node, i));

        if (found) {
            return found;
        }
    }

    return NULL;
}

n00b_parse_tree_t *
n00b_pt_last_token(n00b_parse_tree_t *node)
{
    if (!node) {
        return NULL;
    }

    if (n00b_pt_is_token(node)) {
        return node;
    }

    if (n00b_tree_is_leaf(node)) {
        return NULL;
    }

    size_t nch = n00b_tree_num_children(node);

    for (size_t i = nch; i > 0; i--) {
        n00b_parse_tree_t *found = n00b_pt_last_token(
            n00b_tree_child(node, i - 1));

        if (found) {
            return found;
        }
    }

    return NULL;
}

// ============================================================================
// Deep search (recursive DFS over entire subtree)
// ============================================================================

static int
search_by_nt_recurse(n00b_parse_tree_t  *node,
                     const char         *nt_name,
                     size_t              nt_len,
                     n00b_parse_tree_t **out,
                     int                 max,
                     int                 count)
{
    if (!node || count >= max) {
        return count;
    }

    // Check this node.
    if (!n00b_tree_is_leaf(node)) {
        n00b_nt_node_t *pn   = &n00b_tree_node_value(node);
        const char     *name = pn->name.data;
        size_t          nlen = pn->name.u8_bytes;

        if (name && nlen == nt_len && memcmp(name, nt_name, nlen) == 0) {
            out[count++] = node;

            if (count >= max) {
                return count;
            }
        }

        // Recurse into children.
        size_t nch = n00b_tree_num_children(node);

        for (size_t i = 0; i < nch && count < max; i++) {
            count = search_by_nt_recurse(
                n00b_tree_child(node, i), nt_name, nt_len, out, max, count);
        }
    }

    return count;
}

int
n00b_pt_search_by_nt(n00b_parse_tree_t  *node,
                      const char         *nt_name,
                      n00b_parse_tree_t **out,
                      int                 max)
{
    if (!node || !nt_name || !out || max <= 0) {
        return 0;
    }

    return search_by_nt_recurse(node, nt_name, strlen(nt_name), out, max, 0);
}

static int
search_by_token_text_recurse(n00b_parse_tree_t  *node,
                             const char         *text,
                             size_t              tlen,
                             n00b_parse_tree_t **out,
                             int                 max,
                             int                 count)
{
    if (!node || count >= max) {
        return count;
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && n00b_option_is_set(tok->value)) {
            n00b_string_t val = n00b_option_get(tok->value);

            if (val.u8_bytes == tlen && memcmp(val.data, text, tlen) == 0) {
                out[count++] = node;
            }
        }

        return count;
    }

    size_t nch = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch && count < max; i++) {
        count = search_by_token_text_recurse(
            n00b_tree_child(node, i), text, tlen, out, max, count);
    }

    return count;
}

int
n00b_pt_search_by_token_text(n00b_parse_tree_t  *node,
                              const char         *text,
                              n00b_parse_tree_t **out,
                              int                 max)
{
    if (!node || !text || !out || max <= 0) {
        return 0;
    }

    return search_by_token_text_recurse(node, text, strlen(text), out, max, 0);
}

static int
search_by_tid_recurse(n00b_parse_tree_t  *node,
                      int64_t             tid,
                      n00b_parse_tree_t **out,
                      int                 max,
                      int                 count)
{
    if (!node || count >= max) {
        return count;
    }

    if (n00b_tree_is_leaf(node)) {
        n00b_token_info_t *tok = n00b_tree_leaf_value(node);

        if (tok && tok->tid == tid) {
            out[count++] = node;
        }

        return count;
    }

    size_t nch = n00b_tree_num_children(node);

    for (size_t i = 0; i < nch && count < max; i++) {
        count = search_by_tid_recurse(
            n00b_tree_child(node, i), tid, out, max, count);
    }

    return count;
}

int
n00b_pt_search_by_tid(n00b_parse_tree_t  *node,
                       int64_t             tid,
                       n00b_parse_tree_t **out,
                       int                 max)
{
    if (!node || !out || max <= 0) {
        return 0;
    }

    return search_by_tid_recurse(node, tid, out, max, 0);
}

// ============================================================================
// Default disambiguator
// ============================================================================

static size_t
count_nodes(n00b_parse_tree_t *t)
{
    if (!t) {
        return 0;
    }

    if (n00b_tree_is_leaf(t)) {
        return 1;
    }

    size_t count = 1;
    size_t nc    = n00b_tree_num_children(t);

    for (size_t i = 0; i < nc; i++) {
        count += count_nodes(n00b_tree_child(t, i));
    }

    return count;
}

// Recursive structural comparison for deterministic tie-breaking.
static int
structural_cmp(n00b_parse_tree_t *a, n00b_parse_tree_t *b)
{
    if (a == b) {
        return 0;
    }

    if (!a) {
        return -1;
    }

    if (!b) {
        return 1;
    }

    bool a_leaf = n00b_tree_is_leaf(a);
    bool b_leaf = n00b_tree_is_leaf(b);

    if (a_leaf != b_leaf) {
        return a_leaf ? -1 : 1;
    }

    if (a_leaf) {
        n00b_token_info_t *ta = n00b_tree_leaf_value(a);
        n00b_token_info_t *tb = n00b_tree_leaf_value(b);

        if (!ta || !tb) {
            return (ta ? 1 : 0) - (tb ? 1 : 0);
        }

        if (ta->tid != tb->tid) {
            return ta->tid < tb->tid ? -1 : 1;
        }

        return 0;
    }

    n00b_nt_node_t *pa = &n00b_tree_node_value(a);
    n00b_nt_node_t *pb = &n00b_tree_node_value(b);

    if (pa->rule_index != pb->rule_index) {
        return pa->rule_index < pb->rule_index ? -1 : 1;
    }

    if (pa->id != pb->id) {
        return pa->id < pb->id ? -1 : 1;
    }

    size_t na = n00b_tree_num_children(a);
    size_t nb = n00b_tree_num_children(b);

    if (na != nb) {
        return na < nb ? -1 : 1;
    }

    for (size_t i = 0; i < na; i++) {
        int c = structural_cmp(n00b_tree_child(a, i), n00b_tree_child(b, i));

        if (c != 0) {
            return c;
        }
    }

    return 0;
}

int
n00b_parse_tree_default_disambig(n00b_parse_tree_t *a, n00b_parse_tree_t *b)
{
    if (a == b) {
        return 0;
    }

    if (!a) {
        return 1;
    }

    if (!b) {
        return -1;
    }

    // Both interior — compare penalty, cost.
    if (!n00b_tree_is_leaf(a) && !n00b_tree_is_leaf(b)) {
        n00b_nt_node_t *pa = &n00b_tree_node_value(a);
        n00b_nt_node_t *pb = &n00b_tree_node_value(b);

        if (pa->penalty != pb->penalty) {
            return pa->penalty < pb->penalty ? -1 : 1;
        }

        if (pa->cost != pb->cost) {
            return pa->cost < pb->cost ? -1 : 1;
        }
    }

    // Prefer the more specific parse (more total nodes).
    size_t ca = count_nodes(a);
    size_t cb = count_nodes(b);

    if (ca != cb) {
        return ca > cb ? -1 : 1;
    }

    // Fall back to structural comparison for determinism.
    return structural_cmp(a, b);
}
