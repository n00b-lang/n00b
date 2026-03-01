/*
 * Notcurses renderer backend with optional FreeType pixel rendering.
 *
 * When FreeType is available and the terminal supports pixel blitting
 * (Sixel/Kitty), text is rasterized into RGBA buffers and blitted via
 * ncvisual.  Otherwise, cells are rendered through notcurses' standard
 * character APIs (ncplane_putstr_yx, ncplane_set_channels, etc.).
 *
 * This file is compiled through ncc as a separate static library,
 * gated on `conf_notcurses` in meson.build.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <notcurses/notcurses.h>

#include "n00b.h"
#include "display/render/backend.h"
#include "display/render/cell.h"
#include "display/event.h"
#include "text/strings/text_style.h"

#if N00B_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

// ====================================================================
// System font search paths (used when FreeType is available)
// ====================================================================

#if N00B_HAVE_FREETYPE
static const char *system_font_paths[] = {
#ifdef __APPLE__
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
#endif
    nullptr
};
#endif

// ====================================================================
// Glyph cache (FreeType pixel rendering)
// ====================================================================

#if N00B_HAVE_FREETYPE

#define GLYPH_CACHE_SIZE 4096  // Must be power of 2.
#define GLYPH_CACHE_MASK (GLYPH_CACHE_SIZE - 1)

typedef struct {
    uint32_t codepoint;
    uint32_t fg;
    uint32_t bg;
    uint16_t pixel_h;
    bool     bold;
    bool     occupied;

    // Cached glyph metrics.
    int      advance_x;   // Horizontal advance in pixels.
    int      bitmap_left;
    int      bitmap_top;
    unsigned int bmp_rows;
    unsigned int bmp_width;
    unsigned int bmp_pitch;

    // Cached rendered bitmap (FT_RENDER_MODE_NORMAL, 8-bit alpha).
    uint8_t *alpha_buf;   // bmp_rows * bmp_pitch bytes, or nullptr.
} glyph_cache_entry_t;

typedef struct {
    glyph_cache_entry_t entries[GLYPH_CACHE_SIZE];
} glyph_cache_t;

static uint32_t
glyph_cache_hash(uint32_t cp, uint32_t fg, uint32_t bg,
                 bool bold, uint16_t pixel_h)
{
    // FNV-1a inspired.
    uint32_t h = 2166136261u;
    h ^= cp;       h *= 16777619u;
    h ^= fg;       h *= 16777619u;
    h ^= bg;       h *= 16777619u;
    h ^= (uint32_t)bold;       h *= 16777619u;
    h ^= (uint32_t)pixel_h;   h *= 16777619u;
    return h & GLYPH_CACHE_MASK;
}

static glyph_cache_entry_t *
glyph_cache_lookup(glyph_cache_t *cache, uint32_t cp,
                   uint32_t fg, uint32_t bg,
                   bool bold, uint16_t pixel_h)
{
    uint32_t idx = glyph_cache_hash(cp, fg, bg, bold, pixel_h);

    for (int probe = 0; probe < 16; probe++) {
        glyph_cache_entry_t *e = &cache->entries[(idx + probe) & GLYPH_CACHE_MASK];
        if (!e->occupied) {
            return NULL;
        }
        if (e->codepoint == cp && e->fg == fg && e->bg == bg
            && e->bold == bold && e->pixel_h == pixel_h) {
            return e;
        }
    }
    return NULL;
}

static glyph_cache_entry_t *
glyph_cache_insert(glyph_cache_t *cache, uint32_t cp,
                   uint32_t fg, uint32_t bg,
                   bool bold, uint16_t pixel_h)
{
    uint32_t idx = glyph_cache_hash(cp, fg, bg, bold, pixel_h);

    for (int probe = 0; probe < 16; probe++) {
        glyph_cache_entry_t *e = &cache->entries[(idx + probe) & GLYPH_CACHE_MASK];
        if (!e->occupied) {
            e->codepoint = cp;
            e->fg        = fg;
            e->bg        = bg;
            e->bold      = bold;
            e->pixel_h   = pixel_h;
            e->occupied  = true;
            return e;
        }
        // Already present?
        if (e->codepoint == cp && e->fg == fg && e->bg == bg
            && e->bold == bold && e->pixel_h == pixel_h) {
            return e;
        }
    }
    // Table too full — evict first slot (simple, rare).
    glyph_cache_entry_t *e = &cache->entries[idx];
    free(e->alpha_buf);
    e->codepoint = cp;
    e->fg        = fg;
    e->bg        = bg;
    e->bold      = bold;
    e->pixel_h   = pixel_h;
    e->alpha_buf = NULL;
    return e;
}

static void
glyph_cache_destroy(glyph_cache_t *cache)
{
    if (!cache) {
        return;
    }
    for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
        free(cache->entries[i].alpha_buf);
    }
    free(cache);
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Context
// ====================================================================

typedef struct {
    struct notcurses *nc;
    struct ncplane   *stdplane;
    bool              has_pixel;

#if N00B_HAVE_FREETYPE
    bool              has_freetype;
    FT_Library        ft_lib;
    FT_Face           ft_face;
    glyph_cache_t    *glyph_cache;
#endif

    // Pixel geometry (cached from last query).
    unsigned int      cell_pixel_w;
    unsigned int      cell_pixel_h;

    // Cached row planes for pixel rendering.
    // One child ncplane per row, sized to the full row in pixels.
    // Reused across frames; only recreated on terminal resize.
    struct ncplane  **row_planes;
    unsigned int      row_planes_count;
    unsigned int      row_planes_cols;  // cols at time of creation

    // Resize callback.
    void            (*resize_cb)(n00b_isize_t, n00b_isize_t, void *);
    void             *resize_user;

    // Last known dimensions for resize detection.
    unsigned int      last_rows;
    unsigned int      last_cols;

    // Debug log (written to /tmp/nc_backend.log).
    FILE             *debug_log;
} nc_ctx_t;

// ====================================================================
// FreeType helpers
// ====================================================================

#if N00B_HAVE_FREETYPE

static bool
ft_init(nc_ctx_t *ctx)
{
    if (FT_Init_FreeType(&ctx->ft_lib) != 0) {
        return false;
    }

    // Try to load a system monospace font.
    for (const char **p = system_font_paths; *p; p++) {
        FILE *f = fopen(*p, "rb");
        if (f) {
            fclose(f);
            if (FT_New_Face(ctx->ft_lib, *p, 0, &ctx->ft_face) == 0) {
                ctx->has_freetype = true;
                return true;
            }
        }
    }

    // No font found — FreeType is usable but we have no face.
    FT_Done_FreeType(ctx->ft_lib);
    ctx->ft_lib = nullptr;
    return false;
}

static void
ft_destroy(nc_ctx_t *ctx)
{
    if (ctx->ft_face) {
        FT_Done_Face(ctx->ft_face);
        ctx->ft_face = nullptr;
    }
    if (ctx->ft_lib) {
        FT_Done_FreeType(ctx->ft_lib);
        ctx->ft_lib = nullptr;
    }
    ctx->has_freetype = false;
}

// Decode one UTF-8 codepoint, advance *str.
static uint32_t
utf8_decode(const char **str)
{
    const unsigned char *s = (const unsigned char *)*str;
    uint32_t cp = 0;

    if (s[0] < 0x80) {
        cp   = s[0];
        *str += 1;
    }
    else if ((s[0] & 0xE0) == 0xC0) {
        cp   = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *str += 2;
    }
    else if ((s[0] & 0xF0) == 0xE0) {
        cp   = ((uint32_t)(s[0] & 0x0F) << 12)
             | ((uint32_t)(s[1] & 0x3F) << 6)
             | (s[2] & 0x3F);
        *str += 3;
    }
    else if ((s[0] & 0xF8) == 0xF0) {
        cp   = ((uint32_t)(s[0] & 0x07) << 18)
             | ((uint32_t)(s[1] & 0x3F) << 12)
             | ((uint32_t)(s[2] & 0x3F) << 6)
             | (s[3] & 0x3F);
        *str += 4;
    }
    else {
        *str += 1;
    }

    return cp;
}

/*
 * Rasterize a UTF-8 string into an RGBA buffer using FreeType.
 *
 * Returns a heap-allocated RGBA buffer (caller frees with free()).
 * *out_width and *out_height receive the image dimensions.
 * fg/bg are 0x00RRGGBB packed colors.
 */
