// cfg.c — Control flow graph construction from annotated parse trees.
//
// Walks a parse tree using the CF labels produced by the annotation walk.
// For each labeled node, the builder creates basic blocks and edges
// corresponding to the control flow pattern (branch, loop, switch, jump).

#include "slay/cfg.h"
#include "slay/cf_label.h"
#include "slay/symtab.h"
#include "core/alloc.h"
#include "adt/tree.h"
#include "text/strings/string_ops.h"

#include <assert.h>

// ============================================================================
// Loop stack — tracks break/continue targets
// ============================================================================

#define MAX_LOOP_DEPTH 64

typedef struct {
    int32_t header_id;
    int32_t exit_id;
} loop_info_t;

typedef struct {
    loop_info_t items[MAX_LOOP_DEPTH];
    int         top;
} loop_stack_t;

static void
loop_push(loop_stack_t *ls, int32_t header, int32_t exit_id)
{
    assert(ls->top < MAX_LOOP_DEPTH);
    ls->items[ls->top++] = (loop_info_t){header, exit_id};
}

static void
loop_pop(loop_stack_t *ls)
{
    assert(ls->top > 0);
    ls->top--;
}

// ============================================================================
// Builder context
// ============================================================================

typedef struct {
    n00b_cfg_t       *cfg;
    n00b_cf_labels_t *cf_labels;
    n00b_symtab_t    *symtab;
    loop_stack_t      loops;
} cfg_ctx_t;

// ============================================================================
// Block + edge helpers
// ============================================================================

static int32_t
new_block(cfg_ctx_t *ctx, n00b_string_t label)
{
    n00b_cfg_block_t blk = {
        .id    = (int32_t)n00b_list_len(ctx->cfg->blocks),
        .label = label,
        .stmts = n00b_list_new_private(n00b_parse_tree_ptr_t),
    };

    n00b_list_push(ctx->cfg->blocks, blk);

    return blk.id;
}

static void
add_edge(cfg_ctx_t *ctx, int32_t from, int32_t to,
         n00b_cfg_edge_kind_t kind, n00b_string_t label)
{
    n00b_cfg_edge_t e = {
        .from_id = from,
        .to_id   = to,
        .kind    = kind,
        .label   = label,
    };

    n00b_list_push(ctx->cfg->edges, e);
}

static void
block_add_stmt(cfg_ctx_t *ctx, int32_t block_id, n00b_parse_tree_t *node)
{
    n00b_cfg_block_t *blk = &ctx->cfg->blocks.data[block_id];
    n00b_list_push(blk->stmts, node);
}

// ============================================================================
// Label lookup helper
// ============================================================================

static n00b_cf_label_t *
lookup(cfg_ctx_t *ctx, n00b_parse_tree_t *node)
{
    return n00b_cf_label_lookup(ctx->cf_labels, node);
}

// ============================================================================
// Forward declarations
// ============================================================================

static int32_t cfg_build_stmt(cfg_ctx_t *ctx, n00b_parse_tree_t *node,
                               int32_t cur_block);
static int32_t cfg_build_stmts(cfg_ctx_t *ctx, n00b_parse_tree_t *node,
                                int32_t cur_block);

// ============================================================================
// Collect non-group children (flatten $$group wrappers)
// ============================================================================

#define MAX_CASE_ARMS 128

static int
collect_case_children(n00b_parse_tree_t *node, n00b_parse_tree_t **out, int max)
{
    if (!node || n00b_tree_is_leaf(node) || max <= 0) {
        return 0;
    }

    int    count = 0;
    size_t nc    = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc && count < max; i++) {
        n00b_parse_tree_t *child = n00b_tree_child(node, i);

        if (n00b_pt_is_group(child)) {
            count += collect_case_children(child, out + count, max - count);
        }
        else if (!n00b_tree_is_leaf(child)) {
            out[count++] = child;
        }
    }

    return count;
}

// ============================================================================
// Check if a block ends with a jump (used to avoid double edges)
// ============================================================================

