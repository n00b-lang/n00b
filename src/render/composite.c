/**
 * Compositing pipeline: flatten, z-sort, composite, degrade.
 */

#include <limits.h>
#include "n00b.h"
#include "core/alloc.h"
#include "render/composite.h"
#include "render/box.h"

// -------------------------------------------------------------------
// Internal: flatten recursion
// -------------------------------------------------------------------

typedef struct {
    n00b_composite_entry_t *entries;
    n00b_isize_t            count;
    n00b_isize_t            capacity;
} flatten_ctx_t;

static void
flatten_grow(flatten_ctx_t *ctx)
{
    n00b_isize_t new_cap = ctx->capacity ? ctx->capacity * 2 : 16;
    n00b_composite_entry_t *new_arr = n00b_alloc_array(
        n00b_composite_entry_t, new_cap, .no_scan = true);

    if (ctx->entries) {
        memcpy(new_arr, ctx->entries,
               ctx->count * sizeof(n00b_composite_entry_t));
        n00b_free(ctx->entries);
    }

    ctx->entries  = new_arr;
    ctx->capacity = new_cap;
}

static inline int32_t
imax32(int32_t a, int32_t b) { return a > b ? a : b; }

static inline int32_t
imin32(int32_t a, int32_t b) { return a < b ? a : b; }

static void
flatten_recurse(flatten_ctx_t *ctx, n00b_plane_t *p,
                int32_t parent_x, int32_t parent_y, int32_t parent_z,
                int32_t clip_x, int32_t clip_y,
                int32_t clip_w, int32_t clip_h)
{
    if (!(p->flags & N00B_PLANE_VISIBLE)) {
        return;
    }

    int32_t abs_x = parent_x + p->x;
    int32_t abs_y = parent_y + p->y;
    int32_t abs_z = parent_z + p->z;

    // Intersect this plane's extent with the parent clip rectangle.
    int32_t plane_w = (int32_t)p->vp_cols;
    int32_t plane_h = (int32_t)p->vp_rows;

    // Account for box insets.
    if (p->box) {
        n00b_isize_t it, ib, il, ir;
        n00b_box_insets(p->box, &it, &ib, &il, &ir);
        plane_w += (int32_t)(il + ir);
        plane_h += (int32_t)(it + ib);

        // Account for margins too.
        plane_w += p->box->margin_left + p->box->margin_right;
        plane_h += p->box->margin_top + p->box->margin_bottom;
    }

    int32_t my_clip_x = imax32(clip_x, abs_x);
    int32_t my_clip_y = imax32(clip_y, abs_y);
    int32_t my_clip_r = imin32(clip_x + clip_w, abs_x + plane_w);
    int32_t my_clip_b = imin32(clip_y + clip_h, abs_y + plane_h);
    int32_t my_clip_w = imax32(0, my_clip_r - my_clip_x);
    int32_t my_clip_h = imax32(0, my_clip_b - my_clip_y);

    if (my_clip_w == 0 || my_clip_h == 0) {
        return; // Fully clipped by parent.
    }

    if (ctx->count >= ctx->capacity) {
        flatten_grow(ctx);
    }

    ctx->entries[ctx->count++] = (n00b_composite_entry_t){
        .plane  = p,
        .abs_x  = abs_x,
        .abs_y  = abs_y,
        .abs_z  = abs_z,
        .clip_x = my_clip_x,
        .clip_y = my_clip_y,
        .clip_w = my_clip_w,
        .clip_h = my_clip_h,
    };

    for (n00b_isize_t i = 0; i < p->num_children; i++) {
        flatten_recurse(ctx, p->children[i], abs_x, abs_y, abs_z,
                        my_clip_x, my_clip_y, my_clip_w, my_clip_h);
    }
}

static int
compare_z(const void *a, const void *b)
{
    const n00b_composite_entry_t *ea = a;
    const n00b_composite_entry_t *eb = b;

    if (ea->abs_z != eb->abs_z) {
        return ea->abs_z < eb->abs_z ? -1 : 1;
    }
    return 0;
}

// -------------------------------------------------------------------
// Public: flatten
// -------------------------------------------------------------------

n00b_array_t(n00b_composite_entry_t)
n00b_composite_flatten(n00b_plane_t **planes,
                        n00b_isize_t   num_planes)
{
    flatten_ctx_t ctx = {};

    // Top-level planes get an effectively unbounded clip rectangle.
    // The compositing loop will clip to the actual frame bounds.
    int32_t max_dim = INT32_MAX / 2;

    for (n00b_isize_t i = 0; i < num_planes; i++) {
        flatten_recurse(&ctx, planes[i], 0, 0, 0,
                        0, 0, max_dim, max_dim);
    }

    if (ctx.count > 1) {
        qsort(ctx.entries, ctx.count, sizeof(n00b_composite_entry_t), compare_z);
    }

    return (n00b_array_t(n00b_composite_entry_t)){
        .data = ctx.entries,
        .len  = (size_t)ctx.count,
        .cap  = (size_t)ctx.capacity,
    };
}

