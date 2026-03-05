#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event_loop.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"

#define DEFAULT_OUT_DIR "plans/artifacts/display-rewrite/m2"
#define TOOL_VERSION    "1"

typedef struct {
    n00b_event_t       events[16];
    size_t             count;
    size_t             ix;
    int                poll_calls;
    int                render_calls;
    bool               saw_cursor_hide;
    bool               saw_cursor_show;
    n00b_render_size_t size;
} replay_backend_t;

typedef struct {
    int key_events;
    int activations;
    int mouse_presses;
} replay_widget_state_t;

typedef struct {
    int         calls;
    n00b_isize_t rows;
    n00b_isize_t cols;
} replay_resize_observer_t;

static int
ensure_dir_recursive(const char *dir)
{
    char path[PATH_MAX];
    size_t len = strlen(dir);

    if (len == 0 || len >= sizeof(path)) {
        return -1;
    }

    memcpy(path, dir, len + 1);
    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
    }

    for (char *p = path + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

static int
build_path(char *out, size_t out_sz, const char *dir, const char *name)
{
    int n = snprintf(out, out_sz, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

static int
write_bytes_file(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }

    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) {
        return -1;
    }
    return 0;
}

static void *
replay_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    replay_backend_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return nullptr;
    }

    ctx->size.rows = 8;
    ctx->size.cols = 40;
    ctx->size.cell_pixel_w = 1;
    ctx->size.cell_pixel_h = 1;

    ctx->events[0] = (n00b_event_t){
        .type = N00B_EVENT_RESIZE,
        .resize = {.rows = 8, .cols = 40},
    };
    ctx->events[1] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_NONE},
    };
    ctx->events[2] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'x', .mods = N00B_MOD_NONE},
    };
    ctx->events[3] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_SHIFT},
    };
    ctx->events[4] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'y', .mods = N00B_MOD_NONE},
    };
    ctx->events[5] = (n00b_event_t){
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x = 14,
            .y = 2,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods = N00B_MOD_NONE,
        },
    };
    ctx->events[6] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = ' ', .mods = N00B_MOD_NONE},
    };
    ctx->events[7] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'c', .mods = N00B_MOD_CTRL},
    };
    ctx->count = 8;

    return ctx;
}

static void
replay_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
replay_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_MANAGES_TTY;
}

static n00b_render_size_t
replay_get_size(void *vctx)
{
    replay_backend_t *ctx = vctx;
    return ctx->size;
}

static void
replay_render_frame(void         *vctx,
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
replay_flush(void *vctx)
{
    (void)vctx;
}

static void
replay_render_planes(void                         *vctx,
                     const n00b_composite_entry_t *entries,
                     n00b_isize_t                  count,
                     n00b_isize_t                  total_rows,
                     n00b_isize_t                  total_cols,
                     n00b_text_style_t            *default_style,
                     n00b_render_cap_t             caps)
{
    replay_backend_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
    ctx->render_calls++;
}

static void
replay_cursor_visible(void *vctx, bool visible)
{
    replay_backend_t *ctx = vctx;
    if (visible) {
        ctx->saw_cursor_show = true;
    }
    else {
        ctx->saw_cursor_hide = true;
    }
}

static bool
replay_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    replay_backend_t *ctx = vctx;
    (void)timeout_ms;
    ctx->poll_calls++;

    if (ctx->ix < ctx->count) {
        *out = ctx->events[ctx->ix++];
        return true;
    }

    out->type = N00B_EVENT_NONE;
    return false;
}

static void
replay_widget_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
    n00b_plane_draw_glyph(plane, 0, 0, 'W');
}

static bool
replay_widget_handle(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    replay_widget_state_t *state = data;

    if (event->type == N00B_EVENT_MOUSE
        && event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS) {
        state->mouse_presses++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY && event->key.key == ' ') {
        state->activations++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY
        && event->key.key >= 'a'
        && event->key.key <= 'z') {
        state->key_events++;
        return true;
    }

    return false;
}

static bool
replay_widget_focusable(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t replay_widget = {
    .kind         = "terminal_replay",
    .render       = replay_widget_render,
    .handle_event = replay_widget_handle,
    .can_focus    = replay_widget_focusable,
};

static const n00b_renderer_vtable_t replay_renderer = {
    .name               = "terminal_replay_backend",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = replay_init,
    .destroy            = replay_destroy,
    .capabilities       = replay_caps,
    .get_size           = replay_get_size,
    .render_frame       = replay_render_frame,
    .flush              = replay_flush,
    .render_planes      = replay_render_planes,
    .cursor_set_visible = replay_cursor_visible,
    .poll_event         = replay_poll_event,
};

static void
on_resize(n00b_canvas_t *canvas, void *data)
{
    replay_resize_observer_t *obs = data;
    obs->calls++;
    obs->rows = canvas->frame_rows;
    obs->cols = canvas->frame_cols;
}

static int
write_replay_log(const char *out_dir, const char *text)
{
    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "terminal_replay.txt") != 0) {
        return -1;
    }

    if (write_bytes_file(path, text, strlen(text)) != 0) {
        return -1;
    }

    printf("wrote terminal_replay.txt\n");
    return 0;
}

