#include <stdio.h>
#include <stdlib.h>

#include "harness.h"
#include "ir/ir.h"
#include "ir/typelang.h"
#include "lib/buffer.h"
#include "th/typehash.h"

#define S(x)  CORE_STRING_STATIC(x)
#define PS(s) (int)(s).u8_bytes, (s).data

static core_string_t or_else(core_string_t s, core_string_t fallback)
{
    return s.data ? s : fallback;
}

static size_t occurrences(core_string_t hay, core_string_t needle)
{
    size_t count = 0;

    while (hay.data) {
        int64_t at = core_string_find(hay, needle);

        if (at < 0) {
            break;
        }
        count++;

        size_t next = (size_t)at + needle.u8_bytes;

        hay = core_string_slice(hay, next, hay.u8_bytes - next);
    }

    return count;
}

static void check_line(core_string_t line, int lineno)
{
    int64_t tab1 = core_string_find_byte(line, '\t');

    if (tab1 < 0) {
        return;
    }

    core_string_t rest = core_string_slice(line, (size_t)tab1 + 1,
                                           line.u8_bytes - (size_t)tab1 - 1);
    int64_t       tab2 = core_string_find_byte(rest, '\t');

    if (tab2 < 0) {
        return;
    }

    core_string_t type     = core_string_slice(line, 0, (size_t)tab1);
    core_string_t want_enc = core_string_slice(rest, 0, (size_t)tab2);
    core_string_t want     = core_string_slice(
        rest, (size_t)tab2 + 1, rest.u8_bytes - (size_t)tab2 - 1);

    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);
    th_install_nfc(th);
    core_string_t err = {};
    th_type      *t   = ir_typelang_parse(th, type, &err);

    if (!t) {
        printf("FAIL  line %d: %.*s -- %.*s\n", lineno, PS(type),
               PS(or_else(err, S("no type"))));
        core_free(err.data);
        fails++;
        ir_ctx_free(ctx);

        return;
    }

    const uint8_t *enc = nullptr;
    size_t         n   = 0;

    if (!th_encode(th, t, &enc, &n)) {
        printf("FAIL  line %d: %.*s does not encode (%.*s)\n", lineno,
               PS(type), PS(or_else(th_last_error(th), S("-"))));
        fails++;
        ir_ctx_free(ctx);

        return;
    }

    core_buffer_t hex = {0};

    for (size_t i = 0; i < n; i++) {
        core_buffer_printf(&hex, "%02x", enc[i]);
    }

    core_string_t hex_text = core_buffer_string(&hex);

    if (want_enc.u8_bytes && !core_string_eq(hex_text, want_enc)) {
        printf("FAIL  line %d: %.*s encodes to\n  %.*s\nspec says\n  %.*s\n",
               lineno, PS(type), PS(hex_text), PS(want_enc));
        fails++;
    }
    core_buffer_release(&hex);

    char got[32];
    int  got_len = snprintf(got, sizeof(got), "%llu",
                            (unsigned long long)th_hash(th, t));
    core_string_t got_text = core_string_borrow(got, (size_t)got_len);

    if (!core_string_eq(got_text, want)) {
        printf("FAIL  line %d: %.*s hashes %.*s, spec says %.*s (%.*s)\n",
               lineno, PS(type), PS(got_text), PS(want),
               PS(or_else(th_last_error(th), S("-"))));
        fails++;
    }

    ir_ctx_free(ctx);
}

static void test_a_type_can_be_read_out_of_a_longer_text(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);

    static const struct {
        core_string_t src;
        size_t        want;
    } cases[] = {
        {S("i32, rest"), 3},
        {S("ptr[i32] = 4"), 8},
        {S("(x: int) -> void, next"), 16},
        {S("i32"), 3},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t        used = 0;
        core_string_t err  = {};
        th_type      *t    = ir_typelang_parse_prefix(th, cases[i].src, &used,
                                                      &err);

        if (!t) {
            printf("FAIL  prefix parse of %.*s -- %.*s\n", PS(cases[i].src),
                   PS(or_else(err, S("no type"))));
            core_free(err.data);
            fails++;
            continue;
        }

        if (used != cases[i].want) {
            printf("FAIL  %.*s consumed %zu bytes, expected %zu\n",
                   PS(cases[i].src), used, cases[i].want);
            fails++;
        }
    }

    ir_ctx_free(ctx);
}

