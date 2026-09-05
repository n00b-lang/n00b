#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/rule.h"
#include "ir/text.h"

static int  fails = 0;
static bool covered[IR_OP_COUNT];
static bool produced[IR_DIAG_COUNT];

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

typedef struct {
    ir_ctx_t    *ctx;
    ir_module_t *m;
    ir_rules_t  *rs;
    ir_diags_t  *d;
    ir_func_t   *f;
    ir_block_t  *b;
    uint32_t     n;
} env;

static env *open_env_on(const ir_profile_t *p, const char *ret,
                        const char *target)
{
    env *e = calloc(1, sizeof(env));

    e->ctx = ir_ctx_new();
    e->m   = ir_module_new(e->ctx, "m", p, target);
    e->rs  = ir_rules_from_profile(e->ctx, p);
    e->d   = ir_diags_new(e->ctx, nullptr);

    ir_rules_add_defaults(e->rs);

    size_t n    = strlen(ret) + 8;
    char  *sig  = malloc(n);

    snprintf(sig, n, "() -> %s", ret);
    e->f = ir_func_add(e->m, "f", ir_type_parse(e->m, sig), IR_LINK_EXTERNAL);
    free(sig);
    e->b = ir_block_add(e->f, "entry");

    return e;
}

static env *open_env(const ir_profile_t *p, const char *ret)
{
    return open_env_on(p, ret, "test");
}

static void close_env(env *e)
{
    ir_ctx_free(e->ctx);
    free(e);
}

static ir_value_t *val(env *e, const char *type_text)
{
    char name[32];

    snprintf(name, sizeof(name), "v%u", e->n++);

    return ir_param_add(e->f, name, ir_type_parse(e->m, type_text));
}

static ir_instr_t *op(env *e, ir_op_t o)
{
    covered[o < IR_OP_COUNT ? o : 0] = true;

    return ir_instr_add(e->b, o);
}

static void harvest(env *e)
{
    for (uint32_t i = 0; i < ir_diags_count(e->d); i++) {
        produced[ir_diags_at(e->d, i)->code] = true;
    }
}

static ir_verdict_t apply(env *e, ir_instr_t *in)
{
    ir_verdict_t v = ir_rule_apply(e->m, e->rs, in, e->d).verdict;

    harvest(e);

    return v;
}

static bool accepts_as(env *e, ir_instr_t *in, const char *want)
{
    ir_rule_result_t r = ir_rule_apply(e->m, e->rs, in, e->d);

    if (r.verdict != IR_RULE_ACCEPT) {
        return false;
    }

    if (!want) {
        return r.result == nullptr;
    }

    return r.result && ir_type_eq(e->m, r.result, ir_type_parse(e->m, want));
}

static void test_arithmetic(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *a = op(e, IR_OP_ADD);

    ir_instr_push_mod(a, "wrap");
    ir_instr_push_value(a, val(e, "i32"));
    ir_instr_push_value(a, val(e, "i32"));
    check("add on matching integers yields that type",
          accepts_as(e, a, "i32"));

    ir_instr_t *f = op(e, IR_OP_MUL);

    ir_instr_push_value(f, val(e, "f64"));
    ir_instr_push_value(f, val(e, "f64"));
    check("float arithmetic needs no overflow modifier",
          accepts_as(e, f, "f64"));

    ir_instr_t *nomod = op(e, IR_OP_ADD);

    ir_instr_push_value(nomod, val(e, "i32"));
    ir_instr_push_value(nomod, val(e, "i32"));
    check("integer arithmetic without an overflow modifier is rejected",
          apply(e, nomod) == IR_RULE_REJECT);

    ir_instr_t *mixed = op(e, IR_OP_SUB);

    ir_instr_push_mod(mixed, "trap");
    ir_instr_push_value(mixed, val(e, "i32"));
    ir_instr_push_value(mixed, val(e, "i64"));
    check("mismatched arithmetic operands are rejected",
          apply(e, mixed) == IR_RULE_REJECT);

    ir_instr_t *ptr = op(e, IR_OP_ADD);

    ir_instr_push_mod(ptr, "wrap");
    ir_instr_push_value(ptr, val(e, "ptr[i32]"));
    ir_instr_push_value(ptr, val(e, "ptr[i32]"));
    check("arithmetic on a pointer is rejected",
          apply(e, ptr) == IR_RULE_REJECT);

    ir_instr_t *dv = op(e, IR_OP_DIV);

    ir_instr_push_mod(dv, "wrap");
    ir_instr_push_value(dv, val(e, "i32"));
    ir_instr_push_value(dv, val(e, "i32"));
    check("div without a divide-by-zero modifier is rejected",
          apply(e, dv) == IR_RULE_REJECT);

    ir_instr_t *dv2 = op(e, IR_OP_DIV);

    ir_instr_push_mod(dv2, "wrap");
    ir_instr_push_mod(dv2, "trap");
    ir_instr_push_value(dv2, val(e, "i32"));
    ir_instr_push_value(dv2, val(e, "i32"));
    check("div with both modifiers is accepted", accepts_as(e, dv2, "i32"));

    ir_instr_t *ng = op(e, IR_OP_NEG);

    ir_instr_push_mod(ng, "sat");
    ir_instr_push_value(ng, val(e, "i32"));
    check("neg takes one operand", accepts_as(e, ng, "i32"));

    ir_instr_t *rm = op(e, IR_OP_REM);

    ir_instr_push_mod(rm, "undef");
    ir_instr_push_value(rm, val(e, "u64"));
    ir_instr_push_value(rm, val(e, "u64"));
    check("unsigned rem takes only a divide-by-zero modifier",
          accepts_as(e, rm, "u64"));

    ir_instr_t *rmx = op(e, IR_OP_REM);

    ir_instr_push_mod(rmx, "wrap");
    ir_instr_push_mod(rmx, "undef");
    ir_instr_push_value(rmx, val(e, "u64"));
    ir_instr_push_value(rmx, val(e, "u64"));
    check("unsigned rem does not take an overflow modifier too",
          apply(e, rmx) == IR_RULE_REJECT);

    ir_instr_t *sdv = op(e, IR_OP_DIV);

    ir_instr_push_mod(sdv, "trap");
    ir_instr_push_value(sdv, val(e, "i32"));
    ir_instr_push_value(sdv, val(e, "i32"));
    check("signed div needs an overflow modifier as well, for INT_MIN / -1",
          apply(e, sdv) == IR_RULE_REJECT);

    ir_instr_t *fm = op(e, IR_OP_ADD);

    ir_instr_push_mod(fm, "wrap");
    ir_instr_push_value(fm, val(e, "f64"));
    ir_instr_push_value(fm, val(e, "f64"));
    check("float arithmetic takes no overflow modifier",
          apply(e, fm) == IR_RULE_REJECT);

    const char *fits[] = {"wrap", "trap", "sat", "undef"};
    const char *vecs[] = {"u8", "i8"};

    for (size_t v = 0; v < 2; v++) {
        for (size_t i = 0; i < 4; i++) {
            ir_instr_t *in = op(e, IR_OP_ADD);

            ir_instr_push_mod(in, fits[i]);
            ir_instr_push_value(in, val(e, vecs[v]));
            ir_instr_push_value(in, val(e, vecs[v]));

            if (!accepts_as(e, in, vecs[v])) {
                printf("FAIL  %s add takes the %s overflow modifier\n",
                       vecs[v], fits[i]);
                fails++;
            }
        }
    }

    close_env(e);
}

