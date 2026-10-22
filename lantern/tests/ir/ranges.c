#include "ir/analyses.h"
#include "ir/ranges.h"
#include "ir/summary.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static ir_summaries_t *
bind_summaries(ir_module_t *m)
{
    ir_func_t *functions[16];
    size_t count = 0;
    for (size_t f = 0; f < ir_func_count(m) && count < 16; f++)
        functions[count++] = ir_func_at(m, f);
    ir_summaries_t *summaries = ir_summaries_compute(functions, count,
                                                     nullptr);
    ir_analysis_bind_summaries(summaries);
    return summaries;
}

static bool
range_is(ir_func_t *f, const ir_value_t *value, ir_int_t lo, ir_int_t hi)
{
    ir_ranges_t *ranges = ir_ranges_build(f);
    ir_interval_t range = {0};
    bool ok = ir_range_of(ranges, value, nullptr, &range) && range.lo == lo
        && range.hi == hi;
    ir_ranges_free(ranges);
    return ok;
}

static ir_overflow_t
overflow_of(const ir_instr_t *in)
{
    ir_ranges_t *ranges = ir_ranges_build(in->block->owner);
    ir_overflow_t overflow = ir_range_overflow(ranges, in, nullptr, nullptr);
    ir_ranges_free(ranges);
    return overflow;
}

static bool
sub_safe(const ir_instr_t *sub)
{
    ir_ranges_t *ranges = ir_ranges_build(sub->block->owner);
    bool safe = ir_range_unsigned_sub_safe(ranges, sub);
    ir_ranges_free(ranges);
    return safe;
}

static ir_instr_t *
constant(ir_module_t *m, ir_block_t *b, const char *name, const char *type,
         int64_t value)
{
    ir_instr_t *in = ir_instr_add(b, IR_OP_CONST);
    ir_instr_set_dest(in, core_string_from_cstr(name),
                      ir_type_parse(m, core_string_from_cstr(type)));
    ir_instr_operand_int(in, value);
    return in;
}

static ir_instr_t *
binary(ir_module_t *m, ir_block_t *b, ir_op_t op, const char *name,
       const char *type, ir_value_t *left, ir_value_t *right, bool trap)
{
    ir_instr_t *in = ir_instr_add(b, op);
    ir_instr_set_dest(in, core_string_from_cstr(name),
                      ir_type_parse(m, core_string_from_cstr(type)));
    ir_instr_operand_value(in, left);
    ir_instr_operand_value(in, right);
    if (trap) ir_instr_add_mod(in, S("trap"));
    return in;
}

static void test_trapping_add_bounds(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"), ir_type_parse(m, S("(i:8) -> i:8")),
                               IR_LINK_EXTERNAL);
    ir_value_t *p = ir_param_add(f, S("p"), ir_type_parse(m, S("i:8")));
    ir_block_t *b = ir_block_add(f, S("entry"));
    ir_instr_t *c = constant(m, b, "c", "i:8", 100);
    ir_instr_t *add = binary(m, b, IR_OP_ADD, "sum", "i:8", p, c->dest, true);
    ir_instr_t *ret = ir_instr_add(b, IR_OP_RETURN);
    ir_instr_operand_value(ret, add->dest);
    check("trapping add keeps only representable results",
          range_is(f, add->dest, -28, 127));
    check("a trapping add over an unknown operand may overflow",
          overflow_of(add) == IR_OVERFLOW_MAY);
    ir_ctx_free(ctx);
}

static void test_branch_path_refines_integer_ranges(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"), ir_type_parse(m, S("(i:8) -> i:8")),
                               IR_LINK_EXTERNAL);
    ir_value_t *input = ir_param_add(f, S("input"), ir_type_parse(m, S("i:8")));
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *positive = ir_block_add(f, S("positive"));
    ir_block_t *nonpositive = ir_block_add(f, S("nonpositive"));
    ir_instr_t *zero = constant(m, entry, "zero", "i:8", 0);
    ir_instr_t *compare = binary(m, entry, IR_OP_GT, "is_positive", "bool",
                                 input, zero->dest, false);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, compare->dest);
    ir_instr_operand_block(branch, positive);
    ir_instr_operand_block(branch, nonpositive);
    ir_instr_t *one = constant(m, positive, "one", "i:8", 1);
    ir_instr_t *sum = binary(m, positive, IR_OP_ADD, "sum", "i:8", input,
                             one->dest, true);
    ir_instr_t *positive_return = ir_instr_add(positive, IR_OP_RETURN);
    ir_instr_operand_value(positive_return, sum->dest);
    ir_instr_t *nonpositive_return = ir_instr_add(nonpositive, IR_OP_RETURN);
    ir_instr_operand_value(nonpositive_return, input);
    check("the true comparison edge narrows checked arithmetic",
          range_is(f, sum->dest, 2, 127));
    ir_ctx_free(ctx);
}