static uint8_t *
ft_render_text(nc_ctx_t    *ctx,
               const char  *text,
               int          pixel_height,
               uint32_t     fg_color,
               uint32_t     bg_color,
               bool         bold,
               int         *out_width,
               int         *out_height)
{
    if (!ctx->ft_face || !text || pixel_height <= 0) {
        return nullptr;
    }

    FT_Set_Pixel_Sizes(ctx->ft_face, 0, (FT_UInt)pixel_height);

    // First pass: calculate total width and ascent/descent.
    int total_width = 0;
    int max_ascent  = 0;
    int max_descent = 0;

    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) {
            break;
        }

        FT_UInt glyph_idx = FT_Get_Char_Index(ctx->ft_face, cp);
        if (FT_Load_Glyph(ctx->ft_face, glyph_idx, FT_LOAD_DEFAULT) != 0) {
            continue;
        }

        FT_GlyphSlot slot = ctx->ft_face->glyph;
        total_width += (int)(slot->advance.x >> 6);

        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0) {
            int glyph_top    = slot->bitmap_top;
            int glyph_bottom = slot->bitmap_top - (int)slot->bitmap.rows;

            if (glyph_top > max_ascent) {
                max_ascent = glyph_top;
            }
            if (-glyph_bottom > max_descent) {
                max_descent = -glyph_bottom;
            }
        }
    }

    if (total_width <= 0) {
        return nullptr;
    }

    int height   = max_ascent + max_descent + 2;
    if (height < pixel_height) {
        height = pixel_height;
    }
    int baseline = max_ascent + 1;

    // Allocate RGBA buffer.
    uint8_t *rgba = calloc(1, (size_t)(total_width * height * 4));
    if (!rgba) {
        return nullptr;
    }

    // Fill background.
    uint8_t bg_r = (bg_color >> 16) & 0xFF;
    uint8_t bg_g = (bg_color >> 8) & 0xFF;
    uint8_t bg_b = bg_color & 0xFF;

    for (int i = 0; i < total_width * height; i++) {
        rgba[i * 4 + 0] = bg_r;
        rgba[i * 4 + 1] = bg_g;
        rgba[i * 4 + 2] = bg_b;
        rgba[i * 4 + 3] = 255;
    }

    uint8_t fg_r = (fg_color >> 16) & 0xFF;
    uint8_t fg_g = (fg_color >> 8) & 0xFF;
    uint8_t fg_b = fg_color & 0xFF;

    // Second pass: render glyphs.
    int pen_x = 0;
    p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == 0) {
            break;
        }

        FT_UInt glyph_idx = FT_Get_Char_Index(ctx->ft_face, cp);
        if (FT_Load_Glyph(ctx->ft_face, glyph_idx, FT_LOAD_DEFAULT) != 0) {
            continue;
        }

        FT_GlyphSlot slot = ctx->ft_face->glyph;
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
            pen_x += (int)(slot->advance.x >> 6);
            continue;
        }

        FT_Bitmap *bmp = &slot->bitmap;
        int x0 = pen_x + slot->bitmap_left;
        int y0 = baseline - slot->bitmap_top;

        for (unsigned int by = 0; by < bmp->rows; by++) {
            for (unsigned int bx = 0; bx < bmp->width; bx++) {
                int dx = x0 + (int)bx;
                int dy = y0 + (int)by;

                if (dx < 0 || dx >= total_width || dy < 0 || dy >= height) {
                    continue;
                }

                uint8_t alpha = bmp->buffer[by * (unsigned)bmp->pitch + bx];
                if (alpha == 0) {
                    continue;
                }

                int idx = (dy * total_width + dx) * 4;
                float a = (float)alpha / 255.0f;

                rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                rgba[idx + 3] = 255;
            }
        }

        // Bold synthesis: re-render shifted right by 1 pixel at half alpha.
        if (bold) {
            for (unsigned int by = 0; by < bmp->rows; by++) {
                for (unsigned int bx = 0; bx < bmp->width; bx++) {
                    int dx = x0 + (int)bx + 1;
                    int dy = y0 + (int)by;

                    if (dx < 0 || dx >= total_width || dy < 0 || dy >= height) {
                        continue;
                    }

                    uint8_t alpha = bmp->buffer[by * (unsigned)bmp->pitch + bx];
                    if (alpha == 0) {
                        continue;
                    }

                    int idx = (dy * total_width + dx) * 4;
                    float a = (float)alpha / 255.0f * 0.5f;

                    rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                    rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                    rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                }
            }
        }

        pen_x += (int)(slot->advance.x >> 6);
    }

    *out_width  = total_width;
    *out_height = height;
    return rgba;
}

