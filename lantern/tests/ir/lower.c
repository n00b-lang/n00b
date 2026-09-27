#include <stdio.h>
#include <string.h>

#include "ir/libir.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
#include "scanner/token_stream.h"
#include "harness.h"

typedef struct {
    ir_value_t *left;
    ir_value_t *right;
} parameter_values_t;

typedef struct {
    size_t enters;
    size_t exits;
} visit_counts_t;

static bool
count_lower_visit(ir_lower_context_t *context, parse_visit_t *visit,
                  bool entering, void *state)
{
    (void)context;
    visit_counts_t *counts = state;
    if (!visit->annot || visit->annot->kind != PARSE_ANNOT_SCOPE_OPEN
        || !core_string_eq(visit->annot->scope_tag,
                           CORE_STRING_STATIC("sum")))
        return false;
    if (entering) counts->enters++;
    else counts->exits++;
    return true;
}

static ir_lower_result_t
lower_identity(ir_lower_context_t *context, parse_visit_t *visit,
               ir_value_t *const *operands, size_t operand_count,
               th_type *result_type, ir_span_t span, void *state)
{
    (void)context;
    (void)visit;
    (void)result_type;
    (void)span;
    (void)state;
    return operand_count
        ? (ir_lower_result_t){IR_LOWER_VALUE, operands[0]}
        : (ir_lower_result_t){IR_LOWER_FAILED, nullptr};
}

static ir_lower_result_t
lower_parameter(ir_lower_context_t *context, parse_visit_t *visit,
                ir_value_t *const *operands, size_t operand_count,
                th_type *result_type, ir_span_t span, void *state)
{
    (void)context;
    (void)operands;
    (void)result_type;
    (void)span;
    parameter_values_t *values = state;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);
    if (core_string_eq(name, CORE_STRING_STATIC("left")) && !operand_count)
        return (ir_lower_result_t){IR_LOWER_VALUE, values->left};
    if (core_string_eq(name, CORE_STRING_STATIC("right")) && !operand_count)
        return (ir_lower_result_t){IR_LOWER_VALUE, values->right};
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

static parse_annotation_t *
emit_annotation(const char *action, size_t operand_count)
{
    parse_annotation_t *annotation = parse_annot_new(PARSE_ANNOT_EMIT);
    annotation->emit_action = core_string_from_cstr(action);
    if (operand_count) {
        annotation->emit_operands = core_list_new(parse_child_ref_t);
        for (size_t i = 0; i < operand_count; i++)
            core_list_push(annotation->emit_operands,
                           PARSE_CHILD_IX((int32_t)i));
    }
    return annotation;
}