static void test_negated_guards_and_earlier_traps_refine(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"), ir_type_parse(m, S("(i:8) -> i:8")),
                               IR_LINK_EXTERNAL);
    ir_value_t *input = ir_param_add(f, S("input"), ir_type_parse(m, S("i:8")));
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *big = ir_block_add(f, S("big"));
    ir_block_t *small = ir_block_add(f, S("small"));
    ir_instr_t *ten = constant(m, entry, "ten", "i:8", 10);
    ir_instr_t *below = binary(m, entry, IR_OP_LT, "below", "bool", input,
                               ten->dest, false);
    ir_instr_t *flipped = ir_instr_add(entry, IR_OP_NOT);
    ir_instr_set_dest(flipped, S("flipped"), ir_type_parse(m, S("bool")));
    ir_instr_operand_value(flipped, below->dest);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, flipped->dest);
    ir_instr_operand_block(branch, big);
    ir_instr_operand_block(branch, small);
    ir_instr_t *one = constant(m, big, "one", "i:8", 1);
    ir_instr_t *first = binary(m, big, IR_OP_ADD, "first", "i:8", input,
                               one->dest, true);
    ir_instr_t *second = binary(m, big, IR_OP_ADD, "second", "i:8", input,
                                one->dest, true);
    ir_instr_t *big_return = ir_instr_add(big, IR_OP_RETURN);
    ir_instr_operand_value(big_return, second->dest);
    ir_instr_t *small_return = ir_instr_add(small, IR_OP_RETURN);
    ir_instr_operand_value(small_return, input);
    ir_ranges_t *ranges = ir_ranges_build(f);
    ir_interval_t at_first = {0}, at_second = {0};
    check("a branch on a negated comparison refines its operand",
          ir_range_of(ranges, input, first, &at_first)
          && at_first.lo == 10 && at_first.hi == 127);
    check("a trapping add bounds its operand afterwards",
          ir_range_of(ranges, input, second, &at_second)
          && at_second.lo == 10 && at_second.hi == 126);
    check("the first add may still overflow",
          ir_range_overflow(ranges, first, nullptr, nullptr)
              == IR_OVERFLOW_MAY);
    check("the repeated add cannot overflow",
          ir_range_overflow(ranges, second, nullptr, nullptr)
              == IR_OVERFLOW_NEVER);
    ir_ranges_free(ranges);
    ir_ctx_free(ctx);
}

static void test_cached_ranges_follow_mods_and_scopes(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"), ir_type_parse(m, S("(u:8) -> u:8")),
                               IR_LINK_EXTERNAL);
    ir_value_t *x = ir_param_add(f, S("x"), ir_type_parse(m, S("u:8")));
    ir_block_t *b = ir_block_add(f, S("entry"));
    ir_instr_t *one = constant(m, b, "one", "u:8", 1);
    ir_instr_t *sum = binary(m, b, IR_OP_ADD, "sum", "u:8", x, one->dest,
                             false);
    ir_instr_t *ret = ir_instr_add(b, IR_OP_RETURN);
    ir_instr_operand_value(ret, sum->dest);
    ir_interval_t before = {0}, after = {0};
    ir_ranges_t *outer = ir_func_ranges(f);
    check("a wrapping add covers its type",
          ir_range_of(outer, sum->dest, nullptr, &before)
          && before.lo == 0 && before.hi == 255);
    size_t scope = ir_analysis_scope_begin();
    check("a per-query scope reuses the enclosing phase's ranges",
          ir_func_ranges(f) == outer);
    ir_analysis_scope_end(scope);
    check("closing the scope keeps the enclosing phase's ranges",
          ir_func_ranges(f) == outer);
    ir_instr_add_mod(sum, S("trap"));
    check("adding a mod rebuilds the cached ranges",
          ir_range_of(ir_func_ranges(f), sum->dest, nullptr, &after)
          && after.lo == 1 && after.hi == 255);
    ir_ctx_free(ctx);
}

static void test_phi_ranges_merge_reachable_arms(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("choose"),
        ir_type_parse(m, S("(bool) -> i:8")), IR_LINK_EXTERNAL);
    ir_value_t *condition = ir_param_add(f, S("condition"),
                                         ir_type_parse(m, S("bool")));
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *left = ir_block_add(f, S("left"));
    ir_block_t *right = ir_block_add(f, S("right"));
    ir_block_t *join = ir_block_add(f, S("join"));
    ir_instr_t *two = constant(m, entry, "two", "i:8", 2);
    ir_instr_t *nine = constant(m, entry, "nine", "i:8", 9);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, left);
    ir_instr_operand_block(branch, right);
    ir_instr_t *left_jump = ir_instr_add(left, IR_OP_JUMP);
    ir_instr_operand_block(left_jump, join);
    ir_instr_t *right_jump = ir_instr_add(right, IR_OP_JUMP);
    ir_instr_operand_block(right_jump, join);
    ir_instr_t *phi = ir_instr_add(join, IR_OP_PHI);
    ir_instr_set_dest(phi, S("selected"), ir_type_parse(m, S("i:8")));
    ir_instr_operand_phi_arm(phi, left, two->dest);
    ir_instr_operand_phi_arm(phi, right, nine->dest);
    ir_instr_t *ret = ir_instr_add(join, IR_OP_RETURN);
    ir_instr_operand_value(ret, phi->dest);
    check("phi ranges merge their predecessor arms",
          range_is(f, phi->dest, 2, 9));
    ir_ctx_free(ctx);
}

static void test_loop_induction_is_bounded_by_its_guard(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("count"), ir_type_parse(m, S("() -> u:32")),
                               IR_LINK_EXTERNAL);
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *header = ir_block_add(f, S("header"));
    ir_block_t *body = ir_block_add(f, S("body"));
    ir_block_t *exit = ir_block_add(f, S("exit"));
    ir_instr_t *zero = constant(m, entry, "zero", "u:32", 0);
    ir_instr_t *ten = constant(m, entry, "ten", "u:32", 10);
    ir_instr_t *one = constant(m, entry, "one", "u:32", 1);
    ir_instr_t *enter = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(enter, header);
    ir_instr_t *phi = ir_instr_add(header, IR_OP_PHI);
    ir_instr_set_dest(phi, S("i"), ir_type_parse(m, S("u:32")));
    ir_instr_t *below = binary(m, header, IR_OP_LT, "below", "bool",
                               phi->dest, ten->dest, false);
    ir_instr_t *test = ir_instr_add(header, IR_OP_BRANCH);
    ir_instr_operand_value(test, below->dest);
    ir_instr_operand_block(test, body);
    ir_instr_operand_block(test, exit);
    ir_instr_t *next = binary(m, body, IR_OP_ADD, "next", "u:32", phi->dest,
                              one->dest, true);
    ir_instr_t *back = ir_instr_add(body, IR_OP_JUMP);
    ir_instr_operand_block(back, header);
    ir_instr_operand_phi_arm(phi, entry, zero->dest);
    ir_instr_operand_phi_arm(phi, body, next->dest);
    ir_instr_t *ret = ir_instr_add(exit, IR_OP_RETURN);
    ir_instr_operand_value(ret, phi->dest);
    check("a guarded induction variable converges to its guard",
          range_is(f, phi->dest, 0, 10));
    check("the guarded increment cannot overflow",
          overflow_of(next) == IR_OVERFLOW_NEVER);
    ir_ctx_free(ctx);
}

