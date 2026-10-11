#include "ir/range.h"
#include "harness.h"

static void test_trapping_add_bounds(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "f", ir_type_parse(m, "(i:8) -> i:8"),
                               IR_LINK_EXTERNAL);
    ir_value_t *p = ir_param_add(f, "p", ir_type_parse(m, "i:8"));
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *c = ir_instr_add(b, IR_OP_CONST);
    ir_instr_set_dest(c, "c", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(c, 100);
    ir_instr_t *add = ir_instr_add(b, IR_OP_ADD);
    ir_instr_set_dest(add, "sum", ir_type_parse(m, "i:8"));
    ir_instr_operand_value(add, p);
    ir_instr_operand_value(add, c->dest);
    ir_instr_add_mod(add, "trap");
    ir_instr_t *ret = ir_instr_add(b, IR_OP_RETURN);
    ir_instr_operand_value(ret, add->dest);

    ir_range_analysis_t *a = ir_range_analyze_function(f, nullptr);
    int64_t lo = 0, hi = 0;
    check("trapping add is fully analyzed",
          ir_range_analysis_status(a) == IR_RANGE_COMPLETE);
    check("trapping add narrows parameter by output representability",
          ir_range_analysis_bounds(a, p, &lo, &hi) && lo == -128 && hi == 27);
    check("trapping add narrows its result",
          ir_range_analysis_bounds(a, add->dest, &lo, &hi) && lo == -28
              && hi == 127);
    ir_range_analysis_free(a);
    ir_ctx_free(ctx);
}

static void test_branch_path_refines_integer_ranges(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "f", ir_type_parse(m, "(i:8) -> i:8"),
                               IR_LINK_EXTERNAL);
    ir_value_t *input = ir_param_add(f, "input", ir_type_parse(m, "i:8"));
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *positive = ir_block_add(f, "positive");
    ir_block_t *nonpositive = ir_block_add(f, "nonpositive");
    ir_instr_t *zero = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(zero, "zero", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(zero, 0);
    ir_instr_t *compare = ir_instr_add(entry, IR_OP_GT);
    ir_instr_set_dest(compare, "is_positive", ir_type_parse(m, "bool"));
    ir_instr_operand_value(compare, input);
    ir_instr_operand_value(compare, zero->dest);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, compare->dest);
    ir_instr_operand_block(branch, positive);
    ir_instr_operand_block(branch, nonpositive);
    ir_instr_t *one = ir_instr_add(positive, IR_OP_CONST);
    ir_instr_set_dest(one, "one", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(one, 1);
    ir_instr_t *sum = ir_instr_add(positive, IR_OP_ADD);
    ir_instr_set_dest(sum, "sum", ir_type_parse(m, "i:8"));
    ir_instr_operand_value(sum, input);
    ir_instr_operand_value(sum, one->dest);
    ir_instr_add_mod(sum, "trap");
    ir_instr_t *positive_return = ir_instr_add(positive, IR_OP_RETURN);
    ir_instr_operand_value(positive_return, sum->dest);
    ir_instr_t *nonpositive_return = ir_instr_add(nonpositive, IR_OP_RETURN);
    ir_instr_operand_value(nonpositive_return, input);

    ir_range_analysis_t *a = ir_range_analyze_function(f, nullptr);
    int64_t low = 0, high = 0;
    bool sum_bounds = ir_range_analysis_bounds(a, sum->dest, &low, &high);
    check("acyclic branch paths are analyzed separately",
          ir_range_analysis_status(a) == IR_RANGE_COMPLETE);
    check("the true comparison edge narrows checked arithmetic",
          sum_bounds
              && low == 2 && high == 127);
    check("path ranges merge to a conservative parameter interval",
          ir_range_analysis_bounds(a, input, &low, &high)
              && low == -128 && high == 126);
    ir_range_analysis_free(a);
    ir_ctx_free(ctx);
}

static void test_phi_ranges_merge_reachable_arms(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "choose",
        ir_type_parse(m, "(bool) -> i:8"), IR_LINK_EXTERNAL);
    ir_value_t *condition = ir_param_add(f, "condition",
                                         ir_type_parse(m, "bool"));
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *left = ir_block_add(f, "left");
    ir_block_t *right = ir_block_add(f, "right");
    ir_block_t *join = ir_block_add(f, "join");
    ir_instr_t *two = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(two, "two", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(two, 2);
    ir_instr_t *nine = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(nine, "nine", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(nine, 9);
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, left);
    ir_instr_operand_block(branch, right);
    ir_instr_t *left_jump = ir_instr_add(left, IR_OP_JUMP);
    ir_instr_operand_block(left_jump, join);
    ir_instr_t *right_jump = ir_instr_add(right, IR_OP_JUMP);
    ir_instr_operand_block(right_jump, join);
    ir_instr_t *phi = ir_instr_add(join, IR_OP_PHI);
    ir_instr_set_dest(phi, "selected", ir_type_parse(m, "i:8"));
    ir_instr_operand_phi_arm(phi, left, two->dest);
    ir_instr_operand_phi_arm(phi, right, nine->dest);
    ir_instr_t *ret = ir_instr_add(join, IR_OP_RETURN);
    ir_instr_operand_value(ret, phi->dest);

    ir_range_analysis_t *analysis = ir_range_analyze_function(f, nullptr);
    int64_t low = 0, high = 0;
    check("phi ranges include only the selected predecessor arm",
          ir_range_analysis_status(analysis) == IR_RANGE_COMPLETE
              && ir_range_analysis_bounds(analysis, phi->dest, &low, &high)
              && low == 2 && high == 9);
    ir_range_analysis_free(analysis);
    ir_ctx_free(ctx);
}

