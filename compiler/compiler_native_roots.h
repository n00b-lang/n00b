#pragma once

#include <stddef.h>

#include "runtime_native_roots.h"

bool n00bb_native_root_register(const char *qualified_name,
                                const char *c_alias);
size_t n00bb_native_root_registered_count(void);
const n00bb_runtime_native_root_t *n00bb_native_root_registered_at(size_t index);
const n00bb_runtime_native_root_t *n00bb_native_root_in_module(
    const char *module, size_t module_length, size_t *cursor);
const n00bb_runtime_native_root_t *n00bb_native_root_named(
    const char *module, size_t module_length, const char *function,
    size_t function_length);
const char *n00bb_native_root_function(const n00bb_runtime_native_root_t *root,
                                       size_t module_length);
