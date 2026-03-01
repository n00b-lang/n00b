/**
 * @file checkbox.h
 * @brief Checkbox widget: toggle with label and change callback.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *cb = n00b_checkbox_new(my_label,
 *                                       .on_change = my_handler);
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

typedef void (*n00b_checkbox_cb_t)(n00b_plane_t *plane, bool checked, void *data);

// ====================================================================
// Checkbox data
// ====================================================================

typedef struct n00b_checkbox_t {
    n00b_string_t      *label;
    bool                checked;
    n00b_checkbox_cb_t  on_change;
    void               *on_change_data;
} n00b_checkbox_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_checkbox;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new checkbox widget.
 *
 * @param label Checkbox label text.
 *
 * @kw checked        Initial checked state.
 * @kw on_change      Change callback.
 * @kw on_change_data User data for callback.
 * @kw cols           Width (0 = auto from label + indicator).
 * @kw rows           Height (default 1).
 * @kw style          Text style.
 * @kw allocator      Allocator.
 */
extern n00b_plane_t *
n00b_checkbox_new(n00b_string_t *label) _kargs {
    bool                checked        = false;
    n00b_checkbox_cb_t  on_change      = nullptr;
    void               *on_change_data = nullptr;
    n00b_isize_t        cols           = 0;
    n00b_isize_t        rows           = 1;
    n00b_text_style_t  *style          = nullptr;
    n00b_allocator_t   *allocator      = nullptr;
};

/**
 * @brief Set the checked state programmatically.
 */
extern void n00b_checkbox_set_checked(n00b_plane_t *plane, bool checked);

/**
 * @brief Get the current checked state.
 */
extern bool n00b_checkbox_is_checked(n00b_plane_t *plane);