static void test_bitwise_and_shifts(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    const ir_op_t bits[] = {IR_OP_AND, IR_OP_OR, IR_OP_XOR};

    for (size_t i = 0; i < 3; i++) {
        ir_instr_t *in = op(e, bits[i]);

        ir_instr_push_value(in, val(e, "u32"));
        ir_instr_push_value(in, val(e, "u32"));

        if (!accepts_as(e, in, "u32")) {
            printf("FAIL  %s on integers\n", ir_op_spelling(bits[i]));
            fails++;
        }
    }

    ir_instr_t *nt = op(e, IR_OP_NOT);

    ir_instr_push_value(nt, val(e, "u32"));
    check("not takes one integer", accepts_as(e, nt, "u32"));

    ir_instr_t *bad = op(e, IR_OP_AND);

    ir_instr_push_value(bad, val(e, "f64"));
    ir_instr_push_value(bad, val(e, "f64"));
    check("bitwise on floats is rejected", apply(e, bad) == IR_RULE_REJECT);

    ir_instr_t *widths = op(e, IR_OP_OR);

    ir_instr_push_value(widths, val(e, "u32"));
    ir_instr_push_value(widths, val(e, "u64"));
    check("bitwise operands of different widths are rejected",
          apply(e, widths) == IR_RULE_REJECT);

    const ir_op_t shifts[] = {IR_OP_SHL, IR_OP_SHR, IR_OP_ROL, IR_OP_ROR};

    for (size_t i = 0; i < 4; i++) {
        ir_instr_t *in = op(e, shifts[i]);

        ir_instr_push_value(in, val(e, "u32"));
        ir_instr_push_value(in, val(e, "u8"));

        if (!accepts_as(e, in, "u32")) {
            printf("FAIL  %s keeps the shifted type\n",
                   ir_op_spelling(shifts[i]));
            fails++;
        }
    }

    ir_instr_t *sbad = op(e, IR_OP_SHL);

    ir_instr_push_value(sbad, val(e, "u32"));
    ir_instr_push_value(sbad, val(e, "f64"));
    check("a float shift amount is rejected",
          apply(e, sbad) == IR_RULE_REJECT);

    ir_instr_t *ss = op(e, IR_OP_SHR);

    ir_instr_push_value(ss, val(e, "i32"));
    ir_instr_push_value(ss, val(e, "u8"));
    check("one shr shifts a signed value arithmetically",
          accepts_as(e, ss, "i32"));

    ir_instr_t *su = op(e, IR_OP_SHR);

    ir_instr_push_value(su, val(e, "u32"));
    ir_instr_push_value(su, val(e, "u8"));
    check("and the same shr shifts an unsigned value logically",
          accepts_as(e, su, "u32"));

    ir_instr_t *msk = op(e, IR_OP_SHL);

    check("a shift takes the mask modifier",
          ir_instr_push_mod(msk, "mask"));
    ir_instr_push_value(msk, val(e, "u32"));
    ir_instr_push_value(msk, val(e, "u8"));
    check("and still yields the shifted type", accepts_as(e, msk, "u32"));

    ir_instr_t *nomask = op(e, IR_OP_SHL);

    ir_instr_push_value(nomask, val(e, "u32"));
    ir_instr_push_value(nomask, val(e, "u8"));
    check("a shift without mask is still well typed",
          accepts_as(e, nomask, "u32"));

    ir_instr_t *wrongmod = op(e, IR_OP_SHL);

    check("a shift does not take an overflow modifier",
          !ir_instr_push_mod(wrongmod, "sat"));

    const ir_op_t counts[] = {IR_OP_CLZ, IR_OP_CTZ, IR_OP_POPCOUNT};

    for (size_t i = 0; i < 3; i++) {
        ir_instr_t *in = op(e, counts[i]);

        ir_instr_push_value(in, val(e, "u64"));

        if (!accepts_as(e, in, "u32")) {
            printf("FAIL  %s yields u32\n", ir_op_spelling(counts[i]));
            fails++;
        }
    }

    ir_instr_t *rv = op(e, IR_OP_BITREVERSE);

    ir_instr_push_value(rv, val(e, "u16"));
    check("bitreverse keeps its type", accepts_as(e, rv, "u16"));

    ir_instr_t *bs = op(e, IR_OP_BYTESWAP);

    ir_instr_push_value(bs, val(e, "u16"));
    check("byteswap keeps its type", accepts_as(e, bs, "u16"));

    close_env(e);
}

static void test_comparisons(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    const ir_op_t cmps[] = {IR_OP_EQ, IR_OP_NE, IR_OP_LT,
                            IR_OP_LE, IR_OP_GT, IR_OP_GE};

    for (size_t i = 0; i < 6; i++) {
        ir_instr_t *in = op(e, cmps[i]);

        ir_instr_push_value(in, val(e, "i32"));
        ir_instr_push_value(in, val(e, "i32"));

        if (!accepts_as(e, in, "bool")) {
            printf("FAIL  %s yields bool\n", ir_op_spelling(cmps[i]));
            fails++;
        }
    }

    ir_instr_t *mixed = op(e, IR_OP_LT);

    ir_instr_push_value(mixed, val(e, "i32"));
    ir_instr_push_value(mixed, val(e, "u32"));
    check("comparing different types is rejected",
          apply(e, mixed) == IR_RULE_REJECT);

    ir_instr_t *fl = op(e, IR_OP_LT);

    ir_instr_push_value(fl, val(e, "f64"));
    ir_instr_push_value(fl, val(e, "f64"));
    check("a float comparison without ordering is rejected",
          apply(e, fl) == IR_RULE_REJECT);

    ir_instr_t *fl2 = op(e, IR_OP_LT);

    ir_instr_push_mod(fl2, "ordered");
    ir_instr_push_value(fl2, val(e, "f64"));
    ir_instr_push_value(fl2, val(e, "f64"));
    check("a float comparison with ordering is accepted",
          accepts_as(e, fl2, "bool"));

    ir_instr_t *fl3 = op(e, IR_OP_NE);

    ir_instr_push_mod(fl3, "unordered");
    ir_instr_push_value(fl3, val(e, "f32"));
    ir_instr_push_value(fl3, val(e, "f32"));
    check("unordered is the other choice", accepts_as(e, fl3, "bool"));

    ir_instr_t *iord = op(e, IR_OP_LT);

    ir_instr_push_mod(iord, "ordered");
    ir_instr_push_value(iord, val(e, "i32"));
    ir_instr_push_value(iord, val(e, "i32"));
    check("an integer comparison does not take an ordering modifier",
          apply(e, iord) == IR_RULE_REJECT);

    for (size_t i = 0; i < 6; i++) {
        ir_instr_t *in = op(e, cmps[i]);

        ir_instr_push_value(in, val(e, "u32"));
        ir_instr_push_value(in, val(e, "u32"));

        if (!accepts_as(e, in, "bool")) {
            printf("FAIL  unsigned %s yields bool\n", ir_op_spelling(cmps[i]));
            fails++;
        }
    }

    close_env(e);
}

static void test_memory(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *al = op(e, IR_OP_ALLOCA);

    ir_instr_push_type(al, ir_type_parse(e->m, "i32"));
    check("alloca yields a pointer to its type",
          accepts_as(e, al, "ptr[i32]"));

    ir_instr_t *ld = op(e, IR_OP_LOAD);

    ir_instr_push_value(ld, val(e, "ptr[i32]"));
    check("load yields the pointee", accepts_as(e, ld, "i32"));

    ir_instr_t *ldbad = op(e, IR_OP_LOAD);

    ir_instr_push_value(ldbad, val(e, "i32"));
    check("load through a non-pointer is rejected",
          apply(e, ldbad) == IR_RULE_REJECT);

    ir_instr_t *st = op(e, IR_OP_STORE);

    ir_instr_push_value(st, val(e, "ptr[i32]"));
    ir_instr_push_value(st, val(e, "i32"));
    check("store yields nothing", accepts_as(e, st, nullptr));

    ir_instr_t *stbad = op(e, IR_OP_STORE);

    ir_instr_push_value(stbad, val(e, "ptr[i32]"));
    ir_instr_push_value(stbad, val(e, "u64"));
    check("storing the wrong type is rejected",
          apply(e, stbad) == IR_RULE_REJECT);

    ir_instr_t *fd = op(e, IR_OP_FIELD);

    ir_instr_push_value(fd, val(e, "ptr[(x: i32, y: u64)]"));
    ir_instr_push_name(fd, "y");
    check("field resolves the member through typehash",
          accepts_as(e, fd, "ptr[u64]"));

    ir_instr_t *fdbad = op(e, IR_OP_FIELD);

    ir_instr_push_value(fdbad, val(e, "ptr[(x: i32)]"));
    ir_instr_push_name(fdbad, "nope");
    check("an unknown member is rejected",
          apply(e, fdbad) == IR_RULE_REJECT);

    ir_instr_t *ix = op(e, IR_OP_INDEX);

    ir_instr_push_value(ix, val(e, "ptr[array[i32; 4]]"));
    ir_instr_push_value(ix, val(e, "u32"));
    check("index yields a pointer to the element",
          accepts_as(e, ix, "ptr[i32]"));

    ir_instr_t *ixbad = op(e, IR_OP_INDEX);

    ir_instr_push_value(ixbad, val(e, "ptr[array[i32; 4]]"));
    ir_instr_push_value(ixbad, val(e, "f64"));
    check("a float index is rejected", apply(e, ixbad) == IR_RULE_REJECT);

    ir_instr_t *of = op(e, IR_OP_OFFSET);

    ir_instr_push_value(of, val(e, "ptr[i32]"));
    ir_instr_push_value(of, val(e, "u32"));
    check("offset keeps the pointer type", accepts_as(e, of, "ptr[i32]"));

    ir_instr_t *ofbad = op(e, IR_OP_OFFSET);

    ir_instr_push_value(ofbad, val(e, "ref[i32]"));
    ir_instr_push_value(ofbad, val(e, "u32"));
    check("offset on a non-indexable reference is rejected",
          apply(e, ofbad) == IR_RULE_REJECT);

    ir_instr_t *df = op(e, IR_OP_DIFF);

    ir_instr_push_value(df, val(e, "ptr[i32]"));
    ir_instr_push_value(df, val(e, "ptr[i32]"));
    check("diff yields a signed integer", accepts_as(e, df, "i64"));

    ir_instr_t *cp = op(e, IR_OP_MEMCPY);

    ir_instr_push_value(cp, val(e, "ptr[i32]"));
    ir_instr_push_value(cp, val(e, "ptr[i32]"));
    ir_instr_push_value(cp, val(e, "u64"));
    check("memcpy yields nothing", accepts_as(e, cp, nullptr));

    ir_instr_t *mv = op(e, IR_OP_MEMMOVE);

    ir_instr_push_value(mv, val(e, "ptr[i32]"));
    ir_instr_push_value(mv, val(e, "ptr[i32]"));
    ir_instr_push_value(mv, val(e, "u64"));
    check("memmove yields nothing", accepts_as(e, mv, nullptr));

    ir_instr_t *ms = op(e, IR_OP_MEMSET);

    ir_instr_push_value(ms, val(e, "ptr[i32]"));
    ir_instr_push_value(ms, val(e, "u8"));
    ir_instr_push_value(ms, val(e, "u64"));
    check("memset yields nothing", accepts_as(e, ms, nullptr));

    ir_instr_t *mc = op(e, IR_OP_MEMCMP);

    ir_instr_push_value(mc, val(e, "ptr[i32]"));
    ir_instr_push_value(mc, val(e, "ptr[i32]"));
    ir_instr_push_value(mc, val(e, "u64"));
    check("memcmp yields a 32-bit signed integer",
          accepts_as(e, mc, "i32"));

    ir_instr_t *msbad = op(e, IR_OP_MEMSET);

    ir_instr_push_value(msbad, val(e, "ptr[i32]"));
    ir_instr_push_value(msbad, val(e, "u32"));
    ir_instr_push_value(msbad, val(e, "u64"));
    check("memset fills with a byte, not a wider integer",
          apply(e, msbad) == IR_RULE_REJECT);

    ir_instr_t *cpbad = op(e, IR_OP_MEMCPY);

    ir_instr_push_value(cpbad, val(e, "ptr[i32]"));
    ir_instr_push_value(cpbad, val(e, "u64"));
    ir_instr_push_value(cpbad, val(e, "u64"));
    check("memcpy needs a pointer source",
          apply(e, cpbad) == IR_RULE_REJECT);

    ir_instr_t *lv = op(e, IR_OP_LOAD);

    check("load takes the volatile access modifier",
          ir_instr_push_mod(lv, "volatile"));
    ir_instr_push_value(lv, val(e, "ptr[i32]"));
    check("and an access modifier does not change the loaded type",
          accepts_as(e, lv, "i32"));

    ir_instr_t *sv = op(e, IR_OP_STORE);

    check("store takes the atomic access modifier",
          ir_instr_push_mod(sv, "atomic"));
    ir_instr_push_value(sv, val(e, "ptr[i32]"));
    ir_instr_push_value(sv, val(e, "i32"));
    check("and an access modifier leaves store with no result",
          accepts_as(e, sv, nullptr));

    ir_instr_t *av = op(e, IR_OP_ADD);

    check("an access modifier is not accepted where there is no access",
          !ir_instr_push_mod(av, "volatile"));

    ir_instr_t *ac = op(e, IR_OP_ALLOCA);

    ir_instr_push_type(ac, ir_type_parse(e->m, "i32"));
    ir_instr_push_value(ac, val(e, "u64"));
    check("alloca takes an optional count", accepts_as(e, ac, "ptr[i32]"));

    ir_instr_t *acbad = op(e, IR_OP_ALLOCA);

    ir_instr_push_type(acbad, ir_type_parse(e->m, "i32"));
    ir_instr_push_value(acbad, val(e, "f64"));
    check("an alloca count must be an integer",
          apply(e, acbad) == IR_RULE_REJECT);

    ir_type_define(e->m, ir_type_parse(e->m, "Point"),
                   ir_type_parse(e->m, "(x: i32, y: u64)"));

    ir_instr_t *nf = op(e, IR_OP_FIELD);

    ir_instr_push_value(nf, val(e, "ptr[Point]"));
    ir_instr_push_name(nf, "y");
    check("field resolves a named type through the module's type table",
          accepts_as(e, nf, "ptr[u64]"));

    ir_instr_t *nfbad = op(e, IR_OP_FIELD);

    ir_instr_push_value(nfbad, val(e, "ptr[Widget]"));
    ir_instr_push_name(nfbad, "y");
    check("a named type with no definition has no members",
          apply(e, nfbad) == IR_RULE_REJECT);

    ir_instr_t *ixseq = op(e, IR_OP_INDEX);

    ir_instr_push_value(ixseq, val(e, "ref[array[i32; 4]]"));
    ir_instr_push_value(ixseq, val(e, "u32"));
    check("a sequence is indexable however it is referenced",
          accepts_as(e, ixseq, "ptr[i32]"));

    ir_instr_t *ixref = op(e, IR_OP_INDEX);

    ir_instr_push_value(ixref, val(e, "ref[i32]"));
    ir_instr_push_value(ixref, val(e, "u32"));
    check("a non-sequence needs an indexable indirection",
          apply(e, ixref) == IR_RULE_REJECT);

    close_env(e);
}

