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

#define N00B_INC_0  1
#define N00B_INC_1  2
#define N00B_INC_2  3
#define N00B_INC_3  4
#define N00B_INC_4  5
#define N00B_INC_5  6
#define N00B_INC_6  7
#define N00B_INC_7  8
#define N00B_INC_8  9
#define N00B_INC_9  10
#define N00B_INC_10 11
#define N00B_INC_11 12
#define N00B_INC_12 13
#define N00B_INC_13 14
#define N00B_INC_14 15
#define N00B_INC_15 16
#define N00B_INC_16 17
#define N00B_INC_17 18
#define N00B_INC_18 19
#define N00B_INC_19 20
#define N00B_INC_20 21
#define N00B_INC_21 22
#define N00B_INC_22 23
#define N00B_INC_23 24
#define N00B_INC_24 25
#define N00B_INC_25 26
#define N00B_INC_26 27
#define N00B_INC_27 28
#define N00B_INC_28 29
#define N00B_INC_29 30
#define N00B_INC_30 31
#define N00B_INC_31 32
#define N00B_INC_32 33
#define N00B_INC_33 34
#define N00B_INC_34 35
#define N00B_INC_35 36
#define N00B_INC_36 37
#define N00B_INC_37 38
#define N00B_INC_38 39
#define N00B_INC_39 40
#define N00B_INC_40 41
#define N00B_INC_41 42
#define N00B_INC_42 43
#define N00B_INC_43 44
#define N00B_INC_44 45
#define N00B_INC_45 46
#define N00B_INC_46 47
#define N00B_INC_47 48
#define N00B_INC_48 49
#define N00B_INC_49 50
#define N00B_INC_50 51
#define N00B_INC_51 52
#define N00B_INC_52 53
#define N00B_INC_53 54
#define N00B_INC_54 55
#define N00B_INC_55 56
#define N00B_INC_56 57
#define N00B_INC_57 58
#define N00B_INC_58 59
#define N00B_INC_59 60
#define N00B_INC_60 61
#define N00B_INC_61 62
#define N00B_INC_62 63
#define N00B_INC_63 64
#define N00B_INC_64 65

#define N00B_INC(x) N00B_CONCAT(N00B_INC_, x)

#define N00B_MAP_COUNT(macro, count, ...)                                                      \
    __VA_OPT__(N00B_EVAL(N00B_MAP_COUNT_ONE(macro, count, __VA_ARGS__)))

#define N00B_MAP_COUNT_ONE(macro, count, x, ...)                                               \
    macro(x, count)                                                                            \
        __VA_OPT__(N00B_DEFER(_N00B_MAP_COUNT_ONE)()(macro, N00B_INC(count), __VA_ARGS__))

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
