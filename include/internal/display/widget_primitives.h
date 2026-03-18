#pragma once

#include "n00b.h"
#include "display/event.h"
#include "display/render/plane.h"
#include "display/widget.h"

extern int32_t n00b_widget_cell_px_width(n00b_plane_t *plane);
extern int32_t n00b_widget_line_px_height(n00b_plane_t *plane);
extern bool n00b_widget_state_is_focused_or_active(const n00b_plane_t *plane);
extern bool n00b_widget_event_is_left_press(const n00b_event_t *event);
extern bool n00b_widget_event_is_keyboard_activate(const n00b_event_t *event);
extern void *n00b_widget_data_if_kind(n00b_plane_t *plane,
                                       const n00b_widget_vtable_t *expected);
