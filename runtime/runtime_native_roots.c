#include "runtime_native_roots.h"

static const uint32_t allocator_unmanaged_parameters[] = {0};

const n00bb_runtime_native_root_t n00bb_runtime_native_roots[] = {
    {"sys.internal.gc.alloc.allocate_runtime", "n00b_gc_allocate", nullptr, 0},
    {"sys.internal.gc.alloc.allocate_runtime_known_on_mutator", nullptr,
        allocator_unmanaged_parameters,
        sizeof(allocator_unmanaged_parameters)
            / sizeof(allocator_unmanaged_parameters[0])},
    {"sys.internal.gc.alloc.mutator_for_native", "n00b_gc_mutator", nullptr, 0},
    {"sys.internal.gc.alloc.native_root_depth", "n00b_gc_root_depth", nullptr, 0},
    {"sys.internal.gc.alloc.native_root_push", "n00b_gc_root_push", nullptr, 0},
    {"sys.internal.gc.alloc.native_root_pop", "n00b_gc_root_pop", nullptr, 0},
    {"sys.internal.gc.alloc.barrier_runtime", "n00b_gc_barrier_value", nullptr, 0},
    {"sys.internal.gc.alloc.barrier_pointer_runtime", nullptr, nullptr, 0},
    {"sys.internal.marshal.copy_literal", nullptr, nullptr, 0},
    {"sys.internal.abi.baked_address", "n00b_abi_baked_address", nullptr, 0},
    {"builtin.io.raw.__raw_repr_dispatch", nullptr, nullptr, 0},
    {"builtin.ffi.__ffi_enter", "n00b_ffi_enter", nullptr, 0},
    {"builtin.ffi.__ffi_leave", "n00b_ffi_leave", nullptr, 0},
    {"builtin.ffi.__ffi_panic_hook", "n00b_ffi_panic_hook", nullptr, 0},
};

const size_t n00bb_runtime_native_root_count =
    sizeof(n00bb_runtime_native_roots) / sizeof(n00bb_runtime_native_roots[0]);