static void test_loop_is_unsupported(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "loop", ir_type_parse(m, "() -> void"),
                               IR_LINK_EXTERNAL);
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *body = ir_block_add(f, "body");
    ir_instr_t *to_body = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(to_body, body);
    ir_instr_t *back = ir_instr_add(body, IR_OP_JUMP);
    ir_instr_operand_block(back, entry);
    ir_range_analysis_t *analysis = ir_range_analyze_function(f, nullptr);
    check("cyclic control flow remains explicitly unsupported",
          ir_range_analysis_status(analysis) == IR_RANGE_UNSUPPORTED &&
              ir_range_analysis_unsupported_count(analysis) == 1);
    ir_range_analysis_free(analysis);
    ir_ctx_free(ctx);
}

static void test_unconditional_block_chain(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "f", ir_type_parse(m, "() -> i:8"),
                               IR_LINK_EXTERNAL);
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *exit = ir_block_add(f, "exit");
    ir_instr_t *left = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(left, "left", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(left, 10);
    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, exit);
    ir_instr_t *right = ir_instr_add(exit, IR_OP_CONST);
    ir_instr_set_dest(right, "right", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(right, 5);
    ir_instr_t *sum = ir_instr_add(exit, IR_OP_ADD);
    ir_instr_set_dest(sum, "sum", ir_type_parse(m, "i:8"));
    ir_instr_operand_value(sum, left->dest);
    ir_instr_operand_value(sum, right->dest);
    ir_instr_add_mod(sum, "trap");
    ir_instr_t *ret = ir_instr_add(exit, IR_OP_RETURN);
    ir_instr_operand_value(ret, sum->dest);

    ir_range_analysis_t *analysis = ir_range_analyze_function(f, nullptr);
    int64_t low = 0, high = 0;
    check("unconditional multi-block path is analyzed in execution order",
          ir_range_analysis_status(analysis) == IR_RANGE_COMPLETE &&
              ir_range_analysis_bounds(analysis, sum->dest, &low, &high) &&
              low == 15 && high == 15);
    ir_range_analysis_free(analysis);
    ir_ctx_free(ctx);
}

static void test_trapped_overflow_reports_source(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "f", ir_type_parse(m, "() -> i:8"),
                               IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *left = ir_instr_add(b, IR_OP_CONST);
    ir_instr_set_dest(left, "left", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(left, 127);
    ir_instr_t *right = ir_instr_add(b, IR_OP_CONST);
    ir_instr_set_dest(right, "right", ir_type_parse(m, "i:8"));
    ir_instr_operand_int(right, 1);
    ir_instr_t *add = ir_instr_add(b, IR_OP_ADD);
    ir_instr_set_dest(add, "sum", ir_type_parse(m, "i:8"));
    ir_instr_operand_value(add, left->dest);
    ir_instr_operand_value(add, right->dest);
    ir_instr_add_mod(add, "trap");
    add->span = ir_span_make(0, 10, 13, 2, 4, 2, 7);

    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_range_analysis_t *a = ir_range_analyze_function(f, diagnostics);
    const ir_diag_t *diag = ir_diags_count(diagnostics)
                                ? ir_diags_at(diagnostics, 0)
                                : nullptr;
    check("statically overflowing trapping arithmetic is unsatisfiable",
          ir_range_analysis_status(a) == IR_RANGE_UNSATISFIABLE);
    check("failed range constraint retains the source instruction",
          ir_range_analysis_failure_source(a) == add);
    check("range diagnostic is source-located at the failing operation",
          diag && diag->code == IR_DIAG_RANGE_UNSAT &&
              diag->primary.span.start_byte == 10 &&
              diag->primary.span.end_byte == 13);
    ir_range_analysis_free(a);
    ir_ctx_free(ctx);
}

static void test_wide_literal_does_not_wrap(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "f", ir_type_parse(m, "() -> i:8"),
                               IR_LINK_EXTERNAL);
    ir_block_t *b = ir_block_add(f, "entry");
    ir_instr_t *literal = ir_instr_add(b, IR_OP_CONST);
    ir_instr_set_dest(literal, "value", ir_type_parse(m, "i:8"));
    ir_instr_operand_int_lit(literal, (ir_lit_t){
                                         .mag = ((ir_uint_t)1 << 64) + 127});
    ir_range_analysis_t *a = ir_range_analyze_function(f, nullptr);
    check("128-bit integer literals cannot wrap into a valid narrow range",
          ir_range_analysis_status(a) == IR_RANGE_UNSATISFIABLE &&
              ir_range_analysis_failure_source(a) == literal);
    ir_range_analysis_free(a);
    ir_ctx_free(ctx);
}

