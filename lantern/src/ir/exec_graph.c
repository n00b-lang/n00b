#include <string.h>

#include "private/ir/internal.h"
#include "ir/doms.h"
#include "ir/exec_graph.h"
#include "lib/buffer.h"
#include "lib/list.h"

typedef struct {
    const char *name;
    uint32_t entry_node;
} exec_phase_t;
core_list_decl(exec_phase_t);

typedef struct {
    uint32_t id;
    uint32_t phase;
    ir_exec_graph_node_kind_t kind;
    ir_exec_graph_outcome_t outcome;
    const char *label;
    ir_func_t *function;
    ir_block_t *block;
} exec_node_t;
core_list_decl(exec_node_t);

typedef uint32_t exec_node_id_t;
core_list_decl(exec_node_id_t);

typedef struct {
    uint32_t phase;
    ir_func_t *function;
    uint32_t entry_node;
    core_list_t(exec_node_id_t) exits;
} exec_function_t;
core_list_decl(exec_function_t);

typedef struct {
    uint32_t from;
    uint32_t to;
    ir_exec_graph_edge_kind_t kind;
} exec_edge_t;
core_list_decl(exec_edge_t);

typedef struct {
    const char *identifier;
    uint32_t producer_function;
    uint32_t consumer_function;
    ir_instr_t *consumer;
    th_type *type;
} exec_phase_result_t;
core_list_decl(exec_phase_result_t);

typedef struct {
    const char *identifier;
    uint32_t source_phase;
    uint32_t target_phase;
    ir_module_t *source_module;
    ir_global_t *source_global;
    ir_module_t *target_module;
    ir_global_t *target_global;
    th_type *type;
} exec_global_result_t;
core_list_decl(exec_global_result_t);

struct ir_exec_graph_t {
    ir_ctx_t *ctx;
    core_list_t(exec_phase_t) phases;
    core_list_t(exec_node_t) nodes;
    core_list_t(exec_function_t) functions;
    core_list_t(exec_edge_t) edges;
    core_list_t(exec_phase_result_t) phase_results;
    core_list_t(exec_global_result_t) global_results;
};

typedef struct {
    ir_block_t *block;
    ir_exec_graph_outcome_t outcome;
} exec_exit_spec_t;
core_list_decl(exec_exit_spec_t);

static void
exec_graph_free(void *opaque)
{
    ir_exec_graph_t *graph = opaque;
    if (!graph) return;
    for (size_t i = 0; i < graph->functions.len; i++)
        core_list_free(graph->functions.data[i].exits);
    core_list_free(graph->phases);
    core_list_free(graph->nodes);
    core_list_free(graph->functions);
    core_list_free(graph->edges);
    core_list_free(graph->phase_results);
    core_list_free(graph->global_results);
    core_free(graph);
}

ir_exec_graph_t *
ir_exec_graph_new(ir_ctx_t *ctx)
{
    if (!ctx) return nullptr;
    ir_exec_graph_t *graph = core_alloc(ir_exec_graph_t);
    graph->ctx = ctx;
    graph->phases = core_list_new(exec_phase_t);
    graph->nodes = core_list_new(exec_node_t);
    graph->functions = core_list_new(exec_function_t);
    graph->edges = core_list_new(exec_edge_t);
    graph->phase_results = core_list_new(exec_phase_result_t);
    graph->global_results = core_list_new(exec_global_result_t);
    ir_ctx_on_free(ctx, exec_graph_free, graph);
    return graph;
}

static bool
node_add(ir_exec_graph_t *graph, uint32_t phase,
         ir_exec_graph_node_kind_t kind, ir_exec_graph_outcome_t outcome,
         const char *label, ir_func_t *function, ir_block_t *block,
         uint32_t *node_id)
{
    if (node_id) *node_id = UINT32_MAX;
    if (!graph || phase >= graph->phases.len
        || graph->nodes.len >= UINT32_MAX
        || kind > IR_EXEC_GRAPH_NODE_PHASE_EXIT
        || outcome > IR_EXEC_GRAPH_OUTCOME_OTHER)
        return false;
    uint32_t id = (uint32_t)graph->nodes.len;
    core_list_push(graph->nodes, ((exec_node_t){
        .id = id,
        .phase = phase,
        .kind = kind,
        .outcome = outcome,
        .label = label ? ir_strdup(graph->ctx, label) : nullptr,
        .function = function,
        .block = block,
    }));
    if (node_id) *node_id = id;
    return true;
}

