#pragma once

#include "ir/ir.h"
#include "lib/string.h"

typedef struct ir_exec_graph_t ir_exec_graph_t;

typedef enum {
    IR_EXEC_GRAPH_OUTCOME_NONE,
    IR_EXEC_GRAPH_OUTCOME_SUCCESS,
    IR_EXEC_GRAPH_OUTCOME_FAILURE,
    IR_EXEC_GRAPH_OUTCOME_NONRETURNING,
    IR_EXEC_GRAPH_OUTCOME_OTHER,
    IR_EXEC_GRAPH_OUTCOME_UNCLASSIFIED,
} ir_exec_graph_outcome_t;

typedef enum {
    IR_EXEC_GRAPH_NODE_PHASE_ENTRY,
    IR_EXEC_GRAPH_NODE_FUNCTION_ENTRY,
    IR_EXEC_GRAPH_NODE_FUNCTION_EXIT,
    IR_EXEC_GRAPH_NODE_PHASE_EXIT,
} ir_exec_graph_node_kind_t;

typedef enum {
    IR_EXEC_GRAPH_EDGE_SEQUENCE,
    IR_EXEC_GRAPH_EDGE_REQUIRES,
} ir_exec_graph_edge_kind_t;

typedef ir_exec_graph_outcome_t (*ir_exec_graph_classify_exit_fn)(
    ir_func_t *function, ir_block_t *block, ir_instr_t *terminator,
    void *context);

typedef struct {
    uint32_t id;
    core_string_t name;
    uint32_t entry_node;
} ir_exec_graph_phase_view_t;

typedef struct {
    uint32_t id;
    uint32_t phase;
    ir_exec_graph_node_kind_t kind;
    ir_exec_graph_outcome_t outcome;
    core_string_t label;
    ir_func_t *function;
    ir_block_t *block;
} ir_exec_graph_node_view_t;

typedef struct {
    uint32_t id;
    uint32_t phase;
    ir_func_t *function;
    uint32_t entry_node;
    size_t exit_count;
} ir_exec_graph_function_view_t;

typedef struct {
    uint32_t from;
    uint32_t to;
    ir_exec_graph_edge_kind_t kind;
} ir_exec_graph_edge_view_t;

typedef struct {
    core_string_t identifier;
    uint32_t producer_function;
    uint32_t consumer_function;
    ir_instr_t *consumer;
    th_type *type;
} ir_exec_graph_phase_result_view_t;

typedef struct {
    core_string_t identifier;
    uint32_t source_phase;
    uint32_t target_phase;
    ir_module_t *source_module;
    ir_global_t *source_global;
    ir_module_t *target_module;
    ir_global_t *target_global;
    th_type *type;
} ir_exec_graph_global_result_view_t;

ir_exec_graph_t *ir_exec_graph_new(ir_ctx_t *ctx);
bool ir_exec_graph_add_phase(ir_exec_graph_t *graph, core_string_t name,
                             uint32_t *phase_id, uint32_t *entry_node);
bool ir_exec_graph_add_phase_exit(ir_exec_graph_t *graph, uint32_t phase_id,
                                  core_string_t label,
                                  ir_exec_graph_outcome_t outcome,
                                  uint32_t *node_id);
bool ir_exec_graph_add_function(
    ir_exec_graph_t *graph, uint32_t phase_id, ir_func_t *function,
    ir_exec_graph_classify_exit_fn classify_exit, void *context,
    uint32_t *function_id);
bool ir_exec_graph_add_external_function(
    ir_exec_graph_t *graph, uint32_t phase_id, ir_func_t *function,
    const ir_exec_graph_outcome_t *outcomes, size_t outcome_count,
    uint32_t *function_id);
bool ir_exec_graph_function_at(const ir_exec_graph_t *graph,
                              uint32_t function_id,
                              ir_exec_graph_function_view_t *out);
bool ir_exec_graph_function_exit_at(const ir_exec_graph_t *graph,
                                    uint32_t function_id, size_t index,
                                    uint32_t *node_id,
                                    ir_exec_graph_outcome_t *outcome);
bool ir_exec_graph_add_edge(ir_exec_graph_t *graph, uint32_t from,
                            uint32_t to,
                            ir_exec_graph_edge_kind_t kind);
bool ir_exec_graph_add_phase_result(ir_exec_graph_t *graph,
                                    core_string_t identifier,
                                    uint32_t producer_function,
                                    uint32_t consumer_function,
                                    ir_instr_t *consumer);
bool ir_exec_graph_add_global_result(
    ir_exec_graph_t *graph, core_string_t identifier,
    uint32_t source_phase, uint32_t target_phase,
    ir_module_t *source_module, ir_global_t *source_global,
    ir_module_t *target_module, ir_global_t *target_global);
bool ir_exec_graph_validate(const ir_exec_graph_t *graph);

size_t ir_exec_graph_phase_count(const ir_exec_graph_t *graph);
bool ir_exec_graph_phase_at(const ir_exec_graph_t *graph, size_t index,
                            ir_exec_graph_phase_view_t *out);
size_t ir_exec_graph_node_count(const ir_exec_graph_t *graph);
bool ir_exec_graph_node_at(const ir_exec_graph_t *graph, size_t index,
                           ir_exec_graph_node_view_t *out);
size_t ir_exec_graph_edge_count(const ir_exec_graph_t *graph);
bool ir_exec_graph_edge_at(const ir_exec_graph_t *graph, size_t index,
                           ir_exec_graph_edge_view_t *out);
size_t ir_exec_graph_phase_result_count(const ir_exec_graph_t *graph);
bool ir_exec_graph_phase_result_at(
    const ir_exec_graph_t *graph, size_t index,
    ir_exec_graph_phase_result_view_t *out);
size_t ir_exec_graph_global_result_count(const ir_exec_graph_t *graph);
bool ir_exec_graph_global_result_at(
    const ir_exec_graph_t *graph, size_t index,
    ir_exec_graph_global_result_view_t *out);

core_string_t ir_exec_graph_print(const ir_exec_graph_t *graph);
