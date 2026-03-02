/**
 * @file types.h
 * @brief Core type definitions for the 2D rendering system.
 *
 * Defines enumerations and structures shared across the render layer:
 * alignment, border configuration, box properties, overflow modes,
 * widget state, and state-based style overrides.
 *
 * ### Migrated from
 *
 * `~/n00b-old/include/text/theme.h` — alignment, border sets, box props.
 * Extended with GUI hooks (margin, overflow, `gui_ext`) and widget
 * state styling for the new compositing pipeline.
 *
 * ### Related modules
 *
 * - `render/cell.h` — render cell that references styles
 * - `render/plane.h` — plane that owns box props
 * - `render/box.h` — box stamping using these types
 * - `strings/text_style.h` — text style referenced here
 */
#pragma once

#include "n00b.h"

// ====================================================================
// Alignment
// ====================================================================

/**
 * @brief Alignment flags for content within a box.
 *
 * Horizontal and vertical flags can be OR'd together.
 */
typedef enum : int8_t {
    N00B_ALIGN_IGNORE = 0,
    N00B_ALIGN_LEFT   = 1,
    N00B_ALIGN_RIGHT  = 2,
    N00B_ALIGN_CENTER = 4,
    N00B_ALIGN_TOP    = 8,
    N00B_ALIGN_BOTTOM = 16,
    N00B_ALIGN_MIDDLE = 32,

    N00B_ALIGN_TOP_LEFT      = N00B_ALIGN_LEFT | N00B_ALIGN_TOP,
    N00B_ALIGN_TOP_RIGHT     = N00B_ALIGN_RIGHT | N00B_ALIGN_TOP,
    N00B_ALIGN_TOP_CENTER    = N00B_ALIGN_CENTER | N00B_ALIGN_TOP,
    N00B_ALIGN_MID_LEFT      = N00B_ALIGN_LEFT | N00B_ALIGN_MIDDLE,
    N00B_ALIGN_MID_RIGHT     = N00B_ALIGN_RIGHT | N00B_ALIGN_MIDDLE,
    N00B_ALIGN_MID_CENTER    = N00B_ALIGN_CENTER | N00B_ALIGN_MIDDLE,
    N00B_ALIGN_BOTTOM_LEFT   = N00B_ALIGN_LEFT | N00B_ALIGN_BOTTOM,
    N00B_ALIGN_BOTTOM_RIGHT  = N00B_ALIGN_RIGHT | N00B_ALIGN_BOTTOM,
    N00B_ALIGN_BOTTOM_CENTER = N00B_ALIGN_CENTER | N00B_ALIGN_BOTTOM,
} n00b_alignment_t;

#define N00B_HORIZONTAL_MASK (N00B_ALIGN_LEFT | N00B_ALIGN_CENTER | N00B_ALIGN_RIGHT)
#define N00B_VERTICAL_MASK   (N00B_ALIGN_TOP | N00B_ALIGN_MIDDLE | N00B_ALIGN_BOTTOM)

// ====================================================================
// Border configuration
// ====================================================================

/**
 * @brief Bitmask selecting which borders are drawn.
 */
typedef enum : uint8_t {
    N00B_BORDER_NONE       = 0,
    N00B_BORDER_TOP        = 0x01,
    N00B_BORDER_BOTTOM     = 0x02,
    N00B_BORDER_LEFT       = 0x04,
    N00B_BORDER_RIGHT      = 0x08,
    N00B_BORDER_INTERIOR_H = 0x10,
    N00B_BORDER_INTERIOR_V = 0x20,
    N00B_BORDER_SIDES      = N00B_BORDER_LEFT | N00B_BORDER_RIGHT,
    N00B_BORDER_ALL        = 0x3F,
} n00b_border_set_t;

/**
 * @brief Unicode codepoints for box-drawing characters.
 *
 * Defines the visual appearance of borders: corners, edges,
 * T-junctions, and cross intersections.
 */
