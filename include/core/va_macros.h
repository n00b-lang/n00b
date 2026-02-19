/**
 * @file va_macros.h
 * @brief Recursive variadic argument counting and conversion macros.
 *
 * Provides:
 * - `N00B_VA_COUNT(...)` -- count variadic arguments
 * - `N00B_VA_VOID_STAR_CONVERT(...)` -- cast all args to `void *`
 * - `N00B_VA_TYPE_ENSURE(type, ...)` -- statically assert all args match type
 * - `N00B_MAP(macro, state, ...)` -- general-purpose variadic map
 *
 * These are used internally by the vargs infrastructure and by ncc's
 * typed vararg support.
 */
#pragma once

#define N00B_STR(arg)                #arg
#define N00B_STR_INDIRECT(arg)       N00B_STR(arg)
#define N00B_CONCAT(x, ...)          x##__VA_ARGS__
#define N00B_CONCAT_INDIRECT(x, ...) N00B_CONCAT(x, __VA_ARGS__)

// Recursive evaluation: two expansions per level, up to 256.
#define N00B_EVAL1(...)   __VA_ARGS__
#define N00B_EVAL2(...)   N00B_EVAL1(N00B_EVAL1(__VA_ARGS__))
#define N00B_EVAL4(...)   N00B_EVAL2(N00B_EVAL2(__VA_ARGS__))
#define N00B_EVAL8(...)   N00B_EVAL4(N00B_EVAL4(__VA_ARGS__))
#define N00B_EVAL16(...)  N00B_EVAL8(N00B_EVAL8(__VA_ARGS__))
#define N00B_EVAL32(...)  N00B_EVAL16(N00B_EVAL16(__VA_ARGS__))
#define N00B_EVAL64(...)  N00B_EVAL32(N00B_EVAL32(__VA_ARGS__))
#define N00B_EVAL128(...) N00B_EVAL64(N00B_EVAL64(__VA_ARGS__))
#define N00B_EVAL(...)    N00B_EVAL128(N00B_EVAL128(__VA_ARGS__))

#define N00B_EMPTY()
#define N00B_POSTPONE1(macro) macro N00B_EMPTY()

#define N00B_MAP(macro, state, ...) \
    __VA_OPT__(N00B_EVAL(_N00B_MAP_ONE(macro, state, __VA_ARGS__)))

#define _N00B_MAP_ONE(macro, state, x, ...)                    \
    macro(x, state)                                            \
        __VA_OPT__(N00B_POSTPONE1(_N00B_MAP_INDIRECT)()(macro, \
                                                        state, \
                                                        __VA_ARGS__))
#define _N00B_MAP_INDIRECT() _N00B_MAP_ONE

#define _N00B_COUNT_BODY(arg, state) +1

#define N00B_VA_COUNT(...)  (N00B_MAP(_N00B_COUNT_BODY, 0, __VA_ARGS__) + 0)
#define N00B_FIRST(...)     __VA_OPT__(_N00B_FIRST(__VA_ARGS__))
#define _N00B_FIRST(x, ...) x

#define N00B_REST(...)     __VA_OPT__(_N00B_REST(__VA_ARGS__))
#define _N00B_REST(x, ...) __VA_ARGS__

// ===========================================================================
// Static type selection via _Generic
// ===========================================================================

#define N00B_STATIC_IF(EXPR, TRUECASE, FALSECASE) \
    _Generic(&(char[1 + !!(EXPR)]){0},            \
        char(*)[2]: (TRUECASE),                    \
        char(*)[1]: (FALSECASE))

#define N00B_IS_POINTER(EXPR)     \
    _Generic(&(typeof((EXPR))){0}, \
        typeof(*(EXPR)) **: 1,     \
        default: 0)

#define N00B_IS_ARRAY_EXPR(EXPR) \
    _Generic(&(EXPR),            \
        typeof (*(EXPR))(*)[]: 1, \
        default: 0)

#define N00B_IS_FP_EXPR(EXPR)   \
    _Generic((EXPR),            \
        float: 1,               \
        double: 1,              \
        long double: 1,         \
        float _Complex: 1,      \
        double _Complex: 1,     \
        long double _Complex: 1, \
        default: 0)

