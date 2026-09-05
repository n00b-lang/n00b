#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"

static bool need(ir_sb *b, size_t n)
{
    if (b->len + n + 1 <= b->cap) {
        return true;
    }

    size_t cap = b->cap ? b->cap : 256;

    while (cap < b->len + n + 1) {
        cap *= 2;
    }

    char *q = realloc(b->p, cap);

    if (!q) {
        return false;
    }
    b->p   = q;
    b->cap = cap;

    return true;
}

void ir_sb_put(ir_sb *b, const char *s, size_t n)
{
    if (!need(b, n)) {
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void ir_sb_str(ir_sb *b, const char *s)
{
    if (s) {
        ir_sb_put(b, s, strlen(s));
    }
}

void ir_sb_vfmt(ir_sb *b, const char *fmt, va_list ap)
{
    va_list copy;

    va_copy(copy, ap);

    int n = vsnprintf(nullptr, 0, fmt, ap);

    if (n < 0 || !need(b, (size_t)n)) {
        va_end(copy);

        return;
    }
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, copy);
    va_end(copy);
    b->len += (size_t)n;
}

void ir_sb_fmt(ir_sb *b, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ir_sb_vfmt(b, fmt, ap);
    va_end(ap);
}

static void escape(ir_sb *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '"':
            ir_sb_str(b, "\\\"");
            break;
        case '\\':
            ir_sb_str(b, "\\\\");
            break;
        case '\n':
            ir_sb_str(b, "\\n");
            break;
        case '\t':
            ir_sb_str(b, "\\t");
            break;
        case '\r':
            ir_sb_str(b, "\\r");
            break;
        default:
            if (*p < 0x20) {
                ir_sb_fmt(b, "\\u%04x", *p);
            }
            else {
                ir_sb_put(b, (const char *)p, 1);
            }
        }
    }
}

void ir_sb_json(ir_sb *b, const char *s)
{
    ir_sb_str(b, "\"");
    escape(b, s);
    ir_sb_str(b, "\"");
}

void ir_sb_quote(ir_sb *b, const char *s)
{
    ir_sb_str(b, "\"");
    escape(b, s);
    ir_sb_str(b, "\"");
}

char *ir_sb_take(ir_sb *b)
{
    if (!b->p) {
        b->p = calloc(1, 1);
    }

    char *r = b->p;

    b->p   = nullptr;
    b->len = 0;
    b->cap = 0;

    return r;
}

void ir_sb_lit(ir_sb *b, ir_lit_t v)
{
    static const char HEX[] = "0123456789abcdef";
    char              digits[sizeof(ir_uint_t) * CHAR_BIT];
    size_t            n    = 0;
    ir_uint_t         u    = v.mag;
    unsigned          base = v.hex ? 16u : 10u;

    if (v.neg && v.mag) {
        ir_sb_str(b, "-");
    }

    if (v.hex) {
        ir_sb_str(b, "0x");
    }

    do {
        digits[n++] = HEX[(unsigned)(u % base)];
        u /= base;
    } while (u);

    while (n) {
        char c = digits[--n];

        ir_sb_put(b, &c, 1);
    }
}

char *ir_lit_text(ir_lit_t v)
{
    ir_sb b = {0};

    ir_sb_lit(&b, v);

    return ir_sb_take(&b);
}
