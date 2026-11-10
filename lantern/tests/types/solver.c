#include "types/solver.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

typedef struct {
    th_ctx               *types;
    core_datalog_engine_t *logic;
    ty_solver_t          *s;
} world_t;

static world_t open_world(void)
{
    world_t w = {.types = th_ctx_new(), .logic = core_datalog_engine_new()};

    w.s = ty_solver_new(w.types, w.logic);

    return w;
}

static void close_world(world_t *w)
{
    ty_solver_free(w->s);
    core_datalog_engine_free(w->logic);
    th_ctx_free(w->types);
}

static ty_why_t rule(uint16_t number)
{
    return (ty_why_t){.rule = number};
}

static bool is(th_type *t, th_type *want)
{
    th_type *got = th_resolve(t);

    if (!got || !want || !th_same_shape(got, want)) {
        return false;
    }
    for (size_t i = 0; i < th_child_count(got); i++) {
        th_type *x = th_child(got, i);
        th_type *y = th_child(want, i);

        if ((x || y) && (!x || !y || !is(x, th_resolve(y)))) {
            return false;
        }
    }

    return true;
}

static th_type *list_of(th_ctx *types, th_type *item)
{
    th_arg arg = {.kind = TH_ARG_TYPE, .type = item};

    return th_apply(types, S("List"), &arg, 1);
}

static ty_fail_kind_t last_failure(world_t *w)
{
    size_t n = ty_failure_count(w->s);

    return n ? ty_failure_at(w->s, n - 1)->kind : (ty_fail_kind_t)255;
}

static void test_a_variable_takes_what_flows_in(void)
{
    world_t  w     = open_world();
    th_type *a     = ty_fresh(w.s, (ty_origin_t){0});
    th_type *str   = th_name(w.types, S("string"));

    check("a fresh variable unifies with a concrete type",
          ty_unify(w.s, a, str, rule(2)) && is(a, str));
    check("and a mismatch afterwards is a failure",
          !ty_unify(w.s, a, th_name(w.types, S("int")), rule(2))
              && last_failure(&w) == TY_FAIL_MISMATCH);
    close_world(&w);
}

static void test_nil_then_a_value_is_an_option(void)
{
    world_t  w   = open_world();
    th_type *r   = ty_fresh(w.s, (ty_origin_t){0});
    th_type *str = th_name(w.types, S("string"));

    ty_unify(w.s, r, th_nil(w.types), rule(5));
    ty_unify(w.s, r, str, rule(5));
    check("nil and then a string make a string option",
          is(r, th_option(w.types, str)) && !ty_failure_count(w.s));
    close_world(&w);
}

static void test_a_value_then_nil_is_an_option(void)
{
    world_t  w   = open_world();
    th_type *r   = ty_fresh(w.s, (ty_origin_t){0});
    th_type *str = th_name(w.types, S("string"));

    ty_unify(w.s, r, str, rule(5));
    ty_unify(w.s, r, th_nil(w.types), rule(5));
    check("a string and then nil also make a string option",
          is(r, th_option(w.types, str)) && !ty_failure_count(w.s));
    close_world(&w);
}

static void test_a_declared_type_does_not_take_nil(void)
{
    world_t  w   = open_world();
    th_type *num = th_name(w.types, S("int"));

    check("nil into a declared int is an error, not a widening",
          !ty_unify(w.s, num, th_nil(w.types), rule(5))
              && last_failure(&w) == TY_FAIL_NIL);
    close_world(&w);
}

