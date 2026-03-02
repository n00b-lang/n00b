/**
 * @file input.h
 * @brief Single-line text input widget.
 *
 * Supports cursor navigation, scrolling, editing, password masking,
 * and change/submit callbacks.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *inp = n00b_input_new(.width = 30,
 *                                     .placeholder = my_str,
 *                                     .on_submit = handle_submit);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/event.h"

// ====================================================================
// Callback type
// ====================================================================

typedef void (*n00b_input_cb_t)(n00b_plane_t *plane, n00b_string_t *text, void *data);

// ====================================================================
// Input data
// ====================================================================

typedef struct n00b_input_t {
    n00b_string_t    *text;
    n00b_string_t    *placeholder;
    n00b_isize_t      cursor_pos;     /**< Codepoint index. */
    n00b_isize_t      scroll_offset;  /**< First visible codepoint. */
    n00b_isize_t      max_length;     /**< 0 = unlimited. */
    bool              password;       /**< Mask with '*'. */
    n00b_input_cb_t   on_change;
    void             *on_change_data;
    n00b_input_cb_t   on_submit;      /**< Enter key. */
    void             *on_submit_data;
} n00b_input_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_input;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new text input widget.
 *
 * @kw text           Initial text content.
 * @kw placeholder    Placeholder text (shown when empty and unfocused).
 * @kw max_length     Maximum codepoint length (0 = unlimited).
 * @kw password       Mask characters with '*'.
 * @kw on_change      Called after each edit.
 * @kw on_change_data User data for on_change.
 * @kw on_submit      Called on Enter.
 * @kw on_submit_data User data for on_submit.
 * @kw width          Width in pixels (0 = auto: 20 chars wide).
 * @kw height         Height in pixels (0 = auto from line height).
 * @kw style          Text style.
 * @kw canvas         Canvas for font metrics (nullptr = cell mode).
 * @kw allocator      Allocator.
 */
extern n00b_plane_t *
n00b_input_new() _kargs {
    n00b_string_t    *text           = nullptr;
    n00b_string_t    *placeholder    = nullptr;
    n00b_isize_t      max_length     = 0;
    bool              password       = false;
    n00b_input_cb_t   on_change      = nullptr;
    void             *on_change_data = nullptr;
    n00b_input_cb_t   on_submit      = nullptr;
    void             *on_submit_data = nullptr;
    int32_t           width          = 0;
    int32_t           height         = 0;
    n00b_text_style_t *style         = nullptr;
    n00b_canvas_t     *canvas        = nullptr;
    n00b_allocator_t  *allocator     = nullptr;
};

/**
 * @brief Set the input text programmatically.
 */
extern void n00b_input_set_text(n00b_plane_t *plane, n00b_string_t *text);

/**
 * @brief Get the current input text.
 */
extern n00b_string_t *n00b_input_get_text(n00b_plane_t *plane);
