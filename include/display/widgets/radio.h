/**
 * @file radio.h
 * @brief Radio button widget with group-based mutual exclusion.
 *
 * Radio buttons belong to a group; selecting one deselects all others
 * in the same group.  Supports Unicode and ASCII indicator styles via
 * the same glyph resolution pattern as checkbox.
 *
 * ### Usage
 *
 * ```c
 * n00b_radio_group_t *g = n00b_radio_group_new();
 * n00b_plane_t *r1 = n00b_radio_new(label1, .group = g);
 * n00b_plane_t *r2 = n00b_radio_new(label2, .group = g);
 * n00b_radio_group_on_change(g, my_handler, nullptr);
 * int sel = n00b_radio_group_get_selected(g);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/backend.h"
#include "display/widget.h"
#include "display/event.h"

// ====================================================================
// Indicator style
// ====================================================================

/**
 * @brief Visual style for the radio indicator glyphs.
 */
typedef enum : uint8_t {
    N00B_RADIO_STYLE_AUTO    = 0,
    N00B_RADIO_STYLE_ASCII   = 1, /**< `( )` / `(*)` */
    N00B_RADIO_STYLE_UNICODE = 2, /**< U+25CB / U+25C9 */
} n00b_radio_indicator_t;

// ====================================================================
// Glyph table
// ====================================================================

typedef struct n00b_radio_glyphs_t {
    n00b_codepoint_t unselected;
    n00b_codepoint_t selected;
    n00b_codepoint_t focus_on;
    n00b_codepoint_t focus_off;
    uint8_t          indicator_width;
    uint8_t          focus_width;
} n00b_radio_glyphs_t;

extern const n00b_radio_glyphs_t n00b_radio_glyphs_ascii;
extern const n00b_radio_glyphs_t n00b_radio_glyphs_unicode;

// ====================================================================
// Callback type
// ====================================================================

typedef void (*n00b_radio_cb_t)(n00b_plane_t *group_plane,
                                int           selected,
                                void         *data);

// ====================================================================
// Radio group
// ====================================================================

typedef struct n00b_radio_group_t {
    n00b_plane_t   **radios;
    n00b_isize_t     count;
    n00b_isize_t     capacity;
    int              selected;       /**< -1 = none selected. */
    n00b_radio_cb_t  on_change;
    void            *on_change_data;
} n00b_radio_group_t;

// ====================================================================
// Radio data (per-button)
// ====================================================================

typedef struct n00b_radio_t {
    n00b_string_t        *label;
    n00b_radio_group_t   *group;
    n00b_radio_glyphs_t   glyphs;
    int                   index;
} n00b_radio_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_radio;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new radio group.
 */
extern n00b_radio_group_t *n00b_radio_group_new(void);

/**
 * @brief Create a new radio button in a group.
 *
 * @param label Radio label text.
 *
 * @kw group     Radio group (required).
 * @kw indicator Indicator style (default AUTO).
 * @kw caps      Render capabilities for AUTO resolution.
 * @kw canvas    Canvas for font metrics (nullptr = none).
 * @kw width     Width (0 = auto from label + indicator).
 * @kw height    Height (default 1).
 * @kw style     Text style.
 * @kw allocator Allocator.
 */
extern n00b_plane_t *
n00b_radio_new(n00b_string_t *label) _kargs {
    n00b_radio_group_t    *group     = nullptr;
    n00b_radio_indicator_t indicator = N00B_RADIO_STYLE_AUTO;
    n00b_render_cap_t      caps      = N00B_RCAP_UNICODE;
    n00b_canvas_t         *canvas    = nullptr;
    int32_t                width     = 0;
    int32_t                height    = 0;
    n00b_text_style_t     *style     = nullptr;
    n00b_allocator_t      *allocator = nullptr;
};

/**
 * @brief Get the currently selected index in the group (-1 = none).
 */
extern int n00b_radio_group_get_selected(n00b_radio_group_t *group);

/**
 * @brief Set the selected radio button by index.
 */
extern void n00b_radio_group_set_selected(n00b_radio_group_t *group, int index);

/**
 * @brief Check if a specific radio button is selected.
 */
extern bool n00b_radio_is_selected(n00b_plane_t *plane);

/**
 * @brief Set the change callback on a group.
 */
extern void n00b_radio_group_on_change(n00b_radio_group_t *group,
                                        n00b_radio_cb_t     cb,
                                        void               *data);
