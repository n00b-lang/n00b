#include <stdio.h>
#include <string.h>

#include "typehash.h"
#include "ir_test.h"

static bool binds(th_ctx *ctx, th_constraint c, th_type *to)
{
    th_type *v = th_var(ctx);

    th_var_constrain(ctx, v, c);

    return th_unify(ctx, v, to) != nullptr;
}

static void test_unifies(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_constraint c = {.kind = TH_CON_UNIFIES, .target = i};

    check("unifies accepts the target", binds(ctx, c, i));
    check("unifies rejects a mismatch", !binds(ctx, c, s));
}

static void test_one_of(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *b = th_name(ctx, "bool");
    th_type *s = th_name(ctx, "string");

    th_type *alts[2] = {i, b};

    th_constraint c = {.kind      = TH_CON_ONE_OF,
                       .one_of    = {.types = alts, .n = 2}};

    check("one_of accepts a listed type", binds(ctx, c, b));
    check("one_of rejects an unlisted type", !binds(ctx, c, s));
}

static void test_excludes(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_constraint c = {.kind = TH_CON_EXCLUDES, .target = i};

    check("excludes accepts another type", binds(ctx, c, s));
    check("excludes rejects the excluded type", !binds(ctx, c, i));
}

static void test_promotes(th_ctx *ctx)
{
    th_type *i32 = th_name(ctx, "i32");
    th_type *i64 = th_name(ctx, "i64");
    th_type *f64 = th_name(ctx, "f64");
    th_type *s   = th_name(ctx, "string");

    th_register_promotion(ctx, "i32", "i64");
    th_register_promotion(ctx, "i64", "f64");

    check("a registered promotion holds", th_promotes_to(ctx, "i32", "i64"));
    check("promotion is transitive", th_promotes_to(ctx, "i32", "f64"));
    check("promotion is reflexive", th_promotes_to(ctx, "i32", "i32"));
    check("promotion is directed", !th_promotes_to(ctx, "i64", "i32"));
    check("an unregistered name does not promote",
          !th_promotes_to(ctx, "string", "i64"));

    th_constraint c = {.kind = TH_CON_PROMOTES, .target = f64};

    check("promotes accepts a type that reaches the target",
          binds(ctx, c, i32));
    check("promotes accepts the target itself", binds(ctx, c, f64));
    check("promotes rejects a type with no path", !binds(ctx, c, s));

    (void)i64;
}

static void test_has_field(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_member mx  = {.name = "x", .type = i};
    th_member my  = {.name = "y", .type = i};
    th_member mxs = {.name = "x", .type = s};

    th_type *has_x    = th_record(ctx, &mx, 1);
    th_type *has_y    = th_record(ctx, &my, 1);
    th_type *x_is_str = th_record(ctx, &mxs, 1);

    th_constraint c = {.kind  = TH_CON_HAS_FIELD,
                       .field = {.name = "x", .type = i}};

    check("has_field accepts a record with the field", binds(ctx, c, has_x));
    check("has_field rejects a record without it", !binds(ctx, c, has_y));
    check("has_field rejects a field of the wrong type",
          !binds(ctx, c, x_is_str));
    check("has_field rejects a non-record", !binds(ctx, c, i));
}

static void test_has_param(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_arg ai = {.kind = TH_ARG_TYPE, .type = i};
    th_arg as = {.kind = TH_ARG_TYPE, .type = s};

    th_type *list_int = th_apply(ctx, "list", &ai, 1);
    th_type *list_str = th_apply(ctx, "list", &as, 1);

    th_constraint c = {.kind  = TH_CON_HAS_PARAM,
                       .param = {.index = 0, .type = i}};

    check("has_param accepts a matching parameter", binds(ctx, c, list_int));
    check("has_param rejects a mismatched parameter",
          !binds(ctx, c, list_str));
    check("has_param rejects a type with no parameters", !binds(ctx, c, i));
}

static void test_constraints_move_between_vars(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");

    th_type      *a = th_var(ctx);
    th_constraint c = {.kind = TH_CON_EXCLUDES, .target = i};

    th_var_constrain(ctx, a, c);

    th_type *b = th_var(ctx);

    th_param p1[2] = {{.mode = TH_IN, .type = a}, {.mode = TH_IN, .type = a}};
    th_param p2[2] = {{.mode = TH_IN, .type = b}, {.mode = TH_IN, .type = i}};

    th_type *f1 = th_func(ctx, i, p1, 2, nullptr, nullptr, 0);
    th_type *f2 = th_func(ctx, i, p2, 2, nullptr, nullptr, 0);

    check("a constraint follows a var through a var-to-var binding",
          th_unify(ctx, f1, f2) == nullptr);

    th_param q1[1] = {{.mode = TH_IN, .type = a}};
    th_param q2[1] = {{.mode = TH_IN, .type = b}};

    th_type *g1 = th_func(ctx, i, q1, 1, nullptr, nullptr, 0);
    th_type *g2 = th_func(ctx, i, q2, 1, nullptr, nullptr, 0);

    check("two vars still unify with each other",
          th_unify(ctx, g1, g2) != nullptr);
}

