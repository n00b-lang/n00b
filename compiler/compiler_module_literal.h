#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/buffer.h"
#include "lib/module.h"

typedef struct {
    core_string_t host_path;
    uint32_t      line;
    uint32_t      column;
    uint64_t      byte;
} n00bb_literal_origin_t;

typedef struct {
    core_string_t name;
    core_string_t symbol;
    core_string_t signature;
    bool          pure;
} n00bb_native_declaration_t;

typedef struct {
    n00bb_native_declaration_t *items;
    size_t                      count;
} n00bb_native_declarations_t;

void n00bb_native_declarations_release(n00bb_native_declarations_t *declarations);

typedef struct {
    uint32_t      line;
    core_string_t path;
    uint32_t      origin_line;
    uint32_t      origin_column;
} n00bb_line_origin_t;

typedef struct {
    n00bb_line_origin_t *items;
    size_t               count;
} n00bb_line_origins_t;

void n00bb_line_origins_release(n00bb_line_origins_t *origins);

typedef bool (*n00bb_module_generator_fn)(void *context, core_translation_unit_t *unit, core_string_t name,
                                          core_string_t module, n00bb_literal_origin_t origin,
                                          core_string_t text, core_buffer_t *source,
                                          n00bb_native_declarations_t *natives,
                                          n00bb_line_origins_t *origins);

typedef struct {
    core_string_t             modifier;
    n00bb_module_generator_fn generate;
    void                     *context;
} n00bb_module_generator_t;

typedef struct {
    const n00bb_module_generator_t *items;
    size_t                          count;
} n00bb_module_generators_t;