/*
 * Draw a horizontal line (underline / strikethrough) into an RGBA buffer.
 */
static void
ft_draw_hline(uint8_t *rgba, int img_w, int img_h,
              int y, int x_start, int x_end,
              uint8_t r, uint8_t g, uint8_t b)
{
    if (y < 0 || y >= img_h) {
        return;
    }
    if (x_start < 0) {
        x_start = 0;
    }
    if (x_end > img_w) {
        x_end = img_w;
    }

    for (int x = x_start; x < x_end; x++) {
        int idx = (y * img_w + x) * 4;
        rgba[idx + 0] = r;
        rgba[idx + 1] = g;
        rgba[idx + 2] = b;
        rgba[idx + 3] = 255;
    }
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Row plane cache for pixel rendering
// ====================================================================

/*
 * Ensure we have one child ncplane per row, each spanning the full
 * row width in pixels.  Planes are reused across frames; we only
 * recreate them when the terminal dimensions change.
 */
static void
ensure_row_planes(nc_ctx_t *ctx, unsigned int rows, unsigned int cols)
{
    // Destroy ALL existing row planes each frame.
    // ncvisual_blit(NCBLIT_PIXEL) creates child planes; reusing row
    // planes would accumulate stale blit children.  Destroying and
    // recreating is cheap compared to pixel rendering.
    if (ctx->row_planes) {
        for (unsigned int r = 0; r < ctx->row_planes_count; r++) {
            if (ctx->row_planes[r]) {
                ncplane_destroy(ctx->row_planes[r]);
            }
        }
        if (ctx->row_planes_count != rows) {
            free(ctx->row_planes);
            ctx->row_planes = calloc(rows, sizeof(struct ncplane *));
        }
    }
    else {
        ctx->row_planes = calloc(rows, sizeof(struct ncplane *));
    }

    ctx->row_planes_count = rows;
    ctx->row_planes_cols  = cols;

    for (unsigned int r = 0; r < rows; r++) {
        struct ncplane_options popts = {
            .y    = (int)r,
            .x    = 0,
            .rows = 1,
            .cols = cols,
        };
        ctx->row_planes[r] = ncplane_create(ctx->stdplane, &popts);
    }
}

static void
destroy_row_planes(nc_ctx_t *ctx)
{
    if (!ctx->row_planes) {
        return;
    }
    for (unsigned int r = 0; r < ctx->row_planes_count; r++) {
        if (ctx->row_planes[r]) {
            ncplane_destroy(ctx->row_planes[r]);
        }
    }
    free(ctx->row_planes);
    ctx->row_planes       = nullptr;
    ctx->row_planes_count = 0;
    ctx->row_planes_cols  = 0;
}

// ====================================================================
// Style → notcurses mapping (cell-based fallback)
// ====================================================================

static void
style_to_nc(const n00b_text_style_t *style,
            uint64_t               *channels,
            uint16_t               *stylemask)
{
    *channels  = 0;
    *stylemask = 0;

    if (!style) {
        return;
    }

    if (n00b_color_is_set(style->fg_rgb)) {
        ncchannels_set_fg_rgb(channels, (unsigned)n00b_color_rgb(style->fg_rgb));
    }
    if (n00b_color_is_set(style->bg_rgb)) {
        ncchannels_set_bg_rgb(channels, (unsigned)n00b_color_rgb(style->bg_rgb));
    }

    if (style->bold == N00B_TRI_YES) {
        *stylemask |= NCSTYLE_BOLD;
    }
    if (style->italic == N00B_TRI_YES) {
        *stylemask |= NCSTYLE_ITALIC;
    }
    if (style->underline == N00B_TRI_YES) {
        *stylemask |= NCSTYLE_UNDERLINE;
    }
    if (style->strikethrough == N00B_TRI_YES) {
        *stylemask |= NCSTYLE_STRUCK;
    }
}

// ====================================================================
// Pixel rendering: rasterize an entire row into one RGBA image
// ====================================================================

#if N00B_HAVE_FREETYPE

/*
 * Rasterize all cells in a single row into one RGBA image
 * and blit it onto the row's cached plane.
 *
 * Style changes within the row are handled by switching FreeType
 * colors/attributes per glyph — but the output is a single
 * contiguous RGBA buffer so we only need one ncvisual_blit per row.
 */
/*
 * Per-frame pixel metrics, computed once in render_pixel_frame()
 * and passed to each row to avoid redundant FT_Set_Pixel_Sizes calls.
 */
typedef struct {
    int pixel_h;
    int pixel_w;
    int img_w;      // pixel_w * cols
    int img_h;      // pixel_h
    int baseline;
} pixel_metrics_t;

static void
render_pixel_row(nc_ctx_t              *ctx,
                 const n00b_rcell_t    *cells,
                 n00b_isize_t           cols,
                 n00b_isize_t           row,
                 const pixel_metrics_t *pm,
                 uint8_t               *rgba)
{
    int img_w    = pm->img_w;
    int img_h    = pm->img_h;
    int pixel_w  = pm->pixel_w;
    int baseline = pm->baseline;

    // Clear the reusable row buffer to opaque black.
    for (int i = 0; i < img_w * img_h; i++) {
        rgba[i * 4 + 0] = 0;
        rgba[i * 4 + 1] = 0;
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255;
    }

    // Render each cell at its pixel position.
    for (n00b_isize_t c = 0; c < cols; c++) {
        const n00b_rcell_t *cell = &cells[row * cols + c];

        if (cell->flags & N00B_CELL_WIDE_CONT) {
            continue;
        }

        int cell_x = pixel_w * (int)c;
        int cell_cols = 1;
        if (cell->display_width > 1) {
            cell_cols = cell->display_width;
        }
        int cell_pixel_total = pixel_w * cell_cols;

        // Extract style.
        const n00b_text_style_t *style = cell->style;
        uint32_t fg = 0xFFFFFF;
        uint32_t bg = 0x000000;
        bool     is_bold   = false;
        bool     is_uline  = false;
        bool     is_strike = false;

        if (style) {
            if (n00b_color_is_set(style->fg_rgb)) {
                fg = (uint32_t)n00b_color_rgb(style->fg_rgb);
            }
            if (n00b_color_is_set(style->bg_rgb)) {
                bg = (uint32_t)n00b_color_rgb(style->bg_rgb);
            }
            is_bold   = (style->bold == N00B_TRI_YES);
            is_uline  = (style->underline == N00B_TRI_YES);
            is_strike = (style->strikethrough == N00B_TRI_YES);
        }

        // Fill cell background.
        uint8_t bg_r = (bg >> 16) & 0xFF;
        uint8_t bg_g = (bg >> 8) & 0xFF;
        uint8_t bg_b = bg & 0xFF;

        for (int py = 0; py < img_h; py++) {
            for (int px = cell_x; px < cell_x + cell_pixel_total && px < img_w; px++) {
                int idx = (py * img_w + px) * 4;
                rgba[idx + 0] = bg_r;
                rgba[idx + 1] = bg_g;
                rgba[idx + 2] = bg_b;
                rgba[idx + 3] = 255;
            }
        }

        // Get the text to render.
        const char *text = nullptr;
        if (cell->flags & N00B_CELL_OCCUPIED) {
            text = cell->grapheme;
        }
        if (!text || !*text) {
            // Empty cell — background is already filled.
            continue;
        }

        uint8_t fg_r = (fg >> 16) & 0xFF;
        uint8_t fg_g = (fg >> 8) & 0xFF;
        uint8_t fg_b = fg & 0xFF;

        // Render glyphs for this cell (with glyph cache).
        int pen_x = cell_x;
        const char *p = text;
        while (*p) {
            uint32_t cp = utf8_decode(&p);
            if (cp == 0) {
                break;
            }

            // Look up or populate glyph cache.
            glyph_cache_entry_t *cached = NULL;
            if (ctx->glyph_cache) {
                cached = glyph_cache_lookup(ctx->glyph_cache, cp,
                                            fg, bg, is_bold,
                                            (uint16_t)pm->pixel_h);
            }

            if (!cached) {
                // Cache miss — rasterize with FreeType and store.
                FT_UInt glyph_idx = FT_Get_Char_Index(ctx->ft_face, cp);
                if (FT_Load_Glyph(ctx->ft_face, glyph_idx, FT_LOAD_DEFAULT) != 0) {
                    continue;
                }

                FT_GlyphSlot slot = ctx->ft_face->glyph;
                if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
                    pen_x += (int)(slot->advance.x >> 6);
                    continue;
                }

                FT_Bitmap *bmp = &slot->bitmap;

                if (ctx->glyph_cache) {
                    cached = glyph_cache_insert(ctx->glyph_cache, cp,
                                                fg, bg, is_bold,
                                                (uint16_t)pm->pixel_h);
                    cached->advance_x   = (int)(slot->advance.x >> 6);
                    cached->bitmap_left = slot->bitmap_left;
                    cached->bitmap_top  = slot->bitmap_top;
                    cached->bmp_rows    = bmp->rows;
                    cached->bmp_width   = bmp->width;
                    cached->bmp_pitch   = (unsigned)bmp->pitch;

                    size_t buf_size = (size_t)(bmp->rows * (unsigned)bmp->pitch);
                    if (buf_size > 0) {
                        cached->alpha_buf = malloc(buf_size);
                        if (cached->alpha_buf) {
                            memcpy(cached->alpha_buf, bmp->buffer, buf_size);
                        }
                    } else {
                        cached->alpha_buf = NULL;
                    }
                } else {
                    // No cache — render directly (original path).
                    int x0 = pen_x + slot->bitmap_left;
                    int y0 = baseline - slot->bitmap_top;

                    for (unsigned int by = 0; by < bmp->rows; by++) {
                        for (unsigned int bx = 0; bx < bmp->width; bx++) {
                            int dx = x0 + (int)bx;
                            int dy = y0 + (int)by;
                            if (dx < 0 || dx >= img_w || dy < 0 || dy >= img_h) continue;
                            uint8_t alpha = bmp->buffer[by * (unsigned)bmp->pitch + bx];
                            if (alpha == 0) continue;
                            int idx = (dy * img_w + dx) * 4;
                            float a = (float)alpha / 255.0f;
                            rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                            rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                            rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                        }
                    }
                    if (is_bold) {
                        for (unsigned int by = 0; by < bmp->rows; by++) {
                            for (unsigned int bx = 0; bx < bmp->width; bx++) {
                                int dx = x0 + (int)bx + 1;
                                int dy = y0 + (int)by;
                                if (dx < 0 || dx >= img_w || dy < 0 || dy >= img_h) continue;
                                uint8_t alpha = bmp->buffer[by * (unsigned)bmp->pitch + bx];
                                if (alpha == 0) continue;
                                int idx = (dy * img_w + dx) * 4;
                                float a = (float)alpha / 255.0f * 0.5f;
                                rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                                rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                                rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                            }
                        }
                    }
                    pen_x += (int)(slot->advance.x >> 6);
                    continue;
                }
            }

            // Composite from cached glyph.
            if (cached && cached->alpha_buf) {
                int x0 = pen_x + cached->bitmap_left;
                int y0 = baseline - cached->bitmap_top;

                for (unsigned int by = 0; by < cached->bmp_rows; by++) {
                    for (unsigned int bx = 0; bx < cached->bmp_width; bx++) {
                        int dx = x0 + (int)bx;
                        int dy = y0 + (int)by;
                        if (dx < 0 || dx >= img_w || dy < 0 || dy >= img_h) continue;
                        uint8_t alpha = cached->alpha_buf[by * cached->bmp_pitch + bx];
                        if (alpha == 0) continue;
                        int idx = (dy * img_w + dx) * 4;
                        float a = (float)alpha / 255.0f;
                        rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                        rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                        rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                    }
                }
                // Bold synthesis from cache.
                if (is_bold) {
                    for (unsigned int by = 0; by < cached->bmp_rows; by++) {
                        for (unsigned int bx = 0; bx < cached->bmp_width; bx++) {
                            int dx = x0 + (int)bx + 1;
                            int dy = y0 + (int)by;
                            if (dx < 0 || dx >= img_w || dy < 0 || dy >= img_h) continue;
                            uint8_t alpha = cached->alpha_buf[by * cached->bmp_pitch + bx];
                            if (alpha == 0) continue;
                            int idx = (dy * img_w + dx) * 4;
                            float a = (float)alpha / 255.0f * 0.5f;
                            rgba[idx + 0] = (uint8_t)(fg_r * a + rgba[idx + 0] * (1.0f - a));
                            rgba[idx + 1] = (uint8_t)(fg_g * a + rgba[idx + 1] * (1.0f - a));
                            rgba[idx + 2] = (uint8_t)(fg_b * a + rgba[idx + 2] * (1.0f - a));
                        }
                    }
                }
            }

            pen_x += cached ? cached->advance_x : 0;
        }

        // Underline.
        if (is_uline) {
            ft_draw_hline(rgba, img_w, img_h, img_h - 2,
                          cell_x, cell_x + cell_pixel_total,
                          fg_r, fg_g, fg_b);
        }
        // Strikethrough.
        if (is_strike) {
            ft_draw_hline(rgba, img_w, img_h, img_h / 2,
                          cell_x, cell_x + cell_pixel_total,
                          fg_r, fg_g, fg_b);
        }
    }

    // Blit the whole row as one image onto the cached row plane.
    struct ncvisual *visual =
        ncvisual_from_rgba(rgba, img_h, img_w * 4, img_w);

    if (!visual) {
        if (ctx->debug_log) {
            fprintf(ctx->debug_log,
                    "  pixel_row: ncvisual_from_rgba FAILED row=%u img=%dx%d\n",
                    row, img_w, img_h);
        }
        return;
    }

    struct ncplane *target = nullptr;
    if ((unsigned)row < ctx->row_planes_count) {
        target = ctx->row_planes[row];
    }

    if (!target) {
        ncvisual_destroy(visual);
        return;
    }

    struct ncvisual_options vopts = {
        .n       = target,
        .scaling = NCSCALE_NONE,
        .y       = 0,
        .x       = 0,
        .blitter = NCBLIT_PIXEL,
        .flags   = 0,
    };

    struct ncplane *blit_plane = ncvisual_blit(ctx->nc, visual, &vopts);

    if (ctx->debug_log) {
        fprintf(ctx->debug_log,
                "  pixel_row: blit %s row=%u img=%dx%d\n",
                blit_plane ? "OK" : "FAILED", row,
                img_w, img_h);
        fflush(ctx->debug_log);
    }

    ncvisual_destroy(visual);
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Cell-based rendering (fallback)
// ====================================================================

static void
render_cell_based(nc_ctx_t           *ctx,
                  const n00b_rcell_t *cells,
                  n00b_isize_t        rows,
                  n00b_isize_t        cols,
                  const n00b_rcell_t *prev_cells)
{
    uint64_t cur_channels  = 0;
    uint16_t cur_stylemask = 0;

    for (n00b_isize_t r = 0; r < rows; r++) {
        for (n00b_isize_t c = 0; c < cols; c++) {
            const n00b_rcell_t *cell = &cells[r * cols + c];

            // Skip continuation cells.
            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }

            // Diff rendering: skip unchanged cells.
            if (prev_cells) {
                const n00b_rcell_t *prev = &prev_cells[r * cols + c];
                if (n00b_rcell_equal(cell, prev)) {
                    continue;
                }
            }

            // Resolve style.
            uint64_t channels  = 0;
            uint16_t stylemask = 0;
            style_to_nc(cell->style, &channels, &stylemask);

            if (channels != cur_channels) {
                ncplane_set_channels(ctx->stdplane, channels);
                cur_channels = channels;
            }
            if (stylemask != cur_stylemask) {
                ncplane_set_styles(ctx->stdplane, stylemask);
                cur_stylemask = stylemask;
            }

            if (cell->flags & N00B_CELL_OCCUPIED) {
                ncplane_putstr_yx(ctx->stdplane, (int)r, (int)c,
                                  cell->grapheme);
            }
            else {
                ncplane_putchar_yx(ctx->stdplane, (int)r, (int)c, ' ');
            }
        }
    }
}