bool
ir_exec_graph_add_phase(ir_exec_graph_t *graph, const char *name,
                        uint32_t *phase_id, uint32_t *entry_node)
{
    if (phase_id) *phase_id = UINT32_MAX;
    if (entry_node) *entry_node = UINT32_MAX;
    if (!graph || !name || !name[0] || graph->phases.len >= UINT32_MAX)
        return false;
    for (size_t i = 0; i < graph->phases.len; i++)
        if (!strcmp(graph->phases.data[i].name, name)) return false;

    uint32_t id = (uint32_t)graph->phases.len;
    core_list_push(graph->phases, ((exec_phase_t){
        .name = ir_strdup(graph->ctx, name),
        .entry_node = UINT32_MAX,
    }));
    uint32_t entry;
    if (!node_add(graph, id, IR_EXEC_GRAPH_NODE_PHASE_ENTRY,
            IR_EXEC_GRAPH_OUTCOME_NONE, name, nullptr, nullptr, &entry))
        return false;
    graph->phases.data[id].entry_node = entry;
    if (phase_id) *phase_id = id;
    if (entry_node) *entry_node = entry;
    return true;
}

bool
ir_exec_graph_add_phase_exit(ir_exec_graph_t *graph, uint32_t phase_id,
                             const char *label,
                             ir_exec_graph_outcome_t outcome,
                             uint32_t *node_id)
{
    if (outcome == IR_EXEC_GRAPH_OUTCOME_NONE
        || outcome == IR_EXEC_GRAPH_OUTCOME_UNCLASSIFIED)
        return false;
    return node_add(graph, phase_id, IR_EXEC_GRAPH_NODE_PHASE_EXIT,
                    outcome, label, nullptr, nullptr, node_id);
}

static ir_exec_graph_outcome_t
default_exit_classification(ir_instr_t *terminator)
{
    if (!terminator) return IR_EXEC_GRAPH_OUTCOME_UNCLASSIFIED;
    switch (terminator->op) {
    case IR_OP_RETURN: return IR_EXEC_GRAPH_OUTCOME_SUCCESS;
    case IR_OP_FAIL: return IR_EXEC_GRAPH_OUTCOME_FAILURE;
    case IR_OP_TRAP: return IR_EXEC_GRAPH_OUTCOME_NONRETURNING;
    case IR_OP_UNREACHABLE: return IR_EXEC_GRAPH_OUTCOME_NONRETURNING;
    case IR_OP_JUMP:
    case IR_OP_BRANCH:
    case IR_OP_SWITCH: return IR_EXEC_GRAPH_OUTCOME_NONE;
    default: return IR_EXEC_GRAPH_OUTCOME_UNCLASSIFIED;
    }
}

bool
ir_exec_graph_add_function(
    ir_exec_graph_t *graph, uint32_t phase_id, ir_func_t *function,
    ir_exec_graph_classify_exit_fn classify_exit, void *context,
    uint32_t *function_id)
{
    if (function_id) *function_id = UINT32_MAX;
    if (!graph || phase_id >= graph->phases.len || !function
        || !function->owner || function->owner->ctx != graph->ctx
        || !ir_block_entry(function) || graph->functions.len >= UINT32_MAX)
        return false;

    ir_doms_t *doms = ir_doms_of(function);
    if (!doms) return false;
    core_list_t(exec_exit_spec_t) exits = core_list_new(exec_exit_spec_t);
    bool complete = true;
    for (uint32_t i = 0; i < function->n_blocks; i++) {
        ir_block_t *block = function->blocks[i];
        if (!ir_block_reachable(doms, block)) continue;
        ir_instr_t *terminator = ir_block_terminator(block);
        if (!terminator) {
            complete = false;
            break;
        }
        ir_exec_graph_outcome_t outcome = classify_exit
            ? classify_exit(function, block, terminator, context)
            : default_exit_classification(terminator);
        if (outcome == IR_EXEC_GRAPH_OUTCOME_UNCLASSIFIED
            || outcome > IR_EXEC_GRAPH_OUTCOME_OTHER) {
            complete = false;
            break;
        }
        if (outcome != IR_EXEC_GRAPH_OUTCOME_NONE)
            core_list_push(exits, ((exec_exit_spec_t){block, outcome}));
    }
    if (!complete) {
        core_list_free(exits);
        return false;
    }

    uint32_t id = (uint32_t)graph->functions.len;
    uint32_t entry_node;
    if (!node_add(graph, phase_id, IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY,
            IR_EXEC_GRAPH_OUTCOME_NONE, function->name, function,
            ir_block_entry(function), &entry_node)) {
        core_list_free(exits);
        return false;
    }
    exec_function_t region = {
        .phase = phase_id,
        .function = function,
        .entry_node = entry_node,
        .exits = core_list_new(exec_node_id_t),
    };
    for (size_t i = 0; i < exits.len; i++) {
        exec_exit_spec_t exit = exits.data[i];
        uint32_t node_id;
        if (!node_add(graph, phase_id, IR_EXEC_GRAPH_NODE_FUNCTION_EXIT,
                exit.outcome, function->name, function, exit.block,
                &node_id)) {
            core_list_free(exits);
            core_list_free(region.exits);
            return false;
        }
        core_list_push(region.exits, node_id);
    }
    core_list_free(exits);
    core_list_push(graph->functions, region);
    if (function_id) *function_id = id;
    return true;
}

