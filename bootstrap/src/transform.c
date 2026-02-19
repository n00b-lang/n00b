/**
 * @file transform.c
 * @brief Tree-walking transformer system implementation.
 */

#include <assert.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>

#include "lex.h"
#include "transform.h"

/**
 * @brief Free a list and all its xform_entry_t entries.
 *
 * @param list List to free (may be nullptr)
 */
static void
ncc_list_free_entries(ncc_list_t *list)
{
    if (!list) {
        return;
    }

    for (int i = 0; i < list->nitems; i++) {
        base_dealloc(list->items[i]);
    }
    base_dealloc(list);
}

/* ========================================================================
 * Registry Initialization and Cleanup
 * ======================================================================== */

void
xform_registry_init(xform_registry_t *reg)
{
    assert(reg != nullptr);
    memset(reg, 0, sizeof(*reg));
}

void
xform_registry_free(xform_registry_t *reg)
{
    if (!reg) {
        return;
    }

    // Free all pre-order lists
    for (int i = 0; i < NT_COUNT; i++) {
        ncc_list_free_entries(reg->pre_order[i]);
        reg->pre_order[i] = nullptr;
    }

    // Free all post-order lists
    for (int i = 0; i < NT_COUNT; i++) {
        ncc_list_free_entries(reg->post_order[i]);
        reg->post_order[i] = nullptr;
    }

    // Free wildcard lists
    ncc_list_free_entries(reg->wildcard_pre);
    ncc_list_free_entries(reg->wildcard_post);
    reg->wildcard_pre  = nullptr;
    reg->wildcard_post = nullptr;
}

/* ========================================================================
 * Transformer Registration
 * ======================================================================== */

bool
xform_register_post(xform_registry_t *reg, nt_type_t nt_id, xform_fn_t fn, const char *name)
{
    assert(reg != nullptr);
    assert(fn != nullptr);

    // Allocate entry
    xform_entry_t *entry = base_calloc(1, sizeof(xform_entry_t));
    if (!entry) {
        return false;
    }

    entry->post_fn = fn;
    entry->name    = name;
    entry->is_pre  = false;

    // Handle wildcard registration (NT_NONE matches all nodes)
    if (nt_id == NT_NONE) {
        reg->wildcard_post = ncc_list_append(reg->wildcard_post, entry);
        return true;
    }

    // Regular registration by NT type
    if (nt_id >= NT_COUNT) {
        base_dealloc(entry);
        return false;
    }

    reg->post_order[nt_id] = ncc_list_append(reg->post_order[nt_id], entry);
    return true;
}

bool
xform_register_pre(xform_registry_t *reg, nt_type_t nt_id, xform_pre_fn_t fn, const char *name)
{
    assert(reg != nullptr);
    assert(fn != nullptr);

    // Allocate entry
    xform_entry_t *entry = base_calloc(1, sizeof(xform_entry_t));
    if (!entry) {
        return false;
    }

    entry->pre_fn = fn;
    entry->name   = name;
    entry->is_pre = true;

    // Handle wildcard registration (NT_NONE matches all nodes)
    if (nt_id == NT_NONE) {
        reg->wildcard_pre = ncc_list_append(reg->wildcard_pre, entry);
        return true;
    }

    // Regular registration by NT type
    if (nt_id >= NT_COUNT) {
        base_dealloc(entry);
        return false;
    }

    reg->pre_order[nt_id] = ncc_list_append(reg->pre_order[nt_id], entry);
    return true;
}

int
xform_count(xform_registry_t *reg, nt_type_t nt_id, bool pre)
{
    if (!reg) {
        return 0;
    }

    ncc_list_t *list;

    if (nt_id == NT_NONE) {
        list = pre ? reg->wildcard_pre : reg->wildcard_post;
    }
    else if (nt_id < NT_COUNT) {
        list = pre ? reg->pre_order[nt_id] : reg->post_order[nt_id];
    }
    else {
        return 0;
    }

    return list ? list->nitems : 0;
}

/* ========================================================================
 * Context Initialization
 * ======================================================================== */

void
xform_ctx_init(tree_xform_t *ctx, lex_t *lex, symtab_t *symtab, tnode_t *root)
{
    assert(ctx != nullptr);

    *ctx = (tree_xform_t){
        .input            = lex ? lex->input : nullptr,
        .lex              = lex,
        .symtab           = symtab,
        .root             = root,
        .depth            = 0,
        .pass             = 0,
        .nodes_visited    = 0,
        .nodes_replaced   = 0,
        .user_data        = nullptr,
        .current_func_def = nullptr,
    };
}

/* ========================================================================
 * Internal: Apply Transformers to a Single Node
 * ======================================================================== */

/**
 * @brief Apply pre-order transformers to a node.
 *
 * @param reg     Registry
 * @param ctx     Context
 * @param node    Node to transform
 * @param control [out] Control value (set if any transformer wants to skip children)
 * @return Replacement node, or nullptr if no change
 */
