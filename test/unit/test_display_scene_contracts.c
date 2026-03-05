#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "internal/display/scene_contracts.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);

static void
dummy_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
    n00b_plane_draw_glyph(plane, 0, 0, 'X');
}

static const n00b_widget_vtable_t dummy_widget = {
    .kind   = "scene_dummy",
    .render = dummy_render,
};

static void
test_scene_contracts(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, 8, 20);
    n00b_canvas_resize(canvas, 8, 20);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    root->width  = 20;
    root->height = 8;
    n00b_widget_attach(root, &dummy_widget, nullptr);

    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    child->width  = 6;
    child->height = 1;
    n00b_widget_attach(child, &dummy_widget, nullptr);

    n00b_plane_add_child(root, child, 2, 2);
    n00b_canvas_add_plane(canvas, root);

    assert(n00b_display_scene_any_dirty(canvas));
    n00b_display_scene_rerender_dirty(canvas);
    n00b_canvas_render(canvas);
    assert(!n00b_display_scene_any_dirty(canvas));

    n00b_display_scene_mark_all_dirty(canvas);
    assert(n00b_display_scene_any_dirty(canvas));

    n00b_display_scene_run_layout(canvas);
    n00b_display_scene_rerender_dirty(canvas);

    n00b_array_t(n00b_composite_entry_t) scene = n00b_display_scene_build(canvas);
    assert(scene.data != nullptr);
    assert(scene.len >= 2);
    assert(scene.data[0].abs_z <= scene.data[scene.len - 1].abs_z);
    n00b_display_scene_free(scene);

    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, child);
    n00b_widget_detach(child);
    n00b_widget_detach(root);
    n00b_plane_destroy(child);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] scene contracts build/layout/dirty\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display scene-contract tests...\n");
    test_scene_contracts();

    printf("Display scene-contract tests passed.\n");
    n00b_shutdown();
    return 0;
}
