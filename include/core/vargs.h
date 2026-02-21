/**
 * @file vargs.h
 * @brief n00b variadic argument infrastructure.
 *
 * Defines `n00b_vargs_t`, the struct used by ncc's `+` variadic parameter
 * transform. When ncc sees `void foo(int x, T +)`, it rewrites the
 * declaration to `void foo(int x, n00b_vargs_t *vargs)` and packs
 * arguments at each call site into a compound literal.
 *
 * This header also provides macros and inline helpers for:
 * - Iterating over vargs (`n00b_vargs_next`, `n00b_vargs_peek`)
 * - Constructing vargs from arrays, va_lists, or inline arguments
 * - Stack-allocated (VLA) and heap-allocated vargs
 *
 * ### Related modules
 *
 * - `core/va_macros.h` -- recursive macro helpers (`N00B_VA_COUNT`, etc.)
 * - `core/alloc.h` -- allocator for dynamic vargs
 */
#pragma once

#if __has_include(<stdckdint.h>)
#include <stdckdint.h>
#else
static inline bool
ckd_add(unsigned int *result, unsigned int a, unsigned int b)
{
    *result = a + b;
    return (*result < a);
}
#endif

#include <stdarg.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif
#include "core/macros.h"

typedef struct n00b_vargs_t n00b_vargs_t;

static_assert(sizeof(void *) == 8, "Implementation expects 64-bit pointers.");

// ===========================================================================
// Core struct
// ===========================================================================

/** @brief Packed variadic argument context.
 *
 * Created by ncc's `+` transform at call sites as a compound literal:
 * ```c
 * &(n00b_vargs_t){.nargs=N, .cur_ix=0, .args=(void*[]){a1, a2, ...}}
 * ```
 */
struct n00b_vargs_t {
    unsigned int nargs;  /**< Total number of variadic arguments. */
    unsigned int cur_ix; /**< Current iteration index. */
    void       **args;   /**< Array of arguments (as void pointers). */
};

// ===========================================================================
// Initialization and iteration
// ===========================================================================

static inline n00b_vargs_t *
n00b_vargs_zero_init(n00b_vargs_t *va)
{
    *va = (n00b_vargs_t){
        .nargs  = 0,
        .cur_ix = 0,
        .args   = nullptr,
    };
    return va;
}

static inline void **
_n00b_vargs_zero_varg_array(void **arr, unsigned int count)
{
    memset(arr, 0, sizeof(void *) * count);
    return arr;
}

/** @brief Return the number of unconsumed variadic arguments. */
static inline unsigned int
n00b_remaining_vargs(n00b_vargs_t *va_ctx)
{
    if (!va_ctx) {
        return 0;
    }
    return va_ctx->nargs - va_ctx->cur_ix;
}

/** @brief Get the next variadic argument, advancing the cursor.
 *  @param va_ctx  Vargs context.
 *  @param err     Optional error flag (set true on exhaustion or null ctx).
 *  @return The next argument as `void *`, or nullptr on error.
 */
static inline void *
_n00b_vargs_next(n00b_vargs_t *va_ctx, bool *err)
{
    if (!va_ctx) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (va_ctx->cur_ix >= va_ctx->nargs) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (err) {
        *err = false;
    }
    return va_ctx->args[va_ctx->cur_ix++];
}

#define n00b_vargs_next(va_ctx) _n00b_vargs_next(va_ctx, nullptr)

/** @brief Advance the cursor without returning the argument. */
static inline void
n00b_vargs_advance(n00b_vargs_t *va_ctx)
{
    if (!va_ctx) {
        return;
    }
    if (va_ctx->cur_ix < va_ctx->nargs) {
        ++va_ctx->cur_ix;
    }
}

/** @brief Peek at the current argument without advancing.
 *  @param va_ctx  Vargs context.
 *  @param err     Optional error flag.
 *  @return The current argument as `void *`, or nullptr on error.
 */
static inline void *
_n00b_vargs_peek(n00b_vargs_t *va_ctx, bool *err)
{
    if (!va_ctx) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (va_ctx->cur_ix >= va_ctx->nargs) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (err) {
        *err = false;
    }
    return va_ctx->args[va_ctx->cur_ix];
}

#define n00b_vargs_peek(va_ctx) _n00b_vargs_peek(va_ctx, nullptr)

/** @brief Peek forward by @p n positions without advancing. */
static inline void *
n00b_vargs_peek_forward(n00b_vargs_t *va_ctx, unsigned int n, bool *err)
{
    unsigned int ix;

    if (!va_ctx || ckd_add(&ix, va_ctx->cur_ix, n)) {
bad_loc:
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (ix >= va_ctx->nargs) {
        goto bad_loc;
    }
    if (err) {
        *err = false;
    }
    return va_ctx->args[ix];
}

/** @brief Get the address of the next argument slot, advancing. */
static inline void **
n00b_get_next_vargs_by_address(n00b_vargs_t *va_ctx)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[va_ctx->cur_ix++];
}

