#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#include "driver.h"
#include "compiler_cli.h"
#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_phases.h"
#include "compiler_abi_driver.h"
#include "compiler_object_io.h"
#include "abi_emit.h"
#include "abi_link.h"
#include "abi_semantics.h"
#include "abi_static_graph.h"
#include "global_metadata.h"
#include "llvm_backend.h"
#include "llvm_native_cache.h"
#include "native_frame_maps.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/link_unit.h"
#include "lib/indexed_table.h"
#include "lib/target.h"
#include "lib/object_section.h"
#include "lib/diagnostic.h"
#include "parse/commander.h"
#include "parse/tree.h"
#include "ir/ir.h"
#include "th/environment.h"

typedef struct {
    n00bb_abi_packed_tables_t packed;
    uint8_t *object;
    size_t object_bytes;
} n00bb_abi_artifact_t;

static bool
n00bb_linked_global_bindings(const n00bb_abi_object_view_t *view,
                             core_translation_unit_t *const *units,
                             size_t unit_count,
                             n00bb_llvm_global_binding_t **out,
                             size_t *out_count,
                             core_diagnostics_t *diagnostics)
{
    const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(
        view, N00B_ABI_TABLE_STORAGE);
    if (!table || !out || !out_count) return false;
    size_t capacity = 0;
    for (size_t i = 0; i < unit_count; i++) {
        n00bb_tu_state_t *state = tu_state(units[i]);
        if (!state || !state->ir_module
            || ir_global_count(state->ir_module) > SIZE_MAX - capacity)
            return false;
        capacity += ir_global_count(state->ir_module);
    }
    n00bb_llvm_global_binding_t *bindings = calloc(capacity ? capacity : 1,
                                                    sizeof(*bindings));
    if (!bindings) return false;
    size_t count = 0;
    bool valid = true;
    for (uint32_t i = 1; valid && i < table->record_count; i++) {
        const n00b_abi_storage_v1 *storage = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_STORAGE, i);
        if (!storage || !storage->symbol_index) continue;
        const n00b_abi_symbol_v1 *symbol = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_SYMBOL, storage->symbol_index);
        const n00b_abi_module_v1 *module = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_MODULE, storage->module_index);
        const n00b_abi_native_binding_v1 *native = symbol
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_NATIVE_BINDING,
                                   symbol->native_binding_index) : nullptr;
        n00bb_abi_string_view_t module_name = {0}, symbol_name = {0};
        n00bb_abi_string_view_t native_name = {0};
        valid = symbol && module && native
            && n00bb_abi_object_string(view, &module->full_path,
                                       &module_name)
            && n00bb_abi_object_string(view, &symbol->name, &symbol_name)
            && n00bb_abi_object_string(view, &native->native_name,
                                       &native_name);
        if (!valid) break;
        ir_global_t *matched = nullptr;
        for (size_t u = 0; valid && u < unit_count; u++) {
            ir_module_t *runtime = tu_state(units[u])->ir_module;
            const char *name = ir_module_name(runtime);
            if (strlen(name) != module_name.length
                || memcmp(name, module_name.bytes, module_name.length))
                continue;
            for (size_t g = 0; g < ir_global_count(runtime); g++) {
                ir_global_t *global = ir_global_at(runtime, g);
                size_t global_length = strlen(global->name);
                bool simple = global_length == symbol_name.length
                    && !memcmp(global->name, symbol_name.bytes,
                               symbol_name.length);
                bool qualified = global_length == module_name.length + 1
                        + symbol_name.length
                    && !memcmp(global->name, module_name.bytes,
                               module_name.length)
                    && global->name[module_name.length] == '.'
                    && !memcmp(global->name + module_name.length + 1,
                               symbol_name.bytes, symbol_name.length);
                if (!simple && !qualified) continue;
                if (matched) { valid = false; break; }
                matched = global;
            }
        }
        if (valid && !matched) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "linked storage row %u has no IR global for '%.*s.%.*s'",
                i, (int)module_name.length, module_name.bytes,
                (int)symbol_name.length, symbol_name.bytes);
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-B0010"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
        }
        valid = valid && matched && count < capacity;
        for (size_t j = 0; valid && j < count; j++)
            if (bindings[j].global == matched) valid = false;
        if (!valid) break;
        size_t prefix = view->format == CORE_TARGET_OBJECT_MACH_O ? 1 : 0;
        valid = native_name.length > prefix
            && (!prefix || native_name.bytes[0] == '_');
        if (!valid) break;
        char *name = core_strndup((const char *)native_name.bytes + prefix,
                                  native_name.length - prefix);
        valid = name != nullptr;
        if (valid) bindings[count++] = (n00bb_llvm_global_binding_t){
            .global = matched, .native_name = name, .storage_index = i,
        };
    }
    for (size_t u = 0; valid && u < unit_count; u++) {
        ir_module_t *runtime = tu_state(units[u])->ir_module;
        for (size_t g = 0; valid && g < ir_global_count(runtime); g++) {
            ir_global_t *global = ir_global_at(runtime, g);
            if (global->is_extern || !ir_global_can_read(global,
                    IR_PHASE_RUNTIME)
                || ir_global_storage_duration(global)
                    == IR_STORAGE_DURATION_THREAD) continue;
            bool found = false;
            for (size_t j = 0; j < count; j++)
                if (bindings[j].global == global) { found = true; break; }
            if (!found) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "runtime global '%s.%s' has no linked storage row",
                    ir_module_name(runtime), global->name);
                core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                    CORE_STRING_STATIC("N00BB-B0010"),
                    core_string_borrow(message.data, message.byte_len));
                core_buffer_release(&message);
            }
            valid = found;
        }
    }
    if (!valid) {
        for (size_t i = 0; i < count; i++)
            core_free((void *)bindings[i].native_name);
        free(bindings);
        return false;
    }
    *out = bindings;
    *out_count = count;
    return true;
}

static bool
n00bb_linked_function_indices(const n00bb_abi_object_view_t *view,
                              core_translation_unit_t *const *units,
                              size_t unit_count,
                              n00bb_llvm_function_binding_t **out,
                              size_t *out_count)
{
    const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(view,
        N00B_ABI_TABLE_FUNCTION);
    if (!table || !out || !out_count) return false;
    n00bb_llvm_function_binding_t *bindings = calloc(
        table->record_count ? table->record_count : 1, sizeof(*bindings));
    if (!bindings) return false;
    size_t count = 0;
    for (uint32_t i = 1; i < table->record_count; i++) {
        const n00b_abi_function_v1 *function = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_FUNCTION, i);
        const n00b_abi_symbol_v1 *symbol = function
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_SYMBOL,
                                   function->symbol_index) : nullptr;
        const n00b_abi_module_v1 *module = function
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_MODULE,
                                   function->module_index) : nullptr;
        const n00b_abi_type_v1 *signature = symbol
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_TYPE,
                                   symbol->type_index) : nullptr;
        n00bb_abi_string_view_t module_name = {0}, function_name = {0};
        if (!signature || !module
            || !n00bb_abi_object_string(view, &module->full_path,
                                         &module_name)
            || !n00bb_abi_object_string(view, &function->name,
                                         &function_name)) continue;
        ir_func_t *matched = nullptr;
        for (size_t u = 0; u < unit_count; u++) {
            n00bb_tu_state_t *state = tu_state(units[u]);
            ir_module_t *runtime = state ? state->ir_module : nullptr;
            const char *name = runtime ? ir_module_name(runtime) : nullptr;
            if (!name || strlen(name) != module_name.length
                || memcmp(name, module_name.bytes, module_name.length))
                continue;
            for (size_t f = 0; f < ir_func_count(runtime); f++) {
                ir_func_t *candidate = ir_func_at(runtime, f);
                size_t length = strlen(candidate->name);
                if (!candidate->n_blocks
                    || strstr(candidate->name, "$spec$")
                    || length < function_name.length
                    || memcmp(candidate->name, function_name.bytes,
                              function_name.length)
                    || (length != function_name.length
                        && candidate->name[function_name.length] != '#')
                    || th_hash(ir_module_types(runtime), candidate->type)
                        != signature->typehash) continue;
                if (matched) { free(bindings); return false; }
                matched = candidate;
            }
        }
        if (matched) bindings[count++] = (n00bb_llvm_function_binding_t){
            .function = matched, .index = i,
        };
    }
    *out = bindings;
    *out_count = count;
    return true;
}

