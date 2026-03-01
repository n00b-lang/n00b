/**
 * @file label.h
 * @brief Label widget: displays styled text in a plane.
 *
 * A label renders an `n00b_string_t` (with per-range styling) into a
 * plane's cell grid.  Supports horizontal alignment (left, center,
 * right) and optional word-wrap for multi-line display.
 *
 * All rich string styling flows through `n00b_plane_put_str()`, which
 * resolves per-codepoint styles from the string's styling metadata.
 * Both ANSI and Cocoa backends render the resulting styled cells
 * identically.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *lbl = n00b_label_new(my_string,
 *                                     .alignment = N00B_ALIGN_CENTER,
 *                                     .cols = 40);
 * // Later, to update:
 * n00b_label_set_text(lbl, new_string);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/types.h"
#include "display/render/plane.h"
#include "display/widget.h"

// ====================================================================
// Label data
// ====================================================================

typedef struct n00b_label_t {
    n00b_string_t   *text;
    n00b_alignment_t alignment;
    bool             wrap;
} n00b_label_t;

// ====================================================================
// Vtable (defined in label.c)
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_label;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new label widget.
 *
 * Returns a plane with the label widget attached.  If `cols` or `rows`
 * is 0, they are auto-sized from the text (display width for cols,
 * 1 for rows unless wrapping).
 *
 * @param text  The string to display.
 *
 * @kw alignment  Horizontal alignment (default LEFT).
 * @kw wrap       Word-wrap long text across rows.
 * @kw box        Box decoration properties.
 * @kw style      Default text style for the plane.
 * @kw cols       Plane width (0 = auto from text width).
 * @kw rows       Plane height (0 = auto, 1 unless wrap).
 * @kw allocator  Allocator for internal allocations.
 *
 * @return A new plane with the label widget attached.
 */
extern n00b_plane_t *
n00b_label_new(n00b_string_t *text) _kargs {
    n00b_alignment_t   alignment = N00B_ALIGN_LEFT;
    bool               wrap      = false;
    n00b_box_props_t  *box       = nullptr;
    n00b_text_style_t *style     = nullptr;
    n00b_isize_t       cols      = 0;
    n00b_isize_t       rows      = 0;
    n00b_allocator_t  *allocator = nullptr;
};

/**
 * @brief Update the label's text and re-render.
 *
 * @param plane The label's plane (must have label widget attached).
 * @param text  New text to display.
 */
extern void n00b_label_set_text(n00b_plane_t *plane, n00b_string_t *text);

/**
 * @brief Get the label's current text.
 *
 * @param plane The label's plane.
 * @return      The current text, or nullptr if not a label.
 */
extern n00b_string_t *n00b_label_get_text(n00b_plane_t *plane);
