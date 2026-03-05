#include <errno.h>
#include <limits.h>
#include <stdbool.h>
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

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

#define DEFAULT_OUT_DIR "plans/artifacts/display-rewrite/m3"
#define TOOL_VERSION    "1"

typedef struct {
    n00b_event_t       events[16];
    size_t             count;
    size_t             ix;
    int                poll_calls;
    int                render_calls;
    bool               saw_cursor_hide;
    bool               saw_cursor_show;
    bool               gui_mode;
    n00b_render_size_t size;
} parity_backend_t;

typedef struct {
    int key_events;
    int activations;
    int mouse_presses;
} parity_widget_state_t;

typedef struct {
    int         calls;
    n00b_isize_t rows;
    n00b_isize_t cols;
} parity_resize_observer_t;

typedef struct {
    bool        gui_mode;
    int         resize_calls;
    n00b_isize_t resize_rows;
    n00b_isize_t resize_cols;
    int         left_key_events;
    int         right_key_events;
    int         left_activations;
    int         right_activations;
    int         left_mouse_presses;
    int         right_mouse_presses;
    int         poll_calls;
    int         render_calls;
    bool        cursor_hide;
    bool        cursor_show;
    size_t      events_consumed;
    size_t      events_total;
    uint32_t    next_key_after_stop;
} parity_result_t;

static void
populate_terminal_script(parity_backend_t *ctx)
{
    ctx->events[0] = (n00b_event_t){
        .type = N00B_EVENT_RESIZE,
        .resize = {.rows = 8, .cols = 40},
    };
    ctx->events[1] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'\t', .mods = N00B_MOD_NONE},
    };
    ctx->events[2] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'x', .mods = N00B_MOD_NONE},
    };
    ctx->events[3] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_SHIFT},
    };
    ctx->events[4] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'y', .mods = N00B_MOD_NONE},
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
        .key = {.key = (uint32_t)' ', .mods = N00B_MOD_NONE},
    };
    ctx->events[7] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'\r', .mods = N00B_MOD_NONE},
    };
    ctx->events[8] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)3, .mods = N00B_MOD_NONE},
    };
    ctx->events[9] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'z', .mods = N00B_MOD_NONE},
    };

    ctx->count = 10;
}

static void
populate_gui_script(parity_backend_t *ctx)
{
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
        .key = {.key = (uint32_t)'x', .mods = N00B_MOD_NONE},
    };
    ctx->events[3] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_SHIFT},
    };
    ctx->events[4] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'y', .mods = N00B_MOD_NONE},
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
        .key = {.key = (uint32_t)' ', .mods = N00B_MOD_NONE},
    };
    ctx->events[7] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE},
    };
    ctx->events[8] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'c', .mods = N00B_MOD_CTRL},
    };
    ctx->events[9] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'z', .mods = N00B_MOD_NONE},
    };

    ctx->count = 10;
}

static void *
parity_init_mode(bool gui_mode)
{
    parity_backend_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return nullptr;
    }

    ctx->gui_mode = gui_mode;
    ctx->size.rows = 8;
    ctx->size.cols = 40;
    ctx->size.cell_pixel_w = 1;
    ctx->size.cell_pixel_h = 1;

    if (gui_mode) {
        populate_gui_script(ctx);
    }
    else {
        populate_terminal_script(ctx);
    }

    return ctx;
}

static void *
parity_init_terminal(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    return parity_init_mode(false);
}

static void *
parity_init_gui(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    return parity_init_mode(true);
}

static void
parity_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
parity_caps(void *vctx)
{
    parity_backend_t *ctx = vctx;
    if (ctx->gui_mode) {
        return N00B_RCAP_MANAGES_TTY
             | N00B_RCAP_COLOR_24BIT
             | N00B_RCAP_MOUSE
             | N00B_RCAP_UNICODE
             | N00B_RCAP_WIDE_CHARS
             | N00B_RCAP_PIXEL_COORDS
             | N00B_RCAP_FONT_METRICS
             | N00B_RCAP_DIFF_RENDER
             | N00B_RCAP_GUI_EXT;
    }

    return N00B_RCAP_MANAGES_TTY
         | N00B_RCAP_COLOR_BASIC
         | N00B_RCAP_CURSOR_MOVE
         | N00B_RCAP_ALT_SCREEN
         | N00B_RCAP_MOUSE
         | N00B_RCAP_UNICODE
         | N00B_RCAP_WIDE_CHARS;
}