// ====================================================================
// Pixel rendering (full frame)
// ====================================================================

#if N00B_HAVE_FREETYPE

static void
render_pixel_frame(nc_ctx_t           *ctx,
                   const n00b_rcell_t *cells,
                   n00b_isize_t        rows,
                   n00b_isize_t        cols)
{
    int pixel_h = (int)ctx->cell_pixel_h;
    int pixel_w = (int)ctx->cell_pixel_w;

    if (pixel_h <= 0 || pixel_w <= 0 || !ctx->ft_face) {
        return;
    }

    // Compute font metrics once per frame.
    FT_UInt req_size = (FT_UInt)pixel_h;
    FT_Set_Pixel_Sizes(ctx->ft_face, 0, req_size);

    int ascent  = (int)(ctx->ft_face->size->metrics.ascender >> 6);
    int descent = (int)(-ctx->ft_face->size->metrics.descender >> 6);
    int line_h  = ascent + descent;

    if (line_h > pixel_h) {
        req_size = (FT_UInt)((long)req_size * pixel_h / line_h);
        if (req_size < 1) {
            req_size = 1;
        }
        FT_Set_Pixel_Sizes(ctx->ft_face, 0, req_size);
        ascent  = (int)(ctx->ft_face->size->metrics.ascender >> 6);
        descent = (int)(-ctx->ft_face->size->metrics.descender >> 6);
        line_h  = ascent + descent;
    }

    int img_w = pixel_w * (int)cols;
    int img_h = pixel_h;

    int baseline_val = ascent;
    if (line_h < img_h) {
        baseline_val += (img_h - line_h) / 2;
    }

    pixel_metrics_t pm = {
        .pixel_h  = pixel_h,
        .pixel_w  = pixel_w,
        .img_w    = img_w,
        .img_h    = img_h,
        .baseline = baseline_val,
    };

    // Ensure we have one child plane per row for pixel blitting.
    ensure_row_planes(ctx, (unsigned)rows, (unsigned)cols);

    // Allocate a single reusable RGBA row buffer.
    uint8_t *rgba = malloc((size_t)(img_w * img_h * 4));
    if (!rgba) {
        return;
    }

    for (n00b_isize_t r = 0; r < rows; r++) {
        render_pixel_row(ctx, cells, cols, r, &pm, rgba);
    }

    free(rgba);
}

