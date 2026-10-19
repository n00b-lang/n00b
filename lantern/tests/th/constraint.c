#include <stdio.h>

#include "th/environment.h"
#include "th/typehash.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static bool contains(core_string_t haystack, core_string_t needle)
{
    return core_string_find(haystack, needle) >= 0;
}

static bool binds(th_ctx *ctx, th_constraint c, th_type *to)
{
    th_type *v = th_var(ctx);

    th_var_constrain(ctx, v, c);

    return th_unify(ctx, v, to) != nullptr;
}

static void test_unifies(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_constraint c = {.kind = TH_CON_UNIFIES, .target = i};

    check("unifies accepts the target", binds(ctx, c, i));
    check("unifies rejects a mismatch", !binds(ctx, c, s));
}

static void test_one_of(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_type *b = th_name(ctx, S("bool"));
    th_type *s = th_name(ctx, S("string"));

    th_type *alts[2] = {i, b};

    th_constraint c = {.kind      = TH_CON_ONE_OF,
                       .one_of    = {.types = alts, .n = 2}};

    check("one_of accepts a listed type", binds(ctx, c, b));
    check("one_of rejects an unlisted type", !binds(ctx, c, s));

    th_type *integers = th_var(ctx);
    check("one_of alternative can describe a structural family",
          th_var_constrain(ctx, integers,
              (th_constraint){.kind = TH_CON_KIND, .type_kind = TH_BITINT}));
    th_type *families[] = {integers, b};
    th_constraint family = {.kind = TH_CON_ONE_OF,
        .one_of = {.types = families, .n = 2}};
    size_t errors = th_error_count(ctx);
    check("one_of matches arbitrary members of a structural family",
          binds(ctx, family, th_bitint(ctx, 7, true))
              && binds(ctx, family, th_bitint(ctx, 513, false)));
    check("one_of failed alternatives do not produce diagnostics",
          binds(ctx, family, b) && th_error_count(ctx) == errors);
    check("one_of probes do not refine the alternative",
          th_kind_of(th_resolve(integers)) == TH_VAR);
    check("one_of family rejects unrelated types", !binds(ctx, family, s));
    th_type *variable = th_var(ctx);
    check("one_of family survives generalization and copying",
          th_var_constrain(ctx, variable, family)
              && th_unify(ctx, th_instantiate(ctx, th_generalize(ctx, variable)),
                          th_bitint(ctx, 257, false)));
}

static void test_excludes(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_constraint c = {.kind = TH_CON_EXCLUDES, .target = i};

    check("excludes accepts another type", binds(ctx, c, s));
    check("excludes rejects the excluded type", !binds(ctx, c, i));
}

static void test_repeated_constraints(th_ctx *ctx)
{
    th_type *variable = th_var(ctx);
    th_constraint excluded = {.kind = TH_CON_EXCLUDES,
                              .target = th_name(ctx, S("string"))};
    check("first exclusion is attached",
          th_var_constrain(ctx, variable, excluded)
              && th_var_constraint_count(variable) == 1);
    for (size_t i = 0; i < 4; i++) {
        excluded.target = th_name(ctx, S("string"));
        check("equal exclusions from distinct type nodes stay one constraint",
              th_var_constrain(ctx, variable, excluded)
                  && th_var_constraint_count(variable) == 1);
    }
    excluded.target = th_name(ctx, S("bool"));
    check("distinct exclusion remains distinct",
          th_var_constrain(ctx, variable, excluded)
              && th_var_constraint_count(variable) == 2);
    th_constraint kind = {.kind = TH_CON_KIND,
                          .type_kind = TH_NAMED};
    check("same structural-family constraint is idempotent",
          th_var_constrain(ctx, variable, kind)
              && th_var_constrain(ctx, variable, kind)
              && th_var_constraint_count(variable) == 3);
    th_type *choices[] = {th_name(ctx, S("int")), th_name(ctx, S("bool"))};
    th_constraint one_of = {.kind = TH_CON_ONE_OF,
                            .one_of = {.types = choices, .n = 2}};
    th_type *equal_choices[] = {th_name(ctx, S("int")),
                                th_name(ctx, S("bool"))};
    th_constraint equal_one_of = {.kind = TH_CON_ONE_OF,
                                  .one_of = {.types = equal_choices, .n = 2}};
    check("same alternative list is idempotent",
          th_var_constrain(ctx, variable, one_of)
              && th_var_constrain(ctx, variable, equal_one_of)
              && th_var_constraint_count(variable) == 4);
}

static void test_kind(th_ctx *ctx)
{
    th_constraint constraint = {.kind = TH_CON_KIND, .type_kind = TH_BITINT};
    check("kind accepts unrelated sized integers",
          binds(ctx, constraint, th_bitint(ctx, 7, true))
              && binds(ctx, constraint, th_bitint(ctx, 513, false)));
    check("kind rejects a different structural family",
          !binds(ctx, constraint, th_name(ctx, S("string"))));
    th_type *variable = th_var(ctx);
    check("kind constrains a fresh variable",
          th_var_constrain(ctx, variable, constraint));
    th_type *scheme = th_generalize(ctx, variable);
    th_type *instance = th_instantiate(ctx, scheme);
    th_ctx *copy_ctx = th_ctx_new();
    th_type *copy = th_type_clone(copy_ctx, scheme);
    check("kind survives generalization, instantiation, and cloning",
          instance && copy
              && !th_unify(ctx, instance, th_name(ctx, S("string")))
              && !th_unify(copy_ctx, th_instantiate(copy_ctx, copy),
                            th_name(copy_ctx, S("string"))));
    th_ctx_free(copy_ctx);
}

