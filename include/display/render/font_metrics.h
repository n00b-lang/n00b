/**
 * @file font_metrics.h
 * @brief Font metrics provider for pixel-coordinate text measurement.
 *
 * Widgets need pixel-accurate text measurements for positioning
 * draw commands.  The canvas owns a font metrics provider, obtained
 * from the backend at initialization.
 *
 * - **Notcurses**: wraps FreeType face metrics.
 * - **Cocoa**: wraps CTFont metrics.
 * - **ANSI/cell fallback**: `display_width(text) * cell_px_w`.
 *
 * ### Related modules
 *
 * - `render/canvas.h` — canvas owns the metrics provider
 * - `render/plane.h` — convenience wrappers for plane-level measurement
 * - `render/draw_cmd.h` — draw commands positioned using these measurements
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "text/strings/text_style.h"

// ====================================================================
// Font metrics provider
// ====================================================================

/**
 * @brief Callback-based font metrics interface.
 *
 * Each backend provides an implementation.  The `ctx` pointer is
 * backend-specific opaque state (e.g., FreeType face, CTFont).
 */
typedef struct n00b_font_metrics_provider_t {
    /**
     * @brief Measure the pixel width of a styled text string.
     * @param ctx   Backend-specific context.
     * @param text  String to measure.
     * @param style Style affecting font selection (nullptr = default).
     * @return Pixel width.
     */
    int32_t (*text_width)(void *ctx, n00b_string_t *text,
                           n00b_text_style_t *style);

    /**
     * @brief Get the line height for the given style.
     * @param ctx   Backend-specific context.
     * @param style Style affecting font selection (nullptr = default).
     * @return Pixel height of one line.
     */
    int32_t (*line_height)(void *ctx, n00b_text_style_t *style);

    /**
     * @brief Get the ascent for the given style.
     * @param ctx   Backend-specific context.
     * @param style Style affecting font selection (nullptr = default).
     * @return Pixel ascent (baseline to top of tallest glyph).
     */
    int32_t (*ascent)(void *ctx, n00b_text_style_t *style);

    void *ctx; /**< Opaque backend context. */
} n00b_font_metrics_provider_t;

// ====================================================================
// Fallback (cell-based) metrics provider
// ====================================================================

/**
 * @brief Create a cell-based fallback metrics provider.
 *
 * Text width = `n00b_unicode_display_width(text) * cell_px_w`.
 * Line height = `cell_px_h`.  Ascent = `cell_px_h`.
 *
 * @param cell_px_w Pixel width of one cell column.
 * @param cell_px_h Pixel height of one cell row.
 * @return Initialized metrics provider (stack-copyable).
 */
extern n00b_font_metrics_provider_t
n00b_font_metrics_fallback(int32_t cell_px_w, int32_t cell_px_h);
