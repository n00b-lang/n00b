#include "text/strings/md_lines.h"
#include "text/strings/style_ops.h"
#include "text/strings/string_ops.h"
#include "text/unicode/encoding.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include <string.h>
#include <assert.h>

// ===================================================================
// Style helpers (same pattern as md_render.c)
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
// Output buffer + style records for building a single line
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

typedef struct {
    n00b_style_record_t *records;
    int32_t              count;
    int32_t              cap;
} rec_list_t;

static void
rec_push(rec_list_t *rl, n00b_text_style_t *style, int32_t start, int32_t end)
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
        .end   = n00b_option_set(size_t, (size_t)end),
    };
}

// ===================================================================
// Line context — accumulates text + styles for one logical line
// ===================================================================

typedef struct {
    outbuf_t      ob;
    rec_list_t    rl;
    style_stack_t ss;
} line_ctx_t;

static void
line_emit_text(line_ctx_t *lc, const char *text, int32_t text_len)
{
    if (text_len <= 0) {
        return;
    }

    n00b_text_style_t *cur = style_current(&lc->ss);
    int32_t start          = lc->ob.len;
    outbuf_append(&lc->ob, text, text_len);

    if (cur && !n00b_str_style_is_empty(cur)) {
        rec_push(&lc->rl, cur, start, lc->ob.len);
    }
    else if (cur) {
        n00b_free(cur);
    }
}

static n00b_string_t *
line_ctx_to_string(line_ctx_t *lc)
{
    int64_t total_cps = 0;
    if (lc->ob.len > 0) {
        total_cps = n00b_unicode_utf8_count_codepoints_raw(lc->ob.buf,
                                                            lc->ob.len);
        if (total_cps < 0) {
            total_cps = 0;
        }
    }

    n00b_string_t *result = n00b_string_from_raw(lc->ob.buf, lc->ob.len);

    if (lc->rl.count > 0) {
        n00b_string_style_info_t *info =
            n00b_alloc_flex(n00b_string_style_info_t, n00b_style_record_t,
                            lc->rl.count);
        info->num_styles = lc->rl.count;
        memcpy(info->styles, lc->rl.records,
               lc->rl.count * sizeof(n00b_style_record_t));
        result->styling = info;
    }

    return result;
}

static void
line_ctx_free(line_ctx_t *lc)
{
    if (lc->ob.buf) {
        n00b_free(lc->ob.buf);
    }
    if (lc->rl.records) {
        n00b_free(lc->rl.records);
    }
}

// ===================================================================
// Node-kind → style mapping
// ===================================================================

static n00b_text_style_t *
style_for_kind(n00b_md_node_kind_t kind)
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
    case N00B_MD_BLOCK_H:
        s->bold = N00B_TRI_YES;
        break;
    case N00B_MD_BLOCK_CODE:
        s->font_hint = N00B_FONT_MONO;
        break;
    default:
        n00b_free(s);
        return nullptr;
    }

    return s;
}

// ===================================================================
// Recursive inline renderer — collects text within a block
// ===================================================================

static void
render_inline(line_ctx_t *lc,
              n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *t)
{
    if (!t) {
        return;
    }

    if (n00b_tree_is_leaf(t)) {
        n00b_md_node_t *val = &n00b_tree_leaf_value(t);
        if (val->node_type >= N00B_MD_TEXT_NORMAL
            && val->node_type <= N00B_MD_TEXT_LATEX) {
            n00b_string_t *text = val->detail.text;
            if (text && text->data && text->u8_bytes > 0) {
                line_emit_text(lc, text->data, (int32_t)text->u8_bytes);
            }
        }
        return;
    }

    n00b_md_node_t      *val  = &n00b_tree_node_value(t);
    n00b_md_node_kind_t  kind = val->node_type;
    n00b_text_style_t   *pushed = style_for_kind(kind);

    if (pushed) {
        style_push(&lc->ss, pushed);
    }

    size_t nc = n00b_tree_num_children(t);
    for (size_t i = 0; i < nc; i++) {
        render_inline(lc, n00b_tree_child(t, i));
    }

    if (pushed) {
        style_pop(&lc->ss);
    }
}