static void test_promotes(th_ctx *ctx)
{
    th_type *i32 = th_name(ctx, S("i32"));
    th_type *i64 = th_name(ctx, S("i64"));
    th_type *f64 = th_name(ctx, S("f64"));
    th_type *s   = th_name(ctx, S("string"));

    th_register_promotion(ctx, S("i32"), S("i64"));
    th_register_promotion(ctx, S("i64"), S("f64"));

    check("a registered promotion holds",
          th_promotes_to(ctx, S("i32"), S("i64")));
    check("promotion is transitive", th_promotes_to(ctx, S("i32"), S("f64")));
    check("promotion is reflexive", th_promotes_to(ctx, S("i32"), S("i32")));
    check("promotion is directed", !th_promotes_to(ctx, S("i64"), S("i32")));
    check("an unregistered name does not promote",
          !th_promotes_to(ctx, S("string"), S("i64")));

    th_constraint c = {.kind = TH_CON_PROMOTES, .target = f64};

    check("promotes accepts a type that reaches the target",
          binds(ctx, c, i32));
    check("promotes accepts the target itself", binds(ctx, c, f64));
    check("promotes rejects a type with no path", !binds(ctx, c, s));

    th_type *i8 = th_bitint(ctx, 8, true);
    th_type *i16 = th_bitint(ctx, 16, true);
    th_type *u8 = th_bitint(ctx, 8, false);
    th_type *u16 = th_bitint(ctx, 16, false);
    th_type *bits8 = th_bits(ctx, 8);
    th_type *i8_signed = th_bitint(ctx, 8, true);
    check("signed bit integers promote only to wider signed widths",
          th_type_promotes_to(ctx, i8, i16) &&
              !th_type_promotes_to(ctx, i16, i8) &&
              !th_type_promotes_to(ctx, i8, u16));
    check("unsigned bit integers promote to wider unsigned or signed widths",
          th_type_promotes_to(ctx, u8, u16) &&
              th_type_promotes_to(ctx, u8, i16) &&
              !th_type_promotes_to(ctx, u8, i8_signed));
    check("bit-integer promotions participate in coercion constraints",
          binds(ctx, (th_constraint){.kind = TH_CON_PROMOTES, .target = i16},
                i8));
    check("bits and unsigned integers have distinct type identities",
          th_hash(ctx, bits8) != th_hash(ctx, u8)
              && th_unify(ctx, bits8, u8) == nullptr
              && !th_type_promotes_to(ctx, bits8, u16)
              && !th_type_promotes_to(ctx, u8, bits8));
    check("bits type prints with its distinct spelling",
          core_string_eq(th_print(ctx, bits8), S("bits:8")));
    th_ctx *copy_ctx = th_ctx_new();
    th_type *bits_copy = th_type_clone(copy_ctx, bits8);
    check("bits flavor survives type-graph cloning",
          bits_copy && th_hash(copy_ctx, bits_copy) == th_hash(ctx, bits8));
    th_ctx_free(copy_ctx);

    (void)i64;
}

static void test_has_field(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_member mx  = {.name = S("x"), .type = i};
    th_member my  = {.name = S("y"), .type = i};
    th_member mxs = {.name = S("x"), .type = s};

    th_type *has_x    = th_record(ctx, &mx, 1);
    th_type *has_y    = th_record(ctx, &my, 1);
    th_type *x_is_str = th_record(ctx, &mxs, 1);

    th_constraint c = {.kind  = TH_CON_HAS_FIELD,
                       .field = {.name = S("x"), .type = i}};

    check("has_field accepts a record with the field", binds(ctx, c, has_x));
    check("has_field rejects a record without it", !binds(ctx, c, has_y));
    check("has_field rejects a field of the wrong type",
          !binds(ctx, c, x_is_str));
    check("has_field rejects a non-record", !binds(ctx, c, i));
}

static void test_has_field_auto_deref(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_member value = {.name = S("value"), .type = i};
    th_type *record = th_record(ctx, &value, 1);
    th_type *ref = th_ind(ctx, 0, record);
    th_constraint field = {.kind = TH_CON_HAS_FIELD,
        .field = {.name = S("value"), .type = i, .auto_deref = true}};
    th_constraint strict = {.kind = TH_CON_HAS_FIELD,
        .field = {.name = S("value"), .type = i}};

    check("auto-deref field accepts a value", binds(ctx, field, record));
    check("auto-deref field accepts a reference", binds(ctx, field, ref));
    check("strict field rejects a reference", !binds(ctx, strict, ref));
    check("auto-deref field rejects a missing field",
          !binds(ctx, field, th_ind(ctx, 0, i)));
}

