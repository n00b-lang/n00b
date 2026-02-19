#include "unicode/properties.h"
#include "unicode/encoding.h"
#include "internal/unicode/raw.h"
#include "core/alloc.h"
#include "internal/unicode/tables.h"

// External tables from generated files
extern const uint16_t n00b_unicode_gc_stage1[];
extern const uint8_t  n00b_unicode_gc_stage2[];

extern const uint16_t n00b_unicode_ccc_stage1[];
extern const uint8_t  n00b_unicode_ccc_stage2[];

extern const uint16_t n00b_unicode_script_stage1[];
extern const uint8_t  n00b_unicode_script_stage2[];
extern const char    *n00b_unicode_script_names[];
extern const uint32_t n00b_unicode_script_count;

extern const uint32_t n00b_unicode_block_ranges[][2];
extern const uint16_t n00b_unicode_block_ids[];
extern const uint32_t n00b_unicode_block_count;
extern const char    *n00b_unicode_block_names[];

extern const uint16_t n00b_unicode_bidi_stage1[];
extern const uint8_t  n00b_unicode_bidi_stage2[];

extern const uint16_t n00b_unicode_eaw_stage1[];
extern const uint8_t  n00b_unicode_eaw_stage2[];

extern const uint16_t n00b_unicode_jt_stage1[];
extern const uint8_t  n00b_unicode_jt_stage2[];

extern const uint16_t n00b_unicode_props_stage1[];
extern const uint64_t n00b_unicode_props_stage2[];

extern const uint32_t n00b_unicode_script_ext_index[][2];
extern const uint32_t n00b_unicode_script_ext_index_len;
extern const uint32_t n00b_unicode_script_ext_data[];

extern const uint32_t n00b_unicode_numeric_index[][2];
extern const uint32_t n00b_unicode_numeric_index_len;
extern const uint32_t n00b_unicode_numeric_data[];

n00b_unicode_gc_t
n00b_unicode_general_category(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_GC_CN;
    return (n00b_unicode_gc_t)N00B_UNICODE_LOOKUP(n00b_unicode_gc_stage1,
                                                  n00b_unicode_gc_stage2,
                                                  cp);
}

uint8_t
n00b_unicode_combining_class(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return 0;
    return N00B_UNICODE_LOOKUP(n00b_unicode_ccc_stage1, n00b_unicode_ccc_stage2, cp);
}

n00b_unicode_script_t
n00b_unicode_script(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return 0;
    return N00B_UNICODE_LOOKUP(n00b_unicode_script_stage1, n00b_unicode_script_stage2, cp);
}

const char *
n00b_unicode_script_name(n00b_unicode_script_t s)
{
    if (s >= n00b_unicode_script_count)
        return "Unknown";
    return n00b_unicode_script_names[s];
}

n00b_unicode_block_t
n00b_unicode_block(n00b_codepoint_t cp)
{
    // Binary search in block ranges
    uint32_t lo = 0;
    uint32_t hi = n00b_unicode_block_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cp < n00b_unicode_block_ranges[mid][0]) {
            hi = mid;
        }
        else if (cp > n00b_unicode_block_ranges[mid][1]) {
            lo = mid + 1;
        }
        else {
            return n00b_unicode_block_ids[mid];
        }
    }
    return 0; // No_Block
}

const char *
n00b_unicode_block_name(n00b_unicode_block_t b)
{
    return n00b_unicode_block_names[b];
}

n00b_unicode_bidi_class_t
n00b_unicode_bidi_class(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_BIDI_L;
    return (n00b_unicode_bidi_class_t)N00B_UNICODE_LOOKUP(n00b_unicode_bidi_stage1,
                                                          n00b_unicode_bidi_stage2,
                                                          cp);
}

n00b_unicode_eaw_t
n00b_unicode_east_asian_width(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_EAW_N;
    return (n00b_unicode_eaw_t)N00B_UNICODE_LOOKUP(n00b_unicode_eaw_stage1,
                                                   n00b_unicode_eaw_stage2,
                                                   cp);
}

int
n00b_unicode_char_width(n00b_codepoint_t cp)
{
    // NUL
    if (cp == 0)
        return 0;

    // C0/C1 control chars
    n00b_unicode_gc_t gc = n00b_unicode_general_category(cp);
    if (gc == N00B_UNICODE_GC_CC || gc == N00B_UNICODE_GC_CF || gc == N00B_UNICODE_GC_MN
        || gc == N00B_UNICODE_GC_ME || gc == N00B_UNICODE_GC_MC) {
        // Combining marks and control characters have zero width
        // Exception: U+00AD SOFT HYPHEN is Cf but width 1
        if (cp == 0x00AD)
            return 1;
        if (gc == N00B_UNICODE_GC_MC)
            return 0; // spacing combining marks
        if (gc == N00B_UNICODE_GC_CC || gc == N00B_UNICODE_GC_CF)
            return 0;
        return 0; // Mn, Me
    }

    n00b_unicode_eaw_t eaw = n00b_unicode_east_asian_width(cp);
    if (eaw == N00B_UNICODE_EAW_W || eaw == N00B_UNICODE_EAW_F)
        return 2;

    return 1;
}