static void test_unconditional_block_chain(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> i:8")),
                               IR_LINK_EXTERNAL);
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *exit = ir_block_add(f, S("exit"));
    ir_instr_t *left = constant(m, entry, "left", "i:8", 10);
    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, exit);
    ir_instr_t *right = constant(m, exit, "right", "i:8", 5);
    ir_instr_t *sum = binary(m, exit, IR_OP_ADD, "sum", "i:8", left->dest,
                             right->dest, true);
    ir_instr_t *ret = ir_instr_add(exit, IR_OP_RETURN);
    ir_instr_operand_value(ret, sum->dest);
    check("an unconditional multi-block path is exact",
          range_is(f, sum->dest, 15, 15));
    ir_ctx_free(ctx);
}

static void test_overflow_classification(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"), ir_type_parse(m, S("() -> i:8")),
                               IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, S("entry"));
    ir_instr_t *left = constant(m, b, "left", "i:8", 127);
    ir_instr_t *right = constant(m, b, "right", "i:8", 1);
    ir_instr_t *add = binary(m, b, IR_OP_ADD, "sum", "i:8", left->dest,
                             right->dest, true);
    check("statically overflowing arithmetic always overflows",
          overflow_of(add) == IR_OVERFLOW_ALWAYS);
    ir_instr_t *literal = ir_instr_add(b, IR_OP_CONST);
    ir_instr_set_dest(literal, S("wide"), ir_type_parse(m, S("i:8")));
    ir_instr_operand_int_lit(literal, (ir_lit_t){
                                          .mag = ((ir_uint_t)1 << 64) + 127});
    check("an unrepresentable literal does not wrap into a narrow range",
          range_is(f, literal->dest, -128, 127));
    ir_ctx_free(ctx);
}

static void test_int64_domain_boundaries(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"),
        ir_type_parse(m, S("(i:64, u:64) -> void")), IR_LINK_EXTERNAL);
    ir_value_t *signed_value = ir_param_add(f, S("signed_value"),
                                            ir_type_parse(m, S("i:64")));
    ir_value_t *unsigned_value = ir_param_add(f, S("unsigned_value"),
                                              ir_type_parse(m, S("u:64")));
    ir_block_add(f, S("entry"));
    check("signed 64-bit parameters use the complete range",
          range_is(f, signed_value, INT64_MIN, INT64_MAX));
    check("unsigned 64-bit parameters use the complete range",
          range_is(f, unsigned_value, 0, ((ir_int_t)1 << 64) - 1));

    ir_func_t *overflow = ir_func_add(m, S("overflow"),
        ir_type_parse(m, S("() -> i:64")), IR_LINK_EXTERNAL);
    ir_block_t *block = ir_block_add(overflow, S("entry"));
    ir_instr_t *max_value = constant(m, block, "max", "i:64", INT64_MAX);
    ir_instr_t *one = constant(m, block, "one", "i:64", 1);
    ir_instr_t *add = binary(m, block, IR_OP_ADD, "overflowing", "i:64",
                             max_value->dest, one->dest, true);
    check("i64 trapping overflow is classified as certain",
          overflow_of(add) == IR_OVERFLOW_ALWAYS);
    ir_ctx_free(ctx);
}

static void test_wide_multiplication_stays_conservative(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &ir_profile_bce, S("test"));
    ir_func_t *f = ir_func_add(m, S("f"),
        ir_type_parse(m, S("(u:64, u:64) -> u:64")), IR_LINK_EXTERNAL);
    ir_value_t *left = ir_param_add(f, S("left"), ir_type_parse(m, S("u:64")));
    ir_value_t *right = ir_param_add(f, S("right"),
                                     ir_type_parse(m, S("u:64")));
    ir_block_t *b = ir_block_add(f, S("entry"));
    ir_instr_t *product = binary(m, b, IR_OP_MUL, "product", "u:64", left,
                                 right, true);
    check("a product of unknown u64 values keeps the full range",
          range_is(f, product->dest, 0, ((ir_int_t)1 << 64) - 1));
    check("a product of unknown u64 values may overflow",
          overflow_of(product) != IR_OVERFLOW_NEVER);
    ir_ctx_free(ctx);
}

static void test_named_integer_range(void)
{
    static const ir_scalar_t scalars[] = {
        {S("int"), IR_CLASS_SINT, 64, IR_ROLE_NONE},
    };
    static const ir_profile_t profile = {
        .name = S("named-int-test"),
        .n_scalars = sizeof(scalars) / sizeof(scalars[0]),
        .scalars = scalars,
    };
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("ranges"), &profile, S("test"));
    ir_func_t *f = ir_func_add(m, S("named_int"), ir_type_parse(m, S("() -> int")),
                               IR_LINK_EXTERNAL);
    ir_block_t *block = ir_block_add(f, S("entry"));
    ir_instr_t *one = constant(m, block, "one", "int", 1);
    ir_instr_t *two = constant(m, block, "two", "int", 2);
    ir_instr_t *sum = binary(m, block, IR_OP_ADD, "sum", "int", one->dest,
                             two->dest, true);
    check("named int arithmetic receives exact range facts",
          range_is(f, sum->dest, 3, 3));
    ir_ctx_free(ctx);
}

