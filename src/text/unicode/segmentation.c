#include "text/unicode/segmentation.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"
#include "text/unicode/ctx.h"
#include "core/atomic.h"
#include "core/runtime.h"
#include "core/rt_access.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "internal/text/unicode/tables.h"
#include <string.h>
#include <assert.h>

// Per-runtime unicode subsystem context.  See properties.c for the
// rationale.
static inline n00b_unicode_ctx_t *
_uctx(void)
{
    return n00b_get_runtime()->unicode_ctx;
}

// External generated tables
extern const uint16_t n00b_unicode_gcb_stage1[];
extern const uint8_t  n00b_unicode_gcb_stage2[];

extern const uint16_t n00b_unicode_wb_stage1[];
extern const uint8_t  n00b_unicode_wb_stage2[];

extern const uint16_t n00b_unicode_sb_stage1[];
extern const uint8_t  n00b_unicode_sb_stage2[];

extern const uint16_t n00b_unicode_lb_stage1[];
extern const uint8_t  n00b_unicode_lb_stage2[];

static inline n00b_unicode_gcb_t
get_gcb(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_GCB_OTHER;
    return (n00b_unicode_gcb_t)N00B_UNICODE_LOOKUP(n00b_unicode_gcb_stage1,
                                                   n00b_unicode_gcb_stage2,
                                                   cp);
}

static inline n00b_unicode_wb_t
get_wb(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_WB_OTHER;
    return (n00b_unicode_wb_t)N00B_UNICODE_LOOKUP(n00b_unicode_wb_stage1,
                                                  n00b_unicode_wb_stage2,
                                                  cp);
}

static inline n00b_unicode_sb_t
get_sb(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_SB_OTHER;
    return (n00b_unicode_sb_t)N00B_UNICODE_LOOKUP(n00b_unicode_sb_stage1,
                                                  n00b_unicode_sb_stage2,
                                                  cp);
}

// ---------------------------------------------------------------------------
// Break iterator structure
// ---------------------------------------------------------------------------

struct n00b_unicode_break_iter_s {
    const char               *data;
    int64_t                   len;
    n00b_unicode_break_type_t type;
    uint32_t                  byte_pos; // current byte position in string
    uint32_t                  cp_pos;   // current codepoint index

    // For grapheme breaks:
    uint32_t ri_count;            // regional indicator count
    bool     after_zwj;           // previous was ZWJ
    bool     ep_before_zwj;       // Extended_Pictographic seen before ZWJ (for GB11)
    bool     incb_consonant_base; // cluster started with InCB_Consonant
    bool     incb_linker_seen;    // InCB_Linker seen since consonant

    // For word breaks: track preceding context
    n00b_unicode_wb_t prev_wb;
    n00b_unicode_wb_t prev_prev_wb;

    // For sentence breaks: state machine tracking context since last ATerm/STerm
    n00b_unicode_sb_t prev_sb;
    n00b_unicode_sb_t prev_prev_sb;
    // Phases after ATerm/STerm: 0=none, 1=seen ATerm/STerm, 2=in Close*,
    //                           3=in Sp*, 4=seen ParaSep
    uint8_t           sb_term_phase;
    bool              sb_is_aterm; // true=ATerm context, false=STerm context
};

// ---------------------------------------------------------------------------
// Grapheme cluster break rules (UAX #29)
// ---------------------------------------------------------------------------

static bool
grapheme_is_break(n00b_unicode_break_iter_t *it,
                  n00b_unicode_gcb_t         prev,
                  n00b_unicode_gcb_t         cur)
{
    // GB1/GB2: break at start/end of text (handled by caller)

    // GB3: CR × LF
    if (prev == N00B_UNICODE_GCB_CR && cur == N00B_UNICODE_GCB_LF)
        return false;

    // GB4: (Control | CR | LF) ÷
    if (prev == N00B_UNICODE_GCB_CONTROL || prev == N00B_UNICODE_GCB_CR
        || prev == N00B_UNICODE_GCB_LF)
        return true;

    // GB5: ÷ (Control | CR | LF)
    if (cur == N00B_UNICODE_GCB_CONTROL || cur == N00B_UNICODE_GCB_CR
        || cur == N00B_UNICODE_GCB_LF)
        return true;

    // GB6: L × (L | V | LV | LVT)
    if (prev == N00B_UNICODE_GCB_L
        && (cur == N00B_UNICODE_GCB_L || cur == N00B_UNICODE_GCB_V || cur == N00B_UNICODE_GCB_LV
            || cur == N00B_UNICODE_GCB_LVT))
        return false;

    // GB7: (LV | V) × (V | T)
    if ((prev == N00B_UNICODE_GCB_LV || prev == N00B_UNICODE_GCB_V)
        && (cur == N00B_UNICODE_GCB_V || cur == N00B_UNICODE_GCB_T))
        return false;

    // GB8: (LVT | T) × T
    if ((prev == N00B_UNICODE_GCB_LVT || prev == N00B_UNICODE_GCB_T)
        && cur == N00B_UNICODE_GCB_T)
        return false;

    // GB9: × (Extend | ZWJ)
    if (cur == N00B_UNICODE_GCB_EXTEND || cur == N00B_UNICODE_GCB_ZWJ
        || cur == N00B_UNICODE_GCB_INCB_EXTEND || cur == N00B_UNICODE_GCB_INCB_LINKER)
        return false;

    // GB9a: × SpacingMark
    if (cur == N00B_UNICODE_GCB_SPACINGMARK)
        return false;

    // GB9b: Prepend ×
    if (prev == N00B_UNICODE_GCB_PREPEND)
        return false;

    // GB9c: \p{InCB=Consonant} [{Extend-\p{InCB=Extend}} {Linker}]* {Linker}
    //        [{Extend-\p{InCB=Extend}} {Linker}]* × \p{InCB=Consonant}
    if (cur == N00B_UNICODE_GCB_INCB_CONSONANT && it->incb_consonant_base
        && it->incb_linker_seen)
        return false;

    // GB11: \p{Extended_Pictographic} Extend* ZWJ × \p{Extended_Pictographic}
    // (handled in the main loop, not here — we just need the flag check)

    // GB12/GB13: Regional_Indicator rules
    if (prev == N00B_UNICODE_GCB_REGIONAL_INDICATOR
        && cur == N00B_UNICODE_GCB_REGIONAL_INDICATOR) {
        // Don't break between RI pairs (odd count means we're in a pair)
        return (it->ri_count % 2) == 0;
    }

    // GB999: Otherwise ÷
    return true;
}

// ---------------------------------------------------------------------------
// Grapheme cluster iterator
// ---------------------------------------------------------------------------

n00b_unicode_break_iter_t *
n00b_unicode_grapheme_iter_raw(const char *data, int64_t len)
{
    n00b_unicode_break_iter_t *it = n00b_alloc(n00b_unicode_break_iter_t);
    it->data                      = data;
    it->len                       = len;
    it->type                      = N00B_UNICODE_BREAK_GRAPHEME;
    return it;
}

n00b_unicode_break_iter_t *
n00b_unicode_grapheme_iter(n00b_string_t *s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator; // accepted for API surface
    return n00b_unicode_grapheme_iter_raw(s->data, s->u8_bytes);
}

