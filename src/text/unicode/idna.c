#include "text/unicode/idna.h"
#include "text/unicode/encoding.h"
#include "text/unicode/normalization.h"
#include "text/unicode/properties.h"
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"
#include "internal/text/unicode/tables.h"
#include <string.h>

extern const uint16_t n00b_unicode_idna_status_stage1[];
extern const uint8_t  n00b_unicode_idna_status_stage2[];

extern const uint32_t n00b_unicode_idna_map_index[][2];
extern const uint32_t n00b_unicode_idna_map_index_len;
extern const uint32_t n00b_unicode_idna_map_data[];

// IDNA status values
#define IDNA_VALID      0
#define IDNA_IGNORED    1
#define IDNA_MAPPED     2
#define IDNA_DEVIATION  3
#define IDNA_DISALLOWED 4

// ---------------------------------------------------------------------------
// CONTEXTJ / CONTEXTO rules (RFC 5892 Appendix A)
// ---------------------------------------------------------------------------

// Check CONTEXTJ rules for ZWJ (U+200D) and ZWNJ (U+200C).
static bool
validate_contextj(const n00b_codepoint_t *cps, uint32_t len, uint32_t pos)
{
    n00b_codepoint_t cp = cps[pos];

    if (cp == 0x200D) {
        // ZWJ: Must be preceded by a character with CCC = Virama (9)
        if (pos == 0)
            return false;
        return n00b_unicode_combining_class(cps[pos - 1]) == 9;
    }

    if (cp == 0x200C) {
        // ZWNJ Rule A.1: Preceded by CCC = Virama
        if (pos > 0 && n00b_unicode_combining_class(cps[pos - 1]) == 9)
            return true;

        // ZWNJ Rule A.2: Valid joining context
        // Before: search backward for JT in {D, L} (skip T)
        bool before_ok = false;
        for (int32_t i = (int32_t)pos - 1; i >= 0; i--) {
            n00b_unicode_jt_t jt = n00b_unicode_joining_type(cps[i]);
            if (jt == N00B_UNICODE_JT_T)
                continue;
            if (jt == N00B_UNICODE_JT_D || jt == N00B_UNICODE_JT_L) {
                before_ok = true;
            }
            break;
        }
        if (!before_ok)
            return false;

        // After: search forward for JT in {D, R} (skip T)
        for (uint32_t i = pos + 1; i < len; i++) {
            n00b_unicode_jt_t jt = n00b_unicode_joining_type(cps[i]);
            if (jt == N00B_UNICODE_JT_T)
                continue;
            if (jt == N00B_UNICODE_JT_D || jt == N00B_UNICODE_JT_R)
                return true;
            break;
        }
        return false;
    }

    return true; // Not a CONTEXTJ character
}

// Check CONTEXTO rules per RFC 5892 Appendix A.
static bool
validate_contexto(const n00b_codepoint_t *cps, uint32_t len, uint32_t pos)
{
    n00b_codepoint_t cp = cps[pos];

    switch (cp) {
    case 0x00B7: // MIDDLE DOT
        // Must be between two U+006C (lowercase L)
        return (pos > 0 && pos + 1 < len && cps[pos - 1] == 0x006C && cps[pos + 1] == 0x006C);

    case 0x0375: // GREEK LOWER NUMERAL SIGN (Keraia)
        // Must be followed by a Greek script character
        if (pos + 1 >= len)
            return false;
        return n00b_unicode_script(cps[pos + 1]) == 44; // Greek = index 44

    case 0x05F3:                                        // HEBREW PUNCTUATION GERESH
    case 0x05F4:                                        // HEBREW PUNCTUATION GERSHAYIM
        // Must be preceded by a Hebrew script character
        if (pos == 0)
            return false;
        return n00b_unicode_script(cps[pos - 1]) == 54; // Hebrew = index 54

    default:
        // Arabic-Indic digits (U+0660-U+0669):
        // Label must not contain Extended Arabic-Indic digits (U+06F0-U+06F9)
        if (cp >= 0x0660 && cp <= 0x0669) {
            for (uint32_t i = 0; i < len; i++) {
                if (cps[i] >= 0x06F0 && cps[i] <= 0x06F9)
                    return false;
            }
            return true;
        }
        // Extended Arabic-Indic digits (U+06F0-U+06F9):
        // Label must not contain Arabic-Indic digits (U+0660-U+0669)
        if (cp >= 0x06F0 && cp <= 0x06F9) {
            for (uint32_t i = 0; i < len; i++) {
                if (cps[i] >= 0x0660 && cps[i] <= 0x0669)
                    return false;
            }
            return true;
        }
        break;
    }

    return true;
}