static bool
block_ends_with_jump(cfg_ctx_t *ctx, int32_t block_id)
{
    size_t ne = n00b_list_len(ctx->cfg->edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_cfg_edge_t *e = &ctx->cfg->edges.data[i];

        if (e->from_id == block_id && e->kind == N00B_CFG_JUMP) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Branch (if/else, ternary)
// ============================================================================

static int32_t
cfg_build_branch(cfg_ctx_t *ctx, n00b_cf_label_t *label, int32_t cur_block)
{
    // Add the condition as a statement in the current block.
    if (label->cond) {
        block_add_stmt(ctx, cur_block, label->cond);
    }

    int32_t merge = new_block(ctx, *r"merge");

    // Then-arm.
    int32_t then_blk = new_block(ctx, *r"then");
    add_edge(ctx, cur_block, then_blk, N00B_CFG_BRANCH_TRUE, n00b_string_empty());

    int32_t then_end = then_blk;

    if (label->then_body) {
        then_end = cfg_build_stmts(ctx, label->then_body, then_blk);
    }

    if (!block_ends_with_jump(ctx, then_end)) {
        add_edge(ctx, then_end, merge, N00B_CFG_FALLTHROUGH, n00b_string_empty());
    }

    // Else-arm (optional).
    if (label->else_body) {
        int32_t else_blk = new_block(ctx, *r"else");
        add_edge(ctx, cur_block, else_blk, N00B_CFG_BRANCH_FALSE, n00b_string_empty());

        int32_t else_end = cfg_build_stmts(ctx, label->else_body, else_blk);

        if (!block_ends_with_jump(ctx, else_end)) {
            add_edge(ctx, else_end, merge, N00B_CFG_FALLTHROUGH, n00b_string_empty());
        }
    }
    else {
        // No else — false branch goes straight to merge.
        add_edge(ctx, cur_block, merge, N00B_CFG_BRANCH_FALSE, n00b_string_empty());
    }

    return merge;
}

// ============================================================================
// Loop (while, for, do-while)
// ============================================================================

static int32_t
cfg_build_loop(cfg_ctx_t *ctx, n00b_cf_label_t *label, int32_t cur_block)
{
    int32_t header = new_block(ctx, *r"loop_header");
    int32_t body   = new_block(ctx, *r"loop_body");
    int32_t exit   = new_block(ctx, *r"loop_exit");

    // Current block falls through to loop header.
    add_edge(ctx, cur_block, header, N00B_CFG_FALLTHROUGH, n00b_string_empty());

    // Condition in header.
    if (label->cond) {
        block_add_stmt(ctx, header, label->cond);
    }

    // Header -> body (true), header -> exit (false).
    add_edge(ctx, header, body, N00B_CFG_BRANCH_TRUE, n00b_string_empty());
    add_edge(ctx, header, exit, N00B_CFG_LOOP_EXIT, n00b_string_empty());

    // Build body with loop stack.
    loop_push(&ctx->loops, header, exit);

    int32_t body_end = body;

    if (label->then_body) {
        body_end = cfg_build_stmts(ctx, label->then_body, body);
    }

    loop_pop(&ctx->loops);

    // Back-edge: body end -> header.
    if (!block_ends_with_jump(ctx, body_end)) {
        add_edge(ctx, body_end, header, N00B_CFG_LOOP_BACK, n00b_string_empty());
    }

    return exit;
}

// ============================================================================
// Switch
// ============================================================================

static int32_t
cfg_build_switch(cfg_ctx_t *ctx, n00b_cf_label_t *label, int32_t cur_block)
{
    if (label->cond) {
        block_add_stmt(ctx, cur_block, label->cond);
    }

    int32_t merge = new_block(ctx, *r"switch_merge");

    // The then_body is the cases container.  Flatten $$group wrappers
    // to find the actual case-block / case-else nodes — the grammar's
    // EBNF quantifiers (e.g. (%"case" <block>)+) wrap cases in groups.
    if (!label->then_body || n00b_tree_is_leaf(label->then_body)) {
        add_edge(ctx, cur_block, merge, N00B_CFG_FALLTHROUGH, n00b_string_empty());
        return merge;
    }

    n00b_parse_tree_t *arms[MAX_CASE_ARMS];
    int                narms = collect_case_children(label->then_body,
                                                     arms, MAX_CASE_ARMS);

    if (narms == 0) {
        add_edge(ctx, cur_block, merge, N00B_CFG_FALLTHROUGH, n00b_string_empty());
        return merge;
    }

    for (int i = 0; i < narms; i++) {
        int32_t case_blk = new_block(ctx, *r"case");
        add_edge(ctx, cur_block, case_blk, N00B_CFG_CASE_BRANCH, n00b_string_empty());

        int32_t case_end = cfg_build_stmts(ctx, arms[i], case_blk);

        if (!block_ends_with_jump(ctx, case_end)) {
            add_edge(ctx, case_end, merge, N00B_CFG_FALLTHROUGH, n00b_string_empty());
        }
    }

    return merge;
}

// ============================================================================
// Jump (break, continue, return, goto)
// ============================================================================

static int32_t
cfg_build_jump(cfg_ctx_t *ctx, n00b_cf_label_t *label, int32_t cur_block)
{
    block_add_stmt(ctx, cur_block, label->self);

    int32_t target;

    if (label->jump_kind.data && ctx->loops.top > 0) {
        loop_info_t *loop = &ctx->loops.items[ctx->loops.top - 1];

        if (n00b_unicode_str_starts_with(label->jump_kind, *r"break")) {
            target = loop->exit_id;
        }
        else if (n00b_unicode_str_starts_with(label->jump_kind, *r"continue")) {
            target = loop->header_id;
        }
        else {
            // return, goto — go to function exit.
            target = ctx->cfg->exit_id;
        }
    }
    else {
        // No loop context or unknown — go to function exit.
        target = ctx->cfg->exit_id;
    }

    add_edge(ctx, cur_block, target, N00B_CFG_JUMP, label->jump_kind);

    // Code after a jump is unreachable — start a new dead block.
    return new_block(ctx, *r"unreachable");
}

// ============================================================================
// Statement dispatch
// ============================================================================

static int32_t
cfg_build_stmt(cfg_ctx_t *ctx, n00b_parse_tree_t *node, int32_t cur_block)
{
    if (!node) {
        return cur_block;
    }

    n00b_cf_label_t *label = lookup(ctx, node);

    if (label) {
        switch (label->kind) {
        case N00B_CF_BRANCH:
            return cfg_build_branch(ctx, label, cur_block);
        case N00B_CF_LOOP:
            return cfg_build_loop(ctx, label, cur_block);
        case N00B_CF_SWITCH:
            return cfg_build_switch(ctx, label, cur_block);
        case N00B_CF_JUMP:
            return cfg_build_jump(ctx, label, cur_block);
        case N00B_CF_ASSIGNS:
        case N00B_CF_VARREF:
        case N00B_CF_CAPTURE:
        case N00B_CF_UNWRAP_RESULT:
            // Treat as a regular statement for CFG purposes.
            block_add_stmt(ctx, cur_block, node);
            return cur_block;
        }
    }

    // No label — transparent wrapper. Recurse into children.
    if (n00b_tree_is_leaf(node)) {
        // Leaf with no label — add as a statement.
        block_add_stmt(ctx, cur_block, node);
        return cur_block;
    }

    // Function definitions get their own CFG — don't inline their body
    // into the enclosing CFG.  Detected via @scope("function", ...) tag
    // stored during the annotation walk (language-independent).
    n00b_nt_node_t *nt = &n00b_tree_node_value(node);

    if (nt->scope && nt->scope->scope_tag.u8_bytes > 0
        && n00b_unicode_str_eq(nt->scope->scope_tag, *r"function")) {
        // Find the body child (last non-leaf NT child).
        n00b_parse_tree_t *body = NULL;
        size_t nc = n00b_tree_num_children(node);

        for (size_t i = nc; i > 0; i--) {
            n00b_parse_tree_t *child = n00b_tree_child(node, i - 1);

            if (!n00b_tree_is_leaf(child)) {
                body = child;
                break;
            }
        }

        // Get function name from the scope attached to this node.
        n00b_string_t func_name = nt->scope->name;

        if (body) {
            // Build a separate CFG for the function body.
            n00b_cfg_t *func_cfg = n00b_build_cfg(ctx->cf_labels, body,
                                                    func_name,
                                                    ctx->symtab);

            // Store the CFG on the function's symbol entry.
            if (func_cfg && ctx->symtab) {
                n00b_sym_entry_t *sym = n00b_symtab_lookup_all(
                    ctx->symtab, n00b_string_empty(), func_name);

                if (sym && sym->kind == N00B_SYM_FUNCTION) {
                    sym->cfg = func_cfg;
                }
            }
        }

        block_add_stmt(ctx, cur_block, node);
        return cur_block;
    }

    return cfg_build_stmts(ctx, node, cur_block);
}

// ============================================================================
// Statement list (iterate children of a non-terminal)
// ============================================================================

static int32_t
cfg_build_stmts(cfg_ctx_t *ctx, n00b_parse_tree_t *node, int32_t cur_block)
{
    if (!node || n00b_tree_is_leaf(node)) {
        return cur_block;
    }

    size_t nc = n00b_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        cur_block = cfg_build_stmt(ctx, n00b_tree_child(node, i), cur_block);
    }

    return cur_block;
}

// ============================================================================
// Public API — build
// ============================================================================

n00b_cfg_t *
n00b_build_cfg(n00b_cf_labels_t *cf_labels,
               n00b_parse_tree_t   *func_body,
               n00b_string_t        func_name,
               n00b_symtab_t       *symtab)
{
    if (!cf_labels || !func_body) {
        return NULL;
    }

    n00b_cfg_t *cfg = n00b_alloc(n00b_cfg_t);
    cfg->blocks = n00b_list_new_private(n00b_cfg_block_t);
    cfg->edges  = n00b_list_new_private(n00b_cfg_edge_t);
    cfg->name   = func_name;

    cfg_ctx_t ctx = {
        .cfg       = cfg,
        .cf_labels = cf_labels,
        .symtab    = symtab,
        .loops     = {.top = 0},
    };

    // Create entry and exit blocks.
    cfg->entry_id = new_block(&ctx, *r"entry");
    cfg->exit_id  = new_block(&ctx, *r"exit");

    cfg->blocks.data[cfg->entry_id].is_entry = true;
    cfg->blocks.data[cfg->exit_id].is_exit   = true;

    // Build the body starting from the entry block.
    int32_t last = cfg_build_stmts(&ctx, func_body, cfg->entry_id);

    // Connect the last block to exit if it doesn't already jump.
    if (!block_ends_with_jump(&ctx, last) && last != cfg->exit_id) {
        add_edge(&ctx, last, cfg->exit_id, N00B_CFG_FALLTHROUGH, n00b_string_empty());
    }

    return cfg;
}

// ============================================================================
// Public API — query
// ============================================================================

n00b_list_t(n00b_cfg_edge_t)
n00b_cfg_successors(n00b_cfg_t *cfg, int32_t block_id)
{
    n00b_list_t(n00b_cfg_edge_t) result = n00b_list_new_private(n00b_cfg_edge_t);

    if (!cfg) {
        return result;
    }

    size_t ne = n00b_list_len(cfg->edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_cfg_edge_t *e = &cfg->edges.data[i];

        if (e->from_id == block_id) {
            n00b_list_push(result, *e);
        }
    }

    return result;
}

n00b_list_t(n00b_cfg_edge_t)
n00b_cfg_predecessors(n00b_cfg_t *cfg, int32_t block_id)
{
    n00b_list_t(n00b_cfg_edge_t) result = n00b_list_new_private(n00b_cfg_edge_t);

    if (!cfg) {
        return result;
    }

    size_t ne = n00b_list_len(cfg->edges);

    for (size_t i = 0; i < ne; i++) {
        n00b_cfg_edge_t *e = &cfg->edges.data[i];

        if (e->to_id == block_id) {
            n00b_list_push(result, *e);
        }
    }

    return result;
}

// ============================================================================
// Cleanup
// ============================================================================

void
n00b_cfg_free(n00b_cfg_t *cfg)
{
    if (!cfg) {
        return;
    }

    // Free per-block statement lists.
    size_t nb = n00b_list_len(cfg->blocks);

    for (size_t i = 0; i < nb; i++) {
        n00b_list_free(cfg->blocks.data[i].stmts);
    }

    n00b_list_free(cfg->blocks);
    n00b_list_free(cfg->edges);
    n00b_free(cfg);
}