/** @brief Advance cursor to the end (consume all remaining). */
static inline void
n00b_vargs_advance_to_end(n00b_vargs_t *va_ctx)
{
    if (va_ctx) {
        va_ctx->cur_ix = va_ctx->nargs;
    }
}

/** @brief Rewind cursor to the beginning. */
static inline void
n00b_vargs_rewind(n00b_vargs_t *va_ctx)
{
    if (va_ctx) {
        va_ctx->cur_ix = 0;
    }
}

/** @brief Get the address of the current argument without advancing. */
static inline void *
n00b_vargs_peek_address(n00b_vargs_t *va_ctx)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[va_ctx->cur_ix];
}

/** @brief Get the address of the argument @p n positions forward. */
static inline void *
n00b_vargs_peek_forward_address(n00b_vargs_t *va_ctx, unsigned int n)
{
    unsigned int ix;
    if (!va_ctx || ckd_add(&ix, va_ctx->cur_ix, n)) {
        return nullptr;
    }
    if (ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[ix];
}

// ===========================================================================
// Stack-allocated (VLA) construction
// ===========================================================================

/** @brief Construct a stack-allocated vargs with inline arguments. */
#define _n00b_vargs_vla_static(count, ...)                                                     \
    (n00b_vargs_t[1])                                                                          \
    {                                                                                          \
        {                                               \
            .nargs  = (count),                          \
            .cur_ix = 0,                                \
            .args   = (void **)(void *[]){              \
                N00B_VA_VOID_STAR_CONVERT(__VA_ARGS__), \
            },                                          \
        },                                      \
    }

/** @brief Construct a stack-allocated vargs with a dynamic count. */
#define _n00b_vargs_vla_dynamic(count)                                                         \
    (n00b_vargs_t[1])                                                                          \
    {                                                                                          \
        {                                                                                      \
            .nargs  = (count),                                                                 \
            .cur_ix = 0,                                                                       \
            .args   = _n00b_vargs_zero_varg_array(((void **)alloca(sizeof(void *) * (count))), \
                                                  (count)),                                    \
        },                                                                                     \
    }

#define n00b_vargs_empty_vla(nitems) _n00b_vargs_vla_dynamic(nitems)

// ===========================================================================
// Population from va_list and arrays
// ===========================================================================

static inline n00b_vargs_t *
_n00b_vargs_va_list_populate(n00b_vargs_t *va_ctx, unsigned int nargs, va_list user_va_list)
{
    va_list ap;
    va_copy(ap, user_va_list);

    va_ctx->nargs  = nargs;
    va_ctx->cur_ix = 0;
    assert(va_ctx->args);
    memset(va_ctx->args, 0, sizeof(void *) * nargs);

    for (unsigned int i = 0; i < nargs; i++) {
        va_ctx->args[i] = va_arg(ap, void *);
    }

    va_end(ap);
    return va_ctx;
}

static inline n00b_vargs_t *
_n00b_vargs_va_populate(n00b_vargs_t *va_ctx, unsigned int nargs, ...)
{
    va_list ap;
    va_start(ap);
    _n00b_vargs_va_list_populate(va_ctx, nargs, ap);
    va_end(ap);
    return va_ctx;
}

static inline n00b_vargs_t *
n00b_vargs_array_populate(n00b_vargs_t *va_ctx,
                          unsigned int  len,
                          unsigned int  nargs,
                          void         *ptr_arr)
{
    if (len == sizeof(void *)) {
        memcpy(va_ctx->args, ptr_arr, sizeof(void *) * nargs);
        return va_ctx;
    }

    uint8_t *p = (void *)ptr_arr;
    uint64_t tmp;

    switch (len) {
    case 1:
        for (unsigned int i = 0; i < nargs; i++) {
            tmp             = *p++;
            va_ctx->args[i] = (void *)tmp;
        }
        return va_ctx;
    case 2:
        for (unsigned int i = 0; i < nargs; i++) {
            tmp             = *(uint16_t *)p;
            va_ctx->args[i] = (void *)tmp;
            p += 2;
        }
        return va_ctx;
    case 4:
        for (unsigned int i = 0; i < nargs; i++) {
            tmp             = *(uint32_t *)p;
            va_ctx->args[i] = (void *)tmp;
            p += 4;
        }
        return va_ctx;
    default:
        assert(0);
    }
}

static inline n00b_vargs_t *
_n00b_vargs_copy(n00b_vargs_t *dst, n00b_vargs_t *src)
{
    assert(dst->nargs == src->nargs);
    dst->cur_ix = src->cur_ix;
    memcpy(dst->args, src->args, sizeof(void *) * src->nargs);
    return dst;
}

// ===========================================================================
// High-level stack-allocated construction macros
// ===========================================================================

#define _n00b_vargs_base(allocated, ...)                                                       \
    _n00b_vargs_va_populate(allocated,                                                         \
                            N00B_VA_COUNT(__VA_ARGS__),                                        \
                            N00B_VA_VOID_STAR_CONVERT(__VA_ARGS__))

#define _n00b_vargs_from_va_list_base(allocated, count, ap)                                    \
    _n00b_vargs_va_list_populate(allocated, count, ap)

#define _n00b_vargs_from_array_base(allocated, count, ptr)                                     \
    n00b_vargs_array_populate((allocated), sizeof(ptr[0]), (count), (ptr))

static inline n00b_vargs_t *
_n00b_vargs_from_array_ref_base(n00b_vargs_t *allocated, unsigned int count, void *ptr)
{
    *allocated = (n00b_vargs_t){
        .nargs  = count,
        .cur_ix = 0,
        .args   = ptr,
    };
    return allocated;
}

/** @brief Construct a vargs from inline arguments (stack-allocated). */
#define _n00b_vargs_args(...)                                                                  \
    _n00b_vargs_vla_static(N00B_VA_COUNT(__VA_ARGS__), N00B_VA_VOID_STAR_CONVERT(__VA_ARGS__))

#define _n00b_vargs_empty(...) n00b_vargs_zero_init(&((n00b_vargs_t[1]){0}[0]))

#define n00b_vargs(...)                                                                        \
    N00B_CONCAT_INDIRECT(_n00b_vargs_, N00B_FIRST(__VA_OPT__(args, ) empty))                   \
    (__VA_ARGS__)

#define n00b_vargs_checked(typename, ...) n00b_vargs(N00B_VA_TYPE_ENSURE(typename, __VA_ARGS__))

#define n00b_vargs_from_va_list(count, ap)                                                     \
    _n00b_vargs_from_va_list_base(_n00b_vargs_vla_dynamic(count), count, ap)

#define n00b_vargs_from_array(count, ptr)                                                      \
    _n00b_vargs_from_array_base(_n00b_vargs_vla_dynamic(count), (count), (ptr))

#define n00b_vargs_from_reference(count, ptr)                                                  \
    _n00b_vargs_from_array_ref_base(_n00b_vargs_vla_dynamic(count), (count), (ptr))

#define n00b_vargs_copy(src_ctx)                                                               \
    _n00b_vargs_copy(_n00b_vargs_vla_dynamic(src_ctx->nargs), src_ctx)

// ===========================================================================
// Dynamic (heap) allocation support
// ===========================================================================

typedef void *(*n00b_vargs_alloc_fn)(size_t size);

static inline n00b_vargs_t *
_n00b_vargs_alloc_custom(n00b_vargs_alloc_fn alloc, unsigned int nargs)
{
    n00b_vargs_t *result;

    result  = (*alloc)(sizeof(n00b_vargs_t));
    *result = (n00b_vargs_t){
        .nargs  = nargs,
        .cur_ix = 0,
        .args   = (*alloc)(sizeof(void *) * nargs),
    };
    return result;
}

#define _n00b_vargs_malloc(nargs) _n00b_vargs_alloc_custom(malloc, nargs)

#define n00b_vargs_dynamic(...)                                                                \
    _n00b_vargs_va_populate(_n00b_vargs_malloc(N00B_VA_COUNT(__VA_ARGS__)),                    \
                            N00B_VA_COUNT(__VA_ARGS__),                                        \
                            N00B_VA_VOID_STAR_CONVERT(__VA_ARGS__))

#define n00b_vargs_dynamic_from_va_list(count, ap)                                             \
    _n00b_vargs_from_va_list_base(_n00b_vargs_malloc(count), count, ap)

#define n00b_vargs_dynamic_from_array(count, ptr)                                              \
    _n00b_vargs_from_array_base(_n00b_vargs_malloc(count), (count), (ptr))

#define n00b_vargs_dynamic_copy(src_ctx)                                                       \
    _n00b_vargs_copy(_n00b_vargs_malloc(src_ctx->nargs), src_ctx)

#define n00b_vargs_dynamic_empty(count) _n00b_vargs_alloc_custom(malloc, count)

// Custom allocator variants.
#define n00b_vargs_custom_allocator(alloc, ...)                                                \
    _n00b_vargs_va_populate(_n00b_vargs_alloc_custom(alloc, N00B_VA_COUNT(__VA_ARGS__)),       \
                            N00B_VA_COUNT(__VA_ARGS__),                                        \
                            N00B_VA_VOID_STAR_CONVERT(__VA_ARGS__))

#define n00b_vargs_custom_allocator_from_va_list(alloc, count, ap)                             \
    _n00b_vargs_from_va_list_base(_n00b_vargs_alloc_custom(alloc, count), (count), (ap))

#define n00b_vargs_custom_allocator_from_array(alloc, count, ptr)                              \
    _n00b_vargs_from_array_base(_n00b_vargs_alloc_custom(alloc, count), (count), (ptr))

#define n00b_vargs_custom_allocator_copy(alloc, src_ctx)                                       \
    _n00b_vargs_copy(_n00b_vargs_alloc_custom(alloc, src_ctx->nargs), src_ctx)

#define n00b_vargs_custom_allocator_empty(allocator, count)                                    \
    _n00b_vargs_alloc_custom(allocator, count)
