#pragma once
/** @file iter.h
 *  @brief Iteration macros for codepoints, grapheme clusters, words,
 *         sentences, and lines.
 *
 *  All iteration macros use a two-level for-loop pattern that supports
 *  `break` and `continue` correctly:
 *
 *  ```
 *  for (outer; !brk && more; )     // brk=1 means user called break
 *    for (var = (brk=1, val); brk; brk=0) // body runs once per iteration
 *      USER_BODY
 *  ```
 *
 *  - Normal: body runs, inner incr sets brk=0, inner cond->false, outer
 *    retests.
 *  - `break`: exits inner, brk stays 1, outer sees !brk->false -> stops.
 *  - `continue`: inner incr runs (brk=0), inner cond->false, outer
 *    retests -> ok.
 */

#include "unicode/types_ext.h"
#include "unicode/encoding.h"
#include "unicode/segmentation.h"
#include "internal/unicode/raw.h"

// ---------------------------------------------------------------------------
// n00b_unicode_foreach_cp(s, cp) { ... }
//   s is n00b_string_t *. cp is int32_t.
// ---------------------------------------------------------------------------

/** @brief Iterate over each codepoint in a UTF-8 string.
 *
 *  Usage:
 *  @code
 *  n00b_unicode_foreach_cp(my_str, cp) {
 *      printf("U+%04X\n", cp);
 *  }
 *  @endcode
 *
 *  @param s   An `n00b_string_t *` pointer to the string.
 *  @param cp  Loop variable name (declared as `int32_t` in the loop body).
 */
