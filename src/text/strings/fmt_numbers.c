#include "text/strings/fmt_numbers.h"
#include "text/strings/fptostr.h"
#include "text/strings/string_convert.h"
#include "text/unicode/encoding.h"
#include "text/unicode/properties.h"
#include "core/alloc.h"
#include "core/string.h"
#include <string.h>

#include <stddef.h>
#define FP_TO_STRING_BUFSZ     24
#define FP_MAX_INTERNAL_SZ     100
#define FP_OFFSET              (FP_MAX_INTERNAL_SZ - FP_TO_STRING_BUFSZ)
#define MAX_INT_LEN            100
#define LONGEST_UNICODE_ESCAPE 9
#define LONGEST_PTR_REPR       18
#define MAX_CP                 0x10FFFF
#define INVALID_CP             0xFFFD

static const char hex_lower[] = "0123456789abcdef";
static const char hex_upper[] = "0123456789ABCDEF";

// ===================================================================
// Hex formatting
// ===================================================================

n00b_string_t
n00b_fmt_hex(uint64_t value) _kargs
{
    bool              caps      = false;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    if (!value) {
        return n00b_string_from_raw("0", 1, .allocator = allocator);
    }

    const char *map               = caps ? hex_upper : hex_lower;
    int         n                 = MAX_INT_LEN - 1;
    char        repr[MAX_INT_LEN] = {};

    while (value) {
        repr[--n] = map[value & 0x0f];
        value >>= 4;
    }

    int len = MAX_INT_LEN - 1 - n;
    return n00b_string_from_raw(repr + n, len, .allocator = allocator);
}

// ===================================================================
// Integer formatting
// ===================================================================

n00b_string_t
n00b_fmt_int(int64_t value) _kargs
{
    bool              commas    = false;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    if (!value) {
        return n00b_string_from_raw("0", 1, .allocator = allocator);
    }

    bool     neg                = false;
    int      n                  = MAX_INT_LEN - 1;
    char     repr[MAX_INT_LEN]  = {};
    uint64_t mag;

    if (value < 0) {
        neg = true;
        // Handle INT64_MIN safely via unsigned arithmetic.
        mag = (uint64_t)(-(value + 1)) + 1;
    }
    else {
        mag = (uint64_t)value;
    }

    if (!commas) {
        while (mag) {
            repr[--n] = (mag % 10) + '0';
            mag /= 10;
        }
    }
    else {
        int digits = 0;

        while (mag) {
            if (digits && !(digits % 3)) {
                repr[--n] = ',';
            }
            repr[--n] = (mag % 10) + '0';
            mag /= 10;
            digits++;
        }
    }

    if (neg) {
        repr[--n] = '-';
    }

    int len = MAX_INT_LEN - 1 - n;
    return n00b_string_from_raw(repr + n, len, .allocator = allocator);
}

n00b_string_t
n00b_fmt_uint(uint64_t value) _kargs
{
    bool              commas    = false;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    if (!value) {
        return n00b_string_from_raw("0", 1, .allocator = allocator);
    }

    int  n                 = MAX_INT_LEN - 1;
    char repr[MAX_INT_LEN] = {};

    if (!commas) {
        while (value) {
            repr[--n] = (value % 10) + '0';
            value /= 10;
        }
    }
    else {
        int digits = 0;

        while (value) {
            if (digits && !(digits % 3)) {
                repr[--n] = ',';
            }
            repr[--n] = (value % 10) + '0';
            value /= 10;
            digits++;
        }
    }

    int len = MAX_INT_LEN - 1 - n;
    return n00b_string_from_raw(repr + n, len, .allocator = allocator);
}

// ===================================================================
// Float formatting
// ===================================================================

n00b_string_t
n00b_fmt_float(double value) _kargs
{
    int               width     = 0;
    bool              fill      = false;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    char fprepr[FP_MAX_INTERNAL_SZ] = {};

    int  slen       = n00b_fptostr(value, fprepr + FP_OFFSET);
    int  n          = FP_OFFSET;
    bool using_sign = value < 0;

    if (width != 0 && slen < width) {
        int tofill = width - slen;

        if (fill) {
            // Zero-pad: build [sign?] + zeros + digits
            char padded[FP_MAX_INTERNAL_SZ] = {};
            int  p                          = 0;

            if (using_sign) {
                padded[p++] = fprepr[n++];
                slen--;
            }

            for (int i = 0; i < tofill; i++) {
                padded[p++] = '0';
            }

            memcpy(padded + p, fprepr + n, slen);
            p += slen;

            return n00b_string_from_raw(padded, p, .allocator = allocator);
        }
    }

    return n00b_string_from_raw(fprepr + n, slen, .allocator = allocator);
}

// ===================================================================
// Boolean formatting
// ===================================================================

