#pragma once

#include <stdatomic.h> // IWYU pragma: export

#define n00b_atomic_load(x)     atomic_load_explicit(x, memory_order_acquire)
#define n00b_atomic_store(x, y) atomic_store_explicit(x, y, memory_order_release)
#define n00b_atomic_add(x, y)   atomic_fetch_add_explicit(x, y, memory_order_acq_rel)
#define n00b_atomic_cas(x, y, z)                                                               \
    atomic_compare_exchange_strong_explicit(x, y, z, memory_order_acq_rel, memory_order_acquire)
#define n00b_atomic_or(x, y)            atomic_fetch_or_explicit(x, y, memory_order_acq_rel)
#define n00b_atomic_xor(x, y)           atomic_fetch_xor_explicit(x, y, memory_order_acq_rel)
#define n00b_atomic_and(x, y)           atomic_fetch_and_explicit(x, y, memory_order_acq_rel)
#define n00b_atomic_read_then_set(x, y) atomic_exchange_explicit(x, y, memory_order_acq_rel)
#define n00b_atomic_fence()             atomic_thread_fence(memory_order_acq_rel)

#define n00b_cas(target, expected, desired)                                                    \
    atomic_compare_exchange_strong(target, expected, desired)
