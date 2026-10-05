#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/rule.h"
#include "ir/shape.h"
#include "ir/text.h"
#include "harness.h"

static bool covered[IR_OP_COUNT];
static bool produced[IR_DIAG_COUNT];

static void
test_context_owned_named_types(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *owner = ir_module_new(ctx, "owner", &ir_profile_bce, "host");
    ir_module_t *consumer = ir_module_new(ctx, "consumer", &ir_profile_bce,
                                          "host");
    th_type *named = ir_type_parse(owner, "pkg.Point");
    th_type *definition = ir_type_parse(owner, "(x: i32, y: u64)");
    check("one module owns a context-wide named type definition",
          ir_type_define(owner, named, definition)
              && ir_type_definition_owner(owner, named) == owner);
    check("another module resolves the canonical named type definition",
          ir_type_definition(consumer, named) == definition
              && ir_type_definition_owner(consumer, named) == owner);
    check("a consumer can reuse the same named type representation",
          ir_type_define(consumer, named, definition)
              && ir_type_definition(consumer, named) == definition
              && ir_type_definition_owner(consumer, named) == owner);
    check("a consumer cannot install a conflicting type representation",
          !ir_type_define(consumer, named,
                          ir_type_parse(consumer, "(x: u64, y: i32)")));

    th_type *callable = ir_type_parse(owner, "() -> i32 fail: string");
    th_type *outcome = ir_call_outcome_type(owner, callable);
    check("the defining module owns the call outcome type",
          outcome && ir_type_definition_owner(owner, outcome) == owner);
    check("consumer modules can inspect the shared call outcome definition",
          outcome && ir_type_definition(consumer, outcome)
              == ir_type_definition(owner, outcome)
              && ir_type_definition_owner(consumer, outcome) == owner);
    th_type *consumer_outcome = ir_call_outcome_type(consumer, callable);
    check("consumer modules reuse the shared call outcome type",
          outcome && consumer_outcome
              && ir_type_eq(consumer, consumer_outcome, outcome));
    ir_ctx_free(ctx);
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

    th_type *expected = ir_type_parse(e->m, want);
    return r.result && expected
        && th_hash(ir_ctx_types(e->ctx), r.result)
            == th_hash(ir_ctx_types(e->ctx), expected);
}

