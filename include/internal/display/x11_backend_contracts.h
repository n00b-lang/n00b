#pragma once

#include <X11/X.h>

#include "n00b.h"
#include "display/event.h"

typedef struct {
    bool         has_pending_resize;
    n00b_isize_t pending_resize_rows;
    n00b_isize_t pending_resize_cols;
    bool         has_pending_repaint;
} n00b_x11_pending_state_t;

extern bool n00b_x11_translate_lookup_bytes(const char   *buf,
                                            int           nbytes,
                                            n00b_key_mod_t mods,
                                            n00b_event_t  *out);
extern void n00b_x11_translate_motion_state(unsigned int          state,
                                            n00b_mouse_button_t  *button,
                                            n00b_mouse_action_t  *action);
extern void n00b_x11_note_expose(n00b_x11_pending_state_t *state,
                                 int                       expose_count);
extern bool n00b_x11_take_pending_event(n00b_x11_pending_state_t *state,
                                        n00b_isize_t              current_rows,
                                        n00b_isize_t              current_cols,
                                        n00b_event_t             *out);
