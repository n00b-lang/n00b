#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "display/event.h"
#include "display/focus.h"
#include "display/hexdump.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/table/table.h"
#include "display/widget.h"
#include "display/widgets/button.h"
#include "display/widgets/checkbox.h"
#include "display/widgets/label.h"
#include "internal/display/event_dispatch.h"

extern n00b_string_t *n00b_stream_backend_get_buffer(void *ctx);
extern void          n00b_stream_backend_set_size(void         *ctx,
                                                   n00b_isize_t  rows,
                                                   n00b_isize_t  cols);

#define DEFAULT_OUT_DIR "plans/artifacts/display-rewrite/m4"
#define TOOL_VERSION    "1"
#define SHOWCASE_ROWS   18
#define SHOWCASE_COLS   72

static int g_button_clicks = 0;

static n00b_string_t *
make_str(const char *s)
{
    return n00b_string_from_raw(s, (int64_t)strlen(s));
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

static void
on_run_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    g_button_clicks++;

    n00b_plane_t *status = (n00b_plane_t *)data;
    n00b_label_set_text(status, n00b_string_from_cstr("status=run"));
}

static n00b_string_t *
first_hexdump_line(void)
{
    uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef, 0x41, 0x42, 0x43, 0x44};
    n00b_buffer_t *buf = n00b_buffer_from_bytes((char *)payload, sizeof(payload));
    n00b_buffer_t *hex = n00b_hexdump_buf(buf, .width = 48);
    if (!hex) {
        return n00b_string_from_cstr("hexdump unavailable");
    }

    int64_t len = 0;
    char *data = n00b_buffer_to_c(hex, &len);
    if (!data || len <= 0) {
        return n00b_string_from_cstr("hexdump unavailable");
    }

    int64_t line_len = 0;
    while (line_len < len && data[line_len] != '\n') {
        line_len++;
    }

    return n00b_string_from_raw(data, line_len);
}

static int
write_showcase_stream(const char *out_dir)
{
    g_button_clicks = 0;

    n00b_canvas_t    *canvas = nullptr;
    n00b_plane_t     *root = nullptr;
    n00b_plane_t     *status = nullptr;
    n00b_plane_t     *button = nullptr;
    n00b_plane_t     *checkbox = nullptr;
    n00b_plane_t     *table_plane = nullptr;
    n00b_plane_t     *hexdump_label = nullptr;
    n00b_focus_mgr_t *fm = nullptr;
    n00b_table_t     *table = nullptr;
    int               rc = -1;

    canvas = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, SHOWCASE_ROWS, SHOWCASE_COLS);
    n00b_canvas_resize(canvas, SHOWCASE_ROWS, SHOWCASE_COLS);

    root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = SHOWCASE_COLS;
    root->height = SHOWCASE_ROWS;

    status = n00b_label_new(
        n00b_string_from_cstr("status=idle"),
        .canvas = canvas,
        .width  = 20,
        .height = 1);

    button = n00b_button_new(
        n00b_string_from_cstr("Run"),
        .canvas        = canvas,
        .width         = 10,
        .height        = 1,
        .on_click      = on_run_click,
        .on_click_data = status);

    checkbox = n00b_checkbox_new(
        n00b_string_from_cstr("Enable"),
        .canvas = canvas,
        .width  = 14,
        .height = 1);

    n00b_table_style_t style = n00b_table_style_ascii();
    table = n00b_new_kargs(
        n00b_table_t,
        table,
        .num_cols    = 2,
        .table_props = style.table_props,
        .cell_props  = style.cell_props);

    n00b_table_add_cell(table, make_str("Component"));
    n00b_table_add_cell(table, make_str("State"));
    n00b_table_end_row(table);
    n00b_table_add_cell(table, make_str("widget"));
    n00b_table_add_cell(table, make_str("ready"));
    n00b_table_end_row(table);

    table_plane = n00b_table_render(table, .width = 34);
    if (!table_plane) {
        goto done;
    }

    hexdump_label = n00b_label_new(
        first_hexdump_line(),
        .canvas = canvas,
        .width  = 48,
        .height = 1);

    n00b_plane_add_child(root, status, 2, 1);
    n00b_plane_add_child(root, button, 2, 3);
    n00b_plane_add_child(root, checkbox, 16, 3);
    n00b_plane_add_child(root, table_plane, 2, 5);
    n00b_plane_add_child(root, hexdump_label, 2, 12);
    n00b_canvas_add_plane(canvas, root);

    fm = n00b_focus_mgr_new(canvas);
    (void)n00b_focus_mgr_set(fm, button);

    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE},
    };
    (void)n00b_display_dispatch_event(canvas, fm, &enter);

    n00b_event_t click_checkbox = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x = 16,
            .y = 3,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods = N00B_MOD_NONE,
        },
    };
    (void)n00b_display_dispatch_event(canvas, fm, &click_checkbox);

    n00b_canvas_render(canvas);
    n00b_string_t *buf = n00b_stream_backend_get_buffer(canvas->backend_ctx);
    if (!buf || !buf->data) {
        goto done;
    }

    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "showcase_stream.txt") != 0) {
        goto done;
    }

    if (write_bytes_file(path, buf->data, buf->u8_bytes) != 0) {
        goto done;
    }

    printf("wrote showcase_stream.txt\n");
    rc = 0;

