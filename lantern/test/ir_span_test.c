#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"

static int fails = 0;

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static uint32_t scalars_in(const char *s, size_t n)
{
    uint32_t k = 0;

    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            k++;
        }
    }

    return k;
}

static void locate(const char *src, uint32_t byte, uint32_t *line,
                   uint32_t *col)
{
    uint32_t ln = 1;
    uint32_t start = 0;

    for (uint32_t i = 0; i < byte; i++) {
        if (src[i] == '\n') {
            ln++;
            start = i + 1;
        }
    }

    *line = ln;
    *col  = scalars_in(src + start, byte - start) + 1;
}

static uint64_t seed = 0x9E3779B97F4A7C15u;

static uint32_t rnd(uint32_t bound)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;

    return bound ? (uint32_t)(seed % bound) : 0;
}

static const char *corpus[] = {
    "func f() {\n  x = 1\n}\n",
    "a\nbb\nccc\ndddd\n",
    "caf\xc3\xa9 = 1\nna\xc3\xafve = 2\n",
    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e = 3\nx = 4\n",
    "\xf0\x9f\x8e\x89 party\nline two\n",
    "no trailing newline",
};

static void test_roundtrip(void)
{
    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "test");

    for (size_t c = 0; c < sizeof(corpus) / sizeof(corpus[0]); c++) {
        const char *src = corpus[c];
        size_t      len = strlen(src);
        int32_t     f   = ir_file_add(m, "probe");

        for (int trial = 0; trial < 400; trial++) {
            uint32_t sb = rnd((uint32_t)len + 1);
            uint32_t eb = sb + rnd((uint32_t)len + 1 - sb);

            while (sb < len && ((unsigned char)src[sb] & 0xC0) == 0x80) {
                sb++;
            }

            while (eb < len && ((unsigned char)src[eb] & 0xC0) == 0x80) {
                eb++;
            }

            if (eb < sb) {
                eb = sb;
            }

            uint32_t sl, sc, el, ec;

            locate(src, sb, &sl, &sc);
            locate(src, eb, &el, &ec);

            ir_span_t s = ir_span_make(f, sb, eb, sl, sc, el, ec);

            if (!ir_span_known(s)) {
                check("a span over a real file is known", false);

                return;
            }

            uint32_t rl, rc;

            locate(src, s.start_byte, &rl, &rc);

            if (rl != s.start_line || rc != s.start_col) {
                printf("FAIL  span byte %u disagrees with line/col: "
                       "%u:%u vs %u:%u (corpus %zu)\n",
                       s.start_byte, s.start_line, s.start_col, rl, rc, c);
                fails++;

                return;
            }

            locate(src, s.end_byte, &rl, &rc);

            if (rl != s.end_line || rc != s.end_col) {
                printf("FAIL  end byte %u disagrees with line/col (corpus "
                       "%zu)\n", s.end_byte, c);
                fails++;

                return;
            }

            if (s.start_byte > s.end_byte) {
                check("a span does not run backwards", false);

                return;
            }
        }
    }

    ir_ctx_free(ctx);
}

static void test_columns_are_scalars(void)
{
    const char *src = "caf\xc3\xa9 = 1\n";
    uint32_t    line, col;

    locate(src, 5, &line, &col);
    check("a two-byte scalar counts as one column",
          line == 1 && col == 5);

    locate(src, 3, &line, &col);
    check("columns before a multibyte scalar are unaffected",
          line == 1 && col == 4);

    const char *emoji = "\xf0\x9f\x8e\x89 x\n";

    locate(emoji, 4, &line, &col);
    check("a four-byte scalar counts as one column",
          line == 1 && col == 2);

    const char *two = "a\nb\n";

    locate(two, 2, &line, &col);
    check("lines are 1-based and reset the column",
          line == 2 && col == 1);

    locate(two, 0, &line, &col);
    check("the first position is 1:1", line == 1 && col == 1);
}

int main(void)
{
    test_columns_are_scalars();
    test_roundtrip();

    if (!fails) {
        printf("ir spans: all checks passed\n");
    }

    return fails ? 1 : 0;
}
