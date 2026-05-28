#pragma once

#include "n00b.h"
#include "display/event.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"

extern n00b_render_size_t n00b_display_backend_get_size(n00b_canvas_t *canvas);
extern n00b_render_cap_t  n00b_display_backend_caps(n00b_canvas_t *canvas);
extern bool               n00b_display_backend_poll_event(n00b_canvas_t *canvas,
                                                           int32_t        timeout_ms,
                                                           n00b_event_t  *out);
extern bool               n00b_display_backend_copy_text(n00b_canvas_t *canvas,
                                                          const char    *utf8,
                                                          size_t         len);
extern void               n00b_display_backend_set_cursor_visible(n00b_canvas_t *canvas,
                                                                   bool           visible);
