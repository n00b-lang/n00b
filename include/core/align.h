#pragma once

#include <stdint.h>

#if !defined(N00B_ALIGN)
#define N00B_ALIGN 16
#endif

extern size_t n00b_page_size;

static inline void *
n00b_align_to_page_start(void *addr)
{
    uint64_t power   = n00b_page_size;
    uint64_t modulus = power - 1;

    return (char *)(((uint64_t)addr) & ~modulus);
}

#if __has_include(<stdbit.h>)
#include <stdbit.h>
#else
#define stdc_bit_ceil_ull(x)  (1ULL << (64 - __builtin_clzll(x)))
#define stdc_bit_floor_ull(x) (1ULL << (63 - __builtin_clzll(x)))
#endif

#define n00b_align_closest_pow2_floor(value) stdc_bit_floor_ull((unsigned long long int)(value))
#define n00b_align_closest_pow2_ceil(value)  stdc_bit_ceil_ull((unsigned long long int)value)

static inline uint64_t
n00b_align_floor(uint64_t n, uint64_t base)
{
    uint64_t mask = base - 1;

    assert(!(base & mask));
    return n & ~mask;
}

static inline uint64_t
n00b_align_ceil(uint64_t n, uint64_t base)
{
    uint64_t mask = base - 1;

    assert(!(base & mask));

    return (n + mask) & ~mask;
}

static inline uint64_t
n00b_page_align(uint64_t n)
{
    return n00b_align_ceil(n, n00b_page_size);
}

static inline uint64_t
n00b_align(uint64_t n)
{
    return n00b_align_ceil(n, N00B_ALIGN);
}
