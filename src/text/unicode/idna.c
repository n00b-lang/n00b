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

// ---------------------------------------------------------------------------
// UTS #46 processing
// ---------------------------------------------------------------------------

n00b_string_t *
n00b_unicode_idna_to_ascii_raw(n00b_allocator_t          *allocator,
                               const char                *domain,
                               int64_t                    len,
                               n00b_unicode_idna_error_t *err)
{
    if (err)
        *err = N00B_UNICODE_IDNA_OK;

    uint32_t num_bytes = (uint32_t)len;

    // Step 1: Map
    char    *mapped  = n00b_alloc_array(char, num_bytes * 12 + 1);
    uint32_t map_pos = 0;
    uint32_t pos     = 0;

    while (pos < num_bytes) {
        int32_t cp = n00b_unicode_utf8_decode(domain, num_bytes, &pos);
        if (cp < 0)
            break;

        uint8_t status = get_idna_status((n00b_codepoint_t)cp);
        switch (status) {
        case IDNA_VALID:
        case IDNA_DEVIATION:
            map_pos += n00b_unicode_utf8_encode((n00b_codepoint_t)cp, mapped + map_pos);
            break;
        case IDNA_IGNORED:
            break; // skip
        case IDNA_MAPPED: {
            const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_idna_map_index,
                                                               n00b_unicode_idna_map_index_len,
                                                               n00b_unicode_idna_map_data,
                                                               (n00b_codepoint_t)cp);
            if (entry) {
                uint32_t count = entry[0];
                for (uint32_t i = 0; i < count; i++) {
                    map_pos += n00b_unicode_utf8_encode(entry[1 + i], mapped + map_pos);
                }
            }
            break;
        }
        case IDNA_DISALLOWED:
        default:
            if (err)
                *err = N00B_UNICODE_IDNA_DISALLOWED;
            n00b_free(mapped);
            return n00b_string_empty(.allocator = allocator);
        }
    }
    mapped[map_pos] = '\0';

    // Step 2: Normalize to NFC
    n00b_string_t *normalized = n00b_unicode_nfc_raw(allocator, mapped, map_pos);
    n00b_free(mapped);

    // Step 3: Break into labels at '.'
    // Step 4: Convert non-ASCII labels to Punycode
    char    *result      = n00b_alloc_array(char, normalized->u8_bytes * 2 + 64);
    uint32_t res_pos     = 0;
    uint32_t label_start = 0;

    pos                 = 0;
    uint32_t norm_bytes = (uint32_t)normalized->u8_bytes;
    while (pos <= norm_bytes) {
        int32_t  cp       = -1;
        uint32_t save_pos = pos;
        if (pos < norm_bytes) {
            cp = n00b_unicode_utf8_decode(normalized->data, norm_bytes, &pos);
        }

        bool is_dot = (cp == '.' || cp == 0x3002 || cp == 0xFF0E || cp == 0xFF61);
        bool is_end = (save_pos >= norm_bytes);

        if (is_dot || is_end) {
            uint32_t label_len_bytes = save_pos - label_start;

            if (label_len_bytes == 0 && !is_end) {
                // Empty label
                result[res_pos++] = '.';
                label_start       = pos;
                continue;
            }

            // Check if label is all ASCII
            bool all_ascii = true;
            for (uint32_t i = label_start; i < save_pos; i++) {
                if ((uint8_t)normalized->data[i] > 0x7F) {
                    all_ascii = false;
                    break;
                }
            }

            if (all_ascii) {
                memcpy(result + res_pos, normalized->data + label_start, label_len_bytes);
                res_pos += label_len_bytes;
            }
            else {
                // Decode label to codepoints for Punycode
                n00b_codepoint_t *label_cps
                    = n00b_alloc_array(char, label_len_bytes * sizeof(n00b_codepoint_t));
                uint32_t label_cp_count = 0;
                uint32_t lp             = label_start;
                while (lp < save_pos) {
                    int32_t lcp = n00b_unicode_utf8_decode(normalized->data, norm_bytes, &lp);
                    if (lcp < 0)
                        break;
                    label_cps[label_cp_count++] = (n00b_codepoint_t)lcp;
                }

                // Validate CONTEXTJ/CONTEXTO rules
                if (!validate_label_context(label_cps, label_cp_count, err)) {
                    n00b_free(label_cps);
                    n00b_free(result);
                    return n00b_string_empty(.allocator = allocator);
                }

                // Add ACE prefix
                memcpy(result + res_pos, "xn--", 4);
                res_pos += 4;

                uint32_t puny_len;
                char    *puny = puny_encode(label_cps, label_cp_count, &puny_len);
                if (puny) {
                    memcpy(result + res_pos, puny, puny_len);
                    res_pos += puny_len;
                    n00b_free(puny);
                }
                n00b_free(label_cps);
            }

            if (is_dot) {
                result[res_pos++] = '.';
            }
            label_start = pos;
        }

        if (is_end)
            break;
    }

    result[res_pos] = '\0';

    // Check total length
    if (res_pos > 253) {
        if (err)
            *err = N00B_UNICODE_IDNA_DOMAIN_TOO_LONG;
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
    if (err)
        *err = N00B_UNICODE_IDNA_OK;

    uint32_t num_bytes = (uint32_t)len;

    // Simplified: for now, just return mapped + NFC normalized form
    // Full implementation would decode Punycode labels
    char    *mapped  = n00b_alloc_array(char, num_bytes * 12 + 1);
    uint32_t map_pos = 0;
    uint32_t pos     = 0;

    while (pos < num_bytes) {
        int32_t cp = n00b_unicode_utf8_decode(domain, num_bytes, &pos);
        if (cp < 0)
            break;

        uint8_t status = get_idna_status((n00b_codepoint_t)cp);
        switch (status) {
        case IDNA_VALID:
        case IDNA_DEVIATION:
            map_pos += n00b_unicode_utf8_encode((n00b_codepoint_t)cp, mapped + map_pos);
            break;
        case IDNA_IGNORED:
            break;
        case IDNA_MAPPED: {
            const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_idna_map_index,
                                                               n00b_unicode_idna_map_index_len,
                                                               n00b_unicode_idna_map_data,
                                                               (n00b_codepoint_t)cp);
            if (entry) {
                uint32_t count = entry[0];
                for (uint32_t i = 0; i < count; i++) {
                    map_pos += n00b_unicode_utf8_encode(entry[1 + i], mapped + map_pos);
                }
            }
            break;
        }
        default:
            map_pos += n00b_unicode_utf8_encode((n00b_codepoint_t)cp, mapped + map_pos);
            break;
        }
    }
    mapped[map_pos] = '\0';

    n00b_string_t *normalized = n00b_unicode_nfc_raw(allocator, mapped, map_pos);
    n00b_free(mapped);

    return normalized;
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