static tnode_t *
apply_pre_transformers(xform_registry_t *reg, tree_xform_t *ctx, tnode_t *node, xform_control_t *control)
{
    *control        = XFORM_CONTINUE;
    tnode_t *result = nullptr;

    // Apply type-specific pre-order transformers
    nt_type_t nt_id = node->nt_id;
    if (nt_id > NT_NONE && nt_id < NT_COUNT) {
        ncc_list_t *list = reg->pre_order[nt_id];
        if (list) {
            for (int i = 0; i < list->nitems; i++) {
                xform_entry_t  *entry    = list->items[i];
                xform_control_t ctrl     = XFORM_CONTINUE;
                tnode_t        *replaced = entry->pre_fn(ctx, result ? result : node, &ctrl);

                if (replaced != nullptr) {
                    result = replaced;
                    ctx->nodes_replaced++;
                }
                if (ctrl == XFORM_SKIP_CHILDREN) {
                    *control = XFORM_SKIP_CHILDREN;
                }
            }
        }
    }

    // Apply wildcard pre-order transformers
    ncc_list_t *wildcard = reg->wildcard_pre;
    if (wildcard) {
        for (int i = 0; i < wildcard->nitems; i++) {
            xform_entry_t  *entry    = wildcard->items[i];
            xform_control_t ctrl     = XFORM_CONTINUE;
            tnode_t        *replaced = entry->pre_fn(ctx, result ? result : node, &ctrl);

            if (replaced != nullptr) {
                result = replaced;
                ctx->nodes_replaced++;
            }
            if (ctrl == XFORM_SKIP_CHILDREN) {
                *control = XFORM_SKIP_CHILDREN;
            }
        }
    }

    return result;
}

/**
 * @brief Apply post-order transformers to a node.
 *
 * @param reg  Registry
 * @param ctx  Context
 * @param node Node to transform
 * @return Replacement node, or nullptr if no change
 */
static tnode_t *
apply_post_transformers(xform_registry_t *reg, tree_xform_t *ctx, tnode_t *node)
{
    if (!node) return nullptr;

    tnode_t *result = nullptr;

    // Apply type-specific post-order transformers
    nt_type_t nt_id = node->nt_id;
    if (nt_id > NT_NONE && nt_id < NT_COUNT) {
        ncc_list_t *list = reg->post_order[nt_id];
        if (list) {
            for (int i = 0; i < list->nitems; i++) {
                xform_entry_t *entry    = list->items[i];
                tnode_t       *replaced = entry->post_fn(ctx, result ? result : node);

                if (replaced != nullptr) {
                    result = replaced;
                    ctx->nodes_replaced++;
                }
            }
        }
    }

    // Apply wildcard post-order transformers
    ncc_list_t *wildcard = reg->wildcard_post;
    if (wildcard) {
        for (int i = 0; i < wildcard->nitems; i++) {
            xform_entry_t *entry    = wildcard->items[i];
            tnode_t       *replaced = entry->post_fn(ctx, result ? result : node);

            if (replaced != nullptr) {
                result = replaced;
                ctx->nodes_replaced++;
            }
        }
    }

    return result;
}

/* ========================================================================
 * Internal: Iterative Tree Walk
 * ======================================================================== */

/**
 * @brief Work item for iterative tree traversal.
 *
 * The iterative algorithm uses a stack of work items. Each item tracks:
 * - The node being processed
 * - Which child we're currently processing
 * - Whether pre-order transforms have been applied
 * - Whether children should be skipped
 */
typedef struct walk_item_t {
    tnode_t *node;           /**< Current node being processed */
    tnode_t *current;        /**< Node after pre-transform (may differ from node) */
    tnode_t *saved_func_def; /**< Previous current_func_def (restore on pop) */
    int      child_idx;      /**< Next child index to process (-1 = pre, num_kids = post) */
    bool     skip_children;  /**< Whether to skip child processing */
} walk_item_t;

/**
 * @brief Initial stack capacity for iterative traversal.
 */
#define WALK_STACK_INITIAL_CAP 256

/**
 * @brief Iteratively walk and transform a node.
 *
 * Uses an explicit stack instead of recursion to avoid deep call stacks
 * on large parse trees. This significantly improves performance for
 * trees with thousands of nodes.
 *
 * @param reg  Registry
 * @param ctx  Context
 * @param root Root node to process
 * @return Transformed root (original, replacement, or nullptr)
 */
