#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lib/option.h"
#include "lib/string.h"

core_option_decl(core_string_t);

uint32_t parse_source_new(core_option_t(core_string_t) name);

void parse_source_retain(uint32_t id);

void parse_source_release(uint32_t id);

bool parse_source_append(uint32_t id, core_string_t data);

bool parse_source_replace(uint32_t id, size_t start, size_t end,
                          core_string_t data);

size_t parse_source_length(uint32_t id);

bool parse_source_store_value(uint32_t id, size_t start, size_t end,
                              uint32_t *offset);

core_string_t parse_source_value(uint32_t id, uint32_t offset);

bool parse_source_slice(uint32_t id, size_t start, size_t end,
                        core_string_t *out);

core_option_t(core_string_t) parse_source_name(uint32_t id);

bool parse_source_position(uint32_t id, size_t offset, uint32_t *line,
                           uint32_t *column);