static void test_has_field_reports_field_constraint(th_ctx *ctx)
{
    th_member value = {.name = S("value"),
                       .type = th_name(ctx, S("string"))};
    th_type *record = th_record(ctx, &value, 1);
    th_type *field = th_var(ctx);
    th_var_constrain(ctx, field,
                     (th_constraint){.kind = TH_CON_KIND,
                                     .type_kind = TH_BITINT});
    th_constraint c = {.kind = TH_CON_HAS_FIELD,
                       .field = {.name = S("value"), .type = field}};
    th_errors_clear(ctx);
    bool bound = binds(ctx, c, record);
    th_error_t last = {0};
    bool recorded = th_error_count(ctx)
        && th_error_at(ctx, th_error_count(ctx) - 1, &last);
    check("has_field reports the field's own failing constraint",
          !bound && recorded && last.constraint.data
              && core_string_eq(last.constraint, S("kind")));
}

static void test_has_param(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_arg ai = {.kind = TH_ARG_TYPE, .type = i};
    th_arg as = {.kind = TH_ARG_TYPE, .type = s};

    th_type *list_int = th_apply(ctx, S("list"), &ai, 1);
    th_type *list_str = th_apply(ctx, S("list"), &as, 1);

    th_constraint c = {.kind  = TH_CON_HAS_PARAM,
                       .param = {.index = 0, .type = i}};

    check("has_param accepts a matching parameter", binds(ctx, c, list_int));
    check("has_param rejects a mismatched parameter",
          !binds(ctx, c, list_str));
    check("has_param rejects a type with no parameters", !binds(ctx, c, i));
}

static void test_constraints_move_between_vars(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));

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
    th_type *i = th_name(ctx, S("int"));

    th_type *reader = th_func(ctx, i, nullptr, 0, nullptr, nullptr, 0);

    th_member rm = {.name = S("read"), .type = reader};
    th_member wm = {.name = S("write"), .type = reader};

    th_type *readable = th_record(ctx, &rm, 1);

    th_register_iface(ctx, S("Readable"), readable);

    th_member both[2] = {{.name = S("read"), .type = reader},
                         {.name = S("write"), .type = reader}};

    th_type *rw       = th_record(ctx, both, 2);
    th_type *only_w   = th_record(ctx, &wm, 1);
    th_type *wrong_ty = th_record(ctx,
                                  &(th_member){.name = S("read"), .type = i},
                                  1);

    check("a type with the method satisfies the interface",
          th_implements(ctx, rw, S("Readable")));
    check("satisfaction is superset, not equality",
          th_implements(ctx, readable, S("Readable")));
    check("a missing method fails",
          !th_implements(ctx, only_w, S("Readable")));
    check("a wrong method type fails",
          !th_implements(ctx, wrong_ty, S("Readable")));
    check("an unregistered interface fails",
          !th_implements(ctx, rw, S("Writable")));

    th_constraint impl = {.kind = TH_CON_IMPLEMENTS, .iface = S("Readable")};

    check("implements accepts a satisfying type", binds(ctx, impl, rw));
    check("implements rejects a non-satisfying type", !binds(ctx, impl, i));

    core_string_t got[4];
    size_t        n = th_infer_ifaces(ctx, rw, got, 4);

    check("the satisfied set is inferred",
          n == 1 && core_string_eq(got[0], S("Readable")));
    check("nothing is inferred for a type that satisfies none",
          th_infer_ifaces(ctx, i, got, 4) == 0);
}

