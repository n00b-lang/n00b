/**
 * @file backend_cocoa.h
 * @brief Native Cocoa/macOS renderer backend with Core Text rendering.
 *
 * Draws into an NSWindow using Core Text + Core Graphics, enabling
 * rich GUI rendering of the same plane/canvas/table content that
 * currently renders to terminals.
 *
 * ### GUI extensions
 *
 * Attach a `cocoa_gui_ext_t` to `n00b_box_props_t::gui_ext` to enable
 * per-plane shadows, rounded corners, opacity, gradients, and blend
 * modes.  The `prepare_gui` vtable slot caches these for rendering.
 *
 * ### Related modules
 *
 * - `render/backend.h` — vtable and capability types
 * - `render/types.h` — `n00b_box_props_t::gui_ext`
 * - `render/plane.h` — plane hierarchy rendered by this backend
 */
#pragma once

#include "n00b.h"
#include "text/strings/text_style.h"

// ====================================================================
// Blend modes
// ====================================================================

/**
 * @brief Core Graphics blend modes exposed to the Cocoa backend.
 */
typedef enum : uint8_t {
    N00B_BLEND_NORMAL     = 0,
    N00B_BLEND_MULTIPLY   = 1,
    N00B_BLEND_SCREEN     = 2,
    N00B_BLEND_OVERLAY    = 3,
    N00B_BLEND_SOFT_LIGHT = 4,
} n00b_cocoa_blend_mode_t;

// ====================================================================
// Gradient direction
// ====================================================================

/**
 * @brief Direction/type of background gradient fill.
 */
typedef enum : uint8_t {
    N00B_GRADIENT_NONE        = 0,
    N00B_GRADIENT_TOP_BOTTOM  = 1,
    N00B_GRADIENT_LEFT_RIGHT  = 2,
    N00B_GRADIENT_DIAGONAL    = 3,
    N00B_GRADIENT_RADIAL      = 4,
} n00b_cocoa_gradient_dir_t;

// ====================================================================
// GUI extension struct
// ====================================================================

/**
 * @brief Per-plane GUI decoration properties for the Cocoa backend.
 *
 * Allocate with `n00b_cocoa_gui_ext_new()` and attach to
 * `box_props->gui_ext`.  Terminal backends ignore this pointer.
 *
 * @note Color fields use the same `N00B_COLOR_VALID_BIT` convention
 *       as `n00b_color_t` — a color is active only when
 *       `n00b_color_is_set()` returns true.
 */
typedef struct cocoa_gui_ext_t {
    float                     shadow_offset_x;
    float                     shadow_offset_y;
    float                     shadow_blur;
    n00b_color_t              shadow_color;     /**< High bit = valid. */

    float                     corner_radius;    /**< 0 = sharp corners. */
    float                     opacity;          /**< 0.0..1.0, default 1.0. */

    n00b_cocoa_gradient_dir_t gradient_dir;
    n00b_color_t              gradient_start;   /**< High bit = valid. */
    n00b_color_t              gradient_end;     /**< High bit = valid. */

    n00b_cocoa_blend_mode_t   blend_mode;
} cocoa_gui_ext_t;

// ====================================================================
// Constructor
// ====================================================================

/**
 * @brief Allocate a zeroed `cocoa_gui_ext_t` with safe defaults.
 * @return Heap-allocated struct with `opacity = 1.0`.
 */
extern cocoa_gui_ext_t *n00b_cocoa_gui_ext_new(void);

/**
 * @brief Pump the NSRunLoop for `seconds` seconds.
 *
 * The Cocoa backend does **not** own the event loop.  After calling
 * `n00b_canvas_render()`, call this to let AppKit process window
 * display, mouse events, resize notifications, etc.
 *
 * @param seconds Duration to pump.  Use a large value and Ctrl-C to
 *                exit, or call repeatedly in a loop.
 */
extern void n00b_cocoa_run_loop_pump(double seconds);
