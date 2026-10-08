#include <string.h>

#include "ir/exec_graph.h"
#include "ir/verify.h"
#include "harness.h"

static ir_func_t *
make_mixed_exit_function(ir_module_t *module)
{
    th_type *type = ir_type_parse(module, "(bool, bool) -> nil fail: int");
    ir_func_t *function = type
        ? ir_func_add(module, "mixed", type, IR_LINK_INTERNAL) : nullptr;
    ir_value_t *first = function
        ? ir_param_add(function, "first", ir_type_parse(module, "bool"))
        : nullptr;
    ir_value_t *second = function
        ? ir_param_add(function, "second", ir_type_parse(module, "bool"))
        : nullptr;
    ir_block_t *entry = function ? ir_block_add(function, "entry") : nullptr;
    ir_block_t *left = function ? ir_block_add(function, "left") : nullptr;
    ir_block_t *right = function ? ir_block_add(function, "right") : nullptr;
    ir_block_t *success = function
        ? ir_block_add(function, "success") : nullptr;
    ir_block_t *failure = function
        ? ir_block_add(function, "failure") : nullptr;
    ir_block_t *unreachable = function
        ? ir_block_add(function, "unreachable") : nullptr;
    if (!entry || !left || !right || !success || !failure || !unreachable)
        return nullptr;
    ir_instr_t *branch = ir_instr_add(entry, IR_OP_BRANCH);
    ir_instr_t *nested = ir_instr_add(right, IR_OP_BRANCH);
    if (!branch || !nested || !first || !second
        || !ir_instr_operand_value(branch, first)
        || !ir_instr_operand_block(branch, left)
        || !ir_instr_operand_block(branch, right)
        || !ir_instr_operand_value(nested, second)
        || !ir_instr_operand_block(nested, success)
        || !ir_instr_operand_block(nested, failure)
        || !ir_instr_add(left, IR_OP_RETURN)
        || !ir_instr_add(success, IR_OP_RETURN)
        || !ir_instr_add(failure, IR_OP_FAIL)
        || !ir_instr_add(unreachable, IR_OP_RETURN))
        return nullptr;
    return function;
}

static void
test_phase_dependency_and_exit_classification(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "phase_graph", &ir_profile_bce,
                                         "test");
    ir_func_t *mixed = make_mixed_exit_function(module);
    ir_exec_graph_t *graph = ir_exec_graph_new(ctx);
    uint32_t comptime, comptime_entry, runtime, runtime_entry;
    bool built = module && mixed && graph
        && ir_exec_graph_add_phase(graph, "comptime", &comptime,
                                   &comptime_entry)
        && ir_exec_graph_add_phase(graph, "runtime", &runtime,
                                   &runtime_entry);
    uint32_t mixed_region = UINT32_MAX;
    built = built && ir_exec_graph_add_function(graph, comptime, mixed,
        nullptr, nullptr, &mixed_region);
    ir_exec_graph_function_view_t region;
    built = built && ir_exec_graph_function_at(graph, mixed_region, &region);
    size_t successes = 0;
    uint32_t failure_exit = UINT32_MAX;
    for (size_t i = 0; built && i < region.exit_count; i++) {
        uint32_t exit_node;
        ir_exec_graph_outcome_t outcome;
        built = ir_exec_graph_function_exit_at(graph, mixed_region, i,
                                                &exit_node, &outcome);
        if (!built) break;
        if (outcome == IR_EXEC_GRAPH_OUTCOME_SUCCESS) {
            successes++;
            built = ir_exec_graph_add_edge(graph, runtime_entry, exit_node,
                IR_EXEC_GRAPH_EDGE_REQUIRES) && built;
        }
        else if (outcome == IR_EXEC_GRAPH_OUTCOME_FAILURE) {
            failure_exit = exit_node;
            check("a failed exit cannot be a phase prerequisite",
                  !ir_exec_graph_add_edge(graph, runtime_entry, exit_node,
                      IR_EXEC_GRAPH_EDGE_REQUIRES));
        }
    }
    check("function analysis discovers reachable mixed outcomes but omits dead exits",
          built && region.exit_count == 3 && successes == 2
              && failure_exit != UINT32_MAX);
    check("phase sequence edges stay within their phase",
          ir_exec_graph_add_edge(graph, comptime_entry, region.entry_node,
              IR_EXEC_GRAPH_EDGE_SEQUENCE)
          && !ir_exec_graph_add_edge(graph, region.entry_node, runtime_entry,
              IR_EXEC_GRAPH_EDGE_SEQUENCE));
    check("phase graph validates after success-only prerequisites",
          ir_exec_graph_validate(graph));

    char *printed = ir_exec_graph_print(graph);
    check("phase graph dump records prerequisite direction and outcomes",
          printed && strstr(printed, "edge requires")
              && strstr(printed, "outcome=failure")
              && strstr(printed, "function-exit")
              && !strstr(printed, "block=unreachable"));
    core_free(printed);
    ir_ctx_free(ctx);
}