static n00b_render_size_t
parity_get_size(void *vctx)
{
    parity_backend_t *ctx = vctx;
    return ctx->size;
}

static void
parity_render_frame(void         *vctx,
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
parity_flush(void *vctx)
{
    (void)vctx;
}

static void
parity_render_planes(void                         *vctx,
                     const n00b_composite_entry_t *entries,
                     n00b_isize_t                  count,
                     n00b_isize_t                  total_rows,
                     n00b_isize_t                  total_cols,
                     n00b_text_style_t            *default_style,
                     n00b_render_cap_t             caps)
{
    parity_backend_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
    ctx->render_calls++;
}

static void
parity_cursor_visible(void *vctx, bool visible)
{
    parity_backend_t *ctx = vctx;
    if (visible) {
        ctx->saw_cursor_show = true;
    }
    else {
        ctx->saw_cursor_hide = true;
    }
}

static bool
parity_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    parity_backend_t *ctx = vctx;
    (void)timeout_ms;
    ctx->poll_calls++;

    if (ctx->ix < ctx->count) {
        *out = ctx->events[ctx->ix++];
        return true;
    }

    out->type = N00B_EVENT_NONE;
    return false;
}

static const n00b_renderer_vtable_t parity_terminal_renderer = {
    .name               = "m3_terminal_profile",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = parity_init_terminal,
    .destroy            = parity_destroy,
    .capabilities       = parity_caps,
    .get_size           = parity_get_size,
    .render_frame       = parity_render_frame,
    .flush              = parity_flush,
    .render_planes      = parity_render_planes,
    .cursor_set_visible = parity_cursor_visible,
    .poll_event         = parity_poll_event,
};

static const n00b_renderer_vtable_t parity_gui_renderer = {
    .name               = "m3_gui_profile",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = parity_init_gui,
    .destroy            = parity_destroy,
    .capabilities       = parity_caps,
    .get_size           = parity_get_size,
    .render_frame       = parity_render_frame,
    .flush              = parity_flush,
    .render_planes      = parity_render_planes,
    .cursor_set_visible = parity_cursor_visible,
    .poll_event         = parity_poll_event,
};

static void
parity_widget_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
    n00b_plane_draw_glyph(plane, 0, 0, 'P');
}

static bool
parity_widget_handle(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    parity_widget_state_t *state = data;

    if (event->type == N00B_EVENT_MOUSE
        && event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS) {
        state->mouse_presses++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY
        && (event->key.key == (uint32_t)' ' || event->key.key == N00B_KEY_ENTER)) {
        state->activations++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY
        && event->key.key >= (uint32_t)'a'
        && event->key.key <= (uint32_t)'z') {
        state->key_events++;
        return true;
    }

    return false;
}

