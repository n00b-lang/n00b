/**
 * @file text.h
 * @brief Wrapped rich-text widget with selection and clipboard copy.
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"

/**
 * @brief Visual selection endpoints for a wrapped text widget.
 *
 * Coordinates are stored as wrapped-line caret slots between grapheme
 * clusters, not raw byte offsets into the source string.
 */
typedef struct n00b_text_selection_t {
    int32_t start_line;
    int32_t start_col;
    int32_t end_line;
    int32_t end_col;
    bool    active;
} n00b_text_selection_t;

/**
 * @brief Public state for the wrapped rich-text widget.
 *
 * The widget renders a styled `n00b_string_t`, optionally wraps it,
 * supports hanging indent, and can expose a visual selection for copy.
 * `wrapped_line_count` and `cached_wrap_cols` reflect the most recent
 * internal cache build.
 */
typedef struct n00b_text_t {
    n00b_string_t        *text;
    n00b_alignment_t      alignment;
    bool                  wrap;
    int32_t               hang_indent_cols;
    bool                  selectable;
    bool                  copy_on_release;
    n00b_text_selection_t selection;
    int32_t               wrapped_line_count;
    int32_t               cached_wrap_cols;
} n00b_text_t;

/** @brief Widget vtable for the wrapped rich-text widget. */
extern const n00b_widget_vtable_t n00b_widget_text;

/**
 * @brief Create a new wrapped rich-text widget.
 *
 * @param text Initial text to render (nullable).
 *
 * @kw alignment Horizontal alignment for each visual line.
 * @kw wrap Enable width-sensitive wrapping (default `true`).
 * @kw hang_indent_cols Continuation-line indent in text columns.
 * @kw selectable Enable mouse selection and focus.
 * @kw copy_on_release Copy a non-empty selection on mouse release.
 * @kw canvas Optional canvas for metrics, focus, and clipboard services.
 * @kw allocator Optional allocator for internal state.
 *
 * @return A new plane with the text widget attached.
 */
extern n00b_plane_t *
n00b_text_new(n00b_string_t *text) _kargs {
    n00b_alignment_t  alignment        = N00B_ALIGN_LEFT;
    bool              wrap             = true;
    int32_t           hang_indent_cols = 0;
    bool              selectable       = false;
    bool              copy_on_release  = true;
    n00b_canvas_t    *canvas           = nullptr;
    n00b_allocator_t *allocator        = nullptr;
};

/** @brief Replace the widget's source text and invalidate cached wrapping. */
extern void           n00b_text_set_text(n00b_plane_t *text_plane, n00b_string_t *text);
/** @brief Return the widget's current source text, or `nullptr` if not a text widget. */
extern n00b_string_t *n00b_text_get_text(n00b_plane_t *text_plane);
/** @brief Set horizontal alignment for rendered visual lines. */
extern void           n00b_text_set_alignment(n00b_plane_t *text_plane, n00b_alignment_t alignment);
/** @brief Set the continuation-line hanging indent in text columns. */
extern void           n00b_text_set_hang_indent(n00b_plane_t *text_plane, int32_t hang_indent_cols);
/** @brief Enable or disable selection/focus behavior for the widget. */
extern void           n00b_text_set_selectable(n00b_plane_t *text_plane, bool selectable);
/** @brief Report whether the widget currently has a non-empty visual selection. */
extern bool           n00b_text_has_selection(n00b_plane_t *text_plane);
/** @brief Clear any active selection range. */
extern void           n00b_text_clear_selection(n00b_plane_t *text_plane);
/** @brief Return the current visual selection joined with `\\n`, or `nullptr` if empty. */
extern n00b_string_t *n00b_text_get_selection(n00b_plane_t *text_plane);
/** @brief Copy the current visual selection through the canvas clipboard path. */
extern bool           n00b_text_copy_selection(n00b_plane_t *text_plane);
/** @brief Return the current wrapped visual-line count after ensuring the cache exists. */
extern int32_t        n00b_text_get_wrapped_line_count(n00b_plane_t *text_plane);