static void test_implements(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");

    th_type *reader = th_func(ctx, i, nullptr, 0, nullptr, nullptr, 0);

    th_member rm = {.name = "read", .type = reader};
    th_member wm = {.name = "write", .type = reader};

    th_type *readable = th_record(ctx, &rm, 1);

    th_register_iface(ctx, "Readable", readable);

    th_member both[2] = {{.name = "read", .type = reader},
                         {.name = "write", .type = reader}};

    th_type *rw       = th_record(ctx, both, 2);
    th_type *only_w   = th_record(ctx, &wm, 1);
    th_type *wrong_ty = th_record(ctx,
                                  &(th_member){.name = "read", .type = i}, 1);

    check("a type with the method satisfies the interface",
          th_implements(ctx, rw, "Readable"));
    check("satisfaction is superset, not equality",
          th_implements(ctx, readable, "Readable"));
    check("a missing method fails", !th_implements(ctx, only_w, "Readable"));
    check("a wrong method type fails",
          !th_implements(ctx, wrong_ty, "Readable"));
    check("an unregistered interface fails",
          !th_implements(ctx, rw, "Writable"));

    th_constraint impl = {.kind = TH_CON_IMPLEMENTS, .iface = "Readable"};

    check("implements accepts a satisfying type", binds(ctx, impl, rw));
    check("implements rejects a non-satisfying type", !binds(ctx, impl, i));

    const char *got[4];
    size_t      n = th_infer_ifaces(ctx, rw, got, 4);

    check("the satisfied set is inferred", n == 1 && !strcmp(got[0],
                                                             "Readable"));
    check("nothing is inferred for a type that satisfies none",
          th_infer_ifaces(ctx, i, got, 4) == 0);
}

static void test_constraints_are_not_identity(void)
{
    th_ctx *a = th_ctx_new();
    th_ctx *b = th_ctx_new();

    th_type *va = th_var(a);
    th_type *vb = th_var(b);

    th_constraint c = {.kind = TH_CON_EXCLUDES, .target = th_name(a, "int")};

    th_var_constrain(a, va, c);

    th_type *fa = th_func(a, th_name(a, "int"), nullptr, 0, nullptr,
                          nullptr, 0);
    th_type *fb = th_func(b, th_name(b, "int"), nullptr, 0, nullptr,
                          nullptr, 0);

    check("a constraint does not change the hash",
          th_hash(a, fa) == th_hash(b, fb));

    check("the constraint is readable back",
          th_var_constraint_count(va) == 1);
    check("an unconstrained var has none",
          th_var_constraint_count(vb) == 0);

    th_ctx_free(a);
    th_ctx_free(b);
}

static void test_open_records_accumulate(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_constraint fx = {.kind  = TH_CON_HAS_FIELD,
                        .field = {.name = "x", .type = i}};
    th_constraint fy = {.kind  = TH_CON_HAS_FIELD,
                        .field = {.name = "y", .type = s}};
    th_constraint fx2 = {.kind  = TH_CON_HAS_FIELD,
                         .field = {.name = "x", .type = s}};

    th_type *v = th_var(ctx);

    check("a field constraint is recorded", th_var_constrain(ctx, v, fx));
    check("a second field is recorded too", th_var_constrain(ctx, v, fy));
    check("two fields, two constraints", th_var_constraint_count(v) == 2);

    check("the same field at the same type merges",
          th_var_constrain(ctx, v, fx));
    check("merging does not grow the set", th_var_constraint_count(v) == 2);

    check("the same field at a different type is refused",
          !th_var_constrain(ctx, v, fx2));

    th_member both[2] = {{.name = "x", .type = i}, {.name = "y", .type = s}};
    th_member only_x  = {.name = "x", .type = i};

    th_type *rec_both = th_record(ctx, both, 2);
    th_type *rec_one  = th_record(ctx, &only_x, 1);

    check("the accumulated set is satisfied by a record with both fields",
          th_unify(ctx, v, rec_both) != nullptr);
    check("and refused by a record missing one",
          th_unify(ctx, v, rec_one) == nullptr);
}