static void test_aggregates_and_variants(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *ex = op(e, IR_OP_EXTRACT);

    ir_instr_push_value(ex, val(e, "(x: i32, y: u64)"));
    ir_instr_push_name(ex, "x");
    check("extract yields the member type", accepts_as(e, ex, "i32"));

    ir_instr_t *ins = op(e, IR_OP_INSERT);

    ir_instr_push_value(ins, val(e, "(x: i32)"));
    ir_instr_push_name(ins, "x");
    ir_instr_push_value(ins, val(e, "i32"));
    check("insert yields the aggregate", accepts_as(e, ins, "(x: i32)"));

    ir_instr_t *insbad = op(e, IR_OP_INSERT);

    ir_instr_push_value(insbad, val(e, "(x: i32)"));
    ir_instr_push_name(insbad, "x");
    ir_instr_push_value(insbad, val(e, "u64"));
    check("inserting the wrong member type is rejected",
          apply(e, insbad) == IR_RULE_REJECT);

    ir_instr_t *vm = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vm, "r", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_push_name(vm, "ok");
    ir_instr_push_value(vm, val(e, "i32"));
    check("variant.make takes its type from its destination",
          accepts_as(e, vm, "variant R[i32]"));

    ir_instr_t *vmbad = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vmbad, "s", ir_type_parse(e->m, "R[i32]"));
    ir_instr_push_name(vmbad, "ok");
    ir_instr_push_value(vmbad, val(e, "i32"));
    check("a non-variant destination is rejected",
          apply(e, vmbad) == IR_RULE_REJECT);

    ir_instr_t *vt = op(e, IR_OP_VARIANT_TAG);

    ir_instr_push_value(vt, val(e, "variant R[i32]"));
    check("variant.tag yields a 64-bit unsigned integer",
          accepts_as(e, vt, "u64"));

    ir_instr_t *vtbad = op(e, IR_OP_VARIANT_TAG);

    ir_instr_push_value(vtbad, val(e, "i32"));
    check("variant.tag on a non-variant is rejected",
          apply(e, vtbad) == IR_RULE_REJECT);

    ir_instr_t *vp = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(vp, "p", ir_type_parse(e->m, "i32"));
    ir_instr_push_value(vp, val(e, "variant R[i32]"));
    ir_instr_push_name(vp, "ok");
    check("variant.payload takes its arm type from its destination",
          accepts_as(e, vp, "i32"));

    ir_type_define(e->m, ir_type_parse(e->m, "variant R[i32]"),
                   ir_type_parse(e->m, "(ok: i32, err: u64)"));

    ir_instr_t *vd = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vd, "rd", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_push_name(vd, "ok");
    ir_instr_push_value(vd, val(e, "i32"));
    check("a declared arm and a matching payload are accepted",
          accepts_as(e, vd, "variant R[i32]"));

    ir_instr_t *vtypo = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vtypo, "rt", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_push_name(vtypo, "okk");
    ir_instr_push_value(vtypo, val(e, "i32"));
    check("a misspelled arm name is caught once the variant is declared",
          apply(e, vtypo) == IR_RULE_REJECT);

    ir_instr_t *vpay = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vpay, "rp", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_push_name(vpay, "err");
    ir_instr_push_value(vpay, val(e, "i32"));
    check("and so is a payload that is not what the arm carries",
          apply(e, vpay) == IR_RULE_REJECT);

    ir_instr_t *pd = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(pd, "pd", ir_type_parse(e->m, "u64"));
    ir_instr_push_value(pd, val(e, "variant R[i32]"));
    ir_instr_push_name(pd, "err");
    check("variant.payload agrees with the declared arm",
          accepts_as(e, pd, "u64"));

    ir_instr_t *pbad = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(pbad, "pb", ir_type_parse(e->m, "i32"));
    ir_instr_push_value(pbad, val(e, "variant R[i32]"));
    ir_instr_push_name(pbad, "err");
    check("and is rejected when it does not",
          apply(e, pbad) == IR_RULE_REJECT);

    ir_instr_t *ptypo = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(ptypo, "pt", ir_type_parse(e->m, "i32"));
    ir_instr_push_value(ptypo, val(e, "variant R[i32]"));
    ir_instr_push_name(ptypo, "nope");
    check("an undeclared arm is rejected on the reading side too",
          apply(e, ptypo) == IR_RULE_REJECT);

    ir_annot_t *tag = ir_annot_get(&vm->annots, "ir.const");

    check("variant.make records the arm tag",
          tag && tag->kind == IR_ANNOT_INT
              && tag->i.mag == th_arm_tag("ok"));

    check("insert produces a new value rather than mutating one",
          ir_op_has_result(IR_OP_INSERT)
              && ir_op_operand_kind(IR_OP_INSERT, 0) == IR_OPD_VALUE);

    ir_instr_t *agn = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(agn, "an", ir_type_parse(e->m, "(x: i32, y: u64)"));
    ir_instr_push_value(agn, val(e, "i32"));
    check("an aggregate needs one value per member",
          apply(e, agn) == IR_RULE_REJECT);

    ir_instr_t *ago = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(ago, "ao", ir_type_parse(e->m, "(x: i32, y: u64)"));
    ir_instr_push_value(ago, val(e, "u64"));
    ir_instr_push_value(ago, val(e, "i32"));
    check("aggregate members are matched in typehash order",
          apply(e, ago) == IR_RULE_REJECT);

    ir_instr_t *agg = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(agg, "ag2", ir_type_parse(e->m, "(x: i32, y: u64)"));
    ir_instr_push_value(agg, val(e, "i32"));
    ir_instr_push_value(agg, val(e, "u64"));
    check("and in that order they match",
          accepts_as(e, agg, "(x: i32, y: u64)"));

    ir_type_define(e->m, ir_type_parse(e->m, "Pair"),
                   ir_type_parse(e->m, "(a: i32, b: u64)"));

    ir_instr_t *nx = op(e, IR_OP_EXTRACT);

    ir_instr_push_value(nx, val(e, "Pair"));
    ir_instr_push_name(nx, "b");
    check("extract resolves a named record through the type table",
          accepts_as(e, nx, "u64"));

    close_env(e);
}