static bool
n00bb_linked_finalizers(const n00bb_abi_object_view_t *view,
                        core_translation_unit_t *const *units,
                        size_t unit_count,
                        n00bb_llvm_finalizer_binding_t **out,
                        size_t *out_count)
{
    const n00bb_abi_directory_entry_t *table = n00bb_abi_object_table(view,
        N00B_ABI_TABLE_TYPE);
    if (!table || !out || !out_count) return false;
    n00bb_llvm_finalizer_binding_t *bindings = calloc(
        table->record_count ? table->record_count : 1, sizeof(*bindings));
    if (!bindings) return false;
    size_t count = 0;
    bool valid = true;
    for (uint32_t i = 1; valid && i < table->record_count; i++) {
        const n00b_abi_type_v1 *type = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_TYPE, i);
        if (!type) { valid = false; break; }
        if (!type->finalizer_function_index) continue;
        const n00b_abi_function_v1 *function = n00bb_abi_object_row(view,
            N00B_ABI_TABLE_FUNCTION, type->finalizer_function_index);
        const n00b_abi_symbol_v1 *symbol = function
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_SYMBOL,
                                   function->symbol_index) : nullptr;
        const n00b_abi_module_v1 *module = function
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_MODULE,
                                   function->module_index) : nullptr;
        const n00b_abi_type_v1 *signature = symbol
            ? n00bb_abi_object_row(view, N00B_ABI_TABLE_TYPE,
                                   symbol->type_index) : nullptr;
        n00bb_abi_string_view_t module_name = {0}, function_name = {0};
        valid = signature && module
            && n00bb_abi_object_string(view, &module->full_path,
                                         &module_name)
            && n00bb_abi_object_string(view, &function->name,
                                         &function_name);
        ir_func_t *matched = nullptr;
        for (size_t u = 0; valid && u < unit_count; u++) {
            n00bb_tu_state_t *state = tu_state(units[u]);
            ir_module_t *runtime = state ? state->ir_module : nullptr;
            const char *name = runtime ? ir_module_name(runtime) : nullptr;
            if (!name || strlen(name) != module_name.length
                || memcmp(name, module_name.bytes, module_name.length))
                continue;
            for (size_t f = 0; f < ir_func_count(runtime); f++) {
                ir_func_t *candidate = ir_func_at(runtime, f);
                size_t length = strlen(candidate->name);
                if (!candidate->n_blocks
                    || length < function_name.length
                    || memcmp(candidate->name, function_name.bytes,
                              function_name.length)
                    || (length != function_name.length
                        && candidate->name[function_name.length] != '#')
                    || th_hash(ir_module_types(runtime), candidate->type)
                        != signature->typehash) continue;
                if (matched) { valid = false; break; }
                matched = candidate;
            }
        }
        valid = valid && matched;
        if (valid) bindings[count++] = (n00bb_llvm_finalizer_binding_t){
            .type_index = i, .function = matched,
        };
    }
    if (valid) {
        *out = bindings;
        *out_count = count;
    } else free(bindings);
    return valid;
}

typedef struct {
    bool has_user_native_objects;
} n00bb_storage_origin_context_t;

static bool
n00bb_native_unmanaged_parameter(const ir_value_t *parameter, void *context)
{
    const n00bb_storage_origin_context_t *origin = context;
    if (!parameter || parameter->kind != IR_VAL_PARAM
        || !parameter->owner || !parameter->owner->owner
        || !origin || origin->has_user_native_objects) return false;
    const char *module = ir_module_name(parameter->owner->owner);
    const char *function = parameter->owner->name;
    size_t module_length = module ? strlen(module) : 0;
    for (size_t i = 0; i < n00bb_runtime_native_root_count; i++) {
        const n00bb_runtime_native_root_t *root = &n00bb_runtime_native_roots[i];
        const char *name = root->qualified_name;
        if (!module_length || strncmp(name, module, module_length)
            || name[module_length] != '.'
            || strcmp(name + module_length + 1, function)) continue;
        for (size_t j = 0; j < root->unmanaged_parameter_count; j++)
            if (root->unmanaged_parameters[j] == parameter->index) return true;
    }
    return false;
}

static bool
n00bb_closed_callers(const ir_func_t *function, void *context)
{
    /* Native roots and user objects can call functions outside the IR graph. */
    const n00bb_storage_origin_context_t *origin = context;
    if (!function || !function->owner || !origin
        || origin->has_user_native_objects) return false;
    if (!strcmp(function->name, "__finalize")
        || !strncmp(function->name, "__finalize#", 11)) return false;
    const char *module = ir_module_name(function->owner);
    size_t module_length = module ? strlen(module) : 0;
    for (size_t i = 0; i < n00bb_runtime_native_root_count; i++) {
        const char *root = n00bb_runtime_native_roots[i].qualified_name;
        if (module_length && !strncmp(root, module, module_length)
            && root[module_length] == '.'
            && !strcmp(root + module_length + 1, function->name))
            return false;
    }
    return true;
}

static bool
n00bb_unmanaged_os_mapping(const ir_value_t *value, void *context)
{
    (void)context;
    if (!ir_value_has_external_writable_storage(value)
        || value->kind != IR_VAL_INSTR || !value->def
        || value->def->op != IR_OP_CALL
        || !value->def->n_operands) return false;
    const ir_value_t *callee = ir_operand_value(&value->def->operands[0]);
    return callee && callee->kind == IR_VAL_FUNC && callee->owner
        && callee->owner->owner
        && !strcmp(ir_module_name(callee->owner->owner), "sys.internal.memory")
        && (!strcmp(callee->owner->name, "map_zeroed")
            || !strcmp(callee->owner->name, "__os_map_zeroed"));
}