done:
    if (fm) {
        n00b_focus_mgr_destroy(fm);
    }
    if (canvas && root) {
        n00b_canvas_remove_plane(canvas, root);
    }
    if (root && status) {
        n00b_plane_remove_child(root, status);
    }
    if (root && button) {
        n00b_plane_remove_child(root, button);
    }
    if (root && checkbox) {
        n00b_plane_remove_child(root, checkbox);
    }
    if (root && table_plane) {
        n00b_plane_remove_child(root, table_plane);
    }
    if (root && hexdump_label) {
        n00b_plane_remove_child(root, hexdump_label);
    }
    if (status) {
        n00b_widget_detach(status);
        n00b_plane_destroy(status);
    }
    if (button) {
        n00b_widget_detach(button);
        n00b_plane_destroy(button);
    }
    if (checkbox) {
        n00b_widget_detach(checkbox);
        n00b_plane_destroy(checkbox);
    }
    if (hexdump_label) {
        n00b_widget_detach(hexdump_label);
        n00b_plane_destroy(hexdump_label);
    }
    if (table_plane) {
        n00b_plane_destroy(table_plane);
    }
    if (root) {
        n00b_plane_destroy(root);
    }
    if (canvas) {
        n00b_canvas_destroy(canvas);
    }
    if (table) {
        n00b_table_destroy(table);
    }

    return rc;
}

static int
write_hexdump_stream(const char *out_dir)
{
    uint8_t payload[32];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)i;
    }

    n00b_buffer_t *buf = n00b_buffer_from_bytes((char *)payload, sizeof(payload));
    n00b_buffer_t *hex = n00b_hexdump_buf(buf, .width = 64);
    if (!hex) {
        return -1;
    }

    int64_t len = 0;
    char *data = n00b_buffer_to_c(hex, &len);
    if (!data || len <= 0) {
        return -1;
    }

    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "hexdump_stream.txt") != 0) {
        return -1;
    }

    if (write_bytes_file(path, data, (size_t)len) != 0) {
        return -1;
    }

    printf("wrote hexdump_stream.txt\n");
    return 0;
}

static int
write_showcase_metadata(const char *out_dir)
{
    char path[PATH_MAX];
    if (build_path(path, sizeof(path), out_dir, "showcase_metadata.txt") != 0) {
        return -1;
    }

    char contents[512];
    int n = snprintf(contents,
                     sizeof(contents),
                     "tool=display_m4_showcase\n"
                     "tool_version=%s\n"
                     "backend=stream\n"
                     "rows=%d\n"
                     "cols=%d\n"
                     "button_clicks=%d\n"
                     "n00b_version=%u.%u.%u\n",
                     TOOL_VERSION,
                     SHOWCASE_ROWS,
                     SHOWCASE_COLS,
                     g_button_clicks,
                     (unsigned)N00B_VERS_MAJOR,
                     (unsigned)N00B_VERS_MINOR,
                     (unsigned)N00B_VERS_PATCH);
    if (n < 0 || (size_t)n >= sizeof(contents)) {
        return -1;
    }

    if (write_bytes_file(path, contents, (size_t)n) != 0) {
        return -1;
    }

    printf("wrote showcase_metadata.txt\n");
    return 0;
}

static void
print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--out-dir PATH]\n"
            "Generate deterministic Milestone 4 showcase artifacts.\n",
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
        return 1;
    }

    if (write_showcase_stream(out_dir) != 0) {
        fprintf(stderr, "Error: failed writing showcase stream.\n");
        return 1;
    }

    if (write_hexdump_stream(out_dir) != 0) {
        fprintf(stderr, "Error: failed writing hexdump stream.\n");
        return 1;
    }

    if (write_showcase_metadata(out_dir) != 0) {
        fprintf(stderr, "Error: failed writing showcase metadata.\n");
        return 1;
    }

    n00b_shutdown();
    return 0;
}