n00b_string_t
n00b_fmt_bool(bool value) _kargs
{
    bool              upper     = false;
    bool              word      = false;
    bool              yn        = false;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    int val = value ? 1 : 0;

    if (upper) {
        val |= 2;
    }
    if (word) {
        val |= 4;
    }
    if (yn) {
        val |= 8;
    }

    const char *s;
    int         len;

    switch (val) {
    case 0:  s = "f";     len = 1; break;
    case 1:  s = "t";     len = 1; break;
    case 2:  s = "F";     len = 1; break;
    case 3:  s = "T";     len = 1; break;
    case 4:  s = "false"; len = 5; break;
    case 5:  s = "true";  len = 4; break;
    case 6:  s = "False"; len = 5; break;
    case 7:  s = "True";  len = 4; break;
    case 8:  s = "n";     len = 1; break;
    case 9:  s = "y";     len = 1; break;
    case 10: s = "N";     len = 1; break;
    case 11: s = "Y";     len = 1; break;
    case 12: s = "no";    len = 2; break;
    case 13: s = "yes";   len = 3; break;
    case 14: s = "No";    len = 2; break;
    case 15: s = "Yes";   len = 3; break;
    default: unreachable();
    }

    return n00b_string_from_raw(s, len, .allocator = allocator);
}

// ===================================================================
// Codepoint formatting
// ===================================================================

n00b_string_t
n00b_fmt_codepoint(n00b_codepoint_t cp) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    if (cp > MAX_CP) {
        cp = INVALID_CP;
    }

    n00b_unicode_gc_t cat = n00b_unicode_general_category(cp);

    // Non-printable categories: Cn, Cc, Cf, Cs, Co → show as U+XXXX
    switch (cat) {
    case N00B_UNICODE_GC_CN:
    case N00B_UNICODE_GC_CC:
    case N00B_UNICODE_GC_CF:
    case N00B_UNICODE_GC_CS:
    case N00B_UNICODE_GC_CO: {
        char buf[LONGEST_UNICODE_ESCAPE];
        int  i = 2;

        buf[0] = 'U';
        buf[1] = '+';

        if (cp & 0x100000) {
            buf[i++] = '1';
        }

        int extract;
        extract  = (cp >> 16) & 0x0f;
        buf[i++] = hex_upper[extract];
        extract  = (cp >> 12) & 0x0f;
        buf[i++] = hex_upper[extract];
        extract  = (cp >> 8) & 0x0f;
        buf[i++] = hex_upper[extract];
        extract  = (cp >> 4) & 0x0f;
        buf[i++] = hex_upper[extract];
        extract  = cp & 0xF;
        buf[i++] = hex_upper[extract];
        buf[i]   = 0;

        return n00b_string_from_raw(buf, i, .allocator = allocator);
    }
    default:
        return n00b_unicode_str_from_codepoint(cp, .allocator = allocator);
    }
}

// ===================================================================
// Pointer formatting
// ===================================================================

n00b_string_t
n00b_fmt_pointer(void *ptr) _kargs
{
    bool              caps      = false;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;

    char buf[LONGEST_PTR_REPR] = {'@'};

    uint64_t    as_int  = (uint64_t)ptr;
    const char *map     = caps ? hex_upper : hex_lower;
    uint64_t    extract;

    extract = as_int >> 60;
    buf[1]  = map[extract];
    extract = (as_int >> 56) & 0x0f;
    buf[2]  = map[extract];
    extract = (as_int >> 52) & 0x0f;
    buf[3]  = map[extract];
    extract = (as_int >> 48) & 0x0f;
    buf[4]  = map[extract];
    extract = (as_int >> 44) & 0x0f;
    buf[5]  = map[extract];
    extract = (as_int >> 40) & 0x0f;
    buf[6]  = map[extract];
    extract = (as_int >> 36) & 0x0f;
    buf[7]  = map[extract];
    extract = (as_int >> 32) & 0x0f;
    buf[8]  = map[extract];
    extract = (as_int >> 28) & 0x0f;
    buf[9]  = map[extract];
    extract = (as_int >> 24) & 0x0f;
    buf[10] = map[extract];
    extract = (as_int >> 20) & 0x0f;
    buf[11] = map[extract];
    extract = (as_int >> 16) & 0x0f;
    buf[12] = map[extract];
    extract = (as_int >> 12) & 0x0f;
    buf[13] = map[extract];
    extract = (as_int >> 8) & 0x0f;
    buf[14] = map[extract];
    extract = (as_int >> 4) & 0x0f;
    buf[15] = map[extract];
    extract = as_int & 0x0f;
    buf[16] = map[extract];
    buf[17] = 0;

    return n00b_string_from_raw(buf, 17, .allocator = allocator);
}