static bool
n00bb_emit_runtime_literal_graph(n00bb_link_analysis_t *analysis,
    uint8_t **object, size_t *object_bytes, bool *has_graph)
{
    *object = nullptr;
    *object_bytes = 0;
    *has_graph = false;
    size_t node_count = analysis->static_graph_nodes.len;
    if (!node_count) return true;
    bool *selected = calloc(node_count, sizeof(*selected));
    size_t *queue = calloc(node_count, sizeof(*queue));
    size_t *indexes = calloc(node_count, sizeof(*indexes));
    size_t *heads = calloc(node_count, sizeof(*heads));
    size_t edge_count = analysis->static_graph_edges.len;
    size_t *next = calloc(edge_count ? edge_count : 1, sizeof(*next));
    bool valid = selected && queue && indexes && heads && next;
    size_t queued = 0;
    if (valid) {
        for (size_t i = 0; i < node_count; i++) heads[i] = SIZE_MAX;
        for (size_t i = 0; i < edge_count; i++) {
            const n00bb_static_graph_edge_t *edge =
                &analysis->static_graph_edges.data[i];
            if (edge->source >= node_count || edge->target >= node_count) {
                valid = false;
                break;
            }
            next[i] = heads[edge->source];
            heads[edge->source] = i;
        }
    }
    for (size_t i = 0; valid && i < analysis->phase_results.len; i++) {
        const n00bb_phase_result_binding_t *binding =
            &analysis->phase_results.data[i];
        if (binding->materialized_kind != N00BB_LLVM_LITERAL_GRAPH
            || !binding->instruction || !binding->consumer) continue;
        bool reachable = false;
        for (size_t f = 0; f < analysis->runtime_reachable_functions.len; f++)
            if (analysis->runtime_reachable_functions.data[f]
                == binding->consumer) {
                reachable = true;
                break;
            }
        if (!reachable) continue;
        size_t root = binding->graph_root_index;
        if (root >= node_count) { valid = false; break; }
        if (!selected[root]) {
            selected[root] = true;
            queue[queued++] = root;
        }
    }
    for (size_t read = 0; valid && read < queued; read++) {
        size_t source = queue[read];
        for (size_t i = heads[source]; i != SIZE_MAX; i = next[i]) {
            size_t target = analysis->static_graph_edges.data[i].target;
            if (!selected[target]) {
                selected[target] = true;
                queue[queued++] = target;
            }
        }
    }
    if (valid && queued) {
        n00bb_static_graph_node_t *nodes = calloc(queued, sizeof(*nodes));
        n00bb_static_graph_edge_t *edges = calloc(edge_count ? edge_count : 1,
            sizeof(*edges));
        valid = nodes && edges;
        size_t copied = 0, copied_edges = 0;
        for (size_t i = 0; valid && i < node_count; i++)
            if (selected[i]) {
                indexes[i] = copied;
                nodes[copied++] = analysis->static_graph_nodes.data[i].node;
            }
        for (size_t i = 0; valid && i < edge_count; i++) {
            n00bb_static_graph_edge_t edge =
                analysis->static_graph_edges.data[i];
            if (!selected[edge.source]) continue;
            if (!selected[edge.target]) { valid = false; break; }
            edge.source = indexes[edge.source];
            edge.target = indexes[edge.target];
            edges[copied_edges++] = edge;
        }
        if (valid) valid = copied == queued
            && n00bb_static_graph_emit(&analysis->target, nodes, copied,
                edges, copied_edges, object, object_bytes);
        free(nodes);
        free(edges);
        if (valid) *has_graph = true;
    }
    free(selected);
    free(queue);
    free(indexes);
    free(heads);
    free(next);
    return valid;
}

static bool
n00bb_validate_executable_completeness(core_link_unit_t *link,
                                        n00bb_link_analysis_t *analysis,
                                        core_diagnostics_t *diagnostics)
{
    core_link_contribution_view_t contribution;
    static const char *required[] = {
        "n00bb.semantic-ready",
        "n00bb.bootstrap-pipeline-ran",
        "n00bb.initializer-dependencies",
        "n00bb.runtime-module-initializer-order",
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (core_link_unit_require_contribution(link, (core_string_t){0},
                core_string_from_cstr(required[i]), 1, &contribution))
            continue;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "executable build requires complete %s facts", required[i]);
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0012"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
        return false;
    }

    for (size_t i = 0; i < analysis->runtime_reachable_functions.len; i++) {
        ir_func_t *function = analysis->runtime_reachable_functions.data[i];
        if (function && function->linkage == IR_LINK_EXTERN
            && !function->n_blocks) continue;
        bool valid = function && function->owner && function->type;
        const char *missing = valid ? nullptr : "IR function";
        for (uint32_t b = 0; valid && b < function->n_blocks; b++)
            if (!function->blocks[b]
                || !ir_block_terminator(function->blocks[b])) {
                valid = false;
                missing = "terminated control flow";
            }
        for (size_t d = 0; valid
             && d < core_link_unit_declaration_count(link); d++) {
            core_link_declaration_t *declaration =
                core_link_unit_declaration_at(link, d);
            if (core_link_declaration_binding(declaration,
                    CORE_STRING_STATIC("n00bb.ir.function")) != function)
                continue;
            n00bb_link_payload_t *facts = core_link_declaration_binding(
                declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"));
            valid = facts && facts->facts_complete
                && facts->cfg_liveness_complete
                && facts->cleanup_obligations_complete;
            if (!valid) missing = !facts || !facts->facts_complete
                ? "declaration facts" : !facts->cfg_liveness_complete
                    ? "CFG liveness" : "cleanup obligations";
            break;
        }
        if (valid) continue;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "executable build has incomplete %s for reachable function %s.%s",
            missing ? missing : "facts",
            function && function->owner
                ? ir_module_name(function->owner) : "<unknown>",
            function && function->name ? function->name : "<unknown>");
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0012"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
        return false;
    }
    return true;
}