// Forward: returns byte offset of NEXT grapheme boundary, or -1
int32_t
n00b_unicode_break_next(n00b_unicode_break_iter_t *it)
{
    assert(it);
    if (it->byte_pos >= (uint32_t)it->len)
        return -1;

    // Decode first codepoint
    int32_t cp = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &it->byte_pos);
    if (cp < 0)
        return -1;

    if (it->type == N00B_UNICODE_BREAK_GRAPHEME) {
        n00b_unicode_gcb_t prev_gcb   = get_gcb((n00b_codepoint_t)cp);
        bool               prev_is_ep = n00b_unicode_has_property((n00b_codepoint_t)cp,
                                                    N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC);

        if (prev_gcb == N00B_UNICODE_GCB_REGIONAL_INDICATOR)
            it->ri_count = 1;
        else
            it->ri_count = 0;

        it->after_zwj     = (prev_gcb == N00B_UNICODE_GCB_ZWJ);
        // GB11: track if ExtPic was seen before current ZWJ
        it->ep_before_zwj = false;
        if (prev_is_ep) {
            it->ep_before_zwj = true; // for if next char is ZWJ
        }

        // Track InCB state
        it->incb_consonant_base = (prev_gcb == N00B_UNICODE_GCB_INCB_CONSONANT);
        it->incb_linker_seen    = false;

        while (it->byte_pos < (uint32_t)it->len) {
            uint32_t save_pos = it->byte_pos;
            int32_t  next_cp
                = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &it->byte_pos);
            if (next_cp < 0)
                break;

            n00b_unicode_gcb_t cur_gcb   = get_gcb((n00b_codepoint_t)next_cp);
            bool               cur_is_ep = n00b_unicode_has_property((n00b_codepoint_t)next_cp,
                                                       N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC);

            // GB11: \p{ExtPic} Extend* ZWJ × \p{ExtPic}
            if (it->after_zwj && it->ep_before_zwj && cur_is_ep) {
                prev_gcb          = cur_gcb;
                it->after_zwj     = false;
                it->ep_before_zwj = true; // cur IS ExtPic
                it->ri_count      = 0;
                if (cur_gcb == N00B_UNICODE_GCB_INCB_LINKER)
                    it->incb_linker_seen = true;
                continue;
            }

            if (grapheme_is_break(it, prev_gcb, cur_gcb)) {
                it->byte_pos = save_pos;
                return (int32_t)it->byte_pos;
            }

            // Update state
            if (cur_gcb == N00B_UNICODE_GCB_REGIONAL_INDICATOR) {
                it->ri_count++;
            }
            else {
                it->ri_count = 0;
            }

            // GB11 state tracking
            if (cur_is_ep) {
                it->ep_before_zwj = true;
            }
            else if (cur_gcb == N00B_UNICODE_GCB_EXTEND || cur_gcb == N00B_UNICODE_GCB_ZWJ) {
                // Extend and ZWJ are transparent for EP tracking
            }
            else {
                it->ep_before_zwj = false;
            }
            it->after_zwj = (cur_gcb == N00B_UNICODE_GCB_ZWJ);

            // InCB tracking: GB9c requires Consonant...Linker...×Consonant
            if (cur_gcb == N00B_UNICODE_GCB_INCB_LINKER) {
                it->incb_linker_seen = true;
            }
            else if (cur_gcb == N00B_UNICODE_GCB_INCB_CONSONANT) {
                // After GB9c consumed it, reset for next potential conjunct
                it->incb_consonant_base = true;
                it->incb_linker_seen    = false;
            }
            else if (cur_gcb != N00B_UNICODE_GCB_EXTEND && cur_gcb != N00B_UNICODE_GCB_ZWJ) {
                it->incb_consonant_base = false;
                it->incb_linker_seen    = false;
            }

            prev_gcb = cur_gcb;
        }
    }

    else if (it->type == N00B_UNICODE_BREAK_WORD) {
        n00b_unicode_wb_t prev_wb = get_wb((n00b_codepoint_t)cp);
        // Initialize lookback tracking for WB7/WB11
        it->prev_prev_wb          = N00B_UNICODE_WB_OTHER;
        it->prev_wb               = prev_wb;

        // Track RI count for WB15/16
        uint32_t ri_count = (prev_wb == N00B_UNICODE_WB_REGIONAL_INDICATOR) ? 1 : 0;
        bool     prev_zwj = (prev_wb == N00B_UNICODE_WB_ZWJ); // Track ZWJ for WB3c

        while (it->byte_pos < (uint32_t)it->len) {
            uint32_t save_pos = it->byte_pos;
            int32_t  next_cp
                = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &it->byte_pos);
            if (next_cp < 0)
                break;

            n00b_unicode_wb_t cur_wb = get_wb((n00b_codepoint_t)next_cp);

            // WB3: CR × LF
            if (prev_wb == N00B_UNICODE_WB_CR && cur_wb == N00B_UNICODE_WB_LF) {
                prev_wb  = cur_wb;
                prev_zwj = false;
                ri_count = 0;
                continue;
            }

            // WB3a: (Newline | CR | LF) ÷
            if (prev_wb == N00B_UNICODE_WB_NEWLINE || prev_wb == N00B_UNICODE_WB_CR
                || prev_wb == N00B_UNICODE_WB_LF) {
                it->byte_pos = save_pos;
                return (int32_t)it->byte_pos;
            }

            // WB3b: ÷ (Newline | CR | LF)
            if (cur_wb == N00B_UNICODE_WB_NEWLINE || cur_wb == N00B_UNICODE_WB_CR
                || cur_wb == N00B_UNICODE_WB_LF) {
                it->byte_pos = save_pos;
                return (int32_t)it->byte_pos;
            }

            // WB3c: ZWJ × \p{Extended_Pictographic}
            if (prev_zwj
                && n00b_unicode_has_property((n00b_codepoint_t)next_cp,
                                             N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC)) {
                prev_wb  = cur_wb;
                prev_zwj = false;
                ri_count = 0;
                continue;
            }

            // WB3d: WSegSpace × WSegSpace
            if (prev_wb == N00B_UNICODE_WB_WSEGSPACE && cur_wb == N00B_UNICODE_WB_WSEGSPACE) {
                prev_wb  = cur_wb;
                prev_zwj = false;
                continue;
            }

            // WB4: X (Extend | Format | ZWJ)* → X
            // Extend/Format/ZWJ are transparent: don't update prev_wb
            if (cur_wb == N00B_UNICODE_WB_EXTEND || cur_wb == N00B_UNICODE_WB_FORMAT
                || cur_wb == N00B_UNICODE_WB_ZWJ) {
                // WB4 clarification: Extend/ZWJ after WSegSpace means the
                // combined sequence is no longer WSegSpace for WB3d
                if (prev_wb == N00B_UNICODE_WB_WSEGSPACE && cur_wb != N00B_UNICODE_WB_FORMAT) {
                    prev_wb = N00B_UNICODE_WB_OTHER;
                }
                // Track ZWJ for WB3c
                prev_zwj = (cur_wb == N00B_UNICODE_WB_ZWJ);
                continue;
            }

            // If we get here, cur is not Extend/Format/ZWJ, so reset ZWJ tracking
            prev_zwj = false;

            bool prev_ahletter = (prev_wb == N00B_UNICODE_WB_ALETTER
                                  || prev_wb == N00B_UNICODE_WB_HEBREW_LETTER);
            bool cur_ahletter  = (cur_wb == N00B_UNICODE_WB_ALETTER
                                 || cur_wb == N00B_UNICODE_WB_HEBREW_LETTER);
            bool cur_midletter
                = (cur_wb == N00B_UNICODE_WB_MIDLETTER || cur_wb == N00B_UNICODE_WB_MIDNUMLET
                   || cur_wb == N00B_UNICODE_WB_SINGLE_QUOTE);
            bool cur_midnum
                = (cur_wb == N00B_UNICODE_WB_MIDNUM || cur_wb == N00B_UNICODE_WB_MIDNUMLET
                   || cur_wb == N00B_UNICODE_WB_SINGLE_QUOTE);

            // WB5: AHLetter × AHLetter
            if (prev_ahletter && cur_ahletter) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB6: AHLetter × (MidLetter|MidNumLetQ) AHLetter (lookahead)
            if (prev_ahletter && cur_midletter) {
                // Look ahead past Extend/Format/ZWJ to see if AHLetter follows
                uint32_t la_pos = it->byte_pos;
                while (la_pos < (uint32_t)it->len) {
                    int32_t la_cp
                        = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &la_pos);
                    if (la_cp < 0)
                        break;
                    n00b_unicode_wb_t la_wb = get_wb((n00b_codepoint_t)la_cp);
                    if (la_wb == N00B_UNICODE_WB_EXTEND || la_wb == N00B_UNICODE_WB_FORMAT
                        || la_wb == N00B_UNICODE_WB_ZWJ)
                        continue;
                    if (la_wb == N00B_UNICODE_WB_ALETTER
                        || la_wb == N00B_UNICODE_WB_HEBREW_LETTER) {
                        // WB6 matches: don't break
                        goto wb_no_break;
                    }
                    break;
                }
            }

            // WB7: AHLetter (MidLetter|MidNumLetQ) × AHLetter (look back)
            if (cur_ahletter && it->prev_wb != N00B_UNICODE_WB_OTHER) {
                bool pmid = (it->prev_wb == N00B_UNICODE_WB_MIDLETTER
                             || it->prev_wb == N00B_UNICODE_WB_MIDNUMLET
                             || it->prev_wb == N00B_UNICODE_WB_SINGLE_QUOTE);
                bool ppah = (it->prev_prev_wb == N00B_UNICODE_WB_ALETTER
                             || it->prev_prev_wb == N00B_UNICODE_WB_HEBREW_LETTER);
                if (pmid && ppah) {
                    prev_wb = cur_wb;

                    ri_count         = 0;
                    it->prev_prev_wb = it->prev_wb;
                    it->prev_wb      = cur_wb;
                    continue;
                }
            }

            // WB7a: Hebrew_Letter × Single_Quote
            if (prev_wb == N00B_UNICODE_WB_HEBREW_LETTER
                && cur_wb == N00B_UNICODE_WB_SINGLE_QUOTE) {
                // Don't break, but don't update prev_wb to preserve Hebrew context
                it->prev_prev_wb = it->prev_wb;
                it->prev_wb      = cur_wb;
                ri_count         = 0;
                continue;
            }

            // WB7b: Hebrew_Letter × Double_Quote Hebrew_Letter (lookahead)
            if (prev_wb == N00B_UNICODE_WB_HEBREW_LETTER
                && cur_wb == N00B_UNICODE_WB_DOUBLE_QUOTE) {
                uint32_t la_pos = it->byte_pos;
                while (la_pos < (uint32_t)it->len) {
                    int32_t la_cp
                        = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &la_pos);
                    if (la_cp < 0)
                        break;
                    n00b_unicode_wb_t la_wb = get_wb((n00b_codepoint_t)la_cp);
                    if (la_wb == N00B_UNICODE_WB_EXTEND || la_wb == N00B_UNICODE_WB_FORMAT
                        || la_wb == N00B_UNICODE_WB_ZWJ)
                        continue;
                    if (la_wb == N00B_UNICODE_WB_HEBREW_LETTER)
                        goto wb_no_break;
                    break;
                }
            }

            // WB7c: Hebrew_Letter Double_Quote × Hebrew_Letter (look back)
            if (cur_wb == N00B_UNICODE_WB_HEBREW_LETTER
                && it->prev_wb == N00B_UNICODE_WB_DOUBLE_QUOTE
                && it->prev_prev_wb == N00B_UNICODE_WB_HEBREW_LETTER) {
                prev_wb = cur_wb;

                ri_count         = 0;
                it->prev_prev_wb = it->prev_wb;
                it->prev_wb      = cur_wb;
                continue;
            }

            // WB8: Numeric × Numeric
            if (prev_wb == N00B_UNICODE_WB_NUMERIC && cur_wb == N00B_UNICODE_WB_NUMERIC) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB9: AHLetter × Numeric
            if (prev_ahletter && cur_wb == N00B_UNICODE_WB_NUMERIC) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB10: Numeric × AHLetter
            if (prev_wb == N00B_UNICODE_WB_NUMERIC && cur_ahletter) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB11: Numeric (MidNum|MidNumLetQ) × Numeric (look back)
            if (cur_wb == N00B_UNICODE_WB_NUMERIC) {
                bool pmn = (it->prev_wb == N00B_UNICODE_WB_MIDNUM
                            || it->prev_wb == N00B_UNICODE_WB_MIDNUMLET
                            || it->prev_wb == N00B_UNICODE_WB_SINGLE_QUOTE);
                if (pmn && it->prev_prev_wb == N00B_UNICODE_WB_NUMERIC) {
                    prev_wb = cur_wb;

                    ri_count         = 0;
                    it->prev_prev_wb = it->prev_wb;
                    it->prev_wb      = cur_wb;
                    continue;
                }
            }

            // WB12: Numeric × (MidNum|MidNumLetQ) Numeric (lookahead)
            if (prev_wb == N00B_UNICODE_WB_NUMERIC && cur_midnum) {
                uint32_t la_pos = it->byte_pos;
                while (la_pos < (uint32_t)it->len) {
                    int32_t la_cp
                        = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &la_pos);
                    if (la_cp < 0)
                        break;
                    n00b_unicode_wb_t la_wb = get_wb((n00b_codepoint_t)la_cp);
                    if (la_wb == N00B_UNICODE_WB_EXTEND || la_wb == N00B_UNICODE_WB_FORMAT
                        || la_wb == N00B_UNICODE_WB_ZWJ)
                        continue;
                    if (la_wb == N00B_UNICODE_WB_NUMERIC)
                        goto wb_no_break;
                    break;
                }
            }

            // WB13: Katakana × Katakana
            if (prev_wb == N00B_UNICODE_WB_KATAKANA && cur_wb == N00B_UNICODE_WB_KATAKANA) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB13a: (AHLetter | Numeric | Katakana | ExtendNumLet) × ExtendNumLet
            if ((prev_ahletter || prev_wb == N00B_UNICODE_WB_NUMERIC
                 || prev_wb == N00B_UNICODE_WB_KATAKANA
                 || prev_wb == N00B_UNICODE_WB_EXTENDNUMLET)
                && cur_wb == N00B_UNICODE_WB_EXTENDNUMLET) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB13b: ExtendNumLet × (AHLetter | Numeric | Katakana)
            if (prev_wb == N00B_UNICODE_WB_EXTENDNUMLET
                && (cur_ahletter || cur_wb == N00B_UNICODE_WB_NUMERIC
                    || cur_wb == N00B_UNICODE_WB_KATAKANA)) {
                prev_wb = cur_wb;

                ri_count = 0;
                continue;
            }

            // WB15/16: RI × RI (odd count = pair)
            if (prev_wb == N00B_UNICODE_WB_REGIONAL_INDICATOR
                && cur_wb == N00B_UNICODE_WB_REGIONAL_INDICATOR) {
                if (ri_count % 2 == 1) {
                    ri_count++;
                    prev_wb = cur_wb;

                    continue;
                }
            }

            // WB999: Otherwise ÷
            it->byte_pos     = save_pos;
            it->prev_prev_wb = it->prev_wb;
            it->prev_wb      = prev_wb;
            return (int32_t)it->byte_pos;