bool
ir_exec_graph_function_at(const ir_exec_graph_t *graph, uint32_t function_id,
                          ir_exec_graph_function_view_t *out)
{
    if (!graph || !out || function_id >= graph->functions.len) return false;
    exec_function_t *region = &graph->functions.data[function_id];
    *out = (ir_exec_graph_function_view_t){
        .id = function_id,
        .phase = region->phase,
        .function = region->function,
        .entry_node = region->entry_node,
        .exit_count = region->exits.len,
    };
    return true;
}

bool
ir_exec_graph_function_exit_at(const ir_exec_graph_t *graph,
                               uint32_t function_id, size_t index,
                               uint32_t *node_id,
                               ir_exec_graph_outcome_t *outcome)
{
    if (node_id) *node_id = UINT32_MAX;
    if (!graph || function_id >= graph->functions.len) return false;
    exec_function_t *region = &graph->functions.data[function_id];
    if (index >= region->exits.len) return false;
    uint32_t id = region->exits.data[index];
    if (node_id) *node_id = id;
    if (outcome) *outcome = graph->nodes.data[id].outcome;
    return true;
}

static bool
edge_valid(const ir_exec_graph_t *graph, uint32_t from, uint32_t to,
           ir_exec_graph_edge_kind_t kind)
{
    if (!graph || from >= graph->nodes.len || to >= graph->nodes.len
        || from == to || kind > IR_EXEC_GRAPH_EDGE_REQUIRES)
        return false;
    const exec_node_t *source = &graph->nodes.data[from];
    const exec_node_t *target = &graph->nodes.data[to];
    if (kind == IR_EXEC_GRAPH_EDGE_SEQUENCE) {
        bool source_can_continue = source->kind
                == IR_EXEC_GRAPH_NODE_PHASE_ENTRY
            || ((source->kind == IR_EXEC_GRAPH_NODE_FUNCTION_EXIT
                 || source->kind == IR_EXEC_GRAPH_NODE_PHASE_EXIT)
                && source->outcome == IR_EXEC_GRAPH_OUTCOME_SUCCESS);
        bool target_can_start = target->kind
                == IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY
            || (target->kind == IR_EXEC_GRAPH_NODE_PHASE_EXIT
                && target->outcome == IR_EXEC_GRAPH_OUTCOME_SUCCESS);
        return source->phase == target->phase
            && source_can_continue && target_can_start;
    }
    bool source_can_depend = source->kind
            == IR_EXEC_GRAPH_NODE_PHASE_ENTRY
        || source->kind == IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY
        || ((source->kind == IR_EXEC_GRAPH_NODE_FUNCTION_EXIT
             || source->kind == IR_EXEC_GRAPH_NODE_PHASE_EXIT)
            && source->outcome == IR_EXEC_GRAPH_OUTCOME_SUCCESS);
    bool target_is_success = (target->kind
                == IR_EXEC_GRAPH_NODE_FUNCTION_EXIT
            || target->kind == IR_EXEC_GRAPH_NODE_PHASE_EXIT)
        && target->outcome == IR_EXEC_GRAPH_OUTCOME_SUCCESS;
    return source_can_depend && target_is_success;
}

bool
ir_exec_graph_add_edge(ir_exec_graph_t *graph, uint32_t from, uint32_t to,
                       ir_exec_graph_edge_kind_t kind)
{
    if (!edge_valid(graph, from, to, kind)) return false;
    for (size_t i = 0; i < graph->edges.len; i++) {
        exec_edge_t edge = graph->edges.data[i];
        if (edge.from == from && edge.to == to && edge.kind == kind)
            return false;
    }
    core_list_push(graph->edges, ((exec_edge_t){from, to, kind}));
    return true;
}