static void test_n00b_function_failure_channel(void)
{
    ir_ctx_t     *ctx     = ir_ctx_new();
    th_ctx       *th      = ir_ctx_types(ctx);
    th_type      *failure = nullptr;
    core_string_t err     = {};
    th_type      *fn      = ir_typelang_parse_function(
        th, S("(`a, `a) -> `a fail: `e"), &failure, &err);
    bool shared  = fn && th_func_error(fn) == failure &&
                  th_func_nparams(fn) == 2 &&
                  th_func_param(fn, 0)->type == th_func_param(fn, 1)->type &&
                  th_func_param(fn, 0)->type == th_func_ret(fn);
    if (!fn || !failure || !shared) {
        printf("FAIL  typelang fail channel: %.*s\n",
               PS(or_else(err, S("missing type"))));
        fails++;
    }
    core_string_t printed   = fn ? th_print(th, fn) : (core_string_t){};
    th_type      *roundtrip = printed.data
                                ? ir_typelang_parse(th, printed, &err)
                                : nullptr;
    if (!printed.data || core_string_find(printed, S("fail:")) < 0
        || !roundtrip || th_unify(th, fn, roundtrip) == nullptr) {
        printf("FAIL  printed function types retain their failure channel: "
               "%.*s\n",
               PS(or_else(err, or_else(printed, S("no printed type")))));
        fails++;
    }
    core_free(err.data);
    err = (core_string_t){};

    failure = nullptr;
    fn      = ir_typelang_parse_function(
        th, S("(`a) -> `a fail: `e where `a: Copy + != string"), &failure,
        &err);
    th_constraint constraint = {0};
    bool          retained =
        fn && th_func_nparams(fn) == 1 &&
        th_var_constraint_count(th_func_param(fn, 0)->type) == 2 &&
        th_var_constraint_at(th_func_param(fn, 0)->type, 0, &constraint) &&
        constraint.kind == TH_CON_IMPLEMENTS &&
        core_string_eq(constraint.iface, S("Copy"));
    if (!retained) {
        printf("FAIL  typelang function where constraints: %.*s\n",
               PS(or_else(err, S("constraint was not retained"))));
        fails++;
    }
    core_free(err.data);
    th_type *scheme       = fn ? th_generalize(th, fn) : nullptr;
    th_type *instantiated = scheme ? th_instantiate(th, scheme) : nullptr;
    if (instantiated && th_func_nparams(instantiated) == 1 &&
        th_var_constraint_count(th_func_param(instantiated, 0)->type) == 2)
        retained = true;
    else
        retained = false;
    if (!scheme || !th_func_error(instantiated) ||
        core_string_find(th_print(th, scheme),
                         S("where `a: Copy + != string")) < 0 ||
        core_string_find(th_print(th, scheme), S("fail:")) < 0)
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
    core_string_t err = {};
    th_type *fn = ir_typelang_parse(th, S("(consume int) -> void"), &err);
    const th_param *parameter = fn ? th_func_param(fn, 0) : nullptr;
    core_string_t printed = fn ? th_print(th, fn) : (core_string_t){};
    th_type *roundtrip = printed.data ? ir_typelang_parse(th, printed, &err)
                                      : nullptr;

    if (!parameter || parameter->mode != TH_CONSUME || !printed.data
        || !core_string_eq(printed, S("(consume int) -> void")) || !roundtrip
        || th_unify(th, fn, roundtrip) == nullptr) {
        printf("FAIL  consume parameter mode round-trip: %.*s\n",
               PS(or_else(err, or_else(printed, S("missing type")))));
        fails++;
    }
    core_free(err.data);
    ir_ctx_free(ctx);
}

static void test_plain_function_type(void)
{
    ir_ctx_t     *ctx = ir_ctx_new();
    th_ctx       *th  = ir_ctx_types(ctx);
    core_string_t err = {};
    th_type      *fn  = ir_typelang_parse(th, S("() -> int"), &err);
    if (!fn || th_kind_of(fn) != TH_FUNCTION || th_func_nparams(fn) != 0) {
        printf("FAIL  plain function type: %.*s\n",
               PS(or_else(err, S("missing type"))));
        fails++;
    }
    core_free(err.data);
    ir_ctx_free(ctx);
}

static void test_large_tuple_is_not_truncated(void)
{
    ir_ctx_t      *ctx = ir_ctx_new();
    core_buffer_t *src = core_buffer_empty();
    core_buffer_puts(src, "t(");
    for (size_t i = 0; i < 70; i++) {
        if (i)
            core_buffer_putc(src, ',');
        core_buffer_puts(src, "int");
    }
    core_buffer_putc(src, ')');

    core_string_t err  = {};
    th_type      *type = ir_typelang_parse(ir_ctx_types(ctx),
                                           core_buffer_string(src), &err);
    core_string_t printed = type ? th_print(ir_ctx_types(ctx), type) : S("");
    size_t        count   = occurrences(printed, S("int"));
    if (!type || count != 70) {
        printf("FAIL  a 70-element tuple retained %zu elements: %.*s\n", count,
               PS(or_else(err, printed)));
        fails++;
    }
    core_free(err.data);
    core_buffer_free(src);
    ir_ctx_free(ctx);
}

