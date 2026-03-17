#pragma once

#include "n00b.h"

extern n00b_string_t *n00b_stream_backend_get_buffer(void *ctx);
extern size_t         n00b_stream_backend_get_length(void *ctx);
extern void           n00b_stream_backend_set_size(void *ctx,
                                                   n00b_isize_t rows,
                                                   n00b_isize_t cols);