static void test_arithmetic(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *a = op(e, IR_OP_ADD);

    ir_instr_add_mod(a, "wrap");
    ir_instr_operand_value(a, val(e, "i32"));
    ir_instr_operand_value(a, val(e, "i32"));
    check("add on matching integers yields that type",
          accepts_as(e, a, "i32"));

    ir_instr_t *f = op(e, IR_OP_MUL);

    ir_instr_operand_value(f, val(e, "f64"));
    ir_instr_operand_value(f, val(e, "f64"));
    check("float arithmetic needs no overflow modifier",
          accepts_as(e, f, "f64"));

    ir_instr_t *nomod = op(e, IR_OP_ADD);

    ir_instr_operand_value(nomod, val(e, "i32"));
    ir_instr_operand_value(nomod, val(e, "i32"));
    check("integer arithmetic without an overflow modifier is rejected",
          apply(e, nomod) == IR_RULE_REJECT);

    ir_instr_t *mixed = op(e, IR_OP_SUB);

    ir_instr_add_mod(mixed, "trap");
    ir_instr_operand_value(mixed, val(e, "i32"));
    ir_instr_operand_value(mixed, val(e, "i64"));
    check("mismatched arithmetic operands are rejected",
          apply(e, mixed) == IR_RULE_REJECT);

    ir_instr_t *ptr = op(e, IR_OP_ADD);

    ir_instr_add_mod(ptr, "wrap");
    ir_instr_operand_value(ptr, val(e, "ptr[i32]"));
    ir_instr_operand_value(ptr, val(e, "ptr[i32]"));
    check("arithmetic on a pointer is rejected",
          apply(e, ptr) == IR_RULE_REJECT);

    ir_instr_t *dv = op(e, IR_OP_DIV);

    ir_instr_add_mod(dv, "wrap");
    ir_instr_operand_value(dv, val(e, "i32"));
    ir_instr_operand_value(dv, val(e, "i32"));
    check("div without a divide-by-zero modifier is rejected",
          apply(e, dv) == IR_RULE_REJECT);

    ir_instr_t *dv2 = op(e, IR_OP_DIV);

    ir_instr_add_mod(dv2, "wrap");
    ir_instr_add_mod(dv2, "trap");
    ir_instr_operand_value(dv2, val(e, "i32"));
    ir_instr_operand_value(dv2, val(e, "i32"));
    check("div with both modifiers is accepted", accepts_as(e, dv2, "i32"));

    ir_instr_t *ng = op(e, IR_OP_NEG);

    ir_instr_add_mod(ng, "sat");
    ir_instr_operand_value(ng, val(e, "i32"));
    check("neg takes one operand", accepts_as(e, ng, "i32"));

    ir_instr_t *rm = op(e, IR_OP_REM);

    ir_instr_add_mod(rm, "undef");
    ir_instr_operand_value(rm, val(e, "u64"));
    ir_instr_operand_value(rm, val(e, "u64"));
    check("unsigned rem takes only a divide-by-zero modifier",
          accepts_as(e, rm, "u64"));

    ir_instr_t *rmx = op(e, IR_OP_REM);

    ir_instr_add_mod(rmx, "wrap");
    ir_instr_add_mod(rmx, "undef");
    ir_instr_operand_value(rmx, val(e, "u64"));
    ir_instr_operand_value(rmx, val(e, "u64"));
    check("unsigned rem does not take an overflow modifier too",
          apply(e, rmx) == IR_RULE_REJECT);

    ir_instr_t *sdv = op(e, IR_OP_DIV);

    ir_instr_add_mod(sdv, "trap");
    ir_instr_operand_value(sdv, val(e, "i32"));
    ir_instr_operand_value(sdv, val(e, "i32"));
    check("signed div needs an overflow modifier as well, for INT_MIN / -1",
          apply(e, sdv) == IR_RULE_REJECT);

    ir_instr_t *fm = op(e, IR_OP_ADD);

    ir_instr_add_mod(fm, "wrap");
    ir_instr_operand_value(fm, val(e, "f64"));
    ir_instr_operand_value(fm, val(e, "f64"));
    check("float arithmetic takes no overflow modifier",
          apply(e, fm) == IR_RULE_REJECT);

    const char *fits[] = {"wrap", "trap", "sat", "undef"};
    const char *vecs[] = {"u8", "i8"};

    for (size_t v = 0; v < 2; v++) {
        for (size_t i = 0; i < 4; i++) {
            ir_instr_t *in = op(e, IR_OP_ADD);

            ir_instr_add_mod(in, fits[i]);
            ir_instr_operand_value(in, val(e, vecs[v]));
            ir_instr_operand_value(in, val(e, vecs[v]));

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

        ir_instr_operand_value(in, val(e, "u32"));
        ir_instr_operand_value(in, val(e, "u32"));

        if (!accepts_as(e, in, "u32")) {
            printf("FAIL  %s on integers\n", ir_op_spelling(bits[i]));
            fails++;
        }
    }

    ir_instr_t *nt = op(e, IR_OP_NOT);

    ir_instr_operand_value(nt, val(e, "u32"));
    check("not takes one integer", accepts_as(e, nt, "u32"));

    ir_instr_t *bad = op(e, IR_OP_AND);

    ir_instr_operand_value(bad, val(e, "f64"));
    ir_instr_operand_value(bad, val(e, "f64"));
    check("bitwise on floats is rejected", apply(e, bad) == IR_RULE_REJECT);

    ir_instr_t *widths = op(e, IR_OP_OR);

    ir_instr_operand_value(widths, val(e, "u32"));
    ir_instr_operand_value(widths, val(e, "u64"));
    check("bitwise operands of different widths are rejected",
          apply(e, widths) == IR_RULE_REJECT);

    const ir_op_t shifts[] = {IR_OP_SHL, IR_OP_SHR, IR_OP_ROL, IR_OP_ROR};

    for (size_t i = 0; i < 4; i++) {
        ir_instr_t *in = op(e, shifts[i]);

        ir_instr_operand_value(in, val(e, "u32"));
        ir_instr_operand_value(in, val(e, "u8"));

        if (!accepts_as(e, in, "u32")) {
            printf("FAIL  %s keeps the shifted type\n",
                   ir_op_spelling(shifts[i]));
            fails++;
        }
    }

    ir_instr_t *sbad = op(e, IR_OP_SHL);

    ir_instr_operand_value(sbad, val(e, "u32"));
    ir_instr_operand_value(sbad, val(e, "f64"));
    check("a float shift amount is rejected",
          apply(e, sbad) == IR_RULE_REJECT);

    ir_instr_t *ss = op(e, IR_OP_SHR);

    ir_instr_operand_value(ss, val(e, "i32"));
    ir_instr_operand_value(ss, val(e, "u8"));
    check("one shr shifts a signed value arithmetically",
          accepts_as(e, ss, "i32"));

    ir_instr_t *su = op(e, IR_OP_SHR);

    ir_instr_operand_value(su, val(e, "u32"));
    ir_instr_operand_value(su, val(e, "u8"));
    check("and the same shr shifts an unsigned value logically",
          accepts_as(e, su, "u32"));

    ir_instr_t *msk = op(e, IR_OP_SHL);

    check("a shift takes the mask modifier",
          ir_instr_add_mod(msk, "mask"));
    ir_instr_operand_value(msk, val(e, "u32"));
    ir_instr_operand_value(msk, val(e, "u8"));
    check("and still yields the shifted type", accepts_as(e, msk, "u32"));

    ir_instr_t *nomask = op(e, IR_OP_SHL);

    ir_instr_operand_value(nomask, val(e, "u32"));
    ir_instr_operand_value(nomask, val(e, "u8"));
    check("a shift without mask is still well typed",
          accepts_as(e, nomask, "u32"));

    ir_instr_t *wrongmod = op(e, IR_OP_SHL);

    check("a shift accepts a checked overflow modifier",
          ir_instr_add_mod(wrongmod, "trap"));
    ir_instr_operand_value(wrongmod, val(e, "u32"));
    ir_instr_operand_value(wrongmod, val(e, "u8"));
    check("a checked shift remains well typed",
          accepts_as(e, wrongmod, "u32"));

    const ir_op_t counts[] = {IR_OP_CLZ, IR_OP_CTZ, IR_OP_POPCOUNT};

    for (size_t i = 0; i < 3; i++) {
        ir_instr_t *in = op(e, counts[i]);

        ir_instr_operand_value(in, val(e, "u64"));

        if (!accepts_as(e, in, "u32")) {
            printf("FAIL  %s yields u32\n", ir_op_spelling(counts[i]));
            fails++;
        }
    }

    ir_instr_t *rv = op(e, IR_OP_BITREVERSE);

    ir_instr_operand_value(rv, val(e, "u16"));
    check("bitreverse keeps its type", accepts_as(e, rv, "u16"));

    ir_instr_t *bs = op(e, IR_OP_BYTESWAP);

    ir_instr_operand_value(bs, val(e, "u16"));
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

        ir_instr_operand_value(in, val(e, "i32"));
        ir_instr_operand_value(in, val(e, "i32"));

        if (!accepts_as(e, in, "bool")) {
            printf("FAIL  %s yields bool\n", ir_op_spelling(cmps[i]));
            fails++;
        }
    }

    ir_instr_t *mixed = op(e, IR_OP_LT);

    ir_instr_operand_value(mixed, val(e, "i32"));
    ir_instr_operand_value(mixed, val(e, "u32"));
    check("comparing different types is rejected",
          apply(e, mixed) == IR_RULE_REJECT);

    ir_instr_t *fl = op(e, IR_OP_LT);

    ir_instr_operand_value(fl, val(e, "f64"));
    ir_instr_operand_value(fl, val(e, "f64"));
    check("a float comparison without ordering is rejected",
          apply(e, fl) == IR_RULE_REJECT);

    ir_instr_t *fl2 = op(e, IR_OP_LT);

    ir_instr_add_mod(fl2, "ordered");
    ir_instr_operand_value(fl2, val(e, "f64"));
    ir_instr_operand_value(fl2, val(e, "f64"));
    check("a float comparison with ordering is accepted",
          accepts_as(e, fl2, "bool"));

    ir_instr_t *fl3 = op(e, IR_OP_NE);

    ir_instr_add_mod(fl3, "unordered");
    ir_instr_operand_value(fl3, val(e, "f32"));
    ir_instr_operand_value(fl3, val(e, "f32"));
    check("unordered is the other choice", accepts_as(e, fl3, "bool"));

    ir_instr_t *iord = op(e, IR_OP_LT);

    ir_instr_add_mod(iord, "ordered");
    ir_instr_operand_value(iord, val(e, "i32"));
    ir_instr_operand_value(iord, val(e, "i32"));
    check("an integer comparison does not take an ordering modifier",
          apply(e, iord) == IR_RULE_REJECT);

    for (size_t i = 0; i < 6; i++) {
        ir_instr_t *in = op(e, cmps[i]);

        ir_instr_operand_value(in, val(e, "u32"));
        ir_instr_operand_value(in, val(e, "u32"));

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

    ir_local_t *local = ir_local_add(e->f, "item", ir_type_parse(e->m, "i32"));
    ir_instr_t *address = op(e, IR_OP_ADDRESS_OF);
    ir_instr_operand_local(address, local);
    check("address.of returns a pointer to the existing local",
          accepts_as(e, address, "ref[i32]"));

    ir_global_t *addressable_global = ir_global_add(
        e->m, "item_global", ir_type_parse(e->m, "i32"));
    ir_instr_t *global_address = op(e, IR_OP_ADDRESS_OF);
    ir_instr_operand_value(global_address,
                           ir_global_address(e->m, addressable_global));
    check("address.of returns a pointer to the existing global",
          accepts_as(e, global_address, "ref[i32]"));

    ir_instr_t *copy = op(e, IR_OP_COPY);
    ir_instr_operand_value(copy, val(e, "i32"));
    ir_instr_set_dest(copy, "temporary", ir_type_parse(e->m, "i32"));
    ir_instr_t *bad_address = op(e, IR_OP_ADDRESS_OF);
    ir_instr_operand_value(bad_address, copy->dest);
    check("address.of rejects a non-binding value",
          apply(e, bad_address) == IR_RULE_REJECT);

    ir_instr_t *alloc = op(e, IR_OP_ALLOC);
    ir_instr_operand_type(alloc, ir_type_parse(e->m, "i32"));
    check("alloc yields an address to its type",
          accepts_as(e, alloc, "ptr[i32]"));
    check("alloc preserves an extensible required storage class",
          ir_instr_set_storage_class(alloc, "gc", true));
    bool storage_required = false;
    check("alloc exposes its selected storage class and requirement",
          !strcmp(ir_instr_storage_class(alloc, &storage_required), "gc")
              && storage_required);

    ir_instr_t *ref_alloc = op(e, IR_OP_ALLOC);
    ir_instr_set_dest(ref_alloc, "heap_ref", ir_type_parse(e->m, "ref[i32]"));
    ir_instr_operand_type(ref_alloc, ir_type_parse(e->m, "i32"));
    check("alloc accepts a front-end-selected ref result type",
          accepts_as(e, ref_alloc, "ref[i32]"));
    ir_instr_t *bad_ref_alloc = op(e, IR_OP_ALLOC);
    ir_instr_set_dest(bad_ref_alloc, "bad_heap_ref",
                      ir_type_parse(e->m, "ref[u32]"));
    ir_instr_operand_type(bad_ref_alloc, ir_type_parse(e->m, "i32"));
    check("alloc rejects an indirection with the wrong referent",
          apply(e, bad_ref_alloc) == IR_RULE_REJECT);

    ir_instr_t *alloc_using = op(e, IR_OP_ALLOC_USING);
    ir_instr_operand_type(alloc_using, ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(alloc_using, val(e, "Allocator"));
    check("alloc.using retains a generic runtime allocator operand",
          accepts_as(e, alloc_using, "ptr[i32]"));
    ir_instr_t *alloc_using_bad = op(e, IR_OP_ALLOC_USING);
    ir_instr_operand_type(alloc_using_bad, ir_type_parse(e->m, "i32"));
    check("alloc.using requires an allocator operand",
          apply(e, alloc_using_bad) == IR_RULE_REJECT);

    ir_instr_t *finalize = op(e, IR_OP_FINALIZE);
    ir_instr_operand_value(finalize, val(e, "i32"));
    check("finalize is an explicit side-effecting no-result operation",
          accepts_as(e, finalize, nullptr));

    ir_instr_t *dealloc = op(e, IR_OP_DEALLOC);
    ir_instr_operand_value(dealloc, val(e, "ptr[i32]"));
    check("dealloc accepts an address and yields no result",
          accepts_as(e, dealloc, nullptr));

    ir_instr_t *deallocbad = op(e, IR_OP_DEALLOC);
    ir_instr_operand_value(deallocbad, val(e, "i32"));
    check("dealloc rejects non-address values",
          apply(e, deallocbad) == IR_RULE_REJECT);

    ir_instr_t *al = op(e, IR_OP_ALLOCA);

    ir_instr_operand_type(al, ir_type_parse(e->m, "i32"));
    check("alloca yields a pointer to its type",
          accepts_as(e, al, "ptr[i32]"));

    ir_instr_t *ld = op(e, IR_OP_LOAD);

    ir_instr_operand_value(ld, val(e, "ptr[i32]"));
    check("load yields the pointee", accepts_as(e, ld, "i32"));

    ir_instr_t *ldbad = op(e, IR_OP_LOAD);

    ir_instr_operand_value(ldbad, val(e, "i32"));
    check("load through a non-pointer is rejected",
          apply(e, ldbad) == IR_RULE_REJECT);

    ir_instr_t *st = op(e, IR_OP_STORE);

    ir_instr_operand_value(st, val(e, "ptr[i32]"));
    ir_instr_operand_value(st, val(e, "i32"));
    check("store yields nothing", accepts_as(e, st, nullptr));

    ir_instr_t *stbad = op(e, IR_OP_STORE);

    ir_instr_operand_value(stbad, val(e, "ptr[i32]"));
    ir_instr_operand_value(stbad, val(e, "u64"));
    check("storing the wrong type is rejected",
          apply(e, stbad) == IR_RULE_REJECT);

    ir_global_t *global = ir_global_add(e->m, "counter",
                                        ir_type_parse(e->m, "i32"));
    if (global) global->mutability = IR_MUT_MUTABLE;
    ir_value_t *global_value = global
        ? ir_global_address(e->m, global) : nullptr;
    ir_instr_t *global_store = op(e, IR_OP_GLOBAL_STORE);
    ir_instr_operand_value(global_store, global_value);
    ir_instr_operand_value(global_store, val(e, "i32"));
    check("global.store accepts a matching module global",
          accepts_as(e, global_store, nullptr));

    ir_instr_t *global_store_bad = op(e, IR_OP_GLOBAL_STORE);
    ir_instr_operand_value(global_store_bad, global_value);
    ir_instr_operand_value(global_store_bad, val(e, "u64"));
    check("global.store rejects a mismatched value type",
          apply(e, global_store_bad) == IR_RULE_REJECT);

    ir_global_t *constant = ir_global_add(e->m, "constant",
                                          ir_type_parse(e->m, "i32"));
    if (constant) constant->mutability = IR_MUT_CONST;
    ir_instr_t *constant_store = op(e, IR_OP_GLOBAL_STORE);
    ir_instr_operand_value(constant_store,
        constant ? ir_global_address(e->m, constant) : nullptr);
    ir_instr_operand_value(constant_store, val(e, "i32"));
    check("phase mutability metadata is accepted",
          constant
              && ir_global_set_comptime_mutability(
                  e->ctx, constant, IR_MUT_MUTABLE)
              && ir_func_set_execution_phase(e->ctx, e->f,
                                             IR_PHASE_COMPTIME)
              && ir_func_execution_phase(e->f) == IR_PHASE_COMPTIME);
    check("global.store accepts a compile-time write to a const global",
          apply(e, constant_store) == IR_RULE_ACCEPT);
    check("runtime phase can be restored",
          ir_func_set_execution_phase(e->ctx, e->f, IR_PHASE_RUNTIME)
              && ir_func_execution_phase(e->f) == IR_PHASE_RUNTIME);
    check("global.store rejects runtime writes to constant globals",
          apply(e, constant_store) == IR_RULE_REJECT);

    ir_global_t *phase_limited = ir_global_add(
        e->m, "phase_limited", ir_type_parse(e->m, "i32"));
    ir_instr_t *phase_store = op(e, IR_OP_GLOBAL_STORE);
    ir_instr_operand_value(phase_store,
        phase_limited ? ir_global_address(e->m, phase_limited) : nullptr);
    ir_instr_operand_value(phase_store, val(e, "i32"));
    bool phase_contract = phase_limited
        && ir_global_set_phase_access(e->ctx, phase_limited,
            IR_PHASE_ACCESS_RUNTIME, IR_PHASE_ACCESS_RUNTIME,
            IR_PHASE_ACCESS_COMPTIME)
        && ir_func_set_execution_phase(e->ctx, e->f, IR_PHASE_COMPTIME);
    check("ordinary compile-time code cannot write a runtime-only global",
          phase_contract && apply(e, phase_store) == IR_RULE_REJECT);
    check("a compile-time initializer can initialize that global",
          phase_contract
              && ir_func_set_initializer(e->ctx, e->f, true)
              && apply(e, phase_store) == IR_RULE_ACCEPT);
    check("an initializer marker is queryable and clearable",
          ir_func_is_initializer(e->f)
              && ir_func_set_initializer(e->ctx, e->f, false)
              && !ir_func_is_initializer(e->f));
    check("a marked compile-time assignment can initialize a global",
          phase_contract
              && ir_instr_set_binding_initialization(phase_store, true)
              && apply(e, phase_store) == IR_RULE_ACCEPT);
    check("runtime mutable access remains allowed",
          ir_func_set_execution_phase(e->ctx, e->f, IR_PHASE_RUNTIME)
              && apply(e, phase_store) == IR_RULE_ACCEPT);

    ir_instr_t *fd = op(e, IR_OP_FIELD);

    ir_instr_operand_value(fd, val(e, "ptr[(x: i32, y: u64)]"));
    ir_instr_operand_name(fd, "y");
    check("field resolves the member through typehash",
          accepts_as(e, fd, "ptr[u64]"));

    ir_instr_t *fdref = op(e, IR_OP_FIELD);

    ir_instr_operand_value(fdref, val(e, "ref[(x: i32, y: u64)]"));
    ir_instr_operand_name(fdref, "y");
    check("field preserves the base reference kind",
          accepts_as(e, fdref, "ref[u64]"));

    ir_instr_t *fdbad = op(e, IR_OP_FIELD);

    ir_instr_operand_value(fdbad, val(e, "ptr[(x: i32)]"));
    ir_instr_operand_name(fdbad, "nope");
    check("an unknown member is rejected",
          apply(e, fdbad) == IR_RULE_REJECT);

    ir_instr_t *ix = op(e, IR_OP_INDEX);

    ir_instr_operand_value(ix, val(e, "ptr[array[i32; 4]]"));
    ir_instr_operand_value(ix, val(e, "u32"));
    check("index yields a pointer to the element",
          accepts_as(e, ix, "ptr[i32]"));

    ir_instr_t *ixbad = op(e, IR_OP_INDEX);

    ir_instr_operand_value(ixbad, val(e, "ptr[array[i32; 4]]"));
    ir_instr_operand_value(ixbad, val(e, "f64"));
    check("a float index is rejected", apply(e, ixbad) == IR_RULE_REJECT);

    ir_instr_t *of = op(e, IR_OP_OFFSET);

    ir_instr_operand_value(of, val(e, "ptr[i32]"));
    ir_instr_operand_value(of, val(e, "u32"));
    check("offset keeps the pointer type", accepts_as(e, of, "ptr[i32]"));

    ir_instr_t *ofbad = op(e, IR_OP_OFFSET);

    ir_instr_operand_value(ofbad, val(e, "ref[i32]"));
    ir_instr_operand_value(ofbad, val(e, "u32"));
    check("offset on a non-indexable reference is rejected",
          apply(e, ofbad) == IR_RULE_REJECT);

    ir_instr_t *df = op(e, IR_OP_DIFF);

    ir_instr_operand_value(df, val(e, "ptr[i32]"));
    ir_instr_operand_value(df, val(e, "ptr[i32]"));
    check("diff yields a signed integer", accepts_as(e, df, "i64"));

    ir_instr_t *cp = op(e, IR_OP_MEMCPY);

    ir_instr_operand_value(cp, val(e, "ptr[i32]"));
    ir_instr_operand_value(cp, val(e, "ptr[i32]"));
    ir_instr_operand_value(cp, val(e, "u64"));
    check("memcpy yields nothing", accepts_as(e, cp, nullptr));

    ir_instr_t *mv = op(e, IR_OP_MEMMOVE);

    ir_instr_operand_value(mv, val(e, "ptr[i32]"));
    ir_instr_operand_value(mv, val(e, "ptr[i32]"));
    ir_instr_operand_value(mv, val(e, "u64"));
    check("memmove yields nothing", accepts_as(e, mv, nullptr));

    ir_instr_t *ms = op(e, IR_OP_MEMSET);

    ir_instr_operand_value(ms, val(e, "ptr[i32]"));
    ir_instr_operand_value(ms, val(e, "u8"));
    ir_instr_operand_value(ms, val(e, "u64"));
    check("memset yields nothing", accepts_as(e, ms, nullptr));

    ir_instr_t *mc = op(e, IR_OP_MEMCMP);

    ir_instr_operand_value(mc, val(e, "ptr[i32]"));
    ir_instr_operand_value(mc, val(e, "ptr[i32]"));
    ir_instr_operand_value(mc, val(e, "u64"));
    check("memcmp yields a 32-bit signed integer",
          accepts_as(e, mc, "i32"));

    ir_instr_t *msbad = op(e, IR_OP_MEMSET);

    ir_instr_operand_value(msbad, val(e, "ptr[i32]"));
    ir_instr_operand_value(msbad, val(e, "u32"));
    ir_instr_operand_value(msbad, val(e, "u64"));
    check("memset fills with a byte, not a wider integer",
          apply(e, msbad) == IR_RULE_REJECT);

    ir_instr_t *cpbad = op(e, IR_OP_MEMCPY);

    ir_instr_operand_value(cpbad, val(e, "ptr[i32]"));
    ir_instr_operand_value(cpbad, val(e, "u64"));
    ir_instr_operand_value(cpbad, val(e, "u64"));
    check("memcpy needs a pointer source",
          apply(e, cpbad) == IR_RULE_REJECT);

    ir_instr_t *lv = op(e, IR_OP_LOAD);

    check("load takes the volatile access modifier",
          ir_instr_add_mod(lv, "volatile"));
    ir_instr_operand_value(lv, val(e, "ptr[i32]"));
    check("and an access modifier does not change the loaded type",
          accepts_as(e, lv, "i32"));

    ir_instr_t *sv = op(e, IR_OP_STORE);

    check("store takes the atomic access modifier",
          ir_instr_add_mod(sv, "atomic"));
    ir_instr_operand_value(sv, val(e, "ptr[i32]"));
    ir_instr_operand_value(sv, val(e, "i32"));
    check("and an access modifier leaves store with no result",
          accepts_as(e, sv, nullptr));

    ir_instr_t *av = op(e, IR_OP_ADD);

    check("an access modifier is not accepted where there is no access",
          !ir_instr_add_mod(av, "volatile"));

    ir_instr_t *ac = op(e, IR_OP_ALLOCA);

    ir_instr_operand_type(ac, ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(ac, val(e, "u64"));
    check("alloca takes an optional count", accepts_as(e, ac, "ptr[i32]"));

    ir_instr_t *acbad = op(e, IR_OP_ALLOCA);

    ir_instr_operand_type(acbad, ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(acbad, val(e, "f64"));
    check("an alloca count must be an integer",
          apply(e, acbad) == IR_RULE_REJECT);

    ir_type_define(e->m, ir_type_parse(e->m, "Point"),
                   ir_type_parse(e->m, "(x: i32, y: u64)"));

    ir_instr_t *nf = op(e, IR_OP_FIELD);

    ir_instr_operand_value(nf, val(e, "ptr[Point]"));
    ir_instr_operand_name(nf, "y");
    check("field resolves a named type through the module's type table",
          accepts_as(e, nf, "ptr[u64]"));

    ir_instr_t *nfbad = op(e, IR_OP_FIELD);

    ir_instr_operand_value(nfbad, val(e, "ptr[Widget]"));
    ir_instr_operand_name(nfbad, "y");
    check("a named type with no definition has no members",
          apply(e, nfbad) == IR_RULE_REJECT);

    ir_instr_t *ixseq = op(e, IR_OP_INDEX);

    ir_instr_operand_value(ixseq, val(e, "ref[array[i32; 4]]"));
    ir_instr_operand_value(ixseq, val(e, "u32"));
    check("a sequence is indexable however it is referenced",
          accepts_as(e, ixseq, "ref[i32]"));

    ir_instr_t *ixref = op(e, IR_OP_INDEX);

    ir_instr_operand_value(ixref, val(e, "ref[i32]"));
    ir_instr_operand_value(ixref, val(e, "u32"));
    check("a non-sequence needs an indexable indirection",
          apply(e, ixref) == IR_RULE_REJECT);

    close_env(e);
}

static void test_aggregates_and_variants(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *ex = op(e, IR_OP_EXTRACT);

    ir_instr_operand_value(ex, val(e, "(x: i32, y: u64)"));
    ir_instr_operand_name(ex, "x");
    check("extract yields the member type", accepts_as(e, ex, "i32"));

    ir_instr_t *pos = op(e, IR_OP_EXTRACT_INDEX);

    ir_instr_operand_value(pos, val(e, "t(i32, u64)"));
    ir_instr_operand_uint(pos, 1);
    check("extract.index yields an ordered tuple element",
          accepts_as(e, pos, "u64"));

    ir_instr_t *posbad = op(e, IR_OP_EXTRACT_INDEX);

    ir_instr_operand_value(posbad, val(e, "t(i32, u64)"));
    ir_instr_operand_uint(posbad, 2);
    check("extract.index rejects an out-of-range tuple element",
          apply(e, posbad) == IR_RULE_REJECT);

    ir_instr_t *ins = op(e, IR_OP_INSERT);

    ir_instr_operand_value(ins, val(e, "(x: i32)"));
    ir_instr_operand_name(ins, "x");
    ir_instr_operand_value(ins, val(e, "i32"));
    check("insert yields the aggregate", accepts_as(e, ins, "(x: i32)"));

    ir_instr_t *insbad = op(e, IR_OP_INSERT);

    ir_instr_operand_value(insbad, val(e, "(x: i32)"));
    ir_instr_operand_name(insbad, "x");
    ir_instr_operand_value(insbad, val(e, "u64"));
    check("inserting the wrong member type is rejected",
          apply(e, insbad) == IR_RULE_REJECT);

    ir_instr_t *vn = op(e, IR_OP_VARIANT_NEW);
    ir_instr_set_dest(vn, "empty", ir_type_parse(e->m, "variant R[i32]"));
    check("variant.new yields an unset value of its destination type",
          accepts_as(e, vn, "variant R[i32]"));

    ir_instr_t *vnu = op(e, IR_OP_VARIANT_NEW_USING);
    ir_instr_set_dest(vnu, "empty_using_allocator",
                      ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_operand_value(vnu, val(e, "Allocator"));
    check("variant.new.using yields unset storage with its allocator",
          accepts_as(e, vnu, "variant R[i32]"));

    ir_instr_t *vnbad = op(e, IR_OP_VARIANT_NEW);
    ir_instr_set_dest(vnbad, "not_variant", ir_type_parse(e->m, "i32"));
    check("variant.new rejects a non-variant destination",
          apply(e, vnbad) == IR_RULE_REJECT);

    ir_instr_t *vm = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vm, "r", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_operand_name(vm, "ok");
    ir_instr_operand_value(vm, val(e, "i32"));
    check("variant.make takes its type from its destination",
          accepts_as(e, vm, "variant R[i32]"));

    ir_instr_t *vmbad = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vmbad, "s", ir_type_parse(e->m, "R[i32]"));
    ir_instr_operand_name(vmbad, "ok");
    ir_instr_operand_value(vmbad, val(e, "i32"));
    check("a non-variant destination is rejected",
          apply(e, vmbad) == IR_RULE_REJECT);

    ir_instr_t *vt = op(e, IR_OP_VARIANT_TAG);

    ir_instr_operand_value(vt, val(e, "variant R[i32]"));
    check("variant.tag yields a 64-bit unsigned integer",
          accepts_as(e, vt, "u64"));

    ir_instr_t *vtbad = op(e, IR_OP_VARIANT_TAG);

    ir_instr_operand_value(vtbad, val(e, "i32"));
    check("variant.tag on a non-variant is rejected",
          apply(e, vtbad) == IR_RULE_REJECT);

    ir_instr_t *option_is_nil = op(e, IR_OP_OPTION_IS_NIL);
    ir_instr_operand_value(option_is_nil, val(e, "option[i32]"));
    check("option.is_nil accepts an option and returns bool",
          accepts_as(e, option_is_nil, "bool"));

    ir_instr_t *option_is_nil_bad = op(e, IR_OP_OPTION_IS_NIL);
    ir_instr_operand_value(option_is_nil_bad, val(e, "i32"));
    check("option.is_nil rejects a non-option",
          apply(e, option_is_nil_bad) == IR_RULE_REJECT);

    ir_instr_t *nil_value = op(e, IR_OP_NIL);
    ir_instr_set_dest(nil_value, "nil_value", ir_type_parse(e->m, "nil"));
    check("nil produces the nil sentinel type", accepts_as(e, nil_value, "nil"));

    ir_instr_t *nil_value_bad = op(e, IR_OP_NIL);
    ir_instr_set_dest(nil_value_bad, "nil_value_bad",
                      ir_type_parse(e->m, "i32"));
    check("nil rejects a non-nil result type",
          apply(e, nil_value_bad) == IR_RULE_REJECT);

    ir_instr_t *option_none = op(e, IR_OP_OPTION_NONE);
    ir_instr_set_dest(option_none, "option_none",
                      ir_type_parse(e->m, "option[i32]"));
    check("option.none produces a contextual option value",
          accepts_as(e, option_none, "option[i32]"));

    ir_instr_t *option_some = op(e, IR_OP_OPTION_SOME);
    ir_instr_set_dest(option_some, "option_some",
                      ir_type_parse(e->m, "option[i32]"));
    ir_instr_operand_value(option_some, val(e, "i32"));
    check("option.some wraps a value of the element type",
          accepts_as(e, option_some, "option[i32]"));

    ir_instr_t *option_some_bad = op(e, IR_OP_OPTION_SOME);
    ir_instr_set_dest(option_some_bad, "option_some_bad",
                      ir_type_parse(e->m, "option[i32]"));
    ir_instr_operand_value(option_some_bad, val(e, "string"));
    check("option.some rejects a mismatched payload",
          apply(e, option_some_bad) == IR_RULE_REJECT);

    ir_instr_t *option_get = op(e, IR_OP_OPTION_GET);
    ir_instr_set_dest(option_get, "option_value", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(option_get, val(e, "option[i32]"));
    check("option.get yields the option element type",
          accepts_as(e, option_get, "i32"));

    ir_instr_t *option_get_bad = op(e, IR_OP_OPTION_GET);
    ir_instr_set_dest(option_get_bad, "option_value_bad",
                      ir_type_parse(e->m, "string"));
    ir_instr_operand_value(option_get_bad, val(e, "option[i32]"));
    check("option.get rejects a mismatched destination type",
          apply(e, option_get_bad) == IR_RULE_REJECT);

    ir_instr_t *option_get_or_else = op(e, IR_OP_OPTION_GET_OR_ELSE);
    ir_instr_operand_value(option_get_or_else, val(e, "option[i32]"));
    ir_instr_operand_value(option_get_or_else, val(e, "i32"));
    check("option.get_or_else accepts a matching fallback",
          accepts_as(e, option_get_or_else, "i32"));

    ir_instr_t *option_get_or_else_bad = op(e, IR_OP_OPTION_GET_OR_ELSE);
    ir_instr_operand_value(option_get_or_else_bad, val(e, "option[i32]"));
    ir_instr_operand_value(option_get_or_else_bad, val(e, "string"));
    check("option.get_or_else rejects a mismatched fallback",
          apply(e, option_get_or_else_bad) == IR_RULE_REJECT);

    ir_instr_t *atomic_load = op(e, IR_OP_LOAD);
    ir_instr_operand_value(atomic_load, val(e, "ref[atomic[i32]]"));
    ir_instr_add_mod(atomic_load, "atomic");
    check("atomic load unwraps the atomic storage type",
          accepts_as(e, atomic_load, "i32"));

    ir_instr_t *atomic_store = op(e, IR_OP_STORE);
    ir_instr_operand_value(atomic_store, val(e, "ref[atomic[i32]]"));
    ir_instr_operand_value(atomic_store, val(e, "i32"));
    ir_instr_add_mod(atomic_store, "atomic");
    check("atomic store accepts the underlying value type",
          apply(e, atomic_store) == IR_RULE_ACCEPT);

    ir_instr_t *atomic_wrap = op(e, IR_OP_ATOMIC_WRAP);
    ir_instr_set_dest(atomic_wrap, "atomic_wrap",
                      ir_type_parse(e->m, "atomic[i32]"));
    ir_instr_operand_value(atomic_wrap, val(e, "i32"));
    check("atomic.wrap converts an underlying value to its atomic type",
          accepts_as(e, atomic_wrap, "atomic[i32]"));

    ir_instr_t *atomic_wrap_bad = op(e, IR_OP_ATOMIC_WRAP);
    ir_instr_set_dest(atomic_wrap_bad, "atomic_wrap_bad",
                      ir_type_parse(e->m, "atomic[i32]"));
    ir_instr_operand_value(atomic_wrap_bad, val(e, "string"));
    check("atomic.wrap rejects a mismatched underlying value",
          apply(e, atomic_wrap_bad) == IR_RULE_REJECT);

    ir_instr_t *tt = op(e, IR_OP_TYPE_TEST);
    ir_instr_operand_value(tt, val(e, "i32 | string"));
    ir_instr_operand_type(tt, ir_type_parse(e->m, "i32"));
    check("type.test accepts a member type of a union",
          accepts_as(e, tt, "bool"));

    ir_instr_t *tn = op(e, IR_OP_TYPE_NARROW);
    ir_instr_set_dest(tn, "narrowed", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(tn, val(e, "i32 | string"));
    ir_instr_operand_type(tn, ir_type_parse(e->m, "i32"));
    check("type.narrow extracts the requested union member",
          accepts_as(e, tn, "i32"));

    th_type *dynamic_type = th_var(ir_ctx_types(e->ctx));
    ir_value_t *dynamic_value = ir_param_add(e->f, "dynamic", dynamic_type);
    ir_instr_t *dynamic_test = op(e, IR_OP_TYPE_TEST);
    ir_instr_operand_value(dynamic_test, dynamic_value);
    ir_instr_operand_type(dynamic_test, ir_type_parse(e->m, "string"));
    check("type.test accepts a concrete member of a constrained value",
          accepts_as(e, dynamic_test, "bool"));

    ir_instr_t *dynamic_narrow = op(e, IR_OP_TYPE_NARROW);
    ir_instr_set_dest(dynamic_narrow, "dynamic_narrowed",
                      ir_type_parse(e->m, "string"));
    ir_instr_operand_value(dynamic_narrow, dynamic_value);
    ir_instr_operand_type(dynamic_narrow, ir_type_parse(e->m, "string"));
    check("type.narrow extracts a concrete constrained value",
          accepts_as(e, dynamic_narrow, "string"));

    ir_instr_t *tnbad = op(e, IR_OP_TYPE_NARROW);
    ir_instr_set_dest(tnbad, "bad_narrow", ir_type_parse(e->m, "u64"));
    ir_instr_operand_value(tnbad, val(e, "i32 | string"));
    ir_instr_operand_type(tnbad, ir_type_parse(e->m, "u64"));
    check("type.narrow rejects a type outside the source union",
          apply(e, tnbad) == IR_RULE_REJECT);

    ir_instr_t *vp = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(vp, "p", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(vp, val(e, "variant R[i32]"));
    ir_instr_operand_name(vp, "ok");
    check("variant.payload takes its arm type from its destination",
          accepts_as(e, vp, "i32"));

    ir_type_define(e->m, ir_type_parse(e->m, "variant R[i32]"),
                   ir_type_parse(e->m, "(ok: i32, err: u64)"));

    ir_type_define(e->m, ir_type_parse(e->m, "NamedPayload"),
                   ir_type_parse(e->m, "(amount: i32)"));
    ir_type_define(e->m, ir_type_parse(e->m, "variant NamedWrap[i32]"),
                   ir_type_parse(e->m, "(number: NamedPayload)"));
    check("named variant payload definition is retained",
          ir_type_definition(e->m,
              ir_type_parse(e->m, "NamedPayload")) != NULL);

    ir_instr_t *vrecord = op(e, IR_OP_VARIANT_MAKE);
    ir_instr_set_dest(vrecord, "wrapped_record",
                      ir_type_parse(e->m, "variant NamedWrap[i32]"));
    ir_instr_operand_name(vrecord, "number");
    ir_instr_operand_value(vrecord, val(e, "(amount: i32)"));
    check("variant.make accepts a matching named-field record literal",
          accepts_as(e, vrecord, "variant NamedWrap[i32]"));

    ir_instr_t *vd = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vd, "rd", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_operand_name(vd, "ok");
    ir_instr_operand_value(vd, val(e, "i32"));
    check("a declared arm and a matching payload are accepted",
          accepts_as(e, vd, "variant R[i32]"));

    ir_instr_t *vtypo = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vtypo, "rt", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_operand_name(vtypo, "okk");
    ir_instr_operand_value(vtypo, val(e, "i32"));
    check("a misspelled arm name is caught once the variant is declared",
          apply(e, vtypo) == IR_RULE_REJECT);

    ir_instr_t *vpay = op(e, IR_OP_VARIANT_MAKE);

    ir_instr_set_dest(vpay, "rp", ir_type_parse(e->m, "variant R[i32]"));
    ir_instr_operand_name(vpay, "err");
    ir_instr_operand_value(vpay, val(e, "i32"));
    check("and so is a payload that is not what the arm carries",
          apply(e, vpay) == IR_RULE_REJECT);

    ir_instr_t *pd = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(pd, "pd", ir_type_parse(e->m, "u64"));
    ir_instr_operand_value(pd, val(e, "variant R[i32]"));
    ir_instr_operand_name(pd, "err");
    check("variant.payload agrees with the declared arm",
          accepts_as(e, pd, "u64"));

    ir_instr_t *pbad = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(pbad, "pb", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(pbad, val(e, "variant R[i32]"));
    ir_instr_operand_name(pbad, "err");
    check("and is rejected when it does not",
          apply(e, pbad) == IR_RULE_REJECT);

    ir_instr_t *ptypo = op(e, IR_OP_VARIANT_PAYLOAD);

    ir_instr_set_dest(ptypo, "pt", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(ptypo, val(e, "variant R[i32]"));
    ir_instr_operand_name(ptypo, "nope");
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
    ir_instr_operand_value(agn, val(e, "i32"));
    check("an aggregate needs one value per member",
          apply(e, agn) == IR_RULE_REJECT);

    ir_instr_t *ago = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(ago, "ao", ir_type_parse(e->m, "(x: i32, y: u64)"));
    ir_instr_operand_value(ago, val(e, "u64"));
    ir_instr_operand_value(ago, val(e, "i32"));
    check("aggregate members are matched in typehash order",
          apply(e, ago) == IR_RULE_REJECT);

    ir_instr_t *agg = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(agg, "ag2", ir_type_parse(e->m, "(x: i32, y: u64)"));
    ir_instr_operand_value(agg, val(e, "i32"));
    ir_instr_operand_value(agg, val(e, "u64"));
    check("and in that order they match",
          accepts_as(e, agg, "(x: i32, y: u64)"));

    ir_instr_t *tuple = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(tuple, "tuple", ir_type_parse(e->m, "t(i32, u64)"));
    ir_instr_operand_value(tuple, val(e, "i32"));
    ir_instr_operand_value(tuple, val(e, "u64"));
    check("an aggregate constructs a tuple with positional element types",
          accepts_as(e, tuple, "t(i32, u64)"));

    ir_instr_t *bad_tuple = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(bad_tuple, "bad_tuple",
                      ir_type_parse(e->m, "t(i32, u64)"));
    ir_instr_operand_value(bad_tuple, val(e, "u64"));
    ir_instr_operand_value(bad_tuple, val(e, "i32"));
    check("tuple aggregate elements are checked positionally",
          apply(e, bad_tuple) == IR_RULE_REJECT);

    ir_instr_t *array = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(array, "array", ir_type_parse(e->m, "array[i32; 2]"));
    ir_instr_operand_value(array, val(e, "i32"));
    ir_instr_operand_value(array, val(e, "i32"));
    check("an aggregate constructs a fixed-extent sequence",
          accepts_as(e, array, "array[i32; 2]"));

    ir_instr_t *array_bad_extent = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(array_bad_extent, "array_bad_extent",
                      ir_type_parse(e->m, "array[i32; 2]"));
    ir_instr_operand_value(array_bad_extent, val(e, "i32"));
    check("a fixed-extent sequence aggregate checks its extent",
          apply(e, array_bad_extent) == IR_RULE_REJECT);

    ir_instr_t *array_bad_element = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(array_bad_element, "array_bad_element",
                      ir_type_parse(e->m, "array[i32]"));
    ir_instr_operand_value(array_bad_element, val(e, "u64"));
    check("a sequence aggregate checks each element type",
          apply(e, array_bad_element) == IR_RULE_REJECT);

    ir_instr_t *bytes = op(e, IR_OP_CONST);
    const uint8_t byte_payload[] = {0, 'x', 0xff};
    ir_instr_set_dest(bytes, "bytes", ir_type_parse(e->m, "string"));
    bool byte_form_set = ir_instr_set_const_bytes(bytes, byte_payload,
                                                   sizeof(byte_payload));
    size_t byte_length = 0;
    const char *byte_hex = ir_instr_const_bytes_hex(bytes, &byte_length);
    check("byte constants preserve embedded zeroes in their IR payload",
          byte_form_set && byte_hex && !strcmp(byte_hex, "0078ff")
              && byte_length == sizeof(byte_payload)
              && accepts_as(e, bytes, "string"));

    ir_instr_t *elements = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(elements, "elements",
                      ir_type_parse(e->m, "list[i32]"));
    ir_instr_operand_value(elements, val(e, "i32"));
    ir_instr_operand_value(elements, val(e, "i32"));
    check("a typed element aggregate accepts matching values",
          ir_instr_set_aggregate_form(elements, IR_AGGREGATE_ELEMENTS)
              && accepts_as(e, elements, "list[i32]"));

    ir_instr_t *bad_elements = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(bad_elements, "bad_elements",
                      ir_type_parse(e->m, "list[i32]"));
    ir_instr_operand_value(bad_elements, val(e, "u64"));
    check("a typed element aggregate rejects mismatched values",
          ir_instr_set_aggregate_form(bad_elements, IR_AGGREGATE_ELEMENTS)
              && apply(e, bad_elements) == IR_RULE_REJECT);

    ir_instr_t *pairs = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(pairs, "pairs",
                      ir_type_parse(e->m, "map[i32, u64]"));
    ir_instr_operand_value(pairs, val(e, "i32"));
    ir_instr_operand_value(pairs, val(e, "u64"));
    check("a pair aggregate verifies alternating key and value types",
          ir_instr_set_aggregate_form(pairs, IR_AGGREGATE_PAIRS)
              && accepts_as(e, pairs, "map[i32, u64]"));

    ir_instr_t *odd_pairs = op(e, IR_OP_AGGREGATE);
    ir_instr_set_dest(odd_pairs, "odd_pairs",
                      ir_type_parse(e->m, "map[i32, u64]"));
    ir_instr_operand_value(odd_pairs, val(e, "i32"));
    check("a pair aggregate rejects a missing value",
          ir_instr_set_aggregate_form(odd_pairs, IR_AGGREGATE_PAIRS)
              && apply(e, odd_pairs) == IR_RULE_REJECT);

    th_type *array_new_type = ir_type_parse(e->m,
                                             "array[array[i32]]");
    ir_value_t *outer_extent = val(e, "u64");
    ir_value_t *inner_extent = val(e, "u64");
    ir_value_t *element_count = val(e, "u64");
    ir_value_t *outer_stride = val(e, "u64");
    ir_value_t *inner_stride = val(e, "u64");
    ir_shape_t *array_new_shape = ir_shape_new(e->ctx,
        ir_type_parse(e->m, "i32"), 2);
    bool array_new_shape_ok = array_new_shape && element_count
        && outer_stride && inner_stride
        && ir_shape_set_extent(array_new_shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_RUNTIME,
            .value = outer_extent,
        })
        && ir_shape_set_extent(array_new_shape, 1, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_RUNTIME,
            .value = inner_extent,
        })
        && ir_shape_set_element_count(array_new_shape,
            (ir_shape_measure_t){
                .kind = IR_SHAPE_MEASURE_RUNTIME,
                .value = element_count,
            })
        && ir_shape_set_stride(array_new_shape, 0, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_RUNTIME,
            .value = outer_stride,
        })
        && ir_shape_set_stride(array_new_shape, 1, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_RUNTIME,
            .value = inner_stride,
        })
        && ir_shape_set_storage(array_new_shape, 2, true);
    ir_instr_t *array_new = op(e, IR_OP_ARRAY_NEW);
    bool array_new_built = array_new_type && outer_extent && inner_extent
        && array_new_shape_ok
        && ir_instr_set_dest(array_new, "created", array_new_type)
        && ir_instr_set_array_init(e->ctx, array_new,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO)
        && ir_instr_operand_value(array_new, outer_extent)
        && ir_instr_operand_value(array_new, inner_extent)
        && ir_value_set_shape(array_new->dest, array_new_shape);
    check("array.new accepts runtime extents matching its shape",
          array_new_built
              && accepts_as(e, array_new, "array[array[i32]]"));
    ir_instr_t *array_new_using = op(e, IR_OP_ARRAY_NEW_USING);
    bool array_new_using_built = array_new_type && outer_extent
        && inner_extent && array_new_shape_ok
        && ir_instr_set_dest(array_new_using, "created_using_allocator",
                             array_new_type)
        && ir_instr_set_array_init(e->ctx, array_new_using,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO)
        && ir_instr_operand_value(array_new_using, val(e, "Allocator"))
        && ir_instr_operand_value(array_new_using, outer_extent)
        && ir_instr_operand_value(array_new_using, inner_extent)
        && ir_value_set_shape(array_new_using->dest, array_new_shape);
    check("array.new.using preserves its allocator and shape extents",
          array_new_using_built
              && accepts_as(e, array_new_using, "array[array[i32]]"));
    ir_array_init_t array_init = IR_ARRAY_INIT_UNINITIALIZED;
    check("array.new retains its typed element initialization policy",
          array_new_built
              && ir_instr_array_init(array_new, &array_init)
              && array_init == IR_ARRAY_INIT_LANGUAGE_ZERO);
    ir_instr_t *array_new_missing_init = op(e, IR_OP_ARRAY_NEW);
    bool array_new_missing_init_built = array_new_type && outer_extent
        && inner_extent && array_new_shape_ok
        && ir_instr_set_dest(array_new_missing_init, "created_without_init",
                             array_new_type)
        && ir_instr_operand_value(array_new_missing_init, outer_extent)
        && ir_instr_operand_value(array_new_missing_init, inner_extent)
        && ir_value_set_shape(array_new_missing_init->dest, array_new_shape);
    check("array.new requires an element initialization policy",
          array_new_missing_init_built
              && apply(e, array_new_missing_init) == IR_RULE_REJECT);
    ir_instr_t *array_new_bad_init = op(e, IR_OP_ARRAY_NEW);
    bool array_new_bad_init_built = array_new_type && outer_extent
        && inner_extent && array_new_shape_ok
        && ir_instr_set_dest(array_new_bad_init, "created_with_bad_init",
                             array_new_type)
        && ir_annot_set(e->ctx, &array_new_bad_init->annots, (ir_annot_t){
            .key = "ir.array.element_initialization",
            .kind = IR_ANNOT_STR,
            .s = "unknown",
        })
        && ir_instr_operand_value(array_new_bad_init, outer_extent)
        && ir_instr_operand_value(array_new_bad_init, inner_extent)
        && ir_value_set_shape(array_new_bad_init->dest, array_new_shape);
    check("array.new rejects an unknown element initialization policy",
          array_new_bad_init_built
              && apply(e, array_new_bad_init) == IR_RULE_REJECT);

    ir_shape_t *expected_shape = ir_shape_new(e->ctx,
        ir_type_parse(e->m, "i32"), 1);
    bool expected_shape_ok = expected_shape
        && ir_shape_set_extent(expected_shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = 2,
        });
    ir_value_t *shaped_sequence = val(e, "array[i32]");
    ir_shape_t *actual_shape = ir_shape_new(e->ctx,
        ir_type_parse(e->m, "i32"), 1);
    bool actual_shape_ok = actual_shape && outer_extent
        && ir_shape_set_extent(actual_shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_RUNTIME,
            .value = outer_extent,
        })
        && ir_value_set_shape(shaped_sequence, actual_shape);
    ir_instr_t *shape_check = op(e, IR_OP_SEQUENCE_SHAPE_CHECK);
    bool shape_check_built = expected_shape_ok && actual_shape_ok
        && ir_instr_operand_value(shape_check, shaped_sequence)
        && ir_instr_set_shape_constraint(shape_check, expected_shape);
    check("sequence.shape.check accepts a runtime sequence with a required shape",
          shape_check_built && apply(e, shape_check) == IR_RULE_ACCEPT);

    ir_instr_t *shape_check_missing = op(e, IR_OP_SEQUENCE_SHAPE_CHECK);
    bool shape_check_missing_built = actual_shape_ok
        && ir_instr_operand_value(shape_check_missing, shaped_sequence);
    check("sequence.shape.check requires an expected shape",
          shape_check_missing_built
              && apply(e, shape_check_missing) == IR_RULE_REJECT);

    ir_value_t *unshaped_sequence = val(e, "array[i32]");
    ir_instr_t *shape_check_unshaped = op(e, IR_OP_SEQUENCE_SHAPE_CHECK);
    bool shape_check_unshaped_built = expected_shape_ok && unshaped_sequence
        && ir_instr_operand_value(shape_check_unshaped, unshaped_sequence)
        && ir_instr_set_shape_constraint(shape_check_unshaped,
                                         expected_shape);
    check("sequence.shape.check accepts a runtime value without static extents",
          shape_check_unshaped_built
              && apply(e, shape_check_unshaped) == IR_RULE_ACCEPT);

    ir_shape_t *archived_shape = ir_shape_new(e->ctx,
        ir_type_parse(e->m, "i32"), 1);
    bool archived_shape_ok = archived_shape
        && ir_shape_set_extent(archived_shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_COMPTIME,
            .producer_identifier = "dep.__array_extent_0",
        });
    ir_instr_t *archived_shape_check = op(e, IR_OP_SEQUENCE_SHAPE_CHECK);
    bool archived_shape_check_built = archived_shape_ok
        && unshaped_sequence
        && ir_instr_operand_value(archived_shape_check, unshaped_sequence)
        && ir_instr_set_shape_constraint(archived_shape_check,
                                         archived_shape);
    check("sequence.shape.check accepts a deferred external producer identity",
          archived_shape_check_built
              && apply(e, archived_shape_check) == IR_RULE_ACCEPT);

    th_type *bound_signature = ir_type_parse(e->m, "() -> u64");
    ir_func_t *bound_producer = bound_signature
        ? ir_func_add(e->m, "array_bound", bound_signature,
                      IR_LINK_INTERNAL) : nullptr;
    bool bound_producer_ok = bound_producer
        && ir_func_set_execution_phase(e->ctx, bound_producer,
                                       IR_PHASE_COMPTIME);
    ir_shape_t *comptime_shape = ir_shape_new(e->ctx,
        ir_type_parse(e->m, "i32"), 1);
    bool comptime_shape_ok = bound_producer_ok && comptime_shape
        && element_count && inner_stride
        && ir_shape_set_extent(comptime_shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_COMPTIME,
            .value = outer_extent,
            .producer = bound_producer,
        })
        && ir_shape_set_element_count(comptime_shape,
            (ir_shape_measure_t){
                .kind = IR_SHAPE_MEASURE_RUNTIME,
                .value = element_count,
            })
        && ir_shape_set_stride(comptime_shape, 0, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_RUNTIME,
            .value = inner_stride,
        })
        && ir_shape_set_storage(comptime_shape, 1, true);
    ir_instr_t *array_new_comptime = op(e, IR_OP_ARRAY_NEW);
    bool array_new_comptime_built = comptime_shape_ok
        && ir_instr_set_dest(array_new_comptime, "created_from_comptime",
                             ir_type_parse(e->m, "array[i32]"))
        && ir_instr_set_array_init(e->ctx, array_new_comptime,
                                   IR_ARRAY_INIT_UNINITIALIZED)
        && ir_instr_operand_value(array_new_comptime, outer_extent)
        && ir_value_set_shape(array_new_comptime->dest, comptime_shape);
    check("array.new retains a full-comptime extent producer",
          array_new_comptime_built
              && accepts_as(e, array_new_comptime, "array[i32]"));

    ir_instr_t *array_new_wrong_element = op(e, IR_OP_ARRAY_NEW);
    bool array_new_wrong_element_built = comptime_shape_ok && outer_extent
        && ir_instr_set_dest(array_new_wrong_element,
                             "created_with_wrong_element",
                             ir_type_parse(e->m, "array[u64]"))
        && ir_instr_set_array_init(e->ctx, array_new_wrong_element,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO)
        && ir_instr_operand_value(array_new_wrong_element, outer_extent)
        && ir_value_set_shape(array_new_wrong_element->dest, comptime_shape);
    check("array.new rejects shape metadata with the wrong leaf type",
          array_new_wrong_element_built
              && apply(e, array_new_wrong_element) == IR_RULE_REJECT);

    ir_shape_t *missing_value_shape = ir_shape_new(e->ctx,
        ir_type_parse(e->m, "i32"), 1);
    bool missing_value_shape_ok = bound_producer_ok && missing_value_shape
        && ir_shape_set_extent(missing_value_shape, 0,
            (ir_shape_extent_t){
                .kind = IR_SHAPE_EXTENT_COMPTIME,
                .producer = bound_producer,
            })
        && ir_shape_set_element_count(missing_value_shape,
            (ir_shape_measure_t){
                .kind = IR_SHAPE_MEASURE_RUNTIME,
                .value = element_count,
            })
        && ir_shape_set_stride(missing_value_shape, 0,
            (ir_shape_measure_t){
                .kind = IR_SHAPE_MEASURE_RUNTIME,
                .value = inner_stride,
            })
        && ir_shape_set_storage(missing_value_shape, 1, true);
    ir_instr_t *array_new_missing_extent = op(e, IR_OP_ARRAY_NEW);
    bool array_new_missing_extent_built = missing_value_shape_ok
        && ir_instr_set_dest(array_new_missing_extent,
                             "created_without_comptime_extent",
                             ir_type_parse(e->m, "array[i32]"))
        && ir_instr_set_array_init(e->ctx, array_new_missing_extent,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO)
        && ir_value_set_shape(array_new_missing_extent->dest,
                              missing_value_shape);
    check("array.new rejects a known extent without a graph value",
          array_new_missing_extent_built
              && apply(e, array_new_missing_extent) == IR_RULE_REJECT);

    ir_instr_t *array_new_missing_shape = op(e, IR_OP_ARRAY_NEW);
    bool array_new_missing_built = array_new_type
        && ir_instr_set_dest(array_new_missing_shape, "created_without_shape",
                             array_new_type)
        && ir_instr_set_array_init(e->ctx, array_new_missing_shape,
                                   IR_ARRAY_INIT_LANGUAGE_ZERO);
    check("array.new requires shape metadata",
          array_new_missing_built
              && apply(e, array_new_missing_shape) == IR_RULE_REJECT);

    ir_type_define(e->m, ir_type_parse(e->m, "Pair"),
                   ir_type_parse(e->m, "(a: i32, b: u64)"));

    ir_instr_t *nx = op(e, IR_OP_EXTRACT);

    ir_instr_operand_value(nx, val(e, "Pair"));
    ir_instr_operand_name(nx, "b");
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
    {"u128", "u64", true, false, "wide unsigned integer to narrower integer"},
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
        ir_instr_add_mod(in, m1);
    }

    if (m2) {
        ir_instr_add_mod(in, m2);
    }
    ir_instr_operand_value(in, val(e, c->from));

    return in;
}

