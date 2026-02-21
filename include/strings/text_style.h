#pragma once
/** @file text_style.h
 *  @brief Core type definitions for the rich-text style system.
 *
 *  Defines the data structures used to attach abstract styling
 *  information to `n00b_string_t` values.  No rendering logic
 *  lives here -- these types describe *what* styling is requested,
 *  not *how* it is realized on a particular output device.
 *
 *  ### Key types
 *
 *  - `n00b_text_style_t` -- a single style description (bold, color, etc.)
 *  - `n00b_style_record_t` -- a style + byte-range within a string
 *  - `n00b_string_style_info_t` -- the styling metadata attached to a string
 *
 *  ### Related modules
 *
 *  - `strings/style_ops.h` -- construct, merge, compare styles
 *  - `strings/string_style.h` -- attach/query styles on `n00b_string_t`
 *  - `core/string.h` -- the `void *styling` field in `n00b_string_t`
 */

#include "n00b.h"
#include "core/option.h"

// ===================================================================
// Tristate for style inheritance
// ===================================================================

/** @brief Tristate value for style properties that support inheritance.
 *
 *  `N00B_TRI_UNSPECIFIED` means "inherit from parent/base"; the other
 *  two are explicit on/off overrides.
 */
typedef enum {
    N00B_TRI_UNSPECIFIED = 0,
    N00B_TRI_NO,
    N00B_TRI_YES,
} n00b_tristate_t;

// ===================================================================
// Text case transformation
// ===================================================================

/** @brief Text case transformation applied during rendering. */
typedef enum {
    N00B_TEXT_CASE_NONE  = 0, /**< As-is (no transformation) */
    N00B_TEXT_CASE_UPPER,     /**< Uppercase */
    N00B_TEXT_CASE_LOWER,     /**< Lowercase */
    N00B_TEXT_CASE_TITLE,     /**< Title case */
    N00B_TEXT_CASE_CAPS,      /**< Small-caps / all-caps */
} n00b_text_case_t;

// ===================================================================
// Font hint
// ===================================================================

/** @brief Broad font-family hint for the rendering layer. */
typedef enum {
    N00B_FONT_DEFAULT = 0, /**< Use environment default */
    N00B_FONT_MONO,        /**< Monospace */
    N00B_FONT_SERIF,       /**< Serif */
    N00B_FONT_SANS,        /**< Sans-serif */
} n00b_font_hint_t;

// ===================================================================
// Text style
// ===================================================================

/** @brief Abstract text style descriptor.
 *
 *  Every field uses a tristate, enum, or sentinel value so that
 *  "unspecified" is always distinguishable from an explicit setting.
 *  This allows styles to be merged/overlaid: an overlay's unspecified
 *  fields inherit from a base style.
 *
 *  Color fields use the high bit (`1 << 31`) as a validity flag;
 *  when the flag is clear the color is considered unset.
 */
typedef struct n00b_text_style_t {
    // --- Tristate decorations ------------------------------------------
    n00b_tristate_t   bold;
    n00b_tristate_t   italic;
    n00b_tristate_t   underline;
    n00b_tristate_t   double_underline;
    n00b_tristate_t   strikethrough;
    n00b_tristate_t   reverse;
    n00b_tristate_t   dim;
    n00b_tristate_t   blink;

    // --- Transforms / hints -------------------------------------------
    n00b_text_case_t  text_case;
    n00b_font_hint_t  font_hint;

    /** Font table index; -1 = use `font_hint` instead. */
    int8_t            font_index;

    /** Foreground palette index; -1 = unset. */
    int8_t            fg_palette_ix;
    /** Background palette index; -1 = unset. */
    int8_t            bg_palette_ix;

    /** Direct foreground color.  High bit set = valid. */
    n00b_color_t      fg_rgb;
    /** Direct background color.  High bit set = valid. */
    n00b_color_t      bg_rgb;

    /** Point size for GUI backends (0 = default/inherit).
     *  Terminal backends ignore this field entirely. */
    int16_t           font_size;
} n00b_text_style_t;

/** @brief Sentinel: high bit marks a direct color as valid. */
#define N00B_COLOR_VALID_BIT ((n00b_color_t)(1 << 31))

/** @brief Test whether a direct color value has been set. */
#define n00b_color_is_set(c) (((c) & N00B_COLOR_VALID_BIT) != 0)

/** @brief Extract the 24-bit RGB from a direct color value. */
#define n00b_color_rgb(c) ((c) & 0x00FFFFFF)

/** @brief Construct a valid direct color from 24-bit RGB. */
#define n00b_color_make(rgb) ((n00b_color_t)((rgb) | N00B_COLOR_VALID_BIT))

// ===================================================================
// Option type for size_t
// ===================================================================

// size_t option type declared in core/option.h.

// ===================================================================
// Style record
// ===================================================================

/** @brief A style applied to a byte range within a string.
 *
 *  @c start is a byte offset into the string's UTF-8 data.
 *  @c end is either `n00b_option_set(size_t, N)` for a closed range
 *  or `n00b_option_none(size_t)` meaning "extends to end of string".
 *
 *  When @c info is non-nullptr the style is fully resolved.  When @c info
 *  is nullptr and @c tag is non-nullptr, the style is *deferred*: the tag is
 *  looked up on first access via `n00b_str_style_lookup()` (plain name)
 *  or `n00b_str_role_lookup()` (tag starting with `@`), and the result
 *  is cached in @c info.
 */
typedef struct n00b_style_record_t {
    n00b_text_style_t      *info;
    const char             *tag;
    size_t                  start;
    n00b_option_t(size_t)   end;
} n00b_style_record_t;

// ===================================================================
// String style info (flexible array)
// ===================================================================

/** @brief Aggregate styling metadata attached to an `n00b_string_t`.
 *
 *  Stored in the `void *styling` field of `n00b_string_t`.
 *  The `base_style` (if non-nullptr) is the default style for the whole
 *  string; the `styles` array contains per-range overrides.
 */
typedef struct n00b_string_style_info_t {
    int64_t                 num_styles;
    n00b_text_style_t      *base_style;
    n00b_style_record_t     styles[];
} n00b_string_style_info_t;
