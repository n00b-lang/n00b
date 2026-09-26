#include <stdio.h>
#include <string.h>

#include "th/typehash.h"
#include "harness.h"

static th_type *ptr_to(th_ctx *ctx, th_type *t)
{
    return th_ind(ctx, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, t);
}

static th_type *rec_self(th_ctx *ctx, const char *field)
{
    th_type  *r = th_record_stub(ctx);
    th_member m = {.name = field, .type = ptr_to(ctx, r)};

    th_record_fill(ctx, r, &m, 1);

    return r;
}

static th_type *rec_var(th_ctx *ctx, const char *field, th_type **out_var)
{
    th_type  *r = th_record_stub(ctx);
    th_type  *v = th_var(ctx);
    th_member m[2];

    m[0] = (th_member){.name = field, .type = ptr_to(ctx, r)};
    m[1] = (th_member){.name = "payload", .type = v};

    th_record_fill(ctx, r, m, 2);
    *out_var = v;

    return r;
}

int main(void)
{
    th_ctx *ctx = th_ctx_new();

    th_type *a = rec_self(ctx, "next");

    check("free_vars terminates on a cyclic closed type",
          th_free_vars(ctx, a, nullptr) == 0);
    check("cyclic closed type is closed", th_is_closed(ctx, a));

    th_type *ca = th_instantiate(ctx, th_generalize(ctx, a));

    check("instantiating a cyclic type terminates and is non-null",
          ca != nullptr);
    check("instantiating a cyclic type preserves identity",
          th_hash(ctx, ca) == th_hash(ctx, a));

    th_type *sa = th_apply_subst(ctx, a);

    check("apply_subst on a cyclic type preserves identity",
          th_hash(ctx, sa) == th_hash(ctx, a));

    th_type *u = th_unify(ctx, a, a);

    check("a cyclic type unifies with itself", u != nullptr);

    if (u) {
        check("cyclic self-unification preserves identity",
              th_hash(ctx, u) == th_hash(ctx, a));
    }

    th_type *v1;
    th_type *b = rec_var(ctx, "next", &v1);

    check("free_vars finds the variable inside a cycle",
          th_free_vars(ctx, b, nullptr) == 1);
    check("cyclic type with a variable is not closed", !th_is_closed(ctx, b));
    check("occurs finds a variable inside a cycle", th_occurs(ctx, v1, b));

    th_type *v2;
    th_type *c = rec_var(ctx, "next", &v2);

    th_type *bc = th_unify(ctx, b, c);

    check("two cyclic types with variables unify", bc != nullptr);

    th_type *d   = rec_self(ctx, "next");
    th_type *bad = th_unify(ctx, b, d);

    check("cyclic types with differing field sets do not unify",
          bad == nullptr);

    th_type *var = th_var(ctx);

    check("occurs is false for an unrelated variable",
          !th_occurs(ctx, var, a));

    th_type *bound = th_unify(ctx, var, a);

    check("a bare variable unifies with a cyclic type", bound != nullptr);

    if (bound) {
        check("binding a variable to a cyclic type yields that type",
              th_hash(ctx, bound) == th_hash(ctx, a));
    }

    th_type *gen = th_generalize(ctx, b);

    check("generalize over a cyclic type yields a scheme",
          gen && th_is_scheme(gen));

    if (gen) {
        th_type *inst = th_instantiate(ctx, gen);

        check("instantiate reopens a cyclic scheme",
              inst && !th_is_scheme(inst));
        check("instantiated cyclic scheme has one free variable",
              inst && th_free_vars(ctx, inst, nullptr) == 1);
        check("generalize round-trips through instantiate on a cyclic type",
              inst && th_hash(ctx, th_generalize(ctx, inst))
                          == th_hash(ctx, gen));
    }

    th_type *outer = th_var(ctx);
    th_type *inner = th_var(ctx);
    th_type *members[] = {outer, inner};
    th_type *pair = th_tuple(ctx, members, 2);
    th_type *environment[] = {outer};
    th_type *relative = th_generalize_excluding(ctx, pair, environment, 1);
    uint64_t arity = 0;
    check("environment-aware generalization leaves outer variables free",
          relative && th_binder_arity(relative, &arity) && arity == 1
              && th_free_vars(ctx, relative, nullptr) == 1);
    th_type *relative_instance = relative ? th_instantiate(ctx, relative)
                                          : nullptr;
    check("instantiation freshens only quantified variables",
          relative_instance
              && th_free_vars(ctx, relative_instance, nullptr) == 2);

    th_ctx_free(ctx);

    return report("cyclic inference");
}
