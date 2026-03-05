#pragma once

#include "n00b.h"
#include "adt/array.h"
#include "display/render/canvas.h"
#include "display/render/composite.h"

extern n00b_array_t(n00b_composite_entry_t)
n00b_display_scene_build(n00b_canvas_t *canvas);

extern void
n00b_display_scene_free(n00b_array_t(n00b_composite_entry_t) scene);

extern bool n00b_display_scene_any_dirty(n00b_canvas_t *canvas);
extern void n00b_display_scene_mark_all_dirty(n00b_canvas_t *canvas);
extern void n00b_display_scene_rerender_dirty(n00b_canvas_t *canvas);
extern void n00b_display_scene_run_layout(n00b_canvas_t *canvas);
