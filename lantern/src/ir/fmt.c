#include <limits.h>
#include <stdlib.h>

#include "private/ir/internal.h"

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

core_string_t ir_lit_text(ir_lit_t v)
{
    core_buffer_t *b = core_buffer_empty();

    ir_lit_into(b, v);

    return core_buffer_take_string(b);
}