static void test_open_records_merge_across_vars(th_ctx *ctx)
{
    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_type *a = th_var(ctx);
    th_type *b = th_var(ctx);

    th_var_constrain(ctx, a,
                     (th_constraint){.kind  = TH_CON_HAS_FIELD,
                                     .field = {.name = "x", .type = i}});
    th_var_constrain(ctx, b,
                     (th_constraint){.kind  = TH_CON_HAS_FIELD,
                                     .field = {.name = "y", .type = s}});

    th_member both[2] = {{.name = "x", .type = i}, {.name = "y", .type = s}};
    th_member only_y  = {.name = "y", .type = s};

    th_type *rec_both = th_record(ctx, both, 2);
    th_type *rec_one  = th_record(ctx, &only_y, 1);

    th_param p1[2] = {{.mode = TH_IN, .type = a}, {.mode = TH_IN, .type = a}};
    th_param p2[2] = {{.mode = TH_IN, .type = b},
                      {.mode = TH_IN, .type = rec_both}};

    th_type *f1 = th_func(ctx, i, p1, 2, nullptr, nullptr, 0);
    th_type *f2 = th_func(ctx, i, p2, 2, nullptr, nullptr, 0);

    check("two open rows merge and the union is satisfiable",
          th_unify(ctx, f1, f2) != nullptr);

    th_param q2[2] = {{.mode = TH_IN, .type = b},
                      {.mode = TH_IN, .type = rec_one}};

    th_type *g2 = th_func(ctx, i, q2, 2, nullptr, nullptr, 0);

    check("the merged row rejects a record satisfying only one side",
          th_unify(ctx, f1, g2) == nullptr);
}

static void test_coerce(th_ctx *ctx)
{
    th_type *i32 = th_name(ctx, "i32");
    th_type *i64 = th_name(ctx, "i64");
    th_type *s   = th_name(ctx, "string");
    th_type *ref = th_ind(ctx, TH_NULLABLE, i32);

    th_register_promotion(ctx, "i32", "i64");

    th_coerce_policy exact = {.allow = TH_COERCE_NONE, .target = i64};
    th_coerce_policy promo = {.allow = TH_COERCE_PROMOTE, .target = i64};
    th_coerce_policy deref = {.allow = TH_COERCE_DEREF | TH_COERCE_PROMOTE,
                              .target = i64};

    check("an exact match needs no permission",
          !th_is_bottom(th_coerce(ctx, i64, exact)));
    check("a promotion is refused when not allowed",
          th_is_bottom(th_coerce(ctx, i32, exact)));
    check("a promotion is taken when allowed",
          !th_is_bottom(th_coerce(ctx, i32, promo)));
    check("a deref is refused when not allowed",
          th_is_bottom(th_coerce(ctx, ref, promo)));
    check("a deref then promotion is taken when both are allowed",
          !th_is_bottom(th_coerce(ctx, ref, deref)));
    check("an impossible coercion is bottom",
          th_is_bottom(th_coerce(ctx, s, deref)));
}

static th_span_t at(const char *file, uint32_t line, uint32_t col)
{
    return (th_span_t){.file       = file,
                       .start_line = line,
                       .start_col  = col,
                       .end_line   = line,
                       .end_col    = col + 1};
}

static void test_errors_are_structured(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    check("a fresh context has no errors", th_error_count(ctx) == 0);
    check("and no last error", th_last_error(ctx) == nullptr);

    th_ctx_set_span(ctx, at("a.n00b", 12, 4));

    check("the span is readable back", th_ctx_span(ctx).start_line == 12);

    check("unifying two different types fails",
          th_unify(ctx, i, s) == nullptr);

    th_error_t e;

    check("the failure is recorded", th_error_count(ctx) > 0);
    check("and readable", th_error_at(ctx, 0, &e));
    check("it carries the span it was raised at",
          e.span.start_line == 12 && e.span.start_col == 4
              && !strcmp(e.span.file, "a.n00b"));
    check("it names a kind", e.kind == TH_ERR_UNIFY);
    check("the kind has a name", !strcmp(th_err_kind_name(e.kind), "unify"));
    check("it carries what was expected and what was got",
          e.expected != nullptr && e.got != nullptr);
    check("and a message", e.message != nullptr);
    check("th_last_error still answers", th_last_error(ctx) != nullptr);

    th_errors_clear(ctx);

    check("errors can be cleared", th_error_count(ctx) == 0);
    check("and the last error with them", th_last_error(ctx) == nullptr);

    th_ctx_free(ctx);
}

