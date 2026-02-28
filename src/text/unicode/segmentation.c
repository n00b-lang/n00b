#include "text/unicode/segmentation.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "internal/text/unicode/tables.h"
#include <string.h>
#include <assert.h>

// External generated tables
extern const uint16_t n00b_unicode_gcb_stage1[];
extern const uint8_t  n00b_unicode_gcb_stage2[];

extern const uint16_t n00b_unicode_wb_stage1[];
extern const uint8_t  n00b_unicode_wb_stage2[];

extern const uint16_t n00b_unicode_sb_stage1[];
extern const uint8_t  n00b_unicode_sb_stage2[];

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
n00b_unicode_grapheme_iter(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator; // accepted for API surface
    return n00b_unicode_grapheme_iter_raw(s.data, s.u8_bytes);
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
n00b_unicode_word_iter(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator; // accepted for API surface
    return n00b_unicode_word_iter_raw(s.data, s.u8_bytes);
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
n00b_unicode_sentence_iter(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    (void)allocator; // accepted for API surface
    return n00b_unicode_sentence_iter_raw(s.data, s.u8_bytes);
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
n00b_unicode_grapheme_count(n00b_string_t s)
{
    return n00b_unicode_grapheme_count_raw(s.data, s.u8_bytes);
}
