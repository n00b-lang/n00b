#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend.h"
#include "display/render/backend_registry.h"
#include "display/render/canvas.h"
#include "display/event_loop.h"
#include "internal/display/diagnostics.h"

static void
set_backend_override(const char *value)
{
#ifdef _WIN32
    _putenv_s("N00B_RENDERER_BACKEND", value ? value : "");
#else
    if (value) {
        setenv("N00B_RENDERER_BACKEND", value, 1);
    }
    else {
        unsetenv("N00B_RENDERER_BACKEND");
    }
#endif
}

static void *
fail_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    errno = EIO;
    return nullptr;
}

static void
fail_destroy(void *ctx)
{
    (void)ctx;
}

static n00b_render_cap_t
fail_caps(void *ctx)
{
    (void)ctx;
    return N00B_RCAP_NONE;
}

static n00b_render_size_t
fail_get_size(void *ctx)
{
    (void)ctx;
    return (n00b_render_size_t){0};
}

static void
fail_render_frame(void         *ctx,
                  n00b_rcell_t *cells,
                  n00b_isize_t  rows,
                  n00b_isize_t  cols,
                  n00b_rcell_t *prev_cells)
{
    (void)ctx;
    (void)cells;
    (void)rows;
    (void)cols;
    (void)prev_cells;
}

static void
fail_flush(void *ctx)
{
    (void)ctx;
}

static void
fail_render_planes(void                         *ctx,
                   const n00b_composite_entry_t *entries,
                   n00b_isize_t                  count,
                   n00b_isize_t                  total_rows,
                   n00b_isize_t                  total_cols,
                   n00b_text_style_t            *default_style,
                   n00b_render_cap_t             caps)
{
    (void)ctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
}

static const n00b_renderer_vtable_t fail_vtable = {
    .name          = "m5_fail",
    .version       = N00B_RENDERER_ABI_VERSION,
    .init          = fail_init,
    .destroy       = fail_destroy,
    .capabilities  = fail_caps,
    .get_size      = fail_get_size,
    .render_frame  = fail_render_frame,
    .flush         = fail_flush,
    .render_planes = fail_render_planes,
};

static void
read_path(const char *path, char *buf, size_t buf_sz)
{
    assert(buf_sz > 0);

    FILE *fp = fopen(path, "rb");
    assert(fp != nullptr);

    size_t n = fread(buf, 1, buf_sz - 1, fp);
    buf[n] = '\0';

    fclose(fp);
}

static void
make_temp_path(char *path, size_t path_sz)
{
    snprintf(path, path_sz, "/tmp/n00b-display-m5-diag-XXXXXX");
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
}

static void
test_explicit_stream_startup(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas,
                     .backend_name               = r"stream",
                     .backend_allow_fallback     = false,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = false,
                     .output                     = output);

    assert(n00b_canvas_backend_ready(canvas));
    assert(n00b_canvas_backend_error(canvas) == 0);
    assert(canvas->vtable == &n00b_renderer_stream);

    n00b_canvas_destroy(canvas);
    printf("  [PASS] m5 explicit backend startup\n");
}

static void
test_auto_env_override_startup(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    set_backend_override("stream");

    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas,
                     .backend_name               = r"auto",
                     .backend_allow_fallback     = true,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = true,
                     .output                     = output);

    assert(n00b_canvas_backend_ready(canvas));
    assert(n00b_canvas_backend_error(canvas) == 0);
    assert(canvas->vtable);
    assert(canvas->vtable->name);
    assert(strcmp(canvas->vtable->name, "stream") == 0);

    n00b_canvas_destroy(canvas);
    set_backend_override(nullptr);
    printf("  [PASS] m5 auto backend startup with env override\n");
}

static void
test_missing_backend_fallback(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    char diag_path[] = "/tmp/n00b-display-m5-diag-XXXXXX";
    char diag_log[2048];
    char err_field[32];

    make_temp_path(diag_path, sizeof(diag_path));
    setenv("N00B_DISPLAY_DIAG", diag_path, 1);
    unsetenv("N00B_DISPLAY_DIAG_LEVEL");
    n00b_display_diag_shutdown();

    n00b_renderer_register(r"m5_fail", &fail_vtable);

    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas,
                     .backend_name               = r"m5_fail",
                     .backend_allow_fallback     = true,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = false,
                     .output                     = output);

    assert(n00b_canvas_backend_ready(canvas));
    assert(n00b_canvas_backend_error(canvas) == 0);
    assert(canvas->vtable);
    assert(canvas->vtable != &fail_vtable);
    assert(strcmp(canvas->vtable->name, "m5_fail") != 0);

    n00b_canvas_destroy(canvas);
    n00b_display_diag_shutdown();

    read_path(diag_path, diag_log, sizeof(diag_log));
    snprintf(err_field, sizeof(err_field), "failed_err=%d", EIO);
    assert(strstr(diag_log, "backend selected: requested=m5_fail") != nullptr);
    assert(strstr(diag_log, "fallback=true") != nullptr);
    assert(strstr(diag_log, "failed_candidate=m5_fail") != nullptr);
    assert(strstr(diag_log, err_field) != nullptr);
    assert(strstr(diag_log, "backend candidate init failed") == nullptr);

    unlink(diag_path);
    unsetenv("N00B_DISPLAY_DIAG");
    unsetenv("N00B_DISPLAY_DIAG_LEVEL");
    n00b_display_diag_shutdown();

    printf("  [PASS] m5 missing-backend fallback startup\n");
}

static void
test_failed_backend_startup_is_safe(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    n00b_renderer_register(r"m5_fail_only", &fail_vtable);

    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas,
                     .backend_name               = r"m5_fail_only",
                     .backend_allow_fallback     = false,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = false,
                     .output                     = output);

    assert(!n00b_canvas_backend_ready(canvas));
    assert(n00b_canvas_backend_error(canvas) != 0);

    n00b_canvas_render(canvas);
    n00b_canvas_run(canvas, .tick_ms = 1);

    n00b_canvas_destroy(canvas);
    printf("  [PASS] m5 failed backend startup is safe\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    printf("Running display m5 runtime selection integration test...\n");
    test_explicit_stream_startup(stdout_topic);
    test_auto_env_override_startup(stdout_topic);
    test_missing_backend_fallback(stdout_topic);
    test_failed_backend_startup_is_safe(stdout_topic);
    printf("Display m5 runtime selection integration test passed.\n");

    n00b_shutdown();
    return 0;
}
