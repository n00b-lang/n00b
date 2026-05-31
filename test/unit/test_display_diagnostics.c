#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event_loop.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "internal/display/diagnostics.h"

typedef struct {
    n00b_event_t       events[2];
    size_t             count;
    size_t             ix;
    n00b_render_size_t size;
} diag_loop_backend_t;

static void *
diag_loop_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    diag_loop_backend_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != nullptr);

    ctx->size.rows         = 6;
    ctx->size.cols         = 20;
    ctx->size.cell_pixel_w = 1;
    ctx->size.cell_pixel_h = 1;
    ctx->events[0]         = (n00b_event_t){.type = N00B_EVENT_NONE};
    ctx->events[1]         = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key  = {.key = 'c', .mods = N00B_MOD_CTRL},
    };
    ctx->count = 2;

    return ctx;
}

static void
diag_loop_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
diag_loop_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_MANAGES_TTY;
}

static n00b_render_size_t
diag_loop_get_size(void *vctx)
{
    diag_loop_backend_t *ctx = vctx;
    return ctx->size;
}

static void
diag_loop_render_frame(void         *vctx,
                       n00b_rcell_t *cells,
                       n00b_isize_t  rows,
                       n00b_isize_t  cols,
                       n00b_rcell_t *prev_cells)
{
    (void)vctx;
    (void)cells;
    (void)rows;
    (void)cols;
    (void)prev_cells;
}

static void
diag_loop_flush(void *vctx)
{
    (void)vctx;
}

static void
diag_loop_render_planes(void                         *vctx,
                        const n00b_composite_entry_t *entries,
                        n00b_isize_t                  count,
                        n00b_isize_t                  total_rows,
                        n00b_isize_t                  total_cols,
                        n00b_text_style_t            *default_style,
                        n00b_render_cap_t             caps)
{
    (void)vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
}

static bool
diag_loop_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    diag_loop_backend_t *ctx = vctx;
    (void)timeout_ms;

    if (ctx->ix < ctx->count) {
        *out = ctx->events[ctx->ix++];
        return out->type != N00B_EVENT_NONE;
    }

    out->type = N00B_EVENT_NONE;
    return false;
}

static const n00b_renderer_vtable_t diag_loop_renderer = {
    .name          = "diag_loop_test",
    .version       = N00B_RENDERER_ABI_VERSION,
    .init          = diag_loop_init,
    .destroy       = diag_loop_destroy,
    .capabilities  = diag_loop_caps,
    .get_size      = diag_loop_get_size,
    .render_frame  = diag_loop_render_frame,
    .flush         = diag_loop_flush,
    .render_planes = diag_loop_render_planes,
    .poll_event    = diag_loop_poll_event,
};

static void
read_file(FILE *fp, char *buf, size_t buf_sz)
{
    assert(fp);
    assert(buf_sz > 0);

    fflush(fp);
    rewind(fp);

    size_t n = fread(buf, 1, buf_sz - 1, fp);
    buf[n] = '\0';
}

static off_t
file_size(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return st.st_size;
}

static void
read_path(const char *path, char *buf, size_t buf_sz)
{
    FILE *fp = fopen(path, "rb");
    assert(fp != nullptr);
    read_file(fp, buf, buf_sz);
    fclose(fp);
}

static void
make_temp_path(char *path, size_t path_sz)
{
    snprintf(path, path_sz, "/tmp/n00b-display-diag-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
}

static void
run_diag_loop_once(const char *path)
{
    setenv("N00B_DISPLAY_DIAG", path, 1);
    setenv("N00B_DISPLAY_DIAG_LEVEL", "trace", 1);

    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                           .vtable = &diag_loop_renderer);
    n00b_canvas_run(canvas, .tick_ms = 100);
    n00b_canvas_destroy(canvas);
}

static void
test_diagnostics_gating(void)
{
    unsetenv("N00B_DISPLAY_DIAG");
    unsetenv("N00B_DISPLAY_DIAG_LEVEL");

    n00b_display_diag_shutdown();
    assert(!n00b_display_diag_would_log(N00B_DISPLAY_DIAG_ERROR));

    FILE *tmp = tmpfile();
    assert(tmp != nullptr);

    n00b_display_diag_set_stream(tmp);
    n00b_display_diag_set_level(N00B_DISPLAY_DIAG_OFF);
    assert(!n00b_display_diag_would_log(N00B_DISPLAY_DIAG_ERROR));
    n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                           "test",
                           "should-not-appear");

    char buf[512];
    read_file(tmp, buf, sizeof(buf));
    assert(strlen(buf) == 0);

    n00b_display_diag_set_level(N00B_DISPLAY_DIAG_INFO);
    assert(n00b_display_diag_would_log(N00B_DISPLAY_DIAG_ERROR));
    assert(n00b_display_diag_would_log(N00B_DISPLAY_DIAG_INFO));
    assert(!n00b_display_diag_would_log(N00B_DISPLAY_DIAG_TRACE));

    n00b_display_diag_log(N00B_DISPLAY_DIAG_INFO,
                           "test",
                           "hello-info");
    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "test",
                           "hello-trace");

    read_file(tmp, buf, sizeof(buf));
    assert(strstr(buf, "hello-info") != nullptr);
    assert(strstr(buf, "hello-trace") == nullptr);

    fclose(tmp);
    n00b_display_diag_shutdown();

    printf("  [PASS] diagnostics gating and sink policy\n");
}

static void
test_diagnostics_canvas_run_reinitializes(void)
{
    char path_a[] = "/tmp/n00b-display-diag-a.XXXXXX";
    char path_b[] = "/tmp/n00b-display-diag-b.XXXXXX";
    char buf[1024];

    make_temp_path(path_a, sizeof(path_a));
    make_temp_path(path_b, sizeof(path_b));

    n00b_display_diag_shutdown();
    run_diag_loop_once(path_a);
    off_t size_a_before = file_size(path_a);
    assert(size_a_before > 0);

    run_diag_loop_once(path_b);
    off_t size_a_after = file_size(path_a);
    off_t size_b_after = file_size(path_b);

    assert(size_a_after == size_a_before);
    assert(size_b_after > 0);

    read_path(path_a, buf, sizeof(buf));
    assert(strstr(buf, "initial-render") != nullptr);
    read_path(path_b, buf, sizeof(buf));
    assert(strstr(buf, "initial-render") != nullptr);

    unlink(path_a);
    unlink(path_b);
    unsetenv("N00B_DISPLAY_DIAG");
    unsetenv("N00B_DISPLAY_DIAG_LEVEL");
    n00b_display_diag_shutdown();

    printf("  [PASS] diagnostics shutdown between canvas runs\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display diagnostics tests...\n");
    test_diagnostics_gating();
    test_diagnostics_canvas_run_reinitializes();

    printf("Display diagnostics tests passed.\n");
    n00b_shutdown();
    return 0;
}