wb_no_break:
            it->prev_prev_wb = it->prev_wb;
            it->prev_wb      = cur_wb;
            prev_wb          = cur_wb;
            if (cur_wb == N00B_UNICODE_WB_REGIONAL_INDICATOR)
                ri_count++;
            else
                ri_count = 0;
            continue;
        }
    }

    else if (it->type == N00B_UNICODE_BREAK_SENTENCE) {
        n00b_unicode_sb_t prev_sb = get_sb((n00b_codepoint_t)cp);

        // Initialize term tracking for first codepoint
        it->sb_term_phase = 0;
        it->prev_prev_sb  = N00B_UNICODE_SB_OTHER;
        if (prev_sb == N00B_UNICODE_SB_ATERM) {
            it->sb_term_phase = 1;
            it->sb_is_aterm   = true;
        }
        else if (prev_sb == N00B_UNICODE_SB_STERM) {
            it->sb_term_phase = 1;
            it->sb_is_aterm   = false;
        }

        while (it->byte_pos < (uint32_t)it->len) {
            uint32_t save_pos = it->byte_pos;
            int32_t  next_cp
                = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &it->byte_pos);
            if (next_cp < 0)
                break;

            n00b_unicode_sb_t cur_sb = get_sb((n00b_codepoint_t)next_cp);

            // SB3: CR × LF
            if (prev_sb == N00B_UNICODE_SB_CR && cur_sb == N00B_UNICODE_SB_LF) {
                // After CR LF in term context, advance phase to "seen ParaSep"
                if (it->sb_term_phase >= 1 && it->sb_term_phase <= 3)
                    it->sb_term_phase = 4;
                prev_sb = cur_sb;
                continue;
            }

            // SB4: Sep | CR | LF ÷
            if (prev_sb == N00B_UNICODE_SB_SEP || prev_sb == N00B_UNICODE_SB_CR
                || prev_sb == N00B_UNICODE_SB_LF) {
                // If in term context, SB4 fires the break (SB11 condition met)
                it->sb_term_phase = 0;
                it->byte_pos      = save_pos;
                return (int32_t)it->byte_pos;
            }

            // SB5: × (Format | Extend) — transparent, skip
            if (cur_sb == N00B_UNICODE_SB_FORMAT || cur_sb == N00B_UNICODE_SB_EXTEND) {
                continue;
            }

            bool in_term = (it->sb_term_phase >= 1);

            // SB6: ATerm × Numeric
            if (in_term && it->sb_is_aterm && it->sb_term_phase == 1
                && cur_sb == N00B_UNICODE_SB_NUMERIC) {
                it->sb_term_phase = 0;
                it->prev_prev_sb  = prev_sb;
                prev_sb           = cur_sb;
                continue;
            }

            // SB7: (Upper|Lower) ATerm × Upper
            if (in_term && it->sb_is_aterm && it->sb_term_phase == 1
                && cur_sb == N00B_UNICODE_SB_UPPER
                && (it->prev_prev_sb == N00B_UNICODE_SB_UPPER
                    || it->prev_prev_sb == N00B_UNICODE_SB_LOWER)) {
                it->sb_term_phase = 0;
                it->prev_prev_sb  = prev_sb;
                prev_sb           = cur_sb;
                continue;
            }

            // SB8a: (STerm|ATerm) Close* Sp* × (SContinue|STerm|ATerm)
            // Applies in phases 1-3 (before ParaSep)
            if (in_term && it->sb_term_phase <= 3
                && (cur_sb == N00B_UNICODE_SB_SCONTINUE || cur_sb == N00B_UNICODE_SB_ATERM
                    || cur_sb == N00B_UNICODE_SB_STERM)) {
                // Reset term context based on current
                if (cur_sb == N00B_UNICODE_SB_ATERM) {
                    it->sb_term_phase = 1;
                    it->sb_is_aterm   = true;
                }
                else if (cur_sb == N00B_UNICODE_SB_STERM) {
                    it->sb_term_phase = 1;
                    it->sb_is_aterm   = false;
                }
                else {
                    it->sb_term_phase = 0;
                }
                it->prev_prev_sb = prev_sb;
                prev_sb          = cur_sb;
                continue;
            }

            // SB8: ATerm Close* Sp* × ( ¬(OLetter|Upper|Lower|Sep|CR|LF|STerm|ATerm)
            // )* Lower Only applies to ATerm context, phases 1-3
            if (in_term && it->sb_is_aterm && it->sb_term_phase <= 3) {
                // Check: is cur_sb Lower? If so, don't break.
                if (cur_sb == N00B_UNICODE_SB_LOWER) {
                    it->sb_term_phase = 0;
                    it->prev_prev_sb  = prev_sb;
                    prev_sb           = cur_sb;
                    continue;
                }
                // Check: is cur_sb one of the "blocking" classes that
                // prevent SB8 from looking further?
                bool sb8_blocker
                    = (cur_sb == N00B_UNICODE_SB_OLETTER || cur_sb == N00B_UNICODE_SB_UPPER
                       || cur_sb == N00B_UNICODE_SB_SEP || cur_sb == N00B_UNICODE_SB_CR
                       || cur_sb == N00B_UNICODE_SB_LF || cur_sb == N00B_UNICODE_SB_STERM
                       || cur_sb == N00B_UNICODE_SB_ATERM);
                if (!sb8_blocker) {
                    // Scan forward (past Extend/Format) looking for Lower
                    // before hitting a blocker
                    uint32_t la_pos      = it->byte_pos;
                    bool     found_lower = false;
                    while (la_pos < (uint32_t)it->len) {
                        int32_t la_cp
                            = n00b_unicode_utf8_decode(it->data, (uint32_t)it->len, &la_pos);
                        if (la_cp < 0)
                            break;
                        n00b_unicode_sb_t la_sb = get_sb((n00b_codepoint_t)la_cp);
                        if (la_sb == N00B_UNICODE_SB_FORMAT || la_sb == N00B_UNICODE_SB_EXTEND)
                            continue;
                        if (la_sb == N00B_UNICODE_SB_LOWER) {
                            found_lower = true;
                            break;
                        }
                        if (la_sb == N00B_UNICODE_SB_OLETTER || la_sb == N00B_UNICODE_SB_UPPER
                            || la_sb == N00B_UNICODE_SB_SEP || la_sb == N00B_UNICODE_SB_CR
                            || la_sb == N00B_UNICODE_SB_LF || la_sb == N00B_UNICODE_SB_STERM
                            || la_sb == N00B_UNICODE_SB_ATERM)
                            break;
                        // Other classes (Numeric, Close, Sp, etc.) are transparent for SB8
                    }
                    if (found_lower) {
                        // SB8: don't break — stay in term context
                        // Don't reset phase, just continue
                        it->prev_prev_sb = prev_sb;
                        prev_sb          = cur_sb;
                        continue;
                    }
                }
            }

            // SB9: (STerm|ATerm) Close* × (Close | Sp | Sep | CR | LF)
            // Phase 1 or 2 (haven't seen Sp yet)
            if (in_term && it->sb_term_phase <= 2) {
                if (cur_sb == N00B_UNICODE_SB_CLOSE) {
                    it->sb_term_phase = 2;
                    it->prev_prev_sb  = prev_sb;
                    prev_sb           = cur_sb;
                    continue;
                }
                if (cur_sb == N00B_UNICODE_SB_SP || cur_sb == N00B_UNICODE_SB_SEP
                    || cur_sb == N00B_UNICODE_SB_CR || cur_sb == N00B_UNICODE_SB_LF) {
                    // SB9 matches for Sep/CR/LF too
                    if (cur_sb == N00B_UNICODE_SB_SP)
                        it->sb_term_phase = 3;
                    else
                        it->sb_term_phase = 4; // ParaSep seen
                    it->prev_prev_sb = prev_sb;
                    prev_sb          = cur_sb;
                    continue;
                }
            }

            // SB10: (STerm|ATerm) Close* Sp* × (Sp | Sep | CR | LF)
            // Phase 3 (in Sp*)
            if (in_term && it->sb_term_phase == 3) {
                if (cur_sb == N00B_UNICODE_SB_SP) {
                    it->prev_prev_sb = prev_sb;
                    prev_sb          = cur_sb;
                    continue;
                }
                if (cur_sb == N00B_UNICODE_SB_SEP || cur_sb == N00B_UNICODE_SB_CR
                    || cur_sb == N00B_UNICODE_SB_LF) {
                    it->sb_term_phase = 4;
                    it->prev_prev_sb  = prev_sb;
                    prev_sb           = cur_sb;
                    continue;
                }
            }

            // SB11: (STerm|ATerm) Close* Sp* (ParaSep)? ÷
            // If we're still in term context and none of the above matched,
            // then break here
            if (in_term) {
                it->sb_term_phase = 0;
                it->byte_pos      = save_pos;
                return (int32_t)it->byte_pos;
            }

            // Update term tracking for new ATerm/STerm
            if (cur_sb == N00B_UNICODE_SB_ATERM) {
                it->sb_term_phase = 1;
                it->sb_is_aterm   = true;
            }
            else if (cur_sb == N00B_UNICODE_SB_STERM) {
                it->sb_term_phase = 1;
                it->sb_is_aterm   = false;
            }

            // SB998: Otherwise, do not break
            it->prev_prev_sb = prev_sb;
            prev_sb          = cur_sb;
        }
    }

    // End of string is a boundary
    return (int32_t)it->byte_pos;
}

