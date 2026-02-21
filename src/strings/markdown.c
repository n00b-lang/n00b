#include "strings/markdown.h"
#include "core/alloc.h"
#include "core/string.h"
#include "unicode/encoding.h"
#include "internal/unicode/raw.h"
#include "vendor/md4c.h"

#include <string.h>

// -------------------------------------------------------------------
// Convenience macros matching the old code's enum offsetting
// -------------------------------------------------------------------

#define convert_block_kind(x) (n00b_md_node_kind_t)(x)
#define convert_span_kind(x)  (n00b_md_node_kind_t)((x) + (int)N00B_MD_SPAN_EM)
#define convert_text_kind(x)  (n00b_md_node_kind_t)((x) + (int)N00B_MD_TEXT_NORMAL)

#define N00B_MD_GITHUB MD_DIALECT_GITHUB

// Max nesting depth for markdown elements.
#define MD_MAX_DEPTH 64

// -------------------------------------------------------------------
// Build context — tracks current position in tree with explicit stack
// -------------------------------------------------------------------

typedef struct {
    n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *cur;
    n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *stack[MD_MAX_DEPTH];
    int                                            depth;
    n00b_allocator_t                              *allocator;
} md_build_ctx;

// Push current node onto stack, set cur to child.
static inline void
md_push(md_build_ctx *ctx,
        n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *child)
{
    if (ctx->depth < MD_MAX_DEPTH) {
        ctx->stack[ctx->depth++] = ctx->cur;
    }
    ctx->cur = child;
}

// Pop from stack to restore parent.
static inline void
md_pop(md_build_ctx *ctx)
{
    if (ctx->depth > 0) {
        ctx->cur = ctx->stack[--ctx->depth];
    }
}

// -------------------------------------------------------------------
// md4c callbacks
// -------------------------------------------------------------------

static int
enter_md_block(MD_BLOCKTYPE type, void *detail, void *extra)
{
    md_build_ctx   *ctx  = (md_build_ctx *)extra;
    n00b_md_node_t  node = {};

    node.node_type = convert_block_kind(type);

    // Copy block detail into opaque storage.
    switch (type) {
    case MD_BLOCK_UL:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_UL_DETAIL));
        break;
    case MD_BLOCK_OL:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_OL_DETAIL));
        break;
    case MD_BLOCK_LI:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_LI_DETAIL));
        break;
    case MD_BLOCK_H:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_H_DETAIL));
        break;
    case MD_BLOCK_CODE:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_CODE_DETAIL));
        break;
    case MD_BLOCK_TABLE:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_TABLE_DETAIL));
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        memcpy(node.detail.raw, detail, sizeof(MD_BLOCK_TD_DETAIL));
        break;
    default:
        break;
    }

    auto child = n00b_tree_add_node(ctx->cur, n00b_md_node_t, n00b_md_node_t,
                                    node);
    md_push(ctx, child);

    return 0;
}

static int
exit_md_block(MD_BLOCKTYPE type, void *detail, void *extra)
{
    md_build_ctx *ctx = (md_build_ctx *)extra;
    md_pop(ctx);
    return 0;
}

static int
enter_md_span(MD_SPANTYPE type, void *detail, void *extra)
{
    md_build_ctx   *ctx  = (md_build_ctx *)extra;
    n00b_md_node_t  node = {};

    node.node_type = convert_span_kind(type);

    switch (type) {
    case MD_SPAN_A:
    case MD_SPAN_A_CODELINK:
    case MD_SPAN_A_SELF:
        memcpy(node.detail.raw, detail, sizeof(MD_SPAN_A_DETAIL));
        break;
    case MD_SPAN_IMG:
        memcpy(node.detail.raw, detail, sizeof(MD_SPAN_IMG_DETAIL));
        break;
    case MD_SPAN_WIKILINK:
        memcpy(node.detail.raw, detail, sizeof(MD_SPAN_WIKILINK_DETAIL));
        break;
    default:
        break;
    }

    auto child = n00b_tree_add_node(ctx->cur, n00b_md_node_t, n00b_md_node_t,
                                    node);
    md_push(ctx, child);

    return 0;
}

static int
exit_md_span(MD_SPANTYPE type, void *detail, void *extra)
{
    md_build_ctx *ctx = (md_build_ctx *)extra;
    md_pop(ctx);
    return 0;
}

static int
md_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *extra)
{
    md_build_ctx   *ctx  = (md_build_ctx *)extra;
    n00b_md_node_t  node = {};

    node.node_type  = convert_text_kind(type);

    node.detail.text = n00b_string_from_raw(text, size, .allocator = ctx->allocator);

    // Text nodes are leaves — they never have children.
    (void)n00b_tree_add_leaf(ctx->cur, n00b_md_node_t, n00b_md_node_t, node);

    return 0;
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *
n00b_parse_markdown(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    MD_PARSER parser = {
        0,
        N00B_MD_GITHUB,
        enter_md_block,
        exit_md_block,
        enter_md_span,
        exit_md_span,
        md_text,
        nullptr,
        nullptr,
    };

    n00b_md_node_t root_val = {
        .node_type = N00B_MD_DOCUMENT,
        .detail    = {},
    };

    auto t = n00b_tree_node(n00b_md_node_t, n00b_md_node_t, root_val);

    md_build_ctx build_ctx = {
        .cur       = t,
        .depth     = 0,
        .allocator = allocator,
    };

    md_parse(s.data, (unsigned)s.u8_bytes, &parser, (void *)&build_ctx);

    return t;
}
