/**
 * @file macros.h
 * @brief Utility macros for type traits, generic programming, and variadic helpers.
 *
 * Includes:
 * - Stringification and concatenation macros
 * - Static if (compile-time conditional)
 * - Type trait macros (is_pointer, is_signed, is_array, etc.)
 * - Generic value-to-void* conversion
 * - Variadic argument helpers (map, count, etc.)
 *
 * Type traits adapted from https://dev.to/pauljlucas/generic-in-c-i48
 */
#pragma once

#include <stdint.h>

/** @name Stringification Macros
 * @{
 */
#define NCC_TO_STRING_BASE(x) #x
#define NCC_TO_STRING(x)      NCC_TO_STRING_BASE(x)
#define NCC_TO_STRING2(x)     NCC_TO_STRING(x)
#define NCC_TO_STRING3(x)     NCC_TO_STRING(x)
/** @} */

/** @name Utility Macros
 * @{
 */
/** @brief Create "file:line" location string */
#define NCC_LOC_STRING() (__FILE__ ":" NCC_TO_STRING(__LINE__))

#define NCC_EMPTY()
#define NCC_DEFER(macro) macro NCC_EMPTY()
/** @} */

/** @name Recursive Evaluation Macros
 * @brief Allow up to 1024 levels of macro expansion
 * @{
 */
#define NCC_EVAL(...)     NCC_EVAL1024(__VA_ARGS__)
#define NCC_EVAL1024(...) NCC_EVAL512(NCC_EVAL512(__VA_ARGS__))
#define NCC_EVAL512(...)  NCC_EVAL256(NCC_EVAL256(__VA_ARGS__))
#define NCC_EVAL256(...)  NCC_EVAL128(NCC_EVAL128(__VA_ARGS__))
#define NCC_EVAL128(...)  NCC_EVAL64(NCC_EVAL64(__VA_ARGS__))
#define NCC_EVAL64(...)   NCC_EVAL32(NCC_EVAL32(__VA_ARGS__))
#define NCC_EVAL32(...)   NCC_EVAL16(NCC_EVAL16(__VA_ARGS__))
#define NCC_EVAL16(...)   NCC_EVAL8(NCC_EVAL8(__VA_ARGS__))
#define NCC_EVAL8(...)    NCC_EVAL4(NCC_EVAL4(__VA_ARGS__))
#define NCC_EVAL4(...)    NCC_EVAL2(NCC_EVAL2(__VA_ARGS__))
#define NCC_EVAL2(...)    NCC_EVAL1(NCC_EVAL1(__VA_ARGS__))
#define NCC_EVAL1(...)    __VA_ARGS__
/** @} */

/** @name Concatenation Macros
 * @{
 */
#define NCC_CONCAT_BASE(x, ...) x##__VA_ARGS__
#define NCC_CONCAT(x, ...)      NCC_CONCAT_BASE(x, __VA_ARGS__)
#define NCC_CONCAT2(x, ...)     NCC_CONCAT(x, __VA_ARGS__)
#define NCC_CONCAT3(a, b, c)    NCC_CONCAT(NCC_CONCAT(a, b), c)
/** @} */

/**
 * @brief Namespace macro - prefixes identifier with DEFAULT_LIB_PREFIX.
 * @param x Identifier to prefix
 */
#define NS(x) NCC_CONCAT(DEFAULT_LIB_PREFIX, x)

/**
 * @brief Union for converting between double and void*.
 */
typedef union {
    double d; /**< Double value */
    void  *v; /**< Pointer value */
} ncc_double_ctx;

/** @name Type Trait Macros
 * @{
 */

/**
 * @brief Compile-time conditional selection.
 * @param EXPR Constant expression (evaluated at compile time)
 * @param TRUECASE Value if EXPR is true
 * @param FALSECASE Value if EXPR is false
 */
#define NCC_STATIC_IF(EXPR, TRUECASE, FALSECASE) \
    _Generic(&(char[1 + !!(EXPR)]){0},           \
        char (*)[2]: (TRUECASE),                 \
        char (*)[1]: (FALSECASE))

/** @brief Check if expression is a pointer type */
#define NCC_IS_POINTER(EXPR)       \
    _Generic(&(typeof((EXPR))){0}, \
        typeof(*(EXPR)) **: 1,     \
        default: 0)

/** @brief Check if expression is an array type */
#define NCC_IS_ARRAY_EXPR(EXPR)   \
    _Generic(&(EXPR),             \
        typeof (*(EXPR))(*)[]: 1, \
        default: 0)

