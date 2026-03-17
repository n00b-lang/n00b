#pragma once

#include <stdbool.h>

#include "n00b.h"

typedef struct {
    int          resize_calls;
    n00b_isize_t resize_rows;
    n00b_isize_t resize_cols;
    int          left_key_events;
    int          right_key_events;
    int          right_mouse_presses;
    int          right_activations;
    bool         cursor_hide;
    bool         cursor_show;
    int          poll_calls;
    int          render_calls;
} n00b_display_terminal_replay_summary_t;

extern int n00b_display_terminal_replay_run(
    n00b_display_terminal_replay_summary_t *summary);

extern int n00b_display_terminal_replay_write_artifacts(
    const char                                  *out_dir,
    const n00b_display_terminal_replay_summary_t *summary);
