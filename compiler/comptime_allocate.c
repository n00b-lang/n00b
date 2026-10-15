#include "builtin_os.h"
#include "comptime_allocate.h"
#include "n00b-abi-v1.h"

#include <stdint.h>
#include <string.h>

static atomic_flag allocation_lock = ATOMIC_FLAG_INIT;
static n00bb_comptime_allocation_t *allocations;
static size_t allocation_count;
static size_t allocation_capacity;

static void
lock_allocations(void)
{
    while (atomic_flag_test_and_set_explicit(&allocation_lock,
                                              memory_order_acquire))
        n00b_os_cpu_relax();
}

static void
unlock_allocations(void)
{
    atomic_flag_clear_explicit(&allocation_lock, memory_order_release);
}

static bool
record_allocation(void *base, size_t size, uint32_t type_index,
                  uint32_t alignment)
{
    lock_allocations();
    if (allocation_count == allocation_capacity) {
        size_t next = allocation_capacity
            ? allocation_capacity * 2
            : n00b_os_page_size() / sizeof(*allocations);
        if (next <= allocation_capacity || next > SIZE_MAX / sizeof(*allocations)) {
            unlock_allocations();
            return false;
        }
        size_t bytes = next * sizeof(*allocations);
        n00bb_comptime_allocation_t *grown = n00b_os_map_zeroed(
            bytes, _Alignof(n00bb_comptime_allocation_t));
        if (!grown) {
            unlock_allocations();
            return false;
        }
        if (allocation_count)
            memcpy(grown, allocations,
                   allocation_count * sizeof(*allocations));
        n00bb_comptime_allocation_t *old = allocations;
        size_t old_bytes = allocation_capacity * sizeof(*allocations);
        allocations = grown;
        allocation_capacity = next;
        if (old) (void)n00b_os_unmap(old, old_bytes);
    }
    allocations[allocation_count++] = (n00bb_comptime_allocation_t){
        .base = base,
        .size = size,
        .type_index = type_index,
        .alignment = alignment,
    };
    unlock_allocations();
    return true;
}

bool
n00bb_comptime_allocation_containing(const void *address,
                                     n00bb_comptime_allocation_t *out)
{
    if (!address || !out) return false;
    uintptr_t wanted = (uintptr_t)address;
    lock_allocations();
    for (size_t i = allocation_count; i > 0; i--) {
        const n00bb_comptime_allocation_t *item = &allocations[i - 1];
        uintptr_t base = (uintptr_t)item->base;
        if (wanted < base || wanted - base >= item->size) continue;
        *out = *item;
        unlock_allocations();
        return true;
    }
    unlock_allocations();
    return false;
}

void *
n00b_comptime_allocate(uint32_t allocator, uint32_t type_index,
                        uint64_t byte_size, uint32_t alignment,
                        const void *instance_descriptor)
{
    if (allocator != N00B_ABI_ALLOCATOR_GC || !type_index
        || byte_size > SIZE_MAX) return nullptr;
    size_t descriptor_bytes = 0;
    if (instance_descriptor) {
        const n00b_abi_array_descriptor_header_v1 *descriptor =
            instance_descriptor;
        if (!descriptor->rank || !descriptor->flat_rank
            || descriptor->flat_rank > descriptor->rank
            || (descriptor->flags & ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK)
            || descriptor->reserved_zero
            || __builtin_mul_overflow((size_t)descriptor->rank,
                sizeof(uint64_t), &descriptor_bytes)
            || __builtin_add_overflow(descriptor_bytes,
                sizeof(*descriptor), &descriptor_bytes)
            || descriptor_bytes > byte_size) return nullptr;
    }
    size_t mapped_bytes = byte_size ? (size_t)byte_size : 1;
    uint8_t *storage = n00b_os_map_zeroed(mapped_bytes, alignment);
    if (!storage) return nullptr;
    const uint8_t *source = instance_descriptor;
    for (size_t i = 0; i < descriptor_bytes; i++) storage[i] = source[i];
    if (!record_allocation(storage, mapped_bytes, type_index, alignment)) {
        (void)n00b_os_unmap(storage, mapped_bytes);
        return nullptr;
    }
    return storage;
}

void *
n00b_allocate(uint32_t allocator, uint32_t type_index,
              uint64_t byte_size, uint32_t alignment,
              const void *instance_descriptor)
{
    void *result = n00b_comptime_allocate(allocator, type_index, byte_size,
                                           alignment, instance_descriptor);
    if (!result) __builtin_trap();
    return result;
}

void
n00b_gc_barrier_value(const void *address, uint32_t type_index)
{
    (void)address;
    (void)type_index;
}

void
n00b_gc_root_push(void **slot)
{
    (void)slot;
}

void
n00b_gc_root_pop(uint64_t depth)
{
    (void)depth;
}