static void test_a_join_with_nil_leaves_the_operand_alone(void)
{
    world_t  w   = open_world();
    th_type *a   = ty_fresh(w.s, (ty_origin_t){0});
    th_type *num = th_name(w.types, S("int"));
    th_type *j   = ty_join(w.s, a, th_nil(w.types), rule(4));

    check("joining an unknown with nil gives its option",
          j && th_option_item(th_resolve(j)) == a);
    check("and the operand itself stays unknown",
          th_kind_of(th_resolve(a)) == TH_VAR);
    check("a join of int and nil is an int option",
          is(ty_join(w.s, num, th_nil(w.types), rule(4)),
             th_option(w.types, num)));
    check("a join of two unknowns makes them one",
          ty_join(w.s, a, ty_fresh(w.s, (ty_origin_t){0}), rule(4)) != nullptr);
    close_world(&w);
}

static void test_children_unify_and_cycles_fail(void)
{
    world_t  w   = open_world();
    th_type *a   = ty_fresh(w.s, (ty_origin_t){0});
    th_type *num = th_name(w.types, S("int"));

    check("a list of an unknown unifies with a list of int",
          ty_unify(w.s, list_of(w.types, a), list_of(w.types, num), rule(2))
              && is(a, num));

    th_type *b = ty_fresh(w.s, (ty_origin_t){0});

    check("a type that would contain itself fails",
          !ty_unify(w.s, b, list_of(w.types, b), rule(2))
              && last_failure(&w) == TY_FAIL_OCCURS);
    close_world(&w);
}

static void test_traits_apply_when_the_methods_are_there(void)
{
    world_t       w      = open_world();
    core_string_t hash[] = {S("hash")};
    th_type      *str    = th_name(w.types, S("string"));
    th_type      *num    = th_name(w.types, S("int"));
    th_type      *a      = ty_fresh(w.s, (ty_origin_t){0});

    ty_declare_trait(w.s, S("Hash"), hash, 1);
    ty_declare_method(w.s, str, S("hash"));

    ty_pred_t wants_hash = {.kind = TY_PRED_IMPLEMENTS, .name = S("Hash"),
                            .subject = a};

    check("a predicate on an unknown waits",
          ty_require(w.s, wants_hash, rule(18)) && ty_pending_count(w.s) == 1);
    check("and holds once the unknown is a type with the methods",
          ty_unify(w.s, a, str, rule(2)) && ty_pending_count(w.s) == 0
              && !ty_failure_count(w.s));

    wants_hash.subject = num;
    check("a type without the method fails at once",
          !ty_require(w.s, wants_hash, rule(18))
              && last_failure(&w) == TY_FAIL_PREDICATE);
    close_world(&w);
}

static void test_a_marker_trait_needs_an_implementation(void)
{
    world_t  w   = open_world();
    th_type *num = th_name(w.types, S("int"));
    th_type *str = th_name(w.types, S("string"));

    ty_declare_trait(w.s, S("Some_Integer"), nullptr, 0);
    ty_declare_impl(w.s, num, S("Some_Integer"));

    ty_pred_t integer = {.kind = TY_PRED_IMPLEMENTS,
                         .name = S("Some_Integer"), .subject = num};

    check("an integer type is Some_Integer", ty_require(w.s, integer, rule(20)));
    integer.subject = str;
    check("a string is not, even though the trait has no methods",
          !ty_require(w.s, integer, rule(20)));
    close_world(&w);
}

static void test_an_operator_picks_its_result(void)
{
    world_t  w   = open_world();
    th_type *num = th_name(w.types, S("int"));
    th_type *a   = ty_fresh(w.s, (ty_origin_t){0});
    th_type *b   = ty_fresh(w.s, (ty_origin_t){0});
    th_type *r   = ty_fresh(w.s, (ty_origin_t){0});

    ty_declare_operator(w.s, S("Add"), num, num, num);
    ty_require(w.s, (ty_pred_t){.kind = TY_PRED_OPERATOR, .name = S("Add"),
                                .subject = a, .other = b, .result = r},
               rule(19));
    check("the result waits for both operands",
          ty_unify(w.s, a, num, rule(2)) && th_kind_of(th_resolve(r)) == TH_VAR);
    check("and is the implementation's result once both are known",
          ty_unify(w.s, b, num, rule(2)) && is(r, num)
              && !ty_failure_count(w.s));
    close_world(&w);
}