static bool
parity_widget_focusable(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t parity_widget = {
    .kind         = "m3_parity_tool",
    .render       = parity_widget_render,
    .handle_event = parity_widget_handle,
    .can_focus    = parity_widget_focusable,
};

static void
on_resize(n00b_canvas_t *canvas, void *data)
{
    parity_resize_observer_t *obs = data;
    obs->calls++;
    obs->rows = canvas->frame_rows;
    obs->cols = canvas->frame_cols;
}

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

static bool
parity_equivalent(const parity_result_t *terminal, const parity_result_t *gui)
{
    if (!terminal || !gui) {
        return false;
    }

    return terminal->resize_calls == gui->resize_calls
        && terminal->resize_rows == gui->resize_rows
        && terminal->resize_cols == gui->resize_cols
        && terminal->left_key_events == gui->left_key_events
        && terminal->right_key_events == gui->right_key_events
        && terminal->left_activations == gui->left_activations
        && terminal->right_activations == gui->right_activations
        && terminal->left_mouse_presses == gui->left_mouse_presses
        && terminal->right_mouse_presses == gui->right_mouse_presses
        && terminal->cursor_hide == gui->cursor_hide
        && terminal->cursor_show == gui->cursor_show
        && terminal->events_consumed == gui->events_consumed
        && terminal->events_total == gui->events_total
        && terminal->next_key_after_stop == gui->next_key_after_stop;
}

static int
run_parity_case(bool gui_mode, parity_result_t *out)
{
    if (!out) {
        return -1;
    }

    const n00b_renderer_vtable_t *renderer = gui_mode
        ? &parity_gui_renderer
        : &parity_terminal_renderer;

    int rc = -1;
    n00b_canvas_t *canvas = nullptr;
    n00b_plane_t *root = nullptr;
    n00b_plane_t *left_plane = nullptr;
    n00b_plane_t *right_plane = nullptr;
    parity_widget_state_t left = {};
    parity_widget_state_t right = {};
    parity_resize_observer_t resize = {};

    canvas = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = renderer);
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

    n00b_widget_attach(left_plane, &parity_widget, &left);
    n00b_widget_attach(right_plane, &parity_widget, &right);
    n00b_plane_add_child(root, left_plane, 2, 2);
    n00b_plane_add_child(root, right_plane, 14, 2);
    n00b_canvas_add_plane(canvas, root);

    n00b_canvas_run(canvas,
                     .tick_ms = 100,
                     .on_resize = on_resize,
                     .resize_data = &resize);

    parity_backend_t *ctx = canvas->backend_ctx;
    out->gui_mode = gui_mode;
    out->resize_calls = resize.calls;
    out->resize_rows = resize.rows;
    out->resize_cols = resize.cols;
    out->left_key_events = left.key_events;
    out->right_key_events = right.key_events;
    out->left_activations = left.activations;
    out->right_activations = right.activations;
    out->left_mouse_presses = left.mouse_presses;
    out->right_mouse_presses = right.mouse_presses;
    out->poll_calls = ctx->poll_calls;
    out->render_calls = ctx->render_calls;
    out->cursor_hide = ctx->saw_cursor_hide;
    out->cursor_show = ctx->saw_cursor_show;
    out->events_consumed = ctx->ix;
    out->events_total = ctx->count;
    out->next_key_after_stop = 0;
    if (ctx->ix < ctx->count && ctx->events[ctx->ix].type == N00B_EVENT_KEY) {
        out->next_key_after_stop = ctx->events[ctx->ix].key.key;
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

static int
write_parity_report(const char *out_dir,
                    const parity_result_t *terminal,
                    const parity_result_t *gui)
{
    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "parity_report.txt") != 0) {
        return -1;
    }

    bool equivalent = parity_equivalent(terminal, gui);
    char report[2048];
    int n = snprintf(report,
                     sizeof(report),
                     "scenario=focus,key,mouse,resize,stop\n"
                     "terminal.resize.calls=%d\n"
                     "terminal.resize.rows=%d\n"
                     "terminal.resize.cols=%d\n"
                     "terminal.left.key_events=%d\n"
                     "terminal.right.key_events=%d\n"
                     "terminal.left.activations=%d\n"
                     "terminal.right.activations=%d\n"
                     "terminal.left.mouse_presses=%d\n"
                     "terminal.right.mouse_presses=%d\n"
                     "terminal.cursor.hide=%d\n"
                     "terminal.cursor.show=%d\n"
                     "terminal.events_consumed=%zu\n"
                     "terminal.events_total=%zu\n"
                     "terminal.next_key_after_stop=%u\n"
                     "gui.resize.calls=%d\n"
                     "gui.resize.rows=%d\n"
                     "gui.resize.cols=%d\n"
                     "gui.left.key_events=%d\n"
                     "gui.right.key_events=%d\n"
                     "gui.left.activations=%d\n"
                     "gui.right.activations=%d\n"
                     "gui.left.mouse_presses=%d\n"
                     "gui.right.mouse_presses=%d\n"
                     "gui.cursor.hide=%d\n"
                     "gui.cursor.show=%d\n"
                     "gui.events_consumed=%zu\n"
                     "gui.events_total=%zu\n"
                     "gui.next_key_after_stop=%u\n"
                     "parity.equivalent=%d\n",
                     terminal->resize_calls,
                     (int)terminal->resize_rows,
                     (int)terminal->resize_cols,
                     terminal->left_key_events,
                     terminal->right_key_events,
                     terminal->left_activations,
                     terminal->right_activations,
                     terminal->left_mouse_presses,
                     terminal->right_mouse_presses,
                     terminal->cursor_hide ? 1 : 0,
                     terminal->cursor_show ? 1 : 0,
                     terminal->events_consumed,
                     terminal->events_total,
                     (unsigned)terminal->next_key_after_stop,
                     gui->resize_calls,
                     (int)gui->resize_rows,
                     (int)gui->resize_cols,
                     gui->left_key_events,
                     gui->right_key_events,
                     gui->left_activations,
                     gui->right_activations,
                     gui->left_mouse_presses,
                     gui->right_mouse_presses,
                     gui->cursor_hide ? 1 : 0,
                     gui->cursor_show ? 1 : 0,
                     gui->events_consumed,
                     gui->events_total,
                     (unsigned)gui->next_key_after_stop,
                     equivalent ? 1 : 0);

    if (n < 0 || (size_t)n >= sizeof(report)) {
        return -1;
    }

    if (write_bytes_file(path, report, (size_t)n) != 0) {
        return -1;
    }

    printf("wrote parity_report.txt\n");
    return 0;
}

