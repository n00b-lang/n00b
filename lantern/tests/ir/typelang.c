#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "ir/ir.h"
#include "ir/typelang.h"
#include "lib/buffer.h"
#include "th/typehash.h"

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

    const char *type     = line;
    const char *want_enc = tab1 + 1;
    const char *want     = tab2 + 1;

    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);
    th_install_nfc(th);
    char     *err = nullptr;
    th_type  *t   = ir_typelang_parse(th, type, &err);

    if (!t) {
        printf("FAIL  line %d: %s -- %s\n", lineno, type,
               err ? err : "no type");
        free(err);
        fails++;
        ir_ctx_free(ctx);

        return;
    }

    const uint8_t *enc = nullptr;
    size_t         n   = 0;

    if (!th_encode(th, t, &enc, &n)) {
        printf("FAIL  line %d: %s does not encode (%s)\n", lineno, type,
               th_last_error(th) ? th_last_error(th) : "-");
        fails++;
        ir_ctx_free(ctx);

        return;
    }

    char hex[8192];

    for (size_t i = 0; i < n && 2 * i + 3 <= sizeof(hex); i++) {
        snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", enc[i]);
    }
    hex[2 * n] = 0;

    if (*want_enc && strcmp(hex, want_enc)) {
        printf("FAIL  line %d: %s encodes to\n  %s\nspec says\n  %s\n", lineno,
               type, hex, want_enc);
        fails++;
    }

    char got[32];

    snprintf(got, sizeof(got), "%llu", (unsigned long long)th_hash(th, t));

    if (strcmp(got, want)) {
        printf("FAIL  line %d: %s hashes %s, spec says %s (%s)\n", lineno, type,
               got, want, th_last_error(th) ? th_last_error(th) : "-");
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
        {"i32, rest", 3},
        {"ptr[i32] = 4", 8},
        {"(x: int) -> void, next", 16},
        {"i32", 3},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t   used = 0;
        char    *err  = nullptr;
        th_type *t    = ir_typelang_parse_prefix(
            th, cases[i].src, strlen(cases[i].src), &used, &err);

        if (!t) {
            printf("FAIL  prefix parse of %s -- %s\n", cases[i].src,
                   err ? err : "no type");
            free(err);
            fails++;
            continue;
        }

        if (used != cases[i].want) {
            printf("FAIL  %s consumed %zu bytes, expected %zu\n", cases[i].src,
                   used, cases[i].want);
            fails++;
        }
    }

    ir_ctx_free(ctx);
}

static void test_n00b_function_failure_channel(void)
{
    ir_ctx_t *ctx     = ir_ctx_new();
    th_ctx   *th      = ir_ctx_types(ctx);
    th_type  *failure = nullptr;
    char     *err     = nullptr;
    th_type  *fn = ir_typelang_parse_function(th, "(`a, `a) -> `a fail: `e",
                                              &failure, &err);
    bool shared  = fn && th_func_error(fn) == failure &&
                  th_func_nparams(fn) == 2 &&
                  th_func_param(fn, 0)->type == th_func_param(fn, 1)->type &&
                  th_func_param(fn, 0)->type == th_func_ret(fn);
    if (!fn || !failure || !shared) {
        printf("FAIL  typelang fail channel: %s\n", err ? err : "missing type");
        fails++;
    }
    const char *printed = fn ? th_print(th, fn) : nullptr;
    th_type *roundtrip = printed ? ir_typelang_parse(th, printed, &err)
                                 : nullptr;
    if (!printed || !strstr(printed, "fail:") || !roundtrip ||
        th_unify(th, fn, roundtrip) == nullptr) {
        printf("FAIL  printed function types retain their failure channel: %s\n",
               err ? err : (printed ? printed : "no printed type"));
        fails++;
    }
    free(err);

    failure = nullptr;
    fn      = ir_typelang_parse_function(
        th, "(`a) -> `a fail: `e where `a: Copy + != string", &failure, &err);
    th_constraint constraint = {0};
    bool          retained =
        fn && th_func_nparams(fn) == 1 &&
        th_var_constraint_count(th_func_param(fn, 0)->type) == 2 &&
        th_var_constraint_at(th_func_param(fn, 0)->type, 0, &constraint) &&
        constraint.kind == TH_CON_IMPLEMENTS &&
        strcmp(constraint.iface, "Copy") == 0;
    if (!retained) {
        printf("FAIL  typelang function where constraints: %s\n",
               err ? err : "constraint was not retained");
        fails++;
    }
    free(err);
    th_type *scheme       = fn ? th_generalize(th, fn) : nullptr;
    th_type *instantiated = scheme ? th_instantiate(th, scheme) : nullptr;
    if (instantiated && th_func_nparams(instantiated) == 1 &&
        th_var_constraint_count(th_func_param(instantiated, 0)->type) == 2)
        retained = true;
    else
        retained = false;
    if (!scheme || !th_func_error(instantiated) ||
        !strstr(th_print(th, scheme), "where `a: Copy + != string") ||
        !strstr(th_print(th, scheme), "fail:"))
        retained = false;
    if (!retained) {
        printf("FAIL  generalization dropped a function where constraint\n");
        fails++;
    }
    ir_ctx_free(ctx);
}

