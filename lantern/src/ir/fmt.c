#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"

static void escape_n(core_buffer_t *b, const char *s, size_t length)
{
    for (size_t i = 0; s && i < length; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            core_buffer_puts(b, "\\\"");
            break;
        case '\\':
            core_buffer_puts(b, "\\\\");
            break;
        case '\n':
            core_buffer_puts(b, "\\n");
            break;
        case '\t':
            core_buffer_puts(b, "\\t");
            break;
        case '\r':
            core_buffer_puts(b, "\\r");
            break;
        default:
            if (c < 0x20 || c == 0x7f) {
                core_buffer_printf(b, "\\u%04x", c);
            }
            else {
                core_buffer_append(b, (const char *)&s[i], 1);
            }
        }
    }
}

void ir_quote(core_buffer_t *b, const char *s)
{
    ir_quote_n(b, s, s ? strlen(s) : 0);
}

void ir_quote_n(core_buffer_t *b, const char *s, size_t length)
{
    core_buffer_puts(b, "\"");
    escape_n(b, s, length);
    core_buffer_puts(b, "\"");
}

void ir_lit_into(core_buffer_t *b, ir_lit_t v)
{
    static const char HEX[] = "0123456789abcdef";
    char              digits[sizeof(ir_uint_t) * CHAR_BIT];
    size_t            n    = 0;
    ir_uint_t         u    = v.mag;
    unsigned          base = v.hex ? 16u : 10u;

    if (v.neg && v.mag) {
        core_buffer_puts(b, "-");
    }

    if (v.hex) {
        core_buffer_puts(b, "0x");
    }

    do {
        digits[n++] = HEX[(unsigned)(u % base)];
        u /= base;
    } while (u);

    while (n) {
        char c = digits[--n];

        core_buffer_append(b, &c, 1);
    }
}

char *ir_lit_text(ir_lit_t v)
{
    core_buffer_t *b = core_buffer_empty();

    ir_lit_into(b, v);

    return core_buffer_take(b);
}