typedef struct n00b_border_theme_t {
    n00b_codepoint_t horizontal;
    n00b_codepoint_t vertical;
    n00b_codepoint_t upper_left;
    n00b_codepoint_t upper_right;
    n00b_codepoint_t lower_left;
    n00b_codepoint_t lower_right;
    n00b_codepoint_t cross;
    n00b_codepoint_t top_t;
    n00b_codepoint_t bottom_t;
    n00b_codepoint_t left_t;
    n00b_codepoint_t right_t;
} n00b_border_theme_t;

// Predefined border themes.
extern const n00b_border_theme_t n00b_border_plain;
extern const n00b_border_theme_t n00b_border_bold;
extern const n00b_border_theme_t n00b_border_double;
extern const n00b_border_theme_t n00b_border_dash;
extern const n00b_border_theme_t n00b_border_bold_dash;
extern const n00b_border_theme_t n00b_border_dash2;
extern const n00b_border_theme_t n00b_border_bold_dash2;
extern const n00b_border_theme_t n00b_border_ascii;
extern const n00b_border_theme_t n00b_border_rounded;

// ====================================================================
// Overflow
// ====================================================================

/**
 * @brief How content that exceeds the plane boundary is handled.
 */
typedef enum : uint8_t {
    N00B_OVERFLOW_CLIP     = 0, /**< Content clipped at boundary (default). */
    N00B_OVERFLOW_SCROLL   = 1, /**< Enables scrolling viewport. */
    N00B_OVERFLOW_ELLIPSIS = 2, /**< Truncate with "..." on last visible line. */
    N00B_OVERFLOW_VISIBLE  = 3, /**< Content extends past bounds (compositing clips). */
} n00b_overflow_t;

// ====================================================================
// Widget state
// ====================================================================

/**
 * @brief Visual state of a widget / plane for style resolution.
 *
 * During compositing, the compositor checks `box->state_styles[state]`
 * for style overrides.  If non-null, those overrides replace the base
 * border/fill/text styles for that render pass.
 */
typedef enum : uint8_t {
    N00B_WSTATE_NORMAL   = 0,
    N00B_WSTATE_FOCUSED  = 1,
    N00B_WSTATE_DISABLED = 2,
    N00B_WSTATE_HOVER    = 3,
    N00B_WSTATE_ACTIVE   = 4,
    N00B_WSTATE_COUNT    = 5,
} n00b_widget_state_t;

// ====================================================================
// State style overrides
// ====================================================================

/**
 * @brief Per-state style overrides for box props.
 *
 * Any nullptr field means "inherit from the base (NORMAL) style".
 */
typedef struct n00b_state_style_t {
    n00b_text_style_t         *text_style;
    n00b_text_style_t         *border_style;
    n00b_text_style_t         *fill_style;
    const n00b_border_theme_t *border_theme;
} n00b_state_style_t;

// ====================================================================
// Box properties
// ====================================================================

/**
 * @brief Decoration and layout properties for a plane's box.
 *
 * Controls borders, padding, margins, alignment, overflow mode,
 * and per-state style overrides.
 *
 * @note `gui_ext` is an opaque pointer for GUI-backend-specific
 *       properties (shadows, rounded corners, opacity, etc.).
 *       Terminal backends ignore it entirely.
 */
typedef struct n00b_box_props_t {
    const n00b_border_theme_t *border_theme;
    n00b_text_style_t         *border_style;
    n00b_text_style_t         *fill_style;
    n00b_text_style_t         *text_style;   /**< Style for content text. */
    n00b_border_set_t          borders;
    int8_t                     pad_top;
    int8_t                     pad_bottom;
    int8_t                     pad_left;
    int8_t                     pad_right;
    int8_t                     margin_top;
    int8_t                     margin_bottom;
    int8_t                     margin_left;
    int8_t                     margin_right;
    n00b_alignment_t           alignment;
    n00b_overflow_t            overflow;

    /**
     * Per-state style overrides, indexed by `n00b_widget_state_t`.
     * Entry `[N00B_WSTATE_NORMAL]` is unused (base styles above serve
     * as normal).  nullptr entries inherit from the base.
     */
    n00b_state_style_t        *state_styles[N00B_WSTATE_COUNT];

    void                      *gui_ext;
} n00b_box_props_t;

