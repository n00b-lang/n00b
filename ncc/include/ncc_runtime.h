#pragma once
/**
 * @file ncc_runtime.h
 * @brief Runtime support for code compiled by ncc.
 *
 * This header is included by ncc-compiled C output. It provides:
 * - ncc_vargs_t: variadic argument struct for the + parameter transform
 * - ncc_once(): pthread_once wrapper for the once function specifier
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
// ncc_vargs_t — variadic argument packing
// ============================================================================
//
// ncc's + transform rewrites:
//   void foo(int x, +)   →  void foo(int x, ncc_vargs_t *_ncc_vargs)
//   foo(1, a, b, c)      →  foo(1, &(ncc_vargs_t){3, 0, (void*[]){a,b,c}})

typedef struct ncc_vargs_t ncc_vargs_t;

struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
};

static inline unsigned int
ncc_remaining_vargs(ncc_vargs_t *va_ctx)
{
    if (!va_ctx) {
        return 0;
    }
    return va_ctx->nargs - va_ctx->cur_ix;
}

static inline void *
_ncc_vargs_next(ncc_vargs_t *va_ctx, bool *err)
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

#define ncc_vargs_next(va_ctx) _ncc_vargs_next(va_ctx, nullptr)

static inline void *
_ncc_vargs_peek(ncc_vargs_t *va_ctx, bool *err)
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

#define ncc_vargs_peek(va_ctx) _ncc_vargs_peek(va_ctx, nullptr)

static inline void
ncc_vargs_advance(ncc_vargs_t *va_ctx)
{
    if (va_ctx && va_ctx->cur_ix < va_ctx->nargs) {
        ++va_ctx->cur_ix;
    }
}

static inline void
ncc_vargs_rewind(ncc_vargs_t *va_ctx)
{
    if (va_ctx) {
        va_ctx->cur_ix = 0;
    }
}

static inline void
ncc_vargs_advance_to_end(ncc_vargs_t *va_ctx)
{
    if (va_ctx) {
        va_ctx->cur_ix = va_ctx->nargs;
    }
}

static inline void **
ncc_get_next_vargs_by_address(ncc_vargs_t *va_ctx)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[va_ctx->cur_ix++];
}

static inline void *
ncc_vargs_peek_address(ncc_vargs_t *va_ctx)
{
    if (!va_ctx || va_ctx->cur_ix >= va_ctx->nargs) {
        return nullptr;
    }
    return &va_ctx->args[va_ctx->cur_ix];
}

static inline void *
ncc_vargs_peek_forward(ncc_vargs_t *va_ctx, unsigned int n, bool *err)
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
ncc_vargs_peek_forward_address(ncc_vargs_t *va_ctx, unsigned int n)
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

typedef pthread_once_t ncc_once_t;

#define NCC_ONCE_INIT PTHREAD_ONCE_INIT

static inline void
ncc_once_run(ncc_once_t *flag, void (*init_fn)(void))
{
    pthread_once(flag, init_fn);
}

// ============================================================================
// Rich string support types (for r"..." transform output)
// ============================================================================
//
// ncc's r"..." transform produces static ncc_string_t + styling structs.
// The types below use ncc_ names to match what the transform emits.

typedef int32_t ncc_color_t;

typedef enum {
    NCC_TRI_UNSPECIFIED = 0,
    NCC_TRI_NO,
    NCC_TRI_YES,
} ncc_tristate_t;

typedef enum {
    NCC_TEXT_CASE_NONE = 0,
    NCC_TEXT_CASE_UPPER,
    NCC_TEXT_CASE_LOWER,
    NCC_TEXT_CASE_TITLE,
    NCC_TEXT_CASE_CAPS,
} ncc_text_case_t;

typedef enum {
    NCC_FONT_DEFAULT = 0,
    NCC_FONT_MONO,
    NCC_FONT_SERIF,
    NCC_FONT_SANS,
} ncc_font_hint_t;

typedef struct ncc_text_style_t {
    ncc_tristate_t  bold;
    ncc_tristate_t  italic;
    ncc_tristate_t  underline;
    ncc_tristate_t  double_underline;
    ncc_tristate_t  strikethrough;
    ncc_tristate_t  reverse;
    ncc_tristate_t  dim;
    ncc_tristate_t  blink;
    ncc_text_case_t text_case;
    ncc_font_hint_t font_hint;
    int8_t           font_index;
    int8_t           fg_palette_ix;
    int8_t           bg_palette_ix;
    ncc_color_t     fg_rgb;
    ncc_color_t     bg_rgb;
    int16_t          font_size;
} ncc_text_style_t;

// ncc_option_size_t: standalone definition for compiled output.
// This intentionally duplicates the layout of the internal
// ncc_option_t(size_t) so that ncc-compiled code links correctly
// without pulling in ncc's own build infrastructure.  The struct
// layout (bool + size_t, naturally aligned) is ABI-compatible.
typedef struct ncc_option_size {
    bool   has_value;
    size_t value;
} ncc_option_size_t;

typedef struct ncc_style_record_t {
    ncc_text_style_t *info;
    const char        *tag;
    size_t             start;
    ncc_option_size_t end;
} ncc_style_record_t;

typedef struct ncc_string_style_info_t {
    int64_t              num_styles;
    ncc_text_style_t   *base_style;
    ncc_style_record_t  styles[];
} ncc_string_style_info_t;

struct ncc_string_t {
    char  *data;
    size_t u8_bytes;
    size_t codepoints;
    void  *styling;
};
