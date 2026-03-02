/**
 * @file button.h
 * @brief Button widget: clickable with label, Enter/Space activation.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *btn = n00b_button_new(my_label,
 *                                      .on_click = my_handler);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/types.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/event.h"

// ====================================================================
// Callback type
// ====================================================================

typedef void (*n00b_button_cb_t)(n00b_plane_t *plane, void *data);

// ====================================================================
// Button data
// ====================================================================

typedef struct n00b_button_t {
    n00b_string_t    *label;
    n00b_button_cb_t  on_click;
    void             *on_click_data;
    n00b_codepoint_t  shortcut;  /**< 0 = none. */
} n00b_button_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_button;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new button widget.
 *
 * @param label Button label text.
 *
 * @kw on_click      Click callback.
 * @kw on_click_data User data for callback.
 * @kw shortcut      Keyboard shortcut codepoint (0 = none).
 * @kw box           Box decoration (nullptr = auto with rounded border).
 * @kw canvas        Canvas for font metrics (nullptr = none).
 * @kw width         Width (0 = auto from label).
 * @kw height        Height (0 = auto, typically 3 for border).
 * @kw style         Text style.
 * @kw allocator     Allocator.
 */
extern n00b_plane_t *
n00b_button_new(n00b_string_t *label) _kargs {
    n00b_button_cb_t   on_click      = nullptr;
    void              *on_click_data = nullptr;
    n00b_codepoint_t   shortcut      = 0;
    n00b_box_props_t  *box           = nullptr;
    n00b_canvas_t     *canvas        = nullptr;
    int32_t            width         = 0;
    int32_t            height        = 0;
    n00b_text_style_t *style         = nullptr;
    n00b_allocator_t  *allocator     = nullptr;
};