typedef struct {
    const char *from;
    const char *to;
    bool        fit;
    bool        round;
    const char *what;
} conv_t;

static const conv_t CONVERSIONS[] = {
    {"i32", "i64", false, false, "integer to wider integer"},
    {"i32", "i8", true, false, "integer to narrower integer"},
    {"f64", "i32", true, true, "float to integer"},
    {"i32", "f64", false, true, "integer to float"},
    {"f32", "f64", false, false, "float to wider float"},
    {"f64", "f32", false, true, "float to narrower float"},
    {"ptr[i32]", "u64", false, false, "pointer to integer"},
    {"bool", "i32", false, false, "bool to integer"},
};

static ir_instr_t *conv(env *e, const conv_t *c, const char *m1,
                        const char *m2)
{
    char name[32];

    snprintf(name, sizeof(name), "c%u", e->n++);

    ir_instr_t *in = op(e, IR_OP_CAST);

    ir_instr_set_dest(in, ir_strdup(e->ctx, name), ir_type_parse(e->m, c->to));

    if (m1) {
        ir_instr_push_mod(in, m1);
    }

    if (m2) {
        ir_instr_push_mod(in, m2);
    }
    ir_instr_push_value(in, val(e, c->from));

    return in;
}

static void test_conversion(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    for (size_t i = 0; i < sizeof(CONVERSIONS) / sizeof(CONVERSIONS[0]); i++) {
        const conv_t *c   = &CONVERSIONS[i];
        const char   *fit = c->fit ? "sat" : nullptr;
        const char   *rnd = c->round ? "nearest" : nullptr;

        if (!accepts_as(e, conv(e, c, fit, rnd), c->to)) {
            printf("FAIL  %s takes exactly the modifiers the table gives it\n",
                   c->what);
            fails++;
        }

        if (c->fit && apply(e, conv(e, c, nullptr, rnd)) != IR_RULE_REJECT) {
            printf("FAIL  %s without its fit modifier is rejected\n", c->what);
            fails++;
        }

        if (c->round && apply(e, conv(e, c, fit, nullptr)) != IR_RULE_REJECT) {
            printf("FAIL  %s without its rounding modifier is rejected\n",
                   c->what);
            fails++;
        }

        if (!c->fit && apply(e, conv(e, c, "sat", rnd)) != IR_RULE_REJECT) {
            printf("FAIL  %s takes no fit modifier\n", c->what);
            fails++;
        }

        if (!c->round && apply(e, conv(e, c, fit, "nearest"))
                             != IR_RULE_REJECT) {
            printf("FAIL  %s takes no rounding modifier\n", c->what);
            fails++;
        }
    }

    const conv_t back[] = {
        {"u64", "ptr[i32]", false, false, "integer to pointer"},
        {"i32", "bool", false, false, "integer to bool"},
    };

    for (size_t i = 0; i < 2; i++) {
        if (!accepts_as(e, conv(e, &back[i], nullptr, nullptr), back[i].to)) {
            printf("FAIL  %s converts too\n", back[i].what);
            fails++;
        }
    }

    ir_instr_t *widen_u = op(e, IR_OP_CAST);

    ir_instr_set_dest(widen_u, "wu", ir_type_parse(e->m, "u64"));
    ir_instr_push_value(widen_u, val(e, "u32"));
    check("zero extension is not spelled; the source type says so",
          accepts_as(e, widen_u, "u64"));

    ir_instr_t *widen_s = op(e, IR_OP_CAST);

    ir_instr_set_dest(widen_s, "ws", ir_type_parse(e->m, "i64"));
    ir_instr_push_value(widen_s, val(e, "i32"));
    check("nor is sign extension", accepts_as(e, widen_s, "i64"));

    const conv_t bad = {"(x: i32)", "i32", false, false, "record to integer"};

    check("cast does not convert an aggregate to a scalar",
          apply(e, conv(e, &bad, nullptr, nullptr)) == IR_RULE_REJECT);

    const conv_t badp = {"ptr[i32]", "ptr[u8]", false, false, "pointer swap"};

    check("cast does not reinterpret one pointer as another",
          apply(e, conv(e, &badp, nullptr, nullptr)) == IR_RULE_REJECT);

    ir_instr_t *bc = op(e, IR_OP_BITCAST);

    ir_instr_set_dest(bc, "b", ir_type_parse(e->m, "u32"));
    ir_instr_push_value(bc, val(e, "f32"));
    check("bitcast reinterprets instead", accepts_as(e, bc, "u32"));

    ir_instr_t *bcm = op(e, IR_OP_BITCAST);

    check("and bitcast takes no modifiers",
          !ir_instr_push_mod(bcm, "sat"));

    close_env(e);
}

static void test_select_phi_call(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *s = op(e, IR_OP_SELECT);

    ir_instr_push_value(s, val(e, "bool"));
    ir_instr_push_value(s, val(e, "i32"));
    ir_instr_push_value(s, val(e, "i32"));
    check("select yields its arm type", accepts_as(e, s, "i32"));

    ir_instr_t *sbad = op(e, IR_OP_SELECT);

    ir_instr_push_value(sbad, val(e, "i32"));
    ir_instr_push_value(sbad, val(e, "i32"));
    ir_instr_push_value(sbad, val(e, "i32"));
    check("select on a non-boolean is rejected",
          apply(e, sbad) == IR_RULE_REJECT);

    ir_instr_t *smix = op(e, IR_OP_SELECT);

    ir_instr_push_value(smix, val(e, "bool"));
    ir_instr_push_value(smix, val(e, "i32"));
    ir_instr_push_value(smix, val(e, "u32"));
    check("select with mismatched arms is rejected",
          apply(e, smix) == IR_RULE_REJECT);

    ir_block_t *other = ir_block_add(e->f, "other");
    ir_instr_t *p     = op(e, IR_OP_PHI);

    ir_instr_set_dest(p, "ph", ir_type_parse(e->m, "i32"));
    ir_instr_push_phi_arm(p, other, val(e, "i32"));
    check("phi takes its type from its destination",
          accepts_as(e, p, "i32"));

    ir_instr_t *pbad = op(e, IR_OP_PHI);

    ir_instr_set_dest(pbad, "ph2", ir_type_parse(e->m, "i32"));
    ir_instr_push_phi_arm(pbad, other, val(e, "u64"));
    check("a phi arm of the wrong type is rejected",
          apply(e, pbad) == IR_RULE_REJECT);

    ir_func_t *g = ir_func_add(e->m, "g",
                               ir_type_parse(e->m, "(i32, x: u64) -> i64"),
                               IR_LINK_EXTERNAL);

    (void)g;

    ir_value_t *gv = ir_alloc(e->ctx, sizeof(ir_value_t));

    gv->kind = IR_VAL_FUNC;
    gv->name = "g";
    gv->type = ir_type_parse(e->m, "(i32, x: u64) -> i64");

    ir_instr_t *c = op(e, IR_OP_CALL);

    ir_instr_push_value(c, gv);
    ir_instr_push_value(c, val(e, "i32"));
    ir_instr_push_named(c, "x", val(e, "u64"));
    check("call yields the callee's return type", accepts_as(e, c, "i64"));

    ir_instr_t *few = op(e, IR_OP_CALL);

    ir_instr_push_value(few, gv);
    check("too few arguments is rejected", apply(e, few) == IR_RULE_REJECT);

    ir_instr_t *wrongkw = op(e, IR_OP_CALL);

    ir_instr_push_value(wrongkw, gv);
    ir_instr_push_value(wrongkw, val(e, "i32"));
    ir_instr_push_named(wrongkw, "nope", val(e, "u64"));
    check("an unknown keyword parameter is rejected",
          apply(e, wrongkw) == IR_RULE_REJECT);

    ir_instr_t *wrongty = op(e, IR_OP_CALL);

    ir_instr_push_value(wrongty, gv);
    ir_instr_push_value(wrongty, val(e, "u64"));
    ir_instr_push_named(wrongty, "x", val(e, "u64"));
    check("an argument of the wrong type is rejected",
          apply(e, wrongty) == IR_RULE_REJECT);

    ir_instr_t *ind = op(e, IR_OP_CALL);

    ir_instr_push_value(ind, val(e, "ptr[(i32, x: u64) -> i64]"));
    ir_instr_push_value(ind, val(e, "i32"));
    ir_instr_push_named(ind, "x", val(e, "u64"));
    check("the same opcode calls through a pointer to a function",
          accepts_as(e, ind, "i64"));

    ir_instr_t *notfn = op(e, IR_OP_CALL);

    ir_instr_push_value(notfn, val(e, "i32"));
    check("calling a non-function is rejected",
          apply(e, notfn) == IR_RULE_REJECT);

    ir_value_t *rv = ir_alloc(e->ctx, sizeof(ir_value_t));

    rv->kind = IR_VAL_FUNC;
    rv->name = "r";
    rv->type = ir_type_parse(e->m, "(i32, *u64) -> i64");

    ir_instr_t *rest = op(e, IR_OP_CALL);

    ir_instr_push_value(rest, rv);
    ir_instr_push_value(rest, val(e, "i32"));
    ir_instr_push_value(rest, val(e, "u64"));
    ir_instr_push_value(rest, val(e, "u64"));
    check("a rest parameter accepts any number of its element type",
          accepts_as(e, rest, "i64"));

    ir_instr_t *restbad = op(e, IR_OP_CALL);

    ir_instr_push_value(restbad, rv);
    ir_instr_push_value(restbad, val(e, "i32"));
    ir_instr_push_value(restbad, val(e, "u64"));
    ir_instr_push_value(restbad, val(e, "i8"));
    check("but a rest argument of the wrong element type is rejected",
          apply(e, restbad) == IR_RULE_REJECT);

    ir_value_t *ov = ir_alloc(e->ctx, sizeof(ir_value_t));

    ov->kind = IR_VAL_FUNC;
    ov->name = "o";
    ov->type = ir_type_parse(e->m, "(out i32, inout u64) -> void");

    ir_instr_t *outc = op(e, IR_OP_CALL);

    ir_instr_push_value(outc, ov);
    ir_instr_push_value(outc, val(e, "ptr[i32]"));
    ir_instr_push_value(outc, val(e, "ptr[u64]"));
    check("out and inout arguments are passed by reference",
          accepts_as(e, outc, "void"));

    ir_instr_t *outbad = op(e, IR_OP_CALL);

    ir_instr_push_value(outbad, ov);
    ir_instr_push_value(outbad, val(e, "i32"));
    ir_instr_push_value(outbad, val(e, "ptr[u64]"));
    check("passing an out parameter by value is rejected",
          apply(e, outbad) == IR_RULE_REJECT);

    close_env(e);
}