static void test_int64_domain_boundaries(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(
        m, "f", ir_type_parse(m, "(i:64, u:63) -> void"),
        IR_LINK_EXTERNAL);
    ir_value_t *signed_value = ir_param_add(f, "signed_value",
                                            ir_type_parse(m, "i:64"));
    ir_value_t *unsigned_value = ir_param_add(f, "unsigned_value",
                                              ir_type_parse(m, "u:63"));
    ir_block_add(f, "entry");
    ir_range_analysis_t *a = ir_range_analyze_function(f, nullptr);
    int64_t lo = 0, hi = 0;
    check("signed 64-bit parameter uses the complete CSP range",
          ir_range_analysis_status(a) == IR_RANGE_COMPLETE &&
              ir_range_analysis_bounds(a, signed_value, &lo, &hi) &&
              lo == INT64_MIN && hi == INT64_MAX);
    check("unsigned 63-bit parameter fits the signed CSP domain",
          ir_range_analysis_bounds(a, unsigned_value, &lo, &hi) && lo == 0 &&
              hi == INT64_MAX);
    ir_range_analysis_free(a);

    ir_func_t *u64 = ir_func_add(m, "u64", ir_type_parse(m, "(u:64) -> void"),
                                 IR_LINK_EXTERNAL);
    ir_value_t *unsigned_64 = ir_param_add(u64, "value",
                                           ir_type_parse(m, "u:64"));
    ir_block_add(u64, "entry");
    a = ir_range_analyze_function(u64, nullptr);
    check("unsigned 64-bit values remain unsupported without wrapping",
          ir_range_analysis_status(a) == IR_RANGE_PARTIAL &&
              ir_range_analysis_unsupported_count(a) == 1 &&
              !ir_range_analysis_bounds(a, unsigned_64, &lo, &hi));
    ir_range_analysis_free(a);

    ir_func_t *wide_add = ir_func_add(
        m, "wide_add", ir_type_parse(m, "(i:64, i:64) -> i:64"),
        IR_LINK_EXTERNAL);
    ir_value_t *left = ir_param_add(wide_add, "left",
                                    ir_type_parse(m, "i:64"));
    ir_value_t *right = ir_param_add(wide_add, "right",
                                     ir_type_parse(m, "i:64"));
    ir_block_t *block = ir_block_add(wide_add, "entry");
    ir_instr_t *sum = ir_instr_add(block, IR_OP_ADD);
    ir_instr_set_dest(sum, "sum", ir_type_parse(m, "i:64"));
    ir_instr_operand_value(sum, left);
    ir_instr_operand_value(sum, right);
    ir_instr_add_mod(sum, "trap");
    a = ir_range_analyze_function(wide_add, nullptr);
    check("wide linear relations use checked CSP bounds",
          ir_range_analysis_status(a) == IR_RANGE_COMPLETE &&
              ir_range_analysis_unsupported_count(a) == 0 &&
              ir_range_analysis_bounds(a, sum->dest, &lo, &hi) &&
              lo == INT64_MIN && hi == INT64_MAX);
    ir_range_analysis_free(a);

    ir_func_t *overflow = ir_func_add(m, "overflow", ir_type_parse(m, "() -> i:64"),
                                      IR_LINK_EXTERNAL);
    block = ir_block_add(overflow, "entry");
    ir_instr_t *max_value = ir_instr_add(block, IR_OP_CONST);
    ir_instr_set_dest(max_value, "max", ir_type_parse(m, "i:64"));
    ir_instr_operand_int(max_value, INT64_MAX);
    ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
    ir_instr_set_dest(one, "one", ir_type_parse(m, "i:64"));
    ir_instr_operand_int(one, 1);
    ir_instr_t *overflowing_add = ir_instr_add(block, IR_OP_ADD);
    ir_instr_set_dest(overflowing_add, "overflowing", ir_type_parse(m, "i:64"));
    ir_instr_operand_value(overflowing_add, max_value->dest);
    ir_instr_operand_value(overflowing_add, one->dest);
    ir_instr_add_mod(overflowing_add, "trap");
    a = ir_range_analyze_function(overflow, nullptr);
    check("i64 trapping overflow is proven unsatisfiable",
          ir_range_analysis_status(a) == IR_RANGE_UNSATISFIABLE &&
              ir_range_analysis_failure_source(a) == overflowing_add);
    ir_range_analysis_free(a);
    ir_ctx_free(ctx);
}

