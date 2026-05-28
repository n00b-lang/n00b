#pragma once

#include <stdbool.h>

#include "n00b.h"

enum {
    N00B_DISPLAY_M4_SHOWCASE_ROWS = 18,
    N00B_DISPLAY_M4_SHOWCASE_COLS = 72,
};

typedef struct {
    n00b_string_t *showcase_stream;
    bool           enter_handled;
    bool           checkbox_click_handled;
    bool           status_is_run;
    bool           checkbox_checked;
    int            button_clicks;
} n00b_display_m4_showcase_summary_t;

extern int n00b_display_m4_showcase_run(n00b_display_m4_showcase_summary_t *out);