bool
n00bb_build_executable(core_link_unit_t *link, n00bb_link_analysis_t *analysis,
                       parse_cmdr_result_t *args, bool debug_enabled,
                       bool native_cpu,
                       const char *compiler_path,
                       core_diagnostics_t *diagnostics)
{
    core_list_t(core_translation_unit_ptr_t) order =
        core_list_new(core_translation_unit_ptr_t);
    bool ok = link && analysis
        && core_link_unit_root_count(link) == 1
        && n00bb_collect_initializer_order(link,
            "n00bb.runtime-module-initializer-order", &order);
    if (!ok) {
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0010"),
            CORE_STRING_STATIC("runtime initializer order is unavailable"));
        core_list_free(order);
        return false;
    }
    if (!n00bb_validate_executable_completeness(link, analysis,
            diagnostics)) {
        core_list_free(order);
        return false;
    }
    size_t count = order.len;
    bool lto = parse_cmdr_flag_present(args, CORE_STRING_STATIC("--lto"));
    char **names = calloc(count, sizeof(*names));
    char **objects = calloc(count + 3, sizeof(*objects));
    core_buffer_t *lto_modules = lto
        ? calloc(count + 1, sizeof(*lto_modules)) : nullptr;
    size_t phase_capacity = analysis->phase_results.len;
    size_t phase_count = 0;
    n00bb_llvm_phase_array_t *phase_arrays = phase_capacity
        ? calloc(phase_capacity, sizeof(*phase_arrays)) : nullptr;
    size_t graph_node_count = analysis->static_graph_nodes.len;
    n00bb_llvm_graph_node_t *graph_nodes = graph_node_count
        ? calloc(graph_node_count, sizeof(*graph_nodes)) : nullptr;
    ok = names && objects && (!lto || lto_modules)
        && (!phase_capacity || phase_arrays)
        && (!graph_node_count || graph_nodes);
    for (size_t i = 0; ok && i < graph_node_count; i++) {
        const n00bb_captured_graph_node_t *node =
            &analysis->static_graph_nodes.data[i];
        ok = node->node.alias_count && node->node.aliases
            && node->node.aliases[0];
        if (ok) graph_nodes[i] = (n00bb_llvm_graph_node_t){
            .symbol = node->node.aliases[0],
            .typehash = node->typehash,
            .size = node->node.size,
            .alignment = node->node.alignment,
            .allocation = node->address != 0,
            .immutable = node->immutable,
        };
    }
    for (size_t i = 0; ok && i < phase_capacity; i++) {
        const n00bb_phase_result_binding_t *binding =
            &analysis->phase_results.data[i];
        if (!binding->instruction) continue;
        phase_arrays[phase_count] = (n00bb_llvm_phase_array_t){
            .identifier = binding->identifier,
            .bytes = binding->materialized_bytes,
            .byte_len = binding->materialized_byte_len,
            .count = binding->materialized_count,
            .kind = binding->materialized_kind,
            .string_encoding = binding->materialized_encoding,
            .element_stride = binding->materialized_stride,
            .inner_stride = binding->materialized_inner_stride,
            .string_offsets = binding->materialized_string_offsets,
            .string_offset_count = binding->materialized_string_offset_count,
            .graph_nodes = graph_nodes,
            .graph_node_count = graph_node_count,
            .graph_edges = analysis->static_graph_edges.data,
            .graph_edge_count = analysis->static_graph_edges.len,
            .graph_root_index = binding->graph_root_index,
        };
        ok = phase_arrays[phase_count].identifier
            && phase_arrays[phase_count].bytes
            && (phase_arrays[phase_count].kind == N00BB_LLVM_LITERAL_STRING
                || phase_arrays[phase_count].kind
                    == N00BB_LLVM_LITERAL_RELOC
                || phase_arrays[phase_count].element_stride);
        phase_count++;
    }
    uint8_t *linked_abi = nullptr;
    size_t linked_abi_bytes = 0;
    n00bb_llvm_type_index_t *type_indices = nullptr;
    size_t type_index_count = 0;
    n00bb_llvm_global_binding_t *global_bindings = nullptr;
    size_t global_binding_count = 0;
    n00bb_llvm_finalizer_binding_t *finalizers = nullptr;
    size_t finalizer_count = 0;
    n00bb_llvm_function_binding_t *function_indices = nullptr;
    size_t function_index_count = 0;
    const char *build_stage = "linking ABI metadata";
    if (ok) ok = n00bb_link_abi_artifact(link, analysis, &analysis->target,
        true, &linked_abi, &linked_abi_bytes, diagnostics);
    if (ok) build_stage = "reading linked ABI metadata";
    if (ok) {
        const char *read_stage = "opening ABI object";
        n00bb_abi_object_view_t view = {0};
        ok = n00bb_abi_object_open(&analysis->target, linked_abi,
            linked_abi_bytes, &view);
        if (ok) {
            read_stage = "reading type rows";
            const n00bb_abi_directory_entry_t *table =
                n00bb_abi_object_table(&view, N00B_ABI_TABLE_TYPE);
            ok = table != nullptr;
            if (ok) {
                type_index_count = table->record_count;
                type_indices = calloc(type_index_count ? type_index_count : 1,
                    sizeof(*type_indices));
                ok = type_indices != nullptr;
            }
            for (uint32_t i = 0; ok && i < type_index_count; i++) {
                const n00b_abi_type_v1 *type = n00bb_abi_object_row(&view,
                    N00B_ABI_TABLE_TYPE, i);
                ok = type != nullptr;
                if (ok) type_indices[i] = (n00bb_llvm_type_index_t){
                    .typehash = type->typehash, .index = i,
                    .finalizable = type->finalizer_function_index != 0,
                    .array_type = type->kind == N00B_ABI_TYPE_ARRAY,
                };
            }
            if (ok) {
                read_stage = "matching global bindings";
                ok = n00bb_linked_global_bindings(&view, order.data,
                    count, &global_bindings, &global_binding_count,
                    diagnostics);
            }
            if (ok) {
                read_stage = "matching finalizers";
                ok = n00bb_linked_finalizers(&view, order.data, count,
                    &finalizers, &finalizer_count);
            }
            if (ok) {
                read_stage = "matching function indices";
                ok = n00bb_linked_function_indices(&view, order.data,
                    count, &function_indices, &function_index_count);
            }
            n00bb_abi_object_close(&view);
        }
        if (!ok) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "could not read linked ABI type table while %s", read_stage);
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-B0010"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
        }
    }
    core_object_section_t metadata = {0};
    if (ok) build_stage = "emitting native module objects";
    if (ok) ok = core_object_section_find(linked_abi, linked_abi_bytes,
        analysis->target.object_format,
        analysis->target.object_format == CORE_TARGET_OBJECT_MACH_O
            ? "__DATA_CONST" : nullptr,
        analysis->target.object_format == CORE_TARGET_OBJECT_MACH_O
            ? "__n00b_meta" : analysis->target.object_format
                == CORE_TARGET_OBJECT_COFF ? ".n00b$M"
                                       : ".data.rel.ro.n00b_meta",
        &metadata);
    if (ok) objects[count] = n00bb_write_temporary_object(
        linked_abi, linked_abi_bytes);
    ok = ok && objects[count];
    core_free(linked_abi);
    for (size_t i = 0; ok && i < count; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        if (!state || !state->comptime_ir_module || !state->ir_module) {
            core_diagnostics_report(diagnostics, order.data[i],
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0012"),
                CORE_STRING_STATIC("native build is missing a phase module"));
            ok = false;
            break;
        }
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%s.__n00bb_runtime_init",
            ir_module_name(state->ir_module));
        names[i] = core_strndup(name.data, name.byte_len);
        core_buffer_release(&name);
        ok = names[i] != nullptr;
    }
    n00bb_storage_origin_context_t origin_context = {
        .has_user_native_objects = parse_cmdr_flag_value_count(args,
            CORE_STRING_STATIC("--link-object")) != 0,
    };
    ir_storage_origin_t *storage_origin = ok
        ? ir_storage_origin_new(
            analysis->runtime_reachable_functions.data,
            analysis->runtime_reachable_functions.len,
            n00bb_unmanaged_os_mapping, n00bb_native_unmanaged_parameter,
            n00bb_closed_callers, false,
            &origin_context) : nullptr;
    if (ok && !storage_origin) {
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0012"),
            CORE_STRING_STATIC("could not analyze native storage origins"));
        ok = false;
    }
    n00bb_native_frames_t *native_frames = ok
        ? n00bb_native_frames_new(analysis->target.object_format) : nullptr;
    if (ok && !native_frames) ok = false;
    char *executable_path = ok && !lto
        ? n00bb_executable_path(compiler_path) : nullptr;
    n00bb_native_cache_t *native_cache = executable_path
        ? n00bb_native_cache_open(executable_path) : nullptr;
    core_free(executable_path);
    for (size_t i = 0; ok && i < count; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
            state->comptime_ir_module, state->ir_module,
            debug_enabled, native_cpu,
            core_translation_unit_diagnostics(order.data[i]));
        if (!backend) {
            ok = false;
            break;
        }
        n00bb_llvm_backend_bind_native_cache(backend, native_cache);
        ok = n00bb_llvm_backend_bind_reachable_functions(backend, false,
            analysis->runtime_reachable_functions.data,
            analysis->runtime_reachable_functions.len)
            && n00bb_llvm_backend_bind_storage_origin(backend,
                storage_origin);
        if (ok && !parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--gc-conservative-stacks")))
            ok = n00bb_llvm_backend_enable_frame_maps(backend);
        if (ok && (!n00bb_llvm_backend_bind_phase_arrays(backend, phase_arrays,
                                                  phase_count)
            || !n00bb_llvm_backend_bind_type_indices(backend, type_indices,
                                                      type_index_count)
            || !n00bb_llvm_backend_bind_function_indices(backend,
                function_indices, function_index_count)
            || !n00bb_llvm_backend_bind_globals(backend, global_bindings,
                                                global_binding_count)
            || (order.data[i] == core_link_unit_root(link, 0)
                && !n00bb_llvm_backend_bind_finalizers(backend,
                    finalizers, finalizer_count)))) {
            n00bb_llvm_backend_free(backend);
            ok = false;
            break;
        }
        core_buffer_t object = {0};
        bool is_root = order.data[i] == core_link_unit_root(link, 0);
        if (lto)
            ok = n00bb_llvm_backend_emit_linkable_bitcode(backend,
                is_root ? (const char *const *)names : nullptr,
                is_root ? count : 0, is_root ? metadata.size : 0,
                &object, diagnostics);
        else
            ok = n00bb_llvm_backend_emit_object(backend, false,
                is_root ? (const char *const *)names : nullptr,
                is_root ? count : 0, nullptr, 0,
                is_root ? metadata.size : 0,
                &object, diagnostics);
        n00bb_llvm_backend_free(backend);
        if (ok && lto) {
            lto_modules[i] = object;
            object = (core_buffer_t){0};
        } else if (ok) {
            ok = n00bb_native_frames_add(native_frames,
                object.data, object.byte_len);
            if (ok) objects[i] = n00bb_write_temporary_object(
                object.data, object.byte_len);
            ok = ok && objects[i];
        }
        core_buffer_release(&object);
    }
    if (ok && lto) {
        build_stage = "link-time optimization";
        char *runtime_path = n00bb_archive_path(compiler_path,
            "libn00b-abi-runtime.bc");
        ok = runtime_path && n00bb_read_bitcode(runtime_path,
            &lto_modules[count]);
        core_free(runtime_path);
        n00bb_llvm_bitcode_input_t *inputs = ok
            ? calloc(count + 1, sizeof(*inputs)) : nullptr;
        ok = ok && inputs;
        for (size_t i = 0; ok && i < count + 1; i++)
            inputs[i] = (n00bb_llvm_bitcode_input_t){
                .data = lto_modules[i].data,
                .length = lto_modules[i].byte_len,
                .native_support = i == count,
            };
        core_buffer_t object = {0};
        if (ok) ok = n00bb_llvm_backend_link_bitcode(inputs, count + 1,
            debug_enabled, native_cpu,
            !parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--gc-conservative-stacks")),
            &object, diagnostics);
        if (ok) ok = n00bb_native_frames_add(native_frames,
            object.data, object.byte_len);
        if (ok) objects[0] = n00bb_write_temporary_object(
            object.data, object.byte_len);
        ok = ok && objects[0];
        core_buffer_release(&object);
        free(inputs);
    }
    if (ok) {
        uint8_t *frame_object = nullptr;
        size_t frame_object_bytes = 0;
        ok = n00bb_native_frames_emit(native_frames, &analysis->target,
            &frame_object, &frame_object_bytes);
        if (ok) objects[count + 1] = n00bb_write_temporary_object(
            frame_object, frame_object_bytes);
        core_free(frame_object);
        ok = ok && objects[count + 1];
    }
    bool has_static_graph = false;
    if (ok) build_stage = "emitting static literal graph";
    if (ok) {
        uint8_t *graph_object = nullptr;
        size_t graph_object_bytes = 0;
        ok = n00bb_emit_runtime_literal_graph(analysis, &graph_object,
            &graph_object_bytes, &has_static_graph);
        if (ok && has_static_graph) {
            objects[count + 2] = n00bb_write_temporary_object(
                graph_object, graph_object_bytes);
            ok = objects[count + 2] != nullptr;
        }
        core_free(graph_object);
    }
    core_string_t requested = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    char *output = requested.data
        ? core_strndup(requested.data, requested.u8_bytes)
        : core_strdup("a.out");
    char *staging = output ? malloc(strlen(output) + sizeof(".XXXXXX"))
                           : nullptr;
    if (staging) {
        sprintf(staging, "%s.XXXXXX", output);
        int staging_fd = mkstemp(staging);
        if (staging_fd < 0) ok = false;
        else {
            close(staging_fd);
            unlink(staging);
        }
    } else ok = false;
    char *archive = n00bb_archive_path(compiler_path,
        "libn00b-abi-runtime.a");
    int link_spawn_error = 0;
    int link_exit_status = -1;
    int output_rename_error = 0;
    if (ok) build_stage = "linking native executable";
    if (ok && archive) {
#ifdef __APPLE__
        const char *linker = "/usr/bin/clang";
#else
        const char *linker = "clang";
#endif
        int32_t extra_count = parse_cmdr_flag_value_count(args,
            CORE_STRING_STATIC("--link-object"));
        size_t object_count = 0;
        for (size_t i = 0; i < count + 3; i++)
            if (objects[i]) object_count++;
        char **arguments = calloc(object_count + (size_t)extra_count + 5,
                                  sizeof(*arguments));
        ok = arguments != nullptr;
        if (ok) {
            arguments[0] = (char *)linker;
            size_t next = 1;
            for (size_t i = 0; i < count + 3; i++)
                if (objects[i]) arguments[next++] = objects[i];
            for (int32_t i = 0; i < extra_count; i++) {
                core_string_t value = parse_cmdr_flag_str_at(args,
                    CORE_STRING_STATIC("--link-object"), i);
                arguments[next + (size_t)i] = core_strndup(value.data,
                    value.u8_bytes);
                if (!arguments[next + (size_t)i]) ok = false;
            }
            if (ok) {
                arguments[next + (size_t)extra_count] = archive;
                arguments[next + 1 + (size_t)extra_count] = "-o";
                arguments[next + 2 + (size_t)extra_count] = staging;
                pid_t child = 0;
                int launched = posix_spawnp(&child, arguments[0], nullptr,
                    nullptr, arguments, environ);
                if (launched) {
                    link_spawn_error = launched;
                    ok = false;
                }
                else {
                    int status = 0;
                    bool waited = waitpid(child, &status, 0) == child;
                    if (waited && WIFEXITED(status))
                        link_exit_status = WEXITSTATUS(status);
                    ok = waited && link_exit_status == 0;
                }
            }
            if (ok && rename(staging, output)) {
                output_rename_error = errno;
                ok = false;
            }
            for (int32_t i = 0; i < extra_count; i++)
                core_free(arguments[next + (size_t)i]);
        }
        free(arguments);
    } else ok = false;
    if (staging) unlink(staging);
    free(staging);
    core_free(archive);
    for (size_t i = 0; i < count + 3; i++) {
        if (objects && objects[i]) {
            unlink(objects[i]);
            core_free(objects[i]);
        }
    }
    for (size_t i = 0; i < count; i++)
        if (names) core_free(names[i]);
    free(objects);
    if (lto_modules) {
        for (size_t i = 0; i < count + 1; i++)
            core_buffer_release(&lto_modules[i]);
        free(lto_modules);
    }
    free(names);
    free(phase_arrays);
    free(graph_nodes);
    free(type_indices);
    for (size_t i = 0; i < global_binding_count; i++)
        core_free((void *)global_bindings[i].native_name);
    free(global_bindings);
    free(finalizers);
    free(function_indices);
    n00bb_native_cache_close(native_cache);
    n00bb_native_frames_free(native_frames);
    ir_storage_origin_free(storage_origin);
    core_list_free(order);
    if (!ok && !core_diagnostics_failed(diagnostics)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not build native executable while %s",
            build_stage);
        if (link_spawn_error)
            core_buffer_printf(&message, ": linker could not start: %s",
                strerror(link_spawn_error));
        else if (link_exit_status > 0)
            core_buffer_printf(&message, ": linker exited with status %d",
                link_exit_status);
        else if (output_rename_error)
            core_buffer_printf(&message, ": output rename failed: %s",
                strerror(output_rename_error));
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0011"),
            core_string_from_cstr(message.data));
        core_buffer_release(&message);
    }
    core_free(output);
    return ok;
}