static void test_named_integer_range(void)
{
    static const ir_scalar_t scalars[] = {
        {"int", IR_CLASS_SINT, 64, IR_ROLE_NONE},
    };
    static const ir_profile_t profile = {
        .name = "named-int-test",
        .n_scalars = sizeof(scalars) / sizeof(scalars[0]),
        .scalars = scalars,
    };
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &profile, "test");
    ir_func_t *f = ir_func_add(m, "named_int", ir_type_parse(m, "() -> int"),
                               IR_LINK_EXTERNAL);
    ir_block_t *block = ir_block_add(f, "entry");
    ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
    ir_instr_set_dest(one, "one", ir_type_parse(m, "int"));
    ir_instr_operand_int(one, 1);
    ir_instr_t *two = ir_instr_add(block, IR_OP_CONST);
    ir_instr_set_dest(two, "two", ir_type_parse(m, "int"));
    ir_instr_operand_int(two, 2);
    ir_instr_t *sum = ir_instr_add(block, IR_OP_ADD);
    ir_instr_set_dest(sum, "sum", ir_type_parse(m, "int"));
    ir_instr_operand_value(sum, one->dest);
    ir_instr_operand_value(sum, two->dest);
    ir_instr_add_mod(sum, "trap");

    ir_range_analysis_t *analysis = ir_range_analyze_function(f, nullptr);
    int64_t lo = 0, hi = 0;
    check("named int arithmetic receives exact range facts",
          ir_range_analysis_status(analysis) == IR_RANGE_COMPLETE
              && ir_range_analysis_bounds(analysis, sum->dest, &lo, &hi)
              && lo == 3 && hi == 3);
    ir_range_analysis_free(analysis);
    ir_ctx_free(ctx);
}

static void test_loop_index_proofs(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "index_ranges", &ir_profile_bce,
                                   "test");
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, "u:64");
    th_type *wide = ir_type_parse(m, "u:128");
    th_type *array = ir_type_parse(m, "array[u:64]");
    th_type *reference = ir_type_parse(m, "ref[array[u:64]]");
    ir_func_t *home = ir_func_add(m, "home",
        ir_type_parse(m, "(u:128, u:64) -> u:64"), IR_LINK_INTERNAL);
    ir_value_t *home_hash = ir_param_add(home, "hash", wide);
    ir_value_t *home_size = ir_param_add(home, "size", word);
    ir_block_t *home_entry = ir_block_add(home, "entry");
    ir_instr_t *home_one = ir_instr_add(home_entry, IR_OP_CONST);
    ir_instr_set_dest(home_one, "one", word);
    ir_instr_operand_int(home_one, 1);
    ir_instr_t *home_mask = ir_instr_add(home_entry, IR_OP_SUB);
    ir_instr_set_dest(home_mask, "mask", word);
    ir_instr_operand_value(home_mask, home_size);
    ir_instr_operand_value(home_mask, home_one->dest);
    ir_instr_add_mod(home_mask, "trap");
    ir_instr_t *wide_mask = ir_instr_add(home_entry, IR_OP_CAST);
    ir_instr_set_dest(wide_mask, "wide_mask", wide);
    ir_instr_operand_value(wide_mask, home_mask->dest);
    ir_instr_t *home_value = ir_instr_add(home_entry, IR_OP_AND);
    ir_instr_set_dest(home_value, "value", wide);
    ir_instr_operand_value(home_value, home_hash);
    ir_instr_operand_value(home_value, wide_mask->dest);
    ir_instr_t *narrow = ir_instr_add(home_entry, IR_OP_CAST);
    ir_instr_set_dest(narrow, "narrow", word);
    ir_instr_operand_value(narrow, home_value->dest);
    ir_instr_t *home_return = ir_instr_add(home_entry, IR_OP_RETURN);
    ir_instr_operand_value(home_return, narrow->dest);

    ir_func_t *f = ir_func_add(m, "probe",
        ir_type_parse(m, "(ref[array[u:64]], ref[array[u:64]], u:128, bool) -> void"),
        IR_LINK_EXTERNAL);
    ir_value_t *slots = ir_param_add(f, "slots", reference);
    ir_value_t *other = ir_param_add(f, "other", reference);
    ir_value_t *hash = ir_param_add(f, "hash", wide);
    ir_value_t *condition = ir_param_add(f, "condition",
                                         ir_type_parse(m, "bool"));
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *header = ir_block_add(f, "header");
    ir_block_t *body = ir_block_add(f, "body");
    ir_block_t *exit = ir_block_add(f, "exit");
    ir_instr_t *loaded = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(loaded, "array", array);
    ir_instr_operand_value(loaded, slots);
    ir_instr_t *length = ir_instr_add(entry, IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(length, "length", word);
    ir_instr_operand_value(length, loaded->dest);
    ir_instr_t *one = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(one, "one", word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(mask, "mask", word);
    ir_instr_operand_value(mask, length->dest);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, "trap");
    ir_instr_t *first = ir_instr_add(entry, IR_OP_CALL);
    ir_instr_set_dest(first, "first", word);
    ir_instr_operand_value(first, ir_func_address(m, home));
    ir_instr_operand_value(first, hash);
    ir_instr_operand_value(first, length->dest);
    ir_instr_t *to_header = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(to_header, header);

    ir_instr_t *at = ir_instr_add(header, IR_OP_PHI);
    ir_instr_set_dest(at, "at", word);
    ir_instr_operand_phi_arm(at, entry, first->dest);
    ir_instr_t *branch = ir_instr_add(header, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, body);
    ir_instr_operand_block(branch, exit);

    ir_instr_t *index = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(index, "slot", ir_type_parse(m, "ref[u:64]"));
    ir_instr_operand_value(index, slots);
    ir_instr_operand_value(index, at->dest);
    ir_instr_t *wrong_array = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(wrong_array, "wrong_array", ir_type_parse(m,
                                                                "ref[u:64]"));
    ir_instr_operand_value(wrong_array, other);
    ir_instr_operand_value(wrong_array, at->dest);
    ir_instr_t *unchecked = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(unchecked, "unchecked", ir_type_parse(m,
                                                             "ref[u:64]"));
    ir_instr_operand_value(unchecked, slots);
    ir_instr_operand_value(unchecked, hash);
    ir_instr_t *sum = ir_instr_add(body, IR_OP_ADD);
    ir_instr_set_dest(sum, "sum", word);
    ir_instr_operand_value(sum, at->dest);
    ir_instr_operand_value(sum, one->dest);
    ir_instr_add_mod(sum, "trap");
    ir_instr_t *next = ir_instr_add(body, IR_OP_AND);
    ir_instr_set_dest(next, "next", word);
    ir_instr_operand_value(next, sum->dest);
    ir_instr_operand_value(next, mask->dest);
    ir_instr_operand_phi_arm(at, body, next->dest);
    ir_instr_t *wrapped = ir_instr_add(body, IR_OP_SUB);
    ir_instr_set_dest(wrapped, "wrapped", word);
    ir_instr_operand_value(wrapped, length->dest);
    ir_instr_operand_value(wrapped, one->dest);
    ir_instr_add_mod(wrapped, "wrap");
    ir_instr_t *wrap_index = ir_instr_add(body, IR_OP_AND);
    ir_instr_set_dest(wrap_index, "wrap_index", word);
    ir_instr_operand_value(wrap_index, at->dest);
    ir_instr_operand_value(wrap_index, wrapped->dest);
    ir_instr_t *unsafe = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(unsafe, "unsafe", ir_type_parse(m, "ref[u:64]"));
    ir_instr_operand_value(unsafe, slots);
    ir_instr_operand_value(unsafe, wrap_index->dest);
    ir_instr_t *back = ir_instr_add(body, IR_OP_JUMP);
    ir_instr_operand_block(back, header);
    ir_instr_add(exit, IR_OP_RETURN);

    ir_index_proofs_t *proofs = ir_index_proofs_new(f);
    check("checked mask and pure call bound an SSA loop index",
          ir_index_proven(proofs, index));
    check("a different Array does not inherit the bound",
          !ir_index_proven(proofs, wrong_array));
    check("an unrelated unsigned index keeps its bounds check",
          !ir_index_proven(proofs, unchecked));
    check("wrapping subtraction cannot establish a nonzero bound",
          !ir_index_proven(proofs, unsafe));
    ir_index_proofs_free(proofs);
    ir_ctx_free(ctx);
}