#endif /* N00B_HAVE_FREETYPE */

// ====================================================================
// Resize detection
// ====================================================================

static void
check_resize(nc_ctx_t *ctx)
{
    if (!ctx->resize_cb) {
        return;
    }

    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(ctx->stdplane, &rows, &cols);

    if (rows != ctx->last_rows || cols != ctx->last_cols) {
        ctx->last_rows = rows;
        ctx->last_cols = cols;
        ctx->resize_cb((n00b_isize_t)rows, (n00b_isize_t)cols,
                        ctx->resize_user);
    }
}

// ====================================================================
// Vtable implementation
// ====================================================================

static void *
nc_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output; // notcurses writes directly to the terminal.

    nc_ctx_t *ctx = calloc(1, sizeof(nc_ctx_t));
    if (!ctx) {
        return nullptr;
    }

    ctx->debug_log = fopen("/tmp/nc_backend.log", "w");

    // The n00b runtime owns fatal-signal handlers and their alternate
    // signal stack.  If notcurses also installs fatal handlers it
    // allocates its own sigaltstack; on notcurses_stop() it tries to
    // remove it, but the stack may still be active on the calling
    // thread, causing "couldn't remove alternate signal stack
    // (invalid argument)".
    //
    // NCOPTION_NO_QUIT_SIGHANDLERS prevents this — notcurses skips
    // SIGINT/SIGTERM/etc. and never touches sigaltstack.  We leave
    // SIGWINCH to notcurses so it gets proper resize events and can
    // run its terminal probe without interference.
    struct notcurses_options opts = {
        .flags = NCOPTION_SUPPRESS_BANNERS
               | NCOPTION_NO_QUIT_SIGHANDLERS,
    };

    ctx->nc = notcurses_init(&opts, nullptr);
    if (!ctx->nc) {
        if (ctx->debug_log) {
            fprintf(ctx->debug_log, "notcurses_init FAILED\n");
            fclose(ctx->debug_log);
        }
        free(ctx);
        return nullptr;
    }

    ctx->stdplane = notcurses_stdplane(ctx->nc);

    // Disable line signals so Ctrl+C/Ctrl+Z come through as key
    // events instead of generating SIGINT/SIGTSTP.
    notcurses_linesigs_disable(ctx->nc);

    // Cache dimensions.
    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(ctx->stdplane, &rows, &cols);
    ctx->last_rows = rows;
    ctx->last_cols = cols;

    // Query pixel geometry.
    unsigned int pix_y = 0, pix_x = 0;
    ncplane_pixel_geom(ctx->stdplane, nullptr, nullptr,
                       &pix_y, &pix_x, nullptr, nullptr);
    ctx->cell_pixel_h = pix_y;
    ctx->cell_pixel_w = pix_x;

    // Detect pixel support.
    ctx->has_pixel = (notcurses_check_pixel_support(ctx->nc) > 0);

