#include "unicode/linebreak.h"
#include "unicode/encoding.h"
#include "unicode/properties.h"
#include "internal/unicode/raw.h"
#include "core/alloc.h"
#include "internal/unicode/tables.h"
#include <string.h>
#include <assert.h>

extern const uint16_t n00b_unicode_lb_stage1[];
extern const uint8_t  n00b_unicode_lb_stage2[];

static inline n00b_unicode_lb_t
get_lb_class(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_LB_XX;
    return (n00b_unicode_lb_t)N00B_UNICODE_LOOKUP(n00b_unicode_lb_stage1,
                                                  n00b_unicode_lb_stage2,
                                                  cp);
}

// Resolve AI, CJ, SA, XX to their effective classes
static n00b_unicode_lb_t
resolve_lb(n00b_codepoint_t cp, n00b_unicode_lb_t cls)
{
    switch (cls) {
    case N00B_UNICODE_LB_AI:
        return N00B_UNICODE_LB_AL;
    case N00B_UNICODE_LB_CJ:
        return N00B_UNICODE_LB_NS;
    case N00B_UNICODE_LB_SA: {
        n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
        if (gc == N00B_UNICODE_GC_MN || gc == N00B_UNICODE_GC_MC)
            return N00B_UNICODE_LB_CM;
        return N00B_UNICODE_LB_AL;
    }
    case N00B_UNICODE_LB_XX:
        return N00B_UNICODE_LB_AL;
    case N00B_UNICODE_LB_SG:
        return N00B_UNICODE_LB_AL;
    default:
        return cls;
    }
}

static inline bool
is_qu_pf(n00b_codepoint_t cp)
{
    return n00b_unicode_general_category(cp) == N00B_UNICODE_GC_PF;
}

static inline bool
is_qu_pi(n00b_codepoint_t cp)
{
    return n00b_unicode_general_category(cp) == N00B_UNICODE_GC_PI;
}

static inline bool
is_east_asian(n00b_codepoint_t cp)
{
    n00b_unicode_eaw_t eaw = n00b_unicode_east_asian_width(cp);
    return (eaw == N00B_UNICODE_EAW_F || eaw == N00B_UNICODE_EAW_W
            || eaw == N00B_UNICODE_EAW_H);
}

// Is this a "Hyphen" for LB20.1? BA characters with GC=Pd that aren't East
// Asian
static inline bool
is_hyphen_ba(n00b_codepoint_t cp, n00b_unicode_lb_t cls)
{
    return cls == N00B_UNICODE_LB_BA && !is_east_asian(cp)
        && n00b_unicode_general_category(cp) == N00B_UNICODE_GC_PD;
}

// DottedCircle: U+25CC
#define DOTTED_CIRCLE 0x25CC

// Find the base codepoint index for position idx (look back past CM/ZWJ that
// inherited their class via LB9).
static inline uint32_t
find_base_idx(const n00b_codepoint_t *cps, uint32_t idx)
{
    while (idx > 0) {
        n00b_unicode_lb_t raw = resolve_lb(cps[idx], get_lb_class(cps[idx]));
        if (raw != N00B_UNICODE_LB_CM && raw != N00B_UNICODE_LB_ZWJ)
            break;
        idx--;
    }
    return idx;
}

// Check if position j's resolved class matches one of the "break context"
// classes used in LB20.1: (sot | BK | CR | LF | NL | SP | ZW | CB | GL)
static inline bool
is_break_context(n00b_unicode_lb_t cls)
{
    return cls == N00B_UNICODE_LB_BK || cls == N00B_UNICODE_LB_CR || cls == N00B_UNICODE_LB_LF
        || cls == N00B_UNICODE_LB_NL || cls == N00B_UNICODE_LB_SP || cls == N00B_UNICODE_LB_ZW
        || cls == N00B_UNICODE_LB_CB || cls == N00B_UNICODE_LB_GL;
}