static void test_nested_checked_unsigned_subtraction(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "nested_sub", &ir_profile_bce,
                                   "test");
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, "u:64");
    ir_func_t *home = ir_func_add(m, "home",
        ir_type_parse(m, "(u:64, u:64) -> u:64"), IR_LINK_INTERNAL);
    ir_value_t *hash = ir_param_add(home, "hash", word);
    ir_value_t *capacity = ir_param_add(home, "capacity", word);
    ir_block_t *home_entry = ir_block_add(home, "entry");
    ir_instr_t *one = ir_instr_add(home_entry, IR_OP_CONST);
    ir_instr_set_dest(one, "one", word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(home_entry, IR_OP_SUB);
    ir_instr_set_dest(mask, "mask", word);
    ir_instr_operand_value(mask, capacity);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, "trap");
    ir_instr_t *masked = ir_instr_add(home_entry, IR_OP_AND);
    ir_instr_set_dest(masked, "masked", word);
    ir_instr_operand_value(masked, hash);
    ir_instr_operand_value(masked, mask->dest);
    ir_instr_t *home_return = ir_instr_add(home_entry, IR_OP_RETURN);
    ir_instr_operand_value(home_return, masked->dest);

    ir_func_t *f = ir_func_add(m, "distance",
        ir_type_parse(m, "(u:64, u:64, u:64) -> u:64"), IR_LINK_EXTERNAL);
    ir_value_t *size = ir_param_add(f, "size", word);
    ir_value_t *at = ir_param_add(f, "at", word);
    ir_value_t *input = ir_param_add(f, "input", word);
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_instr_t *start = ir_instr_add(entry, IR_OP_CALL);
    ir_instr_set_dest(start, "start", word);
    ir_instr_operand_value(start, ir_func_address(m, home));
    ir_instr_operand_value(start, input);
    ir_instr_operand_value(start, size);
    ir_instr_t *difference = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(difference, "difference", word);
    ir_instr_operand_value(difference, start->dest);
    ir_instr_operand_value(difference, at);
    ir_instr_add_mod(difference, "trap");
    ir_instr_t *safe = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(safe, "safe", word);
    ir_instr_operand_value(safe, size);
    ir_instr_operand_value(safe, difference->dest);
    ir_instr_add_mod(safe, "trap");
    ir_instr_t *unbounded = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(unbounded, "unbounded", word);
    ir_instr_operand_value(unbounded, at);
    ir_instr_operand_value(unbounded, difference->dest);
    ir_instr_add_mod(unbounded, "trap");
    ir_instr_t *wrapped = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(wrapped, "wrapped", word);
    ir_instr_operand_value(wrapped, start->dest);
    ir_instr_operand_value(wrapped, at);
    ir_instr_add_mod(wrapped, "wrap");
    ir_instr_t *unchecked_inner = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(unchecked_inner, "unchecked_inner", word);
    ir_instr_operand_value(unchecked_inner, size);
    ir_instr_operand_value(unchecked_inner, wrapped->dest);
    ir_instr_add_mod(unchecked_inner, "trap");
    ir_instr_t *ret = ir_instr_add(entry, IR_OP_RETURN);
    ir_instr_operand_value(ret, safe->dest);

    check("bounded checked difference cannot underflow outer subtraction",
          ir_unsigned_sub_proven_safe(safe));
    check("unbounded outer operand retains its subtraction trap",
          !ir_unsigned_sub_proven_safe(unbounded));
    check("wrapping inner subtraction cannot justify dropping a trap",
          !ir_unsigned_sub_proven_safe(unchecked_inner));
    ir_ctx_free(ctx);
}