/** @brief Check if expression is a floating-point type */
#define NCC_IS_FP_EXPR(EXPR)     \
    _Generic((EXPR),             \
        float: 1,                \
        double: 1,               \
        long double: 1,          \
        float _Complex: 1,       \
        double _Complex: 1,      \
        long double _Complex: 1, \
        default: 0)

/** @brief Check if type is unsigned */
#define NCC_IS_UNSIGNED_TYPE(TYPE) ((TYPE) - 1 > 0)

/** @brief Check if type is signed */
#define NCC_IS_SIGNED_TYPE(TYPE) !NCC_IS_UNSIGNED_TYPE(TYPE)

/** @brief Check if expression is a signed integer type */
#define NCC_IS_SIGNED_EXPR(EXPR)        \
    _Generic((EXPR),                    \
        char: NCC_IS_SIGNED_TYPE(char), \
        signed char: 1,                 \
        short: 1,                       \
        int: 1,                         \
        long: 1,                        \
        long long: 1,                   \
        default: 0)

/** @brief Check if expression is an unsigned integer type */
#define NCC_IS_UNSIGNED_EXPR(EXPR)        \
    _Generic((EXPR),                      \
        _Bool: 1,                         \
        char: NCC_IS_UNSIGNED_TYPE(char), \
        unsigned char: 1,                 \
        unsigned short: 1,                \
        unsigned int: 1,                  \
        unsigned long: 1,                 \
        unsigned long long: 1,            \
        default: 0)

/** @brief Check if expression is any integer type */
#define NCC_IS_INTEGER_EXPR(EXPR) \
    (NCC_IS_SIGNED_EXPR(EXPR) || NCC_IS_UNSIGNED_EXPR(EXPR))

/** @brief Check if expression can be cast to pointer */
#define NCC_CAN_CAST_TO_PTR(EXPR) \
    (NCC_IS_POINTER(EXPR) || NCC_IS_ARRAY_EXPR(EXPR))

/** @brief Check if two types are the same */
#define NCC_IS_SAME_TYPE(T, U) \
    _Generic(*(T *)0,          \
        typeof_unqual(U): 1,   \
        default: 0)

/** @} */

/** @name Type Conversion Functions
 * @{
 */

/**
 * @brief Convert floating-point to void*.
 * @param fp Double value
 * @return void* with same bit pattern
 */
static inline void *
ncc_fp_to_void_ptr(double fp)
{
    return (ncc_double_ctx){
        .d = fp,
    }
        .v;
}

/**
 * @brief Convert void* to floating-point.
 * @param p Pointer value
 * @return double with same bit pattern
 */
static inline double
ncc_void_ptr_to_fp(void *p)
{
    return (ncc_double_ctx){
        .v = p,
    }
        .d;
}

/**
 * @brief Convert unsigned integer to void*.
 * @param u Unsigned 64-bit value
 * @return Pointer with same value
 */
static inline void *
ncc_unsigned_to_void_ptr(uint64_t u)
{
    return (void *)u;
}

/**
 * @brief Convert signed integer to void*.
 * @param i Signed 64-bit value
 * @return Pointer with same value
 */
static inline void *
ncc_signed_to_void_ptr(int64_t i)
{
    return (void *)i;
}

/**
 * @brief Pass pointer through unchanged.
 * @param p Pointer value
 * @return Same pointer
 */
static inline void *
ncc_ptr_to_void_ptr(void *p)
{
    return p;
}

/** @} */

/** @name Generic Conversion Macros
 * @{
 */

/**
 * @brief Select appropriate converter function for an expression.
 */
#define NCC_GET_CONVERTER(EXPR)                                     \
    NCC_STATIC_IF(NCC_IS_SIGNED_EXPR(EXPR),                         \
                  ncc_signed_to_void_ptr,                           \
                  NCC_STATIC_IF(NCC_IS_UNSIGNED_EXPR(EXPR),         \
                                ncc_unsigned_to_void_ptr,           \
                                NCC_STATIC_IF(NCC_IS_FP_EXPR(EXPR), \
                                              ncc_fp_to_void_ptr,   \
                                              ncc_ptr_to_void_ptr)))

/** @brief Error for unconvertible types */
#define NCC_CONV_ERR() \
    static_assert(0, "Value cannot be converted to (void *)")

/**
 * @brief Convert any 64-bit compatible value to void*.
 */
#define ncc_generic_cast(EXPR) \
    NCC_GET_CONVERTER(EXPR)    \
    (EXPR)

/** @} */

/** @name Variadic Argument Helpers
 * @{
 */