static int
write_metadata(const char *out_dir)
{
    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "metadata.txt") != 0) {
        return -1;
    }

    char text[512];
    int n = snprintf(text,
                     sizeof(text),
                     "tool=display_terminal_replay\n"
                     "tool_version=%s\n"
                     "sequence=Resize,Tab,x,Shift+Tab,y,MousePress,Space,Ctrl-C\n"
                     "expected=left.key_events=1,right.key_events=1,right.mouse_presses=1,right.activations=1\n"
                     "n00b_version=%u.%u.%u\n",
                     TOOL_VERSION,
                     (unsigned)N00B_VERS_MAJOR,
                     (unsigned)N00B_VERS_MINOR,
                     (unsigned)N00B_VERS_PATCH);
    if (n < 0 || (size_t)n >= sizeof(text)) {
        return -1;
    }

    if (write_bytes_file(path, text, (size_t)n) != 0) {
        return -1;
    }

    printf("wrote metadata.txt\n");
    return 0;
}

static int
run_replay(const char *out_dir)
{
    int rc = 1;
    n00b_canvas_t *canvas = nullptr;
    n00b_plane_t *root = nullptr;
    n00b_plane_t *left_plane = nullptr;
    n00b_plane_t *right_plane = nullptr;
    replay_widget_state_t left = {};
    replay_widget_state_t right = {};
    replay_resize_observer_t resize = {};

    canvas = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &replay_renderer);
    if (!canvas) {
        goto done;
    }
    n00b_canvas_resize(canvas, 8, 40);

    root = n00b_new_kargs(n00b_plane_t, plane);
    left_plane = n00b_new_kargs(n00b_plane_t, plane);
    right_plane = n00b_new_kargs(n00b_plane_t, plane);
    if (!root || !left_plane || !right_plane) {
        goto done;
    }

    root->width = 40;
    root->height = 8;
    left_plane->width = 8;
    left_plane->height = 1;
    right_plane->width = 8;
    right_plane->height = 1;

    n00b_widget_attach(left_plane, &replay_widget, &left);
    n00b_widget_attach(right_plane, &replay_widget, &right);
    n00b_plane_add_child(root, left_plane, 2, 2);
    n00b_plane_add_child(root, right_plane, 14, 2);
    n00b_canvas_add_plane(canvas, root);

    n00b_canvas_run(canvas,
                     .tick_ms = 100,
                     .on_resize = on_resize,
                     .resize_data = &resize);

    replay_backend_t *ctx = canvas->backend_ctx;
    char log[1024];
    int n = snprintf(log,
                     sizeof(log),
                     "replay=display_terminal_replay\n"
                     "resize.calls=%d\n"
                     "resize.rows=%d\n"
                     "resize.cols=%d\n"
                     "left.key_events=%d\n"
                     "right.key_events=%d\n"
                     "right.mouse_presses=%d\n"
                     "right.activations=%d\n"
                     "cursor.hide=%d\n"
                     "cursor.show=%d\n"
                     "poll.calls=%d\n"
                     "render.calls=%d\n",
                     resize.calls,
                     (int)resize.rows,
                     (int)resize.cols,
                     left.key_events,
                     right.key_events,
                     right.mouse_presses,
                     right.activations,
                     ctx->saw_cursor_hide ? 1 : 0,
                     ctx->saw_cursor_show ? 1 : 0,
                     ctx->poll_calls,
                     ctx->render_calls);
    if (n < 0 || (size_t)n >= sizeof(log)) {
        goto done;
    }

    if (write_replay_log(out_dir, log) != 0) {
        goto done;
    }
    if (write_metadata(out_dir) != 0) {
        goto done;
    }

    rc = 0;

done:
    if (canvas && root) {
        n00b_canvas_remove_plane(canvas, root);
    }
    if (root && left_plane) {
        n00b_plane_remove_child(root, left_plane);
    }
    if (root && right_plane) {
        n00b_plane_remove_child(root, right_plane);
    }
    if (left_plane) {
        n00b_widget_detach(left_plane);
        n00b_plane_destroy(left_plane);
    }
    if (right_plane) {
        n00b_widget_detach(right_plane);
        n00b_plane_destroy(right_plane);
    }
    if (root) {
        n00b_plane_destroy(root);
    }
    if (canvas) {
        n00b_canvas_destroy(canvas);
    }

    return rc;
}

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--out-dir PATH]\n"
            "Run deterministic terminal replay and write artifact logs.\n",
            prog);
}

int
main(int argc, char **argv)
{
    const char *out_dir = DEFAULT_OUT_DIR;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out-dir") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --out-dir requires a value.\n");
                return 1;
            }
            out_dir = argv[i];
            continue;
        }

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    if (ensure_dir_recursive(out_dir) != 0) {
        fprintf(stderr, "Error: could not create output directory '%s': %s\n",
                out_dir, strerror(errno));
        n00b_shutdown();
        return 1;
    }

    int rc = run_replay(out_dir);
    if (rc != 0) {
        fprintf(stderr, "Error: terminal replay failed.\n");
    }

    n00b_shutdown();
    return rc;
}