static void test_comptime_and_terminators(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    const ir_op_t sizes[] = {IR_OP_SIZEOF, IR_OP_ALIGNOF, IR_OP_TYPEHASH};

    for (size_t i = 0; i < 3; i++) {
        ir_instr_t *in = op(e, sizes[i]);

        ir_instr_push_type(in, ir_type_parse(e->m, "i32"));

        if (!accepts_as(e, in, "u64")) {
            printf("FAIL  %s yields u64\n", ir_op_spelling(sizes[i]));
            fails++;
        }
    }

    ir_instr_t *ofs = op(e, IR_OP_OFFSETOF);

    ir_instr_push_type(ofs, ir_type_parse(e->m, "(x: i32)"));
    ir_instr_push_name(ofs, "x");
    check("offsetof yields u64", accepts_as(e, ofs, "u64"));

    ir_instr_t *at = op(e, IR_OP_ARMTAG);

    ir_instr_push_name(at, "ok");
    check("armtag yields u64", accepts_as(e, at, "u64"));

    ir_instr_t *tn = op(e, IR_OP_TYPENAME);

    ir_instr_push_type(tn, ir_type_parse(e->m, "i32"));
    check("typename yields a character pointer",
          accepts_as(e, tn, "ptr[char]"));

    ir_annot_t *nm = ir_annot_get(&tn->annots, "ir.const");

    check("and records the canonical spelling",
          nm && nm->kind == IR_ANNOT_STR && !strcmp(nm->s, "i32"));

    ir_annot_t *tg = ir_annot_get(&at->annots, "ir.const");

    check("armtag records th_arm_tag of the name",
          tg && tg->kind == IR_ANNOT_INT
              && tg->i.mag == th_arm_tag("ok"));

    ir_instr_t *th = op(e, IR_OP_TYPEHASH);

    ir_instr_push_type(th, ir_type_parse(e->m, "ptr[i32]"));
    check("typehash yields u64", accepts_as(e, th, "u64"));

    ir_annot_t *hv = ir_annot_get(&th->annots, "ir.const");

    check("a hash is recorded to be read as a bit pattern",
          hv && hv->i.hex);

    check("and records the hash itself",
          hv && hv->kind == IR_ANNOT_INT
              && hv->i.mag
                     == th_hash(ir_ctx_types(e->ctx),
                                ir_type_parse(e->m, "ptr[i32]")));

    ir_instr_t *obad = op(e, IR_OP_OFFSETOF);

    ir_instr_push_type(obad, ir_type_parse(e->m, "(x: i32)"));
    ir_instr_push_name(obad, "nope");
    check("offsetof of an unknown member is rejected",
          apply(e, obad) == IR_RULE_REJECT);

    ir_instr_t *j = op(e, IR_OP_JUMP);

    ir_instr_push_block(j, e->b);
    check("jump yields nothing", accepts_as(e, j, nullptr));

    ir_instr_t *u = op(e, IR_OP_UNREACHABLE);

    check("unreachable yields nothing", accepts_as(e, u, nullptr));

    ir_instr_t *br = op(e, IR_OP_BRANCH);

    ir_instr_push_value(br, val(e, "bool"));
    ir_instr_push_block(br, e->b);
    ir_instr_push_block(br, e->b);
    check("branch on a boolean is accepted", accepts_as(e, br, nullptr));

    ir_instr_t *brbad = op(e, IR_OP_BRANCH);

    ir_instr_push_value(brbad, val(e, "i32"));
    ir_instr_push_block(brbad, e->b);
    ir_instr_push_block(brbad, e->b);
    check("branch on a non-boolean is rejected",
          apply(e, brbad) == IR_RULE_REJECT);

    ir_instr_t *sw = op(e, IR_OP_SWITCH);

    ir_instr_push_value(sw, val(e, "i32"));
    ir_instr_push_default(sw, e->b);
    ir_instr_push_switch_arm(sw, 1, e->b);
    ir_instr_push_switch_arm(sw, 2, e->b);
    check("a switch with distinct cases is accepted",
          accepts_as(e, sw, nullptr));

    ir_instr_t *dup = op(e, IR_OP_SWITCH);

    ir_instr_push_value(dup, val(e, "i32"));
    ir_instr_push_default(dup, e->b);
    ir_instr_push_switch_arm(dup, 1, e->b);
    ir_instr_push_switch_arm(dup, 1, e->b);
    check("duplicate case values are rejected",
          apply(e, dup) == IR_RULE_REJECT);

    ir_instr_t *swb = op(e, IR_OP_SWITCH);

    ir_instr_push_value(swb, val(e, "bool"));
    ir_instr_push_default(swb, e->b);
    ir_instr_push_switch_arm(swb, 0, e->b);
    ir_instr_push_switch_arm(swb, 1, e->b);
    check("a boolean scrutinee is allowed too",
          accepts_as(e, swb, nullptr));

    ir_instr_t *swf = op(e, IR_OP_SWITCH);

    ir_instr_push_value(swf, val(e, "f64"));
    ir_instr_push_default(swf, e->b);
    check("switching on a float is rejected",
          apply(e, swf) == IR_RULE_REJECT);

    ir_instr_t *r = op(e, IR_OP_RETURN);

    ir_instr_push_value(r, val(e, "i32"));
    check("returning the declared type is accepted",
          accepts_as(e, r, nullptr));

    ir_instr_t *rbad = op(e, IR_OP_RETURN);

    ir_instr_push_value(rbad, val(e, "u64"));
    check("returning the wrong type is rejected",
          apply(e, rbad) == IR_RULE_REJECT);

    ir_instr_t *rnone = op(e, IR_OP_RETURN);

    check("returning nothing from a non-void function is rejected",
          apply(e, rnone) == IR_RULE_REJECT);

    env *v = open_env(&ir_profile_bce, "void");

    ir_instr_t *rv = op(v, IR_OP_RETURN);

    check("a void function may return nothing",
          accepts_as(v, rv, nullptr));

    ir_instr_t *rvbad = op(v, IR_OP_RETURN);

    ir_instr_push_value(rvbad, val(v, "i32"));
    check("a void function may not return a value",
          apply(v, rvbad) == IR_RULE_REJECT);

    close_env(v);
    close_env(e);
}

