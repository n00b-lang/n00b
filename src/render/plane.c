/**
 * Render plane: cell grid, cursor, content writing, and state management.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "core/string.h"
#include "render/plane.h"
#include "strings/string_style.h"

// -------------------------------------------------------------------
// Internal: grid access helpers
// -------------------------------------------------------------------

static inline n00b_rcell_t *
plane_cell(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    if (row >= p->total_rows || col >= p->total_cols) {
        return nullptr;
    }

    n00b_isize_t actual_row;

    if (p->scroll_mode == N00B_SCROLL_AUTO && p->ring_base != 0) {
        actual_row = (p->ring_base + row) % p->total_rows;
    }
    else {
        actual_row = row;
    }

    return &p->grid[actual_row * p->total_cols + col];
}

static inline void
plane_lock(n00b_plane_t *p)
{
    while (n00b_atomic_or(&p->lock, 1) != 0)
        ;
}

static inline void
plane_unlock(n00b_plane_t *p)
{
    n00b_atomic_store(&p->lock, 0);
}

static inline void
plane_mark_dirty(n00b_plane_t *p)
{
    p->flags |= N00B_PLANE_DIRTY;
}

// -------------------------------------------------------------------
// Internal: auto-scroll ring buffer advance
// -------------------------------------------------------------------

static void
plane_auto_scroll_advance(n00b_plane_t *p)
{
    // Clear the row that ring_base is about to point to.
    n00b_isize_t clear_row = (p->ring_base + p->total_rows) % p->total_rows;
    for (n00b_isize_t c = 0; c < p->total_cols; c++) {
        n00b_rcell_clear(&p->grid[clear_row * p->total_cols + c]);
    }

    p->ring_base = (p->ring_base + 1) % p->total_rows;

    if (p->ring_len < p->total_rows) {
        p->ring_len++;
    }
}

// -------------------------------------------------------------------
// Internal: cursor advance after writing
// -------------------------------------------------------------------

static void
advance_cursor(n00b_plane_t *p, uint8_t width, bool wrap)
{
    p->cursor_col += width;

    if (p->cursor_col >= p->total_cols && wrap) {
        p->cursor_col = 0;
        p->cursor_row++;

        if (p->cursor_row >= p->total_rows) {
            if (p->scroll_mode == N00B_SCROLL_AUTO) {
                plane_auto_scroll_advance(p);
                p->cursor_row = p->total_rows - 1;
            }
            else {
                p->cursor_row = p->total_rows - 1;
                p->cursor_col = p->total_cols - 1;
            }
        }
    }

    // For manual and auto scroll, keep the viewport following the cursor.
    if (p->scroll_mode != N00B_SCROLL_NONE) {
        // Ensure cursor row is visible in the viewport.
        if (p->cursor_row >= p->vp_row + p->vp_rows) {
            p->vp_row = p->cursor_row - p->vp_rows + 1;
        }
        else if (p->cursor_row < p->vp_row) {
            p->vp_row = p->cursor_row;
        }

        // Ensure cursor col is visible in the viewport.
        if (p->cursor_col >= p->vp_col + p->vp_cols) {
            p->vp_col = p->cursor_col - p->vp_cols + 1;
        }
        else if (p->cursor_col < p->vp_col) {
            p->vp_col = p->cursor_col;
        }
    }
}

// -------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------

n00b_plane_t *
n00b_plane_new(n00b_isize_t cols, n00b_isize_t rows) _kargs
{
    n00b_isize_t       vp_cols   = 0;
    n00b_isize_t       vp_rows   = 0;
    const char        *name      = nullptr;
    n00b_scroll_mode_t scroll    = N00B_SCROLL_NONE;
    int32_t            z         = 0;
    n00b_box_props_t  *box       = nullptr;
    n00b_text_style_t *style     = nullptr;
    n00b_allocator_t  *allocator = nullptr;
}
{
    n00b_plane_t *p = n00b_alloc(n00b_plane_t, .allocator = allocator);

    p->total_cols = cols;
    p->total_rows = rows;
    p->vp_cols    = vp_cols ? vp_cols : cols;
    p->vp_rows    = vp_rows ? vp_rows : rows;
    p->name       = name;
    p->z          = z;
    p->box        = box;
    p->default_style = style;
    p->scroll_mode   = scroll;
    p->flags         = N00B_PLANE_VISIBLE | N00B_PLANE_DIRTY;
    p->allocator     = allocator;
    p->widget_state  = N00B_WSTATE_NORMAL;

    p->grid = n00b_alloc_array(n00b_rcell_t, (size_t)cols * rows,
                                .allocator = allocator,
                                .no_scan   = true);

    return p;
}

void
n00b_plane_destroy(n00b_plane_t *p)
{
    if (!p) {
        return;
    }

    if (p->grid) {
        n00b_free(p->grid);
        p->grid = nullptr;
    }

    if (p->children) {
        n00b_free(p->children);
        p->children = nullptr;
    }

    n00b_free(p);
}

// -------------------------------------------------------------------
// Hierarchy
// -------------------------------------------------------------------

void
n00b_plane_add_child(n00b_plane_t *parent, n00b_plane_t *child,
                      int32_t x, int32_t y)
{
    assert(child->parent == nullptr);

    child->parent = parent;
    child->x      = x;
    child->y      = y;

    plane_lock(parent);

    if (parent->num_children >= parent->children_cap) {
        n00b_isize_t new_cap = parent->children_cap ? parent->children_cap * 2 : 4;
        n00b_plane_t **new_arr = n00b_alloc_array(n00b_plane_t *, new_cap,
                                                    .allocator = parent->allocator);
        if (parent->children) {
            memcpy(new_arr, parent->children,
                   parent->num_children * sizeof(n00b_plane_t *));
            n00b_free(parent->children);
        }
        parent->children     = new_arr;
        parent->children_cap = new_cap;
    }

    parent->children[parent->num_children++] = child;

    plane_unlock(parent);
    plane_mark_dirty(parent);
}

bool
n00b_plane_remove_child(n00b_plane_t *parent, n00b_plane_t *child)
{
    if (!parent || !child || child->parent != parent) {
        return false;
    }

    plane_lock(parent);

    for (n00b_isize_t i = 0; i < parent->num_children; i++) {
        if (parent->children[i] == child) {
            // Shift remaining children down.
            for (n00b_isize_t j = i; j + 1 < parent->num_children; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->num_children--;
            child->parent = nullptr;
            plane_unlock(parent);
            plane_mark_dirty(parent);
            return true;
        }
    }

    plane_unlock(parent);
    return false;
}

// -------------------------------------------------------------------
// Content writing
// -------------------------------------------------------------------

void
n00b_plane_put_str(n00b_plane_t *p, n00b_string_t *s) _kargs
{
    bool wrap = true;
}
{
    if (!s || !s->data || s->u8_bytes == 0) {
        return;
    }

    plane_lock(p);

    const uint8_t *data = (const uint8_t *)s->data;
    size_t         pos  = 0;

    while (pos < s->u8_bytes) {
        if (p->cursor_row >= p->total_rows) {
            break;
        }
        if (!wrap && p->cursor_col >= p->total_cols) {
            break;
        }

        // Decode one UTF-8 codepoint.
        n00b_codepoint_t cp;
        uint8_t          byte_len;

        uint8_t b0 = data[pos];
        if (b0 < 0x80) {
            cp       = b0;
            byte_len = 1;
        }
        else if ((b0 & 0xE0) == 0xC0) {
            cp       = b0 & 0x1F;
            byte_len = 2;
        }
        else if ((b0 & 0xF0) == 0xE0) {
            cp       = b0 & 0x0F;
            byte_len = 3;
        }
        else if ((b0 & 0xF8) == 0xF0) {
            cp       = b0 & 0x07;
            byte_len = 4;
        }
        else {
            pos++;
            continue;
        }

        if (pos + byte_len > s->u8_bytes) {
            break;
        }

        for (uint8_t i = 1; i < byte_len; i++) {
            cp = (cp << 6) | (data[pos + i] & 0x3F);
        }

        // Handle newline.
        if (cp == '\n') {
            p->cursor_col = 0;
            p->cursor_row++;
            if (p->cursor_row >= p->total_rows
                && p->scroll_mode == N00B_SCROLL_AUTO) {
                plane_auto_scroll_advance(p);
                p->cursor_row = p->total_rows - 1;
            }
            pos += byte_len;
            continue;
        }

        // Determine display width (simplified: ASCII=1, wide CJK=2, else=1).
        uint8_t width = 1;
        if (cp >= 0x1100
            && ((cp <= 0x115F)
                || (cp >= 0x2E80 && cp <= 0x9FFF)
                || (cp >= 0xF900 && cp <= 0xFAFF)
                || (cp >= 0xFE10 && cp <= 0xFE6F)
                || (cp >= 0xFF01 && cp <= 0xFF60)
                || (cp >= 0xFFE0 && cp <= 0xFFE6)
                || (cp >= 0x1F000 && cp <= 0x1FAFF)
                || (cp >= 0x20000 && cp <= 0x2FFFF))) {
            width = 2;
        }

        n00b_rcell_t *cell = plane_cell(p, p->cursor_row, p->cursor_col);
        if (cell) {
            n00b_text_style_t *cell_style = p->default_style;

            if (s->styling) {
                n00b_text_style_t *resolved =
                    n00b_str_resolve_style_at(*s, pos);
                if (resolved) {
                    cell_style = resolved;
                }
            }

            n00b_rcell_set_grapheme(cell,
                                    (const char *)&data[pos],
                                    byte_len,
                                    width,
                                    cell_style);

            // For wide chars, mark the continuation cell.
            if (width == 2 && p->cursor_col + 1 < p->total_cols) {
                n00b_rcell_t *cont = plane_cell(p, p->cursor_row,
                                                 p->cursor_col + 1);
                if (cont) {
                    n00b_rcell_clear(cont);
                    cont->flags         = N00B_CELL_WIDE_CONT | N00B_CELL_DIRTY;
                    cont->display_width = 0;
                }
            }
        }

        advance_cursor(p, width, wrap);
        pos += byte_len;
    }

    plane_mark_dirty(p);
    plane_unlock(p);
}

void
n00b_plane_put_str_at(n00b_plane_t *p, n00b_isize_t row,
                       n00b_isize_t col, n00b_string_t *s)
{
    p->cursor_row = row;
    p->cursor_col = col;
    n00b_plane_put_str(p, s);
}

void
n00b_plane_put_cp(n00b_plane_t *p, n00b_codepoint_t cp) _kargs
{
    n00b_text_style_t *style = nullptr;
}
{
    plane_lock(p);

    if (p->cursor_row >= p->total_rows || p->cursor_col >= p->total_cols) {
        plane_unlock(p);
        return;
    }

    // Simple width heuristic.
    uint8_t width = 1;
    if (cp >= 0x1100
        && ((cp <= 0x115F)
            || (cp >= 0x2E80 && cp <= 0x9FFF)
            || (cp >= 0xF900 && cp <= 0xFAFF)
            || (cp >= 0x20000 && cp <= 0x2FFFF))) {
        width = 2;
    }

    n00b_rcell_t *cell = plane_cell(p, p->cursor_row, p->cursor_col);
    if (cell) {
        n00b_text_style_t *effective = style ? style : p->default_style;
        n00b_rcell_set_codepoint(cell, cp, width, effective);
    }

    advance_cursor(p, width, true);
    plane_mark_dirty(p);
    plane_unlock(p);
}

void
n00b_plane_newline(n00b_plane_t *p)
{
    plane_lock(p);

    p->cursor_col = 0;
    p->cursor_row++;

    if (p->cursor_row >= p->total_rows) {
        if (p->scroll_mode == N00B_SCROLL_AUTO) {
            plane_auto_scroll_advance(p);
            p->cursor_row = p->total_rows - 1;
        }
        else {
            p->cursor_row = p->total_rows - 1;
        }
    }

    plane_mark_dirty(p);
    plane_unlock(p);
}

// -------------------------------------------------------------------
// Cell access
// -------------------------------------------------------------------

n00b_option_t(n00b_const_rcell_ptr_t)
n00b_plane_get_cell(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    n00b_rcell_t *cell = plane_cell(p, row, col);
    return n00b_option_from_nullable(n00b_const_rcell_ptr_t, cell);
}

// -------------------------------------------------------------------
// Clear / fill
// -------------------------------------------------------------------

void
n00b_plane_clear(n00b_plane_t *p)
{
    plane_lock(p);

    memset(p->grid, 0, sizeof(n00b_rcell_t) * p->total_rows * p->total_cols);
    p->cursor_row = 0;
    p->cursor_col = 0;
    p->ring_base  = 0;
    p->ring_len   = 0;

    plane_mark_dirty(p);
    plane_unlock(p);
}

void
n00b_plane_fill_rect(n00b_plane_t *p,
                      n00b_isize_t  row,
                      n00b_isize_t  col,
                      n00b_isize_t  rows,
                      n00b_isize_t  cols) _kargs
{
    n00b_codepoint_t   cp    = ' ';
    n00b_text_style_t *style = nullptr;
}
{
    n00b_text_style_t *effective = style ? style : p->default_style;

    plane_lock(p);

    for (n00b_isize_t r = row; r < row + rows && r < p->total_rows; r++) {
        for (n00b_isize_t c = col; c < col + cols && c < p->total_cols; c++) {
            n00b_rcell_t *cell = plane_cell(p, r, c);
            if (cell) {
                n00b_rcell_set_codepoint(cell, cp, 1, effective);
            }
        }
    }

    plane_mark_dirty(p);
    plane_unlock(p);
}

// -------------------------------------------------------------------
// Cursor
// -------------------------------------------------------------------

void
n00b_plane_cursor_move(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    p->cursor_row = n00b_min(row, p->total_rows > 0 ? p->total_rows - 1 : 0);
    p->cursor_col = n00b_min(col, p->total_cols > 0 ? p->total_cols - 1 : 0);
}

// -------------------------------------------------------------------
// Viewport / scrolling
// -------------------------------------------------------------------

void
n00b_plane_scroll(n00b_plane_t *p, int32_t drow, int32_t dcol)
{
    int64_t new_row = (int64_t)p->vp_row + drow;
    int64_t new_col = (int64_t)p->vp_col + dcol;

    if (new_row < 0) {
        new_row = 0;
    }
    if (new_col < 0) {
        new_col = 0;
    }
    if (new_row + p->vp_rows > p->total_rows) {
        new_row = p->total_rows > p->vp_rows ? p->total_rows - p->vp_rows : 0;
    }
    if (new_col + p->vp_cols > p->total_cols) {
        new_col = p->total_cols > p->vp_cols ? p->total_cols - p->vp_cols : 0;
    }

    p->vp_row = (n00b_isize_t)new_row;
    p->vp_col = (n00b_isize_t)new_col;
    plane_mark_dirty(p);
}

void
n00b_plane_scroll_to(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    p->vp_row = n00b_min(row, p->total_rows > p->vp_rows
                                   ? p->total_rows - p->vp_rows
                                   : 0);
    p->vp_col = n00b_min(col, p->total_cols > p->vp_cols
                                   ? p->total_cols - p->vp_cols
                                   : 0);
    plane_mark_dirty(p);
}

// -------------------------------------------------------------------
// Geometry
// -------------------------------------------------------------------

void
n00b_plane_move(n00b_plane_t *p, int32_t x, int32_t y)
{
    p->x = x;
    p->y = y;
    plane_mark_dirty(p);
}

void
n00b_plane_set_z(n00b_plane_t *p, int32_t z)
{
    p->z = z;
    plane_mark_dirty(p);
}

void
n00b_plane_resize(n00b_plane_t *p, n00b_isize_t rows, n00b_isize_t cols)
{
    plane_lock(p);

    n00b_rcell_t *new_grid = n00b_alloc_array(n00b_rcell_t,
                                                (size_t)cols * rows,
                                                .allocator = p->allocator,
                                                .no_scan   = true);

    // Copy what fits.
    n00b_isize_t copy_rows = n00b_min(rows, p->total_rows);
    n00b_isize_t copy_cols = n00b_min(cols, p->total_cols);

    for (n00b_isize_t r = 0; r < copy_rows; r++) {
        n00b_rcell_t *src = plane_cell(p, r, 0);
        n00b_rcell_t *dst = &new_grid[r * cols];
        memcpy(dst, src, copy_cols * sizeof(n00b_rcell_t));
    }

    n00b_free(p->grid);
    p->grid       = new_grid;
    p->total_rows = rows;
    p->total_cols = cols;
    p->ring_base  = 0;
    p->ring_len   = 0;

    if (p->cursor_row >= rows) {
        p->cursor_row = rows > 0 ? rows - 1 : 0;
    }
    if (p->cursor_col >= cols) {
        p->cursor_col = cols > 0 ? cols - 1 : 0;
    }

    if (p->vp_rows > rows) {
        p->vp_rows = rows;
    }
    if (p->vp_cols > cols) {
        p->vp_cols = cols;
    }

    plane_mark_dirty(p);
    plane_unlock(p);
}

void
n00b_plane_set_visible(n00b_plane_t *p, bool visible)
{
    if (visible) {
        p->flags |= N00B_PLANE_VISIBLE;
    }
    else {
        p->flags &= (uint16_t)~N00B_PLANE_VISIBLE;
    }
    plane_mark_dirty(p);
}

// -------------------------------------------------------------------
// Box decoration
// -------------------------------------------------------------------

void
n00b_plane_set_box(n00b_plane_t *p, n00b_box_props_t *box)
{
    p->box = box;
    plane_mark_dirty(p);
}

void
n00b_plane_content_size(n00b_plane_t *p,
                         n00b_isize_t *out_rows,
                         n00b_isize_t *out_cols)
{
    n00b_isize_t h_inset = 0;
    n00b_isize_t v_inset = 0;

    if (p->box) {
        if (p->box->borders & N00B_BORDER_LEFT) {
            h_inset++;
        }
        if (p->box->borders & N00B_BORDER_RIGHT) {
            h_inset++;
        }
        if (p->box->borders & N00B_BORDER_TOP) {
            v_inset++;
        }
        if (p->box->borders & N00B_BORDER_BOTTOM) {
            v_inset++;
        }

        h_inset += p->box->pad_left + p->box->pad_right;
        v_inset += p->box->pad_top + p->box->pad_bottom;
    }

    *out_cols = p->vp_cols > h_inset ? p->vp_cols - h_inset : 0;
    *out_rows = p->vp_rows > v_inset ? p->vp_rows - v_inset : 0;
}

// -------------------------------------------------------------------
// Widget state
// -------------------------------------------------------------------

void
n00b_plane_set_state(n00b_plane_t *p, n00b_widget_state_t state)
{
    p->widget_state = state;
    plane_mark_dirty(p);
}

n00b_widget_state_t
n00b_plane_get_state(n00b_plane_t *p)
{
    return p->widget_state;
}