// Reverse iterator (simplified: re-scan from beginning)
int32_t
n00b_unicode_break_prev(n00b_unicode_break_iter_t *it)
{
    assert(it);
    if (it->byte_pos == 0)
        return -1;

    // Find the boundary before the current position by scanning forward
    uint32_t target        = it->byte_pos;
    uint32_t last_boundary = 0;

    // Create a temp iterator
    n00b_unicode_break_iter_t tmp = *it;
    tmp.byte_pos                  = 0;
    tmp.ri_count                  = 0;
    tmp.after_zwj                 = false;
    tmp.incb_linker_seen          = false;

    int32_t b;
    while ((b = n00b_unicode_break_next(&tmp)) >= 0 && (uint32_t)b < target) {
        last_boundary = (uint32_t)b;
    }

    it->byte_pos = last_boundary;
    return (int32_t)last_boundary;
}

void
n00b_unicode_break_iter_free(n00b_unicode_break_iter_t *it)
{
    n00b_free(it);
}

// ---------------------------------------------------------------------------
// Word + sentence iterators
// ---------------------------------------------------------------------------

n00b_unicode_break_iter_t *
n00b_unicode_word_iter_raw(const char *data, int64_t len)
{
    n00b_unicode_break_iter_t *it = n00b_alloc(n00b_unicode_break_iter_t);
    it->data                      = data;
    it->len                       = len;
    it->type                      = N00B_UNICODE_BREAK_WORD;
    return it;
}

