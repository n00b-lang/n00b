#include <assert.h>
#include <stdio.h>
#include <string.h>

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
#include "text/strings/string_ops.h"

extern n00b_string_t *n00b_stream_backend_get_buffer(void *ctx);
extern void          n00b_stream_backend_set_size(void         *ctx,
                                                   n00b_isize_t  rows,
                                                   n00b_isize_t  cols);

static int g_button_clicks = 0;

static n00b_string_t *
make_str(const char *s)
{
    return n00b_string_from_raw(s, (int64_t)strlen(s));
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
    assert(hex != nullptr);

    int64_t len = 0;
    char *data = n00b_buffer_to_c(hex, &len);
    assert(data != nullptr);
    assert(len > 0);

    int64_t line_len = 0;
    while (line_len < len && data[line_len] != '\n') {
        line_len++;
    }

    return n00b_string_from_raw(data, line_len);
}

static void
test_m4_widget_table_flow(void)
{
    g_button_clicks = 0;

    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, 18, 72);
    n00b_canvas_resize(canvas, 18, 72);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width        = 72;
    root->height       = 18;

    n00b_plane_t *status = n00b_label_new(
        n00b_string_from_cstr("status=idle"),
        .canvas = canvas,
        .width  = 20,
        .height = 1);

    n00b_plane_t *button = n00b_button_new(
        n00b_string_from_cstr("Run"),
        .canvas        = canvas,
        .width         = 10,
        .height        = 1,
        .on_click      = on_run_click,
        .on_click_data = status);

    n00b_plane_t *checkbox = n00b_checkbox_new(
        n00b_string_from_cstr("Enable"),
        .canvas = canvas,
        .width  = 14,
        .height = 1);

    n00b_table_style_t style = n00b_table_style_ascii();
    n00b_table_t *table = n00b_new_kargs(
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

    n00b_plane_t *table_plane = n00b_table_render(table, .width = 34);
    assert(table_plane != nullptr);

    n00b_plane_t *hexdump_label = n00b_label_new(
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

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    assert(n00b_focus_mgr_set(fm, button));

    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE},
    };
    n00b_display_dispatch_result_t enter_result =
        n00b_display_dispatch_event(canvas, fm, &enter);
    assert(enter_result.handled);
    assert(g_button_clicks == 1);
    assert(n00b_unicode_str_contains(n00b_label_get_text(status), r"status=run"));

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
    n00b_display_dispatch_result_t click_result =
        n00b_display_dispatch_event(canvas, fm, &click_checkbox);
    assert(click_result.handled);
    assert(n00b_checkbox_is_checked(checkbox));

    n00b_canvas_render(canvas);

    n00b_string_t *stream = n00b_stream_backend_get_buffer(canvas->backend_ctx);
    assert(stream != nullptr);
    assert(n00b_unicode_str_contains(stream, r"status=run"));
    assert(n00b_unicode_str_contains(stream, r"Component"));
    assert(n00b_unicode_str_contains(stream, r"00000000"));

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, status);
    n00b_plane_remove_child(root, button);
    n00b_plane_remove_child(root, checkbox);
    n00b_plane_remove_child(root, table_plane);
    n00b_plane_remove_child(root, hexdump_label);
    n00b_widget_detach(status);
    n00b_widget_detach(button);
    n00b_widget_detach(checkbox);
    n00b_widget_detach(hexdump_label);
    n00b_plane_destroy(status);
    n00b_plane_destroy(button);
    n00b_plane_destroy(checkbox);
    n00b_plane_destroy(hexdump_label);
    n00b_plane_destroy(table_plane);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);
    n00b_table_destroy(table);

    printf("  [PASS] display m4 widget/table/hexdump flow\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display m4 widget/table flow integration test...\n");
    test_m4_widget_table_flow();
    printf("Display m4 widget/table flow integration test passed.\n");

    n00b_shutdown();
    return 0;
}
