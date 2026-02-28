#include "text/strings/md_render.h"
#include "text/strings/style_ops.h"
#include "text/strings/string_ops.h"
#include "text/unicode/encoding.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include <string.h>
#include <assert.h>

// ===================================================================
// Style stack (same pattern as format.c)
// ===================================================================

#define MAX_STYLE_DEPTH 32

typedef struct {
    n00b_text_style_t *styles[MAX_STYLE_DEPTH];
    int                depth;
} style_stack_t;

static void
style_push(style_stack_t *ss, n00b_text_style_t *s)
{
    assert(ss->depth < MAX_STYLE_DEPTH);
    ss->styles[ss->depth++] = s;
}

static void
style_pop(style_stack_t *ss)
{
    if (ss->depth > 0) {
        ss->depth--;
    }
}

static n00b_text_style_t *
style_current(style_stack_t *ss)
{
    if (ss->depth == 0) {
        return nullptr;
    }

    n00b_text_style_t *acc = n00b_str_style_copy(ss->styles[0]);
    for (int i = 1; i < ss->depth; i++) {
        n00b_text_style_t *merged = n00b_str_style_merge(acc, ss->styles[i]);
        n00b_free(acc);
        acc = merged;
    }
    return acc;
}

// ===================================================================
// Output buffer
// ===================================================================

typedef struct {
    char   *buf;
    int32_t len;
    int32_t cap;
} outbuf_t;

static void
outbuf_append(outbuf_t *ob, const char *data, int32_t data_len)
{
    if (data_len <= 0) {
        return;
    }
    if (ob->len + data_len > ob->cap) {
        int32_t new_cap = ob->cap ? ob->cap * 2 : 256;
        while (new_cap < ob->len + data_len) {
            new_cap *= 2;
        }
        char *new_buf = n00b_alloc_array(char, new_cap);
        if (ob->buf) {
            memcpy(new_buf, ob->buf, ob->len);
            n00b_free(ob->buf);
        }
        ob->buf = new_buf;
        ob->cap = new_cap;
    }
    memcpy(ob->buf + ob->len, data, data_len);
    ob->len += data_len;
}

// ===================================================================
// Style record collector
// ===================================================================

typedef struct {
    n00b_style_record_t *records;
    int32_t              count;
    int32_t              cap;
} rec_list_t;

static void
rec_push(rec_list_t *rl, n00b_text_style_t *style, int32_t start)
{
    if (rl->count >= rl->cap) {
        int32_t new_cap            = rl->cap ? rl->cap * 2 : 16;
        n00b_style_record_t *new_r = n00b_alloc_array(n00b_style_record_t,
                                                       new_cap);
        if (rl->records) {
            memcpy(new_r, rl->records,
                   rl->count * sizeof(n00b_style_record_t));
            n00b_free(rl->records);
        }
        rl->records = new_r;
        rl->cap     = new_cap;
    }

    rl->records[rl->count++] = (n00b_style_record_t){
        .info  = style,
        .start = (size_t)start,
        .end   = n00b_option_none(size_t),
    };
}

// ===================================================================
// Render context
// ===================================================================

typedef struct {
    outbuf_t      ob;
    rec_list_t    rl;
    style_stack_t ss;
} render_ctx_t;

static void
emit_text(render_ctx_t *ctx, const char *text, int32_t text_len)
{
    if (text_len <= 0) {
        return;
    }

    n00b_text_style_t *cur = style_current(&ctx->ss);
    int32_t start          = ctx->ob.len;
    outbuf_append(&ctx->ob, text, text_len);

    if (cur && !n00b_str_style_is_empty(cur)) {
        rec_push(&ctx->rl, cur, start);
        ctx->rl.records[ctx->rl.count - 1].end =
            n00b_option_set(size_t, (size_t)ctx->ob.len);
    }
    else if (cur) {
        n00b_free(cur);
    }
}

// ===================================================================
// Node-kind → style mapping
// ===================================================================

