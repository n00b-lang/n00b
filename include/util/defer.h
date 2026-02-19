/**
 * @file defer.h
 * @brief Defer macro system for cleanup-on-scope-exit.
 *
 * Provides Go-style `defer` semantics using GCC computed goto
 * (`&&label` / `goto *ptr`).  Deferred blocks execute in LIFO order
 * when leaving a function via `n00b_defer_return` or falling through
 * `n00b_defer_func_end`.
 *
 * ### Usage
 *
 * ```c
 * void example(void)
 * {
 *     n00b_enable_defer();
 *
 *     int *p = malloc(sizeof(int));
 *     n00b_defer(free(p));
 *
 *     // ... work ...
 *
 *     n00b_defer_return;           // runs deferred blocks, then returns
 *     n00b_defer_func_end();       // safety net (asserts if reached)
 * }
 * ```
 *
 * When `N00B_USE_INTERNAL_API` is defined before including this header,
 * short aliases are available: `defer_on()`, `defer()`, `Return`,
 * `defer_func_end()`.
 */
#pragma once
#include <stdint.h>

typedef struct n00b_defer_ll_t n00b_defer_ll_t;

#if defined(__GNUC__) && !defined(__llvm__)
struct n00b_defer_ll_t {
    volatile void   *next_target;
    volatile int64_t guard;
};
#else
struct n00b_defer_ll_t {
    void   *next_target;
    int64_t guard;
};
#endif

#define N00B_DEFER_INIT ((int64_t)0xdefe11defe11defeLL)

#define n00b_enable_defer()               \
    n00b_defer_ll_t __n00b_defer_list = { \
        nullptr,                          \
        N00B_DEFER_INIT,                  \
    };                                    \
    void *__n00b_defer_return_label = nullptr

#define n00b_token_paste(x, y) x##y

#define n00b_defer_label(x) n00b_token_paste(__defer_block_, x)

#define n00b_defer_node(x) n00b_token_paste(__n00b_defer_node_, x)

#if defined(__GNU_SOURCE)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif

#define n00b_defer(defer_block)                                                \
    n00b_defer_ll_t n00b_defer_node(__LINE__);                                 \
    if (n00b_defer_node(__LINE__).guard != N00B_DEFER_INIT) {                  \
        n00b_defer_node(__LINE__).guard       = N00B_DEFER_INIT;               \
        n00b_defer_node(__LINE__).next_target = __n00b_defer_list.next_target; \
        __n00b_defer_list.next_target         = &&n00b_defer_label(__LINE__);  \
    }                                                                          \
    if (false) {                                                               \
        n00b_defer_label(__LINE__) :                                           \
        {                                                                      \
            n00b_defer_node(__LINE__).guard = 0ULL;                            \
            defer_block;                                                       \
            if (!n00b_defer_node(__LINE__).next_target) {                      \
                if (!__n00b_defer_return_label) {                              \
                    goto n00b_defer_bottom_exit;                               \
                }                                                              \
                goto *(__n00b_defer_return_label);                             \
            }                                                                  \
        }                                                                      \
        goto *(n00b_defer_node(__LINE__).next_target);                         \
    }

#define n00b_defer_func_exit()                 \
    if (__n00b_defer_list.next_target) {       \
        goto *(__n00b_defer_list.next_target); \
    }

#define n00b_defer_return                                     \
    __n00b_defer_return_label = &&n00b_defer_label(__LINE__); \
    n00b_defer_func_exit();                                   \
    n00b_defer_label(__LINE__) : return

#define n00b_defer_func_end()                                              \
    n00b_defer_bottom_exit : assert("You forgot to return on some branch." \
                                    == nullptr)

#if defined(N00B_USE_INTERNAL_API)
#define Return           n00b_defer_return
#define defer(x)         n00b_defer(x)
#define defer_on()       n00b_enable_defer()
#define defer_func_end() n00b_defer_func_end()
#endif