static void
test_failure_only_and_empty_phase(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "phase_graph", &ir_profile_bce,
                                         "test");
    ir_func_t *failure_only = ir_func_add(module, "failure_only",
        ir_type_parse(module, "() -> nil fail: int"), IR_LINK_INTERNAL);
    ir_block_t *entry = failure_only
        ? ir_block_add(failure_only, "entry") : nullptr;
    if (entry) ir_instr_add(entry, IR_OP_FAIL);

    ir_exec_graph_t *failed_graph = ir_exec_graph_new(ctx);
    uint32_t comptime, comptime_entry, runtime_entry, function_id;
    bool built = entry && ir_exec_graph_add_phase(failed_graph, "comptime",
            &comptime, &comptime_entry)
        && ir_exec_graph_add_phase(failed_graph, "runtime", nullptr,
                                   &runtime_entry)
        && ir_exec_graph_add_function(failed_graph, comptime, failure_only,
            nullptr, nullptr, &function_id);
    ir_exec_graph_function_view_t region;
    built = built && ir_exec_graph_function_at(failed_graph, function_id,
                                                &region);
    uint32_t failure_exit = UINT32_MAX;
    ir_exec_graph_outcome_t outcome = IR_EXEC_GRAPH_OUTCOME_NONE;
    built = built && region.exit_count == 1
        && ir_exec_graph_function_exit_at(failed_graph, function_id, 0,
                                           &failure_exit, &outcome);
    check("failure-only compile-time work contributes no runtime prerequisite",
          built && outcome == IR_EXEC_GRAPH_OUTCOME_FAILURE
              && !ir_exec_graph_add_edge(failed_graph, runtime_entry,
                  failure_exit, IR_EXEC_GRAPH_EDGE_REQUIRES)
              && ir_exec_graph_validate(failed_graph));

    ir_exec_graph_t *empty_graph = ir_exec_graph_new(ctx);
    uint32_t empty_comptime, empty_entry, empty_exit;
    uint32_t empty_runtime_entry;
    built = built && ir_exec_graph_add_phase(empty_graph, "comptime",
            &empty_comptime, &empty_entry)
        && ir_exec_graph_add_phase(empty_graph, "runtime", nullptr,
                                   &empty_runtime_entry)
        && ir_exec_graph_add_phase_exit(empty_graph, empty_comptime,
            "no-comptime-work", IR_EXEC_GRAPH_OUTCOME_SUCCESS, &empty_exit)
        && ir_exec_graph_add_edge(empty_graph, empty_entry, empty_exit,
            IR_EXEC_GRAPH_EDGE_SEQUENCE)
        && ir_exec_graph_add_edge(empty_graph, empty_runtime_entry, empty_exit,
            IR_EXEC_GRAPH_EDGE_REQUIRES);
    check("an empty compile-time phase has an explicit successful boundary",
          built && ir_exec_graph_validate(empty_graph));
    ir_ctx_free(ctx);
}