static void test_large_binder_is_not_truncated(void)
{
    ir_ctx_t      *ctx = ir_ctx_new();
    core_buffer_t *src = core_buffer_empty();
    core_buffer_puts(src, "forall[");
    for (size_t i = 0; i < 70; i++) {
        if (i)
            core_buffer_putc(src, ',');
        core_buffer_printf(src, "`t%zu", i);
    }
    core_buffer_puts(src, "]int");

    core_string_t err   = {};
    th_type      *type  = ir_typelang_parse(ir_ctx_types(ctx),
                                            core_buffer_string(src), &err);
    uint64_t      arity = 0;
    if (!type || !th_binder_arity(type, &arity) || arity != 70) {
        printf("FAIL  a 70-variable binder retained %llu variables: %.*s\n",
               (unsigned long long)arity,
               PS(or_else(err, S("wrong binder arity"))));
        fails++;
    }
    core_free(err.data);
    core_buffer_free(src);
    ir_ctx_free(ctx);
}

static void test_bitslice_prefix_types(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);
    static const struct {
        core_string_t    text;
        th_bitint_flavor flavor;
    } valid[] = {{S("i:32"), TH_BITINT_SIGNED},
                 {S("u:32"), TH_BITINT_UNSIGNED},
                 {S("bits:32"), TH_BITINT_BITS}};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        core_string_t    err    = {};
        th_type         *type   = ir_typelang_parse(th, valid[i].text, &err);
        uint64_t         width  = 0;
        th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
        if (!type || !th_bitint_info(type, &width, &flavor) || width != 32 ||
            flavor != valid[i].flavor) {
            printf("FAIL  bitslice type %.*s: %.*s\n", PS(valid[i].text),
                   PS(or_else(err, S("wrong bit integer type"))));
            fails++;
        }
        core_free(err.data);
    }
    static const core_string_t invalid[] = {S("i"), S("u")};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        core_string_t err = {};
        if (ir_typelang_parse(th, invalid[i], &err)) {
            printf("FAIL  bare %.*s unexpectedly parsed as a type\n",
                   PS(invalid[i]));
            fails++;
        }
        core_free(err.data);
    }
    ir_ctx_free(ctx);
}

static void test_keyword_name_segments(void)
{
    ir_ctx_t     *ctx  = ir_ctx_new();
    th_ctx       *th   = ir_ctx_types(ctx);
    core_string_t name = S("builtin.containers.set.Set[builtin.containers.dict."
                           "Dict[int, i64]]");
    core_string_t err  = {};
    th_type      *t    = ir_typelang_parse(th, name, &err);

    if (!t || !core_string_eq(th_print(th, t), name)) {
        core_string_t shown = t ? th_print(th, t) : or_else(err, S("no type"));

        printf("FAIL  %.*s does not read back (%.*s)\n", PS(name), PS(shown));
        fails++;
    }

    core_free(err.data);
    ir_ctx_free(ctx);
}

static void test_nonsense_is_refused(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    th_ctx   *th  = ir_ctx_types(ctx);

    static const core_string_t bad[] = {S("["), S("ptr["), S("(x:"),
                                        S("forall[]")};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        core_string_t err = {};

        if (ir_typelang_parse(th, bad[i], &err)) {
            printf("FAIL  %.*s should not parse as a type\n", PS(bad[i]));
            fails++;
        }

        core_free(err.data);
    }

    ir_ctx_free(ctx);
}

static bool read_all(core_string_t path, core_buffer_t *out)
{
    char *cpath = core_string_to_cstr(path);
    FILE *f     = fopen(cpath, "rb");

    core_free(cpath);

    if (!f) {
        return false;
    }

    char   chunk[8192];
    size_t got;

    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        core_buffer_append(out, chunk, got);
    }

    fclose(f);

    return true;
}

int main(int argc, char **argv)
{
    core_string_t path = argc > 1 ? core_string_from_cstr(argv[1])
                                  : core_string_dup(S("vectors.tsv"));
    core_buffer_t text = {0};

    if (!read_all(path, &text)) {
        printf("FAIL  cannot open %.*s\n", PS(path));
        core_free(path.data);

        return 1;
    }
    core_free(path.data);

    core_string_t rest   = core_buffer_string(&text);
    int           n      = 0;
    int           lineno = 0;

    while (rest.u8_bytes) {
        int64_t       nl   = core_string_find_byte(rest, '\n');
        size_t        len  = nl < 0 ? rest.u8_bytes : (size_t)nl + 1;
        core_string_t line = core_string_slice(rest, 0, len);

        rest = core_string_slice(rest, len, rest.u8_bytes - len);
        lineno++;

        while (line.u8_bytes && (line.data[line.u8_bytes - 1] == '\n'
                                 || line.data[line.u8_bytes - 1] == '\r')) {
            line.u8_bytes--;
        }

        if (!line.u8_bytes) {
            continue;
        }

        check_line(line, lineno);
        n++;
    }

    core_buffer_release(&text);

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