bool
ir_exec_graph_add_phase_result(ir_exec_graph_t *graph,
                               const char *identifier,
                               uint32_t producer_function,
                               uint32_t consumer_function,
                               ir_instr_t *consumer)
{
    if (!graph || !identifier || !identifier[0]
        || producer_function >= graph->functions.len
        || consumer_function >= graph->functions.len || !consumer
        || !consumer->block || !consumer->block->owner
        || !consumer->block->owner->owner
        || consumer->block->owner->owner->ctx != graph->ctx
        || consumer->op != IR_OP_PHASE_RESULT || !consumer->dest
        || consumer->dest_kind != IR_DEST_VALUE
        || !ir_instr_phase_result(consumer)
        || strcmp(identifier, ir_instr_phase_result(consumer))) return false;

    exec_function_t *producer = &graph->functions.data[producer_function];
    exec_function_t *consumer_region =
        &graph->functions.data[consumer_function];
    ir_func_t *producer_func = producer->function;
    ir_func_t *consumer_func = consumer_region->function;
    if (!producer_func || !consumer_func
        || producer_func->owner->ctx != graph->ctx
        || consumer->block->owner != consumer_func
        || producer->phase > consumer_region->phase
        || producer_func == consumer_func
        || ir_func_execution_phase(producer_func) != IR_PHASE_COMPTIME
        || (ir_func_execution_phase(consumer_func) != IR_PHASE_RUNTIME
            && ir_func_execution_phase(consumer_func)
                != IR_PHASE_COMPTIME)
        || (producer->phase == consumer_region->phase
            && ir_func_execution_phase(consumer_func)
                != IR_PHASE_COMPTIME))
        return false;

    bool instruction_present = false;
    ir_block_t *block = consumer->block;
    for (uint32_t i = 0; i < block->n_instrs; i++)
        instruction_present |= block->instrs[i] == consumer;
    if (!instruction_present) return false;

    th_ctx *types = ir_ctx_types(graph->ctx);
    th_type *result_type = th_func_ret(producer_func->type);
    if (!types || !result_type
        || th_hash(types, result_type) != th_hash(types, consumer->dest->type))
        return false;

    for (size_t i = 0; i < graph->phase_results.len; i++) {
        exec_phase_result_t *existing = &graph->phase_results.data[i];
        if (existing->consumer == consumer) return false;
        if (!strcmp(existing->identifier, identifier)
            && (existing->producer_function != producer_function
                || th_hash(types, existing->type)
                    != th_hash(types, result_type))) return false;
    }

    core_list_push(graph->phase_results, ((exec_phase_result_t){
        .identifier = ir_strdup(graph->ctx, identifier),
        .producer_function = producer_function,
        .consumer_function = consumer_function,
        .consumer = consumer,
        .type = result_type,
    }));
    return true;
}

static bool
global_result_valid(const ir_exec_graph_t *graph,
                    const exec_global_result_t *result)
{
    if (!graph || !result || !result->identifier
        || !result->identifier[0]
        || result->source_phase >= graph->phases.len
        || result->target_phase >= graph->phases.len
        || result->source_phase >= result->target_phase
        || !result->source_module || !result->source_global
        || !result->target_module || !result->target_global
        || result->source_module->ctx != graph->ctx
        || result->target_module->ctx != graph->ctx
        || ir_global_find(result->source_module,
                          result->source_global->name)
            != result->source_global
        || ir_global_find(result->target_module,
                          result->target_global->name)
            != result->target_global
        || !result->source_global->type || !result->target_global->type
        || !result->target_global->has_static_initializer
        || !result->target_global->static_initializer
        || result->target_global->static_initializer->kind
            != IR_GLOBAL_INIT_PHASE_RESULT
        || !result->target_global->static_initializer->phase_result.identifier
        || strcmp(result->identifier,
            result->target_global->static_initializer->phase_result.identifier))
        return false;

    th_ctx *types = ir_ctx_types(graph->ctx);
    return types
        && th_hash(types, result->source_global->type)
            == th_hash(types, result->target_global->type)
        && th_hash(types, result->type)
            == th_hash(types, result->target_global->type);
}

