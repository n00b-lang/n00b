#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/ir.h"
#include "ir/typelang.h"
#include "typehash.h"

static int fails;

static void check_line(char *line, int lineno)
{
    char *tab1 = strchr(line, '\t');

    if (!tab1) {
        return;
    }

    *tab1 = 0;

    char *tab2 = strchr(tab1 + 1, '\t');

    if (!tab2) {
        return;
    }

    *tab2 = 0;

    const char *type = line;
    const char *want = tab2 + 1;

    for (const char *p = type; *p; p++) {
        if ((unsigned char)*p >= 0x80) {
            return;
        }
    }

    ir_ctx_t   *ctx  = ir_ctx_new();
    th_ctx     *th   = ir_ctx_types(ctx);
    char       *err  = nullptr;
    th_type    *t    = ir_typelang_parse(th, type, &err);

    if (!t) {
        printf("FAIL  line %d: %s -- %s\n", lineno, type,
               err ? err : "no type");
        free(err);
        fails++;
        ir_ctx_free(ctx);

        return;
    }

    char got[32];

    snprintf(got, sizeof(got), "%llu", (unsigned long long)th_hash(th, t));

    if (strcmp(got, want)) {
        printf("FAIL  line %d: %s hashes %s, spec says %s (%s)\n", lineno,
               type, got, want, th_last_error(th) ? th_last_error(th) : "-");
        fails++;
    }

    ir_ctx_free(ctx);
}

static void test_a_type_can_be_read_out_of_a_longer_text(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);

    static const struct {
        const char *src;
        size_t      want;
    } cases[] = {
        {"i32, rest",       3 },
        {"ptr[i32] = 4",    8 },
        {"(x: int) -> void, next", 16},
        {"i32",             3 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t   used = 0;
        char    *err  = nullptr;
        th_type *t    = ir_typelang_parse_prefix(th, cases[i].src,
                                                 strlen(cases[i].src), &used,
                                                 &err);

        if (!t) {
            printf("FAIL  prefix parse of %s -- %s\n", cases[i].src,
                   err ? err : "no type");
            free(err);
            fails++;
            continue;
        }

        if (used != cases[i].want) {
            printf("FAIL  %s consumed %zu bytes, expected %zu\n",
                   cases[i].src, used, cases[i].want);
            fails++;
        }
    }

    ir_ctx_free(ctx);
}

static void test_nonsense_is_refused(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);

    static const char *const bad[] = {"[", "ptr[", "(x:", "forall[]"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *err = nullptr;

        if (ir_typelang_parse(th, bad[i], &err)) {
            printf("FAIL  %s should not parse as a type\n", bad[i]);
            fails++;
        }

        free(err);
    }

    ir_ctx_free(ctx);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "vectors.tsv";
    FILE       *f    = fopen(path, "rb");

    if (!f) {
        printf("FAIL  cannot open %s\n", path);

        return 1;
    }

    char line[8192];
    int  n = 0;
    int  lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        lineno++;

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }

        if (!len) {
            continue;
        }

        check_line(line, lineno);
        n++;
    }

    fclose(f);

    test_a_type_can_be_read_out_of_a_longer_text();
    test_nonsense_is_refused();
    ir_typelang_shutdown();

    if (!fails) {
        printf("typelang: %d spec vectors match through the grammar\n", n);
    }

    return fails ? 1 : 0;
}