static int
write_parity_metadata(const char *out_dir,
                      bool        cocoa_built,
                      bool        cocoa_smoke_available)
{
    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "parity_metadata.txt") != 0) {
        return -1;
    }

    char text[512];
    int n = snprintf(text,
                     sizeof(text),
                     "tool=display_gui_parity_report\n"
                     "tool_version=%s\n"
                     "subset=focus_traversal,key_activation,mouse_press,resize,stop\n"
                     "cocoa_built=%d\n"
                     "cocoa_smoke_available=%d\n"
                     "n00b_version=%u.%u.%u\n",
                     TOOL_VERSION,
                     cocoa_built ? 1 : 0,
                     cocoa_smoke_available ? 1 : 0,
                     (unsigned)N00B_VERS_MAJOR,
                     (unsigned)N00B_VERS_MINOR,
                     (unsigned)N00B_VERS_PATCH);

    if (n < 0 || (size_t)n >= sizeof(text)) {
        return -1;
    }

    if (write_bytes_file(path, text, (size_t)n) != 0) {
        return -1;
    }

    printf("wrote parity_metadata.txt\n");
    return 0;
}

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--out-dir PATH]\n"
            "Run deterministic backend parity and write M3 artifacts.\n",
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
        fprintf(stderr,
                "Error: could not create output directory '%s': %s\n",
                out_dir,
                strerror(errno));
        n00b_shutdown();
        return 1;
    }

    parity_result_t terminal = {};
    parity_result_t gui = {};

    if (run_parity_case(false, &terminal) != 0
        || run_parity_case(true, &gui) != 0) {
        fprintf(stderr, "Error: parity scenario failed.\n");
        n00b_shutdown();
        return 1;
    }

    if (write_parity_report(out_dir, &terminal, &gui) != 0) {
        fprintf(stderr, "Error: failed writing parity_report.txt\n");
        n00b_shutdown();
        return 1;
    }

    bool cocoa_built = false;
    bool cocoa_smoke_available = false;
#if defined(__APPLE__)
    cocoa_built = true;
    cocoa_smoke_available = CGMainDisplayID() != 0;
#endif

    if (write_parity_metadata(out_dir, cocoa_built, cocoa_smoke_available) != 0) {
        fprintf(stderr, "Error: failed writing parity_metadata.txt\n");
        n00b_shutdown();
        return 1;
    }

    bool equivalent = parity_equivalent(&terminal, &gui);
    n00b_shutdown();

    if (!equivalent) {
        fprintf(stderr, "Error: backend parity mismatch (see parity_report.txt).\n");
        return 1;
    }

    return 0;
}
