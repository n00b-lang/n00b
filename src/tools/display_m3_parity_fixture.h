#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"

typedef struct {
    bool         gui_mode;
    int          resize_calls;
    n00b_isize_t resize_rows;
    n00b_isize_t resize_cols;
    int          left_key_events;
    int          right_key_events;
    int          left_activations;
    int          right_activations;
    int          left_mouse_presses;
    int          right_mouse_presses;
    int          poll_calls;
    int          render_calls;
    bool         cursor_hide;
    bool         cursor_show;
    size_t       events_consumed;
    size_t       events_total;
    uint32_t     next_key_after_stop;
} n00b_m3_parity_result_t;

extern int n00b_m3_parity_run_case(bool                       gui_mode,
                                   n00b_m3_parity_result_t  *out);
extern bool n00b_m3_parity_equivalent(const n00b_m3_parity_result_t *terminal,
                                      const n00b_m3_parity_result_t *gui);