static void
test_trap_exit_classification(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "trap_exit", &ir_profile_bce,
                                        "test");
    ir_func_t *function = module
        ? ir_func_add(module, "stops", ir_type_parse(module, "() -> nil"),
                      IR_LINK_INTERNAL) : nullptr;
    ir_block_t *entry = function ? ir_block_add(function, "entry") : nullptr;
    ir_instr_t *trap = entry ? ir_instr_add(entry, IR_OP_TRAP) : nullptr;
    ir_exec_graph_t *graph = ir_exec_graph_new(ctx);
    uint32_t phase = UINT32_MAX, function_id = UINT32_MAX;
    bool built = trap && graph
        && ir_exec_graph_add_phase(graph, "runtime", &phase, nullptr)
        && ir_exec_graph_add_function(graph, phase, function, nullptr, nullptr,
                                      &function_id);
    ir_exec_graph_function_view_t region;
    uint32_t exit_node = UINT32_MAX;
    ir_exec_graph_outcome_t exit_outcome = IR_EXEC_GRAPH_OUTCOME_NONE;
    built = built && ir_exec_graph_function_at(graph, function_id, &region)
        && region.exit_count == 1
        && ir_exec_graph_function_exit_at(graph, function_id, 0, &exit_node,
                                           &exit_outcome);
    check("trap terminators are classified as nonreturning exits",
          built && exit_node != UINT32_MAX
              && exit_outcome == IR_EXEC_GRAPH_OUTCOME_NONRETURNING
              && ir_exec_graph_validate(graph));
    ir_ctx_free(ctx);
}

static void
test_context_ownership(void)
{
    ir_ctx_t *owner_ctx = ir_ctx_new();
    ir_ctx_t *other_ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(other_ctx, "foreign",
                                         &ir_profile_bce, "test");
    ir_func_t *function = ir_func_add(module, "foreign",
        ir_type_parse(module, "() -> nil"), IR_LINK_INTERNAL);
    ir_block_t *entry = function ? ir_block_add(function, "entry") : nullptr;
    if (entry) ir_instr_add(entry, IR_OP_RETURN);
    ir_exec_graph_t *graph = ir_exec_graph_new(owner_ctx);
    uint32_t phase;
    bool built = graph && function
        && ir_exec_graph_add_phase(graph, "runtime", &phase, nullptr);
    check("phase graphs reject functions from another IR context",
          built && !ir_exec_graph_add_function(graph, phase, function,
              nullptr, nullptr, nullptr));
    ir_ctx_free(owner_ctx);
    ir_ctx_free(other_ctx);
}

