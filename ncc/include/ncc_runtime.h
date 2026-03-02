#pragma once
/**
 * @file ncc_runtime.h
 * @brief Runtime support for code compiled by ncc.
 *
 * This header is included by ncc-compiled C output. It provides:
 * - n00b_vargs_t: variadic argument struct for the + parameter transform
 * - n00b_once(): pthread_once wrapper for the once function specifier
 * - Rich string types for r"..." literals
 *
 * This does NOT include ncc's own build infrastructure — it is solely
 * for the output of ncc to link against.
 */

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// n00b_vargs_t — variadic argument packing
// ============================================================================
//
// ncc's + transform rewrites:
//   void foo(int x, +)   →  void foo(int x, n00b_vargs_t *_ncc_vargs)
//   foo(1, a, b, c)      →  foo(1, &(n00b_vargs_t){3, 0, (void*[]){a,b,c}})

typedef struct n00b_vargs_t n00b_vargs_t;

struct n00b_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
};

static inline unsigned int
n00b_remaining_vargs(n00b_vargs_t *va_ctx)
{
    if (!va_ctx) {
        return 0;
    }
    return va_ctx->nargs - va_ctx->cur_ix;
}

static inline void *
_n00b_vargs_next(n00b_vargs_t *va_ctx, bool *err)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
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

static inline void *
_n00b_vargs_peek(n00b_vargs_t *va_ctx, bool *err)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
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

static inline void
n00b_vargs_advance(n00b_vargs_t *va_ctx)
{
    if (va_ctx && va_ctx->cur_ix < va_ctx->nargs) {
        ++va_ctx->cur_ix;
    }
}

static inline void
n00b_vargs_rewind(n00b_vargs_t *va_ctx)
{
    if (va_ctx) {
        va_ctx->cur_ix = 0;
    }
}

static inline void
n00b_vargs_advance_to_end(n00b_vargs_t *va_ctx)
{
    if (va_ctx) {
        va_ctx->cur_ix = va_ctx->nargs;
    }
}

static inline void **
n00b_get_next_vargs_by_address(n00b_vargs_t *va_ctx)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[va_ctx->cur_ix++];
}

static inline void *
n00b_vargs_peek_address(n00b_vargs_t *va_ctx)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[va_ctx->cur_ix];
}

static inline void *
n00b_vargs_peek_forward(n00b_vargs_t *va_ctx, unsigned int n, bool *err)
{
    if (!va_ctx || va_ctx->cur_ix + n < va_ctx->cur_ix) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    unsigned int ix = va_ctx->cur_ix + n;
    if (ix >= va_ctx->nargs) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (err) {
        *err = false;
    }
    return va_ctx->args[ix];
}

static inline void **
n00b_vargs_peek_forward_address(n00b_vargs_t *va_ctx, unsigned int n)
{
    if (!va_ctx || va_ctx->cur_ix + n < va_ctx->cur_ix) {
        return nullptr;
    }
    unsigned int ix = va_ctx->cur_ix + n;
    if (ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[ix];
}

// ============================================================================
// _Once support — pthread_once wrapper
// ============================================================================
//
// ncc's once transform rewrites:
//   once int foo(void) { ... }
// to a wrapper that calls the body exactly once (thread-safe).

typedef pthread_once_t n00b_once_t;

#define N00B_ONCE_INIT PTHREAD_ONCE_INIT

static inline void
n00b_once_run(n00b_once_t *flag, void (*init_fn)(void))
{
    pthread_once(flag, init_fn);
}

// ============================================================================
// Rich string support types (for r"..." transform output)
// ============================================================================
//
// ncc's r"..." transform produces static n00b_string_t + styling structs.
// The types below use n00b_ names to match what the transform emits.

typedef int32_t n00b_color_t;

typedef enum {
    N00B_TRI_UNSPECIFIED = 0,
    N00B_TRI_NO,
    N00B_TRI_YES,
} n00b_tristate_t;

typedef enum {
    N00B_TEXT_CASE_NONE = 0,
    N00B_TEXT_CASE_UPPER,
    N00B_TEXT_CASE_LOWER,
    N00B_TEXT_CASE_TITLE,
    N00B_TEXT_CASE_CAPS,
} n00b_text_case_t;

typedef enum {
    N00B_FONT_DEFAULT = 0,
    N00B_FONT_MONO,
    N00B_FONT_SERIF,
    N00B_FONT_SANS,
} n00b_font_hint_t;

typedef struct n00b_text_style_t {
    n00b_tristate_t  bold;
    n00b_tristate_t  italic;
    n00b_tristate_t  underline;
    n00b_tristate_t  double_underline;
    n00b_tristate_t  strikethrough;
    n00b_tristate_t  reverse;
    n00b_tristate_t  dim;
    n00b_tristate_t  blink;
    n00b_text_case_t text_case;
    n00b_font_hint_t font_hint;
    int8_t           font_index;
    int8_t           fg_palette_ix;
    int8_t           bg_palette_ix;
    n00b_color_t     fg_rgb;
    n00b_color_t     bg_rgb;
    int16_t          font_size;
} n00b_text_style_t;

// n00b_option_size_t: standalone definition for compiled output.
// This intentionally duplicates the layout of the internal
// n00b_option_t(size_t) so that ncc-compiled code links correctly
// without pulling in ncc's own build infrastructure.  The struct
// layout (bool + size_t, naturally aligned) is ABI-compatible.
typedef struct n00b_option_size {
    bool   has_value;
    size_t value;
} n00b_option_size_t;

typedef struct n00b_style_record_t {
    n00b_text_style_t *info;
    const char        *tag;
    size_t             start;
    n00b_option_size_t end;
} n00b_style_record_t;

typedef struct n00b_string_style_info_t {
    int64_t              num_styles;
    n00b_text_style_t   *base_style;
    n00b_style_record_t  styles[];
} n00b_string_style_info_t;

struct n00b_string_t {
    char  *data;
    size_t u8_bytes;
    size_t codepoints;
    void  *styling;
};
