/*
 * Tests for the native Cocoa/macOS renderer backend.
 *
 * Tests that require a display (window creation) are skipped on
 * headless CI environments.
 */

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

// The canonical n00b headers use ncc extensions that Apple's ObjC
// compiler can't parse.  cocoa_bridge.h provides standalone
// redeclarations of the types we need.
#include "display/render/cocoa_bridge.h"

// ====================================================================
// Headless / non-GUI detection
// ====================================================================

static bool _gui_available = false;

/**
 * Check whether we can safely create AppKit windows.
 * Must be called BEFORE n00b_init() (which starts threads
 * that make fork() unsafe).  Result is cached in _gui_available.
 */
static void
probe_display(void)
{
    // No physical display.
    if (CGMainDisplayID() == 0) {
        return;
    }

    // Fork a probe child that tries to init AppKit.
    // Suppress ObjC fork-safety warnings in the child.
    setenv("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES", 0);

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        // Child: silence stderr (ObjC fork warnings) and try AppKit.
        close(STDERR_FILENO);
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }
        _exit(0);
    }

    // Parent: wait for child.
    int status = 0;
    waitpid(pid, &status, 0);
    _gui_available = WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool
has_display(void)
{
    return _gui_available;
}

// ====================================================================
// Tests
// ====================================================================

static void
test_registration(void)
{
    // We test the extern vtable directly since n00b_renderer_find()
    // returns n00b_option_t which uses ncc extensions.
    const n00b_renderer_vtable_t *vt = &n00b_renderer_cocoa;
    assert(vt != NULL);
    assert(strcmp(vt->name, "cocoa") == 0);
    assert(vt->version == N00B_RENDERER_ABI_VERSION);
    assert(vt->init != NULL);
    assert(vt->destroy != NULL);
    assert(vt->capabilities != NULL);
    assert(vt->get_size != NULL);
    assert(vt->render_frame != NULL);
    assert(vt->flush != NULL);
    assert(vt->cursor_set_visible != NULL);
    assert(vt->cursor_move != NULL);
    assert(vt->on_resize != NULL);
    assert(vt->prepare_gui != NULL);
    printf("  [PASS] registration\n");
}