static core_link_declaration_t *
abi_declaration_for_symbol(void *context, parse_sym_entry_t *symbol)
{
    return link_declaration_for_symbol(context, symbol);
}

typedef struct {
    core_indexed_table_t *scopes;
    th_ctx *types;
    core_dict_t type_owners;
    core_dict_t callback_type_owners;
} n00bb_abi_source_context_t;

static parse_sym_entry_t *
abi_type_owner(void *context, th_type *type)
{
    n00bb_abi_source_context_t *sources = context;
    if (!sources || !sources->types || !type) return nullptr;
    uint64_t typehash = th_hash(sources->types, th_resolve(type));
    parse_sym_entry_t *owner = core_dict_get(&sources->type_owners,
        (uintptr_t)typehash, nullptr);
    if (owner) return canonical_symbol(owner);
    const char *name = nullptr;
    size_t name_length = 0;
    size_t arguments = 0;
    if (!th_named_info(th_resolve(type), &name, &name_length, nullptr,
                       &arguments) || !arguments) return nullptr;
    char *base_name = core_strndup(name, name_length);
    th_type *base = base_name
        ? th_name(sources->types, base_name) : nullptr;
    core_free(base_name);
    if (!base) return nullptr;
    typehash = th_hash(sources->types, base);
    owner = core_dict_get(&sources->callback_type_owners,
        (uintptr_t)typehash, nullptr);
    return owner ? canonical_symbol(owner) : nullptr;
}

