#include "lib/string.h"
#include "lib/alloc.h"
#include <stdlib.h>
#include <string.h>

core_string_t
core_string_from_raw(const char *src, int64_t byte_len)
{
    core_string_t s = {0};

    if (!src || byte_len <= 0) {
        return s;
    }

    s.data = (char *)core_alloc_size(1, (size_t)byte_len + 1);
    memcpy(s.data, src, (size_t)byte_len);
    s.u8_bytes = (size_t)byte_len;

    return s;
}

core_string_t
core_string_from_cstr(const char *src)
{
    core_string_t s = {0};

    if (!src) {
        return s;
    }

    size_t len = strlen(src);

    s.data = (char *)core_alloc_size(1, len + 1);
    memcpy(s.data, src, len);
    s.u8_bytes = len;

    return s;
}

char *
core_string_to_cstr(core_string_t s)
{
    if (!s.data) {
        return nullptr;
    }

    char *text = (char *)core_alloc_size(1, s.u8_bytes + 1);

    memcpy(text, s.data, s.u8_bytes);

    return text;
}

core_string_t
core_string_borrow(const char *src, size_t byte_len)
{
    return (core_string_t){.data = (char *)src, .u8_bytes = byte_len};
}

core_string_t
core_string_dup(core_string_t s)
{
    if (!s.data) {
        return s;
    }

    if (s.u8_bytes == 0) {
        return core_string_empty();
    }

    return core_string_from_raw(s.data, (int64_t)s.u8_bytes);
}

core_string_t
core_string_empty(void)
{
    core_string_t s = {0};

    s.data = (char *)core_alloc_size(1, 1);

    return s;
}

core_string_t
core_string_slice(core_string_t s, size_t start, size_t byte_len)
{
    if (!s.data || start > s.u8_bytes) {
        return (core_string_t){.data = s.data ? s.data + s.u8_bytes : nullptr};
    }

    size_t available = s.u8_bytes - start;

    return (core_string_t){
        .data     = s.data + start,
        .u8_bytes = byte_len < available ? byte_len : available,
    };
}

bool
core_string_eq(core_string_t a, core_string_t b)
{
    if (a.u8_bytes != b.u8_bytes) {
        return false;
    }

    if (a.u8_bytes == 0) {
        return true;
    }

    return memcmp(a.data, b.data, a.u8_bytes) == 0;
}

int
core_string_cmp(core_string_t a, core_string_t b)
{
    size_t shared = a.u8_bytes < b.u8_bytes ? a.u8_bytes : b.u8_bytes;

    if (shared) {
        int order = memcmp(a.data, b.data, shared);

        if (order) {
            return order;
        }
    }

    return (a.u8_bytes > b.u8_bytes) - (a.u8_bytes < b.u8_bytes);
}

bool
core_string_starts_with(core_string_t s, core_string_t prefix)
{
    return prefix.u8_bytes <= s.u8_bytes
        && (!prefix.u8_bytes
            || memcmp(s.data, prefix.data, prefix.u8_bytes) == 0);
}

bool
core_string_ends_with(core_string_t s, core_string_t suffix)
{
    return suffix.u8_bytes <= s.u8_bytes
        && (!suffix.u8_bytes
            || memcmp(s.data + s.u8_bytes - suffix.u8_bytes, suffix.data,
                      suffix.u8_bytes)
                   == 0);
}

int64_t
core_string_find_byte(core_string_t s, char byte)
{
    if (!s.u8_bytes) {
        return -1;
    }

    const char *found = memchr(s.data, byte, s.u8_bytes);

    return found ? (int64_t)(found - s.data) : -1;
}

int64_t
core_string_rfind_byte(core_string_t s, char byte)
{
    for (size_t i = s.u8_bytes; i > 0; i--) {
        if (s.data[i - 1] == byte) {
            return (int64_t)(i - 1);
        }
    }

    return -1;
}

int64_t
core_string_find(core_string_t s, core_string_t needle)
{
    if (!needle.u8_bytes) {
        return 0;
    }

    if (needle.u8_bytes > s.u8_bytes) {
        return -1;
    }

    size_t last = s.u8_bytes - needle.u8_bytes;

    for (size_t i = 0; i <= last; i++) {
        if (s.data[i] == needle.data[0]
            && memcmp(s.data + i, needle.data, needle.u8_bytes) == 0) {
            return (int64_t)i;
        }
    }

    return -1;
}

