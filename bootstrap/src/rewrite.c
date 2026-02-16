/**
 * @file rewrite.c
 * @brief Parse tree rewrite API implementation.
 */

#include <assert.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

#include "rewrite.h"
#include "token.h"

/**
 * @brief Sequence counter for synthetic token offsets.
 *
 * Each synthetic token gets a unique offset = SYNTHETIC_OFFSET_BASE + sequence.
 * This ensures synthetic tokens can be distinguished and ordered.
 */
static int synth_sequence = 0;

/**
 * @brief Node ID counter for synthetic nodes.
 *
 * Shared with parse.c's next_node_id for consistency.
 * Since parse.c's counter is static, we maintain our own and
 * start it high enough to avoid conflicts.
 */
static int synth_node_id = 1000000;

/* ========================================================================
 * Synthetic Token Creation
 * ======================================================================== */

tok_t *
synth_token(const char *text, ttype_t type, int source_line)
{
    assert(text != nullptr);

    tok_t *tok = base_calloc(1, sizeof(tok_t));
    assert(tok != nullptr);

    // Create replacement buffer with the text
    int        len = strlen(text);
    ncc_buf_t *rep = ncc_buf_alloc(len + 1);
    assert(rep != nullptr);
    rep->len = len;
    memcpy(rep->data, text, len);
    rep->data[len] = '\0';

    *tok = (tok_t){
        .replacement = rep,
        .offset      = SYNTHETIC_OFFSET_BASE + synth_sequence++,
        .len         = len,
        .line_no     = source_line,
        .type        = type,
        .skip_emit   = 0,
        .synthetic   = 1,
    };

    return tok;
}

/* ========================================================================
 * Synthetic Node Creation
 * ======================================================================== */

tnode_t *
synth_terminal(const char *text, ttype_t type, int source_line)
{
    tok_t *tok = synth_token(text, type, source_line);

    tnode_t *node = alloc_tnode();
    assert(node != nullptr);

    // For terminals, nt is the token text (from the replacement buffer)
    *node = (tnode_t){
        .nt       = tok->replacement->data,
        .tptr     = tok,
        .parent   = nullptr,
        .origin   = nullptr,
        .id       = synth_node_id++,
        .num_kids = 0,
        .num_toks = 1,
    };

    return node;
}

tnode_t *
synth_nonterminal(const char *nt)
{
    assert(nt != nullptr);

    tnode_t *node = alloc_tnode();
    assert(node != nullptr);

    // Copy the nt string so we own it
    char *nt_copy = base_strdup(nt);
    assert(nt_copy != nullptr);

    *node = (tnode_t){
        .nt       = nt_copy,
        .tptr     = nullptr,
        .parent   = nullptr,
        .origin   = nullptr,
        .id       = synth_node_id++,
        .num_kids = 0,
        .num_toks = 0,
    };

    return node;
}

/* ========================================================================
 * Tree Manipulation
 * ======================================================================== */

/**
 * @brief Create origin tracking for a replacement.
 */
static rewrite_origin_t *
create_origin(tnode_t *original, const char *rewrite_name)
{
    rewrite_origin_t *origin = base_calloc(1, sizeof(rewrite_origin_t));
    assert(origin != nullptr);

    int start_line = -1;
    int end_line   = -1;
    get_source_range(original, &start_line, &end_line);

    origin->original_node       = original;
    origin->original_start_line = start_line;
    origin->original_end_line   = end_line;
    origin->rewrite_name        = rewrite_name ? base_strdup(rewrite_name) : nullptr;

    return origin;
}

tnode_t *
replace_child(tnode_t *parent, int child_idx, tnode_t *new_child, const char *rewrite_name)
{
    assert(parent != nullptr);
    assert(child_idx >= 0 && child_idx < parent->num_kids);
    assert(new_child != nullptr);

    tnode_t *old_child = tnode_get_kid(parent, child_idx);

    // Set up origin tracking
    if (old_child != nullptr) {
        new_child->origin = create_origin(old_child, rewrite_name);
    }

    // Replace in parent
    tnode_set_kid(parent, child_idx, new_child);
    new_child->parent = parent;

    return new_child;
}

tnode_t *
replace_node(tnode_t *old_node, tnode_t *new_node, const char *rewrite_name)
{
    assert(old_node != nullptr);
    assert(new_node != nullptr);

    // Set up origin tracking
    new_node->origin = create_origin(old_node, rewrite_name);

    // If old_node has a parent, find and replace in parent's children
    if (old_node->parent != nullptr) {
        tnode_t *parent = old_node->parent;
        for (int i = 0; i < parent->num_kids; i++) {
            if (tnode_get_kid(parent, i) == old_node) {
                tnode_set_kid(parent, i, new_node);
                new_node->parent = parent;
                break;
            }
        }
    }

    return new_node;
}