static bool
abi_collect_type_owners(core_link_unit_t *link,
                        n00bb_abi_source_context_t *sources)
{
    if (!link || !sources || !sources->types) return false;
    core_dict_init(&sources->type_owners, nullptr, nullptr);
    core_dict_init(&sources->callback_type_owners, nullptr, nullptr);
    for (size_t i = 0; i < core_link_unit_translation_unit_count(link); i++) {
        core_translation_unit_t *unit = core_link_unit_translation_unit(link, i);
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        if (!state || !state->annotations || !state->annotations->symtab
            || !state->ir_module
            || ir_module_types(state->ir_module) != sources->types)
            return false;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             scope; scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type
                    || core_string_eq(symbol->adt_kind,
                        CORE_STRING_STATIC("alias"))) continue;
                bool shaped = symbol->member_scope
                    && (core_string_eq(symbol->member_scope->scope_tag,
                                       CORE_STRING_STATIC("flagset"))
                        || core_string_eq(symbol->member_scope->scope_tag,
                                          CORE_STRING_STATIC("variant"))
                        || core_string_eq(symbol->member_scope->scope_tag,
                                          CORE_STRING_STATIC("trait")));
                if (!symbol->adt_kind.u8_bytes && !shaped) continue;
                uintptr_t typehash = (uintptr_t)th_hash(sources->types,
                    th_resolve(symbol->type));
                core_dict_add(&sources->type_owners, typehash, symbol);
                if (core_string_eq(symbol->adt_kind,
                        CORE_STRING_STATIC("callback")))
                    core_dict_add(&sources->callback_type_owners,
                        typehash, symbol);
            }
        }
    }
    return true;
}

bool
n00bb_emit_link_comptime_catalog(core_link_unit_t *link,
    n00bb_link_analysis_t *analysis,
    n00bb_abi_comptime_type_t **catalog_types,
    size_t *catalog_type_count, uint8_t **catalog_object,
    size_t *catalog_object_bytes, size_t *catalog_metadata_bytes,
    const char **catalog_failure)
{
    core_translation_unit_t *root = core_link_unit_root(link, 0);
    n00bb_tu_state_t *state = root ? tu_state(root) : nullptr;
    n00bb_abi_source_context_t sources = {
        .types = state && state->comptime_ir_module
            ? ir_module_types(state->comptime_ir_module) : nullptr,
    };
    if (!sources.types) return false;
    bool ok = abi_collect_type_owners(link, &sources);
    if (ok) ok = n00bb_abi_emit_comptime_catalog(analysis->ir_context,
        state->comptime_ir_module,
        analysis->comptime_reachable_functions.data,
        analysis->comptime_reachable_functions.len, &analysis->target,
        abi_type_owner, &sources, catalog_types, catalog_type_count,
        catalog_object, catalog_object_bytes, catalog_metadata_bytes,
        catalog_failure);
    core_dict_free(&sources.type_owners);
    core_dict_free(&sources.callback_type_owners);
    return ok;
}

static core_hash_value_t
abi_scope_hash(void *value)
{
    return (core_hash_value_t)(uintptr_t)value;
}

static bool
abi_scope_equal(const void *left, const void *right)
{
    return left == right;
}