static bool
phase_successes_gate_target(const ir_exec_graph_t *graph,
                            uint32_t source_phase, uint32_t target_phase)
{
    if (!graph || source_phase >= graph->phases.len
        || target_phase >= graph->phases.len) return false;
    uint32_t target_entry = graph->phases.data[target_phase].entry_node;
    bool *scheduled = core_alloc_array(bool, graph->nodes.len);
    uint32_t *queue = core_alloc_array(uint32_t, graph->nodes.len);
    if (!scheduled || !queue) {
        core_free(scheduled);
        core_free(queue);
        return false;
    }
    memset(scheduled, 0, graph->nodes.len * sizeof(*scheduled));
    size_t head = 0, tail = 0;
    uint32_t source_entry = graph->phases.data[source_phase].entry_node;
    scheduled[source_entry] = true;
    queue[tail++] = source_entry;
    while (head < tail) {
        uint32_t id = queue[head++];
        const exec_node_t *node = &graph->nodes.data[id];
        if (node->kind == IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY)
            for (size_t f = 0; f < graph->functions.len; f++) {
                const exec_function_t *function = &graph->functions.data[f];
                if (function->entry_node != id) continue;
                for (size_t x = 0; x < function->exits.len; x++) {
                    uint32_t exit = function->exits.data[x];
                    if (graph->nodes.data[exit].outcome
                            != IR_EXEC_GRAPH_OUTCOME_SUCCESS
                        || scheduled[exit]) continue;
                    scheduled[exit] = true;
                    queue[tail++] = exit;
                }
            }
        for (size_t e = 0; e < graph->edges.len; e++) {
            const exec_edge_t *edge = &graph->edges.data[e];
            if (edge->kind != IR_EXEC_GRAPH_EDGE_SEQUENCE
                || edge->from != id || scheduled[edge->to]) continue;
            scheduled[edge->to] = true;
            queue[tail++] = edge->to;
        }
    }
    bool valid = true;
    for (size_t i = 0; i < graph->nodes.len; i++) {
        const exec_node_t *node = &graph->nodes.data[i];
        if (node->phase != source_phase
            || (node->kind != IR_EXEC_GRAPH_NODE_FUNCTION_EXIT
                && node->kind != IR_EXEC_GRAPH_NODE_PHASE_EXIT)
            || node->outcome != IR_EXEC_GRAPH_OUTCOME_SUCCESS
            || (node->kind == IR_EXEC_GRAPH_NODE_FUNCTION_EXIT
                && !scheduled[i])) continue;
        bool continues = false;
        for (size_t e = 0; e < graph->edges.len; e++) {
            const exec_edge_t *edge = &graph->edges.data[e];
            if (edge->kind == IR_EXEC_GRAPH_EDGE_SEQUENCE
                && edge->from == node->id) {
                continues = true;
                break;
            }
        }
        if (continues) continue;
        bool gated = false;
        for (size_t e = 0; e < graph->edges.len; e++) {
            const exec_edge_t *edge = &graph->edges.data[e];
            if (edge->kind == IR_EXEC_GRAPH_EDGE_REQUIRES
                && edge->from == target_entry && edge->to == node->id) {
                gated = true;
                break;
            }
        }
        if (!gated) {
            valid = false;
            break;
        }
    }
    core_free(scheduled);
    core_free(queue);
    return valid;
}

bool
ir_exec_graph_add_global_result(ir_exec_graph_t *graph,
                                const char *identifier,
                                uint32_t source_phase,
                                uint32_t target_phase,
                                ir_module_t *source_module,
                                ir_global_t *source_global,
                                ir_module_t *target_module,
                                ir_global_t *target_global)
{
    if (!graph || !identifier || !identifier[0]
        || !source_global || !target_global) return false;
    exec_global_result_t result = {
        .identifier = identifier,
        .source_phase = source_phase,
        .target_phase = target_phase,
        .source_module = source_module,
        .source_global = source_global,
        .target_module = target_module,
        .target_global = target_global,
        .type = source_global->type,
    };
    if (!global_result_valid(graph, &result)) return false;
    for (size_t i = 0; i < graph->global_results.len; i++) {
        const exec_global_result_t *existing =
            &graph->global_results.data[i];
        if (existing->target_global == target_global
            || !strcmp(existing->identifier, identifier)) return false;
    }
    result.identifier = ir_strdup(graph->ctx, identifier);
    if (!result.identifier) return false;
    core_list_push(graph->global_results, result);
    return true;
}

static bool
node_valid(const ir_exec_graph_t *graph, const exec_node_t *node)
{
    if (node->phase >= graph->phases.len
        || node->kind > IR_EXEC_GRAPH_NODE_PHASE_EXIT
        || node->outcome > IR_EXEC_GRAPH_OUTCOME_OTHER)
        return false;
    if (node->kind == IR_EXEC_GRAPH_NODE_PHASE_ENTRY)
        return node->outcome == IR_EXEC_GRAPH_OUTCOME_NONE
            && !node->function && !node->block
            && graph->phases.data[node->phase].entry_node == node->id;
    if (node->kind == IR_EXEC_GRAPH_NODE_PHASE_EXIT)
        return node->outcome != IR_EXEC_GRAPH_OUTCOME_NONE
            && !node->function && !node->block;
    if (!node->function || !node->block || !node->function->owner
        || node->function->owner->ctx != graph->ctx
        || node->block->owner != node->function)
        return false;
    bool block_present = false;
    for (uint32_t i = 0; i < node->function->n_blocks; i++)
        block_present |= node->function->blocks[i] == node->block;
    if (!block_present) return false;
    if (node->kind == IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY)
        return node->outcome == IR_EXEC_GRAPH_OUTCOME_NONE
            && ir_block_entry(node->function) == node->block;
    return node->outcome != IR_EXEC_GRAPH_OUTCOME_NONE
        && ir_block_terminator(node->block) != nullptr;
}