static void test_consume_parameter_mode(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx *th = ir_ctx_types(ctx);
    char *err = nullptr;
    th_type *fn = ir_typelang_parse(th, "(consume int) -> void", &err);
    const th_param *parameter = fn ? th_func_param(fn, 0) : nullptr;
    const char *printed = fn ? th_print(th, fn) : nullptr;
    th_type *roundtrip = printed ? ir_typelang_parse(th, printed, &err)
                                 : nullptr;

    if (!parameter || parameter->mode != TH_CONSUME || !printed
        || strcmp(printed, "(consume int) -> void") != 0 || !roundtrip
        || th_unify(th, fn, roundtrip) == nullptr) {
        printf("FAIL  consume parameter mode round-trip: %s\n",
               err ? err : (printed ? printed : "missing type"));
        fails++;
    }
    free(err);
    ir_ctx_free(ctx);
}

static void test_plain_function_type(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);
    char     *err = nullptr;
    th_type  *fn  = ir_typelang_parse(th, "() -> int", &err);
    if (!fn || th_kind_of(fn) != TH_FUNCTION || th_func_nparams(fn) != 0) {
        printf("FAIL  plain function type: %s\n", err ? err : "missing type");
        fails++;
    }
    free(err);
    ir_ctx_free(ctx);
}

static void test_large_tuple_is_not_truncated(void)
{
    ir_ctx_t      *ctx = ir_ctx_new();
    core_buffer_t *src = core_buffer_from_cstr("t(");
    for (size_t i = 0; i < 70; i++) {
        if (i)
            core_buffer_putc(src, ',');
        core_buffer_puts(src, "int");
    }
    core_buffer_putc(src, ')');

    char       *err     = nullptr;
    th_type    *type    = ir_typelang_parse(ir_ctx_types(ctx), src->data, &err);
    const char *printed = type ? th_print(ir_ctx_types(ctx), type) : "";
    size_t      count   = 0;
    for (const char *p = printed; (p = strstr(p, "int")); p += 3)
        count++;
    if (!type || count != 70) {
        printf("FAIL  a 70-element tuple retained %zu elements: %s\n", count,
               err ? err : printed);
        fails++;
    }
    free(err);
    core_buffer_free(src);
    ir_ctx_free(ctx);
}

static void test_large_binder_is_not_truncated(void)
{
    ir_ctx_t      *ctx = ir_ctx_new();
    core_buffer_t *src = core_buffer_from_cstr("forall[");
    for (size_t i = 0; i < 70; i++) {
        if (i)
            core_buffer_putc(src, ',');
        core_buffer_printf(src, "`t%zu", i);
    }
    core_buffer_puts(src, "]int");

    char    *err   = nullptr;
    th_type *type  = ir_typelang_parse(ir_ctx_types(ctx), src->data, &err);
    uint64_t arity = 0;
    if (!type || !th_binder_arity(type, &arity) || arity != 70) {
        printf("FAIL  a 70-variable binder retained %llu variables: %s\n",
               (unsigned long long)arity, err ? err : "wrong binder arity");
        fails++;
    }
    free(err);
    core_buffer_free(src);
    ir_ctx_free(ctx);
}

static void test_bitslice_prefix_types(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);
    static const struct {
        const char *text;
        th_bitint_flavor flavor;
    } valid[] = {{"i:32", TH_BITINT_SIGNED},
                 {"u:32", TH_BITINT_UNSIGNED},
                 {"bits:32", TH_BITINT_BITS}};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char    *err       = nullptr;
        th_type *type      = ir_typelang_parse(th, valid[i].text, &err);
        uint64_t width     = 0;
        th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
        if (!type || !th_bitint_info(type, &width, &flavor) || width != 32 ||
            flavor != valid[i].flavor) {
            printf("FAIL  bitslice type %s: %s\n", valid[i].text,
                   err ? err : "wrong bit integer type");
            fails++;
        }
        free(err);
    }
    static const char *const invalid[] = {"i", "u"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *err = nullptr;
        if (ir_typelang_parse(th, invalid[i], &err)) {
            printf("FAIL  bare %s unexpectedly parsed as a type\n", invalid[i]);
            fails++;
        }
        free(err);
    }
    ir_ctx_free(ctx);
}

static void test_keyword_name_segments(void)
{
    ir_ctx_t   *ctx  = ir_ctx_new();
    th_ctx     *th   = ir_ctx_types(ctx);
    const char *name = "builtin.containers.set.Set[builtin.containers.dict.Dict[int, i64]]";
    char       *err  = nullptr;
    th_type    *t    = ir_typelang_parse(th, name, &err);

    if (!t || strcmp(th_print(th, t), name)) {
        printf("FAIL  %s does not read back (%s)\n", name,
               t ? th_print(th, t) : err ? err : "no type");
        fails++;
    }

    free(err);
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
    int  n      = 0;
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
    test_plain_function_type();
    test_large_tuple_is_not_truncated();
    test_large_binder_is_not_truncated();
    test_bitslice_prefix_types();
    test_n00b_function_failure_channel();
    test_consume_parameter_mode();
    test_keyword_name_segments();
    test_nonsense_is_refused();
    ir_typelang_shutdown();

    if (!fails) {
        printf("typelang: %d spec vectors match through the grammar\n", n);
    }

    return fails ? 1 : 0;
}