static void
test_init_destroy(void)
{
    if (!has_display()) {
        printf("  [SKIP] init/destroy (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] init/destroy\n");
}

static void
test_capabilities(void)
{
    if (!has_display()) {
        printf("  [SKIP] capabilities (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_render_cap_t caps = n00b_renderer_cocoa.capabilities(ctx);

    assert(caps & N00B_RCAP_COLOR_24BIT);
    assert(caps & N00B_RCAP_BOLD);
    assert(caps & N00B_RCAP_ITALIC);
    assert(caps & N00B_RCAP_UNDERLINE);
    assert(caps & N00B_RCAP_STRIKETHROUGH);
    assert(caps & N00B_RCAP_DIM);
    assert(caps & N00B_RCAP_UNICODE);
    assert(caps & N00B_RCAP_WIDE_CHARS);
    assert(caps & N00B_RCAP_CURSOR_MOVE);
    assert(caps & N00B_RCAP_PIXEL_COORDS);
    assert(caps & N00B_RCAP_FONT_METRICS);
    assert(caps & N00B_RCAP_DIFF_RENDER);
    assert(caps & N00B_RCAP_GUI_EXT);
    assert(!(caps & N00B_RCAP_ALT_SCREEN));

    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] capabilities\n");
}

static void
test_get_size(void)
{
    if (!has_display()) {
        printf("  [SKIP] get_size (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_render_size_t sz = n00b_renderer_cocoa.get_size(ctx);
    assert(sz.rows > 0);
    assert(sz.cols > 0);
    assert(sz.pixel_w > 0);
    assert(sz.pixel_h > 0);

    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] get_size\n");
}

static void
test_cell_metrics(void)
{
    if (!has_display()) {
        printf("  [SKIP] cell_metrics (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_render_size_t sz = n00b_renderer_cocoa.get_size(ctx);
    assert(sz.cell_pixel_w > 0);
    assert(sz.cell_pixel_h > 0);
    assert(sz.cell_pixel_h >= sz.cell_pixel_w);

    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] cell_metrics\n");
}

static void
test_render_empty(void)
{
    if (!has_display()) {
        printf("  [SKIP] render_empty (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_isize_t rows = 10, cols = 20;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] render_empty\n");
}

static void
test_render_ascii(void)
{
    if (!has_display()) {
        printf("  [SKIP] render_ascii (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_isize_t rows = 5, cols = 10;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    const char *msg = "Hello!";
    for (int i = 0; msg[i]; i++) {
        n00b_rcell_set_ascii(&cells[i], msg[i], NULL);
    }

    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] render_ascii\n");
}

static void
test_render_unicode(void)
{
    if (!has_display()) {
        printf("  [SKIP] render_unicode (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_isize_t rows = 5, cols = 20;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    // CJK character U+4E16 (世) — width 2.
    n00b_rcell_set_codepoint(&cells[0], 0x4E16, 2, NULL);
    cells[1].flags = N00B_CELL_WIDE_CONT;

    // Emoji U+1F600 (😀) — width 2.
    n00b_rcell_set_codepoint(&cells[2], 0x1F600, 2, NULL);
    cells[3].flags = N00B_CELL_WIDE_CONT;

    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] render_unicode\n");
}

static void
test_render_styled(void)
{
    if (!has_display()) {
        printf("  [SKIP] render_styled (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_isize_t rows = 5, cols = 20;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    n00b_text_style_t bold_style = {
        .bold   = N00B_TRI_YES,
        .fg_rgb = n00b_color_make(0xFF0000),
    };
    n00b_text_style_t italic_style = {
        .italic = N00B_TRI_YES,
        .bg_rgb = n00b_color_make(0x0000FF),
    };

    n00b_rcell_set_ascii(&cells[0], 'B', &bold_style);
    n00b_rcell_set_ascii(&cells[1], 'I', &italic_style);

    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] render_styled\n");
}

static void
test_diff_render(void)
{
    if (!has_display()) {
        printf("  [SKIP] diff_render (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_isize_t rows = 5, cols = 10;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *prev = calloc(total, sizeof(n00b_rcell_t));
    n00b_rcell_t *curr = calloc(total, sizeof(n00b_rcell_t));

    n00b_rcell_set_ascii(&prev[0], 'A', NULL);
    n00b_rcell_set_ascii(&curr[0], 'B', NULL);

    // First frame (no prev).
    n00b_renderer_cocoa.render_frame(ctx, prev, rows, cols, NULL);
    n00b_renderer_cocoa.flush(ctx);

    // Second frame with diff.
    n00b_renderer_cocoa.render_frame(ctx, curr, rows, cols, prev);
    n00b_renderer_cocoa.flush(ctx);

    free(prev);
    free(curr);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] diff_render\n");
}

static void
test_cursor_toggle(void)
{
    if (!has_display()) {
        printf("  [SKIP] cursor_toggle (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    // Render a frame so staging exists.
    n00b_isize_t rows = 5, cols = 10;
    n00b_rcell_t *cells = calloc((size_t)rows * cols, sizeof(n00b_rcell_t));
    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);

    n00b_renderer_cocoa.cursor_set_visible(ctx, true);
    n00b_renderer_cocoa.flush(ctx);

    n00b_renderer_cocoa.cursor_set_visible(ctx, false);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] cursor_toggle\n");
}

static void
test_cursor_move(void)
{
    if (!has_display()) {
        printf("  [SKIP] cursor_move (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    // Render a frame so staging exists.
    n00b_isize_t rows = 10, cols = 20;
    n00b_rcell_t *cells = calloc((size_t)rows * cols, sizeof(n00b_rcell_t));
    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);

    n00b_renderer_cocoa.cursor_set_visible(ctx, true);
    n00b_renderer_cocoa.cursor_move(ctx, 0, 0);
    n00b_renderer_cocoa.cursor_move(ctx, 5, 10);
    n00b_renderer_cocoa.cursor_move(ctx, 9, 19);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] cursor_move\n");
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
    if (!has_display()) {
        printf("  [SKIP] resize_callback (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    resize_callback_fired = false;
    resize_got_rows = 0;
    resize_got_cols = 0;

    n00b_renderer_cocoa.on_resize(ctx, resize_handler, NULL);

    // We can't easily programmatically resize here without accessing
    // internal types, so just verify the callback registration doesn't
    // crash and the vtable slot works.

    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] resize_callback\n");
}

static void
test_gui_ext(void)
{
    if (!has_display()) {
        printf("  [SKIP] gui_ext (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    // Create a plane with gui_ext.
    n00b_plane_t plane = {};
    n00b_box_props_t box = {};
    cocoa_gui_ext_t *ext = n00b_cocoa_gui_ext_new();
    assert(ext != NULL);
    assert(ext->opacity == 1.0f);

    ext->shadow_blur     = 5.0;
    ext->shadow_offset_x = 2.0;
    ext->shadow_offset_y = 2.0;
    ext->shadow_color    = n00b_color_make(0x000000);
    ext->corner_radius   = 8.0;
    ext->gradient_dir    = N00B_GRADIENT_TOP_BOTTOM;
    ext->gradient_start  = n00b_color_make(0xFF0000);
    ext->gradient_end    = n00b_color_make(0x0000FF);
    ext->blend_mode      = N00B_BLEND_OVERLAY;

    box.gui_ext = ext;
    plane.box   = &box;

    n00b_plane_t *planes[] = {&plane};
    n00b_renderer_cocoa.prepare_gui(ctx, planes, 1);

    free(ext);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] gui_ext\n");
}

static void
test_gui_ext_null(void)
{
    if (!has_display()) {
        printf("  [SKIP] gui_ext_null (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    // Planes with no gui_ext.
    n00b_plane_t p1 = {};
    n00b_plane_t p2 = {};
    n00b_box_props_t box = {};
    p1.box = &box;

    n00b_plane_t *planes[] = {&p1, &p2};
    n00b_renderer_cocoa.prepare_gui(ctx, planes, 2);

    // Also test with NULL planes array.
    n00b_renderer_cocoa.prepare_gui(ctx, NULL, 0);

    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] gui_ext_null\n");
}

static void
test_font_hints(void)
{
    if (!has_display()) {
        printf("  [SKIP] font_hints (headless)\n");
        return;
    }

    void *ctx = n00b_renderer_cocoa.init(NULL);
    assert(ctx != NULL);

    n00b_isize_t rows = 5, cols = 20;
    size_t total = (size_t)rows * cols;
    n00b_rcell_t *cells = calloc(total, sizeof(n00b_rcell_t));

    n00b_text_style_t mono_style  = {.font_hint = N00B_FONT_MONO};
    n00b_text_style_t serif_style = {.font_hint = N00B_FONT_SERIF};
    n00b_text_style_t sans_style  = {.font_hint = N00B_FONT_SANS};

    n00b_rcell_set_ascii(&cells[0], 'M', &mono_style);
    n00b_rcell_set_ascii(&cells[1], 'S', &serif_style);
    n00b_rcell_set_ascii(&cells[2], 'A', &sans_style);

    n00b_renderer_cocoa.render_frame(ctx, cells, rows, cols, NULL);
    n00b_renderer_cocoa.flush(ctx);

    free(cells);
    n00b_renderer_cocoa.destroy(ctx);
    printf("  [PASS] font_hints\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    // Probe AppKit BEFORE n00b_init (which starts threads that
    // make fork() unsafe).
    probe_display();

    // n00b_runtime_t is opaque from ObjC (~394KB with thread table).
    // Heap-allocate a zeroed buffer to hold it.
    void *runtime_buf = calloc(1, 512 * 1024);
    n00b_cocoa_bridge_init(runtime_buf, argc, argv);

    printf("Running Cocoa backend tests...\n");

    test_registration();
    test_init_destroy();
    test_capabilities();
    test_get_size();
    test_cell_metrics();
    test_render_empty();
    test_render_ascii();
    test_render_unicode();
    test_render_styled();
    test_diff_render();
    test_cursor_toggle();
    test_cursor_move();
    test_resize_callback();
    test_gui_ext();
    test_gui_ext_null();
    test_font_hints();

    printf("All Cocoa backend tests done.\n");
    n00b_cocoa_bridge_shutdown();
    return 0;
}
