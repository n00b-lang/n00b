/**
 * @file checkbox.h
 * @brief Checkbox widget: toggle with label and change callback.
 *
 * Supports multiple visual indicator styles (ASCII, Unicode ballot /
 * circle / square, GUI-native) selectable per-widget.  The style
 * resolves automatically from backend capabilities when set to AUTO.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *cb = n00b_checkbox_new(my_label,
 *                                       .on_change = my_handler);
 *
 * // Explicit circle style:
 * n00b_plane_t *cb2 = n00b_checkbox_new(my_label,
 *                                        .indicator = N00B_CB_STYLE_CIRCLE);
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
 * @brief Visual style for the checkbox indicator glyphs.
 *
 * AUTO resolves at construction time based on backend capabilities:
 * GUI_EXT → GUI, UNICODE → BALLOT, else ASCII.
 */
typedef enum : uint8_t {
    N00B_CB_STYLE_AUTO   = 0,
    N00B_CB_STYLE_ASCII  = 1, /**< `[ ]` / `[x]` / `>` */
    N00B_CB_STYLE_BALLOT = 2, /**< U+2610 / U+2611 / U+25B8 */
    N00B_CB_STYLE_CIRCLE = 3, /**< U+25CB / U+25CF / U+25B8 */
    N00B_CB_STYLE_SQUARE = 4, /**< U+25A1 / U+25A0 / U+25B8 */
    N00B_CB_STYLE_GUI    = 5, /**< Native (falls back to BALLOT). */
} n00b_checkbox_indicator_t;

// ====================================================================
// Glyph table
// ====================================================================

/**
 * @brief Resolved glyph set for a checkbox indicator style.
 *
 * When `unchecked == 0` the renderer uses the ASCII string path
 * (multi-character indicators like `[ ]`); otherwise it uses
 * single-codepoint rendering via `n00b_plane_draw_glyph()`.
 */
typedef struct n00b_checkbox_glyphs_t {
    n00b_codepoint_t unchecked;       /**< 0 = use ASCII string path. */
    n00b_codepoint_t checked;
    n00b_codepoint_t focus_on;
    n00b_codepoint_t focus_off;
    uint8_t          indicator_width; /**< Display columns for indicator. */
    uint8_t          focus_width;     /**< Display columns for focus glyph. */
} n00b_checkbox_glyphs_t;

extern const n00b_checkbox_glyphs_t n00b_cb_glyphs_ascii;
extern const n00b_checkbox_glyphs_t n00b_cb_glyphs_ballot;
extern const n00b_checkbox_glyphs_t n00b_cb_glyphs_circle;
extern const n00b_checkbox_glyphs_t n00b_cb_glyphs_square;
extern const n00b_checkbox_glyphs_t n00b_cb_glyphs_gui;

// ====================================================================
// Callback type
// ====================================================================

typedef void (*n00b_checkbox_cb_t)(n00b_plane_t *plane, bool checked, void *data);

// ====================================================================
// Checkbox data
// ====================================================================

typedef struct n00b_checkbox_t {
    n00b_string_t              *label;
    n00b_checkbox_glyphs_t      glyphs;
    n00b_checkbox_indicator_t   indicator;
    bool                        checked;
    n00b_checkbox_cb_t          on_change;
    void                       *on_change_data;
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
 * @kw indicator      Indicator style (default AUTO).
 * @kw caps           Render capabilities for AUTO resolution.
 * @kw canvas         Canvas for font metrics (nullptr = none).
 * @kw width          Width (0 = auto from label + indicator).
 * @kw height         Height (default 1).
 * @kw style          Text style.
 * @kw allocator      Allocator.
 */
extern n00b_plane_t *
n00b_checkbox_new(n00b_string_t *label) _kargs {
    bool                       checked        = false;
    n00b_checkbox_cb_t         on_change      = nullptr;
    void                      *on_change_data = nullptr;
    n00b_checkbox_indicator_t  indicator      = N00B_CB_STYLE_AUTO;
    n00b_render_cap_t          caps           = N00B_RCAP_UNICODE;
    n00b_canvas_t             *canvas         = nullptr;
    int32_t                    width          = 0;
    int32_t                    height         = 0;
    n00b_text_style_t         *style          = nullptr;
    n00b_allocator_t          *allocator      = nullptr;
};

/**
 * @brief Set the checked state programmatically.
 */
extern void n00b_checkbox_set_checked(n00b_plane_t *plane, bool checked);

/**
 * @brief Get the current checked state.
 */
extern bool n00b_checkbox_is_checked(n00b_plane_t *plane);

/**
 * @brief Change the indicator style at runtime.
 *
 * Resolves glyphs, resizes the plane if widths changed, and
 * re-renders.
 *
 * @param plane     Checkbox plane.
 * @param indicator New indicator style.
 * @param caps      Backend capabilities for AUTO resolution.
 */
extern void n00b_checkbox_set_indicator(n00b_plane_t              *plane,
                                         n00b_checkbox_indicator_t  indicator,
                                         n00b_render_cap_t          caps);