// Validate all CONTEXTJ and CONTEXTO characters in a label.
static bool
validate_label_context(const n00b_codepoint_t    *cps,
                       uint32_t                   len,
                       n00b_unicode_idna_error_t *err)
{
    for (uint32_t i = 0; i < len; i++) {
        n00b_codepoint_t cp = cps[i];

        // CONTEXTJ: ZWJ and ZWNJ
        if (cp == 0x200C || cp == 0x200D) {
            if (!validate_contextj(cps, len, i)) {
                if (err)
                    *err = N00B_UNICODE_IDNA_CONTEXTJ_ERROR;
                return false;
            }
            continue;
        }

        // CONTEXTO characters
        if (cp == 0x00B7 || cp == 0x0375 || cp == 0x05F3 || cp == 0x05F4
            || (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9)) {
            if (!validate_contexto(cps, len, i)) {
                if (err)
                    *err = N00B_UNICODE_IDNA_CONTEXTO_ERROR;
                return false;
            }
        }
    }
    return true;
}

static uint8_t
get_idna_status(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return IDNA_DISALLOWED;
    return N00B_UNICODE_LOOKUP(n00b_unicode_idna_status_stage1,
                               n00b_unicode_idna_status_stage2,
                               cp);
}

// ---------------------------------------------------------------------------
// Punycode (RFC 3492)
// ---------------------------------------------------------------------------

#define PUNY_BASE         36
#define PUNY_TMIN         1
#define PUNY_TMAX         26
#define PUNY_SKEW         38
#define PUNY_DAMP         700
#define PUNY_INITIAL_BIAS 72
#define PUNY_INITIAL_N    128

static char
puny_digit(int d)
{
    return (char)(d < 26 ? 'a' + d : '0' + d - 26);
}

// Inverse of puny_digit: ASCII byte → digit in [0, 35], or -1 if not valid.
static int
puny_decode_digit(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0' + 26;
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    return -1;
}

static int
puny_adapt(int delta, int numpoints, bool firsttime)
{
    delta = firsttime ? delta / PUNY_DAMP : delta / 2;
    delta += delta / numpoints;
    int k = 0;
    while (delta > ((PUNY_BASE - PUNY_TMIN) * PUNY_TMAX) / 2) {
        delta /= PUNY_BASE - PUNY_TMIN;
        k += PUNY_BASE;
    }
    return k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW);
}

// Punycode encode. Returns malloc'd ASCII string or nullptr on error.
static char *
puny_encode(const n00b_codepoint_t *input, uint32_t input_len, uint32_t *out_len)
{
    char    *output  = n00b_alloc_array(char, input_len * 6 + 64);
    uint32_t out_pos = 0;

    // Copy basic (ASCII) characters
    uint32_t b = 0;
    for (uint32_t i = 0; i < input_len; i++) {
        if (input[i] < 128) {
            output[out_pos++] = (char)input[i];
            b++;
        }
    }

    uint32_t h = b;
    if (b > 0)
        output[out_pos++] = '-';

    int n     = PUNY_INITIAL_N;
    int delta = 0;
    int bias  = PUNY_INITIAL_BIAS;

    while (h < input_len) {
        // Find minimum codepoint >= n
        int m = 0x10FFFF + 1;
        for (uint32_t i = 0; i < input_len; i++) {
            if ((int)input[i] >= n && (int)input[i] < m) {
                m = (int)input[i];
            }
        }

        delta += (m - n) * (int)(h + 1);
        n = m;

        for (uint32_t i = 0; i < input_len; i++) {
            if ((int)input[i] < n) {
                delta++;
            }
            else if ((int)input[i] == n) {
                int q = delta;
                for (int k = PUNY_BASE;; k += PUNY_BASE) {
                    int t = (k <= bias)             ? PUNY_TMIN
                          : (k >= bias + PUNY_TMAX) ? PUNY_TMAX
                                                    : k - bias;
                    if (q < t)
                        break;
                    output[out_pos++] = puny_digit(t + (q - t) % (PUNY_BASE - t));
                    q                 = (q - t) / (PUNY_BASE - t);
                }
                output[out_pos++] = puny_digit(q);
                bias              = puny_adapt(delta, (int)(h + 1), h == b);
                delta             = 0;
                h++;
            }
        }

        delta++;
        n++;
    }

    output[out_pos] = '\0';
    if (out_len)
        *out_len = out_pos;
    return output;
}