#define N00B_IS_UNSIGNED_TYPE(TYPE) ((TYPE) - 1 > 0)
#define N00B_IS_SIGNED_TYPE(TYPE)   !N00B_IS_UNSIGNED_TYPE(TYPE)

#define N00B_IS_SIGNED_EXPR(EXPR)        \
    _Generic((EXPR),                     \
        char: N00B_IS_SIGNED_TYPE(char), \
        signed char: 1,                  \
        short: 1,                        \
        int: 1,                          \
        long: 1,                         \
        long long: 1,                    \
        default: 0)

#define N00B_IS_UNSIGNED_EXPR(EXPR)        \
    _Generic((EXPR),                       \
        _Bool: 1,                          \
        char: N00B_IS_UNSIGNED_TYPE(char), \
        unsigned char: 1,                  \
        unsigned short: 1,                 \
        unsigned int: 1,                   \
        unsigned long: 1,                  \
        unsigned long long: 1,             \
        default: 0)

// ===========================================================================
// Type-punning converters for packing scalars into void*
// ===========================================================================

typedef union n00b_fp_convert_t {
    double f;
    void  *v;
} n00b_fp_convert_t;

static inline void *
n00b_fp_to_void_ptr(double fp)
{
    return ((n00b_fp_convert_t){.f = fp}).v;
}

static inline void *
n00b_unsigned_to_void_ptr(unsigned long long u)
{
    return (void *)u;
}

static inline void *
n00b_signed_to_void_ptr(long long i)
{
    return (void *)i;
}

static inline void *
n00b_ptr_to_void_ptr(void *p)
{
    return p;
}

#define _N00B_GET_CONVERTER(EXPR)                                          \
    N00B_STATIC_IF(N00B_IS_SIGNED_EXPR(EXPR),                             \
                   n00b_signed_to_void_ptr,                               \
                   N00B_STATIC_IF(N00B_IS_UNSIGNED_EXPR(EXPR),            \
                                  n00b_unsigned_to_void_ptr,              \
                                  N00B_STATIC_IF(N00B_IS_FP_EXPR(EXPR),  \
                                                 n00b_fp_to_void_ptr,    \
                                                 n00b_ptr_to_void_ptr)))

#define _N00B_CONVERT_ONE(arg) \
    _N00B_GET_CONVERTER(arg)   \
    (arg)

#define N00B_VA_VOID_STAR_CONVERT(...)          \
    _N00B_CONVERT_ONE(N00B_FIRST(__VA_ARGS__)) \
    _N00B_CONVERT_LIST(N00B_REST(__VA_ARGS__))

#define _N00B_CONVERT_LIST(...) \
    N00B_MAP(_N00B_CONVERT_LATER_ARG, 0, __VA_ARGS__)

#define _N00B_CONVERT_LATER_ARG(arg, state) , _N00B_CONVERT_ONE(arg)

// ===========================================================================
// Static type checking for typed varargs
// ===========================================================================

#define N00B_OBJ_OF_TYPE(T, U)      \
    _Generic(*(typeof_unqual(T) *)0, \
        typeof(*(U *)0): 1,          \
        default: 0)

// clang-format off
#define N00B_VA_TYPE_ENSURE(type, ...)               \
    _N00B_ENSURE_ONE(N00B_FIRST(__VA_ARGS__), type) \
    _N00B_ENSURE_LIST(type, N00B_REST(__VA_ARGS__))

#define _N00B_ENSURE_LIST(type, ...) \
    N00B_MAP(_N00B_ENSURE_LATER_ARG, type, __VA_ARGS__)

#define _N00B_ENSURE_ONE(arg, typename)                               \
    ({static_assert(N00B_OBJ_OF_TYPE(arg, typename),                  \
                   " vargs item type for " N00B_STR(arg)              \
                   " != expected type: "                              \
                   N00B_STR(typename)); arg;})

#define _N00B_ENSURE_LATER_ARG(arg, type) , _N00B_ENSURE_ONE(arg, type)
// clang-format on