int
main(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "lower-test", &ir_profile_bce,
                                        "host");
    th_type *i32 = ir_type_parse(module, "i32");
    th_type *signature = ir_type_parse(module, "(i32, i32) -> i32");
    ir_func_t *function = ir_func_add(module, "f", signature,
                                      IR_LINK_EXTERNAL);
    ir_value_t *left = ir_param_add(function, "left", i32);
    ir_value_t *right = ir_param_add(function, "right", i32);
    ir_block_t *block = ir_block_add(function, "entry");
    int32_t file_id = ir_file_add(module, "lower.n00b");
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_lowerer_t *lowerer = ir_lowerer_new(ctx);
    ir_lower_context_t lower = {
        .module = module,
        .function = function,
        .block = block,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .reachable = true,
    };
    ir_value_t *operands[] = {left, right};

    static const char *const grammar_text
        = "@tokenizer(\"toy\")\n"
          "<sum> @scope(\"sum\") @emit(\"add\", $0, $2) ::= <left> \"+\" <right>\n"
          "<left> @emit(\"parameter\") ::= \"a\"\n"
          "<right> @emit(\"parameter\") ::= \"b\"\n";
    parse_grammar_t *grammar = parse_grammar_new();
    bool grammar_loaded = parse_bnf_load(
        core_string_from_cstr(grammar_text), CORE_STRING_STATIC("sum"),
        grammar);
    parse_token_stream_t *tokens = scan_token_stream_from_codepoints(
        CORE_STRING_STATIC("a+b"));
    parse_pwz_parser_t *parser = grammar_loaded
        ? parse_pwz_new_for_start(grammar, parse_nonterm_id(parse_find_nonterm(
                                      grammar, CORE_STRING_STATIC("sum"))))
        : nullptr;
    bool parsed = parser && parse_pwz_parse(parser, tokens);
    parse_tree_t *tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    check("a tiny annotated expression grammar parses", tree != nullptr);
    parameter_values_t parameters = {.left = left, .right = right};
    visit_counts_t visit_counts = {0};
    check("the tree driver registers frontend value callbacks",
          ir_lowerer_register(lowerer, "parameter", lower_parameter,
                              &parameters)
              && ir_lowerer_register_visit(lowerer,
                  PARSE_ANNOT_SCOPE_OPEN, count_lower_visit, &visit_counts));
    if (tree) core_tree_node_value(tree).type = i32;
    ir_lower_result_t tree_sum = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("the generic tree driver resolves annotated children bottom-up",
          tree_sum.status == IR_LOWER_VALUE && tree_sum.value
              && tree_sum.value->def
              && tree_sum.value->def->op == IR_OP_ADD);
    check("emitted instructions retain the source tree span",
          tree_sum.value && ir_span_known(tree_sum.value->def->span)
              && tree_sum.value->def->span.file == file_id
              && tree_sum.value->def->span.start_byte == 0
              && tree_sum.value->def->span.end_byte == 3);
    check("lowering hooks run on scope entry and exit",
          visit_counts.enters == 1 && visit_counts.exits == 1);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);

    check("a custom lowering action registers once",
          ir_lowerer_register(lowerer, "identity", lower_identity, nullptr)
              && !ir_lowerer_register(lowerer, "identity", lower_identity,
                                      nullptr));

    parse_annotation_t *identity = emit_annotation("identity", 1);
    ir_lower_result_t custom = ir_lower_emit(lowerer, &lower, identity, nullptr,
        operands, 1, i32, IR_SPAN_UNKNOWN);
    check("a frontend callback can return a child value",
          custom.status == IR_LOWER_VALUE && custom.value == left
              && lower.reachable);
    parse_annot_free(identity);

    parse_annotation_t *add = emit_annotation("add", 2);
    ir_lower_result_t sum = ir_lower_emit(lowerer, &lower, add, nullptr,
        operands, 2, i32, IR_SPAN_UNKNOWN);
    check("an unregistered action falls back to a builtin IR operation",
          sum.status == IR_LOWER_VALUE && sum.value && sum.value->def
              && sum.value->def->op == IR_OP_ADD
              && sum.value->def->n_operands == 2);
    parse_annot_free(add);

    parse_annotation_t *bad_arity = emit_annotation("add", 0);
    ir_lower_result_t rejected = ir_lower_emit(lowerer, &lower, bad_arity,
        nullptr, operands, 1, i32, IR_SPAN_UNKNOWN);
    check("emit lowering rejects a child-result count mismatch",
          rejected.status == IR_LOWER_FAILED);
    parse_annot_free(bad_arity);

    parse_annotation_t *ret = emit_annotation("return", 1);
    ir_value_t *return_value[] = {sum.value};
    ir_lower_result_t terminated = ir_lower_emit(lowerer, &lower, ret, nullptr,
        return_value, 1, nullptr, IR_SPAN_UNKNOWN);
    check("a builtin terminator updates reachability",
          terminated.status == IR_LOWER_TERMINATED && !lower.reachable
              && ir_block_terminator(block)
              && ir_block_terminator(block)->op == IR_OP_RETURN);
    parse_annot_free(ret);

    lower.reachable = true;
    parse_annotation_t *unknown = emit_annotation("not_a_real_op", 0);
    ir_lower_result_t failed = ir_lower_emit(lowerer, &lower, unknown, nullptr,
        nullptr, 0, nullptr, IR_SPAN_UNKNOWN);
    check("an unknown action fails through the IR diagnostics API",
          failed.status == IR_LOWER_FAILED && ir_diags_count(diagnostics) == 2);
    parse_annot_free(unknown);

    ir_diags_t *verification = ir_diags_new(ctx, nullptr);
    ir_module_establish(module, IR_PROP_PARSED);
    ir_props_t properties = ir_verify(module, verification);
    check("builtin lowering output is accepted by the Lantern verifier",
          (properties & IR_PROP_PARSED) && !ir_diags_failed(verification));
    check("the emitted basic block is structured by Lantern",
          ir_structure(module, verification)
              && (ir_module_props(module) & IR_PROP_STRUCTURED));
    ir_pdg_t *pdg = ir_pdg_of(function);
    check("Lantern builds a dependence graph from the lowered instructions",
          pdg && ir_pdg_node_count(pdg) == 3);

    ir_ctx_free(ctx);
    return report("IR lowering");
}