static void test_checked_index_seeds_loop_bounds(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "checked_index", &ir_profile_bce,
                                   "test");
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, "u:64");
    th_type *array = ir_type_parse(m, "array[u:64]");
    th_type *reference = ir_type_parse(m, "ref[array[u:64]]");
    th_type *element = ir_type_parse(m, "ref[u:64]");
    ir_func_t *f = ir_func_add(m, "shift",
        ir_type_parse(m, "(ref[array[u:64]], ref[array[u:64]], u:64, bool) -> void"),
        IR_LINK_EXTERNAL);
    ir_value_t *slots = ir_param_add(f, "slots", reference);
    ir_value_t *other = ir_param_add(f, "other", reference);
    ir_value_t *slot = ir_param_add(f, "slot", word);
    ir_value_t *condition = ir_param_add(f, "condition",
                                         ir_type_parse(m, "bool"));
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *header = ir_block_add(f, "header");
    ir_block_t *body = ir_block_add(f, "body");
    ir_block_t *exit = ir_block_add(f, "exit");
    ir_instr_t *loaded = ir_instr_add(entry, IR_OP_LOAD);
    ir_instr_set_dest(loaded, "array", array);
    ir_instr_operand_value(loaded, slots);
    ir_instr_t *length = ir_instr_add(entry, IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(length, "length", word);
    ir_instr_operand_value(length, loaded->dest);
    ir_instr_t *guard = ir_instr_add(entry, IR_OP_INDEX);
    ir_instr_set_dest(guard, "checked", element);
    ir_instr_operand_value(guard, slots);
    ir_instr_operand_value(guard, slot);
    ir_instr_t *one = ir_instr_add(entry, IR_OP_CONST);
    ir_instr_set_dest(one, "one", word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(entry, IR_OP_SUB);
    ir_instr_set_dest(mask, "mask", word);
    ir_instr_operand_value(mask, length->dest);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, "trap");
    ir_instr_t *jump = ir_instr_add(entry, IR_OP_JUMP);
    ir_instr_operand_block(jump, header);
    ir_instr_t *hole = ir_instr_add(header, IR_OP_PHI);
    ir_instr_set_dest(hole, "hole", word);
    ir_instr_operand_phi_arm(hole, entry, slot);
    ir_instr_t *branch = ir_instr_add(header, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, body);
    ir_instr_operand_block(branch, exit);
    ir_instr_t *shift = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(shift, "shift", element);
    ir_instr_operand_value(shift, slots);
    ir_instr_operand_value(shift, hole->dest);
    ir_instr_t *wrong = ir_instr_add(body, IR_OP_INDEX);
    ir_instr_set_dest(wrong, "wrong", element);
    ir_instr_operand_value(wrong, other);
    ir_instr_operand_value(wrong, hole->dest);
    ir_instr_t *sum = ir_instr_add(body, IR_OP_ADD);
    ir_instr_set_dest(sum, "sum", word);
    ir_instr_operand_value(sum, hole->dest);
    ir_instr_operand_value(sum, one->dest);
    ir_instr_add_mod(sum, "trap");
    ir_instr_t *next = ir_instr_add(body, IR_OP_AND);
    ir_instr_set_dest(next, "next", word);
    ir_instr_operand_value(next, sum->dest);
    ir_instr_operand_value(next, mask->dest);
    ir_instr_operand_phi_arm(hole, body, next->dest);
    ir_instr_t *back = ir_instr_add(body, IR_OP_JUMP);
    ir_instr_operand_block(back, header);
    ir_instr_add(exit, IR_OP_RETURN);

    ir_index_proofs_t *proofs = ir_index_proofs_new(f);
    check("initial checked access keeps its bounds check",
          !ir_index_proven(proofs, guard));
    check("checked access bounds a loop-carried index",
          ir_index_proven(proofs, shift));
    check("checked access does not bound a different sequence",
          !ir_index_proven(proofs, wrong));
    ir_index_proofs_free(proofs);
    ir_ctx_free(ctx);
}

