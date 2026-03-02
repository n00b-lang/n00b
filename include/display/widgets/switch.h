/**
 * @file switch.h
 * @brief On/off toggle switch widget with label and change callback.
 *
 * Visually distinct from checkbox — renders a sliding track with a
 * thumb indicator rather than a check mark.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *sw = n00b_switch_new(my_label,
 *                                     .on_change = my_handler);
 *
 * bool is_on = n00b_switch_is_on(sw);
 * n00b_switch_toggle(sw);
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

typedef void (*n00b_switch_cb_t)(n00b_plane_t *plane, bool on, void *data);

// ====================================================================
// Switch data
// ====================================================================

typedef struct n00b_switch_t {
    n00b_string_t    *label;
    bool              on;
    n00b_switch_cb_t  on_change;
    void             *on_change_data;
} n00b_switch_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_switch;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new switch widget.
 *
 * @param label Switch label text.
 *
 * @kw on             Initial on/off state.
 * @kw on_change      Change callback.
 * @kw on_change_data User data for callback.
 * @kw width          Width (0 = auto from label + track).
 * @kw height         Height (0 = auto from line height).
 * @kw style          Text style.
 * @kw canvas         Canvas for font metrics (nullptr = cell mode).
 * @kw allocator      Allocator.
 */
extern n00b_plane_t *
n00b_switch_new(n00b_string_t *label) _kargs {
    bool              on             = false;
    n00b_switch_cb_t  on_change      = nullptr;
    void             *on_change_data = nullptr;
    int32_t           width          = 0;
    int32_t           height         = 0;
    n00b_text_style_t *style         = nullptr;
    n00b_canvas_t     *canvas        = nullptr;
    n00b_allocator_t  *allocator     = nullptr;
};

/**
 * @brief Get the current on/off state.
 */
extern bool n00b_switch_is_on(n00b_plane_t *plane);

/**
 * @brief Set the on/off state programmatically.
 */
extern void n00b_switch_set_on(n00b_plane_t *plane, bool on);

/**
 * @brief Toggle the switch state.
 */
extern void n00b_switch_toggle(n00b_plane_t *plane);