static void test_constraints_are_not_identity(void)
{
    th_ctx *a = th_ctx_new();
    th_ctx *b = th_ctx_new();

    th_type *va = th_var(a);
    th_type *vb = th_var(b);

    th_constraint c = {.kind = TH_CON_EXCLUDES,
                       .target = th_name(a, S("int"))};

    th_var_constrain(a, va, c);

    th_type *fa = th_func(a, th_name(a, S("int")), nullptr, 0, nullptr,
                          nullptr, 0);
    th_type *fb = th_func(b, th_name(b, S("int")), nullptr, 0, nullptr,
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
    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_constraint fx = {.kind  = TH_CON_HAS_FIELD,
                        .field = {.name = S("x"), .type = i}};
    th_constraint fy = {.kind  = TH_CON_HAS_FIELD,
                        .field = {.name = S("y"), .type = s}};
    th_constraint fx2 = {.kind  = TH_CON_HAS_FIELD,
                         .field = {.name = S("x"), .type = s}};

    th_type *v = th_var(ctx);

    check("a field constraint is recorded", th_var_constrain(ctx, v, fx));
    check("a second field is recorded too", th_var_constrain(ctx, v, fy));
    check("two fields, two constraints", th_var_constraint_count(v) == 2);

    check("the same field at the same type merges",
          th_var_constrain(ctx, v, fx));
    check("merging does not grow the set", th_var_constraint_count(v) == 2);

    check("the same field at a different type is refused",
          !th_var_constrain(ctx, v, fx2));

    th_member both[2] = {{.name = S("x"), .type = i},
                         {.name = S("y"), .type = s}};
    th_member only_x  = {.name = S("x"), .type = i};

    th_type *rec_both = th_record(ctx, both, 2);
    th_type *rec_one  = th_record(ctx, &only_x, 1);

    check("the accumulated set is satisfied by a record with both fields",
          th_unify(ctx, v, rec_both) != nullptr);
    check("and refused by a record missing one",
          th_unify(ctx, v, rec_one) == nullptr);
}

static void test_open_records_merge_across_vars(th_ctx *ctx)
{
    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_type *a = th_var(ctx);
    th_type *b = th_var(ctx);

    th_var_constrain(ctx, a,
                     (th_constraint){.kind  = TH_CON_HAS_FIELD,
                                     .field = {.name = S("x"), .type = i}});
    th_var_constrain(ctx, b,
                     (th_constraint){.kind  = TH_CON_HAS_FIELD,
                                     .field = {.name = S("y"), .type = s}});

    th_member both[2] = {{.name = S("x"), .type = i},
                         {.name = S("y"), .type = s}};
    th_member only_y  = {.name = S("y"), .type = s};

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
    th_type *i32 = th_name(ctx, S("i32"));
    th_type *i64 = th_name(ctx, S("i64"));
    th_type *s   = th_name(ctx, S("string"));
    th_type *ref = th_ind(ctx, TH_NULLABLE, i32);

    th_register_promotion(ctx, S("i32"), S("i64"));

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

    th_type *bits8 = th_bitint(ctx, 8, true);
    th_type *bits16 = th_bitint(ctx, 16, true);
    check("coercion records a value-preserving bit-integer promotion",
          !th_is_bottom(th_coerce(
              ctx, bits8,
              (th_coerce_policy){.allow = TH_COERCE_PROMOTE,
                                 .target = bits16})));
}

static bool wrap_i32_to_string(th_ctx *ctx, th_type *from, th_type *to,
                               void *user)
{
    (void)ctx;

    *(int *)user += 1;

    return th_kind_of(from) == TH_NAMED && th_kind_of(to) == TH_NAMED;
}

static void test_a_coercion_is_recorded_with_what_it_did(th_ctx *ctx)
{
    th_type *i32 = th_name(ctx, S("i32"));
    th_type *i64 = th_name(ctx, S("i64"));

    th_register_promotion(ctx, S("i32"), S("i64"));
    th_coercions_clear(ctx);

    th_coerce_policy promo = {.allow = TH_COERCE_PROMOTE, .target = i64};

    check("an exact match records nothing to insert",
          !th_is_bottom(th_coerce(ctx, i64, promo))
              && th_coercion_count(ctx) == 0);

    th_coercion_t c = {0};

    check("a promotion is recorded",
          !th_is_bottom(th_coerce(ctx, i32, promo))
              && th_coercion_count(ctx) == 1
              && th_coercion_at(ctx, 0, &c));
    check("with the kind that applied", c.kind == TH_COERCE_PROMOTE);
    check("and the types it went between", c.from == i32 && c.to == i64);
}

static void test_option_wrapping(th_ctx *ctx)
{
    th_type *i32 = th_name(ctx, S("i32"));
    th_type *opt = th_option(ctx, i32);
    th_type *arms[] = {th_nil(ctx), i32};
    th_type *doubled[] = {opt, th_nil(ctx)};

    check("a union with nil is the option of its other arms",
          th_hash(ctx, th_sum(ctx, TH_UNION, arms, 2)) == th_hash(ctx, opt)
              && th_hash(ctx, th_sum(ctx, TH_UNION, doubled, 2))
                     == th_hash(ctx, opt));

    th_coercions_clear(ctx);

    th_coerce_policy off = {.allow = TH_COERCE_NONE, .target = opt};
    th_coerce_policy on  = {.allow = TH_COERCE_OPTION_WRAP, .target = opt};

    check("T does not become option[T] unless wrapping is allowed",
          th_is_bottom(th_coerce(ctx, i32, off)));
    check("and nothing is recorded when it is refused",
          th_coercion_count(ctx) == 0);
    check("T becomes option[T] when wrapping is allowed",
          !th_is_bottom(th_coerce(ctx, i32, on)));

    th_coercion_t c = {0};

    check("and the wrap is recorded",
          th_coercion_count(ctx) == 1 && th_coercion_at(ctx, 0, &c)
              && c.kind == TH_COERCE_OPTION_WRAP);
}

static void test_a_registered_coercion_is_consulted(th_ctx *ctx)
{
    th_type *i32  = th_name(ctx, S("i32"));
    th_type *str  = th_name(ctx, S("string"));
    int      hits = 0;

    th_coercions_clear(ctx);
    th_register_coercion(ctx, wrap_i32_to_string, &hits);

    th_coerce_policy none   = {.allow = TH_COERCE_NONE, .target = str};
    th_coerce_policy custom = {.allow = TH_COERCE_CUSTOM, .target = str};

    check("a registered coercion is not consulted unless allowed",
          th_is_bottom(th_coerce(ctx, i32, none)) && hits == 0);
    check("it is consulted when allowed",
          !th_is_bottom(th_coerce(ctx, i32, custom)) && hits == 1);

    th_coercion_t c = {0};

    check("and what it did is recorded",
          th_coercion_count(ctx) == 1 && th_coercion_at(ctx, 0, &c)
              && c.kind == TH_COERCE_CUSTOM);

    th_register_coercion(ctx, nullptr, nullptr);
}

static th_span_t at(core_string_t file, uint32_t line, uint32_t col)
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

    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    check("a fresh context has no errors", th_error_count(ctx) == 0);
    check("and no last error", th_last_error(ctx).data == nullptr);

    th_ctx_set_span(ctx, at(S("a.n00b"), 12, 4));

    check("the span is readable back", th_ctx_span(ctx).start_line == 12);

    check("unifying two different types fails",
          th_unify(ctx, i, s) == nullptr);

    th_error_t e;

    check("the failure is recorded", th_error_count(ctx) > 0);
    check("and readable", th_error_at(ctx, 0, &e));
    check("it carries the span it was raised at",
          e.span.start_line == 12 && e.span.start_col == 4
              && core_string_eq(e.span.file, S("a.n00b")));
    check("it names a kind", e.kind == TH_ERR_UNIFY);
    check("the kind has a name",
          core_string_eq(th_err_kind_name(e.kind), S("unify")));
    check("it carries what was expected and what was got",
          e.expected != nullptr && e.got != nullptr);
    check("and a message", e.message.data != nullptr);
    check("th_last_error still answers", th_last_error(ctx).data != nullptr);

    th_errors_clear(ctx);

    check("errors can be cleared", th_error_count(ctx) == 0);
    check("and the last error with them", th_last_error(ctx).data == nullptr);

    th_ctx_free(ctx);
}

