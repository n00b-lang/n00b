#ifndef N00BB_RUNTIME_NATIVE_ROOTS_H
#define N00BB_RUNTIME_NATIVE_ROOTS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *qualified_name;
    const char *c_alias;
    const uint32_t *unmanaged_parameters;
    size_t unmanaged_parameter_count;
} n00bb_runtime_native_root_t;

extern const n00bb_runtime_native_root_t n00bb_runtime_native_roots[];
extern const size_t n00bb_runtime_native_root_count;

#endif
