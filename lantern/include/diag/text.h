#pragma once

#include <stdint.h>

#include "diag/record.h"
#include "lib/buffer.h"
#include "lib/string.h"

typedef struct {
    core_string_t one;
    core_string_t other;
} core_text_form_t;

typedef struct {
    const core_text_form_t *forms;
    uint32_t                count;
    uint32_t                list_separator;
    uint32_t                list_and;
    uint32_t                list_or;
    uint32_t                quoted;
    uint32_t                clause_separator;
} core_text_catalog_t;

#define CORE_TEXT_STRING(value) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_TEXT, .text = (value)})
#define CORE_TEXT_NUMBER(value) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_NUMBER, \
                       .number = (int64_t)(value)})
#define CORE_TEXT_ERRNO(value) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_ERRNO, .number = (value)})
#define CORE_TEXT_PHRASE(id) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_PHRASE, .phrase = (id)})
#define CORE_TEXT_ITEMS(arg_kind, values, total) \
    ((core_text_arg_t){.kind = (arg_kind), .items = (values), \
                       .count = (total)})
#define CORE_TEXT_NATIVE(value) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_NATIVE, .native = (value)})
#define CORE_TEXT_CHARACTER(value) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_CHARACTER, .number = (value)})
#define CORE_TEXT_CODEPOINT(value) \
    ((core_text_arg_t){.kind = CORE_TEXT_ARG_CODEPOINT, .number = (value)})
#define CORE_TEXT_ARGS(...) \
    ((core_text_args_t){(const core_text_arg_t[]){__VA_ARGS__}, \
                        sizeof((const core_text_arg_t[]){__VA_ARGS__}) \
                            / sizeof(core_text_arg_t)})
#define CORE_TEXT_NO_ARGS ((core_text_args_t){})
#define CORE_TEXT_MESSAGE(id, ...) \
    ((core_text_message_t){(id), CORE_TEXT_ARGS(__VA_ARGS__)})
#define CORE_TEXT_BARE(id) ((core_text_message_t){.text = (id)})

void core_text_expand(core_buffer_t *out, const core_text_catalog_t *catalog,
                      uint32_t id, core_text_args_t args);
core_string_t core_text_render(const core_text_catalog_t *catalog, uint32_t id,
                               core_text_args_t args);
core_string_t core_text_form(const core_text_catalog_t *catalog, uint32_t id);