static th_type *list_len(void *context, th_type *receiver,
                         core_string_t method, th_type *call)
{
    (void)call;
    th_ctx       *types = context;
    core_string_t name  = {0};
    th_param      self  = {.mode = TH_IN, .type = receiver};

    if (!core_string_eq(method, S("len"))
        || !th_named_info(th_resolve(receiver), &name, nullptr, nullptr)
        || !core_string_eq(name, S("List"))) {
        return nullptr;
    }

    return th_func(types, th_name(types, S("int")), &self, 1, nullptr,
                   nullptr, 0);
}

static void test_dispatch_needs_a_trait_with_the_method(void)
{
    world_t       w     = open_world();
    core_string_t len[] = {S("len")};
    th_type      *num   = th_name(w.types, S("int"));
    th_type      *a     = ty_fresh(w.s, (ty_origin_t){0});

    ty_solver_set_methods(w.s, list_len, w.types);
    ty_declare_trait(w.s, S("Sized"), len, 1);
    ty_declare_method(w.s, list_of(w.types, num), S("len"));
    ty_require(w.s, (ty_pred_t){.kind = TY_PRED_DISPATCH, .name = S("len"),
                                .subject = a},
               rule(17));
    check("a.len() holds when a is a type implementing a trait with len",
          ty_unify(w.s, a, list_of(w.types, num), rule(2))
              && !ty_failure_count(w.s) && ty_pending_count(w.s) == 0);
    check("and fails for a type with no such trait",
          !ty_require(w.s, (ty_pred_t){.kind = TY_PRED_DISPATCH,
                                       .name = S("len"), .subject = num},
                      rule(17)));
    close_world(&w);
}

static void test_unified_variables_share_their_predicates(void)
{
    world_t       w      = open_world();
    core_string_t hash[] = {S("hash")};
    th_type      *a      = ty_fresh(w.s, (ty_origin_t){0});
    th_type      *b      = ty_fresh(w.s, (ty_origin_t){0});

    ty_declare_trait(w.s, S("Hash"), hash, 1);
    ty_require(w.s, (ty_pred_t){.kind = TY_PRED_IMPLEMENTS, .name = S("Hash"),
                                .subject = a},
               rule(18));
    ty_unify(w.s, a, b, rule(12));
    ty_unify(w.s, b, th_name(w.types, S("int")), rule(2));
    check("a predicate on one variable is checked when the other is bound",
          last_failure(&w) == TY_FAIL_PREDICATE && ty_pending_count(w.s) == 0);
    close_world(&w);
}

static void test_refinement_narrows_an_option(void)
{
    world_t  w   = open_world();
    th_type *str = th_name(w.types, S("string"));
    th_type *opt = th_option(w.types, str);

    check("past a nil test, a string option is a string",
          is(ty_refine(w.s, opt, str, rule(11)), str));
    check("and in the nil branch it is nil",
          th_is_nil(ty_refine(w.s, opt, th_nil(w.types), rule(11))));
    check("refining to an unrelated type fails",
          !ty_refine(w.s, opt, th_name(w.types, S("int")), rule(11)));
    close_world(&w);
}

static void test_explain_names_the_rule(void)
{
    world_t       w   = open_world();
    th_type      *r   = ty_fresh(w.s, (ty_origin_t){0});
    core_buffer_t out = {0};

    ty_unify(w.s, r, th_name(w.types, S("string")), rule(3));
    ty_unify(w.s, r, th_nil(w.types), rule(5));
    ty_explain(w.s, r, &out);

    core_string_t text = {.data = out.data, .u8_bytes = out.byte_len};

    check("the explanation cites each rule that shaped the type",
          text.data && core_string_find(text, S("R3")) >= 0
              && core_string_find(text, S("R5")) >= 0);
    close_world(&w);
}