/** @brief Select first argument */
#define NCC_SELECT_ARG_1(x, ...) x

/** @brief Check if variadic list has arguments */
#define NCC_MACRO_HAS_ARGS(...) NCC_SELECT_ARG_1(__VA_OPT__(1, ) 0)

#define NCC_BOOL_()     0
#define NCC_BOOL_0()    0
#define NCC_BOOL_1(...) NCC_STATIC_IF((__VA_ARGS__), 1, 0)

/** @brief Convert expression to boolean */
#define NCC_BOOL(...)                           \
    NCC_CONCAT(NCC_BOOL_,                       \
               NCC_MACRO_HAS_ARGS(__VA_ARGS__)) \
    (__VA_ARGS__)

#define NCC_OPT_ARGS_0()    nullptr
#define NCC_OPT_ARGS_()     nullptr
#define NCC_OPT_ARGS_1(...) __VA_ARGS__

/** @brief Return arguments or nullptr if empty */
#define ncc_opt_args(...) \
    NCC_CONCAT(NCC_OPT_ARGS_, NCC_MACRO_HAS_ARGS(__VA_ARGS__))(__VA_ARGS__)

/** @brief Map a macro over variadic arguments */
#define NCC_MAP(macro, ...) \
    __VA_OPT__(NCC_EVAL(NCC_MAP_ONE(macro, __VA_ARGS__)))

#define NCC_MAP_ONE(macro, x, ...) \
    macro(x) __VA_OPT__(NCC_DEFER(_NCC_MAP_ONE)()(macro, __VA_ARGS__))

#define _NCC_MAP_ONE() NCC_MAP_ONE

/** @brief Map with additional state argument */
#define NCC_MAP_STATE(macro, state, ...) \
    __VA_OPT__(NCC_EVAL(NCC_MAP_STATE_ONE(macro, state, __VA_ARGS__)))

#define NCC_MAP_STATE_ONE(macro, state, x, ...) \
    macro(state, x)                             \
        __VA_OPT__(NCC_DEFER(_NCC_MAP_STATE_ONE)()(macro, state, __VA_ARGS__))

#define _NCC_MAP_STATE_ONE() NCC_MAP_STATE_ONE

/** @brief Count variadic arguments */
#define NCC_VA_COUNT(...) \
    (NCC_MAP(NCC_COUNT_BODY, __VA_ARGS__) 0)

#define NCC_COUNT_BODY(x) 1 +

/** @brief Get first variadic argument */
#define NCC_FIRST(...)     __VA_OPT__(_NCC_FIRST(__VA_ARGS__))
#define _NCC_FIRST(x, ...) x

/** @brief Get all but first variadic argument */
#define NCC_REST(...)     __VA_OPT__(_NCC_REST(__VA_ARGS__))
#define _NCC_REST(x, ...) __VA_ARGS__

#define _NCC_CONVERT_ONE(arg) \
    _NCC_GET_CONVERTER(arg)   \
    (arg)

/** @brief Convert variadic args to void* array */
#define NCC_VA_VOID_STAR_CONVERT(...)        \
    _NCC_CONVERT_ONE(NCC_FIRST(__VA_ARGS__)) \
    _NCC_CONVERT_LIST(NCC_REST(__VA_ARGS__))

#define _NCC_CONVERT_LIST(...) \
    NCC_MAP(_NCC_CONVERT_LATER_ARG, __VA_ARGS__)

#define _NCC_CONVERT_LATER_ARG(arg, state) , _NCC_CONVERT_ONE(arg)

/** @brief Ensure all arguments are of specified type */
#define NCC_VA_TYPE_ENSURE(type, ...)             \
    _NCC_ENSURE_ONE(NCC_FIRST(__VA_ARGS__), type) \
    _NCC_ENSURE_LIST(type, NCC_REST(__VA_ARGS__))

#define _NCC_ENSURE_LIST(type, ...) \
    NCC_MAP_STATE(_NCC_ENSURE_LATER_ARG, type, __VA_ARGS__)

/** @brief Check if object is of type */
#define NCC_OBJ_OF_TYPE(T, U)        \
    _Generic(*(typeof_unqual(T) *)0, \
        typeof(*(U *)0): 1,          \
        default: 0)

// clang-format off
#define _NCC_ENSURE_ONE(arg, typename)                                   \
    ({static_assert(NCC_OBJ_OF_TYPE(arg, typename),		\
		    "Argument is not of correct type."); arg;})
// clang-format on

#define _NCC_ENSURE_LATER_ARG(arg, type) , _NCC_ENSURE_ONE(arg, type)

/** @} */