tnode_t *
replace_node_fast(tnode_t *old_node, tnode_t *new_node)
{
    assert(old_node != nullptr);
    assert(new_node != nullptr);

    // Skip origin tracking for performance

    // If old_node has a parent, find and replace in parent's children
    if (old_node->parent != nullptr) {
        tnode_t *parent = old_node->parent;
        for (int i = 0; i < parent->num_kids; i++) {
            if (tnode_get_kid(parent, i) == old_node) {
                tnode_set_kid(parent, i, new_node);
                new_node->parent = parent;
                break;
            }
        }
    }

    return new_node;
}

void
add_child(tnode_t *parent, tnode_t *child)
{
    assert(parent != nullptr);

    int old_count = parent->num_kids;
    int new_count = old_count + 1;

    // Grow the kids list
    list_t *new_kids = list_alloc(new_count);
    assert(new_kids != nullptr);

    // Copy existing children
    if (parent->kids) {
        for (int i = 0; i < old_count; i++) {
            new_kids->items[i] = parent->kids->items[i];
        }
        base_dealloc(parent->kids);
    }

    // Add new child
    new_kids->items[old_count] = child;
    parent->kids               = new_kids;
    parent->num_kids           = new_count;

    if (child != nullptr && !IS_ELIDED(child)) {
        child->parent = parent;
    }
}

/**
 * @brief Copy a parse tree iteratively.
 *
 * Uses an explicit stack to avoid deep recursion on large trees.
 */
typedef struct copy_frame_t {
    tnode_t *src;
    tnode_t *dst;
    int      child_idx;
} copy_frame_t;

tnode_t *
copy_tree(tnode_t *node)
{
    if (node == nullptr) {
        return nullptr;
    }

    // Handle elided nodes
    if (IS_ELIDED(node)) {
        return (tnode_t *)&elided_node;
    }

    // Allocate work stack
    int           stack_cap = 64;
    int           stack_top = 0;
    copy_frame_t *stack     = base_alloc(stack_cap * sizeof(copy_frame_t));
    assert(stack != nullptr);

    // Copy root node
    tnode_t *root_copy = alloc_tnode();
    assert(root_copy != nullptr);
    *root_copy = (tnode_t){
        .nt       = node->nt,
        .tptr     = node->tptr,
        .parent   = nullptr,
        .origin   = nullptr,
        .id       = synth_node_id++,
        .nt_id    = node->nt_id,
        .branch   = node->branch,
        .num_kids = node->num_kids,
        .num_toks = node->num_toks,
    };
    if (node->origin != nullptr) {
        root_copy->origin = base_calloc(1, sizeof(rewrite_origin_t));
        assert(root_copy->origin != nullptr);
        *root_copy->origin = *node->origin;
        if (node->origin->rewrite_name) {
            root_copy->origin->rewrite_name = base_strdup(node->origin->rewrite_name);
        }
    }
    if (node->num_kids > 0) {
        root_copy->kids = list_alloc(node->num_kids);
    }

    // Push root frame
    stack[stack_top++] = (copy_frame_t){.src = node, .dst = root_copy, .child_idx = 0};

    while (stack_top > 0) {
        copy_frame_t *f = &stack[stack_top - 1];

        if (f->child_idx >= f->src->num_kids) {
            stack_top--;
            continue;
        }

        int      ci    = f->child_idx++;
        tnode_t *child = tnode_get_kid(f->src, ci);

        if (child == nullptr) {
            f->dst->kids->items[ci] = nullptr;
            continue;
        }

        if (IS_ELIDED(child)) {
            f->dst->kids->items[ci] = (tnode_t *)&elided_node;
            continue;
        }

        // Copy the child node
        tnode_t *child_copy = alloc_tnode();
        assert(child_copy != nullptr);
        *child_copy = (tnode_t){
            .nt       = child->nt,
            .tptr     = child->tptr,
            .parent   = f->dst,
            .origin   = nullptr,
            .id       = synth_node_id++,
            .nt_id    = child->nt_id,
            .branch   = child->branch,
            .num_kids = child->num_kids,
            .num_toks = child->num_toks,
        };
        if (child->origin != nullptr) {
            child_copy->origin = base_calloc(1, sizeof(rewrite_origin_t));
            assert(child_copy->origin != nullptr);
            *child_copy->origin = *child->origin;
            if (child->origin->rewrite_name) {
                child_copy->origin->rewrite_name = base_strdup(child->origin->rewrite_name);
            }
        }
        f->dst->kids->items[ci] = child_copy;

        // If child has children, allocate kids list and push frame
        if (child->num_kids > 0) {
            child_copy->kids = list_alloc(child->num_kids);

            // Grow stack if needed
            if (stack_top >= stack_cap) {
                stack_cap *= 2;
                stack = base_realloc(stack, stack_cap * sizeof(copy_frame_t));
                assert(stack != nullptr);
                f = &stack[stack_top - 1]; // realloc may move
            }

            stack[stack_top++] = (copy_frame_t){.src = child, .dst = child_copy, .child_idx = 0};
        }
    }

    base_dealloc(stack);
    return root_copy;
}

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