static void test_constants_and_copy(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *c = op(e, IR_OP_CONST);

    ir_instr_set_dest(c, "k", ir_type_parse(e->m, "i32"));
    ir_instr_push_int(c, 7);
    check("const takes its type from its destination",
          accepts_as(e, c, "i32"));

    ir_instr_t *cbad = op(e, IR_OP_CONST);

    ir_instr_set_dest(cbad, "k2", ir_type_parse(e->m, "Widget"));
    ir_instr_push_int(cbad, 7);
    check("a constant of an opaque type is rejected",
          apply(e, cbad) == IR_RULE_REJECT);

    ir_instr_t *wide = op(e, IR_OP_CONST);

    ir_instr_set_dest(wide, "k3", ir_type_parse(e->m, "i8"));
    ir_instr_push_int(wide, 300);
    check("a literal too large for its type is rejected",
          apply(e, wide) == IR_RULE_REJECT);

    ir_instr_t *neg = op(e, IR_OP_CONST);

    ir_instr_set_dest(neg, "k4", ir_type_parse(e->m, "u8"));
    ir_instr_push_int(neg, -1);
    check("a negative literal in an unsigned type is rejected",
          apply(e, neg) == IR_RULE_REJECT);

    ir_instr_t *edge = op(e, IR_OP_CONST);

    ir_instr_set_dest(edge, "k5", ir_type_parse(e->m, "i8"));
    ir_instr_push_int(edge, -128);
    check("the bottom of the range is representable",
          accepts_as(e, edge, "i8"));

    ir_instr_t *fl = op(e, IR_OP_CONST);

    ir_instr_set_dest(fl, "k6", ir_type_parse(e->m, "i32"));
    ir_instr_push_float(fl, 1.5);
    check("a float literal in an integer type is rejected",
          apply(e, fl) == IR_RULE_REJECT);

    ir_instr_t *fl2 = op(e, IR_OP_CONST);

    ir_instr_set_dest(fl2, "k7", ir_type_parse(e->m, "f64"));
    ir_instr_push_float(fl2, 1.5);
    check("and is representable in a float type",
          accepts_as(e, fl2, "f64"));

    ir_int_t u64max = ((ir_int_t)1 << 64) - 1;

    ir_instr_t *pu = op(e, IR_OP_CONST);

    ir_instr_set_dest(pu, "pu", ir_type_parse(e->m, "u128"));
    ir_instr_push_uint(pu, ~(ir_uint_t)0);
    check("u128 is a type the profile declares, to its top value",
          accepts_as(e, pu, "u128"));

    ir_instr_t *pi = op(e, IR_OP_CONST);

    ir_instr_set_dest(pi, "pi", ir_type_parse(e->m, "i128"));
    ir_instr_push_lit(pi, (ir_lit_t){.mag = (ir_uint_t)1 << 127, .neg = true});
    check("and so is i128, to its bottom value", accepts_as(e, pi, "i128"));

    ir_instr_t *pio = op(e, IR_OP_CONST);

    ir_instr_set_dest(pio, "pio", ir_type_parse(e->m, "i128"));
    ir_instr_push_uint(pio, (ir_uint_t)1 << 127);
    check("i128 stops where it should",
          apply(e, pio) == IR_RULE_REJECT);

    ir_instr_t *add128 = op(e, IR_OP_ADD);

    ir_instr_push_mod(add128, "wrap");
    ir_instr_push_value(add128, val(e, "u128"));
    ir_instr_push_value(add128, val(e, "u128"));
    check("128-bit arithmetic types like any other width",
          accepts_as(e, add128, "u128"));

    ir_instr_t *u128top = op(e, IR_OP_CONST);

    ir_instr_set_dest(u128top, "kt", ir_type_parse(e->m, "ubitint[128]"));
    ir_instr_push_uint(u128top, ~(ir_uint_t)0);
    check("the top of u128 is expressible, which a signed carrier could not do",
          accepts_as(e, u128top, "ubitint[128]"));

    ir_instr_t *u128neg = op(e, IR_OP_CONST);

    ir_instr_set_dest(u128neg, "kn", ir_type_parse(e->m, "ubitint[128]"));
    ir_instr_push_int(u128neg, -1);
    check("and a negative literal there is still rejected",
          apply(e, u128neg) == IR_RULE_REJECT);

    ir_instr_t *i128min = op(e, IR_OP_CONST);

    ir_instr_set_dest(i128min, "ki", ir_type_parse(e->m, "bitint[128]"));
    ir_instr_push_lit(i128min,
                      (ir_lit_t){.mag = (ir_uint_t)1 << 127, .neg = true});
    check("so is the bottom of i128", accepts_as(e, i128min, "bitint[128]"));

    ir_instr_t *i128over = op(e, IR_OP_CONST);

    ir_instr_set_dest(i128over, "kv", ir_type_parse(e->m, "bitint[128]"));
    ir_instr_push_lit(i128over,
                      (ir_lit_t){.mag = ((ir_uint_t)1 << 127) + 1,
                                 .neg = true});
    check("and one below it is not",
          apply(e, i128over) == IR_RULE_REJECT);

    ir_instr_t *top = op(e, IR_OP_CONST);

    ir_instr_set_dest(top, "ku", ir_type_parse(e->m, "u64"));
    ir_instr_push_int(top, u64max);
    check("the whole range of u64 is expressible",
          accepts_as(e, top, "u64"));

    ir_instr_t *over = op(e, IR_OP_CONST);

    ir_instr_set_dest(over, "ko", ir_type_parse(e->m, "u64"));
    ir_instr_push_int(over, u64max + 1);
    check("and one past the top of it is rejected",
          apply(e, over) == IR_RULE_REJECT);

    ir_instr_t *w128 = op(e, IR_OP_CONST);

    ir_instr_set_dest(w128, "kw", ir_type_parse(e->m, "bitint[100]"));
    ir_instr_push_int(w128, ((ir_int_t)1 << 99) - 1);
    check("so is the range of any type narrower than 128 bits",
          accepts_as(e, w128, "bitint[100]"));

    ir_instr_t *big = op(e, IR_OP_CONST);

    ir_instr_set_dest(big, "k9", ir_type_parse(e->m, "f32"));
    ir_instr_push_float(big, 1e300);
    check("a float literal outside the narrower float is rejected",
          apply(e, big) == IR_RULE_REJECT);

    ir_instr_t *ok32 = op(e, IR_OP_CONST);

    ir_instr_set_dest(ok32, "k10", ir_type_parse(e->m, "f32"));
    ir_instr_push_float(ok32, 0.5);
    check("and one it can hold exactly is not",
          accepts_as(e, ok32, "f32"));

    ir_instr_t *rounds = op(e, IR_OP_CONST);

    ir_instr_set_dest(rounds, "k11", ir_type_parse(e->m, "f32"));
    ir_instr_push_float(rounds, 0.1);
    check("a float literal is the nearest value, so rounding is not an error",
          accepts_as(e, rounds, "f32"));

    ir_instr_t *tw = op(e, IR_OP_CONST);

    ir_instr_set_dest(tw, "k8", ir_type_parse(e->m, "bool"));
    ir_instr_push_int(tw, 2);
    check("only zero and one are representable in bool",
          apply(e, tw) == IR_RULE_REJECT);

    ir_instr_t *u = op(e, IR_OP_UNDEF);

    ir_instr_set_dest(u, "un", ir_type_parse(e->m, "i32"));
    check("undef takes its type from its destination",
          accepts_as(e, u, "i32"));

    ir_instr_t *ag = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(ag, "ag", ir_type_parse(e->m, "(x: i32)"));
    ir_instr_push_value(ag, val(e, "i32"));
    check("aggregate takes its type from its destination",
          accepts_as(e, ag, "(x: i32)"));

    ir_instr_t *agbad = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(agbad, "agb", ir_type_parse(e->m, "i32"));
    ir_instr_push_value(agbad, val(e, "i32"));
    check("an aggregate destination that is not a record is rejected",
          apply(e, agbad) == IR_RULE_REJECT);

    ir_local_t *kl = ir_local_add(e->f, "kl", ir_type_parse(e->m, "u16"));

    ir_instr_t *kc = op(e, IR_OP_CONST);

    ir_instr_set_dest_local(kc, kl);
    ir_instr_push_int(kc, 3);
    check("a constant may be assigned into a local",
          accepts_as(e, kc, "u16"));

    ir_instr_t *cp = op(e, IR_OP_COPY);

    ir_instr_push_value(cp, val(e, "u16"));
    check("copy keeps its operand type", accepts_as(e, cp, "u16"));

    ir_instr_t *useundef = op(e, IR_OP_COPY);

    ir_instr_push_value(useundef, u->dest);
    check("an undef value is legal to read",
          accepts_as(e, useundef, "i32"));

    ir_local_t *l = ir_local_add(e->f, "n", ir_type_parse(e->m, "u16"));

    ir_instr_t *rd = op(e, IR_OP_COPY);

    ir_instr_push_local(rd, l);
    check("a local as an operand is a read of the local's type",
          accepts_as(e, rd, "u16"));

    ir_instr_t *wr = op(e, IR_OP_COPY);

    ir_instr_set_dest_local(wr, l);
    ir_instr_push_value(wr, val(e, "u16"));
    check("and a local is a legal destination", accepts_as(e, wr, "u16"));

    ir_instr_t *wrbad = op(e, IR_OP_COPY);

    ir_instr_set_dest_local(wrbad, l);
    ir_instr_push_value(wrbad, val(e, "i32"));
    check("a destination whose type is not the result type is rejected",
          apply(e, wrbad) == IR_RULE_REJECT);

    ir_instr_t *dbad = op(e, IR_OP_ADD);

    ir_instr_set_dest(dbad, "sum", ir_type_parse(e->m, "i64"));
    ir_instr_push_mod(dbad, "wrap");
    ir_instr_push_value(dbad, val(e, "i32"));
    ir_instr_push_value(dbad, val(e, "i32"));
    check("that holds for a value destination too",
          apply(e, dbad) == IR_RULE_REJECT);

    close_env(e);
}

static void test_second_profile(void)
{
    env *e = open_env(&ir_profile_synth, "int");

    ir_instr_t *cmp = op(e, IR_OP_LT);

    ir_instr_push_value(cmp, val(e, "int"));
    ir_instr_push_value(cmp, val(e, "int"));
    check("a comparison yields this profile's boolean, not bce's",
          accepts_as(e, cmp, "flag"));

    ir_instr_t *cnt = op(e, IR_OP_POPCOUNT);

    ir_instr_push_value(cnt, val(e, "int"));
    check("a bit count yields this profile's 32-bit unsigned type",
          accepts_as(e, cnt, "word"));

    ir_instr_t *sz = op(e, IR_OP_SIZEOF);

    ir_instr_push_type(sz, ir_type_parse(e->m, "int"));
    check("sizeof has no 64-bit unsigned type in this profile",
          apply(e, sz) == IR_RULE_REJECT);

    ir_instr_t *cat = op(e, IR_OP_ADD);

    ir_instr_push_value(cat, val(e, "str"));
    ir_instr_push_value(cat, val(e, "str"));
    check("the profile's own rule beats the default for its own type",
          accepts_as(e, cat, "str"));
    check("and the profile rule is what is recorded",
          ir_annot_get(&cat->annots, "ir.rule")
              && !strcmp(ir_annot_get(&cat->annots, "ir.rule")->s,
                         "synth.concat"));

    ir_instr_t *plain = op(e, IR_OP_ADD);

    ir_instr_push_mod(plain, "wrap");
    ir_instr_push_value(plain, val(e, "int"));
    ir_instr_push_value(plain, val(e, "int"));
    check("the default still applies to the profile's other types",
          accepts_as(e, plain, "int"));
    check("and the default rule is what is recorded",
          ir_annot_get(&plain->annots, "ir.rule")
              && !strcmp(ir_annot_get(&plain->annots, "ir.rule")->s,
                         "ir.add"));

    close_env(e);
}

