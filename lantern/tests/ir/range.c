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

static void test_branching_is_unsupported(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *m = ir_module_new(ctx, "ranges", &ir_profile_bce, "test");
    ir_func_t *f = ir_func_add(m, "f", ir_type_parse(m, "(bool) -> void"),
                               IR_LINK_EXTERNAL);
    ir_value_t *condition = ir_param_add(f, "condition",
                                         ir_type_parse(m, "bool"));
    ir_block_t *entry = ir_block_add(f, "entry");
    ir_block_t *left = ir_block_add(f, "left");
    ir_block_t *right = ir_block_add(f, "right");
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_operand_value(branch, condition);
    ir_instr_operand_block(branch, left);
    ir_instr_operand_block(branch, right);
    ir_instr_add(left, IR_OP_RETURN);
    ir_instr_add(right, IR_OP_RETURN);
    ir_range_analysis_t *a = ir_range_analyze_function(f, nullptr);
    check("conditional control flow remains explicitly unsupported",
          ir_range_analysis_status(a) == IR_RANGE_UNSUPPORTED);
    ir_range_analysis_free(a);
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

int main(void)
{
    test_trapping_add_bounds();
    test_branching_is_unsupported();
    test_loop_is_unsupported();
    test_unconditional_block_chain();
    test_trapped_overflow_reports_source();
    test_wide_literal_does_not_wrap();
    test_int64_domain_boundaries();
    return report("IR ranges");
}
