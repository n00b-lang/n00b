#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/event.h"
#include "display/focus.h"
#include "display/hexdump.h"
#include "display/render/backend.h"
#include "display/backend_stream_internal.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/table/table.h"
#include "display/widget.h"
#include "text/strings/string_ops.h"

static n00b_string_t *
make_str(const char *s)
{
    return n00b_string_from_raw(s, (int64_t)strlen(s));
}

static bool
buffer_contains(n00b_buffer_t *buf, const char *needle)
{
    int64_t len  = 0;
    char   *data = n00b_buffer_to_c(buf, &len);

    (void)len;
    return data != nullptr && strstr(data, needle) != nullptr;
}

static void
dummy_render(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static bool
dummy_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    (void)data;
    (void)event;
    return false;
}

static bool
dummy_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t focusable_vtable = {
    .kind         = "baseline_focusable",
    .render       = dummy_render,
    .handle_event = dummy_handle_event,
    .can_focus    = dummy_can_focus,
};

static const n00b_widget_vtable_t nonfocusable_vtable = {
    .kind   = "baseline_nonfocusable",
    .render = dummy_render,
};

static n00b_plane_t *
make_focusable_plane(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width  = 12;
    p->height = 1;
    n00b_widget_attach(p, &focusable_vtable, nullptr);
    return p;
}

static n00b_plane_t *
make_nonfocusable_plane(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width  = 12;
    p->height = 1;
    n00b_widget_attach(p, &nonfocusable_vtable, nullptr);
    return p;
}

static void
test_canvas_and_composition_contract(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, 6, 20);
    n00b_canvas_resize(canvas, 6, 20);

    n00b_plane_t *back = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    back->width        = 20;
    back->height       = 6;
    n00b_plane_draw_glyph(back, 0, 0, 'B');

    n00b_plane_t *front = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    front->width        = 20;
    front->height       = 6;
    n00b_plane_draw_glyph(front, 0, 0, 'F');

    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    child->width        = 4;
    child->height       = 1;
    n00b_plane_draw_glyph(child, 0, 0, 'C');
    n00b_plane_add_child(front, child, 1, 0);

    n00b_canvas_add_plane(canvas, back);
    n00b_canvas_add_plane(canvas, front);
    n00b_canvas_render(canvas);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(canvas->backend_ctx);
    assert(buf != nullptr);
    assert(n00b_stream_backend_get_length(canvas->backend_ctx) > 0);
    assert(buf->data[0] == 'F');
    assert(n00b_unicode_str_contains(buf, r"C"));

    n00b_canvas_remove_plane(canvas, front);
    n00b_canvas_remove_plane(canvas, back);
    n00b_plane_remove_child(front, child);
    n00b_plane_destroy(child);
    n00b_plane_destroy(front);
    n00b_plane_destroy(back);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] canvas and composition contract\n");
}

static void
test_event_normalization_contract(void)
{
    n00b_event_t ev = {
        .type = N00B_EVENT_KEY,
        .key  = {.key = '\t', .mods = N00B_MOD_SHIFT},
    };
    n00b_event_normalize(&ev);
    assert(ev.key.key == N00B_KEY_TAB);
    assert(ev.key.mods == N00B_MOD_SHIFT);

    ev = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key  = {.key = '\n', .mods = N00B_MOD_CTRL},
    };
    n00b_event_normalize(&ev);
    assert(ev.key.key == N00B_KEY_ENTER);
    assert(ev.key.mods == N00B_MOD_NONE);

    ev = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key  = {.key = 3, .mods = N00B_MOD_NONE},
    };
    n00b_event_normalize(&ev);
    assert(ev.key.key == 'c');
    assert(ev.key.mods == N00B_MOD_CTRL);

    ev = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key  = {.key = 'Z', .mods = N00B_MOD_CTRL},
    };
    n00b_event_normalize(&ev);
    assert(ev.key.key == 'z');
    assert(ev.key.mods == N00B_MOD_CTRL);

    printf("  [PASS] event normalization contract\n");
}

static void
test_focus_contract(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width        = 80;
    root->height       = 25;

    n00b_plane_t *f1 = make_focusable_plane();
    n00b_plane_t *nf = make_nonfocusable_plane();
    n00b_plane_t *f2 = make_focusable_plane();

    n00b_plane_add_child(root, f1, 0, 0);
    n00b_plane_add_child(root, nf, 0, 1);
    n00b_plane_add_child(root, f2, 0, 2);
    n00b_canvas_add_plane(canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    assert(fm->count == 2);
    assert(n00b_focus_mgr_current(fm) == f1);
    assert(f1->widget_state == N00B_WSTATE_FOCUSED);

    assert(n00b_focus_mgr_next(fm) == f2);
    assert(n00b_focus_mgr_prev(fm) == f1);

    assert(n00b_plane_remove_child(root, f1));
    n00b_focus_mgr_rebuild(fm);
    assert(fm->count == 1);
    assert(n00b_focus_mgr_current(fm) == f2);
    assert(n00b_focus_mgr_set(fm, f2));

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, root);

    n00b_plane_remove_child(root, nf);
    n00b_plane_remove_child(root, f2);
    n00b_widget_detach(f1);
    n00b_widget_detach(nf);
    n00b_widget_detach(f2);
    n00b_plane_destroy(f1);
    n00b_plane_destroy(nf);
    n00b_plane_destroy(f2);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] focus contract\n");
}

static void
test_table_hexdump_contract(void)
{
    n00b_table_t *table = n00b_new_kargs(n00b_table_t, table, .num_cols = 2);
    n00b_table_add_cell(table, make_str("Name"));
    n00b_table_add_cell(table, make_str("Value"));
    n00b_table_end_row(table);
    n00b_table_add_cell(table, make_str("Mode"));
    n00b_table_add_cell(table, make_str("baseline"));
    n00b_table_end_row(table);

    n00b_plane_t *table_plane = n00b_table_render(table, .width = 48);
    assert(table_plane != nullptr);
    assert(table_plane->width > 0);
    assert(table_plane->height > 0);

    const char    payload[] = "ABCD";
    n00b_buffer_t *input    = n00b_buffer_from_bytes((char *)payload,
                                                      (int64_t)(sizeof(payload) - 1));
    n00b_buffer_t *out      = n00b_hexdump_buf(input, .width = 80);
    assert(out != nullptr);
    assert(buffer_contains(out, "00000000"));
    assert(buffer_contains(out, "4142"));

    n00b_table_destroy(table);
    printf("  [PASS] table/hexdump contract\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display baseline contract tests...\n");

    test_canvas_and_composition_contract();
    test_event_normalization_contract();
    test_focus_contract();
    test_table_hexdump_contract();

    printf("All display baseline contract tests passed.\n");
    n00b_shutdown();
    return 0;
}