static void test_errors_accumulate_with_their_own_spans(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type *i = th_name(ctx, "int");
    th_type *s = th_name(ctx, "string");

    th_ctx_set_span(ctx, at("a.n00b", 1, 1));
    th_unify(ctx, i, s);

    th_ctx_set_span(ctx, at("b.n00b", 9, 3));
    th_ctx_set_related(ctx, at("a.n00b", 1, 1));
    th_unify(ctx, s, i);

    check("both failures are kept", th_error_count(ctx) == 2);

    th_error_t first, second;

    th_error_at(ctx, 0, &first);
    th_error_at(ctx, 1, &second);

    check("each keeps the span current when it was raised",
          first.span.start_line == 1 && second.span.start_line == 9);
    check("the first has no related span", !first.has_related);
    check("the second points at the earlier one",
          second.has_related && second.related.start_line == 1
              && !strcmp(second.related.file, "a.n00b"));

    th_ctx_clear_related(ctx);
    th_unify(ctx, i, s);

    th_error_t third;

    th_error_at(ctx, 2, &third);
    check("a cleared related span stays cleared", !third.has_related);

    th_ctx_free(ctx);
}

static void test_failures_name_what_went_wrong(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type *i = th_name(ctx, "int");

    th_member ma = {.name = "x", .type = i};
    th_member mb = {.name = "y", .type = i};

    th_type *ra = th_record(ctx, &ma, 1);
    th_type *rb = th_record(ctx, &mb, 1);

    th_unify(ctx, ra, rb);

    th_error_t e;

    th_error_at(ctx, 0, &e);
    check("a missing member is a no-such-field, not a bare mismatch",
          e.kind == TH_ERR_NO_SUCH_FIELD);
    check("and it names the member", e.constraint
                                         && !strcmp(e.constraint, "x"));

    th_errors_clear(ctx);

    th_param p1[1] = {{.mode = TH_IN, .type = i}};
    th_type *f1    = th_func(ctx, i, p1, 1, nullptr, nullptr, 0);
    th_type *f0    = th_func(ctx, i, nullptr, 0, nullptr, nullptr, 0);

    th_unify(ctx, f1, f0);
    th_error_at(ctx, 0, &e);
    check("differing arity is an arity mismatch",
          e.kind == TH_ERR_ARITY_MISMATCH);

    th_errors_clear(ctx);

    th_type *v  = th_var(ctx);
    th_arg   la = {.kind = TH_ARG_TYPE, .type = v};
    th_type *lv = th_apply(ctx, "list", &la, 1);
    th_type *w  = th_var(ctx);

    th_param vp[2] = {{.mode = TH_IN, .type = v}, {.mode = TH_IN, .type = lv}};
    th_param wp[2] = {{.mode = TH_IN, .type = w}, {.mode = TH_IN, .type = w}};

    th_unify(ctx,
             th_func(ctx, i, vp, 2, nullptr, nullptr, 0),
             th_func(ctx, i, wp, 2, nullptr, nullptr, 0));

    th_error_at(ctx, 0, &e);
    check("a variable inside its own binding is an occurs failure",
          e.kind == TH_ERR_OCCURS);

    th_ctx_free(ctx);
}

static void test_a_constrained_var_prints_its_bound(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type *i = th_name(ctx, "int");
    th_type *v = th_var_named(ctx, "t");

    check("an unconstrained var has no where clause",
          !strstr(th_print(ctx, th_var_named(ctx, "u")), "where"));

    th_var_constrain(ctx, v,
                     (th_constraint){.kind = TH_CON_EXCLUDES, .target = i});
    th_var_constrain(ctx, v,
                     (th_constraint){.kind  = TH_CON_IMPLEMENTS,
                                     .iface = "Numeric"});

    const char *got = th_print(ctx, v);

    check("the where clause names the variable", strstr(got, "where `t:")
                                                     != nullptr);
    check("it renders each bound", strstr(got, "!= int") != nullptr
                                       && strstr(got, "Numeric") != nullptr);
    check("and joins them", strstr(got, " + ") != nullptr);

    th_ctx_free(ctx);
}

int main(void)
{
    th_ctx *ctx = th_ctx_new();

    test_unifies(ctx);
    test_one_of(ctx);
    test_excludes(ctx);
    test_promotes(ctx);
    test_has_field(ctx);
    test_has_param(ctx);
    test_constraints_move_between_vars(ctx);
    test_implements(ctx);
    test_open_records_accumulate(ctx);
    test_open_records_merge_across_vars(ctx);
    test_coerce(ctx);

    th_ctx_free(ctx);

    test_constraints_are_not_identity();
    test_errors_are_structured();
    test_errors_accumulate_with_their_own_spans();
    test_failures_name_what_went_wrong();
    test_a_constrained_var_prints_its_bound();

    return report("th constraints");
}
