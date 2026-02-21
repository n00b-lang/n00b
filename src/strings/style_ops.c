#include "strings/style_ops.h"
#include "core/alloc.h"

// ===================================================================
// Helpers
// ===================================================================

static inline n00b_tristate_t
merge_tri(n00b_tristate_t base, n00b_tristate_t overlay)
{
    return overlay != N00B_TRI_UNSPECIFIED ? overlay : base;
}

static inline int8_t
merge_i8(int8_t base, int8_t overlay)
{
    return overlay != -1 ? overlay : base;
}

static inline n00b_color_t
merge_color(n00b_color_t base, n00b_color_t overlay)
{
    return n00b_color_is_set(overlay) ? overlay : base;
}

// ===================================================================
// Construction
// ===================================================================

n00b_text_style_t *
n00b_str_style_new()
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    n00b_text_style_t *s = n00b_alloc_with_opts(n00b_text_style_t, &(n00b_alloc_opts_t){.allocator = allocator});

    s->bold             = N00B_TRI_UNSPECIFIED;
    s->italic           = N00B_TRI_UNSPECIFIED;
    s->underline        = N00B_TRI_UNSPECIFIED;
    s->double_underline = N00B_TRI_UNSPECIFIED;
    s->strikethrough    = N00B_TRI_UNSPECIFIED;
    s->reverse          = N00B_TRI_UNSPECIFIED;
    s->dim              = N00B_TRI_UNSPECIFIED;
    s->blink            = N00B_TRI_UNSPECIFIED;

    s->text_case        = N00B_TEXT_CASE_NONE;
    s->font_hint        = N00B_FONT_DEFAULT;
    s->font_index       = -1;
    s->fg_palette_ix    = -1;
    s->bg_palette_ix    = -1;
    s->fg_rgb           = 0;
    s->bg_rgb           = 0;

    return s;
}

// ===================================================================
// Merge / overlay
// ===================================================================

n00b_text_style_t *
n00b_str_style_merge(const n00b_text_style_t *base,
                      const n00b_text_style_t *overlay)
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    n00b_text_style_t *r = n00b_alloc_with_opts(n00b_text_style_t, &(n00b_alloc_opts_t){.allocator = allocator});

    r->bold             = merge_tri(base->bold, overlay->bold);
    r->italic           = merge_tri(base->italic, overlay->italic);
    r->underline        = merge_tri(base->underline, overlay->underline);
    r->double_underline = merge_tri(base->double_underline,
                                    overlay->double_underline);
    r->strikethrough    = merge_tri(base->strikethrough, overlay->strikethrough);
    r->reverse          = merge_tri(base->reverse, overlay->reverse);
    r->dim              = merge_tri(base->dim, overlay->dim);
    r->blink            = merge_tri(base->blink, overlay->blink);

    r->text_case = overlay->text_case != N00B_TEXT_CASE_NONE
                       ? overlay->text_case
                       : base->text_case;
    r->font_hint = overlay->font_hint != N00B_FONT_DEFAULT
                       ? overlay->font_hint
                       : base->font_hint;

    r->font_index    = merge_i8(base->font_index, overlay->font_index);
    r->fg_palette_ix = merge_i8(base->fg_palette_ix, overlay->fg_palette_ix);
    r->bg_palette_ix = merge_i8(base->bg_palette_ix, overlay->bg_palette_ix);

    r->fg_rgb = merge_color(base->fg_rgb, overlay->fg_rgb);
    r->bg_rgb = merge_color(base->bg_rgb, overlay->bg_rgb);

    return r;
}

// ===================================================================
// Comparison
// ===================================================================

bool
n00b_str_style_eq(const n00b_text_style_t *a, const n00b_text_style_t *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }

    return a->bold == b->bold && a->italic == b->italic
           && a->underline == b->underline
           && a->double_underline == b->double_underline
           && a->strikethrough == b->strikethrough
           && a->reverse == b->reverse && a->dim == b->dim
           && a->blink == b->blink && a->text_case == b->text_case
           && a->font_hint == b->font_hint
           && a->font_index == b->font_index
           && a->fg_palette_ix == b->fg_palette_ix
           && a->bg_palette_ix == b->bg_palette_ix
           && a->fg_rgb == b->fg_rgb && a->bg_rgb == b->bg_rgb;
}

// ===================================================================
// Copy
// ===================================================================

n00b_text_style_t *
n00b_str_style_copy(const n00b_text_style_t *src)
    _kargs { n00b_allocator_t *allocator = nullptr; }
{
    n00b_text_style_t *dst = n00b_alloc_with_opts(n00b_text_style_t,
                                                   &(n00b_alloc_opts_t){.allocator = allocator});
    *dst = *src;
    return dst;
}

// ===================================================================
// Query
// ===================================================================

bool
n00b_str_style_is_empty(const n00b_text_style_t *s)
{
    if (!s) {
        return true;
    }

    return s->bold == N00B_TRI_UNSPECIFIED
           && s->italic == N00B_TRI_UNSPECIFIED
           && s->underline == N00B_TRI_UNSPECIFIED
           && s->double_underline == N00B_TRI_UNSPECIFIED
           && s->strikethrough == N00B_TRI_UNSPECIFIED
           && s->reverse == N00B_TRI_UNSPECIFIED
           && s->dim == N00B_TRI_UNSPECIFIED
           && s->blink == N00B_TRI_UNSPECIFIED
           && s->text_case == N00B_TEXT_CASE_NONE
           && s->font_hint == N00B_FONT_DEFAULT && s->font_index == -1
           && s->fg_palette_ix == -1 && s->bg_palette_ix == -1
           && !n00b_color_is_set(s->fg_rgb)
           && !n00b_color_is_set(s->bg_rgb);
}