static void
test_phase_result_binding(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, "phase_values",
                                         &ir_profile_bce, "test");
    th_type *integer = module ? ir_type_parse(module, "int") : nullptr;
    th_type *function_type = module
        ? ir_type_parse(module, "() -> int") : nullptr;
    ir_func_t *producer = function_type
        ? ir_func_add(module, "produce", function_type, IR_LINK_INTERNAL)
        : nullptr;
    ir_func_t *consumer = function_type
        ? ir_func_add(module, "consume", function_type, IR_LINK_INTERNAL)
        : nullptr;
    bool built = ctx && module && integer && producer && consumer
        && ir_func_set_execution_phase(ctx, producer, IR_PHASE_COMPTIME);
    ir_block_t *producer_entry = producer
        ? ir_block_add(producer, "entry") : nullptr;
    ir_instr_t *produced = producer_entry
        ? ir_instr_add(producer_entry, IR_OP_UNDEF) : nullptr;
    ir_instr_t *producer_return = producer_entry
        ? ir_instr_add(producer_entry, IR_OP_RETURN) : nullptr;
    built = built && producer_entry && produced && producer_return
        && ir_instr_set_dest(produced, "value", integer)
        && ir_instr_operand_value(producer_return, produced->dest);

    ir_block_t *consumer_entry = consumer
        ? ir_block_add(consumer, "entry") : nullptr;
    ir_instr_t *phase_result = consumer_entry
        ? ir_instr_add(consumer_entry, IR_OP_PHASE_RESULT) : nullptr;
    ir_instr_t *consumer_return = consumer_entry
        ? ir_instr_add(consumer_entry, IR_OP_RETURN) : nullptr;
    built = built && consumer_entry && phase_result && consumer_return
        && ir_instr_set_dest(phase_result, "phase_value", integer)
        && ir_instr_set_phase_result(ctx, phase_result, "module.value.1")
        && ir_instr_operand_value(consumer_return, phase_result->dest);

    ir_diags_t *diagnostics = ir_diags_new(ctx, nullptr);
    ir_props_t verified = built ? ir_verify(module, diagnostics) : 0;
    check("phase.result is a well-typed runtime IR value",
          built && (verified & IR_PROP_TYPED));

    ir_exec_graph_t *graph = ir_exec_graph_new(ctx);
    uint32_t comptime, runtime, producer_id, consumer_id;
    built = built && graph
        && ir_exec_graph_add_phase(graph, "comptime", &comptime, nullptr)
        && ir_exec_graph_add_phase(graph, "runtime", &runtime, nullptr)
        && ir_exec_graph_add_function(graph, comptime, producer,
            nullptr, nullptr, &producer_id)
        && ir_exec_graph_add_function(graph, runtime, consumer,
            nullptr, nullptr, &consumer_id);
    check("a phase result cannot validate without its compile-time producer",
          built && !ir_exec_graph_validate(graph));

    bool bound = built
        && ir_exec_graph_add_phase_result(graph, "module.value.1",
            producer_id, consumer_id, phase_result)
        && ir_exec_graph_validate(graph);
    ir_exec_graph_phase_result_view_t result;
    char *printed = bound ? ir_exec_graph_print(graph) : nullptr;
    check("typed phase results bind compile-time producers to runtime uses",
          bound && ir_exec_graph_phase_result_count(graph) == 1
              && ir_exec_graph_phase_result_at(graph, 0, &result)
              && result.producer_function == producer_id
              && result.consumer_function == consumer_id
              && result.consumer == phase_result
              && ir_type_eq(module, result.type, integer)
              && printed
              && strstr(printed,
                  "phase-result \"module.value.1\" producer=@phase_values.produce consumer=@phase_values.consume.entry#0"));
    core_free(printed);

    ir_func_t *comptime_consumer = function_type
        ? ir_func_add(module, "consume_in_comptime", function_type,
                      IR_LINK_INTERNAL) : nullptr;
    ir_block_t *comptime_entry = comptime_consumer
        ? ir_block_add(comptime_consumer, "entry") : nullptr;
    ir_instr_t *cached = comptime_entry
        ? ir_instr_add(comptime_entry, IR_OP_PHASE_RESULT) : nullptr;
    ir_instr_t *comptime_return = comptime_entry
        ? ir_instr_add(comptime_entry, IR_OP_RETURN) : nullptr;
    bool same_phase = comptime_consumer && comptime_entry && cached
        && comptime_return
        && ir_func_set_execution_phase(ctx, comptime_consumer,
                                       IR_PHASE_COMPTIME)
        && ir_instr_set_dest(cached, "cached_value", integer)
        && ir_instr_set_phase_result(ctx, cached, "module.value.2")
        && ir_instr_operand_value(comptime_return, cached->dest)
        && (ir_verify(module, diagnostics) & IR_PROP_TYPED);
    ir_exec_graph_t *prelude_graph = ir_exec_graph_new(ctx);
    uint32_t prelude_phase, prelude_producer, prelude_consumer;
    same_phase = same_phase && prelude_graph
        && ir_exec_graph_add_phase(prelude_graph, "comptime",
                                   &prelude_phase, nullptr)
        && ir_exec_graph_add_function(prelude_graph, prelude_phase,
            producer, nullptr, nullptr, &prelude_producer)
        && ir_exec_graph_add_function(prelude_graph, prelude_phase,
            comptime_consumer, nullptr, nullptr, &prelude_consumer)
        && ir_exec_graph_add_phase_result(prelude_graph, "module.value.2",
            prelude_producer, prelude_consumer, cached);
    check("same-phase cached results require producer success",
          same_phase && !ir_exec_graph_validate(prelude_graph));
    ir_exec_graph_function_view_t consumer_region;
    uint32_t producer_exit;
    ir_exec_graph_outcome_t producer_outcome;
    same_phase = same_phase
        && ir_exec_graph_function_at(prelude_graph, prelude_consumer,
                                     &consumer_region)
        && ir_exec_graph_function_exit_at(prelude_graph,
            prelude_producer, 0, &producer_exit, &producer_outcome)
        && producer_outcome == IR_EXEC_GRAPH_OUTCOME_SUCCESS
        && ir_exec_graph_add_edge(prelude_graph, consumer_region.entry_node,
            producer_exit, IR_EXEC_GRAPH_EDGE_REQUIRES);
    check("same-phase cached results bind after producer success",
          same_phase && ir_exec_graph_validate(prelude_graph));
    ir_func_t *wrong_producer = function_type
        ? ir_func_add(module, "runtime_producer", function_type,
                      IR_LINK_INTERNAL) : nullptr;
    ir_block_t *wrong_entry = wrong_producer
        ? ir_block_add(wrong_producer, "entry") : nullptr;
    if (wrong_entry) ir_instr_add(wrong_entry, IR_OP_RETURN);
    uint32_t wrong_id = UINT32_MAX;
    bool wrong_added = wrong_entry
        && ir_exec_graph_add_function(graph, runtime, wrong_producer,
            nullptr, nullptr, &wrong_id);
    check("phase results reject a producer outside the compile-time phase",
          wrong_added && !ir_exec_graph_add_phase_result(graph,
              "module.value.1", wrong_id, consumer_id, phase_result));
    ir_ctx_free(ctx);
}