static void test_constants_are_target_independent(void)
{
    env *a = open_env_on(&ir_profile_bce, "i32", "one");
    env *b = open_env_on(&ir_profile_bce, "i32", "two");

    ir_instr_t *ha = op(a, IR_OP_TYPEHASH);
    ir_instr_t *hb = op(b, IR_OP_TYPEHASH);

    ir_instr_push_type(ha, ir_type_parse(a->m, "ptr[i32]"));
    ir_instr_push_type(hb, ir_type_parse(b->m, "ptr[i32]"));
    apply(a, ha);
    apply(b, hb);

    ir_annot_t *ca = ir_annot_get(&ha->annots, "ir.const");
    ir_annot_t *cb = ir_annot_get(&hb->annots, "ir.const");

    check("typehash is the same constant under either target",
          ca && cb && ir_lit_eq(ca->i, cb->i));

    ir_instr_t *na = op(a, IR_OP_TYPENAME);
    ir_instr_t *nb = op(b, IR_OP_TYPENAME);

    ir_instr_push_type(na, ir_type_parse(a->m, "ptr[i32]"));
    ir_instr_push_type(nb, ir_type_parse(b->m, "ptr[i32]"));
    apply(a, na);
    apply(b, nb);

    ir_annot_t *sa = ir_annot_get(&na->annots, "ir.const");
    ir_annot_t *sb = ir_annot_get(&nb->annots, "ir.const");

    check("and so is typename",
          sa && sb && !strcmp(sa->s, sb->s));

    close_env(b);
    close_env(a);
}

static void test_generics_pass_through(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *cp = op(e, IR_OP_COPY);

    ir_instr_push_value(cp, val(e, "`a"));
    check("a non-ground type passes through copy", accepts_as(e, cp, "`a"));

    ir_instr_t *ar = op(e, IR_OP_ADD);

    ir_instr_push_mod(ar, "wrap");
    ir_instr_push_value(ar, val(e, "`a"));
    ir_instr_push_value(ar, val(e, "`a"));
    check("arithmetic on an opaque type variable is rejected",
          apply(e, ar) == IR_RULE_REJECT);

    ir_instr_t *ld = op(e, IR_OP_LOAD);

    ir_instr_push_value(ld, val(e, "ptr[`a]"));
    check("a pointer to a type variable still loads",
          accepts_as(e, ld, "`a"));

    close_env(e);
}

static const ir_scalar_t bare_scalars[] = {
    {"word", IR_CLASS_UINT, 16, IR_ROLE_NONE},
};

static const ir_profile_t bare_profile = {
    .name      = "bare",
    .n_scalars = 1,
    .scalars   = bare_scalars,
};

static void test_profile_without_the_types_a_rule_needs(void)
{
    env *e = open_env(&bare_profile, "word");

    ir_instr_t *cnt = op(e, IR_OP_POPCOUNT);

    ir_instr_push_value(cnt, val(e, "word"));
    check("a bit count needs a 32-bit unsigned type the profile lacks",
          apply(e, cnt) == IR_RULE_REJECT);

    ir_instr_t *cmp = op(e, IR_OP_EQ);

    ir_instr_push_value(cmp, val(e, "word"));
    ir_instr_push_value(cmp, val(e, "word"));
    check("a comparison needs a boolean type the profile lacks",
          apply(e, cmp) == IR_RULE_REJECT);

    ir_instr_t *df = op(e, IR_OP_DIFF);

    ir_instr_push_value(df, val(e, "ptr[word]"));
    ir_instr_push_value(df, val(e, "ptr[word]"));
    check("diff needs a 64-bit signed type the profile lacks",
          apply(e, df) == IR_RULE_REJECT);

    ir_instr_t *mc = op(e, IR_OP_MEMCMP);

    ir_instr_push_value(mc, val(e, "ptr[word]"));
    ir_instr_push_value(mc, val(e, "ptr[word]"));
    ir_instr_push_value(mc, val(e, "word"));
    check("memcmp needs a 32-bit signed type the profile lacks",
          apply(e, mc) == IR_RULE_REJECT);

    ir_instr_t *vt = op(e, IR_OP_VARIANT_TAG);

    ir_instr_push_value(vt, val(e, "variant R[word]"));
    check("variant.tag needs a 64-bit unsigned type the profile lacks",
          apply(e, vt) == IR_RULE_REJECT);

    ir_instr_t *tn = op(e, IR_OP_TYPENAME);

    ir_instr_push_type(tn, ir_type_parse(e->m, "word"));
    check("typename needs a character type the profile lacks",
          apply(e, tn) == IR_RULE_REJECT);

    close_env(e);
}

static void test_diagnostics_point_at_operands(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *def = op(e, IR_OP_COPY);

    ir_instr_set_dest(def, "src", ir_type_parse(e->m, "u64"));
    ir_instr_push_value(def, val(e, "u64"));
    def->span = ir_span_make(0, 10, 14, 2, 3, 2, 7);
    apply(e, def);

    ir_instr_t *use = op(e, IR_OP_ADD);

    ir_instr_push_mod(use, "wrap");
    ir_instr_push_value(use, val(e, "i32"));
    ir_instr_push_value(use, def->dest);
    use->span = ir_span_make(0, 30, 34, 5, 3, 5, 7);

    uint32_t before = ir_diags_count(e->d);

    check("mismatched operands are rejected",
          apply(e, use) == IR_RULE_REJECT);

    const ir_diag_t *dg = ir_diags_count(e->d) > before
                        ? ir_diags_at(e->d, before)
                        : nullptr;

    check("the diagnostic is at the instruction",
          dg && dg->primary.span.start_line == 5);
    check("and names both types",
          dg && strstr(dg->message, "i32") && strstr(dg->message, "u64"));
    check("and points at where the operand was defined",
          dg && dg->n_secondary == 1
              && dg->secondary[0].span.start_line == 2);

    close_env(e);
}

static void test_a_rule_will_not_overwrite_an_authored_annotation(void)
{
    env        *e  = open_env(&ir_profile_bce, "i32");
    ir_instr_t *in = op(e, IR_OP_ADD);

    ir_instr_push_mod(in, "wrap");
    ir_instr_push_value(in, val(e, "i32"));
    ir_instr_push_value(in, val(e, "i32"));
    ir_annot_set(e->ctx, &in->annots,
                 (ir_annot_t){.key = "ir.rule", .kind = IR_ANNOT_STR,
                              .s = "mine"});

    uint32_t before = ir_diags_count(e->d);

    check("typing still accepts the instruction",
          apply(e, in) == IR_RULE_ACCEPT);

    ir_annot_t *a = ir_annot_get(&in->annots, "ir.rule");

    check("the authored annotation is the one that stands",
          a && a->kind == IR_ANNOT_STR && !strcmp(a->s, "mine"));
    check("and it is still authored, not re-derived",
          a && a->derived_from == 0);
    const ir_diag_t *said = ir_diags_count(e->d) > before
                              ? ir_diags_at(e->d, before)
                              : nullptr;

    check("and the rule said so rather than overwriting in silence",
          said && said->code == IR_DIAG_AUTHORED_ANNOT);
    check("which is a warning, because the output is still valid",
          said && said->severity == IR_SEV_WARNING);

    ir_instr_t *clean = op(e, IR_OP_ADD);

    ir_instr_push_mod(clean, "wrap");
    ir_instr_push_value(clean, val(e, "i32"));
    ir_instr_push_value(clean, val(e, "i32"));

    uint32_t quiet = ir_diags_count(e->d);

    apply(e, clean);

    ir_annot_t *w = ir_annot_get(&clean->annots, "ir.rule");

    check("an instruction with nothing authored is annotated as before",
          w && w->derived_from == IR_PROP_TYPED);
    check("and says nothing", ir_diags_count(e->d) == quiet);
    close_env(e);
}

