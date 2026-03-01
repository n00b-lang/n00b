/**
 * @file backend_notcurses.h
 * @brief Notcurses renderer backend with optional FreeType pixel rendering.
 *
 * Provides a terminal renderer backend that uses notcurses for output.
 * When FreeType is available and the terminal supports pixel blitting
 * (Sixel, Kitty graphics protocol), text is rasterized at the pixel
 * level for true typographic rendering.  Falls back to cell-based
 * rendering through notcurses APIs otherwise.
 *
 * ### Build requirements
 *
 * - **notcurses >= 3.0.0** (required, gated on `enable_notcurses`)
 * - **FreeType 2** (optional, gated on `enable_freetype`)
 *
 * ### Related modules
 *
 * - `render/backend.h` — vtable interface this backend implements
 * - `render/backend_registry.h` — registration and lookup
 * - `render/cell.h` — render cell type consumed by `render_frame`
 */
#pragma once

#include "n00b.h"
#include "display/render/backend.h"

/**
 * @brief Notcurses renderer vtable.
 *
 * Implements the full `n00b_renderer_vtable_t` contract.
 * Pixel rendering is used when both FreeType and terminal pixel
 * support are available; otherwise falls back to cell-based output.
 */
extern const n00b_renderer_vtable_t n00b_renderer_notcurses;

/**
 * @brief Check whether the notcurses backend has pixel support.
 * @param ctx  Backend context returned by `init`.
 * @return     true if pixel blitting (Sixel/Kitty) is available.
 *
 * @pre `ctx` was returned by `n00b_renderer_notcurses.init()`.
 */
bool n00b_notcurses_has_pixel_support(void *ctx);

/**
 * @brief Check whether FreeType font rendering is available.
 * @param ctx  Backend context returned by `init`.
 * @return     true if FreeType was compiled in and initialized.
 *
 * @pre `ctx` was returned by `n00b_renderer_notcurses.init()`.
 */
bool n00b_notcurses_has_freetype(void *ctx);