int
get_node_line(tnode_t *node)
{
    // Iterative DFS to find first node with a line number.
    // Avoids deep recursion on large parse trees.
    int      stack_cap = 32;
    int      stack_top = 0;
    tnode_t **stack    = base_alloc(stack_cap * sizeof(tnode_t *));
    if (!stack) {
        return -1;
    }

    stack[stack_top++] = node;

    while (stack_top > 0) {
        tnode_t *n = stack[--stack_top];

        if (n == nullptr || IS_ELIDED(n)) {
            continue;
        }

        if (n->origin != nullptr) {
            base_dealloc(stack);
            return n->origin->original_start_line;
        }

        if (n->tptr != nullptr) {
            base_dealloc(stack);
            return n->tptr->line_no;
        }

        // Push children in reverse order so leftmost is processed first
        for (int i = n->num_kids - 1; i >= 0; i--) {
            if (stack_top >= stack_cap) {
                stack_cap *= 2;
                stack = base_realloc(stack, stack_cap * sizeof(tnode_t *));
                assert(stack != nullptr);
            }
            stack[stack_top++] = tnode_get_kid(n, i);
        }
    }

    base_dealloc(stack);
    return -1;
}

bool
is_synthetic_node(tnode_t *node)
{
    if (node == nullptr) {
        return false;
    }

    // Has origin tracking (was created by rewrite)
    if (node->origin != nullptr) {
        return true;
    }

    // Has synthetic token
    if (node->tptr != nullptr && node->tptr->synthetic) {
        return true;
    }

    return false;
}

/**
 * @brief Work item for iterative source range traversal.
 */
typedef struct range_item_t {
    tnode_t *node;
    int      child_idx;
} range_item_t;

#define RANGE_STACK_INITIAL_CAP 64

/**
 * @brief Helper to update min/max lines from a node's terminals.
 *
 * Uses iterative traversal to avoid deep recursion on large trees.
 */
static void
update_source_range(tnode_t *root, int *start_line, int *end_line)
{
    if (root == nullptr) {
        return;
    }

    // Skip elided nodes
    if (root->nt[0] == '<' && root->nt[1] == '<') {
        return;
    }

    // Allocate work stack
    int           stack_cap = RANGE_STACK_INITIAL_CAP;
    int           stack_top = 0;
    range_item_t *stack     = base_alloc(stack_cap * sizeof(range_item_t));

    if (!stack) {
        return; // Fallback on allocation failure
    }

    // Push root
    stack[stack_top++] = (range_item_t){.node = root, .child_idx = 0};

    while (stack_top > 0) {
        range_item_t *item = &stack[stack_top - 1];
        tnode_t      *node = item->node;

        // Check if this node has a terminal token - update range
        if (node->tptr != nullptr) {
            int line = node->tptr->line_no;
            if (*start_line < 0 || line < *start_line) {
                *start_line = line;
            }
            if (*end_line < 0 || line > *end_line) {
                *end_line = line;
            }
            // Terminal nodes have no children, so pop
            stack_top--;
            continue;
        }

        // Non-terminal: process next child
        if (item->child_idx < node->num_kids) {
            tnode_t *child = tnode_get_kid(node, item->child_idx);
            item->child_idx++;

            if (child == nullptr) {
                continue;
            }

            // Skip elided children
            if (child->nt[0] == '<' && child->nt[1] == '<') {
                continue;
            }

            // Grow stack if needed
            if (stack_top >= stack_cap) {
                stack_cap *= 2;
                range_item_t *new_stack = base_realloc(stack, stack_cap * sizeof(range_item_t));
                if (!new_stack) {
                    base_dealloc(stack);
                    return;
                }
                stack = new_stack;
                item  = &stack[stack_top - 1];
            }

            // Push child
            stack[stack_top++] = (range_item_t){.node = child, .child_idx = 0};
        }
        else {
            // All children processed - pop this node
            stack_top--;
        }
    }

    base_dealloc(stack);
}

void
get_source_range(tnode_t *node, int *start_line, int *end_line)
{
    assert(start_line != nullptr);
    assert(end_line != nullptr);

    *start_line = -1;
    *end_line   = -1;

    update_source_range(node, start_line, end_line);
}

int
get_next_node_id(void)
{
    return synth_node_id++;
}
