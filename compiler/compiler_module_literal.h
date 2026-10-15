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

typedef bool (*n00bb_module_generator_fn)(void *context, core_translation_unit_t *unit, core_string_t name,
                                          n00bb_literal_origin_t origin, core_string_t text,
                                          core_buffer_t *source);

typedef struct {
    core_string_t             modifier;
    n00bb_module_generator_fn generate;
    void                     *context;
} n00bb_module_generator_t;

typedef struct {
    const n00bb_module_generator_t *items;
    size_t                          count;
} n00bb_module_generators_t;
