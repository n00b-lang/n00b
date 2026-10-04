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
        return true;
    if (entering) counts->enters++;
    else counts->exits++;
    return true;
}

static bool
publish_child_value(ir_lower_context_t *context, parse_visit_t *visit,
                    bool entering, void *state)
{
    (void)state;
    if (!visit->annot || visit->annot->kind != PARSE_ANNOT_SCOPE_OPEN
        || !core_string_eq(visit->annot->scope_tag,
                           CORE_STRING_STATIC("publish")) || entering)
        return true;
    parse_tree_t *child = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    ir_value_t *value = ir_lower_node_value(context, child);
    return value && ir_lower_set_node_value(context, visit->node, value);
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
lower_terminate(ir_lower_context_t *context, parse_visit_t *visit,
                ir_value_t *const *operands, size_t operand_count,
                th_type *result_type, ir_span_t span, void *state)
{
    (void)visit;
    (void)operands;
    (void)result_type;
    (void)state;
    if (operand_count) return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    return ir_lower_emit_terminator(context, IR_OP_UNREACHABLE, span)
        ? (ir_lower_result_t){.status = IR_LOWER_TERMINATED}
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

typedef struct {
    size_t enters;
    size_t exits;
    bool child_value_seen;
} nt_visit_state_t;

static bool
prune_nonterminal(ir_lower_context_t *context, parse_visit_t *visit,
                  bool entering, void *opaque)
{
    nt_visit_state_t *state = opaque;
    if (entering) {
        state->enters++;
        return ir_lower_prune_node(context, visit->node);
    }
    state->exits++;
    return true;
}

static bool
publish_nonterminal_value(ir_lower_context_t *context, parse_visit_t *visit,
                          bool entering, void *opaque)
{
    nt_visit_state_t *state = opaque;
    if (entering) return true;
    parse_tree_t *child = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    ir_value_t *value = ir_lower_node_value(context, child);
    state->child_value_seen = value && value->def
        && value->def->op == IR_OP_CONST;
    return value && ir_lower_set_node_value(context, visit->node, value);
}

static bool
parse_lowering_fixture(parse_grammar_t *grammar, const char *start,
                       const char *source, parse_pwz_parser_t **parser_out,
                       parse_token_stream_t **tokens_out,
                       parse_tree_t **tree_out)
{
    *parser_out = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, core_string_from_cstr(start))));
    *tokens_out = scan_token_stream_from_codepoints(
        core_string_from_cstr(source));
    bool parsed = *parser_out && *tokens_out
        && parse_pwz_parse(*parser_out, *tokens_out);
    *tree_out = parsed ? parse_pwz_get_tree(*parser_out) : nullptr;
    return *tree_out != nullptr;
}

