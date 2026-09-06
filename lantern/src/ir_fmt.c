#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"

static void escape(ncc_buffer_t *b, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '"':
            ncc_buffer_puts(b, "\\\"");
            break;
        case '\\':
            ncc_buffer_puts(b, "\\\\");
            break;
        case '\n':
            ncc_buffer_puts(b, "\\n");
            break;
        case '\t':
            ncc_buffer_puts(b, "\\t");
            break;
        case '\r':
            ncc_buffer_puts(b, "\\r");
            break;
        default:
            if (*p < 0x20) {
                ncc_buffer_printf(b, "\\u%04x", *p);
            }
            else {
                ncc_buffer_append(b, (const char *)p, 1);
            }
        }
    }
}

void ir_quote(ncc_buffer_t *b, const char *s)
{
    ncc_buffer_puts(b, "\"");
    escape(b, s);
    ncc_buffer_puts(b, "\"");
}

void ir_lit_into(ncc_buffer_t *b, ir_lit_t v)
{
    static const char HEX[] = "0123456789abcdef";
    char              digits[sizeof(ir_uint_t) * CHAR_BIT];
    size_t            n    = 0;
    ir_uint_t         u    = v.mag;
    unsigned          base = v.hex ? 16u : 10u;

    if (v.neg && v.mag) {
        ncc_buffer_puts(b, "-");
    }

    if (v.hex) {
        ncc_buffer_puts(b, "0x");
    }

    do {
        digits[n++] = HEX[(unsigned)(u % base)];
        u /= base;
    } while (u);

    while (n) {
        char c = digits[--n];

        ncc_buffer_append(b, &c, 1);
    }
}

char *ir_lit_text(ir_lit_t v)
{
    ncc_buffer_t *b = ncc_buffer_empty();

    ir_lit_into(b, v);

    return ncc_buffer_take(b);
}
