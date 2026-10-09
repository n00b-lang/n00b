#include "native_frame_runtime.h"

#include <stddef.h>

typedef struct {
    uint32_t count;
    uint32_t root_count;
    n00b_native_frame_record frames[];
} native_frame_table_t;

extern const native_frame_table_t __n00bb_gc_native_frames;

_Static_assert(sizeof(n00b_native_frame_record) == 24,
               "native frame record width");

const n00b_native_frame_record *
n00b_builtin_abi_native_frame_at(const void *fp, const void *low,
                                 const void *high)
{
    uintptr_t address = (uintptr_t)fp;
    uintptr_t bottom = (uintptr_t)low;
    uintptr_t top = (uintptr_t)high;
    if (!address || address < bottom || address >= top) return NULL;
    for (uint32_t i = 0; i < __n00bb_gc_native_frames.count; i++) {
        const n00b_native_frame_record *frame =
            &__n00bb_gc_native_frames.frames[i];
        if (frame->sentinel_offset >= 0 || !frame->root_count
            || frame->first_root > __n00bb_gc_native_frames.root_count
            || frame->root_count
                > __n00bb_gc_native_frames.root_count - frame->first_root)
            continue;
        uintptr_t distance = (uintptr_t)-(int64_t)frame->sentinel_offset;
        if (address - bottom < distance || top - address < sizeof(void *))
            continue;
        const void *stored = NULL;
        __builtin_memcpy(&stored, (const void *)(address - distance),
                         sizeof(stored));
        if (stored == frame) return frame;
    }
    return NULL;
}

int32_t
n00b_builtin_abi_native_frame_root_offset(
    const n00b_native_frame_record *frame, uint32_t ordinal)
{
    const n00b_native_frame_record *first =
        __n00bb_gc_native_frames.frames;
    uintptr_t position = (uintptr_t)frame;
    uintptr_t base = (uintptr_t)first;
    size_t rows = (size_t)__n00bb_gc_native_frames.count;
    if (!frame || position < base || rows > SIZE_MAX / sizeof(*first)
        || position - base >= rows * sizeof(*first)
        || (position - base) % sizeof(*first) || ordinal >= frame->root_count
        || frame->first_root > __n00bb_gc_native_frames.root_count
        || ordinal >= __n00bb_gc_native_frames.root_count - frame->first_root)
        __builtin_trap();
    const int32_t *offsets = (const int32_t *)(first + rows);
    return offsets[frame->first_root + ordinal];
}