n00b_unicode_jt_t
n00b_unicode_joining_type(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_JT_U;
    return (n00b_unicode_jt_t)N00B_UNICODE_LOOKUP(n00b_unicode_jt_stage1,
                                                  n00b_unicode_jt_stage2,
                                                  cp);
}

bool
n00b_unicode_has_property(n00b_codepoint_t cp, n00b_unicode_property_t prop)
{
    if (cp >= 0x110000)
        return false;
    uint64_t bits
        = N00B_UNICODE_LOOKUP(n00b_unicode_props_stage1, n00b_unicode_props_stage2, cp);
    return (bits >> prop) & 1;
}

int
n00b_unicode_script_extensions(n00b_codepoint_t       cp,
                               n00b_unicode_script_t *scripts,
                               int                    max_scripts)
{
    if (cp >= 0x110000 || max_scripts <= 0)
        return 0;

    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_script_ext_index,
                                                       n00b_unicode_script_ext_index_len,
                                                       n00b_unicode_script_ext_data,
                                                       cp);

    if (entry) {
        uint32_t count = entry[0];
        int      n     = (int)count < max_scripts ? (int)count : max_scripts;
        for (int i = 0; i < n; i++) {
            scripts[i] = (n00b_unicode_script_t)entry[1 + i];
        }
        return n;
    }

    // Fallback: singleton from Script property
    scripts[0] = n00b_unicode_script(cp);
    return 1;
}

n00b_unicode_numeric_type_t
n00b_unicode_numeric_type(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return N00B_UNICODE_NUMERIC_NONE;
    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_numeric_index,
                                                       n00b_unicode_numeric_index_len,
                                                       n00b_unicode_numeric_data,
                                                       cp);
    if (!entry)
        return N00B_UNICODE_NUMERIC_NONE;
    // entry[0] = count (3), entry[1] = type, entry[2] = numerator, entry[3] =
    // denominator
    return (n00b_unicode_numeric_type_t)entry[1];
}

n00b_unicode_numeric_value_t
n00b_unicode_numeric_value(n00b_codepoint_t cp)
{
    n00b_unicode_numeric_value_t result = {N00B_UNICODE_NUMERIC_NONE, 0, 1};
    if (cp >= 0x110000)
        return result;

    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_numeric_index,
                                                       n00b_unicode_numeric_index_len,
                                                       n00b_unicode_numeric_data,
                                                       cp);
    if (!entry)
        return result;

    result.type        = (n00b_unicode_numeric_type_t)entry[1];
    result.numerator   = (int32_t)entry[2];
    result.denominator = (int32_t)entry[3];
    return result;
}

n00b_unicode_opt_i32_t
n00b_unicode_digit_value(n00b_codepoint_t cp)
{
    if (cp >= 0x110000)
        return (n00b_unicode_opt_i32_t){.has_value = false};
    const uint32_t *entry = n00b_unicode_sparse_lookup(n00b_unicode_numeric_index,
                                                       n00b_unicode_numeric_index_len,
                                                       n00b_unicode_numeric_data,
                                                       cp);
    if (!entry)
        return (n00b_unicode_opt_i32_t){.has_value = false};
    uint32_t type = entry[1];
    if (type != 1 && type != 2)
        return (n00b_unicode_opt_i32_t){.has_value = false}; // Only Decimal and Digit
    int32_t val = (int32_t)entry[2];
    if (val < 0 || val > 9)
        return (n00b_unicode_opt_i32_t){.has_value = false};
    return (n00b_unicode_opt_i32_t){.has_value = true, .value = val};
}

int32_t
n00b_unicode_display_width_raw(const char *data, int64_t len)
{
    int32_t  width = 0;
    uint32_t pos   = 0;

    while (pos < (uint32_t)len) {
        int32_t cp = n00b_unicode_utf8_decode(data, (uint32_t)len, &pos);
        if (cp < 0)
            break;
        width += n00b_unicode_char_width((n00b_codepoint_t)cp);
    }

    return width;
}

int32_t
n00b_unicode_display_width(n00b_string_t s)
{
    return n00b_unicode_display_width_raw(s.data, s.u8_bytes);
}
