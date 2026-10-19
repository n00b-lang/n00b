#pragma once

#include <stddef.h>

#include "lib/string.h"
#include "runtime_native_roots.h"

bool n00bb_native_root_register(core_string_t qualified_name,
                                core_string_t c_alias);
size_t n00bb_native_root_registered_count(void);
const n00bb_runtime_native_root_t *n00bb_native_root_registered_at(size_t index);
const n00bb_runtime_native_root_t *n00bb_native_root_in_module(
    core_string_t module, size_t *cursor);
const n00bb_runtime_native_root_t *n00bb_native_root_named(
    core_string_t module, core_string_t function);
core_string_t n00bb_native_root_function(
    const n00bb_runtime_native_root_t *root, size_t module_length);