static void
test_phase_global_result_binding(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *source_module = ir_module_new(ctx, "state$comptime",
                                                &ir_profile_bce, "test");
    ir_module_t *target_module = ir_module_new(ctx, "state",
                                                &ir_profile_bce, "test");
    th_type *integer = source_module
        ? ir_type_parse(source_module, "i64") : nullptr;
    ir_global_t *source = integer
        ? ir_global_add(source_module, "state.value", integer) : nullptr;
    ir_global_t *target = target_module
        ? ir_global_add(target_module, "state.value",
                        ir_type_parse(target_module, "i64")) : nullptr;
    ir_global_t *wrong_type_target = target_module
        ? ir_global_add(target_module, "state.wrong_type",
                        ir_type_parse(target_module, "string")) : nullptr;
    ir_global_init_value_t initial = {
        .kind = IR_GLOBAL_INIT_INT,
        .integer = ir_lit_of(0),
    };
    bool built = ctx && source_module && target_module && integer
        && source && target && wrong_type_target;
    check("phase-global result test creates its typed globals", built);
    bool literal_set = built
        && ir_global_set_static_initializer(target_module, target, &initial);
    check("runtime global accepts its literal setup", literal_set);
    bool result_set = literal_set
        && ir_global_set_phase_result_initializer(target_module, target,
                                                   "module.global.state");
    check("runtime global accepts a phase-result initializer", result_set);
    bool wrong_result_set = built
        && ir_global_set_phase_result_initializer(target_module,
            wrong_type_target, "module.global.wrong_type");
    check("uninitialized global accepts a phase-result initializer",
          wrong_result_set);
    check("runtime globals accept typed phase-result initializers",
          literal_set && result_set && wrong_result_set);
    built = literal_set && result_set && wrong_result_set;

    ir_exec_graph_t *graph = ir_exec_graph_new(ctx);
    uint32_t compile_phase = UINT32_MAX, compile_entry = UINT32_MAX;
    uint32_t compile_exit = UINT32_MAX;
    uint32_t runtime_phase = UINT32_MAX, runtime_entry = UINT32_MAX;
    built = built && graph
        && ir_exec_graph_add_phase(graph, "compile", &compile_phase,
                                   &compile_entry)
        && ir_exec_graph_add_phase(graph, "runtime", &runtime_phase,
                                   &runtime_entry)
        && ir_exec_graph_add_phase_exit(graph, compile_phase, "complete",
            IR_EXEC_GRAPH_OUTCOME_SUCCESS, &compile_exit)
        && ir_exec_graph_add_edge(graph, compile_entry, compile_exit,
            IR_EXEC_GRAPH_EDGE_SEQUENCE)
        && ir_exec_graph_add_edge(graph, runtime_entry, compile_exit,
            IR_EXEC_GRAPH_EDGE_REQUIRES);
    check("phase-global result test builds its ordered phases", built);
    bool bound = built && ir_exec_graph_add_global_result(graph,
        "module.global.state", compile_phase, runtime_phase,
        source_module, source, target_module, target);
    check("a later global initializer records an earlier phase's typed state",
          bound && ir_exec_graph_validate(graph));

    ir_exec_graph_global_result_view_t result;
    char *printed = ir_exec_graph_print(graph);
    check("global result bindings expose both symbols and their shared type",
          bound && ir_exec_graph_global_result_count(graph) == 1
              && ir_exec_graph_global_result_at(graph, 0, &result)
              && !strcmp(result.identifier, "module.global.state")
              && result.source_phase == compile_phase
              && result.target_phase == runtime_phase
              && result.source_global == source
              && result.target_global == target
              && ir_type_eq(target_module, result.type, target->type)
              && printed
              && strstr(printed,
                  "global-result \"module.global.state\" phase=\"compile\" source=@state$comptime.state.value target=@state.state.value type=i64"));
    core_free(printed);

    check("global results reject duplicate targets and phase/type mismatches",
          !ir_exec_graph_add_global_result(graph, "module.global.state",
              compile_phase, runtime_phase, source_module, source,
              target_module, target)
          && !ir_exec_graph_add_global_result(graph, "module.global.state",
              runtime_phase, compile_phase, source_module, source,
              target_module, target)
          && !ir_exec_graph_add_global_result(graph,
              "module.global.wrong_type", compile_phase, runtime_phase,
              source_module, source, target_module, wrong_type_target)
          && ir_exec_graph_validate(graph));

    ir_exec_graph_t *ungated_graph = ir_exec_graph_new(ctx);
    uint32_t earlier = UINT32_MAX, earlier_entry = UINT32_MAX;
    uint32_t later = UINT32_MAX, later_entry = UINT32_MAX;
    uint32_t earlier_exit = UINT32_MAX;
    bool ungated = ungated_graph
        && ir_exec_graph_add_phase(ungated_graph, "compile", &earlier,
                                   &earlier_entry)
        && ir_exec_graph_add_phase(ungated_graph, "runtime", &later,
                                   &later_entry)
        && ir_exec_graph_add_phase_exit(ungated_graph, earlier, "complete",
            IR_EXEC_GRAPH_OUTCOME_SUCCESS, &earlier_exit)
        && ir_exec_graph_add_global_result(ungated_graph,
            "module.global.state", earlier, later, source_module, source,
            target_module, target);
    check("global phase results require every successful source exit",
          ungated && !ir_exec_graph_validate(ungated_graph));
    ir_ctx_free(ctx);
}

int main(void)
{
    test_phase_dependency_and_exit_classification();
    test_failure_only_and_empty_phase();
    test_trap_exit_classification();
    test_context_ownership();
    test_phase_result_binding();
    test_phase_global_result_binding();
    return fails != 0;
}
