// Random math stuff
#pragma once
#include <stdint.h>

extern uint64_t n00b_clz(uint64_t);

static inline uint64_t
n00b_int_log2(uint64_t n)
{
    return 63 - __builtin_clzll(n);
}

// General purpose round to a given divisor.

static inline uint64_t
n00b_round_up(uint64_t mod, uint64_t to_round)
{
    return (to_round + mod - 1) - (to_round % mod);
}

static inline double
n00b_to_pct(uint64_t numerator, uint64_t denominator)
{
    return 100.0 * ((double)numerator) / ((double)denominator);
}

static inline uint64_t
n00b_pow10_floor(uint64_t n)
{
    uint16_t result = 0;

    while (n) {
        if (!result) {
            result = 1;
        }
        else {
            result *= 10;
        }
        n /= 10;
    }

    return result;
}

#if __has_include(<stdbit.h>)
#include <stdbit.h>
static inline unsigned int
n00b_count_ones(uint64_t n)
{
    return stdc_count_ones_ull((unsigned long long)n);
}
#else
static inline unsigned int
n00b_count_ones(uint64_t n)
{
    return __builtin_popcountll((unsigned long long)n);
}
#endif
