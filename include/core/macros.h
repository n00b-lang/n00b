/**
 * @file macros.h
 * @brief Core metaprogramming macros.
 *
 * Provides token-pasting, deferred expansion, recursive MAP macros,
 * counted iteration (MAP_COUNT), and variadic argument helpers used by
 * the type-safe generic data structures.
 */
#pragma once

#define N00B_TO_STRING_BASE(x) #x
#define N00B_TO_STRING(x)      N00B_TO_STRING_BASE(x)
#define N00B_LOC_STRING()      (__FILE__ ":" N00B_TO_STRING(__LINE__))
#define N00B_EMPTY()
#define N00B_DEFER(macro) macro N00B_EMPTY()

#define N00B_EVAL(...)     N00B_EVAL1024(__VA_ARGS__)
#define N00B_EVAL1024(...) N00B_EVAL512(N00B_EVAL512(__VA_ARGS__))
#define N00B_EVAL512(...)  N00B_EVAL256(N00B_EVAL256(__VA_ARGS__))
#define N00B_EVAL256(...)  N00B_EVAL128(N00B_EVAL128(__VA_ARGS__))
#define N00B_EVAL128(...)  N00B_EVAL64(N00B_EVAL64(__VA_ARGS__))
#define N00B_EVAL64(...)   N00B_EVAL32(N00B_EVAL32(__VA_ARGS__))
#define N00B_EVAL32(...)   N00B_EVAL16(N00B_EVAL16(__VA_ARGS__))
#define N00B_EVAL16(...)   N00B_EVAL8(N00B_EVAL8(__VA_ARGS__))
#define N00B_EVAL8(...)    N00B_EVAL4(N00B_EVAL4(__VA_ARGS__))
#define N00B_EVAL4(...)    N00B_EVAL2(N00B_EVAL2(__VA_ARGS__))
#define N00B_EVAL2(...)    N00B_EVAL1(N00B_EVAL1(__VA_ARGS__))
#define N00B_EVAL1(...)    __VA_ARGS__

#define N00B_CONCAT_BASE(x, ...) x##__VA_ARGS__
#define N00B_CONCAT(x, ...)      N00B_CONCAT_BASE(x, __VA_ARGS__)

#define N00B_OPT_ARGS_0()    nullptr
#define N00B_OPT_ARGS_()     nullptr
#define N00B_OPT_ARGS_1(...) __VA_ARGS__
// Either we got a keywords object passed, or we want to pass null.
#define n00b_opt_args(...)                                                                     \
    N00B_CONCAT(N00B_OPT_ARGS_, N00B_MACRO_HAS_ARGS(__VA_ARGS__))(__VA_ARGS__)

#define N00B_MAP(macro, ...) __VA_OPT__(N00B_EVAL(N00B_MAP_ONE(macro, __VA_ARGS__)))

#define N00B_MAP_ONE(macro, x, ...)                                                            \
    macro(x) __VA_OPT__(N00B_DEFER(_N00B_MAP_ONE)()(macro, __VA_ARGS__))

#define _N00B_MAP_ONE() N00B_MAP_ONE

// Version of MAP that allows you to pass an additional 'state' field.

#define N00B_MAP_COUNT(macro, count, ...)                                                      \
    __VA_OPT__(N00B_EVAL(N00B_MAP_COUNT_ONE(macro, (count), __VA_ARGS__)))

#define N00B_MAP_COUNT_ONE(macro, count, x, ...)                                               \
    macro(x, count)                                                                            \
        __VA_OPT__(N00B_DEFER(_N00B_MAP_COUNT_ONE)()(macro, ((count) + 1), __VA_ARGS__))

#define _N00B_MAP_COUNT_ONE() N00B_MAP_COUNT_ONE

#define N00B_VA_COUNT(...) (N00B_MAP(N00B_COUNT_BODY, __VA_ARGS__) 0)

#define N00B_COUNT_BODY(x) 1 +

#define N00B_FIRST(...)     __VA_OPT__(_N00B_FIRST(__VA_ARGS__))
#define _N00B_FIRST(x, ...) x

#define N00B_REST(...)     __VA_OPT__(_N00B_REST(__VA_ARGS__))
#define _N00B_REST(x, ...) __VA_ARGS__

// clang-format off
#define N00B_IS_POINTER(EXPR)      \
    _Generic(&(typeof((EXPR))){0}, \
        typeof(*(EXPR)) **: 1,     \
        default: 0)

#define N00B_PTR_CASE(EXPR, ptr_dispatch, concrete_dispatch) \
    _Generic(&(typeof((EXPR))){0},                                   \
        typeof(*(EXPR)) **: ptr_dispatch,                            \
	     default: (typeof(EXPR))concrete_dispatch)

// clang-format on