static void test_index_proof_invalidated_by_effects(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "index_effects", &ir_profile_bce,
                                   "test");
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, "u:64");
    th_type *array = ir_type_parse(m, "array[u:64]");
    th_type *reference = ir_type_parse(m, "ref[array[u:64]]");
    ir_func_t *opaque = ir_func_add(m, "opaque",
        ir_type_parse(m, "() -> void"), IR_LINK_EXTERNAL);
    for (unsigned mode = 0; mode < 3; mode++) {
        const char *name = mode == 0 ? "with_write"
            : mode == 1 ? "with_call" : "with_local_write";
        ir_func_t *f = ir_func_add(m, name,
            ir_type_parse(m, "(ref[array[u:64]], u:64) -> void"),
            IR_LINK_EXTERNAL);
        ir_value_t *slots = ir_param_add(f, "slots", reference);
        ir_value_t *hash = ir_param_add(f, "hash", word);
        ir_block_t *block = ir_block_add(f, "entry");
        ir_instr_t *loaded = ir_instr_add(block, IR_OP_LOAD);
        ir_instr_set_dest(loaded, "array", array);
        ir_instr_operand_value(loaded, slots);
        ir_instr_t *length = ir_instr_add(block, IR_OP_SEQUENCE_LENGTH);
        ir_instr_set_dest(length, "length", word);
        ir_instr_operand_value(length, loaded->dest);
        ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
        ir_instr_set_dest(one, "one", word);
        ir_instr_operand_int(one, 1);
        ir_instr_t *mask = ir_instr_add(block, IR_OP_SUB);
        ir_instr_set_dest(mask, "mask", word);
        ir_instr_operand_value(mask, length->dest);
        ir_instr_operand_value(mask, one->dest);
        ir_instr_add_mod(mask, "trap");
        ir_instr_t *at = ir_instr_add(block, IR_OP_AND);
        ir_instr_set_dest(at, "at", word);
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
            ir_local_t *local = ir_local_add(f, "replacement", array);
            ir_instr_t *assign = ir_instr_add(block, IR_OP_COPY);
            ir_instr_set_dest_local(assign, local);
            ir_instr_operand_value(assign, loaded->dest);
        }
        ir_instr_t *index = ir_instr_add(block, IR_OP_INDEX);
        ir_instr_set_dest(index, "slot", ir_type_parse(m, "ref[u:64]"));
        ir_instr_operand_value(index, slots);
        ir_instr_operand_value(index, at->dest);
        ir_instr_add(block, IR_OP_RETURN);
        ir_index_proofs_t *proofs = ir_index_proofs_new(f);
        check(mode == 0 ? "writes invalidate index bounds proofs"
              : mode == 1 ? "opaque calls invalidate index bounds proofs"
              : "local assignments invalidate index bounds proofs",
              !ir_index_proven(proofs, index));
        ir_index_proofs_free(proofs);
    }
    ir_ctx_free(ctx);
}

