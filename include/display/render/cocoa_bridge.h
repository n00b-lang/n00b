/**
 * @file cocoa_bridge.h
 * @brief Minimal type redeclarations for the Cocoa backend ObjC files.
 *
 * The canonical n00b headers use ncc language extensions (`_generic_struct`,
 * `typeid()`, `_kargs`, `nullptr`) that Apple's Objective-C compiler cannot
 * parse.  This header provides standalone redeclarations of the specific
 * types, enums, macros, and inline helpers that `backend_cocoa.m` and
 * `test_cocoa_backend.m` need, without including any ncc-extended header.
 *
 * **Do not include this from C files compiled through ncc** — use the
 * canonical headers instead.
 *
 * @note Struct layouts here must be kept in sync with their canonical
 *       definitions.  If you add a field to `n00b_rcell_t`, `n00b_text_style_t`,
 *       etc., update this file too.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ====================================================================
// Primitive typedefs (from n00b.h)
// ====================================================================

typedef uint32_t n00b_isize_t;
typedef int32_t  n00b_color_t;
typedef uint32_t n00b_codepoint_t;

// ====================================================================
// Color macros (from text/strings/text_style.h)
// ====================================================================

#define N00B_COLOR_VALID_BIT ((n00b_color_t)(1 << 31))
#define n00b_color_is_set(c) (((c) & N00B_COLOR_VALID_BIT) != 0)
#define n00b_color_rgb(c)    ((c) & 0x00FFFFFF)
#define n00b_color_make(rgb) ((n00b_color_t)((rgb) | N00B_COLOR_VALID_BIT))

// ====================================================================
// Tristate + font hint enums (from text/strings/text_style.h)
// ====================================================================

typedef enum {
    N00B_TRI_UNSPECIFIED = 0,
    N00B_TRI_NO,
    N00B_TRI_YES,
} n00b_tristate_t;

typedef enum {
    N00B_FONT_DEFAULT = 0,
    N00B_FONT_MONO,
    N00B_FONT_SERIF,
    N00B_FONT_SANS,
} n00b_font_hint_t;

typedef enum {
    N00B_TCASE_NONE  = 0,
    N00B_TCASE_UPPER = 1,
    N00B_TCASE_LOWER = 2,
    N00B_TCASE_TITLE = 3,
} n00b_text_case_t;

// ====================================================================
// n00b_text_style_t (from text/strings/text_style.h:80)
// ====================================================================

typedef struct n00b_text_style_t {
    n00b_tristate_t   bold;
    n00b_tristate_t   italic;
    n00b_tristate_t   underline;
    n00b_tristate_t   double_underline;
    n00b_tristate_t   strikethrough;
    n00b_tristate_t   reverse;
    n00b_tristate_t   dim;
    n00b_tristate_t   blink;
    n00b_text_case_t  text_case;
    n00b_font_hint_t  font_hint;
    int8_t            font_index;
    int8_t            fg_palette_ix;
    int8_t            bg_palette_ix;
    n00b_color_t      fg_rgb;
    n00b_color_t      bg_rgb;
    int16_t           font_size;
} n00b_text_style_t;

// ====================================================================
// Palette (from text/strings/theme.h)
// ====================================================================

typedef int n00b_palette_ix_t;
#define N00B_PAL_SIZE 31

extern n00b_color_t n00b_theme_resolve_color(n00b_palette_ix_t ix);

// ====================================================================
// Cell flags + n00b_rcell_t (from display/render/cell.h)
// ====================================================================

typedef enum : uint8_t {
    N00B_CELL_EMPTY     = 0,
    N00B_CELL_OCCUPIED  = 1 << 0,
    N00B_CELL_WIDE_CONT = 1 << 1,
    N00B_CELL_DIRTY     = 1 << 2,
    N00B_CELL_BORDER    = 1 << 3,
    N00B_CELL_PADDING   = 1 << 4,
} n00b_cell_flags_t;

typedef struct n00b_rcell_t {
    char               grapheme[16];
    n00b_text_style_t *style;
    uint8_t            grapheme_len;
    uint8_t            display_width;
    n00b_cell_flags_t  flags;
    uint8_t            _pad[5];
} n00b_rcell_t;

static inline bool
n00b_rcell_equal(const n00b_rcell_t *a, const n00b_rcell_t *b)
{
    if (a->grapheme_len != b->grapheme_len) return false;
    if (a->display_width != b->display_width) return false;
    if (a->style != b->style) return false;
    if (a->grapheme_len > 0
        && memcmp(a->grapheme, b->grapheme, a->grapheme_len) != 0) {
        return false;
    }
    return true;
}

static inline void
n00b_rcell_set_ascii(n00b_rcell_t *cell, char ch, n00b_text_style_t *style)
{
    cell->grapheme[0]   = ch;
    cell->grapheme[1]   = '\0';
    cell->grapheme_len  = 1;
    cell->display_width = 1;
    cell->style         = style;
    cell->flags         = (n00b_cell_flags_t)(N00B_CELL_OCCUPIED | N00B_CELL_DIRTY);
}

extern void n00b_rcell_set_codepoint(n00b_rcell_t      *cell,
                                      n00b_codepoint_t   cp,
                                      uint8_t            width,
                                      n00b_text_style_t *style);

// ====================================================================
// Renderer capabilities + size (from display/render/backend.h)
// ====================================================================

typedef enum : uint32_t {
    N00B_RCAP_NONE          = 0,
    N00B_RCAP_COLOR_BASIC   = 1 <<  0,
    N00B_RCAP_COLOR_256     = 1 <<  1,
    N00B_RCAP_COLOR_24BIT   = 1 <<  2,
    N00B_RCAP_BOLD          = 1 <<  3,
    N00B_RCAP_ITALIC        = 1 <<  4,
    N00B_RCAP_UNDERLINE     = 1 <<  5,
    N00B_RCAP_STRIKETHROUGH = 1 <<  6,
    N00B_RCAP_DIM           = 1 <<  9,
    N00B_RCAP_CURSOR_MOVE   = 1 << 10,
    N00B_RCAP_ALT_SCREEN    = 1 << 11,
    N00B_RCAP_UNICODE       = 1 << 14,
    N00B_RCAP_WIDE_CHARS    = 1 << 15,
    N00B_RCAP_PIXEL_COORDS  = 1 << 19,
    N00B_RCAP_FONT_METRICS  = 1 << 20,
    N00B_RCAP_DIFF_RENDER   = 1 << 21,
    N00B_RCAP_GUI_EXT       = 1 << 22,
    N00B_RCAP_MANAGES_TTY   = 1 << 23,
} n00b_render_cap_t;

typedef struct n00b_render_size_t {
    n00b_isize_t cols;
    n00b_isize_t rows;
    n00b_isize_t pixel_w;
    n00b_isize_t pixel_h;
    n00b_isize_t cell_pixel_w;
    n00b_isize_t cell_pixel_h;
} n00b_render_size_t;

#define N00B_RENDERER_ABI_VERSION 1

// ====================================================================
// Event types (from display/event.h)
// ====================================================================

typedef enum : uint8_t {
    N00B_EVENT_NONE   = 0,
    N00B_EVENT_KEY    = 1,
    N00B_EVENT_RESIZE = 2,
} n00b_event_type_t;

typedef enum : uint32_t {
    N00B_KEY_NONE       = 0,
    N00B_KEY_UP         = 0x110000,
    N00B_KEY_DOWN       = 0x110001,
    N00B_KEY_LEFT       = 0x110002,
    N00B_KEY_RIGHT      = 0x110003,
    N00B_KEY_HOME       = 0x110004,
    N00B_KEY_END        = 0x110005,
    N00B_KEY_PAGE_UP    = 0x110006,
    N00B_KEY_PAGE_DOWN  = 0x110007,
    N00B_KEY_INSERT     = 0x110008,
    N00B_KEY_DELETE     = 0x110009,
    N00B_KEY_BACKSPACE  = 0x11000A,
    N00B_KEY_TAB        = 0x11000B,
    N00B_KEY_ENTER      = 0x11000C,
    N00B_KEY_ESCAPE     = 0x11000D,
    N00B_KEY_F1         = 0x110010,
    N00B_KEY_F2         = 0x110011,
    N00B_KEY_F3         = 0x110012,
    N00B_KEY_F4         = 0x110013,
    N00B_KEY_F5         = 0x110014,
    N00B_KEY_F6         = 0x110015,
    N00B_KEY_F7         = 0x110016,
    N00B_KEY_F8         = 0x110017,
    N00B_KEY_F9         = 0x110018,
    N00B_KEY_F10        = 0x110019,
    N00B_KEY_F11        = 0x11001A,
    N00B_KEY_F12        = 0x11001B,
} n00b_key_t;

typedef enum : uint8_t {
    N00B_MOD_NONE  = 0,
    N00B_MOD_SHIFT = 0x01,
    N00B_MOD_CTRL  = 0x02,
    N00B_MOD_ALT   = 0x04,
} n00b_key_mod_t;

typedef struct n00b_event_t {
    n00b_event_type_t type;
    union {
        struct { uint32_t key; n00b_key_mod_t mods; } key;
        struct { n00b_isize_t rows; n00b_isize_t cols; } resize;
    };
} n00b_event_t;

// ====================================================================
// Forward-declare opaque plane/box types used by the vtable
// ====================================================================

typedef struct n00b_plane_t    n00b_plane_t;
typedef struct n00b_box_props_t n00b_box_props_t;

// ====================================================================
// Renderer vtable (from display/render/backend.h)
//
// The canonical `init` slot takes `n00b_conduit_topic_t(n00b_buffer_t *) *`
// which expands through ncc's typeid().  For ObjC we use `void *`.
// ====================================================================

typedef struct n00b_renderer_vtable_t {
    const char *name;
    uint32_t    version;

    void             *(*init)(void *output);
    void              (*destroy)(void *ctx);
    n00b_render_cap_t (*capabilities)(void *ctx);
    n00b_render_size_t (*get_size)(void *ctx);
    void              (*render_frame)(void *ctx,
                                      n00b_rcell_t *cells,
                                      n00b_isize_t  rows,
                                      n00b_isize_t  cols,
                                      n00b_rcell_t *prev_cells);
    void              (*flush)(void *ctx);

    void (*cursor_set_visible)(void *ctx, bool visible);
    void (*cursor_move)(void *ctx, n00b_isize_t row, n00b_isize_t col);
    void (*alt_screen_enter)(void *ctx);
    void (*alt_screen_leave)(void *ctx);
    void (*on_resize)(void *ctx,
                      void (*cb)(n00b_isize_t, n00b_isize_t, void *),
                      void *user_ctx);
    void (*prepare_gui)(void *ctx, n00b_plane_t **planes, n00b_isize_t n);
    bool (*poll_event)(void *ctx, int32_t timeout_ms, n00b_event_t *out);
} n00b_renderer_vtable_t;

extern const n00b_renderer_vtable_t n00b_renderer_cocoa;

// ====================================================================
// Cocoa GUI extension types (from display/render/backend_cocoa.h)
// ====================================================================

typedef enum : uint8_t {
    N00B_BLEND_NORMAL     = 0,
    N00B_BLEND_MULTIPLY   = 1,
    N00B_BLEND_SCREEN     = 2,
    N00B_BLEND_OVERLAY    = 3,
    N00B_BLEND_SOFT_LIGHT = 4,
} n00b_cocoa_blend_mode_t;

typedef enum : uint8_t {
    N00B_GRADIENT_NONE        = 0,
    N00B_GRADIENT_TOP_BOTTOM  = 1,
    N00B_GRADIENT_LEFT_RIGHT  = 2,
    N00B_GRADIENT_DIAGONAL    = 3,
    N00B_GRADIENT_RADIAL      = 4,
} n00b_cocoa_gradient_dir_t;

typedef struct cocoa_gui_ext_t {
    float                     shadow_offset_x;
    float                     shadow_offset_y;
    float                     shadow_blur;
    n00b_color_t              shadow_color;
    float                     corner_radius;
    float                     opacity;
    n00b_cocoa_gradient_dir_t gradient_dir;
    n00b_color_t              gradient_start;
    n00b_color_t              gradient_end;
    n00b_cocoa_blend_mode_t   blend_mode;
} cocoa_gui_ext_t;

extern cocoa_gui_ext_t *n00b_cocoa_gui_ext_new(void);

// ====================================================================
// Minimal plane/box struct for test access to gui_ext
//
// Only the fields actually used by the Cocoa backend are declared.
// The real structs have many more fields, but we only need the ones
// the backend reads: plane->box->gui_ext.
// ====================================================================

struct n00b_box_props_t {
    void *_border_theme;
    n00b_text_style_t *_border_style;
    n00b_text_style_t *_fill_style;
    n00b_text_style_t *_text_style;
    uint32_t           _borders;
    int8_t             _pad_top;
    int8_t             _pad_bottom;
    int8_t             _pad_left;
    int8_t             _pad_right;
    int8_t             _margin_top;
    int8_t             _margin_bottom;
    int8_t             _margin_left;
    int8_t             _margin_right;
    int32_t            _alignment;
    int32_t            _overflow;
    void              *_state_styles[4];
    void              *gui_ext;
};

struct n00b_plane_t {
    void              *_name;
    struct n00b_plane_t *_parent;
    void              *_children;
    n00b_rcell_t      *_grid;
    n00b_isize_t       _total_rows;
    n00b_isize_t       _total_cols;
    n00b_isize_t       _vp_row;
    n00b_isize_t       _vp_col;
    n00b_isize_t       _vp_rows;
    n00b_isize_t       _vp_cols;
    int32_t            _x;
    int32_t            _y;
    int32_t            _z;
    n00b_isize_t       _cursor_row;
    n00b_isize_t       _cursor_col;
    n00b_isize_t       _ring_base;
    n00b_isize_t       _ring_len;
    n00b_box_props_t  *box;
    void              *_default_style;
    uint8_t            _scroll_mode;
    uint8_t            _widget_state;
    uint16_t           _flags;
    const void        *_widget_vtable;
    void              *_widget_data;
    // ... remaining fields omitted (not accessed by Cocoa backend)
};

// ====================================================================
// Runtime init/shutdown (from core/runtime.h)
//
// n00b_runtime_t is opaque from ObjC — we only need to declare it
// and pass a pointer to n00b_init().
// ====================================================================

typedef struct n00b_runtime_t n00b_runtime_t;

/**
 * Bridge functions compiled through ncc so that _kargs-based
 * n00b_init()/n00b_shutdown() are called correctly.
 * @param rt_buf  Heap buffer >= 512 KB, zeroed.
 */
extern void n00b_cocoa_bridge_init(void *rt_buf, int argc, char *argv[]);
extern void n00b_cocoa_bridge_shutdown(void);
extern void n00b_cocoa_run_loop_pump(double seconds);