static void test_errors_accumulate_with_their_own_spans(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type *i = th_name(ctx, S("int"));
    th_type *s = th_name(ctx, S("string"));

    th_ctx_set_span(ctx, at(S("a.n00b"), 1, 1));
    th_unify(ctx, i, s);

    th_ctx_set_span(ctx, at(S("b.n00b"), 9, 3));
    th_ctx_set_related(ctx, at(S("a.n00b"), 1, 1));
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
              && core_string_eq(second.related.file, S("a.n00b")));

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

    th_type *i = th_name(ctx, S("int"));

    th_member ma = {.name = S("x"), .type = i};
    th_member mb = {.name = S("y"), .type = i};

    th_type *ra = th_record(ctx, &ma, 1);
    th_type *rb = th_record(ctx, &mb, 1);

    th_unify(ctx, ra, rb);

    th_error_t e;

    th_error_at(ctx, 0, &e);
    check("a missing member is a no-such-field, not a bare mismatch",
          e.kind == TH_ERR_NO_SUCH_FIELD);
    check("and it names the member",
          e.constraint.data && core_string_eq(e.constraint, S("x")));

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
    th_type *lv = th_apply(ctx, S("list"), &la, 1);
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

    th_type *i = th_name(ctx, S("int"));
    th_type *v = th_var_named(ctx, S("t"));

    check("an unconstrained var has no where clause",
          !contains(th_print(ctx, th_var_named(ctx, S("u"))), S("where")));

    th_var_constrain(ctx, v,
                     (th_constraint){.kind = TH_CON_EXCLUDES, .target = i});
    th_var_constrain(ctx, v,
                     (th_constraint){.kind  = TH_CON_IMPLEMENTS,
                                     .iface = S("Numeric")});

    core_string_t got = th_print(ctx, v);

    check("the where clause names the variable",
          contains(got, S("where `t:")));
    check("it renders each bound",
          contains(got, S("!= int")) && contains(got, S("Numeric")));
    check("and joins them", contains(got, S(" + ")));

    th_ctx_free(ctx);
}

static void test_open_function_prints_free_variable_constraints_once(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *v = th_var_named(ctx, S("item"));
    th_constraint copy = {.kind = TH_CON_IMPLEMENTS, .iface = S("Copy")};

    check("a trait obligation can be added to an open variable",
          th_var_constrain(ctx, v, copy));
    check("repeating a trait obligation is idempotent",
          th_var_constrain(ctx, v, copy) &&
          th_var_constraint_count(v) == 1);

    th_param param = {.mode = TH_IN, .type = v};
    core_string_t got = th_print(ctx, th_func(ctx, v, &param, 1,
                                             nullptr, nullptr, 0));
    check("an open function prints its free-variable constraint",
          contains(got, S("(`a) -> `a where `a: Copy")));
    check("an open function prints the obligation only once",
          !contains(got, S("Copy + Copy")));

    th_ctx_free(ctx);
}

static void test_methods_are_attached_to_receiver_types(void)
{
    th_ctx *ctx = th_ctx_new();
    th_member field = {.name = S("value"), .type = th_name(ctx, S("int"))};
    th_type *receiver = th_record(ctx, &field, 1);
    th_type *signature = th_func(ctx, th_name(ctx, S("string")), nullptr, 0,
                                 nullptr, nullptr, 0);

    check("a method signature registers on its receiver",
          th_register_method(ctx, receiver, S("render"), signature));
    check("the registered signature is found by receiver and name",
          th_method_lookup(ctx, receiver, S("render")) == signature);

    th_member same_field = {.name = S("value"),
                            .type = th_name(ctx, S("int"))};
    th_type *equivalent = th_record(ctx, &same_field, 1);

    check("an equivalent receiver spelling finds the same method",
          th_method_lookup(ctx, equivalent, S("render")) == signature);
    check("a different method name does not match",
          th_method_lookup(ctx, receiver, S("missing")) == nullptr);

    th_ctx_free(ctx);
}