bool
ir_exec_graph_validate(const ir_exec_graph_t *graph)
{
    if (!graph || !graph->phases.len) return false;
    for (size_t i = 0; i < graph->phases.len; i++) {
        exec_phase_t *phase = &graph->phases.data[i];
        if (!phase->name || !phase->name[0]
            || phase->entry_node >= graph->nodes.len
            || graph->nodes.data[phase->entry_node].kind
                != IR_EXEC_GRAPH_NODE_PHASE_ENTRY)
            return false;
        for (size_t j = i + 1; j < graph->phases.len; j++)
            if (!strcmp(phase->name, graph->phases.data[j].name))
                return false;
    }
    for (size_t i = 0; i < graph->nodes.len; i++) {
        exec_node_t *node = &graph->nodes.data[i];
        if (node->id != i || !node_valid(graph, node)) return false;
    }
    for (size_t i = 0; i < graph->functions.len; i++) {
        exec_function_t *region = &graph->functions.data[i];
        if (region->phase >= graph->phases.len || !region->function
            || region->function->owner->ctx != graph->ctx
            || region->entry_node >= graph->nodes.len)
            return false;
        exec_node_t *entry = &graph->nodes.data[region->entry_node];
        if (entry->kind != IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY
            || entry->phase != region->phase
            || entry->function != region->function)
            return false;
        for (size_t j = 0; j < region->exits.len; j++) {
            uint32_t id = region->exits.data[j];
            if (id >= graph->nodes.len) return false;
            exec_node_t *exit = &graph->nodes.data[id];
            if (exit->kind != IR_EXEC_GRAPH_NODE_FUNCTION_EXIT
                || exit->phase != region->phase
                || exit->function != region->function)
                return false;
        }
    }
    for (size_t i = 0; i < graph->edges.len; i++) {
        exec_edge_t edge = graph->edges.data[i];
        if (!edge_valid(graph, edge.from, edge.to, edge.kind)) return false;
    }
    for (size_t i = 0; i < graph->phase_results.len; i++) {
        const exec_phase_result_t *result = &graph->phase_results.data[i];
        if (!result->identifier || !result->identifier[0]
            || result->producer_function >= graph->functions.len
            || result->consumer_function >= graph->functions.len
            || !result->consumer || !result->consumer->block
            || result->consumer->op != IR_OP_PHASE_RESULT
            || result->consumer->dest_kind != IR_DEST_VALUE
            || !result->consumer->dest || !result->type
            || result->consumer->block->owner
                != graph->functions.data[result->consumer_function].function
            || !ir_instr_phase_result(result->consumer)
            || strcmp(result->identifier,
                      ir_instr_phase_result(result->consumer))) return false;
        const exec_function_t *producer =
            &graph->functions.data[result->producer_function];
        const exec_function_t *consumer =
            &graph->functions.data[result->consumer_function];
        ir_func_t *producer_func = producer->function;
        ir_func_t *consumer_func = consumer->function;
        th_type *producer_result = producer_func && producer_func->type
            ? th_func_ret(producer_func->type) : nullptr;
        bool consumer_present = false;
        for (uint32_t i = 0; i < result->consumer->block->n_instrs; i++)
            consumer_present |=
                result->consumer->block->instrs[i] == result->consumer;
        if (producer->phase > consumer->phase
            || !producer_func || !consumer_func || !consumer_present
            || !producer_result
            || producer_func->owner->ctx != graph->ctx
            || consumer_func->owner->ctx != graph->ctx
            || ir_func_execution_phase(producer_func)
                != IR_PHASE_COMPTIME
            || (ir_func_execution_phase(consumer_func)
                    != IR_PHASE_RUNTIME
                && ir_func_execution_phase(consumer_func)
                    != IR_PHASE_COMPTIME)
            || (producer->phase == consumer->phase
                && ir_func_execution_phase(consumer_func)
                    != IR_PHASE_COMPTIME)
            || th_hash(ir_ctx_types(graph->ctx), producer_result)
                != th_hash(ir_ctx_types(graph->ctx), result->type)
            || th_hash(ir_ctx_types(graph->ctx), result->type)
                != th_hash(ir_ctx_types(graph->ctx),
                           result->consumer->dest->type)) return false;
        if (producer->phase == consumer->phase) {
            for (size_t p = 0; p < producer->exits.len; p++) {
                uint32_t exit_node = producer->exits.data[p];
                if (graph->nodes.data[exit_node].outcome
                    != IR_EXEC_GRAPH_OUTCOME_SUCCESS) continue;
                bool required = false;
                for (size_t e = 0; e < graph->edges.len; e++) {
                    const exec_edge_t *edge = &graph->edges.data[e];
                    required |= edge->kind == IR_EXEC_GRAPH_EDGE_REQUIRES
                        && edge->from == consumer->entry_node
                        && edge->to == exit_node;
                }
                if (!required) return false;
            }
        }
        for (size_t j = i + 1; j < graph->phase_results.len; j++) {
            const exec_phase_result_t *other =
                &graph->phase_results.data[j];
            if (other->consumer == result->consumer
                || (!strcmp(other->identifier, result->identifier)
                    && (other->producer_function != result->producer_function
                        || th_hash(ir_ctx_types(graph->ctx), other->type)
                            != th_hash(ir_ctx_types(graph->ctx),
                                       result->type)))) return false;
        }
    }
    for (size_t i = 0; i < graph->global_results.len; i++) {
        const exec_global_result_t *result = &graph->global_results.data[i];
        if (!global_result_valid(graph, result)
            || !phase_successes_gate_target(graph, result->source_phase,
                                             result->target_phase))
            return false;
        for (size_t j = i + 1; j < graph->global_results.len; j++) {
            const exec_global_result_t *other =
                &graph->global_results.data[j];
            if (other->target_global == result->target_global
                || !strcmp(other->identifier, result->identifier))
                return false;
        }
    }
    for (size_t f = 0; f < graph->functions.len; f++) {
        const ir_func_t *function = graph->functions.data[f].function;
        if (!function) return false;
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            const ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                const ir_instr_t *instruction = block->instrs[i];
                if (instruction->op != IR_OP_PHASE_RESULT) continue;
                size_t bindings = 0;
                for (size_t r = 0; r < graph->phase_results.len; r++)
                    bindings += graph->phase_results.data[r].consumer
                        == instruction;
                if (bindings != 1) return false;
            }
        }
    }
    return true;
}