static void test_conversion(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *enum_value = op(e, IR_OP_ENUM_VALUE);
    ir_instr_set_dest(enum_value, "enum_value", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(enum_value, val(e, "Color"));
    check("enum.value preserves a semantic enum-to-integer conversion",
          accepts_as(e, enum_value, "i32"));

    ir_instr_t *checked_enum_value = op(e, IR_OP_ENUM_VALUE);
    ir_instr_set_dest(checked_enum_value, "checked_enum_value",
                      ir_type_parse(e->m, "u8"));
    ir_instr_operand_value(checked_enum_value, val(e, "Color"));
    ir_instr_add_mod(checked_enum_value, "trap");
    check("enum.value accepts an explicit fit policy",
          accepts_as(e, checked_enum_value, "u8"));

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
    ir_instr_operand_value(widen_u, val(e, "u32"));
    check("zero extension is not spelled; the source type says so",
          accepts_as(e, widen_u, "u64"));

    ir_instr_t *widen_s = op(e, IR_OP_CAST);

    ir_instr_set_dest(widen_s, "ws", ir_type_parse(e->m, "i64"));
    ir_instr_operand_value(widen_s, val(e, "i32"));
    check("nor is sign extension", accepts_as(e, widen_s, "i64"));

    ir_instr_t *signed_to_unsigned = op(e, IR_OP_CAST);
    ir_instr_set_dest(signed_to_unsigned, "su", ir_type_parse(e->m, "u64"));
    ir_instr_operand_value(signed_to_unsigned, val(e, "i64"));
    check("signed-to-unsigned conversion requires an explicit fit policy",
          !accepts_as(e, signed_to_unsigned, "u64"));
    check("signed-to-unsigned conversion accepts a checked fit policy",
          ir_instr_add_mod(signed_to_unsigned, "trap")
              && accepts_as(e, signed_to_unsigned, "u64"));

    const conv_t bad = {"(x: i32)", "i32", false, false, "record to integer"};

    check("cast does not convert an aggregate to a scalar",
          apply(e, conv(e, &bad, nullptr, nullptr)) == IR_RULE_REJECT);

    const conv_t badp = {"ptr[i32]", "ptr[u8]", false, false, "pointer swap"};

    check("cast does not reinterpret one pointer as another",
          apply(e, conv(e, &badp, nullptr, nullptr)) == IR_RULE_REJECT);

    ir_instr_t *bc = op(e, IR_OP_BITCAST);

    ir_instr_set_dest(bc, "b", ir_type_parse(e->m, "u32"));
    ir_instr_operand_value(bc, val(e, "f32"));
    check("bitcast reinterprets instead", accepts_as(e, bc, "u32"));

    ir_instr_t *bcm = op(e, IR_OP_BITCAST);

    check("and bitcast takes no modifiers",
          !ir_instr_add_mod(bcm, "sat"));

    th_type *amount = ir_type_parse(e->m, "Amount");
    th_type *other = ir_type_parse(e->m, "OtherAmount");
    check("nominal cast types retain their declared representation",
          ir_type_define(e->m, amount, ir_type_parse(e->m, "i32"))
              && ir_type_define(e->m, other, ir_type_parse(e->m, "i32")));
    ir_instr_t *nominal = op(e, IR_OP_NOMINAL_CAST);
    ir_instr_set_dest(nominal, "amount", amount);
    ir_instr_operand_value(nominal, val(e, "i32"));
    check("explicit nominal conversion accepts the declared representation",
          accepts_as(e, nominal, "Amount"));
    ir_instr_t *representation = op(e, IR_OP_NOMINAL_CAST);
    ir_instr_set_dest(representation, "representation", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(representation, val(e, "Amount"));
    check("explicit nominal conversion can expose the representation",
          accepts_as(e, representation, "i32"));
    ir_instr_t *unrelated = op(e, IR_OP_NOMINAL_CAST);
    ir_instr_set_dest(unrelated, "unrelated", other);
    ir_instr_operand_value(unrelated, val(e, "Amount"));
    check("equal representation does not permit unrelated nominal conversions",
          apply(e, unrelated) == IR_RULE_REJECT);
    ir_instr_t *box = op(e, IR_OP_BOX);
    ir_instr_set_dest(box, "boxed", ir_type_parse(e->m, "Erased"));
    ir_value_t *source = val(e, "(x: i32)");
    ir_instr_operand_value(box, source);
    check("semantic boxing retains the source's type without an ABI",
          accepts_as(e, box, "Erased")
              && ir_operand_value(&box->operands[0]) == source);
    check("complete empty alias facts can be copied to a boxed value",
          ir_value_set_alias_targets(e->ctx, source, nullptr, 0, true)
              && ir_value_copy_alias_targets(e->ctx, box->dest, source));

    close_env(e);
}

static void test_select_phi_call(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *s = op(e, IR_OP_SELECT);

    ir_instr_operand_value(s, val(e, "bool"));
    ir_instr_operand_value(s, val(e, "i32"));
    ir_instr_operand_value(s, val(e, "i32"));
    check("select yields its arm type", accepts_as(e, s, "i32"));

    ir_instr_t *sbad = op(e, IR_OP_SELECT);

    ir_instr_operand_value(sbad, val(e, "i32"));
    ir_instr_operand_value(sbad, val(e, "i32"));
    ir_instr_operand_value(sbad, val(e, "i32"));
    check("select on a non-boolean is rejected",
          apply(e, sbad) == IR_RULE_REJECT);

    ir_instr_t *smix = op(e, IR_OP_SELECT);

    ir_instr_operand_value(smix, val(e, "bool"));
    ir_instr_operand_value(smix, val(e, "i32"));
    ir_instr_operand_value(smix, val(e, "u32"));
    check("select with mismatched arms is rejected",
          apply(e, smix) == IR_RULE_REJECT);

    ir_block_t *other = ir_block_add(e->f, "other");
    ir_instr_t *p     = op(e, IR_OP_PHI);

    ir_instr_set_dest(p, "ph", ir_type_parse(e->m, "i32"));
    ir_instr_operand_phi_arm(p, other, val(e, "i32"));
    check("phi takes its type from its destination",
          accepts_as(e, p, "i32"));

    ir_instr_t *pbad = op(e, IR_OP_PHI);

    ir_instr_set_dest(pbad, "ph2", ir_type_parse(e->m, "i32"));
    ir_instr_operand_phi_arm(pbad, other, val(e, "u64"));
    check("a phi arm of the wrong type is rejected",
          apply(e, pbad) == IR_RULE_REJECT);

    ir_func_t *add_requirement = ir_func_add(e->m, "add_requirement",
        ir_type_parse(e->m, "(i32, i32) -> i32"), IR_LINK_EXTERN);
    ir_instr_t *method = op(e, IR_OP_TRAIT_METHOD);
    ir_instr_set_dest(method, "selected_method",
                      ir_type_parse(e->m, "(i32, i32) -> i32"));
    ir_instr_operand_name(method, "Add");
    ir_instr_operand_name(method, "__add");
    ir_value_t *receiver = val(e, "i32");
    ir_instr_operand_value(method, receiver);
    ir_instr_operand_value(method,
                           ir_func_address(e->m, add_requirement));
    check("a semantic trait method retains its callable type",
          accepts_as(e, method, "(i32, i32) -> i32"));

    ir_instr_t *method_call = op(e, IR_OP_CALL);
    ir_instr_operand_value(method_call, method->dest);
    ir_instr_operand_value(method_call, receiver);
    ir_instr_operand_value(method_call, val(e, "i32"));
    check("the selected semantic trait method can be called",
          accepts_as(e, method_call, "i32"));

    ir_value_t *open_receiver = val(e, "`receiver");
    ir_instr_t *open_method = op(e, IR_OP_TRAIT_METHOD);
    ir_instr_set_dest(open_method, "open_receiver_method", method->dest->type);
    ir_instr_operand_name(open_method, "Add");
    ir_instr_operand_name(open_method, "__add");
    ir_instr_operand_value(open_method, open_receiver);
    ir_instr_operand_value(open_method,
                           ir_func_address(e->m, add_requirement));
    check("trait method checking uses copying unification",
          apply(e, open_method) == IR_RULE_ACCEPT
              && th_kind_of(th_resolve(open_receiver->type)) == TH_VAR);

    ir_func_t *optional_requirement = ir_func_add(e->m,
        "optional_requirement", ir_type_parse(e->m, "(i32, i32) -> i32"),
        IR_LINK_EXTERN);
    ir_func_set_parameter_optional(optional_requirement, 1, true);
    ir_instr_t *optional_method = op(e, IR_OP_TRAIT_METHOD);
    ir_instr_set_dest(optional_method, "optional_method",
                      ir_type_parse(e->m, "(i32, i32) -> i32"));
    ir_instr_operand_name(optional_method, "Add");
    ir_instr_operand_name(optional_method, "__add");
    ir_instr_operand_value(optional_method, receiver);
    ir_instr_operand_value(optional_method,
                           ir_func_address(e->m, optional_requirement));
    ir_instr_t *optional_method_call = op(e, IR_OP_CALL);
    ir_instr_operand_value(optional_method_call, optional_method->dest);
    ir_instr_operand_value(optional_method_call, receiver);
    ir_instr_operand_missing(optional_method_call);
    check("semantic trait calls retain requirement defaults",
          apply(e, optional_method) == IR_RULE_ACCEPT
              && accepts_as(e, optional_method_call, "i32"));

    th_type *fallible_method_type = ir_type_parse(e->m,
        "(i32, inout u64) -> i32 fail: string");
    ir_func_t *update_requirement = ir_func_add(e->m,
        "update_requirement", fallible_method_type, IR_LINK_EXTERN);
    ir_instr_t *fallible_method = op(e, IR_OP_TRAIT_METHOD);
    ir_instr_set_dest(fallible_method, "fallible_method",
                      fallible_method_type);
    ir_instr_operand_name(fallible_method, "Update");
    ir_instr_operand_name(fallible_method, "__update");
    ir_instr_operand_value(fallible_method, receiver);
    ir_instr_operand_value(fallible_method,
                           ir_func_address(e->m, update_requirement));
    th_type *method_outcome = ir_call_outcome_type(e->m,
                                                   fallible_method_type);
    ir_instr_t *fallible_method_call = op(e, IR_OP_CALL);
    ir_instr_set_dest(fallible_method_call, "fallible_method_outcome",
                      method_outcome);
    ir_instr_operand_value(fallible_method_call, fallible_method->dest);
    ir_instr_operand_value(fallible_method_call, receiver);
    ir_instr_operand_value(fallible_method_call, val(e, "u64"));
    ir_instr_t *method_success = op(e, IR_OP_VARIANT_PAYLOAD);
    ir_instr_set_dest(method_success, "fallible_method_success",
                      ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(method_success, fallible_method_call->dest);
    ir_instr_operand_name(method_success, "ok");
    ir_instr_t *method_copyout = op(e, IR_OP_CALL_OUTPUT);
    ir_instr_operand_value(method_copyout, method_success->dest);
    ir_instr_operand_uint(method_copyout, 0);
    ir_instr_set_dest(method_copyout, "method_copyout",
                      ir_type_parse(e->m, "u64"));
    check("semantic trait methods retain failure and copy-out channels",
          apply(e, fallible_method) == IR_RULE_ACCEPT
              && apply(e, fallible_method_call) == IR_RULE_ACCEPT
              && apply(e, method_success) == IR_RULE_ACCEPT
              && apply(e, method_copyout) == IR_RULE_ACCEPT);

    ir_instr_t *wrong_receiver = op(e, IR_OP_TRAIT_METHOD);
    ir_instr_set_dest(wrong_receiver, "wrong_method",
                      ir_type_parse(e->m, "(i32, i32) -> i32"));
    ir_instr_operand_name(wrong_receiver, "Add");
    ir_instr_operand_name(wrong_receiver, "__add");
    ir_instr_operand_value(wrong_receiver, val(e, "u64"));
    ir_instr_operand_value(wrong_receiver,
                           ir_func_address(e->m, add_requirement));
    check("trait method selection rejects an incompatible receiver",
          apply(e, wrong_receiver) == IR_RULE_REJECT);

    ir_func_t *g = ir_func_add(e->m, "g",
                               ir_type_parse(e->m, "(i32, x: u64) -> i64"),
                               IR_LINK_EXTERNAL);

    (void)g;

    ir_value_t *gv = ir_alloc(e->ctx, sizeof(ir_value_t));

    gv->kind = IR_VAL_FUNC;
    gv->name = "g";
    gv->type = ir_type_parse(e->m, "(i32, x: u64) -> i64");

    ir_instr_t *c = op(e, IR_OP_CALL);

    ir_instr_operand_value(c, gv);
    ir_instr_operand_value(c, val(e, "i32"));
    ir_instr_operand_named(c, "x", val(e, "u64"));
    check("call yields the callee's return type", accepts_as(e, c, "i64"));

    ir_value_t *fallible = ir_alloc(e->ctx, sizeof(ir_value_t));
    fallible->kind = IR_VAL_FUNC;
    fallible->name = "fallible";
    fallible->type = ir_type_parse(e->m,
                                   "(i32) -> i64 fail: string");
    th_type *outcome_type = ir_call_outcome_type(e->m, fallible->type);
    ir_instr_t *handled_call = op(e, IR_OP_CALL);
    ir_instr_set_dest(handled_call, "fallible_outcome", outcome_type);
    ir_instr_operand_value(handled_call, fallible);
    ir_instr_operand_value(handled_call, val(e, "i32"));
    ir_rule_result_t handled_result = ir_rule_apply(e->m, e->rs,
                                                    handled_call, e->d);
    check("fallible calls expose both channels through a tagged outcome",
          outcome_type && handled_result.verdict == IR_RULE_ACCEPT
              && ir_type_eq(e->m, handled_result.result, outcome_type));

    ir_instr_t *discarded_call = op(e, IR_OP_CALL);
    ir_instr_operand_value(discarded_call, fallible);
    ir_instr_operand_value(discarded_call, val(e, "i32"));
    check("a fallible call cannot discard its error channel",
          apply(e, discarded_call) == IR_RULE_REJECT);

    ir_instr_t *few = op(e, IR_OP_CALL);

    ir_instr_operand_value(few, gv);
    check("too few arguments is rejected", apply(e, few) == IR_RULE_REJECT);

    ir_instr_t *wrongkw = op(e, IR_OP_CALL);

    ir_instr_operand_value(wrongkw, gv);
    ir_instr_operand_value(wrongkw, val(e, "i32"));
    ir_instr_operand_named(wrongkw, "nope", val(e, "u64"));
    check("an unknown keyword parameter is rejected",
          apply(e, wrongkw) == IR_RULE_REJECT);

    ir_instr_t *wrongty = op(e, IR_OP_CALL);

    ir_instr_operand_value(wrongty, gv);
    ir_instr_operand_value(wrongty, val(e, "u64"));
    ir_instr_operand_named(wrongty, "x", val(e, "u64"));
    check("an argument of the wrong type is rejected",
          apply(e, wrongty) == IR_RULE_REJECT);

    ir_instr_t *ind = op(e, IR_OP_CALL);

    ir_instr_operand_value(ind, val(e, "ptr[(i32, x: u64) -> i64]"));
    ir_instr_operand_value(ind, val(e, "i32"));
    ir_instr_operand_named(ind, "x", val(e, "u64"));
    check("the same opcode calls through a pointer to a function",
          accepts_as(e, ind, "i64"));

    ir_instr_t *notfn = op(e, IR_OP_CALL);

    ir_instr_operand_value(notfn, val(e, "i32"));
    check("calling a non-function is rejected",
          apply(e, notfn) == IR_RULE_REJECT);

    ir_value_t *rv = ir_alloc(e->ctx, sizeof(ir_value_t));

    rv->kind = IR_VAL_FUNC;
    rv->name = "r";
    rv->type = ir_type_parse(e->m, "(i32, *u64) -> i64");

    ir_instr_t *rest = op(e, IR_OP_CALL);

    ir_instr_operand_value(rest, rv);
    ir_instr_operand_value(rest, val(e, "i32"));
    ir_instr_operand_value(rest, val(e, "u64"));
    ir_instr_operand_value(rest, val(e, "u64"));
    check("a rest parameter accepts any number of its element type",
          accepts_as(e, rest, "i64"));

    ir_instr_t *restbad = op(e, IR_OP_CALL);

    ir_instr_operand_value(restbad, rv);
    ir_instr_operand_value(restbad, val(e, "i32"));
    ir_instr_operand_value(restbad, val(e, "u64"));
    ir_instr_operand_value(restbad, val(e, "i8"));
    check("but a rest argument of the wrong element type is rejected",
          apply(e, restbad) == IR_RULE_REJECT);

    ir_func_t *optional_target = ir_func_add(e->m, "optional_target",
        ir_type_parse(e->m, "(i32, i64) -> i32"), IR_LINK_INTERNAL);
    ir_value_t *optional_first = optional_target
        ? ir_param_add(optional_target, "first", ir_type_parse(e->m, "i32"))
        : nullptr;
    ir_value_t *optional_second = optional_target
        ? ir_param_add(optional_target, "second", ir_type_parse(e->m, "i64"))
        : nullptr;
    bool optional_params = optional_first && optional_second
        && ir_param_set_optional(optional_second, true);
    ir_instr_t *omitted = op(e, IR_OP_CALL);
    bool omitted_built = optional_params
        && ir_instr_operand_value(omitted,
                                 ir_func_address(e->m, optional_target))
        && ir_instr_operand_value(omitted, val(e, "i32"))
        && ir_instr_operand_missing(omitted)
        && ir_instr_set_dest(omitted, "optional_call", ir_type_parse(e->m,
                                                       "i32"));
    check("a call may omit a defaultable parameter without a value wrapper",
          omitted_built && apply(e, omitted) == IR_RULE_ACCEPT);

    ir_instr_t *present = op(e, IR_OP_PARAM_PRESENT);
    bool present_built = optional_params
        && ir_instr_operand_value(present, optional_second)
        && ir_instr_set_dest(present, "second_present",
                             ir_type_parse(e->m, "bool"));
    check("the callee can query whether an optional parameter was supplied",
          present_built && apply(e, present) == IR_RULE_ACCEPT);

    ir_instr_t *required_missing = op(e, IR_OP_CALL);
    bool required_missing_built = optional_params
        && ir_instr_operand_value(required_missing,
                                  ir_func_address(e->m, optional_target))
        && ir_instr_operand_missing(required_missing)
        && ir_instr_set_dest(required_missing, "required_missing",
                             ir_type_parse(e->m, "i32"));
    check("a call cannot omit a required parameter",
          required_missing_built
              && apply(e, required_missing) == IR_RULE_REJECT);

    ir_func_t *rest_owner = ir_func_add(e->m, "rest_owner",
        ir_type_parse(e->m, "(*u64) -> void"), IR_LINK_INTERNAL);
    ir_value_t *rest_value = rest_owner
        ? ir_restparam_add(rest_owner, "values", ir_type_parse(e->m, "u64"))
        : nullptr;
    ir_instr_t *collect_rest = op(e, IR_OP_REST_COLLECT);
    bool collect_rest_built = rest_value && collect_rest
        && ir_instr_operand_value(collect_rest, rest_value)
        && ir_instr_set_dest(collect_rest, "collected_values",
                             ir_type_parse(e->m, "array[u64]"));
    check("rest collection yields an array of the variadic element type",
          collect_rest_built && apply(e, collect_rest) == IR_RULE_ACCEPT);

    ir_instr_t *collect_non_rest = op(e, IR_OP_REST_COLLECT);
    bool collect_non_rest_built = collect_non_rest
        && ir_instr_operand_value(collect_non_rest, val(e, "u64"))
        && ir_instr_set_dest(collect_non_rest, "not_a_rest_collection",
                             ir_type_parse(e->m, "array[u64]"));
    check("rest collection rejects an ordinary value",
          collect_non_rest_built
              && apply(e, collect_non_rest) == IR_RULE_REJECT);

    ir_value_t *ov = ir_alloc(e->ctx, sizeof(ir_value_t));

    ov->kind = IR_VAL_FUNC;
    ov->name = "o";
    ov->type = ir_type_parse(e->m, "(out i32, inout u64) -> void");

    ir_instr_t *outc = op(e, IR_OP_CALL);

    ir_instr_operand_value(outc, ov);
    ir_instr_operand_out(outc);
    ir_instr_operand_value(outc, val(e, "u64"));
    ir_instr_set_dest(outc, "out_result", ir_type_parse(e->m, "void"));
    check("out is an output slot and modify passes a value",
          ir_call_output_count(ov->type) == 2
              && ir_call_output_type(ov->type, 0)
              && ir_type_eq(e->m, ir_call_output_type(ov->type, 0),
                            ir_type_parse(e->m, "i32"))
              && apply(e, outc) == IR_RULE_ACCEPT);

    ir_instr_t *first_output = op(e, IR_OP_CALL_OUTPUT);
    ir_instr_operand_value(first_output, outc->dest);
    ir_instr_operand_uint(first_output, 0);
    ir_instr_set_dest(first_output, "copied_out", ir_type_parse(e->m,
                                                                 "i32"));
    check("a copy-out is an independent typed call result",
          apply(e, first_output) == IR_RULE_ACCEPT);

    ir_instr_t *outbad = op(e, IR_OP_CALL);

    ir_instr_operand_value(outbad, ov);
    ir_instr_operand_value(outbad, val(e, "i32"));
    ir_instr_operand_value(outbad, val(e, "u64"));
    check("an out parameter cannot be supplied as an input value",
          apply(e, outbad) == IR_RULE_REJECT);

    ir_instr_t *modify_bad = op(e, IR_OP_CALL);
    ir_instr_operand_value(modify_bad, ov);
    ir_instr_operand_out(modify_bad);
    ir_instr_operand_out(modify_bad);
    check("modify still requires its copy-in value",
          apply(e, modify_bad) == IR_RULE_REJECT);

    ir_value_t *initializer = ir_alloc(e->ctx, sizeof(ir_value_t));
    initializer->kind = IR_VAL_FUNC;
    initializer->name = "initializer";
    initializer->type = ir_type_parse(e->m, "(inout i32) -> void");
    ir_instr_t *fresh_storage = op(e, IR_OP_ALLOC);
    ir_instr_operand_type(fresh_storage, ir_type_parse(e->m, "i32"));
    ir_instr_set_dest(fresh_storage, "fresh_storage",
                      ir_type_parse(e->m, "ptr[i32]"));
    ir_instr_t *initialize = op(e, IR_OP_CALL);
    ir_instr_operand_value(initialize, initializer);
    ir_instr_operand_fresh(initialize, fresh_storage->dest);
    ir_instr_set_dest(initialize, "initialized", ir_type_parse(e->m,
                                                                "void"));
    check("fresh allocated storage can initialize a modify parameter",
          apply(e, initialize) == IR_RULE_ACCEPT);

    ir_instr_t *not_fresh = op(e, IR_OP_CALL);
    ir_instr_operand_value(not_fresh, initializer);
    check("fresh construction operands must refer to allocation results",
          !ir_instr_operand_fresh(not_fresh, val(e, "ptr[i32]")));

    ir_func_t *copyout = ir_func_add(e->m, "copyout",
        ir_type_parse(e->m, "(out i32, inout u64) -> void"),
        IR_LINK_INTERNAL);
    ir_block_t *copyout_entry = ir_block_add(copyout, "entry");
    ir_instr_t *ret = ir_instr_add(copyout_entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, val(e, "i32"));
    ir_instr_operand_value(ret, val(e, "u64"));
    check("successful return carries separate copy-out values",
          apply(e, ret) == IR_RULE_ACCEPT);
    ir_instr_t *incomplete_ret = ir_instr_add(copyout_entry, IR_OP_RETURN);
    ir_instr_operand_value(incomplete_ret, val(e, "i32"));
    check("successful return must provide every copy-out value",
          apply(e, incomplete_ret) == IR_RULE_REJECT);

    close_env(e);
}

static void test_generic_callable_contracts(void)
{
    env *e = open_env(&ir_profile_bce, "i32");
    th_type *identity_body = ir_type_parse(e->m, "(`a) -> `a");
    th_type *identity_contract = identity_body
        ? th_generalize(ir_ctx_types(e->ctx), identity_body) : nullptr;
    ir_func_t *identity = identity_body
        ? ir_func_add(e->m, "identity", identity_body, IR_LINK_EXTERNAL)
        : nullptr;
    bool identity_set = identity && identity_contract
        && ir_func_set_callable_contract(identity, identity_contract);
    ir_value_t *identity_value = identity
        ? ir_func_address(e->m, identity) : nullptr;
    check("a quantified contract is retained on its function",
          identity_contract && th_kind_of(identity_contract) == TH_BINDER
              && identity_set
              && ir_func_callable_contract(identity) == identity_contract);
    check("a function address exposes its callable contract",
          identity_set && identity_value
              && identity_value->type == identity_contract);
    ir_value_t *identity_reference = identity
        ? ir_func_reference(e->m, identity) : nullptr;
    check("a function reference preserves the callable behind an indirection",
          identity_reference
              && identity_reference->kind == IR_VAL_FUNC
              && th_kind_of(th_resolve(identity_reference->type))
                    == TH_INDIRECTION
              && th_ind_referent(identity_reference->type)
                    == identity_contract);
    uint64_t frozen_hash = identity_contract
        ? th_hash(ir_ctx_types(e->ctx), identity_contract) : 0;

    ir_instr_t *integer_call = op(e, IR_OP_CALL);
    if (integer_call) {
        ir_instr_set_dest(integer_call, "identity_integer",
                          ir_type_parse(e->m, "i32"));
        ir_instr_operand_value(integer_call, identity_value);
        ir_instr_operand_value(integer_call, val(e, "i32"));
        ir_instr_set_call_signature(integer_call,
            ir_type_parse(e->m, "(i32) -> i32"));
    }
    check("a generic function contract instantiates for an integer call",
          identity_set && integer_call
              && apply(e, integer_call) == IR_RULE_ACCEPT);

    ir_instr_t *boolean_call = op(e, IR_OP_CALL);
    if (boolean_call) {
        ir_instr_set_dest(boolean_call, "identity_boolean",
                          ir_type_parse(e->m, "bool"));
        ir_instr_operand_value(boolean_call, identity_value);
        ir_instr_operand_value(boolean_call, val(e, "bool"));
        ir_instr_set_call_signature(boolean_call,
            ir_type_parse(e->m, "(bool) -> bool"));
    }
    ir_rule_result_t boolean_result = identity_set && boolean_call
        ? ir_rule_apply(e->m, e->rs, boolean_call, e->d)
        : (ir_rule_result_t){0};
    harvest(e);
    check("a later boolean call receives its own generic instance",
          boolean_result.verdict == IR_RULE_ACCEPT);
    check("generic calls do not refine the frozen callable contract",
          identity_contract
              && th_hash(ir_ctx_types(e->ctx), identity_contract)
                    == frozen_hash);

    th_type *open_signature = ir_type_parse(e->m, "(`call) -> `call");
    ir_instr_t *open_call = op(e, IR_OP_CALL);
    ir_instr_set_dest(open_call, "open_call_result", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(open_call, identity_value);
    ir_instr_operand_value(open_call, val(e, "i32"));
    ir_instr_set_call_signature(open_call, open_signature);
    check("generic call checking does not refine the caller signature",
          apply(e, open_call) == IR_RULE_ACCEPT
              && th_kind_of(th_resolve(th_func_ret(open_signature))) == TH_VAR);

    th_type *pair_body = ir_type_parse(e->m, "(`a, `a) -> `a");
    th_type *pair_contract = pair_body
        ? th_generalize(ir_ctx_types(e->ctx), pair_body) : nullptr;
    ir_func_t *pair = pair_body
        ? ir_func_add(e->m, "same_pair", pair_body, IR_LINK_EXTERNAL)
        : nullptr;
    bool pair_set = pair && pair_contract
        && ir_func_set_callable_contract(pair, pair_contract);
    ir_value_t *pair_value = pair ? ir_func_address(e->m, pair) : nullptr;
    ir_instr_t *mismatched_pair = op(e, IR_OP_CALL);
    if (mismatched_pair) {
        ir_instr_set_dest(mismatched_pair, "bad_pair",
                          ir_type_parse(e->m, "i32"));
        ir_instr_operand_value(mismatched_pair, pair_value);
        ir_instr_operand_value(mismatched_pair, val(e, "i32"));
        ir_instr_operand_value(mismatched_pair, val(e, "bool"));
        ir_instr_set_call_signature(mismatched_pair,
            ir_type_parse(e->m, "(i32, i32) -> i32"));
    }
    check("one generic instantiation correlates all positional arguments",
          pair_set && mismatched_pair
              && apply(e, mismatched_pair) == IR_RULE_REJECT);

    ir_instr_t *bad_signature = op(e, IR_OP_CALL);
    if (bad_signature) {
        ir_instr_set_dest(bad_signature, "bad_signature", ir_type_parse(e->m,
                            "i32"));
        ir_instr_operand_value(bad_signature, pair_value);
        ir_instr_operand_value(bad_signature, val(e, "i32"));
        ir_instr_operand_value(bad_signature, val(e, "i32"));
        ir_instr_set_call_signature(bad_signature,
            ir_type_parse(e->m, "(i32, bool) -> i32"));
    }
    check("a supplied call signature must instantiate the frozen contract",
          pair_set && bad_signature
              && apply(e, bad_signature) == IR_RULE_REJECT);

    th_type *context_body = ir_type_parse(e->m, "() -> `a");
    th_type *context_contract = context_body
        ? th_generalize(ir_ctx_types(e->ctx), context_body) : nullptr;
    ir_func_t *contextual = context_body
        ? ir_func_add(e->m, "contextual", context_body, IR_LINK_EXTERNAL)
        : nullptr;
    bool context_set = contextual && context_contract
        && ir_func_set_callable_contract(contextual, context_contract);
    ir_instr_t *context_call = op(e, IR_OP_CALL);
    if (context_call) {
        ir_instr_set_dest(context_call, "contextual_integer",
                          ir_type_parse(e->m, "i32"));
        ir_instr_operand_value(context_call,
                               ir_func_address(e->m, contextual));
        ir_instr_set_call_signature(context_call,
            ir_type_parse(e->m, "() -> i32"));
    }
    check("the expected call result can constrain a generic return type",
          context_set && context_call
              && apply(e, context_call) == IR_RULE_ACCEPT);

    ir_block_t *identity_block = ir_block_add(identity, "entry");
    ir_value_t *identity_param = identity_block
        ? ir_param_add(identity, "value",
                       th_func_param(identity_body, 0)->type) : nullptr;
    ir_instr_t *identity_return = identity_block
        ? ir_instr_add(identity_block, IR_OP_RETURN) : nullptr;
    if (identity_return) ir_instr_operand_value(identity_return,
                                                 identity_param);

    ir_block_t *pair_block = ir_block_add(pair, "entry");
    ir_value_t *pair_first = pair_block
        ? ir_param_add(pair, "first", th_func_param(pair_body, 0)->type)
        : nullptr;
    if (pair_block)
        ir_param_add(pair, "second", th_func_param(pair_body, 1)->type);
    ir_instr_t *pair_return = pair_block
        ? ir_instr_add(pair_block, IR_OP_RETURN) : nullptr;
    if (pair_return) ir_instr_operand_value(pair_return, pair_first);

    ir_block_t *context_block = ir_block_add(contextual, "entry");
    if (context_block) ir_instr_add(context_block, IR_OP_RETURN);
    ir_instr_t *caller_return = ir_instr_add(e->b, IR_OP_RETURN);
    if (caller_return && e->f->n_params)
        ir_instr_operand_value(caller_return, e->f->params[0]);

    char *text = ir_module_print(e->m);
    ir_ctx_t *roundtrip_ctx = ir_ctx_new();
    ir_diags_t *roundtrip_diags = ir_diags_new(roundtrip_ctx, nullptr);
    const ir_profile_t *profiles[] = {&ir_profile_bce};
    ir_module_t *roundtrip = text
        ? ir_module_parse(roundtrip_ctx, text, 0, profiles, 1,
                          roundtrip_diags) : nullptr;
    ir_func_t *roundtrip_identity = roundtrip
        ? ir_func_find(roundtrip, "identity") : nullptr;
    ir_func_t *roundtrip_caller = roundtrip
        ? ir_func_find(roundtrip, "f") : nullptr;
    bool has_roundtrip_call_signature = false;
    for (uint32_t i = 0; roundtrip_caller
         && i < roundtrip_caller->n_blocks; i++) {
        ir_block_t *block = roundtrip_caller->blocks[i];
        for (uint32_t j = 0; j < block->n_instrs; j++) {
            ir_instr_t *instruction = block->instrs[j];
            has_roundtrip_call_signature |= instruction->op == IR_OP_CALL
                && ir_instr_call_signature(instruction) != nullptr;
        }
    }
    check("IR text round-trips generic callable and call-site contracts",
          roundtrip && roundtrip_identity
              && ir_func_callable_contract(roundtrip_identity)
              && has_roundtrip_call_signature
              && ir_diags_count(roundtrip_diags) == 0);
    if (!roundtrip || !roundtrip_identity || !has_roundtrip_call_signature
        || ir_diags_count(roundtrip_diags)) {
        char *errors = ir_diags_render_text(roundtrip_diags, nullptr);
        printf("  IR text: %s\n", text ? text : "<null>");
        if (errors) { printf("  %s", errors); free(errors); }
    }
    core_free(text);
    ir_ctx_free(roundtrip_ctx);

    close_env(e);
}

static void test_open_callable_contracts(void)
{
    env *e = open_env(&ir_profile_bce, "i32");
    th_ctx *types = ir_ctx_types(e->ctx);
    th_type *value_type = th_var(types);
    th_type *failure_type = th_var(types);
    th_param parameter = {.mode = TH_IN, .type = value_type};
    th_type *open_body = th_func_with_error(types, value_type, failure_type,
                                            &parameter, 1, nullptr, nullptr, 0);
    ir_func_t *open_function = open_body
        ? ir_func_add(e->m, "open_identity", open_body, IR_LINK_EXTERNAL)
        : nullptr;
    bool contract_set = open_function
        && ir_func_set_callable_contract(open_function, open_body);
    ir_value_t *open_function_value = open_function
        ? ir_func_address(e->m, open_function) : nullptr;
    ir_instr_t *call = op(e, IR_OP_CALL);
    if (call) {
        ir_instr_set_dest(call, "open_identity_integer",
                          ir_type_parse(e->m, "i32"));
        ir_instr_operand_value(call, open_function_value);
        ir_instr_operand_value(call, val(e, "i32"));
        ir_instr_set_call_signature(call,
            ir_type_parse(e->m, "(i32) -> i32 fail: void"));
    }
    ir_verdict_t verdict = open_function_value && call
        ? apply(e, call) : IR_RULE_REJECT;
    check("an open callable contract specializes from its call signature",
          contract_set
              && ir_func_callable_contract(open_function) == open_body
              && open_function_value->type == open_body
              && verdict == IR_RULE_ACCEPT);
    if (verdict != IR_RULE_ACCEPT) {
        char *errors = ir_diags_render_text(e->d, nullptr);
        if (errors) { printf("  %s", errors); free(errors); }
    }
    check("open call specialization does not refine the callable contract",
          th_kind_of(th_resolve(value_type)) == TH_VAR
              && th_kind_of(th_resolve(failure_type)) == TH_VAR);
    close_env(e);
}

static void test_comptime_and_terminators(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    const ir_op_t sizes[] = {IR_OP_SIZEOF, IR_OP_ALIGNOF, IR_OP_TYPEHASH};

    for (size_t i = 0; i < 3; i++) {
        ir_instr_t *in = op(e, sizes[i]);

        ir_instr_operand_type(in, ir_type_parse(e->m, "i32"));

        if (!accepts_as(e, in, "u64")) {
            printf("FAIL  %s yields u64\n", ir_op_spelling(sizes[i]));
            fails++;
        }
    }

    ir_instr_t *ofs = op(e, IR_OP_OFFSETOF);

    ir_instr_operand_type(ofs, ir_type_parse(e->m, "(x: i32)"));
    ir_instr_operand_name(ofs, "x");
    check("offsetof yields u64", accepts_as(e, ofs, "u64"));

    ir_instr_t *at = op(e, IR_OP_ARMTAG);

    ir_instr_operand_name(at, "ok");
    check("armtag yields u64", accepts_as(e, at, "u64"));

    ir_instr_t *tn = op(e, IR_OP_TYPENAME);

    ir_instr_operand_type(tn, ir_type_parse(e->m, "i32"));
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

    ir_instr_operand_type(th, ir_type_parse(e->m, "ptr[i32]"));
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

    ir_instr_operand_type(obad, ir_type_parse(e->m, "(x: i32)"));
    ir_instr_operand_name(obad, "nope");
    check("offsetof of an unknown member is rejected",
          apply(e, obad) == IR_RULE_REJECT);

    ir_instr_t *j = op(e, IR_OP_JUMP);

    ir_instr_operand_block(j, e->b);
    check("jump yields nothing", accepts_as(e, j, nullptr));

    ir_instr_t *u = op(e, IR_OP_UNREACHABLE);

    check("unreachable yields nothing", accepts_as(e, u, nullptr));

    ir_instr_t *trap = op(e, IR_OP_TRAP);

    check("trap yields nothing", accepts_as(e, trap, nullptr));

    ir_instr_t *br = op(e, IR_OP_BRANCH);

    ir_instr_operand_value(br, val(e, "bool"));
    ir_instr_operand_block(br, e->b);
    ir_instr_operand_block(br, e->b);
    check("branch on a boolean is accepted", accepts_as(e, br, nullptr));

    ir_instr_t *brbad = op(e, IR_OP_BRANCH);

    ir_instr_operand_value(brbad, val(e, "i32"));
    ir_instr_operand_block(brbad, e->b);
    ir_instr_operand_block(brbad, e->b);
    check("branch on a non-boolean is rejected",
          apply(e, brbad) == IR_RULE_REJECT);

    ir_instr_t *sw = op(e, IR_OP_SWITCH);

    ir_instr_operand_value(sw, val(e, "i32"));
    ir_instr_operand_default(sw, e->b);
    ir_instr_operand_switch_arm(sw, 1, e->b);
    ir_instr_operand_switch_arm(sw, 2, e->b);
    check("a switch with distinct cases is accepted",
          accepts_as(e, sw, nullptr));

    ir_instr_t *dup = op(e, IR_OP_SWITCH);

    ir_instr_operand_value(dup, val(e, "i32"));
    ir_instr_operand_default(dup, e->b);
    ir_instr_operand_switch_arm(dup, 1, e->b);
    ir_instr_operand_switch_arm(dup, 1, e->b);
    check("duplicate case values are rejected",
          apply(e, dup) == IR_RULE_REJECT);

    ir_instr_t *swb = op(e, IR_OP_SWITCH);

    ir_instr_operand_value(swb, val(e, "bool"));
    ir_instr_operand_default(swb, e->b);
    ir_instr_operand_switch_arm(swb, 0, e->b);
    ir_instr_operand_switch_arm(swb, 1, e->b);
    check("a boolean scrutinee is allowed too",
          accepts_as(e, swb, nullptr));

    ir_instr_t *swf = op(e, IR_OP_SWITCH);

    ir_instr_operand_value(swf, val(e, "f64"));
    ir_instr_operand_default(swf, e->b);
    check("switching on a float is rejected",
          apply(e, swf) == IR_RULE_REJECT);

    ir_instr_t *r = op(e, IR_OP_RETURN);

    ir_instr_operand_value(r, val(e, "i32"));
    check("returning the declared type is accepted",
          accepts_as(e, r, nullptr));

    ir_instr_t *rbad = op(e, IR_OP_RETURN);

    ir_instr_operand_value(rbad, val(e, "u64"));
    check("returning the wrong type is rejected",
          apply(e, rbad) == IR_RULE_REJECT);

    ir_instr_t *rnone = op(e, IR_OP_RETURN);

    check("returning nothing from a non-void function is rejected",
          apply(e, rnone) == IR_RULE_REJECT);

    env *generic = open_env(&ir_profile_bce, "i32");
    generic->f->type = ir_type_parse(generic->m, "() -> `a");
    ir_instr_t *generic_return = op(generic, IR_OP_RETURN);
    ir_instr_operand_value(generic_return, val(generic, "i32"));
    check("return copy-unifies a generic result without refining it",
          accepts_as(generic, generic_return, nullptr)
              && th_kind_of(th_resolve(th_func_ret(generic->f->type)))
                  == TH_VAR);

    env *v = open_env(&ir_profile_bce, "void");

    ir_instr_t *rv = op(v, IR_OP_RETURN);

    check("a void function may return nothing",
          accepts_as(v, rv, nullptr));

    ir_instr_t *rvbad = op(v, IR_OP_RETURN);

    ir_instr_operand_value(rvbad, val(v, "i32"));
    check("a void function may not return a value",
          apply(v, rvbad) == IR_RULE_REJECT);

    env *failing = open_env(&ir_profile_bce, "i32");
    failing->f->type = ir_type_parse(failing->m,
                                     "() -> i32 fail: i32");
    ir_instr_t *failure = op(failing, IR_OP_FAIL);
    ir_instr_operand_value(failure, val(failing, "i32"));
    check("fail accepts a value matching the function failure channel",
          accepts_as(failing, failure, nullptr));
    ir_instr_t *wrong_failure = op(failing, IR_OP_FAIL);
    ir_instr_operand_value(wrong_failure, val(failing, "bool"));
    check("fail rejects a value with the wrong failure type",
          apply(failing, wrong_failure) == IR_RULE_REJECT);

    env *generic_failure = open_env(&ir_profile_bce, "i32");
    generic_failure->f->type = ir_type_parse(generic_failure->m,
                                             "() -> i32 fail: `e");
    ir_instr_t *generic_fail = op(generic_failure, IR_OP_FAIL);
    ir_instr_operand_value(generic_fail, val(generic_failure, "string"));
    check("fail copy-unifies a generic failure channel without refining it",
          accepts_as(generic_failure, generic_fail, nullptr)
              && th_kind_of(th_resolve(
                    th_func_error(generic_failure->f->type))) == TH_VAR);

    close_env(v);
    close_env(generic);
    close_env(failing);
    close_env(generic_failure);
    close_env(e);
}

static void test_phase_result_type(void)
{
    env *e = open_env(&ir_profile_bce, "i32");
    ir_instr_t *result = op(e, IR_OP_PHASE_RESULT);
    bool set = result && ir_instr_set_dest(result, "phase_value",
        ir_type_parse(e->m, "i32"))
        && ir_instr_set_phase_result(e->ctx, result, "module.value.1");
    check("a phase result is a typed external runtime value",
          set && accepts_as(e, result, "i32"));
    close_env(e);
}


static void test_constants_and_copy(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *c = op(e, IR_OP_CONST);

    ir_instr_set_dest(c, "k", ir_type_parse(e->m, "i32"));
    ir_instr_operand_int(c, 7);
    check("const takes its type from its destination",
          accepts_as(e, c, "i32"));

    ir_instr_t *enum_value = op(e, IR_OP_ENUM_CONST);
    ir_instr_set_dest(enum_value, "enum_value", ir_type_parse(e->m, "Widget"));
    ir_instr_operand_name(enum_value, "equal");
    ir_instr_operand_uint(enum_value, 1);
    check("an enum constant preserves its named type and member",
          accepts_as(e, enum_value, "Widget"));

    ir_instr_t *enum_scalar = op(e, IR_OP_ENUM_CONST);
    ir_instr_set_dest(enum_scalar, "enum_scalar", ir_type_parse(e->m, "i32"));
    ir_instr_operand_name(enum_scalar, "equal");
    ir_instr_operand_uint(enum_scalar, 1);
    check("an enum constant rejects a scalar result type",
          apply(e, enum_scalar) == IR_RULE_REJECT);

    ir_instr_t *empty = op(e, IR_OP_CONST);
    ir_instr_set_dest(empty, "empty", ir_type_parse(e->m, "i32"));
    check("a scalar const without a payload is rejected",
          apply(e, empty) == IR_RULE_REJECT);

    ir_instr_t *cbad = op(e, IR_OP_CONST);

    ir_instr_set_dest(cbad, "k2", ir_type_parse(e->m, "Widget"));
    ir_instr_operand_int(cbad, 7);
    check("a constant of an opaque type is rejected",
          apply(e, cbad) == IR_RULE_REJECT);

    ir_instr_t *wide = op(e, IR_OP_CONST);

    ir_instr_set_dest(wide, "k3", ir_type_parse(e->m, "i8"));
    ir_instr_operand_int(wide, 300);
    check("a literal too large for its type is rejected",
          apply(e, wide) == IR_RULE_REJECT);

    ir_instr_t *neg = op(e, IR_OP_CONST);

    ir_instr_set_dest(neg, "k4", ir_type_parse(e->m, "u8"));
    ir_instr_operand_int(neg, -1);
    check("a negative literal in an unsigned type is rejected",
          apply(e, neg) == IR_RULE_REJECT);

    ir_instr_t *edge = op(e, IR_OP_CONST);

    ir_instr_set_dest(edge, "k5", ir_type_parse(e->m, "i8"));
    ir_instr_operand_int(edge, -128);
    check("the bottom of the range is representable",
          accepts_as(e, edge, "i8"));

    ir_instr_t *fl = op(e, IR_OP_CONST);

    ir_instr_set_dest(fl, "k6", ir_type_parse(e->m, "i32"));
    ir_instr_operand_float(fl, 1.5);
    check("a float literal in an integer type is rejected",
          apply(e, fl) == IR_RULE_REJECT);

    ir_instr_t *fl2 = op(e, IR_OP_CONST);

    ir_instr_set_dest(fl2, "k7", ir_type_parse(e->m, "f64"));
    ir_instr_operand_float(fl2, 1.5);
    check("and is representable in a float type",
          accepts_as(e, fl2, "f64"));

    ir_int_t u64max = ((ir_int_t)1 << 64) - 1;

    ir_instr_t *pu = op(e, IR_OP_CONST);

    ir_instr_set_dest(pu, "pu", ir_type_parse(e->m, "u128"));
    ir_instr_operand_uint(pu, ~(ir_uint_t)0);
    check("u128 is a type the profile declares, to its top value",
          accepts_as(e, pu, "u128"));

    ir_instr_t *pi = op(e, IR_OP_CONST);

    ir_instr_set_dest(pi, "pi", ir_type_parse(e->m, "i128"));
    ir_instr_operand_int_lit(pi, (ir_lit_t){.mag = (ir_uint_t)1 << 127, .neg = true});
    check("and so is i128, to its bottom value", accepts_as(e, pi, "i128"));

    ir_instr_t *pio = op(e, IR_OP_CONST);

    ir_instr_set_dest(pio, "pio", ir_type_parse(e->m, "i128"));
    ir_instr_operand_uint(pio, (ir_uint_t)1 << 127);
    check("i128 stops where it should",
          apply(e, pio) == IR_RULE_REJECT);

    ir_instr_t *add128 = op(e, IR_OP_ADD);

    ir_instr_add_mod(add128, "wrap");
    ir_instr_operand_value(add128, val(e, "u128"));
    ir_instr_operand_value(add128, val(e, "u128"));
    check("128-bit arithmetic types like any other width",
          accepts_as(e, add128, "u128"));

    ir_instr_t *u128top = op(e, IR_OP_CONST);

    ir_instr_set_dest(u128top, "kt", ir_type_parse(e->m, "ubitint[128]"));
    ir_instr_operand_uint(u128top, ~(ir_uint_t)0);
    check("the top of u128 is expressible, which a signed carrier could not do",
          accepts_as(e, u128top, "ubitint[128]"));

    ir_instr_t *u128neg = op(e, IR_OP_CONST);

    ir_instr_set_dest(u128neg, "kn", ir_type_parse(e->m, "ubitint[128]"));
    ir_instr_operand_int(u128neg, -1);
    check("and a negative literal there is still rejected",
          apply(e, u128neg) == IR_RULE_REJECT);

    ir_instr_t *i128min = op(e, IR_OP_CONST);

    ir_instr_set_dest(i128min, "ki", ir_type_parse(e->m, "bitint[128]"));
    ir_instr_operand_int_lit(i128min,
                      (ir_lit_t){.mag = (ir_uint_t)1 << 127, .neg = true});
    check("so is the bottom of i128", accepts_as(e, i128min, "bitint[128]"));

    ir_instr_t *i128over = op(e, IR_OP_CONST);

    ir_instr_set_dest(i128over, "kv", ir_type_parse(e->m, "bitint[128]"));
    ir_instr_operand_int_lit(i128over,
                      (ir_lit_t){.mag = ((ir_uint_t)1 << 127) + 1,
                                 .neg = true});
    check("and one below it is not",
          apply(e, i128over) == IR_RULE_REJECT);

    ir_instr_t *top = op(e, IR_OP_CONST);

    ir_instr_set_dest(top, "ku", ir_type_parse(e->m, "u64"));
    ir_instr_operand_int(top, u64max);
    check("the whole range of u64 is expressible",
          accepts_as(e, top, "u64"));

    ir_instr_t *over = op(e, IR_OP_CONST);

    ir_instr_set_dest(over, "ko", ir_type_parse(e->m, "u64"));
    ir_instr_operand_int(over, u64max + 1);
    check("and one past the top of it is rejected",
          apply(e, over) == IR_RULE_REJECT);

    ir_instr_t *w128 = op(e, IR_OP_CONST);

    ir_instr_set_dest(w128, "kw", ir_type_parse(e->m, "bitint[100]"));
    ir_instr_operand_int(w128, ((ir_int_t)1 << 99) - 1);
    check("so is the range of any type narrower than 128 bits",
          accepts_as(e, w128, "bitint[100]"));

    ir_instr_t *big = op(e, IR_OP_CONST);

    ir_instr_set_dest(big, "k9", ir_type_parse(e->m, "f32"));
    ir_instr_operand_float(big, 1e300);
    check("a float literal outside the narrower float is rejected",
          apply(e, big) == IR_RULE_REJECT);

    ir_instr_t *ok32 = op(e, IR_OP_CONST);

    ir_instr_set_dest(ok32, "k10", ir_type_parse(e->m, "f32"));
    ir_instr_operand_float(ok32, 0.5);
    check("and one it can hold exactly is not",
          accepts_as(e, ok32, "f32"));

    ir_instr_t *rounds = op(e, IR_OP_CONST);

    ir_instr_set_dest(rounds, "k11", ir_type_parse(e->m, "f32"));
    ir_instr_operand_float(rounds, 0.1);
    check("a float literal is the nearest value, so rounding is not an error",
          accepts_as(e, rounds, "f32"));

    ir_instr_t *tw = op(e, IR_OP_CONST);

    ir_instr_set_dest(tw, "k8", ir_type_parse(e->m, "bool"));
    ir_instr_operand_int(tw, 2);
    check("only zero and one are representable in bool",
          apply(e, tw) == IR_RULE_REJECT);

    ir_instr_t *u = op(e, IR_OP_UNDEF);

    ir_instr_set_dest(u, "un", ir_type_parse(e->m, "i32"));
    check("undef takes its type from its destination",
          accepts_as(e, u, "i32"));

    ir_instr_t *ag = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(ag, "ag", ir_type_parse(e->m, "(x: i32)"));
    ir_instr_operand_value(ag, val(e, "i32"));
    check("aggregate takes its type from its destination",
          accepts_as(e, ag, "(x: i32)"));

    ir_instr_t *agbad = op(e, IR_OP_AGGREGATE);

    ir_instr_set_dest(agbad, "agb", ir_type_parse(e->m, "i32"));
    ir_instr_operand_value(agbad, val(e, "i32"));
    check("an aggregate destination that is not a record is rejected",
          apply(e, agbad) == IR_RULE_REJECT);

    ir_local_t *kl = ir_local_add(e->f, "kl", ir_type_parse(e->m, "u16"));

    ir_instr_t *kc = op(e, IR_OP_CONST);

    ir_instr_set_dest_local(kc, kl);
    ir_instr_operand_int(kc, 3);
    check("a constant may be assigned into a local",
          accepts_as(e, kc, "u16"));

    ir_instr_t *cp = op(e, IR_OP_COPY);

    ir_instr_operand_value(cp, val(e, "u16"));
    check("copy keeps its operand type", accepts_as(e, cp, "u16"));

    ir_instr_t *useundef = op(e, IR_OP_COPY);

    ir_instr_operand_value(useundef, u->dest);
    check("an undef value is legal to read",
          accepts_as(e, useundef, "i32"));

    ir_local_t *l = ir_local_add(e->f, "n", ir_type_parse(e->m, "u16"));

    ir_instr_t *rd = op(e, IR_OP_COPY);

    ir_instr_operand_local(rd, l);
    check("a local as an operand is a read of the local's type",
          accepts_as(e, rd, "u16"));

    ir_instr_t *wr = op(e, IR_OP_COPY);

    ir_instr_set_dest_local(wr, l);
    ir_instr_operand_value(wr, val(e, "u16"));
    check("and a local is a legal destination", accepts_as(e, wr, "u16"));

    ir_instr_t *wrbad = op(e, IR_OP_COPY);

    ir_instr_set_dest_local(wrbad, l);
    ir_instr_operand_value(wrbad, val(e, "i32"));
    check("a destination whose type is not the result type is rejected",
          apply(e, wrbad) == IR_RULE_REJECT);

    ir_instr_t *dbad = op(e, IR_OP_ADD);

    ir_instr_set_dest(dbad, "sum", ir_type_parse(e->m, "i64"));
    ir_instr_add_mod(dbad, "wrap");
    ir_instr_operand_value(dbad, val(e, "i32"));
    ir_instr_operand_value(dbad, val(e, "i32"));
    check("that holds for a value destination too",
          apply(e, dbad) == IR_RULE_REJECT);

    close_env(e);
}

static void test_second_profile(void)
{
    env *e = open_env(&ir_profile_synth, "int");

    ir_instr_t *cmp = op(e, IR_OP_LT);

    ir_instr_operand_value(cmp, val(e, "int"));
    ir_instr_operand_value(cmp, val(e, "int"));
    check("a comparison yields this profile's boolean, not bce's",
          accepts_as(e, cmp, "flag"));

    ir_instr_t *cnt = op(e, IR_OP_POPCOUNT);

    ir_instr_operand_value(cnt, val(e, "int"));
    check("a bit count yields this profile's 32-bit unsigned type",
          accepts_as(e, cnt, "word"));

    ir_instr_t *sz = op(e, IR_OP_SIZEOF);

    ir_instr_operand_type(sz, ir_type_parse(e->m, "int"));
    check("sizeof has no 64-bit unsigned type in this profile",
          apply(e, sz) == IR_RULE_REJECT);

    ir_instr_t *cat = op(e, IR_OP_ADD);

    ir_instr_operand_value(cat, val(e, "str"));
    ir_instr_operand_value(cat, val(e, "str"));
    check("the profile's own rule beats the default for its own type",
          accepts_as(e, cat, "str"));
    check("and the profile rule is what is recorded",
          ir_annot_get(&cat->annots, "ir.rule")
              && !strcmp(ir_annot_get(&cat->annots, "ir.rule")->s,
                         "synth.concat"));

    ir_instr_t *plain = op(e, IR_OP_ADD);

    ir_instr_add_mod(plain, "wrap");
    ir_instr_operand_value(plain, val(e, "int"));
    ir_instr_operand_value(plain, val(e, "int"));
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

    ir_instr_operand_type(ha, ir_type_parse(a->m, "ptr[i32]"));
    ir_instr_operand_type(hb, ir_type_parse(b->m, "ptr[i32]"));
    apply(a, ha);
    apply(b, hb);

    ir_annot_t *ca = ir_annot_get(&ha->annots, "ir.const");
    ir_annot_t *cb = ir_annot_get(&hb->annots, "ir.const");

    check("typehash is the same constant under either target",
          ca && cb && ir_lit_eq(ca->i, cb->i));

    ir_instr_t *na = op(a, IR_OP_TYPENAME);
    ir_instr_t *nb = op(b, IR_OP_TYPENAME);

    ir_instr_operand_type(na, ir_type_parse(a->m, "ptr[i32]"));
    ir_instr_operand_type(nb, ir_type_parse(b->m, "ptr[i32]"));
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

    ir_instr_operand_value(cp, val(e, "`a"));
    check("a non-ground type passes through copy", accepts_as(e, cp, "`a"));

    ir_instr_t *ar = op(e, IR_OP_ADD);

    ir_instr_add_mod(ar, "wrap");
    ir_instr_operand_value(ar, val(e, "`a"));
    ir_instr_operand_value(ar, val(e, "`a"));
    check("arithmetic on an opaque type variable is rejected",
          apply(e, ar) == IR_RULE_REJECT);

    ir_instr_t *ld = op(e, IR_OP_LOAD);

    ir_instr_operand_value(ld, val(e, "ptr[`a]"));
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

    ir_instr_operand_value(cnt, val(e, "word"));
    check("a bit count needs a 32-bit unsigned type the profile lacks",
          apply(e, cnt) == IR_RULE_REJECT);

    ir_instr_t *cmp = op(e, IR_OP_EQ);

    ir_instr_operand_value(cmp, val(e, "word"));
    ir_instr_operand_value(cmp, val(e, "word"));
    check("a comparison needs a boolean type the profile lacks",
          apply(e, cmp) == IR_RULE_REJECT);

    ir_instr_t *df = op(e, IR_OP_DIFF);

    ir_instr_operand_value(df, val(e, "ptr[word]"));
    ir_instr_operand_value(df, val(e, "ptr[word]"));
    check("diff needs a 64-bit signed type the profile lacks",
          apply(e, df) == IR_RULE_REJECT);

    ir_instr_t *mc = op(e, IR_OP_MEMCMP);

    ir_instr_operand_value(mc, val(e, "ptr[word]"));
    ir_instr_operand_value(mc, val(e, "ptr[word]"));
    ir_instr_operand_value(mc, val(e, "word"));
    check("memcmp needs a 32-bit signed type the profile lacks",
          apply(e, mc) == IR_RULE_REJECT);

    ir_instr_t *vt = op(e, IR_OP_VARIANT_TAG);

    ir_instr_operand_value(vt, val(e, "variant R[word]"));
    check("variant.tag needs a 64-bit unsigned type the profile lacks",
          apply(e, vt) == IR_RULE_REJECT);

    ir_instr_t *tn = op(e, IR_OP_TYPENAME);

    ir_instr_operand_type(tn, ir_type_parse(e->m, "word"));
    check("typename needs a character type the profile lacks",
          apply(e, tn) == IR_RULE_REJECT);

    close_env(e);
}

static void test_diagnostics_point_at_operands(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *def = op(e, IR_OP_COPY);

    ir_instr_set_dest(def, "src", ir_type_parse(e->m, "u64"));
    ir_instr_operand_value(def, val(e, "u64"));
    def->span = ir_span_make(0, 10, 14, 2, 3, 2, 7);
    apply(e, def);

    ir_instr_t *use = op(e, IR_OP_ADD);

    ir_instr_add_mod(use, "wrap");
    ir_instr_operand_value(use, val(e, "i32"));
    ir_instr_operand_value(use, def->dest);
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

    ir_instr_add_mod(in, "wrap");
    ir_instr_operand_value(in, val(e, "i32"));
    ir_instr_operand_value(in, val(e, "i32"));
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

    ir_instr_add_mod(clean, "wrap");
    ir_instr_operand_value(clean, val(e, "i32"));
    ir_instr_operand_value(clean, val(e, "i32"));

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

    ir_instr_operand_value(ground, val(e, "i32"));
    apply(e, ground);
    check("a ground operand is not marked boxed",
          ir_annot_get(&ground->annots, "ir.boxed") == nullptr);

    ir_instr_t *gen = op(e, IR_OP_COPY);

    ir_instr_operand_value(gen, val(e, "`a"));
    apply(e, gen);

    ir_annot_t *bx = ir_annot_get(&gen->annots, "ir.boxed");

    check("a non-ground operand is marked boxed",
          bx && bx->kind == IR_ANNOT_INT && bx->i.mag == 1);
    check("and boxing is derived from typing",
          bx && bx->derived_from == IR_PROP_TYPED);

    ir_instr_t *mix = op(e, IR_OP_STORE);

    ir_instr_operand_value(mix, val(e, "ptr[i32]"));
    ir_instr_operand_value(mix, val(e, "i32"));
    apply(e, mix);
    check("a store of ground types boxes nothing",
          ir_annot_get(&mix->annots, "ir.boxed") == nullptr);

    ir_instr_t *mix2 = op(e, IR_OP_STORE);

    ir_value_t *pointer = val(e, "ptr[`a]");
    ir_instr_operand_value(mix2, pointer);
    ir_instr_operand_value(mix2, ir_param_add(e->f, "generic_stored",
        th_ind_referent(pointer->type)));
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
        ir_instr_operand_value(wide, ir_param_add(e->f, mname[i], mem[i].type));
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
                ir_instr_operand_value(in, val(e, "`a"));
                break;
            case IR_OPD_TYPE:
                ir_instr_operand_type(in, ir_type_parse(e->m, "`a"));
                break;
            case IR_OPD_NAME:
                ir_instr_operand_name(in, "x");
                break;
            case IR_OPD_BLOCK:
                ir_instr_operand_block(in, e->b);
                break;
            case IR_OPD_DEFAULT_BLOCK:
                ir_instr_operand_default(in, e->b);
                break;
            case IR_OPD_CONST:
                ir_instr_operand_int(in, 1);
                break;
            case IR_OPD_PHI_ARM:
                ir_instr_operand_phi_arm(in, e->b, val(e, "`a"));
                break;
            case IR_OPD_SWITCH_ARM:
                ir_instr_operand_switch_arm(in, 1, e->b);
                break;
            case IR_OPD_NAMED_ARG:
                ir_instr_operand_named(in, "x", val(e, "`a"));
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

    ir_instr_operand_value(cp, val(e, "`a"));
    check("an accepted generic result stays generic; nothing is instantiated",
          accepts_as(e, cp, "`a"));

    close_env(e);
}

static void test_opaque_named(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *cp = op(e, IR_OP_COPY);

    ir_instr_operand_value(cp, val(e, "Widget"));
    check("an opaque named type can be moved", accepts_as(e, cp, "Widget"));

    ir_instr_t *al = op(e, IR_OP_ALLOCA);

    ir_instr_operand_type(al, ir_type_parse(e->m, "Widget"));
    check("an opaque named type can be pointed to",
          accepts_as(e, al, "ptr[Widget]"));

    ir_value_t *fv = ir_alloc(e->ctx, sizeof(ir_value_t));

    fv->kind = IR_VAL_FUNC;
    fv->name = "takes";
    fv->type = ir_type_parse(e->m, "(Widget) -> i32");

    ir_instr_t *c = op(e, IR_OP_CALL);

    ir_instr_operand_value(c, fv);
    ir_instr_operand_value(c, val(e, "Widget"));
    check("an opaque named type can be passed", accepts_as(e, c, "i32"));

    ir_instr_t *st = op(e, IR_OP_STORE);

    ir_instr_operand_value(st, val(e, "ptr[Widget]"));
    ir_instr_operand_value(st, val(e, "Widget"));
    check("an opaque named type can be stored",
          accepts_as(e, st, nullptr));

    ir_instr_t *ar = op(e, IR_OP_ADD);

    ir_instr_add_mod(ar, "wrap");
    ir_instr_operand_value(ar, val(e, "Widget"));
    ir_instr_operand_value(ar, val(e, "Widget"));
    check("an opaque named type cannot be added",
          apply(e, ar) == IR_RULE_REJECT);

    close_env(e);
}

static void test_annotation_of_results(void)
{
    env *e = open_env(&ir_profile_bce, "i32");

    ir_instr_t *a = op(e, IR_OP_ADD);

    ir_instr_add_mod(a, "wrap");
    ir_instr_operand_value(a, val(e, "i32"));
    ir_instr_operand_value(a, val(e, "i32"));
    apply(e, a);

    ir_annot_t *an = ir_annot_get(&a->annots, "ir.rule");

    check("an accepted instruction records which rule fired",
          an && !strcmp(an->s, "ir.add"));
    check("and marks it derived from typing",
          an && an->derived_from == IR_PROP_TYPED);

    ir_instr_t *bad = op(e, IR_OP_ADD);

    ir_instr_operand_value(bad, val(e, "i32"));
    ir_instr_operand_value(bad, val(e, "i32"));
    apply(e, bad);
    check("a rejected instruction records nothing",
          ir_annot_get(&bad->annots, "ir.rule") == nullptr);

    ir_instr_t *tn = op(e, IR_OP_TYPENAME);

    ir_instr_operand_type(tn, ir_type_parse(e->m, "u8"));
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

int main(void)
{
    test_context_owned_named_types();
    test_constants_and_copy();
    test_arithmetic();
    test_bitwise_and_shifts();
    test_comparisons();
    test_memory();
    test_aggregates_and_variants();
    test_conversion();
    test_select_phi_call();
    test_phase_result_type();
    test_generic_callable_contracts();
    test_open_callable_contracts();
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

    dump_produced_codes("produced-codes-typing.txt", produced);

    return report("ir typing");
}
