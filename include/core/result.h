/**
 * @file result.h
 * @brief Type-safe error handling — no IMPL macro required.
 *
 * @c base_result_t(T, E) represents either a success value of type @p T
 * or an error of type @p E, similar to Rust's @c Result<T, E>.
 *
 * The struct is defined inline and all operations are pure macros.
 *
 * Usage:
 * @code
 *     base_result_t(int, char *) ok  = base_result_ok(int, char *, 42);
 *     base_result_t(int, char *) err = base_result_err(int, char *, "failed");
 *     if (base_result_is_ok(ok)) {
 *         int val = base_result_unwrap(ok);
 *     }
 * @endcode
 */
#pragma once

#include "generic.h"

// ============================================================================
// Standard error type
// ============================================================================

typedef enum base_err {
    BASE_ERR_NONE = 0,
    BASE_ERR_ALLOC,
    BASE_ERR_IO,
    BASE_ERR_INVALID_ARG,
    BASE_ERR_NOT_FOUND,
} base_err_t;

// ============================================================================
// Type definition
// ============================================================================

/**
 * @brief Declare + define a result type with success type @p T and error type
 *        @p E. Use this in variable declarations; the struct body is emitted
 *        here so the type is complete.
 */
#define base_result_t(T, E)                                                    \
    struct BASE_RESULT_NAME(T, E) {                                            \
        bool  is_ok;                                                           \
        union {                                                                \
            T ok;                                                              \
            E err;                                                             \
        };                                                                     \
    }

/**
 * @brief Tag-only reference (no body) — use after the type has already been
 *        defined by @c base_result_t in the same scope.
 */
#define base_result_tag(T, E) struct BASE_RESULT_NAME(T, E)

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Create a successful result containing @p val.
 * @param T    Success type.
 * @param E    Error type.
 * @param val  The success value.
 * @return An Ok result.
 */
#define base_result_ok(T, E, val)                                              \
    ({                                                                         \
        base_result_tag(T, E) _r = {.is_ok = true, .ok = (val)};              \
        _r;                                                                    \
    })

/**
 * @brief Create an error result containing @p val.
 * @param T    Success type.
 * @param E    Error type.
 * @param val  The error value.
 * @return An Err result.
 */
#define base_result_err(T, E, val)                                             \
    ({                                                                         \
        base_result_tag(T, E) _r = {.is_ok = false, .err = (val)};            \
        _r;                                                                    \
    })

// ============================================================================
// Queries
// ============================================================================

/**
 * @brief Check if the result is Ok (success).
 * @param r  Result to check.
 * @return @c true if the result holds a success value.
 */
#define base_result_is_ok(r) ((r).is_ok)

/**
 * @brief Check if the result is Err (error).
 * @param r  Result to check.
 * @return @c true if the result holds an error value.
 */
#define base_result_is_err(r) (!(r).is_ok)

// ============================================================================
// Extraction
// ============================================================================

/**
 * @brief Extract the success value.
 *
 * Undefined behavior if the result is Err.
 *
 * @param r  Result to unwrap.
 * @return The success value.
 */
#define base_result_unwrap(r) ((r).ok)

/**
 * @brief Extract the error value.
 *
 * Undefined behavior if the result is Ok.
 *
 * @param r  Result to unwrap.
 * @return The error value.
 */
#define base_result_unwrap_err(r) ((r).err)

/**
 * @brief Extract the success value, or return @p def if Err.
 * @param r    Result to unwrap.
 * @param def  Default value if the result is Err.
 * @return The success value or @p def.
 */
#define base_result_unwrap_or(r, def) ((r).is_ok ? (r).ok : (def))

/**
 * @brief Extract the error value, or return @p def if Ok.
 * @param r    Result to unwrap.
 * @param def  Default value if the result is Ok.
 * @return The error value or @p def.
 */
#define base_result_unwrap_err_or(r, def) ((r).is_ok ? (def) : (r).err)

// ============================================================================
// Pattern matching
// ============================================================================

/**
 * @brief Pattern-match on a result.
 * @param r         Result to match.
 * @param ok_expr   Expression to evaluate if Ok.
 * @param err_expr  Expression to evaluate if Err.
 * @return Result of the matching expression.
 */
#define base_result_match(r, ok_expr, err_expr)                                \
    ((r).is_ok ? (ok_expr) : (err_expr))

/**
 * @brief Execute @p body if the result is Ok, binding the value to @p var.
 * @param r     Result to check.
 * @param var   Name of the variable bound to the success value.
 * @param body  Statement(s) to execute.
 */
#define base_result_if_ok(r, var, body)                                        \
    do {                                                                       \
        if ((r).is_ok) {                                                       \
            typeof((r).ok) var = (r).ok;                                       \
            body                                                               \
        }                                                                      \
    } while (0)

/**
 * @brief Execute @p body if the result is Err, binding the error to @p var.
 * @param r     Result to check.
 * @param var   Name of the variable bound to the error value.
 * @param body  Statement(s) to execute.
 */
#define base_result_if_err(r, var, body)                                       \
    do {                                                                       \
        if (!(r).is_ok) {                                                      \
            typeof((r).err) var = (r).err;                                     \
            body                                                               \
        }                                                                      \
    } while (0)