size_t
ir_exec_graph_phase_count(const ir_exec_graph_t *graph)
{
    return graph ? graph->phases.len : 0;
}

bool
ir_exec_graph_phase_at(const ir_exec_graph_t *graph, size_t index,
                       ir_exec_graph_phase_view_t *out)
{
    if (!graph || !out || index >= graph->phases.len) return false;
    exec_phase_t phase = graph->phases.data[index];
    *out = (ir_exec_graph_phase_view_t){
        .id = (uint32_t)index,
        .name = phase.name,
        .entry_node = phase.entry_node,
    };
    return true;
}

size_t
ir_exec_graph_node_count(const ir_exec_graph_t *graph)
{
    return graph ? graph->nodes.len : 0;
}

bool
ir_exec_graph_node_at(const ir_exec_graph_t *graph, size_t index,
                      ir_exec_graph_node_view_t *out)
{
    if (!graph || !out || index >= graph->nodes.len) return false;
    exec_node_t node = graph->nodes.data[index];
    *out = (ir_exec_graph_node_view_t){
        .id = node.id,
        .phase = node.phase,
        .kind = node.kind,
        .outcome = node.outcome,
        .label = node.label,
        .function = node.function,
        .block = node.block,
    };
    return true;
}

size_t
ir_exec_graph_edge_count(const ir_exec_graph_t *graph)
{
    return graph ? graph->edges.len : 0;
}

bool
ir_exec_graph_edge_at(const ir_exec_graph_t *graph, size_t index,
                      ir_exec_graph_edge_view_t *out)
{
    if (!graph || !out || index >= graph->edges.len) return false;
    exec_edge_t edge = graph->edges.data[index];
    *out = (ir_exec_graph_edge_view_t){edge.from, edge.to, edge.kind};
    return true;
}

size_t
ir_exec_graph_phase_result_count(const ir_exec_graph_t *graph)
{
    return graph ? graph->phase_results.len : 0;
}

bool
ir_exec_graph_phase_result_at(const ir_exec_graph_t *graph, size_t index,
                              ir_exec_graph_phase_result_view_t *out)
{
    if (!graph || !out || index >= graph->phase_results.len) return false;
    exec_phase_result_t *result = &graph->phase_results.data[index];
    *out = (ir_exec_graph_phase_result_view_t){
        .identifier = result->identifier,
        .producer_function = result->producer_function,
        .consumer_function = result->consumer_function,
        .consumer = result->consumer,
        .type = result->type,
    };
    return true;
}

size_t
ir_exec_graph_global_result_count(const ir_exec_graph_t *graph)
{
    return graph ? graph->global_results.len : 0;
}

bool
ir_exec_graph_global_result_at(const ir_exec_graph_t *graph, size_t index,
                               ir_exec_graph_global_result_view_t *out)
{
    if (!graph || !out || index >= graph->global_results.len) return false;
    const exec_global_result_t *result = &graph->global_results.data[index];
    *out = (ir_exec_graph_global_result_view_t){
        .identifier = result->identifier,
        .source_phase = result->source_phase,
        .target_phase = result->target_phase,
        .source_module = result->source_module,
        .source_global = result->source_global,
        .target_module = result->target_module,
        .target_global = result->target_global,
        .type = result->type,
    };
    return true;
}

