/**
 * @file backend.h
 * @brief Renderer backend interface (vtable).
 *
 * Defines the `n00b_renderer_vtable_t` that every renderer backend must
 * implement.  The vtable has required and optional slots — optional
 * slots that are nullptr are simply skipped by the canvas.
 *
 * ### Related modules
 *
 * - `render/canvas.h` — canvas that dispatches through the vtable
 * - `render/backend_registry.h` — registration and dynamic loading
 */
#pragma once

#include "n00b.h"
#include "display/render/cell.h"
#include "display/render/font_metrics.h"
#include "display/event.h"
#include "conduit/rw.h"
#include "core/buffer.h"

// ====================================================================
// Renderer capabilities
// ====================================================================

/**
 * @brief Capability flags reported by a renderer backend.
 *
 * The compositor uses these to decide on graceful degradation:
 * downgrading Unicode to ASCII, quantizing colors, stripping
 * decorations, etc.
 */
typedef enum : uint32_t {
    N00B_RCAP_NONE          = 0,
    N00B_RCAP_COLOR_BASIC   = 1 <<  0,
    N00B_RCAP_COLOR_256     = 1 <<  1,
    N00B_RCAP_COLOR_24BIT   = 1 <<  2,
    N00B_RCAP_BOLD          = 1 <<  3,
    N00B_RCAP_ITALIC        = 1 <<  4,
    N00B_RCAP_UNDERLINE     = 1 <<  5,
    N00B_RCAP_STRIKETHROUGH = 1 <<  6,
    N00B_RCAP_MOUSE         = 1 <<  7,  /**< Backend supports mouse input. */
    N00B_RCAP_DIM           = 1 <<  9,
    N00B_RCAP_CURSOR_MOVE   = 1 << 10,
    N00B_RCAP_ALT_SCREEN    = 1 << 11,
    N00B_RCAP_UNICODE       = 1 << 14,
    N00B_RCAP_WIDE_CHARS    = 1 << 15,
    N00B_RCAP_PIXEL_COORDS  = 1 << 19,
    N00B_RCAP_FONT_METRICS  = 1 << 20,
    N00B_RCAP_DIFF_RENDER   = 1 << 21,
    N00B_RCAP_GUI_EXT       = 1 << 22,
    N00B_RCAP_MANAGES_TTY   = 1 << 23,  /**< Backend owns terminal state (raw mode, alt screen). */
} n00b_render_cap_t;

// ====================================================================
// Render size
// ====================================================================

/**
 * @brief Output dimensions from a backend.
 */
typedef struct n00b_render_size_t {
    n00b_isize_t cols;
    n00b_isize_t rows;
    n00b_isize_t pixel_w;       /**< 0 = N/A */
    n00b_isize_t pixel_h;       /**< 0 = N/A */
    n00b_isize_t cell_pixel_w;  /**< 0 = N/A */
    n00b_isize_t cell_pixel_h;  /**< 0 = N/A */
} n00b_render_size_t;

// ====================================================================
// ABI version
// ====================================================================

#define N00B_RENDERER_ABI_VERSION 3

// ====================================================================
// Forward declarations
// ====================================================================

typedef struct n00b_plane_t n00b_plane_t;
typedef struct n00b_composite_entry_t n00b_composite_entry_t;

// ====================================================================
// Renderer vtable
// ====================================================================

/**
 * @brief Backend implementor contract.
 *
 * Every renderer backend is a `n00b_renderer_vtable_t`.  This is the
 * **only** interface a backend author implements.
 *
 * Required slots: `init`, `destroy`, `capabilities`, `get_size`,
 * `render_planes`, `flush`.
 *
 * Optional slots (set to nullptr if unsupported):
 * `render_frame`, `cursor_set_visible`, `cursor_move`,
 * `alt_screen_enter`, `alt_screen_leave`, `on_resize`, `prepare_gui`.
 *
 * The canvas dispatches to `render_planes` for all rendering.
 * `render_frame` exists for internal use by backends that composite
 * to a cell grid before output.
 */
