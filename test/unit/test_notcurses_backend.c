/*
 * Tests for the notcurses renderer backend.
 *
 * These tests verify the backend vtable, lifecycle, and capabilities.
 * Tests that require a real terminal (init/destroy, rendering) are
 * skipped when stdin is not a tty (CI environments).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "display/render/backend.h"
#include "display/render/backend_notcurses.h"
#include "display/render/cell.h"
#include "text/strings/text_style.h"

// ====================================================================
// Terminal detection
// ====================================================================

static bool _has_terminal = false;

static void
probe_terminal(void)
{
    _has_terminal = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static bool
has_terminal(void)
{
    return _has_terminal;
}

// ====================================================================
// Tests
// ====================================================================

static void
test_registration(void)
{
    const n00b_renderer_vtable_t *vt = &n00b_renderer_notcurses;
    assert(vt != nullptr);
    assert(strcmp(vt->name, "notcurses") == 0);
    assert(vt->version == N00B_RENDERER_ABI_VERSION);
    assert(vt->init != nullptr);
    assert(vt->destroy != nullptr);
    assert(vt->capabilities != nullptr);
    assert(vt->get_size != nullptr);
    assert(vt->render_frame != nullptr);
    assert(vt->flush != nullptr);
    assert(vt->cursor_set_visible != nullptr);
    assert(vt->cursor_move != nullptr);
    assert(vt->on_resize != nullptr);
    assert(vt->prepare_gui != nullptr);
    printf("  [PASS] registration\n");
}

static void
test_init_destroy(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] init/destroy (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] init/destroy\n");
}

static void
test_capabilities(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] capabilities (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_render_cap_t caps = n00b_renderer_notcurses.capabilities(ctx);

    // notcurses always supports these.
    assert(caps & N00B_RCAP_COLOR_24BIT);
    assert(caps & N00B_RCAP_BOLD);
    assert(caps & N00B_RCAP_ITALIC);
    assert(caps & N00B_RCAP_UNDERLINE);
    assert(caps & N00B_RCAP_STRIKETHROUGH);
    assert(caps & N00B_RCAP_UNICODE);
    assert(caps & N00B_RCAP_WIDE_CHARS);
    assert(caps & N00B_RCAP_CURSOR_MOVE);
    assert(caps & N00B_RCAP_ALT_SCREEN);
    assert(caps & N00B_RCAP_DIFF_RENDER);

    // Pixel support is terminal-dependent — just check the flag is
    // consistent with the query function.
    bool pixel = n00b_notcurses_has_pixel_support(ctx);
    if (pixel) {
        assert(caps & N00B_RCAP_PIXEL_COORDS);
        assert(caps & N00B_RCAP_FONT_METRICS);
    }

    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] capabilities\n");
}

static void
test_get_size(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] get_size (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_render_size_t sz = n00b_renderer_notcurses.get_size(ctx);
    assert(sz.rows > 0);
    assert(sz.cols > 0);

    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] get_size\n");
}

static void
test_freetype_query(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] freetype_query (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    // Just verify the query doesn't crash.
    bool has_ft = n00b_notcurses_has_freetype(ctx);
#if N00B_HAVE_FREETYPE
    // If compiled with FreeType, it should be available on most systems.
    (void)has_ft;
    printf("  [INFO] FreeType available: %s\n", has_ft ? "yes" : "no");
#else
    assert(!has_ft);
#endif

    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] freetype_query\n");
}

static void
test_render_empty(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] render_empty (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_isize_t rows = 5, cols = 10;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    n00b_renderer_notcurses.render_frame(ctx, cells, rows, cols, nullptr);
    n00b_renderer_notcurses.flush(ctx);

    free(cells);
    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] render_empty\n");
}

static void
test_render_ascii(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] render_ascii (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_isize_t rows = 5, cols = 20;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    const char *msg = "Hello notcurses!";
    for (int i = 0; msg[i]; i++) {
        n00b_rcell_set_ascii(&cells[i], msg[i], nullptr);
    }

    n00b_renderer_notcurses.render_frame(ctx, cells, rows, cols, nullptr);
    n00b_renderer_notcurses.flush(ctx);

    free(cells);
    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] render_ascii\n");
}

static void
test_render_styled(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] render_styled (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_isize_t rows = 5, cols = 20;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    n00b_text_style_t bold_red = {
        .bold   = N00B_TRI_YES,
        .fg_rgb = n00b_color_make(0xFF0000),
    };
    n00b_text_style_t italic_blue = {
        .italic = N00B_TRI_YES,
        .bg_rgb = n00b_color_make(0x0000FF),
    };

    n00b_rcell_set_ascii(&cells[0], 'B', &bold_red);
    n00b_rcell_set_ascii(&cells[1], 'I', &italic_blue);

    n00b_renderer_notcurses.render_frame(ctx, cells, rows, cols, nullptr);
    n00b_renderer_notcurses.flush(ctx);

    free(cells);
    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] render_styled\n");
}

static void
test_diff_render(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] diff_render (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    n00b_isize_t rows = 5, cols = 10;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *prev = calloc(total, sizeof(n00b_rcell_t));
    n00b_rcell_t *curr = calloc(total, sizeof(n00b_rcell_t));

    n00b_rcell_set_ascii(&prev[0], 'A', nullptr);
    n00b_rcell_set_ascii(&curr[0], 'B', nullptr);

    // First frame.
    n00b_renderer_notcurses.render_frame(ctx, prev, rows, cols, nullptr);
    n00b_renderer_notcurses.flush(ctx);

    // Second frame with diff.
    n00b_renderer_notcurses.render_frame(ctx, curr, rows, cols, prev);
    n00b_renderer_notcurses.flush(ctx);

    free(prev);
    free(curr);
    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] diff_render\n");
}

static bool        resize_callback_fired = false;
static n00b_isize_t resize_got_rows = 0;
static n00b_isize_t resize_got_cols = 0;

static void
resize_handler(n00b_isize_t r, n00b_isize_t c, void *user)
{
    (void)user;
    resize_callback_fired = true;
    resize_got_rows = r;
    resize_got_cols = c;
}

static void
test_resize_callback(void)
{
    if (!has_terminal()) {
        printf("  [SKIP] resize_callback (no terminal)\n");
        return;
    }

    void *ctx = n00b_renderer_notcurses.init(nullptr);
    assert(ctx != nullptr);

    resize_callback_fired = false;
    resize_got_rows = 0;
    resize_got_cols = 0;

    n00b_renderer_notcurses.on_resize(ctx, resize_handler, nullptr);

    // Can't easily trigger a resize, just verify registration doesn't crash.
    n00b_renderer_notcurses.destroy(ctx);
    printf("  [PASS] resize_callback\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    probe_terminal();

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running notcurses backend tests...\n");

    test_registration();
    test_init_destroy();
    test_capabilities();
    test_get_size();
    test_freetype_query();
    test_render_empty();
    test_render_ascii();
    test_render_styled();
    test_diff_render();
    test_resize_callback();

    printf("All notcurses backend tests done.\n");
    n00b_shutdown();
    return 0;
}