// ====================================================================
// Scroll mode
// ====================================================================

/**
 * @brief Scrolling behavior for a plane.
 */
typedef enum : uint8_t {
    N00B_SCROLL_NONE   = 0, /**< Writes past bounds are clipped. */
    N00B_SCROLL_MANUAL = 1, /**< Viewport moves on explicit API calls. */
    N00B_SCROLL_AUTO   = 2, /**< Viewport follows cursor; old rows discarded. */
} n00b_scroll_mode_t;

// ====================================================================
// Layout types (pixel-based flex layout)
// ====================================================================

/**
 * @brief Pixel-based bounding rectangle used by the layout engine.
 */
typedef struct n00b_rect_t {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} n00b_rect_t;

/**
 * @brief Flex layout direction for child arrangement.
 */
typedef enum : uint8_t {
    N00B_FLEX_ROW,    /**< Arrange children horizontally. */
    N00B_FLEX_COLUMN, /**< Arrange children vertically. */
} n00b_flex_direction_t;

/**
 * @brief Main-axis content justification.
 */
typedef enum : uint8_t {
    N00B_JUSTIFY_START,         /**< Pack children at the start. */
    N00B_JUSTIFY_END,           /**< Pack children at the end. */
    N00B_JUSTIFY_CENTER,        /**< Center children. */
    N00B_JUSTIFY_SPACE_BETWEEN, /**< Even space between children. */
    N00B_JUSTIFY_SPACE_AROUND,  /**< Even space around each child. */
    N00B_JUSTIFY_SPACE_EVENLY,  /**< Equal space between and around children. */
} n00b_justify_t;

/**
 * @brief Cross-axis alignment for flex items and containers.
 *
 * AUTO is first (0) so zero-initialized `align_self` inherits from
 * the container.  For container `align`, AUTO resolves to STRETCH
 * (the CSS flexbox default).
 */
typedef enum : uint8_t {
    N00B_ALIGN_AUTO_CROSS,    /**< Inherit / default (resolves to STRETCH for containers). */
    N00B_ALIGN_STRETCH_CROSS, /**< Stretch to fill the cross axis. */
    N00B_ALIGN_START_CROSS,   /**< Align to the start of the cross axis. */
    N00B_ALIGN_END_CROSS,     /**< Align to the end of the cross axis. */
    N00B_ALIGN_CENTER_CROSS,  /**< Center on the cross axis. */
} n00b_align_items_t;

/**
 * @brief Per-item flex layout properties.
 */
typedef struct n00b_flex_props_t {
    float              grow;       /**< Flex-grow factor (0 = do not grow). */
    float              shrink;     /**< Flex-shrink factor (0 = do not shrink). */
    int32_t            basis;      /**< Flex-basis in pixels (0 = auto / use preferred size). */
    n00b_align_items_t align_self; /**< Cross-axis override (AUTO = inherit from container). */
} n00b_flex_props_t;

/**
 * @brief Flex container layout parameters.
 */
typedef struct n00b_flex_container_t {
    n00b_flex_direction_t direction;
    n00b_justify_t        justify;
    n00b_align_items_t    align;
    int32_t               gap; /**< Gap between children in cells (converted to pixels at layout time). */
} n00b_flex_container_t;

/**
 * @brief Padding in cells (converted to pixels at layout time).
 */
typedef struct n00b_padding_t {
    int32_t top;
    int32_t right;
    int32_t bottom;
    int32_t left;
} n00b_padding_t;
