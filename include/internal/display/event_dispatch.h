#pragma once

#include "n00b.h"
#include "display/event.h"
#include "display/focus.h"
#include "display/render/canvas.h"

typedef struct n00b_display_dispatch_result_t {
    bool handled;
    bool should_stop;
    bool focus_changed;
} n00b_display_dispatch_result_t;

extern n00b_display_dispatch_result_t
n00b_display_dispatch_event(n00b_canvas_t      *canvas,
                             n00b_focus_mgr_t   *fm,
                             const n00b_event_t *event);