n00b_unicode_break_iter_t *
n00b_unicode_word_iter(n00b_string_t *s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator; // accepted for API surface
    return n00b_unicode_word_iter_raw(s->data, s->u8_bytes);
}

n00b_unicode_break_iter_t *
n00b_unicode_sentence_iter_raw(const char *data, int64_t len)
{
    n00b_unicode_break_iter_t *it = n00b_alloc(n00b_unicode_break_iter_t);
    it->data                      = data;
    it->len                       = len;
    it->type                      = N00B_UNICODE_BREAK_SENTENCE;
    return it;
}

n00b_unicode_break_iter_t *
n00b_unicode_sentence_iter(n00b_string_t *s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator; // accepted for API surface
    return n00b_unicode_sentence_iter_raw(s->data, s->u8_bytes);
}

// ---------------------------------------------------------------------------
// Convenience
// ---------------------------------------------------------------------------

uint32_t
n00b_unicode_grapheme_count_raw(const char *data, int64_t len)
{
    n00b_unicode_break_iter_t *it    = n00b_unicode_grapheme_iter_raw(data, len);
    uint32_t                   count = 0;

    while (n00b_unicode_break_next(it) >= 0) {
        count++;
    }

    n00b_unicode_break_iter_free(it);
    return count;
}

uint32_t
n00b_unicode_grapheme_count(n00b_string_t *s)
{
    return n00b_unicode_grapheme_count_raw(s->data, s->u8_bytes);
}

// ===========================================================================
// Range enumeration: turn per-codepoint break property tables into sorted,
// merged range arrays.
//
// Strategy: do a single 0..0x10FFFF sweep per property class on first call,
// behind a CAS-based one-shot init guard. Two passes per class: pass 1
// counts how many merged ranges each enum value will produce; pass 2 fills
// a single shared backing array, slicing it per value. The surrogate hole
// (U+D800..U+DFFF) is excluded; U+D7FF and U+E000 are merged across the hole
// to mirror the canonical UCD range presentation.
//
// Init pattern: an `_Atomic int` state machine with three states
//   0 = uninit, 1 = init in progress, 2 = ready.
// One thread CASes 0->1 and runs the build; others spin on state == 2.
// (Pure atomic CAS rather than `n00b_mutex_t`, because n00b mutexes have no
// static initializer and would themselves require a lazy-init dance; the
// data here is read-only after publication, so CAS leader-wait is sufficient.)
// ===========================================================================

// Range slice type is the canonical `n00b_unicode_range_slice_t` from
// text/unicode/ctx.h; per-class slice arrays live on the unicode ctx.

// Lookup a Line_Break value by codepoint, mirroring the per-class getters
// already defined above for gcb/wb/sb. Kept local; the public getter
// `n00b_unicode_line_break()` is not part of segmentation.h's surface
// (Phase 4.5c owns query.h-side per-codepoint accessors).
static inline n00b_unicode_lb_t
get_lb(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_LB_XX;
    return (n00b_unicode_lb_t)N00B_UNICODE_LOOKUP(n00b_unicode_lb_stage1,
                                                  n00b_unicode_lb_stage2,
                                                  cp);
}

// One-shot init helpers ----------------------------------------------------

// Spin until @p state reaches 2. Used by losers of the CAS race.
static inline void
wait_for_init(_Atomic int *state)
{
    while (n00b_atomic_load(state) != 2) {
        // Brief spin; init is bounded (single sweep over 0..0x10FFFF).
    }
}

// Try to claim the init slot. Returns true if this thread should run init,
// false if another thread already finished it (or is finishing it).
static inline bool
claim_init(_Atomic int *state)
{
    int expected = 0;
    if (n00b_atomic_cas(state, &expected, 1)) {
        return true;
    }
    wait_for_init(state);
    return false;
}

// Publish "init done" so spinners can proceed.
static inline void
publish_init(_Atomic int *state)
{
    n00b_atomic_store(state, 2);
}

// ---------------------------------------------------------------------------
// Generic two-pass build: pass 1 counts merged ranges per value; pass 2
// fills a single backing array and records per-value slices.
//
// `get` is a callback returning the enum value for a codepoint, but we
// inline-specialize via macros below to avoid an indirect call per cp.
// ---------------------------------------------------------------------------

