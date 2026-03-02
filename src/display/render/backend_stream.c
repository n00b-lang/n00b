/*
 * Stream renderer backend: writes plain text to a buffer or FILE *.
 *
 * This is the simplest backend — no terminal capabilities, no cursor
 * movement, no colors.  It linearizes the frame top-to-bottom and
 * writes the grapheme content of each cell, with newlines between rows.
 *
 * Primarily useful for testing: capture output to a buffer and assert
 * against expected content.
 */

#include <stdio.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "core/buffer.h"
#include "display/render/backend.h"
#include "display/render/composite.h"

// -------------------------------------------------------------------
// Stream backend context
// -------------------------------------------------------------------

typedef struct {
    char        *buffer;
    size_t       buf_size;
    size_t       buf_used;
    n00b_isize_t rows;
    n00b_isize_t cols;

    // Persistent compositing grid.
    n00b_rcell_t *comp_grid;
    n00b_isize_t  comp_grid_rows;
    n00b_isize_t  comp_grid_cols;
} stream_ctx_t;

#define STREAM_DEFAULT_ROWS 25
#define STREAM_DEFAULT_COLS 80
#define STREAM_INITIAL_BUF  4096

// -------------------------------------------------------------------
// Vtable implementation
// -------------------------------------------------------------------

static void *
stream_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    stream_ctx_t *ctx = n00b_alloc_with_opts(stream_ctx_t, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->rows     = STREAM_DEFAULT_ROWS;
    ctx->cols     = STREAM_DEFAULT_COLS;
    ctx->buf_size = STREAM_INITIAL_BUF;
    ctx->buffer   = n00b_alloc_array_with_opts(char, STREAM_INITIAL_BUF, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->buf_used = 0;
    return ctx;
}

static void
stream_destroy(void *vctx)
{
    stream_ctx_t *ctx = vctx;
    if (ctx) {
        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        n00b_free(ctx->buffer);
        n00b_free(ctx);
    }
}

static n00b_render_cap_t
stream_capabilities(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_NONE;
}

static n00b_render_size_t
stream_get_size(void *vctx)
{
    stream_ctx_t *ctx = vctx;
    return (n00b_render_size_t){
        .rows = ctx->rows,
        .cols = ctx->cols,
    };
}

static void
stream_ensure_space(stream_ctx_t *ctx, size_t needed)
{
    if (ctx->buf_used + needed < ctx->buf_size) {
        return;
    }

    while (ctx->buf_used + needed >= ctx->buf_size) {
        ctx->buf_size *= 2;
    }

    char *new_buf = n00b_alloc_array_with_opts(char, ctx->buf_size, &(n00b_alloc_opts_t){.no_scan = true});
    memcpy(new_buf, ctx->buffer, ctx->buf_used);
    n00b_free(ctx->buffer);
    ctx->buffer = new_buf;
}

static void
stream_render_frame(void         *vctx,
                    n00b_rcell_t *cells,
                    n00b_isize_t  rows,
                    n00b_isize_t  cols,
                    n00b_rcell_t *prev_cells)
{
    (void)prev_cells;
    stream_ctx_t *ctx = vctx;

    // Reset buffer.
    ctx->buf_used = 0;

    for (n00b_isize_t r = 0; r < rows; r++) {
        // Find last non-space column to trim trailing spaces.
        n00b_isize_t last_content = 0;
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];
            if ((cell->flags & N00B_CELL_OCCUPIED)
                && !(cell->flags & N00B_CELL_WIDE_CONT)
                && !(cell->grapheme_len == 1 && cell->grapheme[0] == ' ')) {
                last_content = c + 1;
            }
        }

        for (n00b_isize_t c = 0; c < last_content; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }

            if (cell->flags & N00B_CELL_OCCUPIED) {
                stream_ensure_space(ctx, cell->grapheme_len);
                memcpy(ctx->buffer + ctx->buf_used,
                       cell->grapheme, cell->grapheme_len);
                ctx->buf_used += cell->grapheme_len;
            }
            else {
                stream_ensure_space(ctx, 1);
                ctx->buffer[ctx->buf_used++] = ' ';
            }
        }

        // Newline between rows (but not after last row).
        if (r + 1 < rows) {
            stream_ensure_space(ctx, 1);
            ctx->buffer[ctx->buf_used++] = '\n';
        }
    }

    // NUL terminate.
    stream_ensure_space(ctx, 1);
    ctx->buffer[ctx->buf_used] = '\0';
}

static void
stream_flush(void *vctx)
{
    (void)vctx;
    // Nothing to flush for buffer mode.
}

// -------------------------------------------------------------------
// Plane-based rendering
// -------------------------------------------------------------------

static void
stream_render_planes(void                         *vctx,
                     const n00b_composite_entry_t *entries,
                     n00b_isize_t                  count,
                     n00b_isize_t                  total_rows,
                     n00b_isize_t                  total_cols,
                     n00b_text_style_t            *default_style,
                     n00b_render_cap_t             caps)
{
    stream_ctx_t *ctx = vctx;

    if (total_rows != ctx->comp_grid_rows
        || total_cols != ctx->comp_grid_cols) {
        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        size_t total = (size_t)total_rows * total_cols;
        ctx->comp_grid = n00b_alloc_array_with_opts(
            n00b_rcell_t, total,
            &(n00b_alloc_opts_t){.no_scan = true});
        ctx->comp_grid_rows = total_rows;
        ctx->comp_grid_cols = total_cols;
    }

    n00b_composite_commands_to_grid(entries, count, ctx->comp_grid,
                                     total_rows, total_cols,
                                     1, 1,
                                     default_style, caps);

    stream_render_frame(vctx, ctx->comp_grid, total_rows, total_cols,
                         nullptr);
}

// -------------------------------------------------------------------
// Public vtable
// -------------------------------------------------------------------

const n00b_renderer_vtable_t n00b_renderer_stream = {
    .name          = "stream",
    .version       = N00B_RENDERER_ABI_VERSION,
    .init          = stream_init,
    .destroy       = stream_destroy,
    .capabilities  = stream_capabilities,
    .get_size      = stream_get_size,
    .render_frame  = stream_render_frame,
    .flush         = stream_flush,
    .render_planes = stream_render_planes,
};

// -------------------------------------------------------------------
// Stream-specific helpers (for testing)
// -------------------------------------------------------------------

/*
 * Get the stream backend's internal buffer as a string.
 * Only valid when the backend is the stream backend.
 */
n00b_string_t *
n00b_stream_backend_get_buffer(void *ctx)
{
    stream_ctx_t *sctx = ctx;
    return n00b_string_from_raw(sctx->buffer, (int64_t)sctx->buf_used);
}

/*
 * Get the byte count of the stream backend's buffer.
 */
size_t
n00b_stream_backend_get_length(void *ctx)
{
    stream_ctx_t *sctx = ctx;
    return sctx->buf_used;
}

/*
 * Set the virtual terminal size for the stream backend.
 */
void
n00b_stream_backend_set_size(void *ctx, n00b_isize_t rows, n00b_isize_t cols)
{
    stream_ctx_t *sctx = ctx;
    sctx->rows = rows;
    sctx->cols = cols;
}