// Punycode decode (RFC 3492 §6.2).  @p input is the ASCII payload *after* the
// `xn--` prefix; @p input_len is its length.  Writes up to @p out_cap
// codepoints into @p output and returns the count produced, or -1 on error
// (malformed encoding, overflow, output overrun).
static int32_t
puny_decode(const char       *input,
            uint32_t          input_len,
            n00b_codepoint_t *output,
            uint32_t          out_cap)
{
    int      n      = PUNY_INITIAL_N;
    int      i      = 0;
    int      bias   = PUNY_INITIAL_BIAS;
    uint32_t in     = 0;
    uint32_t out    = 0;

    // Step 1: copy basic codepoints up to the last hyphen (delimiter).
    int32_t last_delim = -1;
    for (int32_t k = (int32_t)input_len - 1; k >= 0; k--) {
        if (input[k] == '-') {
            last_delim = k;
            break;
        }
    }

    if (last_delim >= 0) {
        for (int32_t k = 0; k < last_delim; k++) {
            uint8_t b = (uint8_t)input[k];
            if (b >= 0x80) {
                return -1;  // basic codepoints must be ASCII
            }
            if (out >= out_cap) {
                return -1;
            }
            output[out++] = (n00b_codepoint_t)b;
        }
        in = (uint32_t)last_delim + 1;
    }

    // Step 2: main decode loop.
    while (in < input_len) {
        int oldi = i;
        int w    = 1;
        for (int k = PUNY_BASE;; k += PUNY_BASE) {
            if (in >= input_len) {
                return -1;  // unterminated variable-length integer
            }
            int digit = puny_decode_digit((uint8_t)input[in++]);
            if (digit < 0) {
                return -1;
            }
            // overflow guard
            if (digit > (0x7FFFFFFF - i) / w) {
                return -1;
            }
            i += digit * w;
            int t = (k <= bias)             ? PUNY_TMIN
                  : (k >= bias + PUNY_TMAX) ? PUNY_TMAX
                                            : k - bias;
            if (digit < t) {
                break;
            }
            if (w > 0x7FFFFFFF / (PUNY_BASE - t)) {
                return -1;
            }
            w *= (PUNY_BASE - t);
        }

        bias = puny_adapt(i - oldi, (int)out + 1, oldi == 0);

        if (i / ((int)out + 1) > 0x7FFFFFFF - n) {
            return -1;
        }
        n += i / ((int)out + 1);
        i %= (int)out + 1;

        if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) {
            return -1;  // not a valid Unicode scalar
        }

        if (out >= out_cap) {
            return -1;
        }

        // Insert n at position i, shifting tail right.
        for (uint32_t k = out; k > (uint32_t)i; k--) {
            output[k] = output[k - 1];
        }
        output[i] = (n00b_codepoint_t)n;
        out++;
        i++;
    }

    return (int32_t)out;
}

// ---------------------------------------------------------------------------
// UTS #46 label validity (§4.1) and BIDI rule (RFC 5893)
// ---------------------------------------------------------------------------

// Test whether @p cp belongs to general categories Mn, Mc, or Me (combining
// marks for the purposes of UTS #46 §4.1 V5).
static bool
is_combining_mark(n00b_codepoint_t cp)
{
    n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
    return gc == N00B_UNICODE_GC_MN
        || gc == N00B_UNICODE_GC_MC
        || gc == N00B_UNICODE_GC_ME;
}