#if N00B_HAVE_FREETYPE
    ft_init(ctx);
    if (ctx->has_freetype) {
        ctx->glyph_cache = calloc(1, sizeof(glyph_cache_t));
    }
#endif

    if (ctx->debug_log) {
        fprintf(ctx->debug_log,
                "nc_init: %ux%u cells, cell_pixel=%ux%u, "
                "has_pixel=%d, has_freetype=%d\n",
                cols, rows, pix_x, pix_y,
                ctx->has_pixel,
#if N00B_HAVE_FREETYPE
                ctx->has_freetype
#else
                0
#endif
                );
        fflush(ctx->debug_log);
    }

    return ctx;
}

static void
nc_destroy(void *vctx)
{
    nc_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

#if N00B_HAVE_FREETYPE
    glyph_cache_destroy(ctx->glyph_cache);
    ctx->glyph_cache = NULL;
    ft_destroy(ctx);
#endif

    destroy_row_planes(ctx);

    if (ctx->debug_log) {
        fprintf(ctx->debug_log, "nc_destroy\n");
        fclose(ctx->debug_log);
        ctx->debug_log = nullptr;
    }

    if (ctx->nc) {
        notcurses_stop(ctx->nc);
    }
    free(ctx);
}

static n00b_render_cap_t
nc_capabilities(void *vctx)
{
    nc_ctx_t *ctx = vctx;

    n00b_render_cap_t caps =
        N00B_RCAP_COLOR_24BIT
      | N00B_RCAP_COLOR_256
      | N00B_RCAP_COLOR_BASIC
      | N00B_RCAP_BOLD
      | N00B_RCAP_ITALIC
      | N00B_RCAP_UNDERLINE
      | N00B_RCAP_STRIKETHROUGH
      | N00B_RCAP_DIM
      | N00B_RCAP_CURSOR_MOVE
      | N00B_RCAP_ALT_SCREEN
      | N00B_RCAP_MANAGES_TTY
      | N00B_RCAP_UNICODE
      | N00B_RCAP_WIDE_CHARS
      | N00B_RCAP_DIFF_RENDER;

    if (ctx->has_pixel) {
        caps |= N00B_RCAP_PIXEL_COORDS | N00B_RCAP_FONT_METRICS;
    }

    return caps;
}