static void
test_nonterminal_lowering_callbacks(void)
{
    static const char *const grammar_text =
        "@tokenizer(\"toy\")\n"
        "<prune_root> ::= <pruned>\n"
        "<pruned> @literal(\"int\") @emit(\"literal\") ::= \"7\"\n"
        "<published> @emit(\"identity\", $0) ::= <literal>\n"
        "<literal> @literal(\"int\") @emit(\"literal\") ::= \"7\"\n";
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "nt-lower-test",
                                         &ir_profile_bce, "host");
    th_type *i32 = ir_type_parse(module, "i32");
    parse_grammar_t *grammar = parse_grammar_new();
    bool loaded = parse_bnf_load(core_string_from_cstr(grammar_text),
        CORE_STRING_STATIC("prune_root"), grammar);
    ir_lowerer_t *lowerer = ir_lowerer_new(ctx);
    bool registered = loaded && lowerer
        && ir_lowerer_register(lowerer, "identity", lower_identity, nullptr);

    th_type *void_signature = ir_type_parse(module, "() -> void");
    ir_func_t *prune_function = void_signature
        ? ir_func_add(module, "pruned", void_signature, IR_LINK_INTERNAL)
        : nullptr;
    ir_block_t *prune_block = prune_function
        ? ir_block_add(prune_function, "entry") : nullptr;
    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_lower_context_t prune_context = {
        .module = module, .function = prune_function, .block = prune_block,
        .diagnostics = diagnostics, .file_id = -1, .reachable = true,
    };
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = nullptr;
    bool parsed = registered && prune_block
        && parse_lowering_fixture(grammar, "prune_root", "7", &parser,
                                  &tokens, &tree);
    nt_visit_state_t prune_state = {0};
    bool prune_registered = parsed
        && ir_lowerer_register_nt_visit(lowerer, "pruned",
            prune_nonterminal, &prune_state);
    ir_lower_result_t pruned = parsed
        ? ir_lower_tree(lowerer, &prune_context, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("nonterminal entry callbacks prune before descendant annotations",
          prune_registered && pruned.status == IR_LOWER_NO_VALUE
              && prune_state.enters == 1 && prune_state.exits == 1
              && prune_block->n_instrs == 0);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

    th_type *value_signature = ir_type_parse(module, "() -> i32");
    ir_func_t *value_function = value_signature
        ? ir_func_add(module, "published", value_signature, IR_LINK_INTERNAL)
        : nullptr;
    ir_block_t *value_block = value_function
        ? ir_block_add(value_function, "entry") : nullptr;
    ir_lower_context_t value_context = {
        .module = module, .function = value_function, .block = value_block,
        .diagnostics = diagnostics, .file_id = -1, .reachable = true,
    };
    parser = nullptr;
    tokens = nullptr;
    tree = nullptr;
    parsed = value_block && parse_lowering_fixture(grammar, "published", "7",
                                                   &parser, &tokens, &tree);
    parse_tree_t *literal = parsed
        ? core_tree_child(tree, 0) : nullptr;
    if (parsed) {
        core_tree_node_value(tree).type = i32;
        core_tree_node_value(literal).type = i32;
    }
    nt_visit_state_t publish_state = {0};
    bool publish_registered = parsed
        && ir_lowerer_register_nt_visit(lowerer, "published",
            publish_nonterminal_value, &publish_state);
    ir_lower_result_t published = parsed
        ? ir_lower_tree(lowerer, &value_context, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("nonterminal exit callbacks can consume lowered children",
          publish_registered && published.status == IR_LOWER_VALUE
              && publish_state.child_value_seen && published.value
              && published.value->def
              && published.value->def->op == IR_OP_CONST
              && value_block->n_instrs == 1);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
    ir_ctx_free(ctx);
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
          "<sum> @scope(\"sum\") @emit(\"add\", $left, $right) ::= <left> \"+\" <right>\n"
          "<left> @emit(\"reference\") ::= \"a\"\n"
          "<right> @emit(\"reference\") ::= \"b\"\n"
          "<literal> @literal(\"int\") @emit(\"literal\") ::= \"7\"\n"
          "<set> @assigns($0, $2) ::= <target> \"=\" <literal>\n"
          "<owner_set> @assigns($0, $2) ::= \"x\" \"=\" <literal>\n"
          "<target> @emit(\"reference\") ::= \"x\"\n"
          "<indexed_set> @assigns($0, $2) ::= <indexed_target> \"=\" <literal>\n"
          "<indexed_target> ::= <base> \"[\" <literal> \"]\"\n"
          "<base> @emit(\"reference\") ::= \"x\"\n"
          "<call> @emit(\"call\", $0, $2) ::= <callee> \"(\" <argument> \")\"\n"
          "<callee> @emit(\"reference\") ::= \"f\"\n"
          "<argument> @emit(\"reference\") ::= \"a\"\n"
          "<ret> @emit(\"return\", $1) ::= \"r\" <literal>\n"
          "<publish> @scope(\"publish\") ::= <left>\n";
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
        .phases = PARSE_SYM_PHASE_BOTH,
        .mutable_phases = PARSE_SYM_PHASE_RUNTIME,
        .initializer_phases = PARSE_SYM_PHASE_COMPTIME,
        .requested_storage_class = CORE_STRING_STATIC("pool"),
        .selected_storage_class = CORE_STRING_STATIC("pool"),
        .storage_class_required = true,
        .storage_duration = PARSE_SYM_STORAGE_DYNAMIC,
        .lifetime = {
            .region = 12, .start_point = 2, .end_point = 8,
            .has_region = true, .has_start = true, .has_end = true,
        },
        .alias_analysis_complete = true,
    };
    parse_sym_entry_t right_symbol = {
        .name = CORE_STRING_STATIC("right"), .type = i32,
        .phases = PARSE_SYM_PHASE_RUNTIME,
        .lifetime = {.region = 12, .end_point = 8,
                     .has_region = true, .has_end = true},
        .alias_analysis_complete = true,
    };
    parse_sym_entry_t local_symbol = {
        .name = CORE_STRING_STATIC("local"), .type = i32,
    };
    parse_sym_entry_t rebound_symbol = {
        .name = CORE_STRING_STATIC("rebound_symbol"), .type = i32,
    };
    ir_local_t *rebound_local = ir_local_add(function, "rebound", i32);
    parse_sym_entry_t callee_symbol = {
        .name = CORE_STRING_STATIC("callee"),
        .kind = PARSE_SYM_FUNCTION,
        .type = callee_signature,
    };
    ir_global_t *global = ir_global_add(module, "global", i32);
    parse_sym_entry_t global_symbol = {
        .name = CORE_STRING_STATIC("global"), .type = i32,
    };
    ir_value_t *global_value = global
        ? ir_global_address(module, global) : nullptr;
    bool alias_fact_added = parse_symtab_update_alias_use(
        &left_symbol, &right_symbol, PARSE_SYM_ALIAS_DERIVED, 3, 7, false);
    parse_sym_set_alias_analysis_complete(&left_symbol, true);
    parse_sym_set_alias_analysis_complete(&right_symbol, true);
    parse_tree_t *left_node = find_nonterminal(grammar, tree, "left");
    parse_tree_t *right_node = find_nonterminal(grammar, tree, "right");
    if (left_node) core_tree_node_value(left_node).sym = &left_symbol;
    if (right_node) core_tree_node_value(right_node).sym = &right_symbol;
    visit_counts_t visit_counts = {0};
    check("symbols bind to IR values and locals",
          alias_fact_added
              && ir_lower_bind_value(&lower, &left_symbol, left)
              && ir_lower_bind_value(&lower, &right_symbol, right)
              && ir_lower_bind_value(&lower, &callee_symbol, callee_value)
              && ir_lower_bind_value(&lower, &global_symbol, global_value)
              && ir_lower_bind_local(&lower, &local_symbol, local)
              && ir_lower_bound_local(&lower, &local_symbol) == local
              && ir_lower_bind_local(&lower, &rebound_symbol, local)
              && rebound_local
              && ir_lower_rebind_local(&lower, &rebound_symbol,
                                       rebound_local)
              && ir_lower_bound_local(&lower, &rebound_symbol)
                    == rebound_local
              && parse_sym_phase_binding(
                     &left_symbol, PARSE_SYM_PHASE_RUNTIME) == left
              && parse_sym_phase_binding(
                     &local_symbol, PARSE_SYM_PHASE_RUNTIME)
                     == ir_local_value(function, local)
              && parse_sym_phase_binding(
                     &rebound_symbol, PARSE_SYM_PHASE_RUNTIME)
                     == ir_local_value(function, rebound_local)
              && ir_lowerer_register_visit(lowerer,
                  PARSE_ANNOT_SCOPE_OPEN, count_lower_visit, &visit_counts)
              && ir_lowerer_register_visit(lowerer,
                  PARSE_ANNOT_SCOPE_OPEN, publish_child_value, nullptr));
    th_type *comptime_signature = ir_type_parse(module, "() -> void");
    ir_func_t *comptime_function = comptime_signature
        ? ir_func_add(module, "comptime_binding", comptime_signature,
                      IR_LINK_INTERNAL) : nullptr;
    ir_block_t *comptime_block = comptime_function
        ? ir_block_add(comptime_function, "entry") : nullptr;
    ir_local_t *comptime_local = comptime_function
        ? ir_local_add(comptime_function, "phase_local", i32) : nullptr;
    parse_sym_entry_t comptime_symbol = {
        .name = CORE_STRING_STATIC("phase_local"),
        .kind = PARSE_SYM_VARIABLE,
        .type = i32,
    };
    ir_lower_context_t comptime_lower = {
        .module = module,
        .function = comptime_function,
        .block = comptime_block,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .reachable = true,
    };
    ir_instr_t *comptime_return = comptime_block
        ? ir_instr_add(comptime_block, IR_OP_RETURN) : nullptr;
    bool comptime_binding_set = comptime_function && comptime_block
        && ir_func_set_execution_phase(ctx, comptime_function,
                                       IR_PHASE_COMPTIME)
        && comptime_local && comptime_return
        && ir_lower_bind_local(&comptime_lower, &comptime_symbol,
                               comptime_local)
        && parse_sym_phase_binding(&comptime_symbol,
                                   PARSE_SYM_PHASE_COMPTIME)
               == ir_local_value(comptime_function, comptime_local)
        && !parse_sym_phase_binding(&comptime_symbol,
                                    PARSE_SYM_PHASE_RUNTIME);
    check("comptime lowering records a separate source-symbol binding",
          comptime_binding_set);
    ir_annots_t *left_facts = ir_value_annots(left);
    ir_annot_t *available = ir_annot_get(left_facts,
                                         "ir.binding_phases");
    ir_annot_t *mutable = ir_annot_get(left_facts,
                                       "ir.binding_mutable_phases");
    ir_annot_t *storage = ir_annot_get(left_facts, "ir.storage_selected");
    ir_annot_t *requested_storage = ir_annot_get(left_facts,
                                                 "ir.storage_requested");
    ir_annot_t *storage_required = ir_annot_get(left_facts,
                                                "ir.storage_required");
    ir_annot_t *duration = ir_annot_get(left_facts, "ir.storage_duration");
    ir_annot_t *region = ir_annot_get(left_facts, "ir.lifetime_region");
    ir_annot_t *alias = ir_annot_get(left_facts, "ir.alias0");
    ir_annot_t *last_use = ir_annot_get(left_facts,
                                        "ir.alias0_last_use");
    check("symbol phase, storage, lifetime, and alias facts project to IR",
          available && available->kind == IR_ANNOT_INT
              && available->i.mag == PARSE_SYM_PHASE_BOTH
              && mutable && mutable->kind == IR_ANNOT_INT
              && mutable->i.mag == PARSE_SYM_PHASE_RUNTIME
              && storage && storage->kind == IR_ANNOT_STR
              && !strcmp(storage->s, "pool")
              && requested_storage
              && requested_storage->kind == IR_ANNOT_STR
              && !strcmp(requested_storage->s, "pool")
              && storage_required
              && storage_required->kind == IR_ANNOT_BOOL
              && storage_required->b
              && duration && duration->kind == IR_ANNOT_INT
              && duration->i.mag == PARSE_SYM_STORAGE_DYNAMIC
              && region && region->kind == IR_ANNOT_INT
              && region->i.mag == 12
              && alias && alias->kind == IR_ANNOT_VALUE
              && alias->v == right
              && last_use && last_use->kind == IR_ANNOT_INT
              && last_use->i.mag == 7
              && !ir_value_can_finalize_at(left, 7)
              && ir_value_can_finalize_at(left, 8));
    parse_sym_entry_t unresolved_target = {
        .name = CORE_STRING_STATIC("unresolved_target"), .type = i32,
    };
    parse_sym_entry_t unresolved_source = {
        .name = CORE_STRING_STATIC("unresolved_source"), .type = i32,
        .alias_analysis_complete = true,
        .lifetime = {.region = 12, .end_point = 10,
                     .has_region = true, .has_end = true},
    };
    bool unresolved_edge = parse_symtab_add_alias(
        &unresolved_source, &unresolved_target, PARSE_SYM_ALIAS_MAY, 4,
        false);
    ir_local_t *unresolved_local =
        ir_local_add(function, "unresolved_local", i32);
    bool unresolved_bound = unresolved_edge && unresolved_local
        && ir_lower_bind_local(&lower, &unresolved_source,
                               unresolved_local);
    ir_value_t *unresolved_value = unresolved_bound
        ? ir_local_value(function, unresolved_local) : nullptr;
    ir_annots_t *unresolved_facts = ir_value_annots(unresolved_value);
    ir_annot_t *unresolved_flag = ir_annot_get(
        unresolved_facts, "ir.alias_unresolved");
    ir_annot_t *incomplete = ir_annot_get(
        unresolved_facts, "ir.alias_analysis_complete");
    ir_annot_t *possibly_escaping = ir_annot_get(
        unresolved_facts, "ir.lifetime_escapes");
    check("unprojected aliases remain incomplete and escaping in IR",
          unresolved_flag && unresolved_flag->kind == IR_ANNOT_BOOL
              && unresolved_flag->b && incomplete
              && incomplete->kind == IR_ANNOT_BOOL && !incomplete->b
              && possibly_escaping
              && possibly_escaping->kind == IR_ANNOT_BOOL
              && possibly_escaping->b
              && !ir_value_can_finalize_at(unresolved_value, 100));
    parse_sym_entry_t cross_scope_target = {
        .name = CORE_STRING_STATIC("cross_scope_target"), .type = i32,
    };
    parse_sym_entry_t global_holder_symbol = {
        .name = CORE_STRING_STATIC("global_holder"), .type = i32,
        .alias_analysis_complete = true,
        .lifetime = {.region = 1, .end_point = 20,
                     .has_region = true, .has_end = true},
    };
    bool cross_scope_edge = parse_symtab_add_alias(
        &global_holder_symbol, &cross_scope_target,
        PARSE_SYM_ALIAS_DERIVED, 5, true);
    ir_global_t *global_holder = ir_global_add(module, "global_holder", i32);
    ir_value_t *global_holder_value = global_holder
        ? ir_global_address(module, global_holder) : nullptr;
    ir_local_t *cross_scope_local =
        ir_local_add(function, "cross_scope_local", i32);
    bool cross_scope_bound = cross_scope_edge && global_holder_value
        && cross_scope_local
        && ir_lower_bind_value(&lower, &global_holder_symbol,
                               global_holder_value)
        && ir_lower_bind_local(&lower, &cross_scope_target,
                               cross_scope_local);
    ir_annots_t *global_holder_facts =
        ir_value_annots(global_holder_value);
    ir_annot_t *cross_scope_unresolved = ir_annot_get(
        global_holder_facts, "ir.alias_unresolved");
    ir_annot_t *cross_scope_complete = ir_annot_get(
        global_holder_facts, "ir.alias_analysis_complete");
    check("global facts never point at function-local values",
          cross_scope_bound && cross_scope_unresolved
              && cross_scope_unresolved->kind == IR_ANNOT_BOOL
              && cross_scope_unresolved->b
              && cross_scope_complete
              && cross_scope_complete->kind == IR_ANNOT_BOOL
              && !cross_scope_complete->b
              && !ir_annot_get(global_holder_facts, "ir.alias0")
              && !ir_value_can_finalize_at(global_holder_value, 100));
    ir_global_t *cycle_a_global = ir_global_add(module, "cycle_a", i32);
    ir_global_t *cycle_b_global = ir_global_add(module, "cycle_b", i32);
    ir_value_t *cycle_a = cycle_a_global
        ? ir_global_address(module, cycle_a_global) : nullptr;
    ir_value_t *cycle_b = cycle_b_global
        ? ir_global_address(module, cycle_b_global) : nullptr;
    ir_annots_t *cycle_a_facts = ir_value_annots(cycle_a);
    ir_annots_t *cycle_b_facts = ir_value_annots(cycle_b);
    bool cycle_facts_added = cycle_a_facts && cycle_b_facts
        && ir_annot_set(ctx, cycle_a_facts, (ir_annot_t){
            .key = "ir.alias_analysis_complete", .kind = IR_ANNOT_BOOL,
            .b = true,
        })
        && ir_annot_set(ctx, cycle_b_facts, (ir_annot_t){
            .key = "ir.alias_analysis_complete", .kind = IR_ANNOT_BOOL,
            .b = true,
        });
    ir_value_t *cycle_values[] = {cycle_a, cycle_b};
    for (size_t i = 0; i < 2 && cycle_facts_added; i++) {
        ir_annots_t *facts = ir_value_annots(cycle_values[i]);
        ir_value_t *target = cycle_values[1 - i];
        cycle_facts_added = ir_annot_set(ctx, facts, (ir_annot_t){
            .key = "ir.alias_count", .kind = IR_ANNOT_INT,
            .i = {.mag = 1},
        })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.aliased_by_count", .kind = IR_ANNOT_INT,
                .i = {.mag = 0},
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.alias_unresolved", .kind = IR_ANNOT_BOOL,
                .b = false,
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.lifetime_escapes", .kind = IR_ANNOT_BOOL,
                .b = false,
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.lifetime_end", .kind = IR_ANNOT_INT,
                .i = {.mag = 10},
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.alias0", .kind = IR_ANNOT_VALUE,
                .v = target,
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.alias0_escapes", .kind = IR_ANNOT_BOOL,
                .b = false,
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.alias0_has_last_use", .kind = IR_ANNOT_BOOL,
                .b = true,
            })
            && ir_annot_set(ctx, facts, (ir_annot_t){
                .key = "ir.alias0_last_use", .kind = IR_ANNOT_INT,
                .i = {.mag = 5},
            });
    }
    check("IR finalization analysis handles cyclic alias relations",
          cycle_facts_added && ir_value_can_finalize_at(cycle_a, 10));
    void *binding_checkpoint = ir_lower_bindings_checkpoint(&lower);
    check("a scoped binding can refine a symbol while retaining its local",
          ir_lower_bind_scoped_value(&lower, &left_symbol, right, local)
              && ir_lower_bound_local(&lower, &left_symbol) == local
              && ir_lower_bindings_restore(&lower, binding_checkpoint)
              && ir_lower_bound_local(&lower, &left_symbol) == nullptr);
    ir_annot_t *right_phases = ir_annot_get(ir_value_annots(right),
                                            "ir.binding_phases");
    check("scoped bindings do not overwrite durable symbol facts",
          right_phases && right_phases->kind == IR_ANNOT_INT
              && right_phases->i.mag == PARSE_SYM_PHASE_RUNTIME);
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
        parse_find_nonterm(grammar, CORE_STRING_STATIC("publish"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("a"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    left_node = find_nonterminal(grammar, tree, "left");
    if (left_node) {
        core_tree_node_value(left_node).sym = &left_symbol;
        core_tree_node_value(left_node).type = i32;
    }
    ir_lower_result_t published = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("a callback can publish a synthesized node value",
          published.status == IR_LOWER_VALUE && published.value == left);
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
    size_t assignment_start = block->n_instrs;
    ir_lower_result_t assigned = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("assign annotations lower through Lantern local destinations",
          assigned.status == IR_LOWER_NO_VALUE
              && block->n_instrs == assignment_start + 2
              && block->instrs[assignment_start]->op == IR_OP_CONST
              && block->instrs[block->n_instrs - 1]->dest_local == local);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("owner_set"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("x=7"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    if (tree) core_tree_node_value(tree).sym = &local_symbol;
    assigned_literal = find_nonterminal(grammar, tree, "literal");
    if (assigned_literal) core_tree_node_value(assigned_literal).type = i32;
    assignment_start = block->n_instrs;
    assigned = tree ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("assign annotations use their owner's declared symbol for leaf targets",
          assigned.status == IR_LOWER_NO_VALUE
              && block->n_instrs == assignment_start + 2
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
    check("a custom lowering action registers once",
          ir_lowerer_register(lowerer, "identity", lower_identity, nullptr)
              && !ir_lowerer_register(lowerer, "identity", lower_identity,
                                      nullptr));
    check("a custom terminator action registers",
          ir_lowerer_register(lowerer, "terminate", lower_terminate, nullptr));

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

    ir_func_t *callback_function = ir_func_add(
        module, "callback_terminator", ir_type_parse(module, "() -> nil"),
        IR_LINK_EXTERNAL);
    ir_block_t *callback_block = callback_function
        ? ir_block_add(callback_function, "entry") : nullptr;
    ir_lower_context_t callback_context = {
        .module = module,
        .function = callback_function,
        .block = callback_block,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .reachable = true,
    };
    parse_annotation_t *terminate = emit_annotation("terminate", 0);
    ir_lower_result_t callback_terminated = ir_lower_emit(
        lowerer, &callback_context, terminate, nullptr, nullptr, 0, nullptr,
        IR_SPAN_UNKNOWN);
    check("a callback-emitted terminator updates IR reachability",
          callback_terminated.status == IR_LOWER_TERMINATED
              && !callback_context.reachable && callback_block
              && ir_block_terminator(callback_block)
              && ir_block_terminator(callback_block)->op
                  == IR_OP_UNREACHABLE);
    parse_annot_free(terminate);

    lower.reachable = true;
    parse_annotation_t *unknown = emit_annotation("not_a_real_op", 0);
    ir_lower_result_t failed = ir_lower_emit(lowerer, &lower, unknown, nullptr,
        nullptr, 0, nullptr, IR_SPAN_UNKNOWN);
    check("an unknown action fails through the IR diagnostics API",
          failed.status == IR_LOWER_FAILED && ir_diags_count(diagnostics) == 2);
    parse_annot_free(unknown);

    th_type *storage_signature = ir_type_parse(module, "(i32) -> void");
    ir_func_t *storage_function = storage_signature
        ? ir_func_add(module, "address_backed", storage_signature,
                      IR_LINK_INTERNAL) : nullptr;
    ir_block_t *storage_block = storage_function
        ? ir_block_add(storage_function, "entry") : nullptr;
    ir_value_t *storage_input = storage_function
        ? ir_param_add(storage_function, "storage_input", i32) : nullptr;
    ir_local_t *storage_local = storage_function
        ? ir_local_add(storage_function, "storage_local", i32) : nullptr;
    th_type *storage_address_type = ir_type_parse(module, "ref[i32]");
    ir_instr_t *allocation = storage_block
        ? ir_instr_add(storage_block, IR_OP_ALLOC) : nullptr;
    bool storage_allocated = allocation && storage_address_type
        && ir_instr_set_dest(allocation, "storage_address",
                             storage_address_type)
        && ir_instr_operand_type(allocation, i32);
    parse_sym_entry_t storage_symbol = {
        .name = CORE_STRING_STATIC("x"),
        .kind = PARSE_SYM_VARIABLE,
        .type = i32,
        .phases = PARSE_SYM_PHASE_RUNTIME,
    };
    ir_lower_context_t storage_lower = {
        .module = module,
        .function = storage_function,
        .block = storage_block,
        .diagnostics = diagnostics,
        .file_id = file_id,
        .reachable = true,
    };
    check("a local may bind to explicit indirection-backed storage",
          storage_allocated && storage_local
              && ir_lower_bind_local_storage(&storage_lower,
                    &storage_symbol, storage_local, allocation->dest)
              && ir_lower_bound_local(&storage_lower, &storage_symbol)
                    == storage_local
              && ir_lower_bound_address(&storage_lower, &storage_symbol)
                    == allocation->dest
              && ir_lower_local_address(&storage_lower, storage_local)
                    == allocation->dest
              && parse_sym_phase_binding(&storage_symbol,
                    PARSE_SYM_PHASE_RUNTIME) == allocation->dest);
    void *storage_checkpoint =
        ir_lower_bindings_checkpoint(&storage_lower);
    check("a scoped value retains its backing storage address",
          storage_input
              && ir_lower_bind_scoped_value(&storage_lower, &storage_symbol,
                                     storage_input,
                                     storage_local)
              && ir_lower_bound_value(&storage_lower, &storage_symbol)
                    == storage_input
              && ir_lower_bound_address(&storage_lower, &storage_symbol)
                    == allocation->dest
              && ir_lower_bindings_restore(&storage_lower,
                                           storage_checkpoint));

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("target"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("x"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    parse_tree_t *storage_target = find_nonterminal(grammar, tree, "target");
    if (storage_target)
        core_tree_node_value(storage_target).sym = &storage_symbol;
    if (tree) core_tree_node_value(tree).type = i32;
    ir_lower_result_t storage_read = tree
        ? ir_lower_tree(lowerer, &storage_lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("a reference to address-backed local storage emits a load",
          storage_read.status == IR_LOWER_VALUE && storage_read.value
              && storage_read.value->def
              && storage_read.value->def->op == IR_OP_LOAD);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("set"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("x=7"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    storage_target = find_nonterminal(grammar, tree, "target");
    parse_tree_t *storage_literal = find_nonterminal(grammar, tree, "literal");
    if (storage_target)
        core_tree_node_value(storage_target).sym = &storage_symbol;
    if (storage_literal) core_tree_node_value(storage_literal).type = i32;
    ir_lower_result_t storage_write = tree
        ? ir_lower_tree(lowerer, &storage_lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    ir_instr_t *stored = storage_block && storage_block->n_instrs
        ? storage_block->instrs[storage_block->n_instrs - 1] : nullptr;
    check("an assignment to address-backed local storage emits a store",
          storage_write.status == IR_LOWER_NO_VALUE && stored
              && stored->op == IR_OP_STORE && stored->n_operands == 2
              && ir_operand_value(&stored->operands[0]) == allocation->dest);
    ir_instr_t *storage_return = storage_block
        ? ir_instr_add(storage_block, IR_OP_RETURN) : nullptr;
    check("the address-backed lowering fixture terminates",
          storage_return != nullptr);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);

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
          pdg && ir_pdg_node_count(pdg) == 9);

    parser = parse_pwz_new_for_start(grammar, parse_nonterm_id(
        parse_find_nonterm(grammar, CORE_STRING_STATIC("indexed_set"))));
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("x[7]=7"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    parse_tree_t *base_node = find_nonterminal(grammar, tree, "base");
    if (base_node) core_tree_node_value(base_node).sym = &local_symbol;
    assigned = tree ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("unsupported compound lvalues are rejected",
          assigned.status == IR_LOWER_FAILED);
    check("compound lvalue rejection emits one diagnostic",
          ir_diags_count(diagnostics) == 3);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);

    static const char *const invalid_emit_grammar
        = "<sum> @emit(\"add\", $4) ::= <left> \"+\" <right>\n"
          "<left> @emit(\"reference\") ::= \"a\"\n"
          "<right> @emit(\"reference\") ::= \"b\"\n";
    grammar = parse_grammar_new();
    bool invalid_grammar_loaded = parse_bnf_load(
        core_string_from_cstr(invalid_emit_grammar),
        CORE_STRING_STATIC("sum"), grammar);
    parser = invalid_grammar_loaded ? parse_pwz_new_for_start(
        grammar, parse_nonterm_id(parse_find_nonterm(
            grammar, CORE_STRING_STATIC("sum")))) : nullptr;
    tokens = scan_token_stream_from_codepoints(CORE_STRING_STATIC("a+b"));
    parsed = parser && parse_pwz_parse(parser, tokens);
    tree = parsed ? parse_pwz_get_tree(parser) : nullptr;
    size_t instruction_count_before = block->n_instrs;
    size_t diagnostic_count_before = ir_diags_count(diagnostics);
    ir_lower_result_t invalid_emit = tree
        ? ir_lower_tree(lowerer, &lower, grammar, tree)
        : (ir_lower_result_t){.status = IR_LOWER_FAILED};
    check("lowering preflights exact-rule child references",
          invalid_emit.status == IR_LOWER_FAILED
              && ir_diags_count(diagnostics) == diagnostic_count_before + 1);
    check("invalid emit metadata fails before emitting IR",
          block->n_instrs == instruction_count_before);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);

    test_nonterminal_lowering_callbacks();
    ir_ctx_free(ctx);
    return report("IR lowering");
}
