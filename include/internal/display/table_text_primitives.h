#pragma once

#include "n00b.h"
#include "adt/array.h"
#include "core/string.h"
#include "display/render/types.h"

typedef struct n00b_table_text_metrics_t {
    int32_t longest_line;
    int32_t longest_word;
} n00b_table_text_metrics_t;

extern n00b_table_text_metrics_t
n00b_table_text_measure(n00b_string_t *text);

extern n00b_array_t(n00b_string_t *)
n00b_table_text_lines_for_width(n00b_string_t *text,
                                 int32_t width,
                                 bool wrap);

extern n00b_string_t *
n00b_table_text_align_line(n00b_string_t *line,
                            int32_t width,
                            n00b_alignment_t alignment);