// -------------------------------------------------------------------
// Internal: resolve effective style for a plane's current state
// -------------------------------------------------------------------

static n00b_text_style_t *
resolve_style(n00b_plane_t *p, n00b_text_style_t *base_style,
              int style_field) // 0=text, 1=border, 2=fill
{
    if (!p->box || p->widget_state == N00B_WSTATE_NORMAL) {
        return base_style;
    }

    n00b_state_style_t *ss = p->box->state_styles[p->widget_state];
    if (!ss) {
        return base_style;
    }

    n00b_text_style_t *override = nullptr;
    switch (style_field) {
    case 0:
        override = ss->text_style;
        break;
    case 1:
        override = ss->border_style;
        break;
    case 2:
        override = ss->fill_style;
        break;
    }

    return override ? override : base_style;
}

// -------------------------------------------------------------------
// Public: composite
// -------------------------------------------------------------------

void
n00b_composite_render(const n00b_composite_entry_t *entries,
                       n00b_isize_t                  count,
                       n00b_rcell_t                 *frame,
                       n00b_isize_t                  frame_rows,
                       n00b_isize_t                  frame_cols,
                       n00b_text_style_t            *default_style)
{
    // Clear frame.
    for (n00b_isize_t i = 0; i < frame_rows * frame_cols; i++) {
        n00b_rcell_set_ascii(&frame[i], ' ', default_style);
        n00b_rcell_mark_clean(&frame[i]);
    }

    // Composite each plane (low-z first).
    for (n00b_isize_t e = 0; e < count; e++) {
        const n00b_composite_entry_t *entry = &entries[e];
        n00b_plane_t *p = entry->plane;

        if (!(p->flags & N00B_PLANE_VISIBLE)) {
            continue;
        }

        // Compute box insets.
        n00b_isize_t inset_top = 0, inset_bot = 0, inset_left = 0, inset_right = 0;
        if (p->box) {
            n00b_box_insets(p->box, &inset_top, &inset_bot, &inset_left, &inset_right);
        }

        // Compute where the plane's outer box starts in frame coords.
        int32_t margin_top  = p->box ? p->box->margin_top : 0;
        int32_t margin_left = p->box ? p->box->margin_left : 0;

        int32_t frame_origin_row = entry->abs_y + margin_top;
        int32_t frame_origin_col = entry->abs_x + margin_left;

        // Total outer dimensions.
        n00b_isize_t outer_rows = p->vp_rows + inset_top + inset_bot;
        n00b_isize_t outer_cols = p->vp_cols + inset_left + inset_right;

        // Stamp box decoration into frame.
        if (p->box && p->box->border_theme) {
            n00b_text_style_t *bs = resolve_style(p, p->box->border_style, 1);
            n00b_text_style_t *fs = resolve_style(p, p->box->fill_style, 2);

            if (frame_origin_row >= 0
                && frame_origin_col >= 0
                && frame_origin_row + (int32_t)outer_rows <= (int32_t)frame_rows
                && frame_origin_col + (int32_t)outer_cols <= (int32_t)frame_cols) {

                n00b_box_stamp(p->box, frame, frame_cols,
                               (n00b_isize_t)frame_origin_row,
                               (n00b_isize_t)frame_origin_col,
                               outer_rows, outer_cols, bs, fs);
            }
        }

        // Copy viewport cells into frame.
        int32_t content_frame_row = frame_origin_row + (int32_t)inset_top;
        int32_t content_frame_col = frame_origin_col + (int32_t)inset_left;

        n00b_text_style_t *text_style = resolve_style(p, p->default_style, 0);

        // Check for ellipsis overflow: if the content grid has more rows
        // than the viewport can show, put "..." on the last visible line.
        n00b_overflow_t overflow = p->box ? p->box->overflow : N00B_OVERFLOW_CLIP;
        bool needs_ellipsis = (overflow == N00B_OVERFLOW_ELLIPSIS
                               && p->total_rows > p->vp_rows
                               && p->vp_row + p->vp_rows < p->total_rows);

        for (n00b_isize_t vr = 0; vr < p->vp_rows; vr++) {
            int32_t fr = content_frame_row + (int32_t)vr;
            if (fr < 0 || fr >= (int32_t)frame_rows) {
                continue;
            }

            // On the last viewport row, emit ellipsis if needed.
            if (needs_ellipsis && vr == p->vp_rows - 1) {
                for (n00b_isize_t vc = 0; vc < p->vp_cols && vc < 3; vc++) {
                    int32_t fc = content_frame_col + (int32_t)vc;
                    if (fc >= 0 && fc < (int32_t)frame_cols) {
                        n00b_rcell_set_ascii(&frame[fr * frame_cols + fc],
                                              '.', text_style);
                    }
                }
                continue;
            }

            n00b_isize_t grid_row = p->vp_row + vr;

            for (n00b_isize_t vc = 0; vc < p->vp_cols; vc++) {
                int32_t fc = content_frame_col + (int32_t)vc;
                if (fc < 0 || fc >= (int32_t)frame_cols) {
                    continue;
                }

                // Parent-bounds clipping: skip cells outside clip rect.
                if (fr < entry->clip_y
                    || fr >= entry->clip_y + entry->clip_h
                    || fc < entry->clip_x
                    || fc >= entry->clip_x + entry->clip_w) {
                    continue;
                }

                n00b_isize_t grid_col = p->vp_col + vc;

                // Ring buffer access for auto-scroll.
                n00b_isize_t actual_row;
                if (p->scroll_mode == N00B_SCROLL_AUTO && p->ring_base != 0) {
                    actual_row = (p->ring_base + grid_row) % p->total_rows;
                }
                else {
                    actual_row = grid_row;
                }

                if (actual_row >= p->total_rows || grid_col >= p->total_cols) {
                    continue;
                }

                const n00b_rcell_t *src = &p->grid[actual_row * p->total_cols
                                                     + grid_col];

                if (src->flags & N00B_CELL_OCCUPIED) {
                    n00b_rcell_t *dst = &frame[fr * frame_cols + fc];
                    *dst = *src;

                    // Override style if state-based resolution yields different.
                    if (!dst->style) {
                        dst->style = text_style;
                    }
                }
            }
        }
    }
}