static void test_loop_index_proofs(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("index_ranges"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    th_type *wide = ir_type_parse(m, S("u:128"));
    th_type *array = ir_type_parse(m, S("array[u:64]"));
    th_type *reference = ir_type_parse(m, S("ref[array[u:64]]"));
    ir_func_t *home = ir_func_add(m, S("home"),
        ir_type_parse(m, S("(u:128, u:64) -> u:64")), IR_LINK_INTERNAL);
    ir_value_t *home_hash = ir_param_add(home, S("hash"), wide);
    ir_value_t *home_size = ir_param_add(home, S("size"), word);
    ir_block_t *home_entry = ir_block_add(home, S("entry"));
    ir_instr_t *home_one = ir_instr_add(home_entry, IR_OP_CONST);
    ir_instr_set_dest(home_one, S("one"), word);
    ir_instr_operand_int(home_one, 1);
    ir_instr_t *home_mask = ir_instr_add(home_entry, IR_OP_SUB);
    ir_instr_set_dest(home_mask, S("mask"), word);
    ir_instr_operand_value(home_mask, home_size);
    ir_instr_operand_value(home_mask, home_one->dest);
    ir_instr_add_mod(home_mask, S("trap"));
    ir_instr_t *wide_mask = ir_instr_add(home_entry, IR_OP_CAST);
    ir_instr_set_dest(wide_mask, S("wide_mask"), wide);
    ir_instr_operand_value(wide_mask, home_mask->dest);
    ir_instr_t *home_value = ir_instr_add(home_entry, IR_OP_AND);
    ir_instr_set_dest(home_value, S("value"), wide);
    ir_instr_operand_value(home_value, home_hash);
    ir_instr_operand_value(home_value, wide_mask->dest);
    ir_instr_t *narrow = ir_instr_add(home_entry, IR_OP_CAST);
    ir_instr_set_dest(narrow, S("narrow"), word);
    ir_instr_operand_value(narrow, home_value->dest);
    ir_instr_t *home_return = ir_instr_add(home_entry, IR_OP_RETURN);
    ir_instr_operand_value(home_return, narrow->dest);

    ir_func_t *f = ir_func_add(m, S("probe"),
        ir_type_parse(m, S("(ref[array[u:64]], ref[array[u:64]], u:128, bool) -> void")),
        IR_LINK_EXTERNAL);
    ir_value_t *slots = ir_param_add(f, S("slots"), reference);
    ir_value_t *other = ir_param_add(f, S("other"), reference);
    ir_value_t *hash = ir_param_add(f, S("hash"), wide);
    ir_value_t *condition = ir_param_add(f, S("condition"),
                                         ir_type_parse(m, S("bool")));
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *header = ir_block_add(f, S("header"));
    ir_block_t *body = ir_block_add(f, S("body"));
    ir_block_t *exit = ir_block_add(f, S("exit"));
    ir_instr_t *loaded = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(loaded, S("array"), array);
    ir_instr_operand_value(loaded, slots);
    ir_instr_t *length = ir_instr_add(entry, IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(length, S("length"), word);
    ir_instr_operand_value(length, loaded->dest);
    ir_instr_t *one = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(one, S("one"), word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(mask, S("mask"), word);
    ir_instr_operand_value(mask, length->dest);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, S("trap"));
    ir_instr_t *first = ir_instr_add(entry, IR_OP_CALL);
    ir_instr_set_dest(first, S("first"), word);
    ir_instr_operand_value(first, ir_func_address(m, home));
    ir_instr_operand_value(first, hash);
    ir_instr_operand_value(first, length->dest);
    ir_instr_t *to_header = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(to_header, header);

    ir_instr_t *at = ir_instr_add(header, IR_OP_PHI);
    ir_instr_set_dest(at, S("at"), word);
    ir_instr_operand_phi_arm(at, entry, first->dest);
    ir_instr_t *branch = ir_instr_add(header, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, body);
    ir_instr_operand_block(branch, exit);

    ir_instr_t *index = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(index, S("slot"), ir_type_parse(m, S("ref[u:64]")));
    ir_instr_operand_value(index, slots);
    ir_instr_operand_value(index, at->dest);
    ir_instr_t *wrong_array = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(wrong_array, S("wrong_array"), ir_type_parse(m,
                                                                S("ref[u:64]")));
    ir_instr_operand_value(wrong_array, other);
    ir_instr_operand_value(wrong_array, at->dest);
    ir_instr_t *unchecked = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(unchecked, S("unchecked"), ir_type_parse(m,
                                                             S("ref[u:64]")));
    ir_instr_operand_value(unchecked, slots);
    ir_instr_operand_value(unchecked, hash);
    ir_instr_t *sum = ir_instr_add(body, IR_OP_ADD);
    ir_instr_set_dest(sum, S("sum"), word);
    ir_instr_operand_value(sum, at->dest);
    ir_instr_operand_value(sum, one->dest);
    ir_instr_add_mod(sum, S("trap"));
    ir_instr_t *next = ir_instr_add(body, IR_OP_AND);
    ir_instr_set_dest(next, S("next"), word);
    ir_instr_operand_value(next, sum->dest);
    ir_instr_operand_value(next, mask->dest);
    ir_instr_operand_phi_arm(at, body, next->dest);
    ir_instr_t *two = ir_instr_add(body, IR_OP_CONST);
    ir_instr_set_dest(two, S("two"), word);
    ir_instr_operand_int(two, 2);
    ir_instr_t *wrapped = ir_instr_add(body, IR_OP_SUB);
    ir_instr_set_dest(wrapped, S("wrapped"), word);
    ir_instr_operand_value(wrapped, length->dest);
    ir_instr_operand_value(wrapped, two->dest);
    ir_instr_add_mod(wrapped, S("wrap"));
    ir_instr_t *wrap_index = ir_instr_add(body, IR_OP_AND);
    ir_instr_set_dest(wrap_index, S("wrap_index"), word);
    ir_instr_operand_value(wrap_index, sum->dest);
    ir_instr_operand_value(wrap_index, wrapped->dest);
    ir_instr_t *unsafe = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(unsafe, S("unsafe"), ir_type_parse(m, S("ref[u:64]")));
    ir_instr_operand_value(unsafe, slots);
    ir_instr_operand_value(unsafe, wrap_index->dest);
    ir_instr_t *back = ir_instr_add(body, IR_OP_JUMP);
    ir_instr_operand_block(back, header);
    ir_instr_add(exit, IR_OP_RETURN);

    ir_summaries_t *summaries = bind_summaries(m);
    ir_ranges_t *proofs = ir_ranges_build(f);
    check("checked mask and pure call bound an SSA loop index",
          ir_range_index_proven(proofs, index));
    check("a different Array does not inherit the bound",
          !ir_range_index_proven(proofs, wrong_array));
    check("an unrelated unsigned index keeps its bounds check",
          !ir_range_index_proven(proofs, unchecked));
    check("wrapping subtraction cannot establish a nonzero bound",
          !ir_range_index_proven(proofs, unsafe));
    ir_ranges_free(proofs);
    ir_summaries_free(summaries);
    ir_ctx_free(ctx);
}

static void test_nested_checked_unsigned_subtraction(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("nested_sub"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    ir_func_t *home = ir_func_add(m, S("home"),
        ir_type_parse(m, S("(u:64, u:64) -> u:64")), IR_LINK_INTERNAL);
    ir_value_t *hash = ir_param_add(home, S("hash"), word);
    ir_value_t *capacity = ir_param_add(home, S("capacity"), word);
    ir_block_t *home_entry = ir_block_add(home, S("entry"));
    ir_instr_t *one = ir_instr_add(home_entry, IR_OP_CONST);
    ir_instr_set_dest(one, S("one"), word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(home_entry, IR_OP_SUB);
    ir_instr_set_dest(mask, S("mask"), word);
    ir_instr_operand_value(mask, capacity);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, S("trap"));
    ir_instr_t *masked = ir_instr_add(home_entry, IR_OP_AND);
    ir_instr_set_dest(masked, S("masked"), word);
    ir_instr_operand_value(masked, hash);
    ir_instr_operand_value(masked, mask->dest);
    ir_instr_t *home_return = ir_instr_add(home_entry, IR_OP_RETURN);
    ir_instr_operand_value(home_return, masked->dest);

    ir_func_t *f = ir_func_add(m, S("distance"),
        ir_type_parse(m, S("(u:64, u:64, u:64) -> u:64")), IR_LINK_EXTERNAL);
    ir_value_t *size = ir_param_add(f, S("size"), word);
    ir_value_t *at = ir_param_add(f, S("at"), word);
    ir_value_t *input = ir_param_add(f, S("input"), word);
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_instr_t *start = ir_instr_add(entry, IR_OP_CALL);
    ir_instr_set_dest(start, S("start"), word);
    ir_instr_operand_value(start, ir_func_address(m, home));
    ir_instr_operand_value(start, input);
    ir_instr_operand_value(start, size);
    ir_instr_t *difference = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(difference, S("difference"), word);
    ir_instr_operand_value(difference, start->dest);
    ir_instr_operand_value(difference, at);
    ir_instr_add_mod(difference, S("trap"));
    ir_instr_t *safe = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(safe, S("safe"), word);
    ir_instr_operand_value(safe, size);
    ir_instr_operand_value(safe, difference->dest);
    ir_instr_add_mod(safe, S("trap"));
    ir_instr_t *unbounded = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(unbounded, S("unbounded"), word);
    ir_instr_operand_value(unbounded, at);
    ir_instr_operand_value(unbounded, difference->dest);
    ir_instr_add_mod(unbounded, S("trap"));
    ir_instr_t *wrapped = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(wrapped, S("wrapped"), word);
    ir_instr_operand_value(wrapped, start->dest);
    ir_instr_operand_value(wrapped, at);
    ir_instr_add_mod(wrapped, S("wrap"));
    ir_instr_t *unchecked_inner = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(unchecked_inner, S("unchecked_inner"), word);
    ir_instr_operand_value(unchecked_inner, size);
    ir_instr_operand_value(unchecked_inner, wrapped->dest);
    ir_instr_add_mod(unchecked_inner, S("trap"));
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, safe->dest);

    ir_summaries_t *summaries = bind_summaries(m);
    check("bounded checked difference cannot underflow outer subtraction",
          sub_safe(safe));
    check("unbounded outer operand retains its subtraction trap",
          !sub_safe(unbounded));
    check("wrapping inner subtraction cannot justify dropping a trap",
          !sub_safe(unchecked_inner));
    ir_summaries_free(summaries);
    ir_ctx_free(ctx);
}

static void test_checked_index_seeds_loop_bounds(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("checked_index"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    th_type *array = ir_type_parse(m, S("array[u:64]"));
    th_type *reference = ir_type_parse(m, S("ref[array[u:64]]"));
    th_type *element = ir_type_parse(m, S("ref[u:64]"));
    ir_func_t *f = ir_func_add(m, S("shift"),
        ir_type_parse(m, S("(ref[array[u:64]], ref[array[u:64]], u:64, bool) -> void")),
        IR_LINK_EXTERNAL);
    ir_value_t *slots = ir_param_add(f, S("slots"), reference);
    ir_value_t *other = ir_param_add(f, S("other"), reference);
    ir_value_t *slot = ir_param_add(f, S("slot"), word);
    ir_value_t *condition = ir_param_add(f, S("condition"),
                                         ir_type_parse(m, S("bool")));
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *header = ir_block_add(f, S("header"));
    ir_block_t *body = ir_block_add(f, S("body"));
    ir_block_t *exit = ir_block_add(f, S("exit"));
    ir_instr_t *loaded = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(loaded, S("array"), array);
    ir_instr_operand_value(loaded, slots);
    ir_instr_t *length = ir_instr_add(entry, IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(length, S("length"), word);
    ir_instr_operand_value(length, loaded->dest);
    ir_instr_t *guard = ir_instr_add(entry, IR_OP_INDEX);
    ir_instr_set_dest(guard, S("checked"), element);
    ir_instr_operand_value(guard, slots);
    ir_instr_operand_value(guard, slot);
    ir_instr_t *one = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(one, S("one"), word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(mask, S("mask"), word);
    ir_instr_operand_value(mask, length->dest);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, S("trap"));
    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, header);
    ir_instr_t *hole = ir_instr_add(header, IR_OP_PHI);
    ir_instr_set_dest(hole, S("hole"), word);
    ir_instr_operand_phi_arm(hole, entry, slot);
    ir_instr_t *branch = ir_instr_add(header, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, body);
    ir_instr_operand_block(branch, exit);
    ir_instr_t *shift = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(shift, S("shift"), element);
    ir_instr_operand_value(shift, slots);
    ir_instr_operand_value(shift, hole->dest);
    ir_instr_t *wrong = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(wrong, S("wrong"), element);
    ir_instr_operand_value(wrong, other);
    ir_instr_operand_value(wrong, hole->dest);
    ir_instr_t *sum = ir_instr_add(body, IR_OP_ADD);
    ir_instr_set_dest(sum, S("sum"), word);
    ir_instr_operand_value(sum, hole->dest);
    ir_instr_operand_value(sum, one->dest);
    ir_instr_add_mod(sum, S("trap"));
    ir_instr_t *next = ir_instr_add(body, IR_OP_AND);
    ir_instr_set_dest(next, S("next"), word);
    ir_instr_operand_value(next, sum->dest);
    ir_instr_operand_value(next, mask->dest);
    ir_instr_operand_phi_arm(hole, body, next->dest);
    ir_instr_t *back = ir_instr_add(body, IR_OP_JUMP);
    ir_instr_operand_block(back, header);
    ir_instr_add(exit, IR_OP_RETURN);

    ir_ranges_t *proofs = ir_ranges_build(f);
    check("initial checked access keeps its bounds check",
          !ir_range_index_proven(proofs, guard));
    check("checked access bounds a loop-carried index",
          ir_range_index_proven(proofs, shift));
    check("checked access does not bound a different sequence",
          !ir_range_index_proven(proofs, wrong));
    ir_ranges_free(proofs);
    ir_ctx_free(ctx);
}

static void test_index_proof_invalidated_by_effects(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("index_effects"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    th_type *array = ir_type_parse(m, S("array[u:64]"));
    th_type *reference = ir_type_parse(m, S("ref[array[u:64]]"));
    ir_func_t *opaque = ir_func_add(m, S("opaque"),
        ir_type_parse(m, S("() -> void")), IR_LINK_EXTERNAL);
    for (unsigned mode = 0; mode < 3; mode++) {
        core_string_t name = mode == 0 ? S("with_write")
            : mode == 1 ? S("with_call") : S("with_local_write");
        ir_func_t *f = ir_func_add(m, name,
            ir_type_parse(m, S("(ref[array[u:64]], u:64) -> void")),
            IR_LINK_EXTERNAL);
        ir_value_t *slots = ir_param_add(f, S("slots"), reference);
        ir_value_t *hash = ir_param_add(f, S("hash"), word);
        ir_block_t *block = ir_block_add(f, S("entry"));
        ir_instr_t *loaded = ir_instr_add(block, IR_OP_LOAD);
        ir_instr_set_dest(loaded, S("array"), array);
        ir_instr_operand_value(loaded, slots);
        ir_instr_t *length = ir_instr_add(block, IR_OP_SEQUENCE_LENGTH);
        ir_instr_set_dest(length, S("length"), word);
        ir_instr_operand_value(length, loaded->dest);
        ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
        ir_instr_set_dest(one, S("one"), word);
        ir_instr_operand_int(one, 1);
        ir_instr_t *mask = ir_instr_add(block, IR_OP_SUB);
        ir_instr_set_dest(mask, S("mask"), word);
        ir_instr_operand_value(mask, length->dest);
        ir_instr_operand_value(mask, one->dest);
        ir_instr_add_mod(mask, S("trap"));
        ir_instr_t *at = ir_instr_add(block, IR_OP_AND);
        ir_instr_set_dest(at, S("at"), word);
        ir_instr_operand_value(at, hash);
        ir_instr_operand_value(at, mask->dest);
        if (mode == 1) {
            ir_instr_t *call = ir_instr_add(block, IR_OP_CALL);
            ir_instr_operand_value(call, ir_func_address(m, opaque));
        }
        else if (mode == 0) {
            ir_instr_t *store = ir_instr_add(block, IR_OP_STORE);
            ir_instr_operand_value(store, slots);
            ir_instr_operand_value(store, loaded->dest);
        }
        else {
            ir_local_t *local = ir_local_add(f, S("replacement"), array);
            ir_instr_t *assign = ir_instr_add(block, IR_OP_COPY);
            ir_instr_set_dest_local(assign, local);
            ir_instr_operand_value(assign, loaded->dest);
        }
        ir_instr_t *index = ir_instr_add(block, IR_OP_INDEX);
        ir_instr_set_dest(index, S("slot"), ir_type_parse(m, S("ref[u:64]")));
        ir_instr_operand_value(index, slots);
        ir_instr_operand_value(index, at->dest);
        ir_instr_add(block, IR_OP_RETURN);
        ir_ranges_t *proofs = ir_ranges_build(f);
        check(mode == 0 ? "writes invalidate index bounds proofs"
              : mode == 1 ? "opaque calls invalidate index bounds proofs"
              : "local assignments invalidate index bounds proofs",
              !ir_range_index_proven(proofs, index));
        ir_ranges_free(proofs);
    }
    ir_ctx_free(ctx);
}

static void test_length_helper_and_disjoint_writes(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("length_helper"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    th_type *bucket = ir_type_parse(m, S("Bucket"));
    ir_type_define(m, bucket,
        ir_type_parse(m, S("(slots: array[u:64], count: u:64)")));
    ir_func_t *capacity = ir_func_add(m, S("capacity"),
        ir_type_parse(m, S("(Bucket) -> u:64")), IR_LINK_INTERNAL);
    ir_value_t *parameter = ir_param_add(capacity, S("bucket"), bucket);
    ir_block_t *helper_block = ir_block_add(capacity, S("entry"));
    ir_instr_t *extract = ir_instr_add(helper_block, IR_OP_EXTRACT);
    ir_instr_set_dest(extract, S("slots"), ir_type_parse(m, S("array[u:64]")));
    ir_instr_operand_value(extract, parameter);
    ir_instr_operand_name(extract, S("slots"));
    ir_instr_t *helper_length = ir_instr_add(helper_block,
                                             IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(helper_length, S("length"), word);
    ir_instr_operand_value(helper_length, extract->dest);
    ir_instr_t *helper_return = ir_instr_add(helper_block, IR_OP_RETURN);
    ir_instr_operand_value(helper_return, helper_length->dest);

    ir_func_t *f = ir_func_add(m, S("probe"),
        ir_type_parse(m, S("(Bucket, u:64) -> void")), IR_LINK_EXTERNAL);
    ir_value_t *self = ir_param_add(f, S("self"), bucket);
    ir_value_t *hash = ir_param_add(f, S("hash"), word);
    ir_block_t *block = ir_block_add(f, S("entry"));
    ir_instr_t *length = ir_instr_add(block, IR_OP_CALL);
    ir_instr_set_dest(length, S("length"), word);
    ir_instr_operand_value(length, ir_func_address(m, capacity));
    ir_instr_operand_value(length, self);
    ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
    ir_instr_set_dest(one, S("one"), word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(block, IR_OP_SUB);
    ir_instr_set_dest(mask, S("mask"), word);
    ir_instr_operand_value(mask, length->dest);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, S("trap"));
    ir_instr_t *at = ir_instr_add(block, IR_OP_AND);
    ir_instr_set_dest(at, S("at"), word);
    ir_instr_operand_value(at, hash);
    ir_instr_operand_value(at, mask->dest);
    ir_instr_t *address = ir_instr_add(block, IR_OP_ADDRESS_OF);
    ir_instr_set_dest(address, S("address"), ir_type_parse(m, S("ref[Bucket]")));
    ir_instr_operand_value(address, self);
    ir_instr_t *slots = ir_instr_add(block, IR_OP_FIELD);
    ir_instr_set_dest(slots, S("slots_address"),
                      ir_type_parse(m, S("ref[array[u:64]]")));
    ir_instr_operand_value(slots, address->dest);
    ir_instr_operand_name(slots, S("slots"));
    ir_instr_t *count = ir_instr_add(block, IR_OP_FIELD);
    ir_instr_set_dest(count, S("count_address"), ir_type_parse(m,
                                                           S("ref[u:64]")));
    ir_instr_operand_value(count, address->dest);
    ir_instr_operand_name(count, S("count"));
    ir_instr_t *slots_value = ir_instr_add(block, IR_OP_LOAD);
    ir_instr_set_dest(slots_value, S("slots_value"),
                      ir_type_parse(m, S("array[u:64]")));
    ir_instr_operand_value(slots_value, slots->dest);
    ir_instr_t *count_store = ir_instr_add(block, IR_OP_STORE);
    ir_instr_operand_value(count_store, count->dest);
    ir_instr_operand_value(count_store, one->dest);
    ir_instr_t *second_address = ir_instr_add(block, IR_OP_ADDRESS_OF);
    ir_instr_set_dest(second_address, S("second_address"),
                      ir_type_parse(m, S("ref[Bucket]")));
    ir_instr_operand_value(second_address, self);
    ir_instr_t *second_slots = ir_instr_add(block, IR_OP_FIELD);
    ir_instr_set_dest(second_slots, S("second_slots"),
                      ir_type_parse(m, S("ref[array[u:64]]")));
    ir_instr_operand_value(second_slots, second_address->dest);
    ir_instr_operand_name(second_slots, S("slots"));
    ir_instr_t *element = ir_instr_add(block, IR_OP_INDEX);
    ir_instr_set_dest(element, S("element"), ir_type_parse(m, S("ref[u:64]")));
    ir_instr_operand_value(element, second_slots->dest);
    ir_instr_operand_value(element, at->dest);
    ir_instr_t *element_store = ir_instr_add(block, IR_OP_STORE);
    ir_instr_operand_value(element_store, element->dest);
    ir_instr_operand_value(element_store, one->dest);
    ir_instr_t *index = ir_instr_add(block, IR_OP_INDEX);
    ir_instr_set_dest(index, S("checked"), ir_type_parse(m, S("ref[u:64]")));
    ir_instr_operand_value(index, slots->dest);
    ir_instr_operand_value(index, at->dest);
    ir_instr_add(block, IR_OP_RETURN);

    ir_summaries_t *summaries = bind_summaries(m);
    ir_ranges_t *proofs = ir_ranges_build(f);
    check("a pure helper returns the length of the indexed field",
          ir_range_index_proven(proofs, index));
    ir_ranges_free(proofs);
    count_store->operands[0].value = slots->dest;
    count_store->operands[1].value = slots_value->dest;
    element->operands[1].value = hash;
    proofs = ir_ranges_build(f);
    check("replacing the measured Array invalidates the helper bound",
          !ir_range_index_proven(proofs, index));
    ir_ranges_free(proofs);
    ir_summaries_free(summaries);
    ir_ctx_free(ctx);
}


static void test_branch_guard_proves_index(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("guarded"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    th_type *array = ir_type_parse(m, S("array[u:64]"));
    ir_func_t *f = ir_func_add(m, S("guarded"),
        ir_type_parse(m, S("(ref[array[u:64]], u:64) -> u:64")),
        IR_LINK_EXTERNAL);
    ir_value_t *slots = ir_param_add(f, S("slots"),
                                     ir_type_parse(m, S("ref[array[u:64]]")));
    ir_value_t *position = ir_param_add(f, S("position"), word);
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_block_t *inside = ir_block_add(f, S("inside"));
    ir_block_t *outside = ir_block_add(f, S("outside"));
    ir_instr_t *loaded = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(loaded, S("array"), array);
    ir_instr_operand_value(loaded, slots);
    ir_instr_t *length = ir_instr_add(entry, IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(length, S("length"), word);
    ir_instr_operand_value(length, loaded->dest);
    ir_instr_t *below = binary(m, entry, IR_OP_LT, "below", "bool", position,
                               length->dest, false);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, below->dest);
    ir_instr_operand_block(branch, inside);
    ir_instr_operand_block(branch, outside);
    ir_instr_t *guarded = binary(m, inside, IR_OP_SEQUENCE_GET, "guarded",
                                 "u:64", loaded->dest, position, false);
    ir_instr_t *inside_return = ir_instr_add(inside, IR_OP_RETURN);
    ir_instr_operand_value(inside_return, guarded->dest);
    ir_instr_t *unguarded = binary(m, outside, IR_OP_SEQUENCE_GET,
                                   "unguarded", "u:64", loaded->dest, position,
                                   false);
    ir_instr_t *outside_return = ir_instr_add(outside, IR_OP_RETURN);
    ir_instr_operand_value(outside_return, unguarded->dest);

    ir_ranges_t *proofs = ir_ranges_build(f);
    check("a dominating index < length test proves the access",
          ir_range_index_proven(proofs, guarded));
    check("the failing arm of the test keeps its bounds check",
          !ir_range_index_proven(proofs, unguarded));
    ir_ranges_free(proofs);
    ir_ctx_free(ctx);
}

static void test_element_reads_see_their_own_store(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, S("elements"), &ir_profile_bce,
                                   S("test"));
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, S("u:64"));
    ir_func_t *f = ir_func_add(m, S("elements"),
        ir_type_parse(m, S("(ref[array[u:64]]) -> u:64")), IR_LINK_EXTERNAL);
    ir_value_t *slots = ir_param_add(f, S("slots"),
                                     ir_type_parse(m, S("ref[array[u:64]]")));
    ir_block_t *entry = ir_block_add(f, S("entry"));
    ir_instr_t *zero = constant(m, entry, "zero", "u:64", 0);
    ir_instr_t *one = constant(m, entry, "one", "u:64", 1);
    ir_instr_t *five = constant(m, entry, "five", "u:64", 5);
    ir_instr_t *nine = constant(m, entry, "nine", "u:64", 9);
    ir_instr_t *first = binary(m, entry, IR_OP_INDEX, "first", "ref[u:64]",
                               slots, zero->dest, false);
    ir_instr_t *store_first = ir_instr_add(entry, IR_OP_STORE);
    ir_instr_operand_value(store_first, first->dest);
    ir_instr_operand_value(store_first, five->dest);
    ir_instr_t *second = binary(m, entry, IR_OP_INDEX, "second", "ref[u:64]",
                                slots, one->dest, false);
    ir_instr_t *store_second = ir_instr_add(entry, IR_OP_STORE);
    ir_instr_operand_value(store_second, second->dest);
    ir_instr_operand_value(store_second, nine->dest);
    ir_instr_t *again = binary(m, entry, IR_OP_INDEX, "again", "ref[u:64]",
                               slots, zero->dest, false);
    ir_instr_t *read = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(read, S("read"), word);
    ir_instr_operand_value(read, again->dest);
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, read->dest);
    check("an element read sees its own store past a disjoint element",
          range_is(f, read->dest, 5, 5));
    ir_ctx_free(ctx);
}

int main(void)
{
    test_element_reads_see_their_own_store();
    test_branch_guard_proves_index();
    test_trapping_add_bounds();
    test_branch_path_refines_integer_ranges();
    test_negated_guards_and_earlier_traps_refine();
    test_cached_ranges_follow_mods_and_scopes();
    test_phi_ranges_merge_reachable_arms();
    test_loop_induction_is_bounded_by_its_guard();
    test_unconditional_block_chain();
    test_overflow_classification();
    test_int64_domain_boundaries();
    test_named_integer_range();
    test_wide_multiplication_stays_conservative();
    test_loop_index_proofs();
    test_nested_checked_unsigned_subtraction();
    test_checked_index_seeds_loop_bounds();
    test_index_proof_invalidated_by_effects();
    test_length_helper_and_disjoint_writes();
    return report("IR ranges");
}
