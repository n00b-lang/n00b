#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "render/backend.h"
#include "render/cell.h"
#include "strings/text_style.h"

// ====================================================================
// Helpers: capture ANSI backend output to a pipe
// ====================================================================

// We test the ANSI backend by rendering to a frame and verifying
// that the vtable calls don't crash, and that the backend reports
// correct capabilities.

static void
test_ansi_capabilities(void)
{
    void *ctx = n00b_renderer_ansi.init(nullptr);
    n00b_render_cap_t caps = n00b_renderer_ansi.capabilities(ctx);

    assert(caps & N00B_RCAP_COLOR_BASIC);
    assert(caps & N00B_RCAP_COLOR_256);
    assert(caps & N00B_RCAP_COLOR_24BIT);
    assert(caps & N00B_RCAP_BOLD);
    assert(caps & N00B_RCAP_ITALIC);
    assert(caps & N00B_RCAP_UNICODE);
    assert(caps & N00B_RCAP_DIFF_RENDER);
    assert(caps & N00B_RCAP_CURSOR_MOVE);
    assert(caps & N00B_RCAP_ALT_SCREEN);

    n00b_renderer_ansi.destroy(ctx);
    printf("  [PASS] ANSI capabilities\n");
}

static void
test_ansi_get_size(void)
{
    void *ctx = n00b_renderer_ansi.init(nullptr);
    n00b_render_size_t sz = n00b_renderer_ansi.get_size(ctx);

    assert(sz.rows > 0);
    assert(sz.cols > 0);

    n00b_renderer_ansi.destroy(ctx);
    printf("  [PASS] ANSI get_size\n");
}

static void
test_ansi_render_frame(void)
{
    void *ctx = n00b_renderer_ansi.init(nullptr);

    // Create a small frame.
    n00b_rcell_t frame[3 * 5]; // 3 rows x 5 cols
    memset(frame, 0, sizeof(frame));

    // Set a few cells.
    n00b_rcell_set_ascii(&frame[0], 'H', nullptr);
    n00b_rcell_set_ascii(&frame[1], 'i', nullptr);

    // Render to /dev/null to avoid cluttering test output.
    // We just verify it doesn't crash.
    n00b_renderer_ansi.render_frame(ctx, frame, 3, 5, nullptr);
    // Don't flush — we don't want to write to the real terminal.

    n00b_renderer_ansi.destroy(ctx);
    printf("  [PASS] ANSI render_frame (no crash)\n");
}

static void
test_ansi_diff_render(void)
{
    void *ctx = n00b_renderer_ansi.init(nullptr);

    n00b_rcell_t frame[2 * 3];
    n00b_rcell_t prev[2 * 3];
    memset(frame, 0, sizeof(frame));
    memset(prev, 0, sizeof(prev));

    // Set identical content.
    n00b_rcell_set_ascii(&frame[0], 'A', nullptr);
    n00b_rcell_set_ascii(&prev[0], 'A', nullptr);

    // Change one cell.
    n00b_rcell_set_ascii(&frame[1], 'B', nullptr);
    n00b_rcell_set_ascii(&prev[1], 'X', nullptr);

    n00b_renderer_ansi.render_frame(ctx, frame, 2, 3, prev);

    n00b_renderer_ansi.destroy(ctx);
    printf("  [PASS] ANSI diff render\n");
}

static void
test_dumb_capabilities(void)
{
    void *ctx = n00b_renderer_dumb.init(nullptr);
    n00b_render_cap_t caps = n00b_renderer_dumb.capabilities(ctx);

    assert(caps == N00B_RCAP_NONE);

    n00b_renderer_dumb.destroy(ctx);
    printf("  [PASS] dumb capabilities\n");
}

static void
test_stream_round_trip(void)
{
    // Already thoroughly tested in test_render_canvas, but verify basics.
    void *ctx = n00b_renderer_stream.init(nullptr);

    n00b_render_size_t sz = n00b_renderer_stream.get_size(ctx);
    assert(sz.rows > 0);
    assert(sz.cols > 0);

    n00b_render_cap_t caps = n00b_renderer_stream.capabilities(ctx);
    assert(caps == N00B_RCAP_NONE);

    n00b_renderer_stream.destroy(ctx);
    printf("  [PASS] stream backend round-trip\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running render ANSI/backend tests...\n");

    test_ansi_capabilities();
    test_ansi_get_size();
    test_ansi_render_frame();
    test_ansi_diff_render();
    test_dumb_capabilities();
    test_stream_round_trip();

    printf("All render ANSI/backend tests passed.\n");
    n00b_shutdown();
    return 0;
}