static th_type *derived_method(th_ctx *ctx, th_type *receiver,
                               core_string_t name, void *user)
{
    (void)user;
    if (!core_string_eq(name, S("invoke"))) return nullptr;
    th_param parameter = {.mode = TH_IN, .type = receiver};
    return th_func(ctx, th_name(ctx, S("bool")), &parameter, 1,
                   nullptr, nullptr, 0);
}

static void test_method_resolvers_derive_signatures(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *receiver = th_name(ctx, S("Callable"));
    th_env *environment = th_env_new();
    check("a derived method resolver registers",
          th_register_method_resolver(ctx, derived_method, nullptr));
    check("derived method resolver registration is idempotent",
          th_register_method_resolver(ctx, derived_method, nullptr));
    th_type *signature = th_method_lookup(ctx, receiver, S("invoke"));
    check("a method resolver derives a signature for a receiver",
          signature && th_kind_of(signature) == TH_FUNCTION
              && th_func_nparams(signature) == 1
              && th_func_ret(signature)
              && th_hash(ctx, th_func_ret(signature))
                    == th_hash(ctx, th_name(ctx, S("bool"))));
    check("a method resolver may decline an unrelated method",
          th_method_lookup(ctx, receiver, S("missing")) == nullptr);
    th_type *deferred_receiver = th_var(ctx);
    th_type *result = th_var(ctx);
    th_type *call = th_func(ctx, result, nullptr, 0, nullptr, nullptr, 0);
    check("a derived method resolver satisfies deferred method constraints",
          environment && th_env_freeze(environment)
              && th_ctx_set_environment(ctx, environment, S("test"),
                                        S("test"))
              && th_require_method(ctx, deferred_receiver, S("invoke"),
                                   result, call)
              && th_unify_in_place(ctx, deferred_receiver, receiver)
              && th_unify_probe(ctx, result, th_name(ctx, S("bool"))));
    th_ctx_free(ctx);
    th_env_free(environment);
}

static void test_function_error_channels_unify_and_generalize(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *error = th_var(ctx);
    th_type *fallible = th_func_with_error(
        ctx, th_name(ctx, S("int")), error, nullptr, 0, nullptr, nullptr, 0);
    th_type *string_failure = th_func_with_error(
        ctx, th_name(ctx, S("int")), th_name(ctx, S("string")), nullptr, 0,
        nullptr, nullptr, 0);

    check("function unification includes its failure channel",
          th_unify(ctx, fallible, string_failure) != nullptr &&
          th_unify(ctx, error, th_name(ctx, S("string"))) != nullptr);

    th_type *generic_error = th_var(ctx);
    th_type *generic = th_func_with_error(
        ctx, th_name(ctx, S("int")), generic_error, nullptr, 0, nullptr,
        nullptr, 0);
    th_type *scheme = th_generalize(ctx, generic);
    th_type *instance = scheme ? th_instantiate(ctx, scheme) : nullptr;
    th_type *instance_error = instance ? th_func_error(instance) : nullptr;

    check("function generalization includes variables in the failure channel",
          scheme && instance_error && instance_error != generic_error &&
          th_kind_of(th_resolve(instance_error)) == TH_VAR &&
          th_unify(ctx, instance_error, th_name(ctx, S("bool"))) != nullptr);
    check("a function without a failure channel remains distinct",
          th_func_error(th_func(ctx, th_name(ctx, S("int")), nullptr, 0,
                                nullptr, nullptr, 0)) == nullptr &&
          th_unify(ctx, fallible,
              th_func(ctx, th_name(ctx, S("int")), nullptr, 0, nullptr,
                      nullptr, 0)) == nullptr);
    th_ctx_free(ctx);
}

static void test_named_type_record_definitions(void)
{
    th_ctx *ctx = th_ctx_new();
    th_member members[] = {
        {.name = S("amount"), .type = th_name(ctx, S("int"))},
    };
    th_type *named = th_name(ctx, S("FieldRecord"));
    th_type *record = th_record(ctx, members, 1);
    th_type *different = th_record(ctx, nullptr, 0);

    check("named types retain their identity while exposing record shape",
          th_type_define(ctx, named, record)
          && th_type_definition(ctx, named) == record
          && th_record_lookup_in(ctx, named, S("amount")) == members[0].type
          && th_record_lookup(named, S("amount")) == nullptr);
    check("a named type definition cannot be silently replaced",
          !th_type_define(ctx, named, different));
    th_type *nominal = th_name(ctx, S("Amount"));
    th_type *integer = th_name(ctx, S("int"));
    check("named types can retain non-record representations",
          th_type_define(ctx, nominal, integer)
              && th_type_definition(ctx, nominal) == integer
              && !th_unify(ctx, nominal, integer)
              && !th_record_lookup_in(ctx, nominal, S("amount")));
    th_ctx_free(ctx);
}

