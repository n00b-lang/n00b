#pragma once

#include <stdio.h>
#include "n00b.h"

typedef enum : uint8_t {
    N00B_DISPLAY_DIAG_OFF   = 0,
    N00B_DISPLAY_DIAG_ERROR = 1,
    N00B_DISPLAY_DIAG_INFO  = 2,
    N00B_DISPLAY_DIAG_TRACE = 3,
} n00b_display_diag_level_t;

extern void n00b_display_diag_init(void);
extern void n00b_display_diag_shutdown(void);
extern void n00b_display_diag_set_level(n00b_display_diag_level_t level);
extern void n00b_display_diag_set_stream(FILE *stream);
extern bool n00b_display_diag_would_log(n00b_display_diag_level_t level);
extern void n00b_display_diag_log(n00b_display_diag_level_t level,
                                   const char               *component,
                                   const char               *fmt, ...);