// Check if codepoint is an Aksara-like class (AK, AS, or DottedCircle treated
// as AL)
static inline bool
is_aksara(n00b_unicode_lb_t cls, n00b_codepoint_t cp)
{
    return cls == N00B_UNICODE_LB_AK || cls == N00B_UNICODE_LB_AS
        || (cp == DOTTED_CIRCLE && cls == N00B_UNICODE_LB_AL);
}

void
n00b_unicode_linebreaks_raw(const char *data, int64_t len, n00b_unicode_lb_action_t *out)
{
    uint32_t      num_bytes      = (uint32_t)len;
    n00b_string_t _s             = {.u8_bytes = num_bytes, .data = (char *)data};
    int64_t       num_codepoints = n00b_unicode_utf8_count_codepoints(_s);

    if (num_codepoints <= 0)
        return;

    if (!out)
        return;

    n00b_codepoint_t *cps
        = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(n00b_codepoint_t));
    n00b_unicode_lb_t *cls
        = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(n00b_unicode_lb_t));
    uint32_t n   = 0;
    uint32_t pos = 0;

    while (pos < num_bytes && n < (uint32_t)num_codepoints) {
        int32_t cp = n00b_unicode_utf8_decode(data, num_bytes, &pos);
        if (cp < 0)
            break;
        cps[n] = (n00b_codepoint_t)cp;
        cls[n] = resolve_lb((n00b_codepoint_t)cp, get_lb_class((n00b_codepoint_t)cp));
        n++;
    }

    out[0] = N00B_UNICODE_LB_ACTION_NONE;

    for (uint32_t i = 1; i < n; i++) {
        n00b_unicode_lb_t prev = cls[i - 1];
        n00b_unicode_lb_t cur  = cls[i];

        // --- LB4: BK ! ---
        if (prev == N00B_UNICODE_LB_BK) {
            out[i] = N00B_UNICODE_LB_ACTION_MANDATORY;
            continue;
        }

        // --- LB5: CR × LF, CR !, LF !, NL ! ---
        if (prev == N00B_UNICODE_LB_CR && cur == N00B_UNICODE_LB_LF) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if (prev == N00B_UNICODE_LB_CR || prev == N00B_UNICODE_LB_LF
            || prev == N00B_UNICODE_LB_NL) {
            out[i] = N00B_UNICODE_LB_ACTION_MANDATORY;
            continue;
        }

        // --- LB6: × (BK | CR | LF | NL) ---
        if (cur == N00B_UNICODE_LB_BK || cur == N00B_UNICODE_LB_CR || cur == N00B_UNICODE_LB_LF
            || cur == N00B_UNICODE_LB_NL) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB7: × SP, × ZW ---
        if (cur == N00B_UNICODE_LB_SP || cur == N00B_UNICODE_LB_ZW) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB8: ZW SP* ÷ ---
        {
            bool after_zw = (prev == N00B_UNICODE_LB_ZW);
            if (!after_zw && prev == N00B_UNICODE_LB_SP) {
                int32_t j = (int32_t)i - 1;
                while (j >= 0 && cls[j] == N00B_UNICODE_LB_SP)
                    j--;
                if (j >= 0 && cls[j] == N00B_UNICODE_LB_ZW)
                    after_zw = true;
            }
            if (after_zw) {
                out[i] = N00B_UNICODE_LB_ACTION_ALLOWED;
                continue;
            }
        }

        // --- LB8a: ZWJ × ---
        if (prev == N00B_UNICODE_LB_ZWJ) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB9: X (CM|ZWJ)* → treat as X ---
        if (cur == N00B_UNICODE_LB_CM || cur == N00B_UNICODE_LB_ZWJ) {
            if (prev != N00B_UNICODE_LB_SP && prev != N00B_UNICODE_LB_BK
                && prev != N00B_UNICODE_LB_CR && prev != N00B_UNICODE_LB_LF
                && prev != N00B_UNICODE_LB_NL && prev != N00B_UNICODE_LB_ZW) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                cls[i] = prev;
                continue;
            }
        }

        // --- LB10: CM → AL ---
        if (prev == N00B_UNICODE_LB_CM)
            prev = N00B_UNICODE_LB_AL;
        if (cur == N00B_UNICODE_LB_CM)
            cur = N00B_UNICODE_LB_AL;

        // --- LB11: × WJ, WJ × ---
        if (cur == N00B_UNICODE_LB_WJ || prev == N00B_UNICODE_LB_WJ) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB12: GL × ---
        if (prev == N00B_UNICODE_LB_GL) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB12a: [^SP BA HY] × GL ---
        if (cur == N00B_UNICODE_LB_GL && prev != N00B_UNICODE_LB_SP
            && prev != N00B_UNICODE_LB_BA && prev != N00B_UNICODE_LB_HY) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB13: × EX, × CL, × CP, × SY ---
        // Note: IS removed from LB13 in Unicode 16.0 (handled by 15.3/15.4)
        if (cur == N00B_UNICODE_LB_EX || cur == N00B_UNICODE_LB_CL || cur == N00B_UNICODE_LB_CP
            || cur == N00B_UNICODE_LB_SY) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB14: OP SP* × ---
        {
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && cls[j] == N00B_UNICODE_LB_SP)
                j--;
            if (j >= 0 && cls[j] == N00B_UNICODE_LB_OP) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB15.11: (sot | BK | CR | LF | NL | OP | QU | GL | SP | ZW) QU_Pi
        // (CM|ZWJ)* SP* × ---
        {
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && cls[j] == N00B_UNICODE_LB_SP)
                j--;
            // Walk back past CM/ZWJ that inherited QU via LB9
            int32_t qu_j = j;
            while (qu_j > 0 && cls[qu_j] == N00B_UNICODE_LB_QU
                   && get_lb_class(cps[qu_j]) != N00B_UNICODE_LB_QU)
                qu_j--;
            if (qu_j >= 0 && cls[qu_j] == N00B_UNICODE_LB_QU && is_qu_pi(cps[qu_j])) {
                bool valid_ctx = (qu_j == 0);
                if (!valid_ctx && qu_j > 0) {
                    n00b_unicode_lb_t before_qu = cls[qu_j - 1];
                    valid_ctx
                        = (before_qu == N00B_UNICODE_LB_BK || before_qu == N00B_UNICODE_LB_CR
                           || before_qu == N00B_UNICODE_LB_LF || before_qu == N00B_UNICODE_LB_NL
                           || before_qu == N00B_UNICODE_LB_OP || before_qu == N00B_UNICODE_LB_QU
                           || before_qu == N00B_UNICODE_LB_GL || before_qu == N00B_UNICODE_LB_SP
                           || before_qu == N00B_UNICODE_LB_ZW);
                }
                if (valid_ctx) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
            }
        }

        // --- LB15.21: × QU_Pf (SP|GL|WJ|CL|QU|CP|EX|IS|SY|BK|CR|LF|NL|ZW|eot) ---
        if (cur == N00B_UNICODE_LB_QU && is_qu_pf(cps[i])) {
            bool valid_after = (i + 1 >= n); // eot
            if (!valid_after) {
                n00b_unicode_lb_t after = cls[i + 1];
                valid_after = (after == N00B_UNICODE_LB_SP || after == N00B_UNICODE_LB_GL
                               || after == N00B_UNICODE_LB_WJ || after == N00B_UNICODE_LB_CL
                               || after == N00B_UNICODE_LB_QU || after == N00B_UNICODE_LB_CP
                               || after == N00B_UNICODE_LB_EX || after == N00B_UNICODE_LB_IS
                               || after == N00B_UNICODE_LB_SY || after == N00B_UNICODE_LB_BK
                               || after == N00B_UNICODE_LB_CR || after == N00B_UNICODE_LB_LF
                               || after == N00B_UNICODE_LB_NL || after == N00B_UNICODE_LB_ZW);
            }
            if (valid_after) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB15.3: SP ÷ IS NU ---
        // Break between SP and IS when IS is followed by NU
        if (prev == N00B_UNICODE_LB_SP && cur == N00B_UNICODE_LB_IS) {
            // Look ahead past CM/ZWJ for NU
            uint32_t la = i + 1;
            while (la < n && (cls[la] == N00B_UNICODE_LB_CM || cls[la] == N00B_UNICODE_LB_ZWJ))
                la++;
            if (la < n && cls[la] == N00B_UNICODE_LB_NU) {
                out[i] = N00B_UNICODE_LB_ACTION_ALLOWED;
                continue;
            }
        }

        // --- LB15.4: × IS ---
        if (cur == N00B_UNICODE_LB_IS) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB16: (CL | CP) SP* × NS ---
        if (cur == N00B_UNICODE_LB_NS) {
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && cls[j] == N00B_UNICODE_LB_SP)
                j--;
            if (j >= 0 && (cls[j] == N00B_UNICODE_LB_CL || cls[j] == N00B_UNICODE_LB_CP)) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB17: B2 SP* × B2 ---
        if (cur == N00B_UNICODE_LB_B2) {
            int32_t j = (int32_t)i - 1;
            while (j >= 0 && cls[j] == N00B_UNICODE_LB_SP)
                j--;
            if (j >= 0 && cls[j] == N00B_UNICODE_LB_B2) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB18: SP ÷ ---
        if (prev == N00B_UNICODE_LB_SP) {
            out[i] = N00B_UNICODE_LB_ACTION_ALLOWED;
            continue;
        }

        // --- LB19: Quotation mark rules (Unicode 16.0) ---
        // 19.01: × QU_mPi (don't break before QU that is not Pi)
        if (cur == N00B_UNICODE_LB_QU && !is_qu_pi(cps[i])) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        // 19.02: QU_mPf × (don't break after QU that is not Pf)
        if (prev == N00B_UNICODE_LB_QU && !is_qu_pf(cps[find_base_idx(cps, i - 1)])) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        // At this point, if cur is QU it must be QU_Pi, and if prev is QU it must
        // be QU_Pf. 19.1: NotEastAsian × QU
        if (cur == N00B_UNICODE_LB_QU) {
            uint32_t base = find_base_idx(cps, i - 1);
            if (!is_east_asian(cps[base])) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }
        // 19.11: × QU (NotEastAsian | eot)
        if (cur == N00B_UNICODE_LB_QU) {
            bool valid = (i + 1 >= n); // eot
            if (!valid) {
                // Check what follows QU (past CM/ZWJ)
                uint32_t la = i + 1;
                while (la < n
                       && (cls[la] == N00B_UNICODE_LB_CM || cls[la] == N00B_UNICODE_LB_ZWJ))
                    la++;
                valid = (la >= n) || !is_east_asian(cps[la]);
            }
            if (valid) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }
        // 19.12: QU × NotEastAsian
        if (prev == N00B_UNICODE_LB_QU && !is_east_asian(cps[i])) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        // 19.13: (sot | NotEastAsian) QU ×
        if (prev == N00B_UNICODE_LB_QU) {
            uint32_t qu_base = find_base_idx(cps, i - 1);
            int32_t  before  = (int32_t)qu_base - 1;
            bool     valid   = (before < 0); // sot
            if (!valid) {
                valid = !is_east_asian(cps[before]);
            }
            if (valid) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB20: ÷ CB, CB ÷ ---
        if (cur == N00B_UNICODE_LB_CB || prev == N00B_UNICODE_LB_CB) {
            out[i] = N00B_UNICODE_LB_ACTION_ALLOWED;
            continue;
        }

        // --- LB20.1: (sot | BK | CR | LF | NL | SP | ZW | CB | GL) (HY | Hyphen) ×
        // AL ---
        if (cur == N00B_UNICODE_LB_AL
            && (prev == N00B_UNICODE_LB_HY || prev == N00B_UNICODE_LB_BA)) {
            uint32_t prev_base = find_base_idx(cps, i - 1);
            bool     is_hyp
                = (prev == N00B_UNICODE_LB_HY) || is_hyphen_ba(cps[prev_base], cls[prev_base]);
            if (is_hyp) {
                // Check context before the HY/Hyphen
                // Look back past the HY/Hyphen's CM cluster to find what precedes it
                int32_t ctx   = (int32_t)prev_base - 1;
                bool    valid = (ctx < 0); // sot
                if (!valid) {
                    valid = is_break_context(cls[ctx]);
                }
                if (valid) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
            }
        }

        // --- LB21: × BA, × HY, × NS, BB × ---
        if (cur == N00B_UNICODE_LB_BA || cur == N00B_UNICODE_LB_HY || cur == N00B_UNICODE_LB_NS
            || prev == N00B_UNICODE_LB_BB) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB21.1: HL (HY | NonEastAsianBA) × [^HL] ---
        if (i >= 2 && cur != N00B_UNICODE_LB_HL) {
            uint32_t prev_base = find_base_idx(cps, i - 1);
            if (prev == N00B_UNICODE_LB_HY
                || (prev == N00B_UNICODE_LB_BA && !is_east_asian(cps[prev_base]))) {
                // Check if the char before HY/BA is HL
                int32_t before = (int32_t)prev_base - 1;
                if (before >= 0 && cls[before] == N00B_UNICODE_LB_HL) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
            }
        }

        // --- LB21.2: SY × HL ---
        if (prev == N00B_UNICODE_LB_SY && cur == N00B_UNICODE_LB_HL) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB22: × IN ---
        if (cur == N00B_UNICODE_LB_IN) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB23: (AL | HL) × NU, NU × (AL | HL) ---
        if ((prev == N00B_UNICODE_LB_AL || prev == N00B_UNICODE_LB_HL)
            && cur == N00B_UNICODE_LB_NU) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if (prev == N00B_UNICODE_LB_NU
            && (cur == N00B_UNICODE_LB_AL || cur == N00B_UNICODE_LB_HL)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB23a: PR × (ID | EB | EM), (ID | EB | EM) × PO ---
        if (prev == N00B_UNICODE_LB_PR
            && (cur == N00B_UNICODE_LB_ID || cur == N00B_UNICODE_LB_EB
                || cur == N00B_UNICODE_LB_EM)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if ((prev == N00B_UNICODE_LB_ID || prev == N00B_UNICODE_LB_EB
             || prev == N00B_UNICODE_LB_EM)
            && cur == N00B_UNICODE_LB_PO) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB24: (PR | PO) × (AL | HL), (AL | HL) × (PR | PO) ---
        if ((prev == N00B_UNICODE_LB_PR || prev == N00B_UNICODE_LB_PO)
            && (cur == N00B_UNICODE_LB_AL || cur == N00B_UNICODE_LB_HL)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if ((prev == N00B_UNICODE_LB_AL || prev == N00B_UNICODE_LB_HL)
            && (cur == N00B_UNICODE_LB_PR || cur == N00B_UNICODE_LB_PO)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB25: Numeric context rules ---
        // Look back for NU context: NU (SY|IS)* (CL|CP)?
        {
            bool    in_nu_ctx = false;
            int32_t j         = (int32_t)i - 1;
            // Look back past SY/IS
            while (j >= 0 && (cls[j] == N00B_UNICODE_LB_SY || cls[j] == N00B_UNICODE_LB_IS))
                j--;
            // Optional CL/CP
            int32_t pre_clcp = j;
            if (j >= 0 && (cls[j] == N00B_UNICODE_LB_CL || cls[j] == N00B_UNICODE_LB_CP))
                j--;
            if (j >= 0 && cls[j] == N00B_UNICODE_LB_NU)
                in_nu_ctx = true;

            if (in_nu_ctx) {
                // 25.01-25.04: NU (SY|IS)* CL × PO, NU (SY|IS)* CP × PO,
                //              NU (SY|IS)* CL × PR, NU (SY|IS)* CP × PR
                if ((cur == N00B_UNICODE_LB_PO || cur == N00B_UNICODE_LB_PR) && pre_clcp >= 0
                    && (cls[pre_clcp] == N00B_UNICODE_LB_CL
                        || cls[pre_clcp] == N00B_UNICODE_LB_CP)) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
                // 25.05-25.06: NU (SY|IS)* × PO, NU (SY|IS)* × PR
                if (cur == N00B_UNICODE_LB_PO || cur == N00B_UNICODE_LB_PR) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
                // 25.15: NU (SY|IS)* × NU
                if (cur == N00B_UNICODE_LB_NU) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
            }
        }

        // 25.07-25.12: (PR|PO) × (OP (IS)?)? NU
        if ((prev == N00B_UNICODE_LB_PR || prev == N00B_UNICODE_LB_PO)
            && cur == N00B_UNICODE_LB_NU) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if ((prev == N00B_UNICODE_LB_PR || prev == N00B_UNICODE_LB_PO)
            && (cur == N00B_UNICODE_LB_OP || cur == N00B_UNICODE_LB_HY)) {
            uint32_t la = i + 1;
            while (la < n && (cls[la] == N00B_UNICODE_LB_CM || cls[la] == N00B_UNICODE_LB_ZWJ))
                la++;
            // Check for NU directly or IS NU
            if (la < n && cls[la] == N00B_UNICODE_LB_NU) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
            if (la < n && cls[la] == N00B_UNICODE_LB_IS) {
                uint32_t la2 = la + 1;
                while (la2 < n
                       && (cls[la2] == N00B_UNICODE_LB_CM || cls[la2] == N00B_UNICODE_LB_ZWJ))
                    la2++;
                if (la2 < n && cls[la2] == N00B_UNICODE_LB_NU) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
            }
        }

        // 25.13: HY × NU
        if (prev == N00B_UNICODE_LB_HY && cur == N00B_UNICODE_LB_NU) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // 25.14: IS × NU
        if (prev == N00B_UNICODE_LB_IS && cur == N00B_UNICODE_LB_NU) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB26: Korean syllable blocks ---
        if (prev == N00B_UNICODE_LB_JL
            && (cur == N00B_UNICODE_LB_JL || cur == N00B_UNICODE_LB_JV
                || cur == N00B_UNICODE_LB_H2 || cur == N00B_UNICODE_LB_H3)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if ((prev == N00B_UNICODE_LB_JV || prev == N00B_UNICODE_LB_H2)
            && (cur == N00B_UNICODE_LB_JV || cur == N00B_UNICODE_LB_JT)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if ((prev == N00B_UNICODE_LB_JT || prev == N00B_UNICODE_LB_H3)
            && cur == N00B_UNICODE_LB_JT) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB27: (JL|JV|JT|H2|H3) × PO, PR × (JL|JV|JT|H2|H3) ---
        if ((prev == N00B_UNICODE_LB_JL || prev == N00B_UNICODE_LB_JV
             || prev == N00B_UNICODE_LB_JT || prev == N00B_UNICODE_LB_H2
             || prev == N00B_UNICODE_LB_H3)
            && cur == N00B_UNICODE_LB_PO) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if (prev == N00B_UNICODE_LB_PR
            && (cur == N00B_UNICODE_LB_JL || cur == N00B_UNICODE_LB_JV
                || cur == N00B_UNICODE_LB_JT || cur == N00B_UNICODE_LB_H2
                || cur == N00B_UNICODE_LB_H3)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB28.0: (AL | HL) × (AL | HL) ---
        if ((prev == N00B_UNICODE_LB_AL || prev == N00B_UNICODE_LB_HL)
            && (cur == N00B_UNICODE_LB_AL || cur == N00B_UNICODE_LB_HL)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB28.11: AP × (AK | DottedCircle | AS) ---
        if (prev == N00B_UNICODE_LB_AP && is_aksara(cur, cps[i])) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB28.12: (AK | DottedCircle | AS) × (VF | VI) ---
        if (is_aksara(prev, cps[find_base_idx(cps, i - 1)])
            && (cur == N00B_UNICODE_LB_VF || cur == N00B_UNICODE_LB_VI)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB28.13: (AK | DottedCircle | AS) VI × (AK | DottedCircle) ---
        if (prev == N00B_UNICODE_LB_VI && i >= 2 && is_aksara(cur, cps[i])) {
            // Check if char before VI is AK/DottedCircle/AS
            // Need to find base of position before VI (may have CMs inheriting)
            uint32_t vi_base   = find_base_idx(cps, i - 1);
            int32_t  before_vi = (int32_t)vi_base - 1;
            if (before_vi >= 0) {
                uint32_t aksara_base = find_base_idx(cps, (uint32_t)before_vi);
                if (is_aksara(cls[aksara_base], cps[aksara_base])) {
                    out[i] = N00B_UNICODE_LB_ACTION_NONE;
                    continue;
                }
            }
        }

        // --- LB28.14: (AK | DottedCircle | AS) × (AK | DottedCircle | AS) VF ---
        // Look ahead: if cur is AK/DottedCircle/AS and next (past CM/ZWJ) is VF
        if (is_aksara(prev, cps[find_base_idx(cps, i - 1)]) && is_aksara(cur, cps[i])) {
            uint32_t la = i + 1;
            while (la < n && (cls[la] == N00B_UNICODE_LB_CM || cls[la] == N00B_UNICODE_LB_ZWJ))
                la++;
            if (la < n && cls[la] == N00B_UNICODE_LB_VF) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB29: IS × (AL | HL) ---
        if (prev == N00B_UNICODE_LB_IS
            && (cur == N00B_UNICODE_LB_AL || cur == N00B_UNICODE_LB_HL)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB30: (AL | HL | NU) × OP30, CP30 × (AL | HL | NU) ---
        // OP30/CP30 = non-East-Asian OP/CP
        if ((prev == N00B_UNICODE_LB_AL || prev == N00B_UNICODE_LB_HL
             || prev == N00B_UNICODE_LB_NU)
            && cur == N00B_UNICODE_LB_OP && !is_east_asian(cps[i])) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        if (prev == N00B_UNICODE_LB_CP && !is_east_asian(cps[find_base_idx(cps, i - 1)])
            && (cur == N00B_UNICODE_LB_AL || cur == N00B_UNICODE_LB_HL
                || cur == N00B_UNICODE_LB_NU)) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }

        // --- LB30a: RI × RI (pair only) ---
        if (prev == N00B_UNICODE_LB_RI && cur == N00B_UNICODE_LB_RI) {
            uint32_t ri_count = 0;
            for (int32_t j = (int32_t)i - 1; j >= 0; j--) {
                if (cls[j] == N00B_UNICODE_LB_RI) {
                    n00b_unicode_lb_t raw = resolve_lb(cps[j], get_lb_class(cps[j]));
                    if (raw == N00B_UNICODE_LB_RI)
                        ri_count++;
                }
                else {
                    break;
                }
            }
            if (ri_count % 2 == 1) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB30b: EB × EM, ExtPictUnassigned × EM ---
        if (prev == N00B_UNICODE_LB_EB && cur == N00B_UNICODE_LB_EM) {
            out[i] = N00B_UNICODE_LB_ACTION_NONE;
            continue;
        }
        // 30.22: ExtPictUnassigned × EM
        // An unassigned codepoint with Extended_Pictographic followed by EM
        if (cur == N00B_UNICODE_LB_EM) {
            uint32_t base = find_base_idx(cps, i - 1);
            if (n00b_unicode_has_property(cps[base], N00B_UNICODE_PROP_EXTENDED_PICTOGRAPHIC)
                && n00b_unicode_general_category(cps[base]) == N00B_UNICODE_GC_CN) {
                out[i] = N00B_UNICODE_LB_ACTION_NONE;
                continue;
            }
        }

        // --- LB31: ALL ÷ ALL ---
        out[i] = N00B_UNICODE_LB_ACTION_ALLOWED;
    }

    n00b_free(cps);
    n00b_free(cls);
}

void
n00b_unicode_linebreaks(n00b_string_t s, n00b_unicode_lb_action_t *out)
{
    assert(out || s.u8_bytes == 0);
    n00b_unicode_linebreaks_raw(s.data, s.u8_bytes, out);
}

uint32_t *
n00b_unicode_linebreak_wrap_raw(const char *data, int64_t len, int width,
                                int hang, bool no_hard_wrap,
                                uint32_t *num_breaks)
{
    uint32_t      num_bytes      = (uint32_t)len;
    n00b_string_t _s2            = {.u8_bytes = num_bytes, .data = (char *)data};
    int64_t       num_codepoints = n00b_unicode_utf8_count_codepoints(_s2);

    if (num_codepoints <= 0 || width <= 0) {
        *num_breaks = 0;
        return nullptr;
    }

    int hang_width = width - hang;

    if (hang_width <= 0) {
        hang_width = 1;
    }

    n00b_unicode_lb_action_t *actions
        = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(n00b_unicode_lb_action_t));
    n00b_unicode_linebreaks_raw(data, len, actions);

    uint32_t *byte_offsets
        = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(uint32_t));
    uint32_t pos = 0;
    for (uint32_t i = 0; i < (uint32_t)num_codepoints; i++) {
        byte_offsets[i] = pos;
        int32_t cp      = n00b_unicode_utf8_decode(data, num_bytes, &pos);
        (void)cp;
    }

    uint32_t *breaks   = n00b_alloc_array(char, (uint32_t)num_codepoints * sizeof(uint32_t));
    uint32_t  n_breaks = 0;
    int       col      = 0;
    int       cur_width = width;
    uint32_t  last_break_idx = 0;
    bool      has_break      = false;

    for (uint32_t i = 0; i < (uint32_t)num_codepoints; i++) {
        uint32_t bp = byte_offsets[i];
        int32_t  cp = n00b_unicode_utf8_decode(data, num_bytes, &bp);
        int      w  = (cp >= 0) ? n00b_unicode_char_width((n00b_codepoint_t)cp) : 1;

        if (actions[i] == N00B_UNICODE_LB_ACTION_MANDATORY) {
            breaks[n_breaks++] = byte_offsets[i];
            col                = 0;
            cur_width          = hang_width;
            has_break          = false;
            continue;
        }

        if (actions[i] == N00B_UNICODE_LB_ACTION_ALLOWED) {
            last_break_idx = i;
            has_break      = true;
        }

        col += w;

        if (col > cur_width) {
            if (has_break) {
                breaks[n_breaks++] = byte_offsets[last_break_idx];
                col                = 0;
                has_break          = false;
                cur_width          = hang_width;

                for (uint32_t j = last_break_idx; j <= i; j++) {
                    uint32_t bp2 = byte_offsets[j];
                    int32_t  cp2 = n00b_unicode_utf8_decode(data, num_bytes, &bp2);
                    col += (cp2 >= 0)
                               ? n00b_unicode_char_width((n00b_codepoint_t)cp2)
                               : 1;
                }
            }
            else if (!no_hard_wrap) {
                // No valid soft break; force-break before this character.
                breaks[n_breaks++] = byte_offsets[i];
                col                = w;
                has_break          = false;
                cur_width          = hang_width;
            }
        }
    }

    n00b_free(actions);
    n00b_free(byte_offsets);

    *num_breaks = n_breaks;

    if (n_breaks == 0) {
        n00b_free(breaks);
        return nullptr;
    }

    uint32_t *trimmed = n00b_alloc_array(char, n_breaks * sizeof(uint32_t));
    memcpy(trimmed, breaks, n_breaks * sizeof(uint32_t));
    n00b_free(breaks);
    return trimmed;
}

n00b_array_t(uint32_t)
n00b_unicode_linebreak_wrap(n00b_string_t s) _kargs
{
    int               width        = 80;
    int               hang         = 0;
    bool              no_hard_wrap = false;
    n00b_allocator_t *allocator    = nullptr;
}
{
    (void)allocator;
    uint32_t  count = 0;
    uint32_t *raw   = n00b_unicode_linebreak_wrap_raw(s.data, s.u8_bytes, width, hang,
                                                       no_hard_wrap, &count);
    if (!raw) {
        n00b_array_t(uint32_t) empty = {};
        return empty;
    }
    n00b_array_t(uint32_t) result = n00b_array_checked_ptr(uint32_t, count, raw);
    result.len = count;
    return result;
}