static void test_boxing(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *ground = op(e, IR_OP_COPY);

    ir_instr_push_value(ground, val(e, "i32"));
    apply(e, ground);
    check("a ground operand is not marked boxed",
          ir_annot_get(&ground->annots, "ir.boxed") == nullptr);

    ir_instr_t *gen = op(e, IR_OP_COPY);

    ir_instr_push_value(gen, val(e, "`a"));
    apply(e, gen);

    ir_annot_t *bx = ir_annot_get(&gen->annots, "ir.boxed");

    check("a non-ground operand is marked boxed",
          bx && bx->kind == IR_ANNOT_INT && bx->i.mag == 1);
    check("and boxing is derived from typing",
          bx && bx->derived_from == IR_PROP_TYPED);

    ir_instr_t *mix = op(e, IR_OP_STORE);

    ir_instr_push_value(mix, val(e, "ptr[i32]"));
    ir_instr_push_value(mix, val(e, "i32"));
    apply(e, mix);
    check("a store of ground types boxes nothing",
          ir_annot_get(&mix->annots, "ir.boxed") == nullptr);

    ir_instr_t *mix2 = op(e, IR_OP_STORE);

    ir_instr_push_value(mix2, val(e, "ptr[`a]"));
    ir_instr_push_value(mix2, val(e, "`a"));
    apply(e, mix2);

    ir_annot_t *bx2 = ir_annot_get(&mix2->annots, "ir.boxed");

    check("the mark names which operands are boxed",
          bx2 && bx2->i.mag == 3);

    ir_module_invalidate(e->m, IR_PROP_TYPED);
    check("and it is dropped when typing is invalidated",
          ir_annot_get(&mix2->annots, "ir.boxed") == nullptr);

    th_member   mem[200];
    char        mname[200][16];
    ir_instr_t *wide = op(e, IR_OP_AGGREGATE);

    for (uint32_t i = 0; i < 200; i++) {
        const char *spelling = i % 2 ? "`a" : "i32";

        snprintf(mname[i], sizeof mname[i], "m%u", i);
        mem[i].name = mname[i];
        mem[i].type = ir_type_parse(e->m, spelling);
        ir_instr_push_value(wide, val(e, spelling));
    }

    th_type *rec = th_record(ir_ctx_types(e->ctx), mem, 200);

    check("a record of 200 members can be built", rec != nullptr);
    ir_instr_set_dest(wide, "wide", rec);
    check("an instruction wider than the mark's integer types",
          apply(e, wide) == IR_RULE_ACCEPT);

    ir_annot_t *w0 = ir_annot_get(&wide->annots, "ir.boxed");
    ir_annot_t *w1 = ir_annot_get(&wide->annots, "ir.boxed.1");

    check("an instruction wider than the mark's integer still says which "
          "operands are boxed",
          w0 && w1 && w0->kind == IR_ANNOT_INT && w1->kind == IR_ANNOT_INT);
    check("the first mark covers the first 128 operands",
          w0 && (w0->i.mag >> 1 & 1) == 1 && (w0->i.mag >> 2 & 1) == 0);
    check("and the second covers the rest, numbered from its own start",
          w1 && (w1->i.mag >> 1 & 1) == 1 && (w1->i.mag >> 2 & 1) == 0);

    close_env(e);
}

static void test_every_opcode_on_generics(void)
{
    env *e = open_env(&ir_profile_bce, "`r");

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        ir_op_t     o  = (ir_op_t)i;
        ir_instr_t *in = ir_instr_add(e->b, o);

        for (uint32_t k = 0; k < ir_op_info(o)->min_operands; k++) {
            switch (ir_op_operand_kind(o, k)) {
            case IR_OPD_VALUE:
                ir_instr_push_value(in, val(e, "`a"));
                break;
            case IR_OPD_TYPE:
                ir_instr_push_type(in, ir_type_parse(e->m, "`a"));
                break;
            case IR_OPD_NAME:
                ir_instr_push_name(in, "x");
                break;
            case IR_OPD_BLOCK:
                ir_instr_push_block(in, e->b);
                break;
            case IR_OPD_DEFAULT_BLOCK:
                ir_instr_push_default(in, e->b);
                break;
            case IR_OPD_CONST:
                ir_instr_push_int(in, 1);
                break;
            case IR_OPD_PHI_ARM:
                ir_instr_push_phi_arm(in, e->b, val(e, "`a"));
                break;
            case IR_OPD_SWITCH_ARM:
                ir_instr_push_switch_arm(in, 1, e->b);
                break;
            case IR_OPD_NAMED_ARG:
                ir_instr_push_named(in, "x", val(e, "`a"));
                break;
            default:
                break;
            }
        }

        uint32_t     before = ir_diags_count(e->d);
        ir_verdict_t v      = apply(e, in);
        bool         spoke  = ir_diags_count(e->d) > before;

        if (v == IR_RULE_REJECT && !spoke) {
            printf("FAIL  %s says why it rejects a non-ground operand\n",
                   ir_op_spelling(o));
            fails++;
        }

        if (v != IR_RULE_REJECT && spoke) {
            printf("FAIL  %s stays quiet when it does not reject\n",
                   ir_op_spelling(o));
            fails++;
        }

        if (v == IR_RULE_ACCEPT && !ir_annot_get(&in->annots, "ir.rule")) {
            printf("FAIL  %s records which rule accepted it\n",
                   ir_op_spelling(o));
            fails++;
        }
    }

    ir_instr_t *cp = op(e, IR_OP_COPY);

    ir_instr_push_value(cp, val(e, "`a"));
    check("an accepted generic result stays generic; nothing is instantiated",
          accepts_as(e, cp, "`a"));

    close_env(e);
}

static void test_opaque_named(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *cp = op(e, IR_OP_COPY);

    ir_instr_push_value(cp, val(e, "Widget"));
    check("an opaque named type can be moved", accepts_as(e, cp, "Widget"));

    ir_instr_t *al = op(e, IR_OP_ALLOCA);

    ir_instr_push_type(al, ir_type_parse(e->m, "Widget"));
    check("an opaque named type can be pointed to",
          accepts_as(e, al, "ptr[Widget]"));

    ir_value_t *fv = ir_alloc(e->ctx, sizeof(ir_value_t));

    fv->kind = IR_VAL_FUNC;
    fv->name = "takes";
    fv->type = ir_type_parse(e->m, "(Widget) -> i32");

    ir_instr_t *c = op(e, IR_OP_CALL);

    ir_instr_push_value(c, fv);
    ir_instr_push_value(c, val(e, "Widget"));
    check("an opaque named type can be passed", accepts_as(e, c, "i32"));

    ir_instr_t *st = op(e, IR_OP_STORE);

    ir_instr_push_value(st, val(e, "ptr[Widget]"));
    ir_instr_push_value(st, val(e, "Widget"));
    check("an opaque named type can be stored",
          accepts_as(e, st, nullptr));

    ir_instr_t *ar = op(e, IR_OP_ADD);

    ir_instr_push_mod(ar, "wrap");
    ir_instr_push_value(ar, val(e, "Widget"));
    ir_instr_push_value(ar, val(e, "Widget"));
    check("an opaque named type cannot be added",
          apply(e, ar) == IR_RULE_REJECT);

    close_env(e);
}

static void test_annotation_of_results(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *a = op(e, IR_OP_ADD);

    ir_instr_push_mod(a, "wrap");
    ir_instr_push_value(a, val(e, "i32"));
    ir_instr_push_value(a, val(e, "i32"));
    apply(e, a);

    ir_annot_t *an = ir_annot_get(&a->annots, "ir.rule");

    check("an accepted instruction records which rule fired",
          an && !strcmp(an->s, "ir.add"));
    check("and marks it derived from typing",
          an && an->derived_from == IR_PROP_TYPED);

    ir_instr_t *bad = op(e, IR_OP_ADD);

    ir_instr_push_value(bad, val(e, "i32"));
    ir_instr_push_value(bad, val(e, "i32"));
    apply(e, bad);
    check("a rejected instruction records nothing",
          ir_annot_get(&bad->annots, "ir.rule") == nullptr);

    ir_instr_t *tn = op(e, IR_OP_TYPENAME);

    ir_instr_push_type(tn, ir_type_parse(e->m, "u8"));
    apply(e, tn);

    ir_annot_t *ret = ir_annot_get(&tn->annots, "ir.const");

    check("an annotation a rule returned is attached to the instruction",
          ret && ret->kind == IR_ANNOT_STR && !strcmp(ret->s, "u8"));
    check("and it too is derived from typing",
          ret && ret->derived_from == IR_PROP_TYPED);

    ir_module_invalidate(e->m, IR_PROP_TYPED);
    check("invalidating typing drops what typing derived",
          ir_annot_get(&a->annots, "ir.rule") == nullptr);
    check("including what a rule returned",
          ir_annot_get(&tn->annots, "ir.const") == nullptr);

    close_env(e);
}

static void write_produced(const char *dir)
{
    const char *leaf = "/produced-codes-typing.txt";
    size_t      n    = strlen(dir) + strlen(leaf) + 1;
    char       *path = malloc(n);

    if (!path) {
        check("the produced-code list could be written", false);

        return;
    }
    snprintf(path, n, "%s%s", dir, leaf);

    FILE *f = fopen(path, "w");

    if (!f) {
        check("the produced-code list could be written", false);
        free(path);

        return;
    }

    for (uint16_t i = 0; i < IR_DIAG_COUNT; i++) {
        if (produced[i]) {
            fprintf(f, "%s\n", ir_diag_code((ir_diagcode_t)i));
        }
    }
    fclose(f);
    free(path);
}

static void report_coverage(void)
{
    uint32_t missing = 0;

    for (uint16_t i = 0; i < IR_OP_COUNT; i++) {
        if (!covered[i]) {
            if (!missing) {
                printf("  opcodes with no typing test:");
            }
            printf(" %s", ir_op_spelling((ir_op_t)i));
            missing++;
        }
    }

    if (missing) {
        printf("\n");
    }
    check("every opcode has a typing test", missing == 0);
    check("every opcode has a default rule",
          ir_default_rule_count() == IR_OP_COUNT);
}

int main(int argc, char **argv)
{

    test_constants_and_copy();
    test_arithmetic();
    test_bitwise_and_shifts();
    test_comparisons();
    test_memory();
    test_aggregates_and_variants();
    test_conversion();
    test_select_phi_call();
    test_comptime_and_terminators();
    test_second_profile();
    test_constants_are_target_independent();
    test_generics_pass_through();
    test_profile_without_the_types_a_rule_needs();
    test_diagnostics_point_at_operands();
    test_a_rule_will_not_overwrite_an_authored_annotation();
    test_boxing();
    test_every_opcode_on_generics();
    test_opaque_named();
    test_annotation_of_results();
    report_coverage();

    if (argc > 1) {
        write_produced(argv[1]);
    }

    if (!fails) {
        printf("ir typing: all checks passed\n");
    }

    return fails ? 1 : 0;
}