static bool
abi_source_of_node(void *context, parse_tree_t *node,
                   const core_translation_unit_t **unit,
                   parse_token_stream_t **tokens)
{
    n00bb_abi_source_context_t *sources = context;
    if (!sources || !sources->scopes || !node || !unit || !tokens)
        return false;
    parse_scope_t *scope = parse_node_is_token(node)
        ? node->num_parents && !parse_node_is_token(node->parents[0])
            ? core_tree_node_value(node->parents[0]).scope : nullptr
        : core_tree_node_value(node).scope;
    for (; scope; scope = scope->parent) {
        const core_indexed_entry_t *entry = core_indexed_table_find(
            sources->scopes, scope);
        if (!entry) continue;
        n00bb_tu_state_t *state = entry->value;
        if (!state || !state->unit || !state->lex || !state->lex->tokens)
            return false;
        *unit = state->unit;
        *tokens = state->lex->tokens;
        return true;
    }
    return false;
}

static void
free_abi_artifact(void *payload)
{
    n00bb_abi_artifact_t *artifact = payload;
    if (!artifact) return;
    n00bb_abi_packed_tables_release(&artifact->packed);
    core_free(artifact->object);
    core_free(artifact);
}

static bool
abi_graph_root(void *context, const char *identifier, size_t *index)
{
    n00bb_link_analysis_t *analysis = context;
    if (!analysis || !identifier || !index) return false;
    for (size_t i = 0; i < analysis->phase_results.len; i++) {
        const n00bb_phase_result_binding_t *result =
            &analysis->phase_results.data[i];
        if (result->materialized_kind == N00BB_LLVM_LITERAL_GRAPH
            && result->owns_materialized && result->identifier
            && !strcmp(result->identifier, identifier)
            && result->graph_root_index < analysis->static_graph_nodes.len) {
            *index = result->graph_root_index;
            return true;
        }
    }
    return false;
}

static bool
abi_graph_node(void *context, size_t index, const uint8_t **bytes,
    size_t *size, uint32_t *alignment, uint64_t *typehash)
{
    n00bb_link_analysis_t *analysis = context;
    if (!analysis || index >= analysis->static_graph_nodes.len
        || !bytes || !size || !alignment || !typehash) return false;
    const n00bb_captured_graph_node_t *node =
        &analysis->static_graph_nodes.data[index];
    *bytes = node->node.bytes;
    *size = node->node.size;
    *alignment = node->node.alignment;
    *typehash = node->typehash;
    return true;
}

static bool
abi_graph_edge(void *context, size_t index, size_t *source,
    size_t *source_offset, size_t *target, size_t *target_offset)
{
    n00bb_link_analysis_t *analysis = context;
    if (!analysis || index >= analysis->static_graph_edges.len
        || !source || !source_offset || !target || !target_offset)
        return false;
    const n00bb_static_graph_edge_t *edge =
        &analysis->static_graph_edges.data[index];
    *source = edge->source;
    *source_offset = edge->source_offset;
    *target = edge->target;
    *target_offset = edge->target_offset;
    return true;
}

bool
n00bb_emit_abi_artifacts(core_link_unit_t *link,
                         n00bb_link_analysis_t *analysis,
                         bool debug_enabled, bool phase_code_present,
                         bool root_only)
{
    if (!link || !analysis || !core_link_unit_graph_is_finalized(link)
        || core_link_unit_stage_state(link,
            N00BB_LINK_STAGE_STORAGE_LIFETIMES) != CORE_LINK_STAGE_COMPLETE)
        return false;
    n00bb_abi_source_context_t sources = {
        .scopes = core_indexed_table_new(1, UINT32_MAX,
            abi_scope_hash, abi_scope_equal),
    };
    n00bb_abi_graph_source_t graphs = {
        .context = analysis,
        .node_count = analysis->static_graph_nodes.len,
        .edge_count = analysis->static_graph_edges.len,
        .root = abi_graph_root,
        .node = abi_graph_node,
        .edge = abi_graph_edge,
    };
    bool valid = sources.scopes != nullptr;
    size_t count = core_link_unit_translation_unit_count(link);
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *unit = core_link_unit_translation_unit(link,
            i);
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        if (!state || !state->annotations || !state->annotations->symtab
            || !state->lex || !state->lex->tokens || !state->ir_module) {
            valid = false;
            break;
        }
        th_ctx *types = ir_module_types(state->ir_module);
        if (!types || (sources.types && sources.types != types)) {
            valid = false;
            break;
        }
        sources.types = types;
        for (parse_scope_t *scope = state->annotations->symtab->all_scopes;
             valid && scope; scope = scope->all_next) {
            const core_indexed_entry_t *entry = core_indexed_table_intern(
                sources.scopes, scope, state, nullptr);
            valid = entry && entry->value == state;
        }
    }
    if (valid) valid = abi_collect_type_owners(link, &sources);
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *unit = core_link_unit_translation_unit(link,
            i);
        if (root_only && unit != core_link_unit_root(link, 0)) continue;
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        if (!state || !state->ir_module || !state->comptime_ir_module) {
            valid = false;
            break;
        }
        n00bb_abi_artifact_t *artifact = core_alloc(n00bb_abi_artifact_t);
        bool emitted = n00bb_abi_emit_unit(link, unit,
            n00bb_package_id(unit),
            state->annotations->symtab, state->lex->tokens,
            state->comptime_ir_module, state->ir_module,
            phase_code_present, &analysis->target,
            debug_enabled ? N00B_ABI_CAP_DEBUG : 0,
            abi_declaration_for_symbol, analysis,
            abi_source_of_node, &sources, abi_type_owner, &sources, &graphs,
            &artifact->packed,
            &artifact->object, &artifact->object_bytes);
        if (!emitted) {
            free_abi_artifact(artifact);
            valid = false;
            break;
        }
        if (!core_link_unit_add_artifact(link, unit->module.identifier,
                CORE_STRING_STATIC("n00bb.abi.object"), 1, 0, artifact,
                CORE_LINK_PAYLOAD_OWNED, free_abi_artifact)) {
            free_abi_artifact(artifact);
            valid = false;
            break;
        }
    }
    core_indexed_table_free(sources.scopes);
    core_dict_free(&sources.type_owners);
    core_dict_free(&sources.callback_type_owners);
    return valid;
}

static bool
n00bb_write_abi_bytes(core_string_t requested, const uint8_t *object,
                      size_t object_bytes, core_translation_unit_t *unit,
                      core_diagnostics_t *diagnostics)
{
    if (!requested.data || !requested.u8_bytes || !object || !object_bytes)
        return false;
    char *path = core_strndup(requested.data, requested.u8_bytes);
    FILE *file = fopen(path, "wb");
    bool saved = file && fwrite(object, 1, object_bytes, file) == object_bytes;
    if (file && fclose(file)) saved = false;
    if (!saved) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not write ABI metadata '%s'", path);
        core_diagnostics_report(diagnostics, unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0008"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
    }
    core_free(path);
    return saved;
}

bool
n00bb_write_compile_abi_artifact(core_link_unit_t *link,
                                 parse_cmdr_result_t *args,
                                 core_diagnostics_t *diagnostics)
{
    core_translation_unit_t *unit = link && core_link_unit_root_count(link) == 1
        ? core_link_unit_root(link, 0) : nullptr;
    core_link_artifact_view_t view = {0};
    if (!unit || !core_link_unit_require_artifact(link,
            unit->module.identifier, CORE_STRING_STATIC("n00bb.abi.object"),
            1, &view)) return false;
    const n00bb_abi_artifact_t *artifact = view.payload;
    if (!artifact || !artifact->object || !artifact->object_bytes)
        return false;

    core_string_t requested = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    core_buffer_t default_name = {0};
    if (!requested.data) {
        core_buffer_printf(&default_name, "%.*s.n00bm",
            (int)unit->module.identifier.u8_bytes,
            unit->module.identifier.data);
        requested = core_string_borrow(default_name.data,
                                       default_name.byte_len);
    }
    bool saved = n00bb_write_abi_bytes(requested, artifact->object,
        artifact->object_bytes, unit, diagnostics);
    core_buffer_release(&default_name);
    return saved;
}