// ===================================================================
// Block-level walker — emits one line per renderable unit
// ===================================================================

static void
walk_block(n00b_list_t(n00b_string_t *) *ll,
           n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return;
    }

    n00b_md_node_t      *val  = &n00b_tree_node_value(t);
    n00b_md_node_kind_t  kind = val->node_type;

    switch (kind) {
    case N00B_MD_BLOCK_P:
    case N00B_MD_BLOCK_H: {
        // Render the entire paragraph/heading as one styled line.
        line_ctx_t lc = {};

        if (kind == N00B_MD_BLOCK_H) {
            n00b_text_style_t *hs = n00b_str_style_new();
            hs->bold = N00B_TRI_YES;
            style_push(&lc.ss, hs);
        }

        size_t nc = n00b_tree_num_children(t);
        for (size_t i = 0; i < nc; i++) {
            render_inline(&lc, n00b_tree_child(t, i));
        }

        if (kind == N00B_MD_BLOCK_H) {
            style_pop(&lc.ss);
        }

        n00b_list_push(*ll, line_ctx_to_string(&lc));
        line_ctx_free(&lc);
        break;
    }

    case N00B_MD_BLOCK_HR: {
        n00b_string_t *hr = n00b_string_from_raw("---", 3);
        n00b_list_push(*ll, hr);
        break;
    }

    case N00B_MD_BLOCK_CODE: {
        // Code block → one line per source line, all with mono style.
        line_ctx_t lc = {};
        n00b_text_style_t *mono = n00b_str_style_new();
        mono->font_hint = N00B_FONT_MONO;
        style_push(&lc.ss, mono);

        size_t nc = n00b_tree_num_children(t);
        for (size_t i = 0; i < nc; i++) {
            render_inline(&lc, n00b_tree_child(t, i));
        }

        style_pop(&lc.ss);

        // Split by newlines.
        if (lc.ob.len > 0) {
            const char *start = lc.ob.buf;
            const char *end   = lc.ob.buf + lc.ob.len;

            while (start < end) {
                const char *nl = memchr(start, '\n', end - start);
                int32_t     line_len;

                if (nl) {
                    line_len = (int32_t)(nl - start);
                }
                else {
                    line_len = (int32_t)(end - start);
                }

                int64_t cps = n00b_unicode_utf8_count_codepoints_raw(
                    start, line_len);
                if (cps < 0) {
                    cps = 0;
                }
                n00b_string_t *line_str = n00b_string_from_raw(
                    start, line_len);

                // Apply mono style to the entire line.
                n00b_text_style_t *ls = n00b_str_style_new();
                ls->font_hint = N00B_FONT_MONO;
                line_str = n00b_str_add_style(
                    line_str, ls, 0,
                    n00b_option_set(size_t, (size_t)line_len));

                n00b_list_push(*ll, line_str);

                start = nl ? nl + 1 : end;
            }
        }

        line_ctx_free(&lc);
        break;
    }

    case N00B_MD_BLOCK_LI: {
        // List item → render inline content with bullet prefix.
        line_ctx_t lc = {};

        line_emit_text(&lc, "- ", 2);

        // render_inline recurses through P and span nodes to text.
        size_t nc = n00b_tree_num_children(t);
        for (size_t i = 0; i < nc; i++) {
            render_inline(&lc, n00b_tree_child(t, i));
        }

        n00b_list_push(*ll, line_ctx_to_string(&lc));
        line_ctx_free(&lc);
        break;
    }

    default: {
        // Container blocks (DOCUMENT, UL, OL, BLOCKQUOTE, TABLE, etc.)
        size_t nc = n00b_tree_num_children(t);
        for (size_t i = 0; i < nc; i++) {
            walk_block(ll, n00b_tree_child(t, i));
        }
        break;
    }
    }
}

// ===================================================================
// Public API
// ===================================================================

n00b_array_t(n00b_string_t *)
n00b_str_md_to_lines(n00b_tree_t(n00b_md_node_t, n00b_md_node_t) *tree)
{
    n00b_list_t(n00b_string_t *) tmp = n00b_list_new(n00b_string_t *);

    walk_block(&tmp, tree);

    return n00b_list_to_array(n00b_string_t *, tmp);
}