static n00b_render_size_t
nc_get_size(void *vctx)
{
    nc_ctx_t *ctx = vctx;

    unsigned int rows = 0, cols = 0;
    ncplane_dim_yx(ctx->stdplane, &rows, &cols);

    // Refresh pixel geometry.
    unsigned int pix_y = 0, pix_x = 0;
    unsigned int total_py = 0, total_px = 0;
    ncplane_pixel_geom(ctx->stdplane, &total_py, &total_px,
                       &pix_y, &pix_x, nullptr, nullptr);
    ctx->cell_pixel_h = pix_y;
    ctx->cell_pixel_w = pix_x;

    return (n00b_render_size_t){
        .cols         = (n00b_isize_t)cols,
        .rows         = (n00b_isize_t)rows,
        .pixel_w      = (n00b_isize_t)total_px,
        .pixel_h      = (n00b_isize_t)total_py,
        .cell_pixel_w = (n00b_isize_t)pix_x,
        .cell_pixel_h = (n00b_isize_t)pix_y,
    };
}

static void
nc_render_frame(void         *vctx,
                n00b_rcell_t *cells,
                n00b_isize_t  rows,
                n00b_isize_t  cols,
                n00b_rcell_t *prev_cells)
{
    nc_ctx_t *ctx = vctx;

    if (ctx->debug_log) {
        fprintf(ctx->debug_log,
                "nc_render_frame: %ux%u cells, prev=%p\n",
                rows, cols, (void *)prev_cells);
    }

    // Do NOT erase stdplane here — the cell-based renderer writes
    // every cell (no diff skip when stdplane is cleared).  Notcurses
    // handles dirty-cell tracking internally via notcurses_render().

    // Always use the cell-based path for widget/text rendering.
    // The terminal natively renders Unicode box-drawing characters,
    // styled text, and symbols — no FreeType needed.  The pixel path
    // (FreeType → RGBA → ncvisual_blit) is reserved for actual
    // graphical content (images, plots) and must be explicitly
    // requested per-plane in the future.
    if (ctx->debug_log) {
        fprintf(ctx->debug_log, "  using CELL path\n");
        fflush(ctx->debug_log);
    }
    render_cell_based(ctx, cells, rows, cols, prev_cells);
}

static void
nc_flush(void *vctx)
{
    nc_ctx_t *ctx = vctx;
    int rc = notcurses_render(ctx->nc);
    if (ctx->debug_log) {
        fprintf(ctx->debug_log, "nc_flush: notcurses_render=%d\n", rc);
        fflush(ctx->debug_log);
    }
    check_resize(ctx);
}

static void
nc_cursor_set_visible(void *vctx, bool visible)
{
    nc_ctx_t *ctx = vctx;
    if (visible) {
        notcurses_cursor_enable(ctx->nc, 0, 0);
    }
    else {
        notcurses_cursor_disable(ctx->nc);
    }
}

static void
nc_cursor_move(void *vctx, n00b_isize_t row, n00b_isize_t col)
{
    nc_ctx_t *ctx = vctx;
    ncplane_cursor_move_yx(ctx->stdplane, (int)row, (int)col);
}

static void
nc_alt_screen_enter(void *vctx)
{
    (void)vctx;
    // notcurses manages the alternate screen internally.
    // If we initialized with NCOPTION_NO_ALTERNATE_SCREEN,
    // re-enter alt screen here.  notcurses 3.x doesn't expose
    // a toggle API, so this is a no-op — the caller should
    // re-init if alt screen behavior is needed.
}

static void
nc_alt_screen_leave(void *vctx)
{
    (void)vctx;
    // See nc_alt_screen_enter.
}

static void
nc_on_resize(void  *vctx,
             void (*cb)(n00b_isize_t, n00b_isize_t, void *),
             void  *user_ctx)
{
    nc_ctx_t *ctx = vctx;
    ctx->resize_cb   = cb;
    ctx->resize_user = user_ctx;
}

static void
nc_prepare_gui(void *vctx, n00b_plane_t **planes, n00b_isize_t n)
{
    // No GUI extensions for terminal backends.
    (void)vctx;
    (void)planes;
    (void)n;
}

// ====================================================================
// Input polling
// ====================================================================