static bool
n00bb_validate_linked_metadata(const uint8_t *metadata, size_t bytes,
                               void *context)
{
    n00b_abi_image_descriptor_v1 image = {
        .descriptor_version = 1,
        .metadata = (const n00b_abi_header_v1 *)metadata,
        .metadata_bytes = bytes,
    };
    return n00b_abi_v1_validate_image(&image, context) == N00B_ABI_OK;
}

bool
n00bb_link_abi_artifact(core_link_unit_t *link,
                        n00bb_link_analysis_t *analysis,
                        const core_target_desc_t *target,
                        bool final_image,
                        uint8_t **out_object, size_t *out_object_bytes,
                        core_diagnostics_t *diagnostics)
{
    size_t count = core_link_unit_translation_unit_count(link);
    n00bb_abi_object_input_t *inputs = core_alloc_array(
        n00bb_abi_object_input_t, count);
    bool valid = count != 0;
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *unit = core_link_unit_translation_unit(link,
            i);
        core_link_artifact_view_t view = {0};
        if (!unit || !core_link_unit_require_artifact(link,
                unit->module.identifier,
                CORE_STRING_STATIC("n00bb.abi.object"), 1, &view)
            || !view.payload) {
            valid = false;
            break;
        }
        const n00bb_abi_artifact_t *artifact = view.payload;
        if (!artifact->object || !artifact->object_bytes) {
            valid = false;
            break;
        }
        inputs[i] = (n00bb_abi_object_input_t){
            .module = unit->module.identifier,
            .object = artifact->object,
            .object_bytes = artifact->object_bytes,
        };
    }
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    const ir_module_t **modules = core_alloc_array(const ir_module_t *,
                                                    count * 2);
    if (valid && !modules) valid = false;
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *unit = core_link_unit_translation_unit(link,
            i);
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        if (!state || !state->comptime_ir_module || !state->ir_module) {
            valid = false;
            break;
        }
        modules[2 * i] = state->comptime_ir_module;
        modules[2 * i + 1] = state->ir_module;
    }
    n00bb_abi_semantics_t semantics = {
        .environment = analysis->environment,
        .modules = modules,
        .module_count = 2 * count,
    };
    const char *failure_stage = nullptr;
    n00bb_abi_index_plan_failure_t plan_failure = {0};
    n00bb_abi_native_rebase_failure_t rebase_failure = {0};
    n00b_abi_error_v1 validation_error = {0};
    if (valid) valid = n00bb_abi_link_objects_with_semantics(link, target,
        inputs, count, n00bb_abi_link_semantic_rows, &semantics,
        final_image ? n00bb_validate_linked_metadata : nullptr,
        &validation_error,
        &object, &object_bytes, &failure_stage, &plan_failure,
        &rebase_failure);
    core_free(modules);
    core_free(inputs);
    if (!valid) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not link library ABI objects%s%s",
            failure_stage ? " at " : "",
            failure_stage ? failure_stage : "");
        if (plan_failure.reason)
            core_buffer_printf(&message,
                ": input %zu table %u row %u (%s)",
                plan_failure.input, plan_failure.table_kind,
                plan_failure.row_index, plan_failure.reason);
        if (plan_failure.reason
            && plan_failure.table_kind == N00B_ABI_TABLE_LAYOUT)
            core_buffer_printf(&message,
                "; owner type %u points to layout %u",
                plan_failure.owner_type_index,
                plan_failure.owner_row_index);
        if (plan_failure.reason
            && plan_failure.table_kind == N00B_ABI_TABLE_TYPE)
            core_buffer_printf(&message,
                "; type kind %u flags %u hash %llu",
                plan_failure.type_kind, plan_failure.type_flags,
                (unsigned long long)plan_failure.typehash);
        if (plan_failure.reason && plan_failure.input < count) {
            core_translation_unit_t *requester =
                core_link_unit_translation_unit(link, plan_failure.input);
            if (requester && requester->module.identifier.data)
                core_buffer_printf(&message, "; requesting module %.*s",
                    (int)requester->module.identifier.u8_bytes,
                    requester->module.identifier.data);
        }
        if (plan_failure.name.bytes) {
            const char *label = plan_failure.table_kind
                    == N00B_ABI_TABLE_REQUIREMENT
                ? "; required symbol " : "; current type ";
            core_buffer_append(&message, label, strlen(label));
            core_buffer_append(&message,
                (const char *)plan_failure.name.bytes,
                plan_failure.name.length);
        }
        if (plan_failure.prior_row_index) {
            core_buffer_printf(&message, "; prior input %zu row %u",
                plan_failure.prior_input, plan_failure.prior_row_index);
            if (plan_failure.type_kind == N00B_ABI_TYPE_VARIABLE)
                core_buffer_printf(&message,
                    "; variable constraints %u%s vs %u%s",
                    plan_failure.constraint_count,
                    plan_failure.imported_type_variable ? " (import)" : "",
                    plan_failure.prior_constraint_count,
                    plan_failure.prior_imported_type_variable
                        ? " (import)" : "");
            if (plan_failure.prior_name.bytes) {
                core_buffer_append(&message, "; prior type ", 13);
                core_buffer_append(&message,
                    (const char *)plan_failure.prior_name.bytes,
                    plan_failure.prior_name.length);
            }
        }
        if (rebase_failure.reason)
            core_buffer_printf(&message,
                ": table %u row %u input %zu section %u (%s)",
                rebase_failure.table_kind, rebase_failure.row_index,
                rebase_failure.input, rebase_failure.local_section,
                rebase_failure.reason);
        if (validation_error.status)
            core_buffer_printf(&message,
                ": status %u table %u row %u offset %llu",
                validation_error.status, validation_error.table_kind,
                validation_error.record_index,
                (unsigned long long)validation_error.byte_offset);
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0010"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
        return false;
    }
    *out_object = object;
    *out_object_bytes = object_bytes;
    return true;
}

bool
n00bb_write_library_abi_artifact(core_link_unit_t *link,
                                 n00bb_link_analysis_t *analysis,
                                 const core_target_desc_t *target,
                                 parse_cmdr_result_t *args,
                                 core_diagnostics_t *diagnostics)
{
    core_string_t requested = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    if (!requested.data || !requested.u8_bytes) {
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0009"),
            CORE_STRING_STATIC("library ABI output requires --output"));
        return false;
    }
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    if (!n00bb_link_abi_artifact(link, analysis, target, false, &object,
                                 &object_bytes, diagnostics)) return false;
    bool saved = n00bb_write_abi_bytes(requested, object, object_bytes,
        nullptr, diagnostics);
    core_free(object);
    return saved;
}
