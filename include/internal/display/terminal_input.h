#pragma once

#include "n00b.h"
#include "display/event.h"

typedef struct n00b_terminal_input_state_t {
    bool mouse_button_down;
} n00b_terminal_input_state_t;

typedef int (*n00b_terminal_read_byte_fn)(void *ctx, int32_t timeout_ms);

typedef struct n00b_terminal_ncinput_view_t {
    uint32_t id;
    uint32_t evtype;
    int32_t  x;
    int32_t  y;
    bool     shift;
    bool     ctrl;
    bool     alt;
    uint32_t eff_text0;
} n00b_terminal_ncinput_view_t;

extern void n00b_terminal_input_reset(n00b_terminal_input_state_t *state);

extern bool n00b_terminal_parse_ansi_event(n00b_terminal_input_state_t *state,
                                            n00b_terminal_read_byte_fn   read_byte,
                                            void                        *io_ctx,
                                            int32_t                      timeout_ms,
                                            n00b_event_t                *out);

extern uint32_t n00b_terminal_map_key(uint32_t raw_key);

extern bool n00b_terminal_translate_notcurses(const n00b_terminal_ncinput_view_t *in,
                                               n00b_terminal_input_state_t         *state,
                                               n00b_isize_t                         cell_px_w,
                                               n00b_isize_t                         cell_px_h,
                                               n00b_event_t                        *out);
