/*
 * Dumb terminal renderer backend.
 *
 * Outputs line-by-line plain text with no cursor movement, no colors,
 * and no escape sequences.  Content is width-wrapped at the output
 * column count.  Suitable for pipe output or terminals with no
 * capabilities.
 */

#include <stdio.h>
#include <unistd.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "render/backend.h"

// -------------------------------------------------------------------
// Dumb context
// -------------------------------------------------------------------

typedef struct {
    int          fd;
    n00b_isize_t rows;
    n00b_isize_t cols;
} dumb_ctx_t;

#define DUMB_DEFAULT_ROWS 25
#define DUMB_DEFAULT_COLS 80

// -------------------------------------------------------------------
// Vtable implementation
// -------------------------------------------------------------------

static void *
dumb_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    dumb_ctx_t *ctx = n00b_alloc_with_opts(dumb_ctx_t,
                                          &(n00b_alloc_opts_t){.no_scan = true});
    ctx->fd   = STDOUT_FILENO;
    ctx->rows = 0;
    ctx->cols = DUMB_DEFAULT_COLS;
    return ctx;
}

static void
dumb_destroy(void *vctx)
{
    dumb_ctx_t *ctx = vctx;
    n00b_free(ctx);
}

static n00b_render_cap_t
dumb_capabilities(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_NONE;
}

static n00b_render_size_t
dumb_get_size(void *vctx)
{
    dumb_ctx_t *ctx = vctx;
    return (n00b_render_size_t){
        .rows = ctx->rows,
        .cols = ctx->cols,
    };
}

static void
dumb_render_frame(void *vctx, n00b_rcell_t *cells,
                  n00b_isize_t rows, n00b_isize_t cols,
                  n00b_rcell_t *prev_cells)
{
    (void)prev_cells;
    dumb_ctx_t *ctx = vctx;

    for (n00b_isize_t r = 0; r < rows; r++) {
        // Find last non-space column.
        n00b_isize_t last = 0;
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];
            if ((cell->flags & N00B_CELL_OCCUPIED)
                && !(cell->flags & N00B_CELL_WIDE_CONT)
                && !(cell->grapheme_len == 1 && cell->grapheme[0] == ' ')) {
                last = c + 1;
            }
        }

        // Emit the line.
        for (n00b_isize_t c = 0; c < last; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];
            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }
            if (cell->flags & N00B_CELL_OCCUPIED) {
                write(ctx->fd, cell->grapheme, cell->grapheme_len);
            }
            else {
                write(ctx->fd, " ", 1);
            }
        }

        write(ctx->fd, "\n", 1);
    }
}

static void
dumb_flush(void *vctx)
{
    (void)vctx;
}

// -------------------------------------------------------------------
// Public vtable
// -------------------------------------------------------------------

const n00b_renderer_vtable_t n00b_renderer_dumb = {
    .name         = "dumb",
    .version      = N00B_RENDERER_ABI_VERSION,
    .init         = dumb_init,
    .destroy      = dumb_destroy,
    .capabilities = dumb_capabilities,
    .get_size     = dumb_get_size,
    .render_frame = dumb_render_frame,
    .flush        = dumb_flush,
};