static tnode_t *
walk_node(xform_registry_t *reg, tree_xform_t *ctx, tnode_t *root)
{
    if (root == nullptr) {
        return nullptr;
    }

    // Allocate work stack
    int          stack_cap = WALK_STACK_INITIAL_CAP;
    int          stack_top = 0;
    walk_item_t *stack     = base_alloc(stack_cap * sizeof(walk_item_t));

    if (!stack) {
        return root; // Fallback: return unchanged on allocation failure
    }

    // Push root node
    stack[stack_top++] = (walk_item_t){
        .node           = root,
        .current        = root,
        .saved_func_def = nullptr,
        .child_idx      = -1, // -1 means pre-order not yet done
        .skip_children  = false,
    };

    tnode_t *final_result = root;

    while (stack_top > 0) {
        walk_item_t *item = &stack[stack_top - 1];

        // Phase 1: Pre-order (child_idx == -1)
        if (item->child_idx == -1) {
            tnode_t *node = item->node;

            // Skip elided nodes entirely
            if (IS_ELIDED(node)) {
                // Pop and continue - elided nodes pass through unchanged
                stack_top--;
                continue;
            }

            ctx->nodes_visited++;
            ctx->depth++;

            // Apply pre-order transformers
            xform_control_t control    = XFORM_CONTINUE;
            tnode_t        *pre_result = apply_pre_transformers(reg, ctx, node, &control);

            if (pre_result != nullptr) {
                item->current = pre_result;
            }
            else {
                item->current = node;
            }

            item->skip_children  = (control == XFORM_SKIP_CHILDREN);
            item->saved_func_def = nullptr;
            item->child_idx      = 0; // Move to child processing phase

            // Track enclosing function_definition for bang (!) transforms
            if (item->current->nt_id == NT_function_definition) {
                item->saved_func_def  = ctx->current_func_def;
                ctx->current_func_def = item->current;
            }
        }

        // Phase 2: Process children (child_idx >= 0 && < num_kids)
        tnode_t *current = item->current;

        if (!item->skip_children && item->child_idx < current->num_kids) {
            tnode_t *child = tnode_get_kid(current, item->child_idx);

            if (child != nullptr) {
                // Grow stack if needed
                if (stack_top >= stack_cap) {
                    stack_cap *= 2;
                    walk_item_t *new_stack = base_realloc(stack, stack_cap * sizeof(walk_item_t));
                    if (!new_stack) {
                        base_dealloc(stack);
                        return root; // Fallback on allocation failure
                    }
                    stack = new_stack;
                    item  = &stack[stack_top - 1]; // Revalidate pointer after realloc
                }

                // Push child for processing
                stack[stack_top++] = (walk_item_t){
                    .node           = child,
                    .current        = child,
                    .saved_func_def = nullptr,
                    .child_idx      = -1,
                    .skip_children  = false,
                };

                item->child_idx++; // Will process next child when we return
                continue;
            }
            else {
                item->child_idx++;
                continue;
            }
        }

        // Phase 3: Post-order (all children processed)
        // Skip to here if skip_children is set
        if (item->skip_children) {
            item->child_idx = current->num_kids; // Ensure we move to post phase
        }

        if (item->child_idx >= current->num_kids) {
            // Apply post-order transformers
            tnode_t *post_result = apply_post_transformers(reg, ctx, current);
            if (post_result != nullptr) {
                current       = post_result;
                item->current = post_result;
            }

            ctx->depth--;

            // Restore enclosing function_definition on exit
            if (item->current->nt_id == NT_function_definition) {
                ctx->current_func_def = item->saved_func_def;
            }

            // Pop this node from the stack
            tnode_t *result = item->current;
            tnode_t *orig   = item->node;
            stack_top--;

            // Update parent's child pointer if node was replaced
            if (stack_top > 0 && result != orig) {
                walk_item_t *parent_item = &stack[stack_top - 1];
                int          child_idx   = parent_item->child_idx - 1; // We already incremented

                if (child_idx >= 0 && child_idx < parent_item->current->num_kids) {
                    tnode_set_kid(parent_item->current, child_idx, result);
                    if (result != nullptr) {
                        result->parent = parent_item->current;
                    }
                }
            }

            // Track final result for root
            if (stack_top == 0) {
                final_result = result;
            }
        }
    }

    base_dealloc(stack);
    return final_result;
}

/* ========================================================================
 * Public API: Transformation Application
 * ======================================================================== */

tnode_t *
xform_apply(xform_registry_t *reg, tree_xform_t *ctx)
{
    assert(reg != nullptr);
    assert(ctx != nullptr);

    // Reset per-pass statistics
    ctx->nodes_visited  = 0;
    ctx->nodes_replaced = 0;
    ctx->depth          = 0;

    if (ctx->root == nullptr) {
        return nullptr;
    }

    // Walk the tree
    tnode_t *result = walk_node(reg, ctx, ctx->root);

    // Update root if it was replaced
    if (result != ctx->root) {
        ctx->root = result;
    }

    return result;
}

tnode_t *
xform_apply_multi(xform_registry_t *reg, tree_xform_t *ctx, int max_passes)
{
    assert(reg != nullptr);
    assert(ctx != nullptr);

    if (max_passes <= 0) {
        max_passes = XFORM_DEFAULT_MAX_PASSES;
    }

    ctx->pass = 0;

    while (ctx->pass < max_passes) {
        tnode_t *result = xform_apply(reg, ctx);

        // If nothing was replaced, we've reached a fixed point
        if (ctx->nodes_replaced == 0) {
            break;
        }

        ctx->pass++;
        ctx->root = result;
    }

    return ctx->root;
}