static uint32_t
nc_map_key(uint32_t nc_id)
{
    switch (nc_id) {
    case NCKEY_UP:        return N00B_KEY_UP;
    case NCKEY_DOWN:      return N00B_KEY_DOWN;
    case NCKEY_LEFT:      return N00B_KEY_LEFT;
    case NCKEY_RIGHT:     return N00B_KEY_RIGHT;
    case NCKEY_HOME:      return N00B_KEY_HOME;
    case NCKEY_END:       return N00B_KEY_END;
    case NCKEY_PGUP:      return N00B_KEY_PAGE_UP;
    case NCKEY_PGDOWN:    return N00B_KEY_PAGE_DOWN;
    case NCKEY_INS:       return N00B_KEY_INSERT;
    case NCKEY_DEL:       return N00B_KEY_DELETE;
    case NCKEY_BACKSPACE: return N00B_KEY_BACKSPACE;
    case 0x7F:            return N00B_KEY_BACKSPACE; // Raw DEL.
    case 0x08:            return N00B_KEY_BACKSPACE; // Raw BS.
    case NCKEY_TAB:       return N00B_KEY_TAB;
    case NCKEY_ENTER:     return N00B_KEY_ENTER;
    case '\r':            return N00B_KEY_ENTER;  // Raw CR.
    case '\n':            return N00B_KEY_ENTER;  // Raw LF.
    case NCKEY_ESC:       return N00B_KEY_ESCAPE;
    case NCKEY_F01:       return N00B_KEY_F1;
    case NCKEY_F02:       return N00B_KEY_F2;
    case NCKEY_F03:       return N00B_KEY_F3;
    case NCKEY_F04:       return N00B_KEY_F4;
    case NCKEY_F05:       return N00B_KEY_F5;
    case NCKEY_F06:       return N00B_KEY_F6;
    case NCKEY_F07:       return N00B_KEY_F7;
    case NCKEY_F08:       return N00B_KEY_F8;
    case NCKEY_F09:       return N00B_KEY_F9;
    case NCKEY_F10:       return N00B_KEY_F10;
    case NCKEY_F11:       return N00B_KEY_F11;
    case NCKEY_F12:       return N00B_KEY_F12;
    default:              return nc_id; // Unicode codepoint.
    }
}

static bool
nc_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    nc_ctx_t *ctx = vctx;
    out->type = N00B_EVENT_NONE;

    // Use poll() + non-blocking get, matching slop's ctui pattern.
    int nc_fd = notcurses_inputready_fd(ctx->nc);
    struct pollfd pfd = { .fd = nc_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return false;
    }

    ncinput ni;
    memset(&ni, 0, sizeof(ni));

    uint32_t r = notcurses_get_nblock(ctx->nc, &ni);
    if (r == (uint32_t)-1 || r == 0) {
        return false;
    }

    if (ctx->debug_log) {
        fprintf(ctx->debug_log,
                "nc_poll: id=0x%x evtype=%d ctrl=%d shift=%d alt=%d eff_text[0]=0x%x\n",
                ni.id, ni.evtype, ni.ctrl, ni.shift, ni.alt,
                (unsigned)ni.eff_text[0]);
        fflush(ctx->debug_log);
    }

    // Resize event.
    if (ni.id == NCKEY_RESIZE) {
        out->type = N00B_EVENT_RESIZE;
        unsigned int rows = 0, cols = 0;
        ncplane_dim_yx(ctx->stdplane, &rows, &cols);
        out->resize.rows = (n00b_isize_t)rows;
        out->resize.cols = (n00b_isize_t)cols;
        ctx->last_rows = rows;
        ctx->last_cols = cols;
        return true;
    }

    // Filter key-release events (can arrive at startup from the
    // Enter that launched the program).  Accept PRESS, REPEAT, and
    // UNKNOWN.
    if (ni.evtype == NCTYPE_RELEASE) {
        return false;
    }

    // Filter synthesized keys above the Unicode range that we
    // don't handle (invalid / unrecognized notcurses keys).
    if (ni.id >= NCKEY_INVALID) {
        uint32_t mapped = nc_map_key(ni.id);
        if (mapped == ni.id) {
            // nc_map_key didn't recognize it — drop.
            return false;
        }
    }

    // Build modifier flags.
    n00b_key_mod_t mods = N00B_MOD_NONE;
    if (ni.shift) {
        mods |= N00B_MOD_SHIFT;
    }
    if (ni.ctrl) {
        mods |= N00B_MOD_CTRL;
    }
    if (ni.alt) {
        mods |= N00B_MOD_ALT;
    }

    out->type     = N00B_EVENT_KEY;
    out->key.mods = mods;

    // Try recognized special keys first (Enter, Tab, arrows, etc.).
    uint32_t mapped = nc_map_key(ni.id);
    if (mapped != ni.id) {
        out->key.key = mapped;
        return true;
    }

    // Raw control characters (bytes 1–26): map to Ctrl+letter,
    // matching the ANSI backend's convention.  This ensures e.g.
    // byte 3 → key='c' + CTRL regardless of what notcurses reports
    // in ni.ctrl / ni.eff_text.
    if (ni.id >= 1 && ni.id <= 26) {
        out->key.key  = ni.id + 'a' - 1;
        out->key.mods |= N00B_MOD_CTRL;
        return true;
    }

    // Printable character — prefer eff_text (has modifiers applied,
    // e.g. Shift+' → ").  Fall back to ni.id.
    if (ni.eff_text[0] != 0) {
        out->key.key = ni.eff_text[0];
    }
    else {
        out->key.key = ni.id;
    }

    // Reject null or bare escape fragments from broken SGR mouse
    // sequences (notcurses issue #2904).
    if (out->key.key == 0) {
        out->type = N00B_EVENT_NONE;
        return false;
    }

    return true;
}

// ====================================================================
// Vtable
// ====================================================================

const n00b_renderer_vtable_t n00b_renderer_notcurses = {
    .name               = "notcurses",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = nc_init,
    .destroy            = nc_destroy,
    .capabilities       = nc_capabilities,
    .get_size           = nc_get_size,
    .render_frame       = nc_render_frame,
    .flush              = nc_flush,
    .cursor_set_visible = nc_cursor_set_visible,
    .cursor_move        = nc_cursor_move,
    .alt_screen_enter   = nc_alt_screen_enter,
    .alt_screen_leave   = nc_alt_screen_leave,
    .on_resize          = nc_on_resize,
    .prepare_gui        = nc_prepare_gui,
    .poll_event         = nc_poll_event,
};

// ====================================================================
// Public query functions
// ====================================================================

bool
n00b_notcurses_has_pixel_support(void *vctx)
{
    nc_ctx_t *ctx = vctx;
    return ctx ? ctx->has_pixel : false;
}

bool
n00b_notcurses_has_freetype(void *vctx)
{
#if N00B_HAVE_FREETYPE
    nc_ctx_t *ctx = vctx;
    return ctx ? ctx->has_freetype : false;
#else
    (void)vctx;
    return false;
#endif
}