static bool
is_space_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f'
        || c == '\r';
}

static int
digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }

    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }

    return 99;
}

typedef struct {
    uint64_t magnitude;
    size_t   consumed;
    bool     negative;
    bool     overflow;
} parsed_integer_t;

static bool
has_prefixed_digit(core_string_t s, size_t at, char lower, int base)
{
    return at + 2 < s.u8_bytes && s.data[at] == '0'
        && (s.data[at + 1] | 0x20) == lower
        && digit_value(s.data[at + 2]) < base;
}

static parsed_integer_t
parse_integer(core_string_t s, int base)
{
    parsed_integer_t result = {0};
    size_t           at     = 0;

    if (base < 0 || base == 1 || base > 36) {
        return result;
    }

    while (at < s.u8_bytes && is_space_byte(s.data[at])) {
        at++;
    }

    if (at < s.u8_bytes && (s.data[at] == '+' || s.data[at] == '-')) {
        result.negative = s.data[at] == '-';
        at++;
    }

    if ((base == 0 || base == 16) && has_prefixed_digit(s, at, 'x', 16)) {
        at += 2;
        base = 16;
    }
    else if ((base == 0 || base == 2) && has_prefixed_digit(s, at, 'b', 2)) {
        at += 2;
        base = 2;
    }
    else if (base == 0) {
        base = at < s.u8_bytes && s.data[at] == '0' ? 8 : 10;
    }

    size_t first = at;

    while (at < s.u8_bytes) {
        int digit = digit_value(s.data[at]);

        if (digit >= base) {
            break;
        }

        uint64_t next = result.magnitude * (uint64_t)base + (uint64_t)digit;

        if (result.magnitude > (UINT64_MAX - (uint64_t)digit) / (uint64_t)base) {
            result.overflow = true;
        }

        result.magnitude = next;
        at++;
    }

    result.consumed = at == first ? 0 : at;

    return result;
}

bool
core_string_parse_i64(core_string_t s, int base, int64_t *out,
                      size_t *consumed)
{
    parsed_integer_t parsed = parse_integer(s, base);
    uint64_t         limit  = parsed.negative ? (uint64_t)INT64_MAX + 1
                                              : (uint64_t)INT64_MAX;
    bool             fits   = !parsed.overflow && parsed.magnitude <= limit;
    int64_t          value  = 0;

    if (!fits) {
        value = parsed.negative ? INT64_MIN : INT64_MAX;
    }
    else if (parsed.negative) {
        value = (int64_t)(0 - parsed.magnitude);
    }
    else {
        value = (int64_t)parsed.magnitude;
    }

    if (out) {
        *out = parsed.consumed ? value : 0;
    }

    if (consumed) {
        *consumed = parsed.consumed;
    }

    return parsed.consumed && fits;
}

bool
core_string_parse_u64(core_string_t s, int base, uint64_t *out,
                      size_t *consumed)
{
    parsed_integer_t parsed = parse_integer(s, base);
    uint64_t         value  = parsed.overflow   ? UINT64_MAX
                            : parsed.negative ? 0 - parsed.magnitude
                                              : parsed.magnitude;

    if (out) {
        *out = parsed.consumed ? value : 0;
    }

    if (consumed) {
        *consumed = parsed.consumed;
    }

    return parsed.consumed && !parsed.overflow;
}

bool
core_string_parse_f64(core_string_t s, double *out, size_t *consumed)
{
    char  small[128];
    char *text = s.u8_bytes < sizeof(small)
                   ? small
                   : (char *)core_alloc_size(1, s.u8_bytes + 1);

    if (s.u8_bytes) {
        memcpy(text, s.data, s.u8_bytes);
    }

    text[s.u8_bytes] = '\0';

    char  *end   = text;
    double value = strtod(text, &end);
    size_t used  = (size_t)(end - text);

    if (text != small) {
        core_free(text);
    }

    if (out) {
        *out = value;
    }

    if (consumed) {
        *consumed = used;
    }

    return used != 0;
}

long long
core_string_strtoll(core_string_t s, int base)
{
    int64_t value = 0;

    core_string_parse_i64(s, base, &value, nullptr);

    return value;
}

unsigned long long
core_string_strtoull(core_string_t s, int base)
{
    uint64_t value = 0;

    core_string_parse_u64(s, base, &value, nullptr);

    return value;
}
