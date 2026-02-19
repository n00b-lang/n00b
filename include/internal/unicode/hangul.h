#pragma once
/** @file hangul.h
 *  @brief Hangul Jamo constants and algorithmic composition/decomposition.
 *
 *  Implements the algorithms from Unicode Standard Section 3.12 for
 *  Hangul syllable decomposition and composition.
 */

#include <stdint.h>
#include <stdbool.h>
#include "unicode/types.h"

#define N00B_UNICODE_HANGUL_S_BASE  0xAC00
#define N00B_UNICODE_HANGUL_L_BASE  0x1100
#define N00B_UNICODE_HANGUL_V_BASE  0x1161
#define N00B_UNICODE_HANGUL_T_BASE  0x11A7
#define N00B_UNICODE_HANGUL_L_COUNT 19
#define N00B_UNICODE_HANGUL_V_COUNT 21
#define N00B_UNICODE_HANGUL_T_COUNT 28
#define N00B_UNICODE_HANGUL_N_COUNT \
    (N00B_UNICODE_HANGUL_V_COUNT * N00B_UNICODE_HANGUL_T_COUNT) // 588
#define N00B_UNICODE_HANGUL_S_COUNT \
    (N00B_UNICODE_HANGUL_L_COUNT * N00B_UNICODE_HANGUL_N_COUNT) // 11172

// Compatibility aliases for source files using the old names.
#define HANGUL_S_BASE  N00B_UNICODE_HANGUL_S_BASE
#define HANGUL_L_BASE  N00B_UNICODE_HANGUL_L_BASE
#define HANGUL_V_BASE  N00B_UNICODE_HANGUL_V_BASE
#define HANGUL_T_BASE  N00B_UNICODE_HANGUL_T_BASE
#define HANGUL_L_COUNT N00B_UNICODE_HANGUL_L_COUNT
#define HANGUL_V_COUNT N00B_UNICODE_HANGUL_V_COUNT
#define HANGUL_T_COUNT N00B_UNICODE_HANGUL_T_COUNT
#define HANGUL_N_COUNT N00B_UNICODE_HANGUL_N_COUNT
#define HANGUL_S_COUNT N00B_UNICODE_HANGUL_S_COUNT

static inline bool
hangul_is_syllable(n00b_codepoint_t cp)
{
    return cp >= HANGUL_S_BASE && cp < HANGUL_S_BASE + HANGUL_S_COUNT;
}

static inline bool
hangul_is_l(n00b_codepoint_t cp)
{
    return cp >= HANGUL_L_BASE && cp < HANGUL_L_BASE + HANGUL_L_COUNT;
}

static inline bool
hangul_is_v(n00b_codepoint_t cp)
{
    return cp >= HANGUL_V_BASE && cp < HANGUL_V_BASE + HANGUL_V_COUNT;
}

static inline bool
hangul_is_t(n00b_codepoint_t cp)
{
    return cp > HANGUL_T_BASE && cp <= HANGUL_T_BASE + HANGUL_T_COUNT;
}

// Algorithmic decomposition: returns number of codepoints written (2 or 3).
static inline int
hangul_decompose(n00b_codepoint_t cp, n00b_codepoint_t *out)
{
    n00b_codepoint_t s_index = cp - HANGUL_S_BASE;
    n00b_codepoint_t l = HANGUL_L_BASE + s_index / HANGUL_N_COUNT;
    n00b_codepoint_t v = HANGUL_V_BASE
                         + (s_index % HANGUL_N_COUNT) / HANGUL_T_COUNT;
    n00b_codepoint_t t = HANGUL_T_BASE + s_index % HANGUL_T_COUNT;

    out[0] = l;
    out[1] = v;
    if (t != HANGUL_T_BASE) {
        out[2] = t;
        return 3;
    }
    return 2;
}

// Algorithmic composition: returns composed syllable or 0 if not composable.
static inline n00b_codepoint_t
hangul_compose(n00b_codepoint_t a, n00b_codepoint_t b)
{
    // <L, V> -> LV syllable
    if (hangul_is_l(a) && hangul_is_v(b)) {
        n00b_codepoint_t l_index = a - HANGUL_L_BASE;
        n00b_codepoint_t v_index = b - HANGUL_V_BASE;
        return HANGUL_S_BASE
               + (l_index * HANGUL_V_COUNT + v_index) * HANGUL_T_COUNT;
    }

    // <LV, T> -> LVT syllable
    if (hangul_is_syllable(a)) {
        n00b_codepoint_t s_index = a - HANGUL_S_BASE;
        if (s_index % HANGUL_T_COUNT == 0 && hangul_is_t(b)) {
            return a + (b - HANGUL_T_BASE);
        }
    }

    return 0;
}