// -------------------------------------------------------------------
// Public: degrade
// -------------------------------------------------------------------

// Map common box-drawing codepoints to ASCII.
static n00b_codepoint_t
degrade_unicode_to_ascii(n00b_codepoint_t cp)
{
    // Horizontal lines.
    if ((cp >= 0x2500 && cp <= 0x2501)
        || (cp >= 0x2504 && cp <= 0x2509)
        || cp == 0x2550) {
        return '-';
    }
    // Vertical lines.
    if ((cp >= 0x2502 && cp <= 0x2503)
        || (cp >= 0x2506 && cp <= 0x250B)
        || cp == 0x2551) {
        return '|';
    }
    // Corners and T-junctions.
    if ((cp >= 0x250C && cp <= 0x254B)
        || (cp >= 0x2554 && cp <= 0x256C)
        || (cp >= 0x256D && cp <= 0x2570)) {
        return '+';
    }
    return cp;
}

void
n00b_composite_degrade(n00b_rcell_t     *frame,
                        n00b_isize_t      frame_rows,
                        n00b_isize_t      frame_cols,
                        n00b_render_cap_t caps)
{
    bool no_unicode = !(caps & N00B_RCAP_UNICODE);
    bool no_24bit   = !(caps & N00B_RCAP_COLOR_24BIT);
    bool no_256     = !(caps & N00B_RCAP_COLOR_256);
    bool no_color   = !(caps & N00B_RCAP_COLOR_BASIC);

    for (n00b_isize_t i = 0; i < frame_rows * frame_cols; i++) {
        n00b_rcell_t *cell = &frame[i];

        if (!(cell->flags & N00B_CELL_OCCUPIED)) {
            continue;
        }

        // Degrade Unicode box-drawing to ASCII.
        if (no_unicode && cell->grapheme_len > 1) {
            // Decode first codepoint from UTF-8.
            const uint8_t *g  = (const uint8_t *)cell->grapheme;
            n00b_codepoint_t cp;

            if ((g[0] & 0xE0) == 0xC0 && cell->grapheme_len >= 2) {
                cp = ((g[0] & 0x1F) << 6) | (g[1] & 0x3F);
            }
            else if ((g[0] & 0xF0) == 0xE0 && cell->grapheme_len >= 3) {
                cp = ((g[0] & 0x0F) << 12) | ((g[1] & 0x3F) << 6) | (g[2] & 0x3F);
            }
            else {
                continue;
            }

            n00b_codepoint_t ascii = degrade_unicode_to_ascii(cp);
            if (ascii != cp && ascii < 0x80) {
                cell->grapheme[0]  = (char)ascii;
                cell->grapheme[1]  = '\0';
                cell->grapheme_len = 1;
                cell->display_width = 1;
            }
        }

        // Strip colors if unsupported.
        if (cell->style && (no_color || no_24bit || no_256)) {
            // We don't modify the shared style pointer; color stripping
            // is handled at render time by the backend.  This is a
            // placeholder for future per-cell style copy-on-write.
        }
    }
}
