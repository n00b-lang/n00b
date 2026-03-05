#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/focus.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/button.h"
#include "display/widgets/label.h"
#include "internal/display/scene_contracts.h"

#define DEFAULT_OUT_PATH "plans/artifacts/display-rewrite/m1/scene_inspect.txt"
#define SCENE_ROWS 10
#define SCENE_COLS 52

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);

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
ensure_parent_dir(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);

    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);

    char *slash = strrchr(tmp, '/');
    if (!slash) {
        return 0;
    }
    if (slash == tmp) {
        return 0;
    }

    *slash = '\0';
    return ensure_dir_recursive(tmp);
}

static int
plane_depth(n00b_plane_t *plane)
{
    int depth = 0;
    for (n00b_plane_t *p = plane; p && p->parent; p = p->parent) {
        depth++;
    }
    return depth;
}

static int
write_scene_report(const char *path)
{
    int rc = -1;
    n00b_canvas_t *canvas = nullptr;
    n00b_plane_t  *root = nullptr;
    n00b_plane_t  *title = nullptr;
    n00b_plane_t  *status = nullptr;
    n00b_plane_t  *button = nullptr;
    n00b_focus_mgr_t *fm = nullptr;
    n00b_array_t(n00b_composite_entry_t) scene = {};
    FILE *fp = nullptr;

    canvas = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, SCENE_ROWS, SCENE_COLS);
    n00b_canvas_resize(canvas, SCENE_ROWS, SCENE_COLS);

    root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = SCENE_COLS;
    root->height = SCENE_ROWS;

    title = n00b_label_new(
        n00b_string_from_cstr("Display Baseline Scene"),
        .canvas = canvas,
        .width = 30,
        .height = 1);

    status = n00b_label_new(
        n00b_string_from_cstr("Status: baseline-ready"),
        .canvas = canvas,
        .width = 36,
        .height = 1);

    button = n00b_button_new(
        n00b_string_from_cstr("Execute"),
        .canvas = canvas,
        .width = 12,
        .height = 1);

    n00b_plane_add_child(root, title, 2, 1);
    n00b_plane_add_child(root, status, 2, 3);
    n00b_plane_add_child(root, button, 2, 5);
    n00b_canvas_add_plane(canvas, root);

    fm = n00b_focus_mgr_new(canvas);
    (void)n00b_focus_mgr_set(fm, button);

    n00b_display_scene_run_layout(canvas);
    n00b_display_scene_mark_all_dirty(canvas);
    n00b_display_scene_rerender_dirty(canvas);
    scene = n00b_display_scene_build(canvas);

    fp = fopen(path, "wb");
    if (!fp) {
        goto done;
    }

    fprintf(fp, "tool=display_scene_inspect\n");
    fprintf(fp, "entries=%zu\n", scene.len);

    for (size_t i = 0; i < scene.len; i++) {
        n00b_composite_entry_t *entry = &scene.data[i];
        n00b_plane_t *plane = entry->plane;
        const char *kind = "plane";

        if (plane && plane->widget_vtable && plane->widget_vtable->kind) {
            kind = plane->widget_vtable->kind;
        }

        fprintf(fp,
                "%03zu depth=%d z=%d abs=(%d,%d) clip=(%d,%d,%d,%d) "
                "plane=(x=%d,y=%d,w=%d,h=%d) kind=%s visible=%d dirty=%d gen=%u\n",
                i,
                plane_depth(plane),
                entry->abs_z,
                entry->abs_x,
                entry->abs_y,
                entry->clip_x,
                entry->clip_y,
                entry->clip_w,
                entry->clip_h,
                plane ? plane->x : 0,
                plane ? plane->y : 0,
                plane ? plane->width : 0,
                plane ? plane->height : 0,
                kind,
                plane ? ((plane->flags & N00B_PLANE_VISIBLE) != 0) : 0,
                plane ? ((plane->flags & N00B_PLANE_DIRTY) != 0) : 0,
                plane ? plane->render_gen : 0);
    }

    rc = 0;

done:
    if (fp) {
        fclose(fp);
    }
    n00b_display_scene_free(scene);
    if (fm) {
        n00b_focus_mgr_destroy(fm);
    }
    if (canvas && root) {
        n00b_canvas_remove_plane(canvas, root);
    }
    if (root && title) {
        n00b_plane_remove_child(root, title);
    }
    if (root && status) {
        n00b_plane_remove_child(root, status);
    }
    if (root && button) {
        n00b_plane_remove_child(root, button);
    }
    if (title) {
        n00b_widget_detach(title);
        n00b_plane_destroy(title);
    }
    if (status) {
        n00b_widget_detach(status);
        n00b_plane_destroy(status);
    }
    if (button) {
        n00b_widget_detach(button);
        n00b_plane_destroy(button);
    }
    if (root) {
        n00b_plane_destroy(root);
    }
    if (canvas) {
        n00b_canvas_destroy(canvas);
    }

    return rc;
}

int
main(int argc, char **argv)
{
    const char *out = DEFAULT_OUT_PATH;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for --out\n");
                return 1;
            }
            out = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--out PATH]\n", argv[0]);
            return 0;
        }

        fprintf(stderr, "unknown argument: %s\n", argv[i]);
        return 1;
    }

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    if (ensure_parent_dir(out) != 0) {
        fprintf(stderr, "failed to create output directory for %s\n", out);
        n00b_shutdown();
        return 1;
    }

    if (write_scene_report(out) != 0) {
        fprintf(stderr, "failed to write scene report to %s\n", out);
        n00b_shutdown();
        return 1;
    }

    printf("wrote %s\n", out);
    n00b_shutdown();
    return 0;
}