#define DEFINE_BREAK_RANGES(NAME, ENUM_T, GETTER, N_VALUES_EXPR)                             \
    enum { NAME##_n_values = (N_VALUES_EXPR) };                                              \
                                                                                             \
    static void                                                                              \
    NAME##_build(void)                                                                       \
    {                                                                                        \
        n00b_unicode_ctx_t *ctx = _uctx();                                                   \
        size_t           counts[NAME##_n_values]    = {};                                    \
        bool             have_prev[NAME##_n_values] = {};                                    \
        n00b_codepoint_t prev_hi[NAME##_n_values]   = {};                                    \
                                                                                             \
        /* Pass 1: count merged ranges per value. */                                         \
        for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {                               \
            if (cp >= 0xD800u && cp <= 0xDFFFu)                                              \
                continue;                                                                    \
            ENUM_T v = GETTER(cp);                                                           \
            if ((unsigned)v >= (unsigned)NAME##_n_values)                                    \
                continue;                                                                    \
            if (have_prev[(unsigned)v]                                                       \
                && (cp == prev_hi[(unsigned)v] + 1                                           \
                    || (cp == 0xE000u && prev_hi[(unsigned)v] == 0xD7FFu))) {                \
                prev_hi[(unsigned)v] = cp;                                                   \
            }                                                                                \
            else {                                                                           \
                counts[(unsigned)v]++;                                                       \
                prev_hi[(unsigned)v]   = cp;                                                 \
                have_prev[(unsigned)v] = true;                                               \
            }                                                                                \
        }                                                                                    \
                                                                                             \
        size_t total = 0;                                                                    \
        for (size_t i = 0; i < NAME##_n_values; i++)                                         \
            total += counts[i];                                                              \
                                                                                             \
        n00b_codepoint_pair_t *backing = nullptr;                                            \
        if (total > 0) {                                                                     \
            backing = n00b_alloc_array(n00b_codepoint_pair_t, total);                        \
        }                                                                                    \
                                                                                             \
        /* Allocate per-class slice array on the unicode ctx. */                             \
        n00b_unicode_range_slice_t *slices                                                   \
            = n00b_alloc_array(n00b_unicode_range_slice_t, NAME##_n_values);                 \
                                                                                             \
        /* Slice the backing array, one slice per value. */                                  \
        size_t off = 0;                                                                      \
        size_t write_pos[NAME##_n_values];                                                   \
        for (size_t i = 0; i < NAME##_n_values; i++) {                                       \
            slices[i].ranges = backing ? backing + off : nullptr;                            \
            slices[i].len    = counts[i];                                                    \
            write_pos[i]     = off;                                                          \
            off += counts[i];                                                                \
        }                                                                                    \
                                                                                             \
        /* Pass 2: fill the backing array. */                                                \
        for (size_t i = 0; i < NAME##_n_values; i++)                                         \
            have_prev[i] = false;                                                            \
                                                                                             \
        for (n00b_codepoint_t cp = 0; cp <= 0x10FFFFu; cp++) {                               \
            if (cp >= 0xD800u && cp <= 0xDFFFu)                                              \
                continue;                                                                    \
            ENUM_T v = GETTER(cp);                                                           \
            if ((unsigned)v >= (unsigned)NAME##_n_values)                                    \
                continue;                                                                    \
            unsigned vi = (unsigned)v;                                                       \
            if (have_prev[vi]) {                                                             \
                n00b_codepoint_pair_t *last = backing + write_pos[vi] - 1;                   \
                if (cp == last->hi + 1                                                       \
                    || (cp == 0xE000u && last->hi == 0xD7FFu)) {                             \
                    last->hi = cp;                                                           \
                    continue;                                                                \
                }                                                                            \
            }                                                                                \
            backing[write_pos[vi]].lo = cp;                                                  \
            backing[write_pos[vi]].hi = cp;                                                  \
            write_pos[vi]++;                                                                 \
            have_prev[vi] = true;                                                            \
        }                                                                                    \
                                                                                             \
        ctx->NAME##_backing = backing;                                                       \
        ctx->NAME##_slices  = slices;                                                        \
    }                                                                                        \
                                                                                             \
    static inline void                                                                       \
    NAME##_ensure_init(void)                                                                 \
    {                                                                                        \
        n00b_unicode_ctx_t *ctx = _uctx();                                                   \
        if (n00b_atomic_load(&ctx->NAME##_state) == 2)                                       \
            return;                                                                          \
        if (claim_init(&ctx->NAME##_state)) {                                                \
            NAME##_build();                                                                  \
            publish_init(&ctx->NAME##_state);                                                \
        }                                                                                    \
    }

DEFINE_BREAK_RANGES(gcb,
                    n00b_unicode_gcb_t,
                    get_gcb,
                    N00B_UNICODE_GCB_INCB_LINKER + 1)

DEFINE_BREAK_RANGES(wb,
                    n00b_unicode_wb_t,
                    get_wb,
                    N00B_UNICODE_WB_WSEGSPACE + 1)

DEFINE_BREAK_RANGES(sb,
                    n00b_unicode_sb_t,
                    get_sb,
                    N00B_UNICODE_SB_SCONTINUE + 1)

DEFINE_BREAK_RANGES(lb,
                    n00b_unicode_lb_t,
                    get_lb,
                    N00B_UNICODE_LB_VI + 1)

void
n00b_unicode_grapheme_break_ranges(n00b_unicode_gcb_t            v,
                                   const n00b_codepoint_pair_t **out,
                                   size_t                       *len)
{
    if ((unsigned)v >= (unsigned)gcb_n_values) {
        *out = nullptr;
        *len = 0;
        return;
    }
    gcb_ensure_init();
    n00b_unicode_ctx_t *ctx = _uctx();
    *out = ctx->gcb_slices[(unsigned)v].ranges;
    *len = ctx->gcb_slices[(unsigned)v].len;
}

void
n00b_unicode_word_break_ranges(n00b_unicode_wb_t             v,
                               const n00b_codepoint_pair_t **out,
                               size_t                       *len)
{
    if ((unsigned)v >= (unsigned)wb_n_values) {
        *out = nullptr;
        *len = 0;
        return;
    }
    wb_ensure_init();
    n00b_unicode_ctx_t *ctx = _uctx();
    *out = ctx->wb_slices[(unsigned)v].ranges;
    *len = ctx->wb_slices[(unsigned)v].len;
}

void
n00b_unicode_sentence_break_ranges(n00b_unicode_sb_t             v,
                                   const n00b_codepoint_pair_t **out,
                                   size_t                       *len)
{
    if ((unsigned)v >= (unsigned)sb_n_values) {
        *out = nullptr;
        *len = 0;
        return;
    }
    sb_ensure_init();
    n00b_unicode_ctx_t *ctx = _uctx();
    *out = ctx->sb_slices[(unsigned)v].ranges;
    *len = ctx->sb_slices[(unsigned)v].len;
}

void
n00b_unicode_line_break_ranges(n00b_unicode_lb_t             v,
                               const n00b_codepoint_pair_t **out,
                               size_t                       *len)
{
    if ((unsigned)v >= (unsigned)lb_n_values) {
        *out = nullptr;
        *len = 0;
        return;
    }
    lb_ensure_init();
    n00b_unicode_ctx_t *ctx = _uctx();
    *out = ctx->lb_slices[(unsigned)v].ranges;
    *len = ctx->lb_slices[(unsigned)v].len;
}

// ===========================================================================
// Property-name -> enum lookups for segmentation properties
//
// Used by regex \p{...} resolution. Loose matching per UAX #44 LM3:
// case-insensitive (ASCII fold) and ignoring whitespace, underscores, and
// hyphens. Both abbreviation and long forms are accepted, per
// PropertyValueAliases.txt.
// ===========================================================================

static inline unsigned char
_seg_ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool
loose_eq(const char *a, const char *b)
{
    while (*a && *b) {
        while (*a == ' ' || *a == '_' || *a == '-') {
            a++;
        }
        while (*b == ' ' || *b == '_' || *b == '-') {
            b++;
        }
        if (!*a || !*b) {
            break;
        }
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (_seg_ascii_lower(ca) != _seg_ascii_lower(cb)) {
            return false;
        }
    }
    while (*a == ' ' || *a == '_' || *a == '-') {
        a++;
    }
    while (*b == ' ' || *b == '_' || *b == '-') {
        b++;
    }
    return *a == 0 && *b == 0;
}

// ---------------------------------------------------------------------------
// Grapheme_Cluster_Break names. Both abbreviation and long forms per
// PropertyValueAliases.txt.
// ---------------------------------------------------------------------------

typedef struct {
    const char        *name;
    n00b_unicode_gcb_t value;
} _gcb_name_t;

static const _gcb_name_t _gcb_names[] = {
    {"Other",              N00B_UNICODE_GCB_OTHER},  {"XX", N00B_UNICODE_GCB_OTHER},
    {"CR",                 N00B_UNICODE_GCB_CR},
    {"LF",                 N00B_UNICODE_GCB_LF},
    {"Control",            N00B_UNICODE_GCB_CONTROL}, {"CN", N00B_UNICODE_GCB_CONTROL},
    {"Extend",             N00B_UNICODE_GCB_EXTEND},  {"EX", N00B_UNICODE_GCB_EXTEND},
    {"ZWJ",                N00B_UNICODE_GCB_ZWJ},
    {"Regional_Indicator", N00B_UNICODE_GCB_REGIONAL_INDICATOR},
    {"RI",                 N00B_UNICODE_GCB_REGIONAL_INDICATOR},
    {"Prepend",            N00B_UNICODE_GCB_PREPEND}, {"PP", N00B_UNICODE_GCB_PREPEND},
    {"SpacingMark",        N00B_UNICODE_GCB_SPACINGMARK},
    {"SM",                 N00B_UNICODE_GCB_SPACINGMARK},
    {"L",                  N00B_UNICODE_GCB_L},
    {"V",                  N00B_UNICODE_GCB_V},
    {"T",                  N00B_UNICODE_GCB_T},
    {"LV",                 N00B_UNICODE_GCB_LV},
    {"LVT",                N00B_UNICODE_GCB_LVT},
    // Indic_Conjunct_Break extension (Unicode 15.1+).
    {"InCB_Consonant",     N00B_UNICODE_GCB_INCB_CONSONANT},
    {"Consonant",          N00B_UNICODE_GCB_INCB_CONSONANT},
    {"InCB_Extend",        N00B_UNICODE_GCB_INCB_EXTEND},
    {"InCB_Linker",        N00B_UNICODE_GCB_INCB_LINKER},
    {"Linker",             N00B_UNICODE_GCB_INCB_LINKER},
};

bool
n00b_unicode_gcb_by_name(const char *name, n00b_unicode_gcb_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_gcb_names) / sizeof(_gcb_names[0]); i++) {
        if (loose_eq(_gcb_names[i].name, name)) {
            if (out) {
                *out = _gcb_names[i].value;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Word_Break names.
// ---------------------------------------------------------------------------

typedef struct {
    const char       *name;
    n00b_unicode_wb_t value;
} _wb_name_t;

static const _wb_name_t _wb_names[] = {
    {"Other",              N00B_UNICODE_WB_OTHER},  {"XX", N00B_UNICODE_WB_OTHER},
    {"CR",                 N00B_UNICODE_WB_CR},
    {"LF",                 N00B_UNICODE_WB_LF},
    {"Newline",            N00B_UNICODE_WB_NEWLINE}, {"NL", N00B_UNICODE_WB_NEWLINE},
    {"Extend",             N00B_UNICODE_WB_EXTEND},
    {"ZWJ",                N00B_UNICODE_WB_ZWJ},
    {"Regional_Indicator", N00B_UNICODE_WB_REGIONAL_INDICATOR},
    {"RI",                 N00B_UNICODE_WB_REGIONAL_INDICATOR},
    {"Format",             N00B_UNICODE_WB_FORMAT}, {"FO", N00B_UNICODE_WB_FORMAT},
    {"Katakana",           N00B_UNICODE_WB_KATAKANA},
    {"KA",                 N00B_UNICODE_WB_KATAKANA},
    {"Hebrew_Letter",      N00B_UNICODE_WB_HEBREW_LETTER},
    {"HL",                 N00B_UNICODE_WB_HEBREW_LETTER},
    {"ALetter",            N00B_UNICODE_WB_ALETTER},
    {"LE",                 N00B_UNICODE_WB_ALETTER},
    {"Single_Quote",       N00B_UNICODE_WB_SINGLE_QUOTE},
    {"SQ",                 N00B_UNICODE_WB_SINGLE_QUOTE},
    {"Double_Quote",       N00B_UNICODE_WB_DOUBLE_QUOTE},
    {"DQ",                 N00B_UNICODE_WB_DOUBLE_QUOTE},
    {"MidNumLet",          N00B_UNICODE_WB_MIDNUMLET},
    {"MB",                 N00B_UNICODE_WB_MIDNUMLET},
    {"MidLetter",          N00B_UNICODE_WB_MIDLETTER},
    {"ML",                 N00B_UNICODE_WB_MIDLETTER},
    {"MidNum",             N00B_UNICODE_WB_MIDNUM},
    {"MN",                 N00B_UNICODE_WB_MIDNUM},
    {"Numeric",            N00B_UNICODE_WB_NUMERIC},
    {"NU",                 N00B_UNICODE_WB_NUMERIC},
    {"ExtendNumLet",       N00B_UNICODE_WB_EXTENDNUMLET},
    {"EX",                 N00B_UNICODE_WB_EXTENDNUMLET},
    {"WSegSpace",          N00B_UNICODE_WB_WSEGSPACE},
};

bool
n00b_unicode_wb_by_name(const char *name, n00b_unicode_wb_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_wb_names) / sizeof(_wb_names[0]); i++) {
        if (loose_eq(_wb_names[i].name, name)) {
            if (out) {
                *out = _wb_names[i].value;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sentence_Break names.
// ---------------------------------------------------------------------------

typedef struct {
    const char       *name;
    n00b_unicode_sb_t value;
} _sb_name_t;

static const _sb_name_t _sb_names[] = {
    {"Other",     N00B_UNICODE_SB_OTHER},     {"XX", N00B_UNICODE_SB_OTHER},
    {"CR",        N00B_UNICODE_SB_CR},
    {"LF",        N00B_UNICODE_SB_LF},
    {"Extend",    N00B_UNICODE_SB_EXTEND},    {"EX", N00B_UNICODE_SB_EXTEND},
    {"Sep",       N00B_UNICODE_SB_SEP},       {"SE", N00B_UNICODE_SB_SEP},
    {"Format",    N00B_UNICODE_SB_FORMAT},    {"FO", N00B_UNICODE_SB_FORMAT},
    {"Sp",        N00B_UNICODE_SB_SP},
    {"Lower",     N00B_UNICODE_SB_LOWER},     {"LO", N00B_UNICODE_SB_LOWER},
    {"Upper",     N00B_UNICODE_SB_UPPER},     {"UP", N00B_UNICODE_SB_UPPER},
    {"OLetter",   N00B_UNICODE_SB_OLETTER},   {"LE", N00B_UNICODE_SB_OLETTER},
    {"Numeric",   N00B_UNICODE_SB_NUMERIC},   {"NU", N00B_UNICODE_SB_NUMERIC},
    {"ATerm",     N00B_UNICODE_SB_ATERM},     {"AT", N00B_UNICODE_SB_ATERM},
    {"STerm",     N00B_UNICODE_SB_STERM},     {"ST", N00B_UNICODE_SB_STERM},
    {"Close",     N00B_UNICODE_SB_CLOSE},     {"CL", N00B_UNICODE_SB_CLOSE},
    {"SContinue", N00B_UNICODE_SB_SCONTINUE}, {"SC", N00B_UNICODE_SB_SCONTINUE},
};

bool
n00b_unicode_sb_by_name(const char *name, n00b_unicode_sb_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_sb_names) / sizeof(_sb_names[0]); i++) {
        if (loose_eq(_sb_names[i].name, name)) {
            if (out) {
                *out = _sb_names[i].value;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Line_Break names.
// ---------------------------------------------------------------------------

typedef struct {
    const char       *name;
    n00b_unicode_lb_t value;
} _lb_name_t;

static const _lb_name_t _lb_names[] = {
    {"Unknown",                      N00B_UNICODE_LB_XX},  {"XX", N00B_UNICODE_LB_XX},
    {"Mandatory_Break",              N00B_UNICODE_LB_BK},  {"BK", N00B_UNICODE_LB_BK},
    {"Carriage_Return",              N00B_UNICODE_LB_CR},  {"CR", N00B_UNICODE_LB_CR},
    {"Line_Feed",                    N00B_UNICODE_LB_LF},  {"LF", N00B_UNICODE_LB_LF},
    {"Combining_Mark",               N00B_UNICODE_LB_CM},  {"CM", N00B_UNICODE_LB_CM},
    {"Next_Line",                    N00B_UNICODE_LB_NL},  {"NL", N00B_UNICODE_LB_NL},
    {"Surrogate",                    N00B_UNICODE_LB_SG},  {"SG", N00B_UNICODE_LB_SG},
    {"Word_Joiner",                  N00B_UNICODE_LB_WJ},  {"WJ", N00B_UNICODE_LB_WJ},
    {"ZWSpace",                      N00B_UNICODE_LB_ZW},  {"ZW", N00B_UNICODE_LB_ZW},
    {"Glue",                         N00B_UNICODE_LB_GL},  {"GL", N00B_UNICODE_LB_GL},
    {"Space",                        N00B_UNICODE_LB_SP},  {"SP", N00B_UNICODE_LB_SP},
    {"ZWJ",                          N00B_UNICODE_LB_ZWJ},
    {"Break_Both",                   N00B_UNICODE_LB_B2},  {"B2", N00B_UNICODE_LB_B2},
    {"Break_After",                  N00B_UNICODE_LB_BA},  {"BA", N00B_UNICODE_LB_BA},
    {"Break_Before",                 N00B_UNICODE_LB_BB},  {"BB", N00B_UNICODE_LB_BB},
    {"Hyphen",                       N00B_UNICODE_LB_HY},  {"HY", N00B_UNICODE_LB_HY},
    {"Contingent_Break",             N00B_UNICODE_LB_CB},  {"CB", N00B_UNICODE_LB_CB},
    {"Close_Punctuation",            N00B_UNICODE_LB_CL},  {"CL", N00B_UNICODE_LB_CL},
    {"Close_Parenthesis",            N00B_UNICODE_LB_CP},  {"CP", N00B_UNICODE_LB_CP},
    {"Exclamation",                  N00B_UNICODE_LB_EX},  {"EX", N00B_UNICODE_LB_EX},
    {"Inseparable",                  N00B_UNICODE_LB_IN},  {"IN", N00B_UNICODE_LB_IN},
    {"Nonstarter",                   N00B_UNICODE_LB_NS},  {"NS", N00B_UNICODE_LB_NS},
    {"Open_Punctuation",             N00B_UNICODE_LB_OP},  {"OP", N00B_UNICODE_LB_OP},
    {"Quotation",                    N00B_UNICODE_LB_QU},  {"QU", N00B_UNICODE_LB_QU},
    {"Infix_Numeric",                N00B_UNICODE_LB_IS},  {"IS", N00B_UNICODE_LB_IS},
    {"Numeric",                      N00B_UNICODE_LB_NU},  {"NU", N00B_UNICODE_LB_NU},
    {"Postfix_Numeric",              N00B_UNICODE_LB_PO},  {"PO", N00B_UNICODE_LB_PO},
    {"Prefix_Numeric",               N00B_UNICODE_LB_PR},  {"PR", N00B_UNICODE_LB_PR},
    {"Break_Symbols",                N00B_UNICODE_LB_SY},  {"SY", N00B_UNICODE_LB_SY},
    {"Ambiguous",                    N00B_UNICODE_LB_AI},  {"AI", N00B_UNICODE_LB_AI},
    {"Alphabetic",                   N00B_UNICODE_LB_AL},  {"AL", N00B_UNICODE_LB_AL},
    {"Conditional_Japanese_Starter", N00B_UNICODE_LB_CJ},  {"CJ", N00B_UNICODE_LB_CJ},
    {"E_Base",                       N00B_UNICODE_LB_EB},  {"EB", N00B_UNICODE_LB_EB},
    {"E_Modifier",                   N00B_UNICODE_LB_EM},  {"EM", N00B_UNICODE_LB_EM},
    {"H2",                           N00B_UNICODE_LB_H2},
    {"H3",                           N00B_UNICODE_LB_H3},
    {"Hebrew_Letter",                N00B_UNICODE_LB_HL},  {"HL", N00B_UNICODE_LB_HL},
    {"Ideographic",                  N00B_UNICODE_LB_ID},  {"ID", N00B_UNICODE_LB_ID},
    {"JL",                           N00B_UNICODE_LB_JL},
    {"JT",                           N00B_UNICODE_LB_JT},
    {"JV",                           N00B_UNICODE_LB_JV},
    {"Regional_Indicator",           N00B_UNICODE_LB_RI},  {"RI", N00B_UNICODE_LB_RI},
    {"Complex_Context",              N00B_UNICODE_LB_SA},  {"SA", N00B_UNICODE_LB_SA},
    {"Aksara",                       N00B_UNICODE_LB_AK},  {"AK", N00B_UNICODE_LB_AK},
    {"Aksara_Prebase",               N00B_UNICODE_LB_AP},  {"AP", N00B_UNICODE_LB_AP},
    {"Aksara_Start",                 N00B_UNICODE_LB_AS},  {"AS", N00B_UNICODE_LB_AS},
    {"Virama_Final",                 N00B_UNICODE_LB_VF},  {"VF", N00B_UNICODE_LB_VF},
    {"Virama",                       N00B_UNICODE_LB_VI},  {"VI", N00B_UNICODE_LB_VI},
};

bool
n00b_unicode_lb_by_name(const char *name, n00b_unicode_lb_t *out)
{
    if (!name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(_lb_names) / sizeof(_lb_names[0]); i++) {
        if (loose_eq(_lb_names[i].name, name)) {
            if (out) {
                *out = _lb_names[i].value;
            }
            return true;
        }
    }
    return false;
}
