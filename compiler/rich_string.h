#pragma once

#include "lib/buffer.h"

typedef enum {
    N00BB_RICH_STRING_INVALID,
    N00BB_RICH_STRING_BAD_ESCAPE,
    N00BB_RICH_STRING_UNCLOSED_ASCII_TAG,
    N00BB_RICH_STRING_MIXED_NUMBERING,
    N00BB_RICH_STRING_INDEX_OVERFLOW,
    N00BB_RICH_STRING_TOO_LARGE,
} n00bb_rich_string_error_t;

bool n00bb_rich_string_parse(core_string_t raw,
                             core_buffer_t **text,
                             core_buffer_t **metadata,
                             n00bb_rich_string_error_t *error);