static void test_generic_record_definitions(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *key = th_var_named(ctx, S("K"));
    th_type *value = th_var_named(ctx, S("V"));
    th_arg parameters[] = {
        {.kind = TH_ARG_TYPE, .type = key},
        {.kind = TH_ARG_TYPE, .type = value},
    };
    th_type *template = th_apply(ctx, S("Pair"), parameters, 2);
    th_member members[] = {
        {.name = S("left"), .type = key},
        {.name = S("right"), .type = value},
    };
    th_type *definition = th_record(ctx, members, 2);
    th_type *integer = th_name(ctx, S("int"));
    th_type *string = th_name(ctx, S("string"));
    th_arg arguments[] = {
        {.kind = TH_ARG_TYPE, .type = integer},
        {.kind = TH_ARG_TYPE, .type = string},
    };
    th_type *concrete = th_apply(ctx, S("Pair"), arguments, 2);
    th_type *bare = th_name(ctx, S("Pair"));
    check("generic records retain their ordered field template",
          th_type_define(ctx, template, definition)
          && th_type_definition(ctx, template) == definition
          && th_type_definition(ctx, bare) == nullptr);
    th_type *specialized = th_type_definition(ctx, concrete);
    check("generic record fields specialize without refining the template",
          specialized && th_kind_of(specialized) == TH_RECORD
          && th_hash(ctx, th_record_lookup_in(ctx, concrete, S("left")))
              == th_hash(ctx, integer)
          && th_hash(ctx, th_record_lookup_in(ctx, concrete, S("right")))
              == th_hash(ctx, string)
          && th_record_lookup(definition, S("left")) == key
          && th_record_lookup(definition, S("right")) == value
          && th_type_definition(ctx, concrete) == specialized);
    th_type *first = th_var_named(ctx, S("First"));
    th_type *second = th_var_named(ctx, S("Second"));
    th_arg first_args[] = {
        {.kind = TH_ARG_TYPE, .type = first},
        {.kind = TH_ARG_TYPE, .type = string},
    };
    th_arg second_args[] = {
        {.kind = TH_ARG_TYPE, .type = second},
        {.kind = TH_ARG_TYPE, .type = string},
    };
    th_type *first_pair = th_apply(ctx, S("Pair"), first_args, 2);
    th_type *second_pair = th_apply(ctx, S("Pair"), second_args, 2);
    check("open generic records preserve distinct variable identities",
          th_record_lookup_in(ctx, first_pair, S("left")) == first
          && th_record_lookup_in(ctx, second_pair, S("left")) == second);
    th_ctx_free(ctx);
}

static void test_unification_probe(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *variable = th_var(ctx);
    th_type *integer = th_name(ctx, S("int"));
    th_type *string = th_name(ctx, S("string"));
    check("a unification probe leaves its inputs unbound",
          th_unify_probe(ctx, variable, integer)
              && th_kind_of(th_resolve(variable)) == TH_VAR);
    size_t errors = th_error_count(ctx);
    check("a failed unification probe retains no diagnostic",
          !th_unify_probe(ctx, integer, string)
              && th_error_count(ctx) == errors);
    th_ctx_free(ctx);
}

static core_string_t display_type_name(core_string_t name, void *context)
{
    (void)context;
    return core_string_eq(name, S("list")) ? S("List") : (core_string_t){};
}

static void test_configurable_type_printing(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *item = th_name(ctx, S("int"));
    th_arg argument = {.kind = TH_ARG_TYPE, .type = item};
    th_type *list = th_apply(ctx, S("list"), &argument, 1);
    th_type *reference = th_ind(ctx, 0, list);
    th_type *sequence = th_array(ctx, reference);
    th_print_options_t options = {
        .named = display_type_name,
        .sequence = S("Array"),
        .reference = S("Ref"),
    };
    check("type printing accepts language-specific display spellings",
          core_string_eq(th_print_with_options(ctx, sequence, &options),
                         S("Array[Ref[List[int]]]"))
              && core_string_eq(th_print(ctx, sequence),
                                S("array[ref[list[int]]]")));
    th_ctx_free(ctx);
}

static void test_name_syntax(void)
{
    const char invalid_utf8[] = {'x', (char)0xc0, (char)0xaf};
    check("typehash names accept qualified identifiers",
          th_name_syntax_valid(S("package.module.Type")));
    check("typehash names accept well-formed Unicode spelling",
          th_name_syntax_valid(S("caf\xc3\xa9")));
    check("typehash names reject punctuation in a component",
          !th_name_syntax_valid(S("my-module.Type")));
    check("typehash names reject malformed UTF-8",
          !th_name_syntax_valid(
              core_string_borrow(invalid_utf8, sizeof(invalid_utf8))));
    check("typehash names reject empty components",
          !th_name_syntax_valid(S("package..Type")));
    check("typehash names reject missing and numeric starts",
          !th_name_syntax_valid(core_string_borrow(nullptr, 1))
              && !th_name_syntax_valid(S(""))
              && !th_name_syntax_valid(S("1Type")));
}

