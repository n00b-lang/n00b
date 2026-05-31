#pragma once

#include <stdbool.h>

#include "n00b.h"
#include "display/focus.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"

typedef struct {
    n00b_canvas_t    *canvas;
    n00b_plane_t     *root;
    n00b_plane_t     *title;
    n00b_plane_t     *status;
    n00b_plane_t     *button;
    n00b_focus_mgr_t *focus_mgr;
} n00b_display_demo_scene_t;

extern bool n00b_display_demo_scene_init(n00b_display_demo_scene_t *scene,
                                         n00b_isize_t               rows,
                                         n00b_isize_t               cols);
extern void n00b_display_demo_scene_destroy(n00b_display_demo_scene_t *scene);