typedef struct n00b_renderer_vtable_t {
    const char *name;
    uint32_t    version;

    // Required.
    void             *(*init)(n00b_conduit_topic_t(n00b_buffer_t *) *output);
    void              (*destroy)(void *ctx);
    n00b_render_cap_t (*capabilities)(void *ctx);
    n00b_render_size_t (*get_size)(void *ctx);
    void              (*render_frame)(void *ctx,
                                      n00b_rcell_t *cells,
                                      n00b_isize_t  rows,
                                      n00b_isize_t  cols,
                                      n00b_rcell_t *prev_cells);
    void              (*flush)(void *ctx);

    /**
     * @brief Plane-based rendering (required).
     *
     * Receives z-sorted composite entries (planes with absolute
     * pixel positions and clip rects) and renders each plane.
     * The canvas always dispatches here.
     *
     * @param ctx           Backend context.
     * @param entries       Z-sorted composite entries (low-z first, pixel coords).
     * @param count         Number of entries.
     * @param total_rows    Frame height in pixels.
     * @param total_cols    Frame width in pixels.
     * @param default_style Default style for empty regions.
     * @param caps          Backend capabilities for degradation.
     */
    void              (*render_planes)(void                         *ctx,
                                       const n00b_composite_entry_t *entries,
                                       n00b_isize_t                  count,
                                       n00b_isize_t                  total_rows,
                                       n00b_isize_t                  total_cols,
                                       n00b_text_style_t            *default_style,
                                       n00b_render_cap_t             caps);

    // Optional.
    void (*cursor_set_visible)(void *ctx, bool visible);
    void (*cursor_move)(void *ctx, n00b_isize_t row, n00b_isize_t col);
    void (*alt_screen_enter)(void *ctx);
    void (*alt_screen_leave)(void *ctx);
    void (*on_resize)(void *ctx,
                      void (*cb)(n00b_isize_t, n00b_isize_t, void *),
                      void *user_ctx);
    void (*prepare_gui)(void *ctx, n00b_plane_t **planes, n00b_isize_t n);

    /**
     * @brief Get a font metrics provider from the backend (optional).
     *
     * If non-nullptr, the canvas uses this for text measurement instead
     * of the cell-based fallback.  Pixel backends (NC, Cocoa) should
     * implement this.
     *
     * @param ctx Backend context.
     * @return    Initialized font metrics provider.
     */
    n00b_font_metrics_provider_t (*get_font_metrics)(void *ctx);

    /**
     * @brief Poll for an input event with timeout.
     *
     * @param ctx        Backend context.
     * @param timeout_ms Max milliseconds to wait (-1 = block, 0 = non-blocking).
     * @param out        Output event (set to NONE if no event).
     * @return           true if an event was produced.
     */
    bool (*poll_event)(void *ctx, int32_t timeout_ms, n00b_event_t *out);
} n00b_renderer_vtable_t;

// Built-in backends.

/** Full-screen ANSI backend with cursor positioning and diff rendering.
 *  Best suited for TUI apps that own the alternate screen. */
extern const n00b_renderer_vtable_t n00b_renderer_ansi;

/** Inline ANSI backend: SGR styling, sequential line-by-line output,
 *  no cursor positioning.  Use for CLI tools that print and exit. */
extern const n00b_renderer_vtable_t n00b_renderer_ansi_inline;

/** No-op backend reporting zero capabilities. */
extern const n00b_renderer_vtable_t n00b_renderer_dumb;

/** Buffer-capture backend for testing (no terminal output). */
extern const n00b_renderer_vtable_t n00b_renderer_stream;

#if defined(__APPLE__)
/** Native Cocoa/macOS backend with Core Text rendering. */
extern const n00b_renderer_vtable_t n00b_renderer_cocoa;
#endif

#if defined(N00B_HAVE_NOTCURSES)
/** Notcurses backend with optional FreeType pixel rendering. */
extern const n00b_renderer_vtable_t n00b_renderer_notcurses;
#endif
