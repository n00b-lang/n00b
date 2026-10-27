#include "native_frame_runtime.h"

#include <stddef.h>

typedef struct {
    uint32_t count;
    uint32_t root_count;
    n00b_native_frame_record frames[];
} native_frame_table_t;

extern const native_frame_table_t __n00bb_gc_native_frames;

_Static_assert(sizeof(n00b_native_frame_record) == 32,
               "native frame record width");
_Static_assert(sizeof(n00b_native_frame_root) == 8, "native frame root width");

uint32_t
n00b_builtin_abi_native_frame_count(void)
{
    return __n00bb_gc_native_frames.count;
}

const n00b_native_frame_record *
n00b_builtin_abi_native_frame(uint32_t index)
{
    return index < __n00bb_gc_native_frames.count
        ? &__n00bb_gc_native_frames.frames[index] : NULL;
}

const n00b_native_frame_root *
n00b_builtin_abi_native_frame_root(const n00b_native_frame_record *frame,
                                   uint32_t ordinal)
{
    const n00b_native_frame_record *first = __n00bb_gc_native_frames.frames;
    uintptr_t position = (uintptr_t)frame;
    uintptr_t base = (uintptr_t)first;
    size_t rows = (size_t)__n00bb_gc_native_frames.count;
    if (!frame || position < base || rows > SIZE_MAX / sizeof(*first)
        || position - base >= rows * sizeof(*first)
        || (position - base) % sizeof(*first) || ordinal >= frame->root_count
        || frame->first_root > __n00bb_gc_native_frames.root_count
        || ordinal >= __n00bb_gc_native_frames.root_count - frame->first_root)
        __builtin_trap();
    const n00b_native_frame_root *roots =
        (const n00b_native_frame_root *)(first + rows);
    return &roots[frame->first_root + ordinal];
}