static void test_clone_replacing(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *original = th_var_named(ctx, S("T"));
    th_type *declared = th_var_named(ctx, S("T"));
    th_type *sequence = th_array(ctx, original);
    const th_type *from[] = {original};
    th_type *to[] = {declared};
    th_type *copy = th_type_clone_replacing(ctx, sequence, from, to, 1);
    check("clone replacement rebases a nested variable without changing source",
          copy && copy != sequence && th_seq_elem(copy) == declared
              && th_seq_elem(sequence) == original);
    check("clone replacement can replace its root",
          th_type_clone_replacing(ctx, original, from, to, 1) == declared);
    th_type *alias = th_type_clone(ctx, original);
    th_type *aliased_sequence = alias ? th_array(ctx, alias) : nullptr;
    th_type *aliased_copy = aliased_sequence
        ? th_type_clone_replacing(ctx, aliased_sequence, from, to, 1)
        : nullptr;
    check("clone replacement follows copied variable identity",
          aliased_copy && th_seq_elem(aliased_copy) == declared);
    th_type *unrelated = th_var_named(ctx, S("T"));
    th_type *unrelated_sequence = th_array(ctx, unrelated);
    th_type *unrelated_copy = th_type_clone_replacing(ctx,
        unrelated_sequence, from, to, 1);
    check("clone replacement leaves a distinct same-name variable alone",
          unrelated_copy && th_seq_elem(unrelated_copy) != declared);
    const th_type *conflicting_from[] = {original, original};
    th_type *conflicting_to[] = {declared, original};
    check("clone replacement rejects conflicting mappings",
          !th_type_clone_replacing(ctx, sequence, conflicting_from,
                                   conflicting_to, 2));
    th_ctx_free(ctx);
}

static void test_clone_shared_constraint_context(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *shared = th_var(ctx);
    th_type *constrained = th_var(ctx);
    th_constraint link = {.kind = TH_CON_UNIFIES, .target = shared};
    th_constraint trait = {.kind = TH_CON_IMPLEMENTS, .iface = S("Example"),
                           .scope_module = S("module"),
                           .scope_package = S("package"),
                           .has_scope = true};
    const th_type *sources[] = {constrained, shared};
    th_type *copies[2] = {0};
    th_constraint copied_link = {0};
    th_constraint copied_trait = {0};
    check("shared constraint context is cloned without source aliases",
          th_var_constrain(ctx, constrained, link)
              && th_var_constrain(ctx, shared, trait)
              && th_type_clone_many(ctx, sources, copies, 2)
              && copies[0] != constrained && copies[1] != shared
              && th_var_constraint_at(copies[0], 0, &copied_link)
              && copied_link.target == copies[1]
              && th_var_constraint_at(copies[1], 0, &copied_trait)
              && copied_trait.has_scope
              && core_string_eq(copied_trait.scope_module, S("module"))
              && core_string_eq(copied_trait.scope_package, S("package")));
    th_ctx_free(ctx);
}

static void test_call_unification_keeps_declared_constraints(void)
{
    th_ctx *ctx = th_ctx_new();
    th_type *variable = th_var_named(ctx, S("item"));
    th_constraint excludes_string = {
        .kind = TH_CON_EXCLUDES,
        .target = th_name(ctx, S("string")),
    };
    th_param parameter = {.mode = TH_IN, .type = variable};
    th_type *declared = th_func(ctx, variable, &parameter, 1,
                                nullptr, nullptr, 0);
    th_type *integer = th_name(ctx, S("int"));
    th_type *string = th_name(ctx, S("string"));
    th_param int_parameter = {.mode = TH_IN, .type = integer};
    th_param string_parameter = {.mode = TH_IN, .type = string};
    th_type *int_call = th_func(ctx, integer, &int_parameter, 1,
                                nullptr, nullptr, 0);
    th_type *string_call = th_func(ctx, string, &string_parameter, 1,
                                   nullptr, nullptr, 0);
    check("a declared constraint accepts a compatible call",
          th_var_constrain(ctx, variable, excludes_string)
              && th_unify(ctx, declared, int_call) != nullptr);
    check("copy-then-unify rejects a call excluded by the declaration",
          th_unify(ctx, declared, string_call) == nullptr);
    check("call specialization does not refine the declaration",
          th_resolve(variable) == variable
              && th_var_constraint_count(variable) == 1);
    th_ctx_free(ctx);
}

int main(void)
{
    th_ctx *ctx = th_ctx_new();

    test_unifies(ctx);
    test_one_of(ctx);
    test_excludes(ctx);
    test_repeated_constraints(ctx);
    test_kind(ctx);
    test_promotes(ctx);
    test_has_field(ctx);
    test_has_field_auto_deref(ctx);
    test_has_field_reports_field_constraint(ctx);
    test_has_param(ctx);
    test_constraints_move_between_vars(ctx);
    test_implements(ctx);
    test_open_records_accumulate(ctx);
    test_open_records_merge_across_vars(ctx);
    test_coerce(ctx);
    test_a_coercion_is_recorded_with_what_it_did(ctx);
    test_option_wrapping(ctx);
    test_a_registered_coercion_is_consulted(ctx);

    th_ctx_free(ctx);

    test_constraints_are_not_identity();
    test_errors_are_structured();
    test_errors_accumulate_with_their_own_spans();
    test_failures_name_what_went_wrong();
    test_a_constrained_var_prints_its_bound();
    test_open_function_prints_free_variable_constraints_once();
    test_methods_are_attached_to_receiver_types();
    test_method_resolvers_derive_signatures();
    test_function_error_channels_unify_and_generalize();
    test_named_type_record_definitions();
    test_generic_record_definitions();
    test_unification_probe();
    test_configurable_type_printing();
    test_name_syntax();
    test_clone_replacing();
    test_clone_shared_constraint_context();
    test_call_unification_keeps_declared_constraints();

    return report("th constraints");
}