static void test_a_generalized_type_is_copied_on_use(void)
{
    world_t  w     = open_world();
    th_type *a     = ty_fresh(w.s, (ty_origin_t){0});
    th_param param = {.mode = TH_IN, .type = a};
    th_type *id    = th_func(w.types, a, &param, 1, nullptr, nullptr, 0);
    th_type *fixed = ty_generalize(w.s, id);
    th_type *one   = ty_instantiate(w.s, fixed, rule(2));
    th_type *two   = ty_instantiate(w.s, fixed, rule(2));
    th_type *num   = th_name(w.types, S("int"));
    th_type *str   = th_name(w.types, S("string"));

    check("each use gets its own copy of a generic function's type",
          ty_unify(w.s, th_func_param(one, 0)->type, num, rule(25))
              && ty_unify(w.s, th_func_param(two, 0)->type, str, rule(25)));
    check("and the copies keep the link between parameter and result",
          is(th_func_ret(one), num) && is(th_func_ret(two), str));
    check("while the fixed type itself is untouched",
          th_kind_of(th_resolve(th_func_ret(fixed))) == TH_VAR);
    close_world(&w);
}

static void test_open_predicates_travel_with_the_copy(void)
{
    world_t       w      = open_world();
    core_string_t hash[] = {S("hash")};
    th_type      *a      = ty_fresh(w.s, (ty_origin_t){0});

    ty_declare_trait(w.s, S("Hash"), hash, 1);
    ty_require(w.s, (ty_pred_t){.kind = TY_PRED_IMPLEMENTS, .name = S("Hash"),
                                .subject = a},
               rule(18));

    th_type *copy = ty_instantiate(w.s, ty_generalize(w.s, a), rule(2));

    ty_unify(w.s, copy, th_name(w.types, S("int")), rule(25));
    check("an open predicate travels with the generic type, so a copy bound "
          "to a type without the method fails",
          copy != a && last_failure(&w) == TY_FAIL_PREDICATE);
    close_world(&w);
}

static void test_an_option_of_nothing_settles_to_nil(void)
{
    world_t  w = open_world();
    th_type *r = ty_fresh(w.s, (ty_origin_t){0});

    ty_unify(w.s, r, th_nil(w.types), rule(5));
    check("a slot that only ever received nil is nil once solved",
          ty_solve(w.s) && th_is_nil(th_resolve(r)));
    close_world(&w);
}

static void test_nil_joined_with_an_unknown_stays_open(void)
{
    world_t  w = open_world();
    th_type *r = ty_fresh(w.s, (ty_origin_t){0});
    th_type *x = ty_fresh(w.s, (ty_origin_t){0});

    ty_unify(w.s, r, th_nil(w.types), rule(5));
    ty_unify(w.s, r, x, rule(5));
    check("nil and an unknown value make an option of the unknown, not nil",
          ty_solve(w.s) && th_option_item(th_resolve(r))
              && th_resolve(th_option_item(th_resolve(r))) == th_resolve(x));
    close_world(&w);
}

int main(void)
{
    test_a_variable_takes_what_flows_in();
    test_nil_then_a_value_is_an_option();
    test_a_value_then_nil_is_an_option();
    test_a_declared_type_does_not_take_nil();
    test_a_join_with_nil_leaves_the_operand_alone();
    test_children_unify_and_cycles_fail();
    test_traits_apply_when_the_methods_are_there();
    test_a_marker_trait_needs_an_implementation();
    test_an_operator_picks_its_result();
    test_dispatch_needs_a_trait_with_the_method();
    test_unified_variables_share_their_predicates();
    test_refinement_narrows_an_option();
    test_explain_names_the_rule();
    test_a_generalized_type_is_copied_on_use();
    test_open_predicates_travel_with_the_copy();
    test_an_option_of_nothing_settles_to_nil();
    test_nil_joined_with_an_unknown_stays_open();

    return report("types solver");
}