// RFC 5893 BIDI rule.  A label that contains any character with Bidi_Class R,
// AL, or AN is an "RTL label"; otherwise it is an "LTR label".  The full rule:
//
//   1. The first character of an RTL label must have Bidi_Class L, R, or AL.
//   2. In an RTL label, only characters with Bidi_Class R, AL, AN, EN, ES,
//      CS, ET, ON, BN, or NSM are allowed.
//   3. In an RTL label, the end of the label must be a character with
//      Bidi_Class R, AL, EN, or AN, followed by zero or more NSM.
//   4. In an RTL label, EN and AN must not both appear.
//   5. In an LTR label, only characters with Bidi_Class L, EN, ES, CS, ET,
//      ON, BN, or NSM are allowed.
//   6. In an LTR label, the end of the label must be a character with
//      Bidi_Class L or EN, followed by zero or more NSM.
//
// The rule applies only if the whole domain is a "Bidi domain name" (contains
// at least one RTL character).  Callers check that before invoking.
static bool
validate_bidi_label(const n00b_codepoint_t *cps, uint32_t len)
{
    if (len == 0) {
        return true;
    }

    bool is_rtl = false;
    for (uint32_t i = 0; i < len; i++) {
        n00b_unicode_bidi_class_t bc = n00b_unicode_bidi_class(cps[i]);
        if (bc == N00B_UNICODE_BIDI_R || bc == N00B_UNICODE_BIDI_AL
            || bc == N00B_UNICODE_BIDI_AN) {
            is_rtl = true;
            break;
        }
    }

    n00b_unicode_bidi_class_t first = n00b_unicode_bidi_class(cps[0]);

    if (is_rtl) {
        if (first != N00B_UNICODE_BIDI_R && first != N00B_UNICODE_BIDI_AL) {
            return false;
        }

        bool seen_en = false;
        bool seen_an = false;
        for (uint32_t i = 0; i < len; i++) {
            n00b_unicode_bidi_class_t bc = n00b_unicode_bidi_class(cps[i]);
            switch (bc) {
            case N00B_UNICODE_BIDI_R:
            case N00B_UNICODE_BIDI_AL:
            case N00B_UNICODE_BIDI_AN:
            case N00B_UNICODE_BIDI_EN:
            case N00B_UNICODE_BIDI_ES:
            case N00B_UNICODE_BIDI_CS:
            case N00B_UNICODE_BIDI_ET:
            case N00B_UNICODE_BIDI_ON:
            case N00B_UNICODE_BIDI_BN:
            case N00B_UNICODE_BIDI_NSM:
                break;
            default:
                return false;
            }
            if (bc == N00B_UNICODE_BIDI_EN)
                seen_en = true;
            if (bc == N00B_UNICODE_BIDI_AN)
                seen_an = true;
        }
        if (seen_en && seen_an) {
            return false;
        }

        // Trailing character (skipping NSM) must be R, AL, EN, or AN.
        int32_t k = (int32_t)len - 1;
        while (k >= 0
               && n00b_unicode_bidi_class(cps[k]) == N00B_UNICODE_BIDI_NSM) {
            k--;
        }
        if (k < 0) {
            return false;
        }
        n00b_unicode_bidi_class_t tail = n00b_unicode_bidi_class(cps[k]);
        if (tail != N00B_UNICODE_BIDI_R && tail != N00B_UNICODE_BIDI_AL
            && tail != N00B_UNICODE_BIDI_EN && tail != N00B_UNICODE_BIDI_AN) {
            return false;
        }
        return true;
    }

    // LTR label.
    if (first != N00B_UNICODE_BIDI_L && first != N00B_UNICODE_BIDI_EN) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        n00b_unicode_bidi_class_t bc = n00b_unicode_bidi_class(cps[i]);
        switch (bc) {
        case N00B_UNICODE_BIDI_L:
        case N00B_UNICODE_BIDI_EN:
        case N00B_UNICODE_BIDI_ES:
        case N00B_UNICODE_BIDI_CS:
        case N00B_UNICODE_BIDI_ET:
        case N00B_UNICODE_BIDI_ON:
        case N00B_UNICODE_BIDI_BN:
        case N00B_UNICODE_BIDI_NSM:
            break;
        default:
            return false;
        }
    }
    int32_t k = (int32_t)len - 1;
    while (k >= 0
           && n00b_unicode_bidi_class(cps[k]) == N00B_UNICODE_BIDI_NSM) {
        k--;
    }
    if (k < 0) {
        return false;
    }
    n00b_unicode_bidi_class_t tail = n00b_unicode_bidi_class(cps[k]);
    if (tail != N00B_UNICODE_BIDI_L && tail != N00B_UNICODE_BIDI_EN) {
        return false;
    }
    return true;
}

