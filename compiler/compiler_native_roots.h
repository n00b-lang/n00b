#pragma once

#include <stddef.h>

#include "runtime_native_roots.h"

const n00bb_runtime_native_root_t *n00bb_native_root_in_module(
    const char *module, size_t module_length, size_t *cursor);
const n00bb_runtime_native_root_t *n00bb_native_root_named(
    const char *module, size_t module_length, const char *function,
    size_t function_length);
const char *n00bb_native_root_function(const n00bb_runtime_native_root_t *root,
                                       size_t module_length);
