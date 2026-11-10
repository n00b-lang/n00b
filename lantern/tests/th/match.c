#include <stdio.h>

#include "th/typehash.h"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "th match: %s\n", what);
        failures++;
    }
}

static th_type *list_of(th_ctx *ctx, th_type *item)
{
    th_arg arg = {.kind = TH_ARG_TYPE, .type = item};

    return th_apply(ctx, CORE_STRING_STATIC("List"), &arg, 1);
}

static void pattern_variables_bind(th_ctx *ctx)
{
    th_type *item = th_var(ctx);
    th_type *pattern = list_of(ctx, item);
    th_type *u8 = th_name(ctx, CORE_STRING_STATIC("u8"));

    check(th_match(pattern, list_of(ctx, u8)), "List['a] matches List[u8]");
    check(th_resolve(item) == u8, "the pattern variable binds to u8");
}

static void concrete_side_never_binds(th_ctx *ctx)
{
    th_type *u8 = th_name(ctx, CORE_STRING_STATIC("u8"));
    th_type *open = th_var(ctx);

    check(!th_match(list_of(ctx, u8), list_of(ctx, open)),
          "a concrete pattern does not match an open value");
    check(th_kind_of(th_resolve(open)) == TH_VAR, "the value stays open");
}

static void failed_matches_roll_back(th_ctx *ctx)
{
    th_type *a = th_var(ctx);
    th_type *u8 = th_name(ctx, CORE_STRING_STATIC("u8"));
    th_type *str = th_name(ctx, CORE_STRING_STATIC("string"));
    th_param params[] = {{.mode = TH_IN, .type = a}, {.mode = TH_IN, .type = a}};
    th_param values[] = {{.mode = TH_IN, .type = u8},
                         {.mode = TH_IN, .type = str}};
    th_type *pattern = th_func(ctx, a, params, 2, nullptr, nullptr, 0);
    th_type *value = th_func(ctx, u8, values, 2, nullptr, nullptr, 0);

    check(!th_match(pattern, value), "one variable cannot be two types");
    check(th_kind_of(th_resolve(a)) == TH_VAR, "a failed match leaves no binding");
}

static void probes_leave_no_bindings(th_ctx *ctx)
{
    th_type *a = th_var(ctx);
    th_type *b = th_var(ctx);
    th_type *u8 = th_name(ctx, CORE_STRING_STATIC("u8"));

    check(th_matches(list_of(ctx, a), list_of(ctx, u8)), "matches probes");
    check(th_kind_of(th_resolve(a)) == TH_VAR, "matches does not bind");
    check(th_compatible(list_of(ctx, u8), list_of(ctx, b)),
          "compatible binds either side while probing");
    check(th_kind_of(th_resolve(b)) == TH_VAR, "compatible does not bind");
}

static void fill_binds_or_compares(th_ctx *ctx)
{
    th_type *slot = th_var(ctx);
    th_type *u8 = th_name(ctx, CORE_STRING_STATIC("u8"));
    th_type *str = th_name(ctx, CORE_STRING_STATIC("string"));

    check(th_fill(slot, u8) == u8, "fill binds an open placeholder");
    check(th_fill(slot, u8) != nullptr, "fill accepts the same type");
    check(th_fill(slot, str) == nullptr, "fill rejects a different type");
}

static void meet_works_on_copies(th_ctx *ctx)
{
    th_type *a = th_var(ctx);
    th_type *u8 = th_name(ctx, CORE_STRING_STATIC("u8"));
    th_type *met = th_meet(ctx, list_of(ctx, a), list_of(ctx, u8));

    check(met && th_hash(ctx, met) == th_hash(ctx, list_of(ctx, u8)),
          "meet produces the matched type");
    check(th_kind_of(th_resolve(a)) == TH_VAR, "meet leaves its inputs open");
}

int main(void)
{
    th_ctx *ctx = th_ctx_new();

    pattern_variables_bind(ctx);
    concrete_side_never_binds(ctx);
    failed_matches_roll_back(ctx);
    probes_leave_no_bindings(ctx);
    fill_binds_or_compares(ctx);
    meet_works_on_copies(ctx);
    th_ctx_free(ctx);

    return failures ? 1 : 0;
}
