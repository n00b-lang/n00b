#include "runtime_native_roots.h"

const n00bb_runtime_native_root_t n00bb_runtime_native_roots[] = {
    {"builtin.gc.alloc.allocate_runtime", "n00b_gc_allocate"},
    {"builtin.gc.alloc.barrier_runtime", "n00b_gc_barrier_value"},
};

const size_t n00bb_runtime_native_root_count =
    sizeof(n00bb_runtime_native_roots) / sizeof(n00bb_runtime_native_roots[0]);