// Does any character in @p cps have Bidi_Class R, AL, or AN?
static bool
label_has_rtl(const n00b_codepoint_t *cps, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        n00b_unicode_bidi_class_t bc = n00b_unicode_bidi_class(cps[i]);
        if (bc == N00B_UNICODE_BIDI_R || bc == N00B_UNICODE_BIDI_AL
            || bc == N00B_UNICODE_BIDI_AN) {
            return true;
        }
    }
    return false;
}

// UTS #46 §4.1 label validity criteria 1–7.  Returns true and leaves *err
// untouched on success; returns false and sets *err on failure.
//
// Criterion 1 (NFC) is enforced by the caller's NFC normalization of the
// whole domain prior to label splitting.  Criterion 6 (status check) skips
// IGNORED/MAPPED — those have already been resolved by the mapping pass —
// and rejects DISALLOWED.  Criteria 7–9 (CONTEXTJ, CONTEXTO, BIDI) are
// delegated to dedicated checkers.
static bool
validate_label_uts46(const n00b_codepoint_t    *cps,
                     uint32_t                   len,
                     bool                       check_bidi,
                     n00b_unicode_idna_error_t *err)
{
    if (len == 0) {
        if (err) *err = N00B_UNICODE_IDNA_EMPTY_LABEL;
        return false;
    }

    // V3: must not begin or end with U+002D.
    if (cps[0] == '-' || cps[len - 1] == '-') {
        if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
        return false;
    }

    // V2: must not have hyphens in both positions 3 and 4 unless the label
    // is an ACE label that has already been decoded — by the time we land
    // here the `xn--` prefix has been stripped, so any decoded label that
    // still has "--" at positions 3,4 is a violation.
    if (len >= 4 && cps[2] == '-' && cps[3] == '-') {
        if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
        return false;
    }

    // V4: must not contain U+002E FULL STOP.
    for (uint32_t i = 0; i < len; i++) {
        if (cps[i] == '.') {
            if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
            return false;
        }
    }

    // V5: first character must not be a combining mark.
    if (is_combining_mark(cps[0])) {
        if (err) *err = N00B_UNICODE_IDNA_LEADING_COMBINING;
        return false;
    }

    // V6: every code point must have IDNA status VALID (DEVIATION is
    // accepted for non-transitional processing, which is the default in
    // UTS #46 §4 since 2015).
    for (uint32_t i = 0; i < len; i++) {
        uint8_t st = get_idna_status(cps[i]);
        if (st == IDNA_DISALLOWED) {
            if (err) *err = N00B_UNICODE_IDNA_DISALLOWED;
            return false;
        }
        // MAPPED / IGNORED at this point means the caller fed an unmapped
        // domain into validation — also a violation.
        if (st == IDNA_MAPPED || st == IDNA_IGNORED) {
            if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
            return false;
        }
    }

    // V7 / V8 — CONTEXTJ and CONTEXTO.
    if (!validate_label_context(cps, len, err)) {
        return false;
    }

    // V9 — BIDI rule.  Only applied when the whole domain contains an RTL
    // label (caller decides; if check_bidi=true we enforce on this label).
    if (check_bidi && !validate_bidi_label(cps, len)) {
        if (err) *err = N00B_UNICODE_IDNA_BIDI_ERROR;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// UTS #46 processing
// ---------------------------------------------------------------------------

// Decode a single label's bytes into codepoints.  If the label begins with
// the ACE prefix "xn--", strip it and Punycode-decode the remainder; else
// UTF-8-decode the bytes as-is.  Returns the codepoint count on success,
// -1 on failure (sets *err).
static int32_t
decode_label_to_cps(const char                *label_bytes,
                    uint32_t                   label_len,
                    n00b_codepoint_t          *out_cps,
                    uint32_t                   out_cap,
                    bool                      *out_was_ace,
                    n00b_unicode_idna_error_t *err)
{
    bool ace = (label_len >= 4
                && (label_bytes[0] == 'x' || label_bytes[0] == 'X')
                && (label_bytes[1] == 'n' || label_bytes[1] == 'N')
                && label_bytes[2] == '-'
                && label_bytes[3] == '-');
    if (out_was_ace) {
        *out_was_ace = ace;
    }

    if (ace) {
        int32_t n = puny_decode(label_bytes + 4, label_len - 4, out_cps, out_cap);
        if (n < 0) {
            if (err) *err = N00B_UNICODE_IDNA_PUNYCODE_ERROR;
            return -1;
        }
        return n;
    }

    uint32_t pos      = 0;
    uint32_t cp_count = 0;
    while (pos < label_len) {
        int32_t cp = n00b_unicode_utf8_decode(label_bytes, label_len, &pos);
        if (cp < 0) {
            if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
            return -1;
        }
        if (cp_count >= out_cap) {
            if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
            return -1;
        }
        out_cps[cp_count++] = (n00b_codepoint_t)cp;
    }
    return (int32_t)cp_count;
}

// Apply UTS #46 §4 steps 1+2 (mapping + NFC).  Returns the normalized form,
// or nullptr (with *err set) on failure.
static n00b_string_t *
map_and_normalize(n00b_allocator_t          *allocator,
                  const char                *domain,
                  uint32_t                   num_bytes,
                  n00b_unicode_idna_error_t *err)
{
    char    *mapped  = n00b_alloc_array(char, num_bytes * 12 + 1);
    uint32_t map_pos = 0;
    uint32_t pos     = 0;

    while (pos < num_bytes) {
        int32_t cp = n00b_unicode_utf8_decode(domain, num_bytes, &pos);
        if (cp < 0) {
            break;
        }

        uint8_t status = get_idna_status((n00b_codepoint_t)cp);
        switch (status) {
        case IDNA_VALID:
        case IDNA_DEVIATION:
            map_pos += n00b_unicode_utf8_encode((n00b_codepoint_t)cp,
                                                mapped + map_pos);
            break;
        case IDNA_IGNORED:
            break;
        case IDNA_MAPPED: {
            const uint32_t *entry = n00b_unicode_sparse_lookup(
                n00b_unicode_idna_map_index,
                n00b_unicode_idna_map_index_len,
                n00b_unicode_idna_map_data,
                (n00b_codepoint_t)cp);
            if (entry) {
                uint32_t count = entry[0];
                for (uint32_t i = 0; i < count; i++) {
                    map_pos += n00b_unicode_utf8_encode(entry[1 + i],
                                                        mapped + map_pos);
                }
            }
            break;
        }
        case IDNA_DISALLOWED:
        default:
            if (err) *err = N00B_UNICODE_IDNA_DISALLOWED;
            n00b_free(mapped);
            return nullptr;
        }
    }
    mapped[map_pos] = '\0';

    n00b_string_t *normalized = n00b_unicode_nfc_raw(allocator, mapped, map_pos);
    n00b_free(mapped);
    return normalized;
}

// First pass over the normalized domain: decode every label to codepoints
// and report whether the domain is a "Bidi domain name" (i.e. some label
// has an R / AL / AN code point) per RFC 5893 §1.4.  Returns true on
// success and sets *out_bidi.  On failure returns false with *err set.
static bool
domain_is_bidi(n00b_string_t            *normalized,
               bool                     *out_bidi,
               n00b_unicode_idna_error_t *err)
{
    *out_bidi = false;
    uint32_t norm_bytes = (uint32_t)normalized->u8_bytes;

    uint32_t pos         = 0;
    uint32_t label_start = 0;
    while (pos <= norm_bytes) {
        int32_t  cp       = -1;
        uint32_t save_pos = pos;
        if (pos < norm_bytes) {
            cp = n00b_unicode_utf8_decode(normalized->data, norm_bytes, &pos);
            if (cp < 0) {
                if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
                return false;
            }
        }
        bool is_dot = (cp == '.' || cp == 0x3002 || cp == 0xFF0E
                       || cp == 0xFF61);
        bool is_end = (save_pos >= norm_bytes);

        if (is_dot || is_end) {
            uint32_t          lbytes = save_pos - label_start;
            if (lbytes > 0) {
                n00b_codepoint_t *cps = n00b_alloc_array(n00b_codepoint_t,
                                                         lbytes + 1);
                int32_t n = decode_label_to_cps(normalized->data + label_start,
                                                lbytes, cps, lbytes + 1,
                                                nullptr, err);
                if (n < 0) {
                    n00b_free(cps);
                    return false;
                }
                if (label_has_rtl(cps, (uint32_t)n)) {
                    *out_bidi = true;
                    n00b_free(cps);
                    return true;
                }
                n00b_free(cps);
            }
            label_start = pos;
            if (is_end) break;
        }
    }
    return true;
}

n00b_string_t *
n00b_unicode_idna_to_ascii_raw(n00b_allocator_t          *allocator,
                               const char                *domain,
                               int64_t                    len,
                               n00b_unicode_idna_error_t *err)
{
    if (err) *err = N00B_UNICODE_IDNA_OK;

    uint32_t num_bytes = (uint32_t)len;

    n00b_string_t *normalized = map_and_normalize(allocator, domain, num_bytes,
                                                  err);
    if (!normalized) {
        return n00b_string_empty(.allocator = allocator);
    }

    bool check_bidi = false;
    if (!domain_is_bidi(normalized, &check_bidi, err)) {
        return n00b_string_empty(.allocator = allocator);
    }

    char    *result      = n00b_alloc_array(char, normalized->u8_bytes * 2 + 64);
    uint32_t res_pos     = 0;
    uint32_t label_start = 0;

    uint32_t pos        = 0;
    uint32_t norm_bytes = (uint32_t)normalized->u8_bytes;
    while (pos <= norm_bytes) {
        int32_t  cp       = -1;
        uint32_t save_pos = pos;
        if (pos < norm_bytes) {
            cp = n00b_unicode_utf8_decode(normalized->data, norm_bytes, &pos);
        }

        bool is_dot = (cp == '.' || cp == 0x3002 || cp == 0xFF0E
                       || cp == 0xFF61);
        bool is_end = (save_pos >= norm_bytes);

        if (is_dot || is_end) {
            uint32_t label_len_bytes = save_pos - label_start;

            if (label_len_bytes == 0) {
                if (is_dot) {
                    result[res_pos++] = '.';
                    label_start       = pos;
                    continue;
                }
                // Trailing empty label (input ends with dot) — OK.
                break;
            }

            // Decode label to codepoints (handling pre-existing xn-- input).
            n00b_codepoint_t *label_cps
                = n00b_alloc_array(n00b_codepoint_t, label_len_bytes + 1);
            bool    was_ace = false;
            int32_t lcp_n   = decode_label_to_cps(
                normalized->data + label_start, label_len_bytes,
                label_cps, label_len_bytes + 1, &was_ace, err);
            if (lcp_n < 0) {
                n00b_free(label_cps);
                n00b_free(result);
                return n00b_string_empty(.allocator = allocator);
            }

            if (!validate_label_uts46(label_cps, (uint32_t)lcp_n, check_bidi,
                                       err)) {
                n00b_free(label_cps);
                n00b_free(result);
                return n00b_string_empty(.allocator = allocator);
            }

            // Decide whether the emitted label needs ACE encoding.
            bool needs_ace = was_ace;
            if (!needs_ace) {
                for (int32_t i = 0; i < lcp_n; i++) {
                    if (label_cps[i] >= 0x80) {
                        needs_ace = true;
                        break;
                    }
                }
            }

            if (needs_ace) {
                // Cap the per-label length: ACE label including prefix must
                // not exceed 63 bytes.
                memcpy(result + res_pos, "xn--", 4);
                res_pos += 4;
                uint32_t puny_len;
                char    *puny = puny_encode(label_cps, (uint32_t)lcp_n,
                                            &puny_len);
                if (!puny || puny_len + 4 > 63) {
                    if (puny) n00b_free(puny);
                    n00b_free(label_cps);
                    n00b_free(result);
                    if (err) *err = N00B_UNICODE_IDNA_LABEL_TOO_LONG;
                    return n00b_string_empty(.allocator = allocator);
                }
                memcpy(result + res_pos, puny, puny_len);
                res_pos += puny_len;
                n00b_free(puny);
            }
            else {
                if (label_len_bytes > 63) {
                    n00b_free(label_cps);
                    n00b_free(result);
                    if (err) *err = N00B_UNICODE_IDNA_LABEL_TOO_LONG;
                    return n00b_string_empty(.allocator = allocator);
                }
                memcpy(result + res_pos, normalized->data + label_start,
                       label_len_bytes);
                res_pos += label_len_bytes;
            }
            n00b_free(label_cps);

            if (is_dot) {
                result[res_pos++] = '.';
            }
            label_start = pos;
        }

        if (is_end) break;
    }

    result[res_pos] = '\0';

    if (res_pos > 253) {
        if (err) *err = N00B_UNICODE_IDNA_DOMAIN_TOO_LONG;
        n00b_free(result);
        return n00b_string_empty(.allocator = allocator);
    }

    n00b_string_t *out
        = n00b_string_from_raw(result, res_pos, .allocator = allocator);
    n00b_free(result);
    return out;
}

n00b_unicode_idna_result_t
n00b_unicode_idna_to_ascii(n00b_string_t *domain) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;
    n00b_unicode_idna_error_t err = N00B_UNICODE_IDNA_OK;
    n00b_string_t            *value
        = n00b_unicode_idna_to_ascii_raw(allocator, domain->data, domain->u8_bytes, &err);
    return (n00b_unicode_idna_result_t){.value = value, .error = err};
}

n00b_string_t *
n00b_unicode_idna_to_unicode_raw(n00b_allocator_t          *allocator,
                                 const char                *domain,
                                 int64_t                    len,
                                 n00b_unicode_idna_error_t *err)
{
    if (err) *err = N00B_UNICODE_IDNA_OK;

    uint32_t num_bytes = (uint32_t)len;

    n00b_string_t *normalized = map_and_normalize(allocator, domain, num_bytes,
                                                  err);
    if (!normalized) {
        return n00b_string_empty(.allocator = allocator);
    }

    bool check_bidi = false;
    if (!domain_is_bidi(normalized, &check_bidi, err)) {
        return n00b_string_empty(.allocator = allocator);
    }

    // Output buffer: each decoded codepoint takes up to 4 UTF-8 bytes; the
    // worst case per byte of normalized input is ~4× when an "xn--" label
    // expands.  Give ample headroom.
    uint32_t norm_bytes = (uint32_t)normalized->u8_bytes;
    char    *result     = n00b_alloc_array(char, norm_bytes * 4 + 64);
    uint32_t res_pos    = 0;

    uint32_t pos         = 0;
    uint32_t label_start = 0;
    while (pos <= norm_bytes) {
        int32_t  cp       = -1;
        uint32_t save_pos = pos;
        if (pos < norm_bytes) {
            cp = n00b_unicode_utf8_decode(normalized->data, norm_bytes, &pos);
            if (cp < 0) {
                n00b_free(result);
                if (err) *err = N00B_UNICODE_IDNA_PROCESSING_ERROR;
                return n00b_string_empty(.allocator = allocator);
            }
        }

        bool is_dot = (cp == '.' || cp == 0x3002 || cp == 0xFF0E
                       || cp == 0xFF61);
        bool is_end = (save_pos >= norm_bytes);

        if (is_dot || is_end) {
            uint32_t label_len_bytes = save_pos - label_start;
            if (label_len_bytes == 0) {
                if (is_dot) {
                    result[res_pos++] = '.';
                    label_start       = pos;
                    continue;
                }
                break;  // trailing empty label
            }

            n00b_codepoint_t *label_cps
                = n00b_alloc_array(n00b_codepoint_t, label_len_bytes + 1);
            bool    was_ace = false;
            int32_t lcp_n   = decode_label_to_cps(
                normalized->data + label_start, label_len_bytes,
                label_cps, label_len_bytes + 1, &was_ace, err);
            if (lcp_n < 0) {
                n00b_free(label_cps);
                n00b_free(result);
                return n00b_string_empty(.allocator = allocator);
            }

            if (!validate_label_uts46(label_cps, (uint32_t)lcp_n, check_bidi,
                                       err)) {
                n00b_free(label_cps);
                n00b_free(result);
                return n00b_string_empty(.allocator = allocator);
            }

            // Emit the U-label form: encode codepoints back to UTF-8.
            for (int32_t i = 0; i < lcp_n; i++) {
                res_pos += n00b_unicode_utf8_encode(label_cps[i],
                                                    result + res_pos);
            }
            n00b_free(label_cps);

            if (is_dot) {
                result[res_pos++] = '.';
            }
            label_start = pos;
        }

        if (is_end) break;
    }

    result[res_pos] = '\0';

    n00b_string_t *out
        = n00b_string_from_raw(result, res_pos, .allocator = allocator);
    n00b_free(result);
    return out;
}

n00b_unicode_idna_result_t
n00b_unicode_idna_to_unicode(n00b_string_t *domain) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;
    n00b_unicode_idna_error_t err = N00B_UNICODE_IDNA_OK;
    n00b_string_t            *value
        = n00b_unicode_idna_to_unicode_raw(allocator, domain->data, domain->u8_bytes, &err);
    return (n00b_unicode_idna_result_t){.value = value, .error = err};
}