#define n00b_unicode_foreach_cp(s, cp)                                      \
    for (struct { uint32_t pos; int32_t val; int brk; }                     \
             _n00b_uni_i_##cp = { .pos = 0, .val = 0, .brk = 0 };          \
         !_n00b_uni_i_##cp.brk                                              \
             && _n00b_uni_i_##cp.pos < (uint32_t)(s)->u8_bytes              \
             && (_n00b_uni_i_##cp.val                                       \
                 = n00b_unicode_utf8_decode(                                 \
                       (s)->data,                                            \
                       (uint32_t)(s)->u8_bytes,                              \
                       &_n00b_uni_i_##cp.pos))                              \
                    >= 0;                                                    \
         (void)0)                                                           \
        for (int32_t cp = (_n00b_uni_i_##cp.brk = 1,                       \
                           _n00b_uni_i_##cp.val);                           \
             _n00b_uni_i_##cp.brk;                                          \
             _n00b_uni_i_##cp.brk = 0)

// ---------------------------------------------------------------------------
// Segment iterators: grapheme / word / sentence
//
// Uses __attribute__((cleanup)) for reliable deallocation on break/return.
// s is n00b_string_t *.
// ---------------------------------------------------------------------------

/** @brief Internal state for segment (grapheme/word/sentence) iteration. */
typedef struct {
    n00b_unicode_break_iter_t *it;   /**< Underlying break iterator */
    const char                *data; /**< Pointer to the string's byte data */
    int64_t                    len;  /**< Length of the string in bytes */
    uint32_t                   prev; /**< Byte offset: start of current segment */
    uint32_t                   cur;  /**< Byte offset: end of current segment */
    bool                       done; /**< true when iteration is complete */
    int                        brk;  /**< Break flag for the two-level pattern */
} n00b_unicode_seg_iter_t;

/** @brief Initialize a segment iterator from a string and break iterator.
 *  @param s   The string to iterate over.
 *  @param it  A break iterator (grapheme, word, or sentence).
 *  @return An initialized segment iterator state.
 */
static inline n00b_unicode_seg_iter_t
n00b_unicode_seg_iter_init(n00b_string_t *s,
                           n00b_unicode_break_iter_t *it)
{
    return (n00b_unicode_seg_iter_t){
        .it   = it,
        .data = s->data,
        .len  = s->u8_bytes,
        .prev = 0,
        .cur  = 0,
        .done = false,
        .brk  = 0,
    };
}

/** @brief Cleanup function for segment iterators
 *         (called via `__attribute__((cleanup))`).
 *  @param si  The segment iterator to clean up.
 */
static inline void
n00b_unicode_seg_iter_cleanup(n00b_unicode_seg_iter_t *si)
{
    if (si->it) {
        n00b_unicode_break_iter_free(si->it);
        si->it = nullptr;
    }
}

/** @brief Advance a segment iterator to the next segment.
 *  @param si   The segment iterator.
 *  @param out  Out: an `n00b_string_t` view of the next segment (not
 *              null-terminated).
 *  @return true if a segment was produced, false if iteration is complete.
 */
static inline bool
n00b_unicode_seg_iter_advance(n00b_unicode_seg_iter_t *si,
                              n00b_string_t *out)
{
    if (si->done || si->brk) return false;

    si->prev = si->cur;
    int32_t b = n00b_unicode_break_next(si->it);

    if (b >= 0) {
        si->cur = (uint32_t)b;
    }
    else {
        si->cur  = (uint32_t)si->len;
        si->done = true;
    }

    if (si->cur > si->prev) {
        uint32_t seg_len = si->cur - si->prev;
        int64_t  ncp     = n00b_unicode_utf8_count_codepoints_raw(
            si->data + si->prev, seg_len);
        *out = (n00b_string_t){
            .u8_bytes   = seg_len,
            .data       = (char *)(si->data + si->prev),
            .codepoints = ncp >= 0 ? ncp : 0,
            .u32_data   = nullptr,
            .styling    = nullptr,
        };
        return true;
    }
    return false;
}

#define _N00B_UNICODE_FOREACH_BREAK(s, var, iter_fn)                        \
    for (n00b_unicode_seg_iter_t _n00b_uni_si_##var                         \
             __attribute__((cleanup(n00b_unicode_seg_iter_cleanup))) =       \
                 n00b_unicode_seg_iter_init(                                 \
                     (s), iter_fn((s)->data, (s)->u8_bytes));               \
         !_n00b_uni_si_##var.done && !_n00b_uni_si_##var.brk;              \
         (void)0)                                                           \
        for (n00b_string_t var = {0};                                       \
             n00b_unicode_seg_iter_advance(&_n00b_uni_si_##var, &var)       \
                 && (_n00b_uni_si_##var.brk = 1);                           \
             _n00b_uni_si_##var.brk = 0)

/** @brief Iterate over each grapheme cluster in a string.
 *
 *  Usage:
 *  @code
 *  n00b_unicode_foreach_grapheme(my_str, g) {
 *      // g is an n00b_string_t view of one grapheme cluster
 *  }
 *  @endcode
 *
 *  @param s  An `n00b_string_t *` pointer to the string.
 *  @param g  Loop variable name (declared as `n00b_string_t` in the body).
 */
#define n00b_unicode_foreach_grapheme(s, g) \
    _N00B_UNICODE_FOREACH_BREAK(s, g, n00b_unicode_grapheme_iter_raw)

/** @brief Iterate over each word in a string (UAX #29 word boundaries).
 *
 *  Usage:
 *  @code
 *  n00b_unicode_foreach_word(my_str, w) {
 *      // w is an n00b_string_t view of one word segment
 *  }
 *  @endcode
 *
 *  @param s  An `n00b_string_t *` pointer to the string.
 *  @param w  Loop variable name (declared as `n00b_string_t` in the body).
 */
#define n00b_unicode_foreach_word(s, w) \
    _N00B_UNICODE_FOREACH_BREAK(s, w, n00b_unicode_word_iter_raw)

/** @brief Iterate over each sentence in a string (UAX #29 sentence
 *         boundaries).
 *
 *  Usage:
 *  @code
 *  n00b_unicode_foreach_sentence(my_str, sent) {
 *      // sent is an n00b_string_t view of one sentence segment
 *  }
 *  @endcode
 *
 *  @param s     An `n00b_string_t *` pointer to the string.
 *  @param sent  Loop variable name (declared as `n00b_string_t` in the
 *               body).
 */
#define n00b_unicode_foreach_sentence(s, sent) \
    _N00B_UNICODE_FOREACH_BREAK(s, sent, n00b_unicode_sentence_iter_raw)

// ---------------------------------------------------------------------------
// n00b_unicode_foreach_line(s, line) { ... }
//   s is n00b_string_t *.  Split at CR, LF, or CRLF.
//   line is n00b_string_t (no terminator).
// ---------------------------------------------------------------------------

/** @brief Internal state for line iteration. */
typedef struct {
    const char *data; /**< Pointer to the string's byte data */
    int64_t     len;  /**< Length of the string in bytes */
    uint32_t    pos;  /**< Current byte position */
    bool        done; /**< true when iteration is complete */
    int         brk;  /**< Break flag for the two-level for-loop pattern */
} n00b_unicode_line_iter_t;

/** @brief Initialize a line iterator from a string.
 *  @param s  The string to iterate over.
 *  @return An initialized line iterator state.
 */
static inline n00b_unicode_line_iter_t
n00b_unicode_line_iter_init(n00b_string_t *s)
{
    return (n00b_unicode_line_iter_t){
        .data = s->data,
        .len  = s->u8_bytes,
        .pos  = 0,
        .done = false,
        .brk  = 0,
    };
}

/** @brief Advance a line iterator to the next line.
 *  @param li   The line iterator.
 *  @param out  Out: an `n00b_string_t` view of the next line (without
 *              terminator).
 *  @return true if a line was produced, false if iteration is complete.
 */
static inline bool
n00b_unicode_line_iter_next(n00b_unicode_line_iter_t *li,
                            n00b_string_t *out)
{
    if (li->done || li->brk) return false;

    uint32_t    start = li->pos;
    const char *d     = li->data;
    uint32_t    ulen  = (uint32_t)li->len;

    while (li->pos < ulen && d[li->pos] != '\n' && d[li->pos] != '\r') {
        li->pos++;
    }

    uint32_t seg_len = li->pos - start;
    int64_t  ncp     = n00b_unicode_utf8_count_codepoints_raw(
        d + start, seg_len);
    *out = (n00b_string_t){
        .u8_bytes   = seg_len,
        .data       = (char *)(d + start),
        .codepoints = ncp >= 0 ? ncp : 0,
        .u32_data   = nullptr,
        .styling    = nullptr,
    };

    if (li->pos < ulen) {
        if (d[li->pos] == '\r') {
            li->pos++;
            if (li->pos < ulen && d[li->pos] == '\n') {
                li->pos++;
            }
        }
        else {
            li->pos++;
        }
    }
    else {
        li->done = true;
    }

    return true;
}

/** @brief Iterate over each line in a string (split at CR, LF, or CRLF).
 *
 *  Usage:
 *  @code
 *  n00b_unicode_foreach_line(my_str, line) {
 *      // line is an n00b_string_t view of one line (without terminator)
 *  }
 *  @endcode
 *
 *  @param s     An `n00b_string_t *` pointer to the string.
 *  @param line  Loop variable name (declared as `n00b_string_t` in the
 *               body).
 */
#define n00b_unicode_foreach_line(s, line)                                  \
    for (n00b_unicode_line_iter_t _n00b_uni_li_##line =                     \
             n00b_unicode_line_iter_init(s);                                \
         !_n00b_uni_li_##line.done && !_n00b_uni_li_##line.brk;            \
         (void)0)                                                           \
        for (n00b_string_t line = {0};                                      \
             n00b_unicode_line_iter_next(&_n00b_uni_li_##line, &line)       \
                 && (_n00b_uni_li_##line.brk = 1);                          \
             _n00b_uni_li_##line.brk = 0)