static n00b_text_style_t *
style_for_span(n00b_md_node_kind_t kind)
{
    n00b_text_style_t *s = n00b_str_style_new();

    switch (kind) {
    case N00B_MD_SPAN_STRONG:
        s->bold = N00B_TRI_YES;
        break;
    case N00B_MD_SPAN_EM:
        s->italic = N00B_TRI_YES;
        break;
    case N00B_MD_SPAN_CODE:
        s->font_hint = N00B_FONT_MONO;
        break;
    case N00B_MD_SPAN_STRIKETHRU:
        s->strikethrough = N00B_TRI_YES;
        break;
    case N00B_MD_SPAN_U:
        s->underline = N00B_TRI_YES;
        break;
    default:
        break;
    }

    return s;
}

static n00b_text_style_t *
style_for_block(n00b_md_node_kind_t kind)
{
    n00b_text_style_t *s = n00b_str_style_new();

    switch (kind) {
    case N00B_MD_BLOCK_H:
        s->bold = N00B_TRI_YES;
        break;
    case N00B_MD_BLOCK_CODE:
        s->font_hint = N00B_FONT_MONO;
        break;
    default:
        // No special style for other blocks.
        n00b_free(s);
        return nullptr;
    }

    return s;
}

// ===================================================================
// Recursive tree walk
// ===================================================================

static void
render_node(render_ctx_t *ctx,
            n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *t)
{
    if (!t) {
        return;
    }

    // Text nodes are leaves in the tree.
    if (n00b_tree_is_leaf(t)) {
        n00b_md_node_t *val = &n00b_tree_leaf_value(t);

        if (val->node_type >= N00B_MD_TEXT_NORMAL
            && val->node_type <= N00B_MD_TEXT_LATEX) {
            n00b_string_t *text = val->detail.text;
            if (text && text->data && text->u8_bytes > 0) {
                emit_text(ctx, text->data, (int32_t)text->u8_bytes);
            }
        }
        return;
    }

    n00b_md_node_t       *val  = &n00b_tree_node_value(t);
    n00b_md_node_kind_t   kind = val->node_type;
    n00b_text_style_t    *pushed_style = nullptr;

    // Determine if this node pushes a style.
    if (kind >= N00B_MD_SPAN_EM && kind <= N00B_MD_SPAN_U) {
        pushed_style = style_for_span(kind);
    }
    else if (kind == N00B_MD_BLOCK_H || kind == N00B_MD_BLOCK_CODE) {
        pushed_style = style_for_block(kind);
    }

    if (pushed_style) {
        style_push(&ctx->ss, pushed_style);
    }

    // Recurse into children.
    size_t nc = n00b_tree_num_children(t);
    for (size_t i = 0; i < nc; i++) {
        render_node(ctx, n00b_tree_child(t, i));
    }

    if (pushed_style) {
        style_pop(&ctx->ss);
    }
}

// ===================================================================
// Public API
// ===================================================================

n00b_string_t *
n00b_str_md_render(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *tree)
{
    render_ctx_t ctx = {};

    render_node(&ctx, tree);

    // Build result string.
    int64_t total_cps = 0;
    if (ctx.ob.len > 0) {
        total_cps = n00b_unicode_utf8_count_codepoints_raw(ctx.ob.buf,
                                                            ctx.ob.len);
        if (total_cps < 0) {
            total_cps = 0;
        }
    }

    n00b_string_t *result = n00b_string_from_raw(ctx.ob.buf, ctx.ob.len);

    // Attach style info if we have records.
    if (ctx.rl.count > 0) {
        n00b_string_style_info_t *info =
            n00b_alloc_flex(n00b_string_style_info_t, n00b_style_record_t,
                            ctx.rl.count);
        info->num_styles = ctx.rl.count;
        memcpy(info->styles, ctx.rl.records,
               ctx.rl.count * sizeof(n00b_style_record_t));
        result->styling = info;
    }

    if (ctx.ob.buf) {
        n00b_free(ctx.ob.buf);
    }
    if (ctx.rl.records) {
        n00b_free(ctx.rl.records);
    }

    return result;
}