static void test_length_helper_and_disjoint_writes(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "length_helper", &ir_profile_bce,
                                   "test");
    ir_module_establish(m, IR_PROP_SSA);
    th_type *word = ir_type_parse(m, "u:64");
    th_type *bucket = ir_type_parse(m, "Bucket");
    ir_type_define(m, bucket,
        ir_type_parse(m, "(slots: array[u:64], count: u:64)"));
    ir_func_t *capacity = ir_func_add(m, "capacity",
        ir_type_parse(m, "(Bucket) -> u:64"), IR_LINK_INTERNAL);
    ir_value_t *parameter = ir_param_add(capacity, "bucket", bucket);
    ir_block_t *helper_block = ir_block_add(capacity, "entry");
    ir_instr_t *extract = ir_instr_add(helper_block, IR_OP_EXTRACT);
    ir_instr_set_dest(extract, "slots", ir_type_parse(m, "array[u:64]"));
    ir_instr_operand_value(extract, parameter);
    ir_instr_operand_name(extract, "slots");
    ir_instr_t *helper_length = ir_instr_add(helper_block,
                                             IR_OP_SEQUENCE_LENGTH);
    ir_instr_set_dest(helper_length, "length", word);
    ir_instr_operand_value(helper_length, extract->dest);
    ir_instr_t *helper_return = ir_instr_add(helper_block, IR_OP_RETURN);
    ir_instr_operand_value(helper_return, helper_length->dest);

    ir_func_t *f = ir_func_add(m, "probe",
        ir_type_parse(m, "(Bucket, u:64) -> void"), IR_LINK_EXTERNAL);
    ir_value_t *self = ir_param_add(f, "self", bucket);
    ir_value_t *hash = ir_param_add(f, "hash", word);
    ir_block_t *block = ir_block_add(f, "entry");
    ir_instr_t *length = ir_instr_add(block, IR_OP_CALL);
    ir_instr_set_dest(length, "length", word);
    ir_instr_operand_value(length, ir_func_address(m, capacity));
    ir_instr_operand_value(length, self);
    ir_instr_t *one = ir_instr_add(block, IR_OP_CONST);
    ir_instr_set_dest(one, "one", word);
    ir_instr_operand_int(one, 1);
    ir_instr_t *mask = ir_instr_add(block, IR_OP_SUB);
    ir_instr_set_dest(mask, "mask", word);
    ir_instr_operand_value(mask, length->dest);
    ir_instr_operand_value(mask, one->dest);
    ir_instr_add_mod(mask, "trap");
    ir_instr_t *at = ir_instr_add(block, IR_OP_AND);
    ir_instr_set_dest(at, "at", word);
    ir_instr_operand_value(at, hash);
    ir_instr_operand_value(at, mask->dest);
    ir_instr_t *address = ir_instr_add(block, IR_OP_ADDRESS_OF);
    ir_instr_set_dest(address, "address", ir_type_parse(m, "ref[Bucket]"));
    ir_instr_operand_value(address, self);
    ir_instr_t *slots = ir_instr_add(block, IR_OP_FIELD);
    ir_instr_set_dest(slots, "slots_address",
                      ir_type_parse(m, "ref[array[u:64]]"));
    ir_instr_operand_value(slots, address->dest);
    ir_instr_operand_name(slots, "slots");
    ir_instr_t *count = ir_instr_add(block, IR_OP_FIELD);
    ir_instr_set_dest(count, "count_address", ir_type_parse(m,
                                                           "ref[u:64]"));
    ir_instr_operand_value(count, address->dest);
    ir_instr_operand_name(count, "count");
    ir_instr_t *slots_value = ir_instr_add(block, IR_OP_LOAD);
    ir_instr_set_dest(slots_value, "slots_value",
                      ir_type_parse(m, "array[u:64]"));
    ir_instr_operand_value(slots_value, slots->dest);
    ir_instr_t *count_store = ir_instr_add(block, IR_OP_STORE);
    ir_instr_operand_value(count_store, count->dest);
    ir_instr_operand_value(count_store, one->dest);
    ir_instr_t *second_address = ir_instr_add(block, IR_OP_ADDRESS_OF);
    ir_instr_set_dest(second_address, "second_address",
                      ir_type_parse(m, "ref[Bucket]"));
    ir_instr_operand_value(second_address, self);
    ir_instr_t *second_slots = ir_instr_add(block, IR_OP_FIELD);
    ir_instr_set_dest(second_slots, "second_slots",
                      ir_type_parse(m, "ref[array[u:64]]"));
    ir_instr_operand_value(second_slots, second_address->dest);
    ir_instr_operand_name(second_slots, "slots");
    ir_instr_t *element = ir_instr_add(block, IR_OP_INDEX);
    ir_instr_set_dest(element, "element", ir_type_parse(m, "ref[u:64]"));
    ir_instr_operand_value(element, second_slots->dest);
    ir_instr_operand_value(element, at->dest);
    ir_instr_t *element_store = ir_instr_add(block, IR_OP_STORE);
    ir_instr_operand_value(element_store, element->dest);
    ir_instr_operand_value(element_store, one->dest);
    ir_instr_t *index = ir_instr_add(block, IR_OP_INDEX);
    ir_instr_set_dest(index, "checked", ir_type_parse(m, "ref[u:64]"));
    ir_instr_operand_value(index, slots->dest);
    ir_instr_operand_value(index, at->dest);
    ir_instr_add(block, IR_OP_RETURN);

    ir_index_proofs_t *proofs = ir_index_proofs_new(f);
    check("a pure helper returns the length of the indexed field",
          ir_index_proven(proofs, index));
    ir_index_proofs_free(proofs);
    count_store->operands[0].value = slots->dest;
    count_store->operands[1].value = slots_value->dest;
    proofs = ir_index_proofs_new(f);
    check("replacing the measured Array invalidates the helper bound",
          !ir_index_proven(proofs, index));
    ir_index_proofs_free(proofs);
    ir_ctx_free(ctx);
}

int main(void)
{
    test_trapping_add_bounds();
    test_branch_path_refines_integer_ranges();
    test_phi_ranges_merge_reachable_arms();
    test_loop_is_unsupported();
    test_unconditional_block_chain();
    test_trapped_overflow_reports_source();
    test_wide_literal_does_not_wrap();
    test_int64_domain_boundaries();
    test_named_integer_range();
    test_loop_index_proofs();
    test_nested_checked_unsigned_subtraction();
    test_checked_index_seeds_loop_bounds();
    test_index_proof_invalidated_by_effects();
    test_length_helper_and_disjoint_writes();
    return report("IR ranges");
}
