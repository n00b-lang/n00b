#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lib/string_type.h"

typedef enum {
    CORE_TEXT_ARG_TEXT,
    CORE_TEXT_ARG_NUMBER,
    CORE_TEXT_ARG_ERRNO,
    CORE_TEXT_ARG_PHRASE,
    CORE_TEXT_ARG_NAMES,
    CORE_TEXT_ARG_CHOICES,
    CORE_TEXT_ARG_LIST,
    CORE_TEXT_ARG_ALTERNATIVES,
    CORE_TEXT_ARG_CLAUSES,
    CORE_TEXT_ARG_NATIVE,
    CORE_TEXT_ARG_CHARACTER,
    CORE_TEXT_ARG_CODEPOINT
} core_text_arg_kind_t;

typedef struct {
    core_text_arg_kind_t kind;
    core_string_t        text;
    const char          *native;
    int64_t              number;
    uint32_t             phrase;
    const core_string_t *items;
    size_t               count;
} core_text_arg_t;

typedef struct {
    const core_text_arg_t *at;
    size_t                 count;
} core_text_args_t;

typedef struct {
    uint32_t         text;
    core_text_args_t args;
} core_text_message_t;