static const char *
node_kind_name(ir_exec_graph_node_kind_t kind)
{
    switch (kind) {
    case IR_EXEC_GRAPH_NODE_PHASE_ENTRY: return "phase-entry";
    case IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY: return "function-entry";
    case IR_EXEC_GRAPH_NODE_FUNCTION_EXIT: return "function-exit";
    case IR_EXEC_GRAPH_NODE_PHASE_EXIT: return "phase-exit";
    }
    return "invalid";
}

static const char *
outcome_name(ir_exec_graph_outcome_t outcome)
{
    switch (outcome) {
    case IR_EXEC_GRAPH_OUTCOME_NONE: return "none";
    case IR_EXEC_GRAPH_OUTCOME_SUCCESS: return "success";
    case IR_EXEC_GRAPH_OUTCOME_FAILURE: return "failure";
    case IR_EXEC_GRAPH_OUTCOME_NONRETURNING: return "nonreturning";
    case IR_EXEC_GRAPH_OUTCOME_OTHER: return "other";
    case IR_EXEC_GRAPH_OUTCOME_UNCLASSIFIED: return "unclassified";
    }
    return "invalid";
}

static void
put_quoted(core_buffer_t *buffer, const char *text)
{
    core_buffer_putc(buffer, '"');
    for (const unsigned char *at = (const unsigned char *)text;
         at && *at; at++) {
        if (*at == '\\' || *at == '"') {
            core_buffer_putc(buffer, '\\');
            core_buffer_putc(buffer, (char)*at);
        }
        else if (*at == '\n') core_buffer_puts(buffer, "\\n");
        else if (*at == '\r') core_buffer_puts(buffer, "\\r");
        else if (*at == '\t') core_buffer_puts(buffer, "\\t");
        else core_buffer_putc(buffer, (char)*at);
    }
    core_buffer_putc(buffer, '"');
}

char *
ir_exec_graph_print(const ir_exec_graph_t *graph)
{
    if (!ir_exec_graph_validate(graph)) return nullptr;
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_puts(buffer, "execution-graph:\n");
    for (size_t i = 0; i < graph->phases.len; i++) {
        exec_phase_t *phase = &graph->phases.data[i];
        core_buffer_printf(buffer, "  phase #%zu ", i);
        put_quoted(buffer, phase->name);
        core_buffer_printf(buffer, " entry=#%u\n", phase->entry_node);
    }
    for (size_t i = 0; i < graph->nodes.len; i++) {
        exec_node_t *node = &graph->nodes.data[i];
        core_buffer_printf(buffer, "  node #%u %s phase=", node->id,
                           node_kind_name(node->kind));
        put_quoted(buffer, graph->phases.data[node->phase].name);
        if (node->function) {
            core_buffer_printf(buffer, " function=%s.%s block=%s",
                ir_module_name(node->function->owner), node->function->name,
                node->block->name);
        }
        else if (node->label) {
            core_buffer_puts(buffer, " label=");
            put_quoted(buffer, node->label);
        }
        core_buffer_printf(buffer, " outcome=%s\n",
                           outcome_name(node->outcome));
    }
    for (size_t i = 0; i < graph->edges.len; i++) {
        exec_edge_t edge = graph->edges.data[i];
        core_buffer_printf(buffer, "  edge %s #%u -> #%u\n",
            edge.kind == IR_EXEC_GRAPH_EDGE_SEQUENCE
                ? "sequence" : "requires",
            edge.from, edge.to);
    }
    for (size_t i = 0; i < graph->phase_results.len; i++) {
        const exec_phase_result_t *result = &graph->phase_results.data[i];
        const ir_func_t *producer =
            graph->functions.data[result->producer_function].function;
        const ir_func_t *consumer =
            graph->functions.data[result->consumer_function].function;
        core_buffer_puts(buffer, "  phase-result ");
        put_quoted(buffer, result->identifier);
        core_buffer_printf(buffer, " producer=@%s.%s consumer=@%s.%s.%s#%u result=%%%s\n",
            ir_module_name(producer->owner), producer->name,
            ir_module_name(consumer->owner), consumer->name,
            result->consumer->block->name, result->consumer->index,
            result->consumer->dest->name);
    }
    for (size_t i = 0; i < graph->global_results.len; i++) {
        const exec_global_result_t *result = &graph->global_results.data[i];
        core_buffer_puts(buffer, "  global-result ");
        put_quoted(buffer, result->identifier);
        core_buffer_printf(buffer,
            " phase=\"%s\" source=@%s.%s target=@%s.%s type=%s\n",
            graph->phases.data[result->source_phase].name,
            ir_module_name(result->source_module), result->source_global->name,
            ir_module_name(result->target_module), result->target_global->name,
            th_print(ir_ctx_types(graph->ctx), result->type));
    }
    return core_buffer_take(buffer);
}
