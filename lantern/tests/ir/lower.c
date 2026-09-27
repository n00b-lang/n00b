#include <stdio.h>
#include <string.h>

#include "ir/libir.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "scanner/token_stream.h"
#include "harness.h"

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

static parse_tree_t *
find_nonterminal(parse_grammar_t *grammar, parse_tree_t *node,
                const char *name)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (core_string_eq(parse_tree_nt_name(grammar, node),
                       core_string_from_cstr(name)))
        return node;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_nonterminal(
            grammar, core_tree_child(node, i), name);
        if (found) return found;
    }
    return nullptr;
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
    ir_local_t *local = ir_local_add(function, "local", i32);
    th_type *callee_signature = ir_type_parse(module, "(i32) -> i32");
    ir_func_t *callee_function = ir_func_add(module, "callee",
        callee_signature, IR_LINK_EXTERN);
    ir_value_t *callee_value = ir_func_address(module, callee_function);
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
          "<left> @emit(\"reference\") ::= \"a\"\n"
          "<right> @emit(\"reference\") ::= \"b\"\n"
          "<literal> @literal(\"int\") @emit(\"literal\") ::= \"7\"\n"
          "<set> @assigns($0, $2) ::= <target> \"=\" <literal>\n"
          "<target> @emit(\"reference\") ::= \"x\"\n"
          "<call> @emit(\"call\", $0, $2) ::= <callee> \"(\" <argument> \")\"\n"
          "<callee> @emit(\"reference\") ::= \"f\"\n"
          "<argument> @emit(\"reference\") ::= \"a\"\n"
          "<ret> @emit(\"return\", $1) ::= \"r\" <literal>\n";
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
    parse_sym_entry_t left_symbol = {
        .name = CORE_STRING_STATIC("left"), .type = i32,
    };
    parse_sym_entry_t right_symbol = {
        .name = CORE_STRING_STATIC("right"), .type = i32,
    };
    parse_sym_entry_t local_symbol = {
        .name = CORE_STRING_STATIC("local"), .type = i32,
    };
    parse_sym_entry_t callee_symbol = {
        .name = CORE_STRING_STATIC("callee"),
        .kind = PARSE_SYM_FUNCTION,
        .type = callee_signature,
    };
    parse_tree_t *left_node = find_nonterminal(grammar, tree, "left");
    parse_tree_t *right_node = find_nonterminal(grammar, tree, "right");
    if (left_node) core_tree_node_value(left_node).sym = &left_symbol;
    if (right_node) core_tree_node_value(right_node).sym = &right_symbol;
    visit_counts_t visit_counts = {0};
    check("symbols bind to IR values and locals",
          ir_lower_bind_value(&lower, &left_symbol, left)
              && ir_lower_bind_value(&lower, &right_symbol, right)
              && ir_lower_bind_value(&lower, &callee_symbol, callee_value)
              && ir_lower_bind_local(&lower, &local_symbol, local)
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

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("call"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("f(a)"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    parse_tree_t *callee_node = find_nonterminal(grammar, tree, "callee");
    parse_tree_t *argument_node = find_nonterminal(grammar, tree, "argument");
    if (callee_node) {
        core_tree_node_value(callee_node).sym = &callee_symbol;
        core_tree_node_value(callee_node).type = callee_signature;
    }
    if (argument_node) {
        core_tree_node_value(argument_node).sym = &left_symbol;
        core_tree_node_value(argument_node).type = i32;
    }
    if (tree) core_tree_node_value(tree).type = i32;
    ir_lower_result_t called = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("declarative call lowering handles a typed function and argument",
          called.status == IR_LOWER_VALUE && called.value
              && called.value->def && called.value->def->op == IR_OP_CALL);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("literal"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("7"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    if (tree) core_tree_node_value(tree).type = i32;
    ir_lower_result_t literal = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("literal annotations lower scalar values generically",
          literal.status == IR_LOWER_VALUE && literal.value
              && literal.value->def
              && literal.value->def->op == IR_OP_CONST);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("set"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("x=7"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    parse_tree_t *target_node = find_nonterminal(grammar, tree, "target");
    if (target_node) core_tree_node_value(target_node).sym = &local_symbol;
    parse_tree_t *assigned_literal = find_nonterminal(grammar, tree, "literal");
    if (assigned_literal) core_tree_node_value(assigned_literal).type = i32;
    ir_lower_result_t assigned = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("assign annotations lower through Lantern local destinations",
          assigned.status == IR_LOWER_NO_VALUE
              && block->instrs[block->n_instrs - 1]->dest_local == local);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

    ir_func_t *return_function = ir_func_add(
        module, "returning", ir_type_parse(module, "() -> i32"),
        IR_LINK_EXTERNAL);
    ir_block_t *return_block = ir_block_add(return_function, "entry");
    ir_lower_context_t return_context = {
        .module = module,
        .function = return_function,
        .block = return_block,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .reachable = true,
    };
    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("ret"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("r7"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    parse_tree_t *return_literal = find_nonterminal(grammar, tree, "literal");
    if (return_literal) core_tree_node_value(return_literal).type = i32;
    ir_lower_result_t returned = tree
        ? ir_lower_tree(lowerer, &return_context, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("return annotations lower and terminate through the tree driver",
          returned.status == IR_LOWER_TERMINATED
              && ir_block_terminator(return_block)
              && ir_block_terminator(return_block)->op == IR_OP_RETURN);
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
          pdg && ir_pdg_node_count(pdg) == 8);

    ir_ctx_free(ctx);
    return report("IR lowering");
}
