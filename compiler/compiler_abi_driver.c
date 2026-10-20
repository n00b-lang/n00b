#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#include "driver.h"
#include "compiler_cli.h"
#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_lower.h"
#include "compiler_phases.h"
#include "compiler_abi_driver.h"
#include "compiler_cache.h"
#include "cached_body.h"
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
#include "compiler_native_roots.h"
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
    n00bb_abi_index_table_t index_table;
    uint8_t *object;
    size_t object_bytes;
} n00bb_abi_artifact_t;

static core_string_t
view_text(n00bb_abi_string_view_t view)
{
    return core_string_borrow((const char *)view.bytes, view.length);
}

static char *
temporary_object_argument(const void *bytes, size_t length)
{
    core_string_t path = n00bb_write_temporary_object(bytes, length);
    char *argument = core_string_to_cstr(path);
    core_free(path.data);
    return argument;
}

static core_string_t
qualified_function_name(core_string_t module, core_string_t function)
{
    core_buffer_t *name = core_buffer_empty();
    core_buffer_append_string(name, module);
    core_buffer_putc(name, '.');
    core_buffer_append_string(name, function);
    return core_buffer_take_string(name);
}

static bool
name_matches_function(core_string_t name, core_string_t function)
{
    return core_string_starts_with(name, function)
        && (name.u8_bytes == function.u8_bytes
            || name.data[function.u8_bytes] == '#');
}

static bool
n00bb_linked_global_bindings(const n00bb_abi_object_view_t *view,
                             const n00bb_cache_t *cache,
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
        ir_module_t *cached = state->interface_module && cache
            ? n00bb_cache_module(cache, units[i]->module.identifier)
            : nullptr;
        if (cached) capacity += ir_global_count(cached);
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
        for (size_t u = 0; valid && !matched && u < 2 * unit_count; u++) {
            n00bb_tu_state_t *unit_state = tu_state(units[u % unit_count]);
            ir_module_t *runtime = u < unit_count ? unit_state->ir_module
                : unit_state->interface_module && cache
                    ? n00bb_cache_module(cache,
                        units[u % unit_count]->module.identifier)
                    : nullptr;
            if (!runtime) continue;
            if (!core_string_eq(ir_module_name(runtime),
                                view_text(module_name)))
                continue;
            for (size_t g = 0; g < ir_global_count(runtime); g++) {
                ir_global_t *global = ir_global_at(runtime, g);
                core_string_t global_name = global->name;
                bool simple = core_string_eq(global_name,
                                             view_text(symbol_name));
                bool qualified = global_name.u8_bytes == module_name.length + 1
                        + symbol_name.length
                    && core_string_starts_with(global_name,
                                               view_text(module_name))
                    && global_name.data[module_name.length] == '.'
                    && core_string_ends_with(global_name,
                                             view_text(symbol_name));
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
        core_string_t name = core_string_from_raw(
            (const char *)native_name.bytes + prefix,
            (int64_t)(native_name.length - prefix));
        valid = name.data != nullptr;
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
                    "runtime global '%.*s.%.*s' has no linked storage row",
                    (int)ir_module_name(runtime).u8_bytes,
                    ir_module_name(runtime).data,
                    (int)global->name.u8_bytes, global->name.data);
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
            core_free(bindings[i].native_name.data);
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
            if (!runtime || !core_string_eq(ir_module_name(runtime),
                                            view_text(module_name)))
                continue;
            for (size_t f = 0; f < ir_func_count(runtime); f++) {
                ir_func_t *candidate = ir_func_at(runtime, f);
                ir_func_t *body = n00bb_cached_body(candidate);
                if (!body->n_blocks
                    || core_string_find(candidate->name,
                                        CORE_STRING_STATIC("$spec$")) >= 0
                    || !name_matches_function(candidate->name,
                                              view_text(function_name))
                    || th_hash(ir_module_types(runtime), body->type)
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
            if (!runtime || !core_string_eq(ir_module_name(runtime),
                                            view_text(module_name)))
                continue;
            for (size_t f = 0; f < ir_func_count(runtime); f++) {
                ir_func_t *candidate = ir_func_at(runtime, f);
                ir_func_t *body = n00bb_cached_body(candidate);
                if (!body->n_blocks
                    || !name_matches_function(candidate->name,
                                              view_text(function_name))
                    || th_hash(ir_module_types(runtime), body->type)
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
    core_string_t module = ir_module_name(parameter->owner->owner);
    core_string_t function = parameter->owner->name;
    const n00bb_runtime_native_root_t *root = module.data && function.data
        ? n00bb_native_root_named(module, function) : nullptr;
    for (size_t j = 0; root && j < root->unmanaged_parameter_count; j++)
        if (root->unmanaged_parameters[j] == parameter->index) return true;
    return false;
}

static bool
n00bb_closed_callers(const ir_func_t *function, void *context)
{
    /* Native roots and user objects can call functions outside the IR graph. */
    const n00bb_storage_origin_context_t *origin = context;
    if (!function || !function->owner || !origin
        || origin->has_user_native_objects) return false;
    if (core_string_eq(function->name, CORE_STRING_STATIC("__finalize"))
        || core_string_starts_with(function->name,
                                   CORE_STRING_STATIC("__finalize#")))
        return false;
    core_string_t module = ir_module_name(function->owner);
    return !module.data || !n00bb_native_root_named(module, function->name);
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
        && core_string_eq(ir_module_name(callee->owner->owner),
                          CORE_STRING_STATIC("sys.internal.memory"))
        && (core_string_eq(callee->owner->name,
                           CORE_STRING_STATIC("map_zeroed"))
            || core_string_eq(callee->owner->name,
                              CORE_STRING_STATIC("__os_map_zeroed")));
}

static bool
n00bb_validate_executable_completeness(core_link_unit_t *link,
                                        n00bb_link_analysis_t *analysis,
                                        core_diagnostics_t *diagnostics)
{
    core_link_contribution_view_t contribution;
    static const core_string_t required[] = {
        CORE_STRING_STATIC("n00bb.semantic-ready"),
        CORE_STRING_STATIC("n00bb.bootstrap-pipeline-ran"),
        CORE_STRING_STATIC("n00bb.initializer-dependencies"),
        CORE_STRING_STATIC(N00BB_INITIALIZER_ORDER_DOMAIN),
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (core_link_unit_require_contribution(link, (core_string_t){0},
                required[i], 1, &contribution))
            continue;
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "executable build requires complete %.*s facts",
            (int)required[i].u8_bytes, required[i].data);
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0012"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
        return false;
    }

    for (size_t i = 0; i < analysis->emitted_functions.len; i++) {
        ir_func_t *function = analysis->emitted_functions.data[i];
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
        core_string_t owner_name = function && function->owner
            ? ir_module_name(function->owner)
            : CORE_STRING_STATIC("<unknown>");
        core_string_t function_name = function && function->name.data
            ? function->name : CORE_STRING_STATIC("<unknown>");
        core_buffer_printf(&message,
            "executable build has incomplete %s for reachable function %.*s.%.*s",
            missing ? missing : "facts",
            (int)owner_name.u8_bytes, owner_name.data,
            (int)function_name.u8_bytes, function_name.data);
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0012"),
            core_string_borrow(message.data, message.byte_len));
        core_buffer_release(&message);
        return false;
    }
    return true;
}

static char *
n00bb_flag_argument(const char *prefix, core_string_t value)
{
    core_buffer_t argument = {0};
    core_buffer_printf(&argument, "%s%.*s", prefix, (int)value.u8_bytes,
                       value.data);
    char *native = core_string_to_cstr(core_buffer_string(&argument));
    core_buffer_release(&argument);
    return native;
}

bool
n00bb_build_time_run_needed(const n00bb_link_analysis_t *analysis)
{
    return analysis && (analysis->build_time_capture
        || n00bb_cache_needs_build_time_capture(analysis->cache)
        || n00bb_root_entry(analysis,
                            CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME)));
}

typedef struct {
    n00bb_link_analysis_t *analysis;
    parse_cmdr_result_t *args;
    bool debug_enabled;
    bool native_cpu;
    bool lto;
    n00bb_native_cache_t *native_cache;
    ir_storage_origin_t *storage_origin;
    const n00bb_llvm_callback_bindings_t *callback_bindings;
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
    const n00bb_llvm_function_binding_t *function_indices;
    size_t function_index_count;
    const n00bb_llvm_global_binding_t *global_bindings;
    size_t global_binding_count;
    n00bb_native_frames_t *native_frames;
    core_diagnostics_t *diagnostics;
    core_list_t(n00bb_cache_module_object_t) *kept;
} module_emission_t;

typedef struct {
    const core_string_t *initializers;
    size_t initializer_count;
    size_t metadata_bytes;
    const n00bb_llvm_finalizer_binding_t *finalizers;
    size_t finalizer_count;
} module_entry_t;

typedef struct {
    ir_module_t *module;
    const n00bb_abi_artifact_t *artifact;
    char **object_path;
    core_buffer_t *bitcode;
    const uint8_t *cached;
    size_t cached_bytes;
    n00bb_llvm_backend_t *backend;
    core_buffer_t object;
    core_diagnostics_t *diagnostics;
    double seconds;
    bool needs_native;
    bool ok;
    bool done;
} module_object_t;

core_list_decl(module_object_t);

static double
monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool
begin_module_object(const module_emission_t *emission, module_object_t *item,
                    core_diagnostics_t *module_diagnostics,
                    const module_entry_t *entry)
{
    n00bb_link_analysis_t *analysis = emission->analysis;
    const n00bb_abi_artifact_t *artifact = item->artifact;
    double started = monotonic_seconds();
    n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(item->module,
        emission->debug_enabled, emission->native_cpu, module_diagnostics);
    item->backend = backend;
    if (!backend) return item->ok = false;
    n00bb_llvm_backend_bind_native_cache(backend, emission->native_cache);
    n00bb_llvm_backend_bind_mutator_classifier(backend,
        n00bb_cache_mutator_classifier(analysis->cache));
    core_string_t table_symbol = artifact
        ? artifact->index_table.symbol : (core_string_t){};
    if (table_symbol.u8_bytes && table_symbol.data[0] == '_'
        && analysis->target.object_format == CORE_TARGET_OBJECT_MACH_O)
        table_symbol = core_string_slice(table_symbol, 1,
                                         table_symbol.u8_bytes - 1);
    n00bb_llvm_index_table_t index_table = {
        .symbol = table_symbol,
        .type_hashes = artifact ? artifact->index_table.type_hashes : nullptr,
        .type_count = artifact ? artifact->index_table.type_count : 0,
        .functions = artifact ? artifact->index_table.functions : nullptr,
        .function_count = artifact
            ? artifact->index_table.function_count : 0,
    };
    bool ok = (!table_symbol.data
            || n00bb_llvm_backend_bind_index_table(backend, &index_table))
        && n00bb_llvm_backend_bind_reachable_functions(backend,
            analysis->emitted_functions.data,
            analysis->emitted_functions.len, emission->callback_bindings)
        && n00bb_llvm_backend_bind_storage_origin(backend,
            emission->storage_origin);
    if (ok && !parse_cmdr_flag_present(emission->args,
            CORE_STRING_STATIC("--gc-conservative-stacks")))
        ok = n00bb_llvm_backend_enable_frame_maps(backend);
    ok = ok && n00bb_llvm_backend_bind_type_indices(backend,
            emission->type_indices, emission->type_index_count)
        && n00bb_llvm_backend_bind_function_indices(backend,
            emission->function_indices, emission->function_index_count)
        && n00bb_llvm_backend_bind_globals(backend,
            emission->global_bindings, emission->global_binding_count)
        && (!entry || n00bb_llvm_backend_bind_finalizers(backend,
            entry->finalizers, entry->finalizer_count));
    const core_string_t *initializers = entry ? entry->initializers : nullptr;
    size_t initializer_count = entry ? entry->initializer_count : 0;
    size_t metadata_bytes = entry ? entry->metadata_bytes : 0;
    if (ok && emission->lto)
        ok = n00bb_llvm_backend_emit_linkable_bitcode(backend, initializers,
            initializer_count, metadata_bytes, &item->object,
            emission->diagnostics);
    else if (ok) {
        ok = n00bb_llvm_backend_prepare_object(backend, initializers,
            initializer_count, metadata_bytes, emission->diagnostics);
        item->needs_native = ok;
    }
    item->seconds += monotonic_seconds() - started;
    return item->ok = ok;
}

static void
finish_module_object(module_object_t *item)
{
    double started = monotonic_seconds();
    item->diagnostics = core_diagnostics_new();
    item->ok = item->diagnostics
        && n00bb_llvm_backend_finish_object(item->backend, &item->object,
                                            item->diagnostics);
    item->seconds += monotonic_seconds() - started;
}

typedef struct {
    module_object_t *items;
    size_t published;
    size_t next;
    bool closed;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t *threads;
    size_t thread_count;
} native_pipeline_t;

static void
finish_if_needed(module_object_t *item)
{
    if (item->needs_native && item->ok) finish_module_object(item);
}

static void *
native_worker(void *opaque)
{
    native_pipeline_t *pipeline = opaque;
    pthread_mutex_lock(&pipeline->lock);
    for (;;) {
        while (pipeline->next >= pipeline->published && !pipeline->closed)
            pthread_cond_wait(&pipeline->changed, &pipeline->lock);
        if (pipeline->next >= pipeline->published) break;
        module_object_t *item = &pipeline->items[pipeline->next++];
        pthread_mutex_unlock(&pipeline->lock);
        finish_if_needed(item);
        pthread_mutex_lock(&pipeline->lock);
        item->done = true;
        pthread_cond_broadcast(&pipeline->changed);
    }
    pthread_mutex_unlock(&pipeline->lock);
    return nullptr;
}

static void
native_pipeline_start(native_pipeline_t *pipeline, module_object_t *items,
                      size_t workers)
{
    *pipeline = (native_pipeline_t){.items = items};
    pthread_mutex_init(&pipeline->lock, nullptr);
    pthread_cond_init(&pipeline->changed, nullptr);
    pipeline->threads = core_alloc_array(pthread_t, workers);
    while (pipeline->thread_count < workers
           && pthread_create(&pipeline->threads[pipeline->thread_count],
                             nullptr, native_worker, pipeline) == 0)
        pipeline->thread_count++;
}

static void
native_pipeline_publish(native_pipeline_t *pipeline, module_object_t *item)
{
    if (!pipeline->thread_count) {
        finish_if_needed(item);
        item->done = true;
    }
    pthread_mutex_lock(&pipeline->lock);
    pipeline->published++;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->lock);
}

static void
native_pipeline_wait(native_pipeline_t *pipeline, module_object_t *item)
{
    pthread_mutex_lock(&pipeline->lock);
    while (!item->done)
        pthread_cond_wait(&pipeline->changed, &pipeline->lock);
    pthread_mutex_unlock(&pipeline->lock);
}

static void
native_pipeline_stop(native_pipeline_t *pipeline)
{
    pthread_mutex_lock(&pipeline->lock);
    pipeline->closed = true;
    pthread_cond_broadcast(&pipeline->changed);
    pthread_mutex_unlock(&pipeline->lock);
    for (size_t i = 0; i < pipeline->thread_count; i++)
        pthread_join(pipeline->threads[i], nullptr);
    core_free(pipeline->threads);
    pthread_cond_destroy(&pipeline->changed);
    pthread_mutex_destroy(&pipeline->lock);
}

static bool
complete_module_object(const module_emission_t *emission,
                       module_object_t *item)
{
    if (item->cached) {
        bool ok = n00bb_native_frames_add(emission->native_frames,
                                          item->cached, item->cached_bytes);
        if (ok) *item->object_path = temporary_object_argument(item->cached,
                                                               item->cached_bytes);
        return ok && *item->object_path;
    }
    bool ok = item->ok;
    if (item->diagnostics) {
        ok = core_diagnostics_merge(emission->diagnostics, item->diagnostics)
            && ok;
        core_diagnostics_free(item->diagnostics);
        item->diagnostics = nullptr;
    }
    core_buffer_t mutators = {0};
    n00bb_llvm_backend_t *backend = item->backend;
    for (size_t i = 0; backend && i < n00bb_llvm_backend_mutator_count(backend);
         i++) {
        core_buffer_append_string(&mutators,
            n00bb_llvm_backend_mutator_name(backend, i));
        core_buffer_putc(&mutators, '\n');
    }
    n00bb_llvm_backend_free(backend);
    item->backend = nullptr;
    if (getenv("N00BB_PROFILE_COMPILE")) {
        core_string_t name = ir_module_name(item->module);
        fprintf(stderr, "PROFILE module-object %.3f %.*s\n", item->seconds,
            (int)name.u8_bytes, name.data);
    }
    core_buffer_t object = item->object;
    item->object = (core_buffer_t){0};
    if (ok && emission->lto) {
        core_buffer_release(&mutators);
        *item->bitcode = object;
        return true;
    }
    if (ok && emission->kept && item->artifact) {
        uint8_t *copy = malloc(object.byte_len ? object.byte_len : 1);
        if (copy) {
            if (object.byte_len) memcpy(copy, object.data, object.byte_len);
            core_list_push(*emission->kept, ((n00bb_cache_module_object_t){
                .module = ir_module_name(item->module),
                .native = copy, .native_bytes = object.byte_len,
                .abi = item->artifact->object,
                .abi_bytes = item->artifact->object_bytes,
                .mutators = (const uint8_t *)mutators.data,
                .mutator_bytes = mutators.byte_len,
            }));
            mutators = (core_buffer_t){0};
        }
    }
    core_buffer_release(&mutators);
    if (ok) ok = n00bb_native_frames_add(emission->native_frames,
        object.data, object.byte_len);
    if (ok) *item->object_path = temporary_object_argument(object.data,
        object.byte_len);
    core_buffer_release(&object);
    return ok && *item->object_path;
}

static void
release_module_object(module_object_t *item)
{
    n00bb_llvm_backend_free(item->backend);
    core_diagnostics_free(item->diagnostics);
    core_buffer_release(&item->object);
    *item = (module_object_t){0};
}

typedef struct {
    const module_emission_t *emission;
    native_pipeline_t pipeline;
    module_object_t *items;
    size_t prepared;
    size_t completed;
    size_t window;
    bool ok;
} module_emitter_t;

static void
module_emitter_start(module_emitter_t *emitter,
                     const module_emission_t *emission, size_t capacity,
                     size_t workers)
{
    *emitter = (module_emitter_t){
        .emission = emission,
        .items = core_alloc_array(module_object_t, capacity),
        .window = 6 * workers,
        .ok = true,
    };
    native_pipeline_start(&emitter->pipeline, emitter->items, workers);
}

static module_object_t *
module_emitter_slot(module_emitter_t *emitter)
{
    return &emitter->items[emitter->prepared];
}

static void
module_emitter_complete_one(module_emitter_t *emitter)
{
    module_object_t *item = &emitter->items[emitter->completed++];
    native_pipeline_wait(&emitter->pipeline, item);
    if (emitter->ok)
        emitter->ok = complete_module_object(emitter->emission, item);
    release_module_object(item);
}

static void
module_emitter_publish(module_emitter_t *emitter)
{
    native_pipeline_publish(&emitter->pipeline,
                            &emitter->items[emitter->prepared++]);
    while (emitter->prepared - emitter->completed > emitter->window)
        module_emitter_complete_one(emitter);
}

static bool
module_emitter_finish(module_emitter_t *emitter)
{
    while (emitter->completed < emitter->prepared)
        module_emitter_complete_one(emitter);
    native_pipeline_stop(&emitter->pipeline);
    core_free(emitter->items);
    return emitter->ok;
}

bool
n00bb_build_executable(core_link_unit_t *link, n00bb_link_analysis_t *analysis,
                       parse_cmdr_result_t *args, bool debug_enabled,
                       bool native_cpu,
                       core_string_t compiler_path,
                       const n00bb_native_inputs_t *native,
                       core_diagnostics_t *diagnostics)
{
    ir_func_t *start = analysis
        ? n00bb_root_entry(analysis, CORE_STRING_STATIC(N00BB_START_NAME))
        : nullptr;
    th_type *start_type = start ? th_resolve(start->type) : nullptr;
    if (start_type && (th_func_nparams(start_type)
                       || !th_is_nil(th_func_ret(start_type)))) {
        core_translation_unit_t *root = core_link_unit_root(link, 0);
        parse_scope_t *scope = root ? module_scope(tu_state(root)) : nullptr;
        parse_sym_entry_t *symbol = scope ? parse_symtab_lookup_in(scope,
            CORE_STRING_STATIC(N00BB_START_NAME)) : nullptr;
        core_diag_location_t location = symbol && symbol->decl_node
            ? link_source_location(symbol->decl_node)
            : (core_diag_location_t){0};
        core_string_t found = n00bb_print_type(ir_module_types(start->owner),
                                               start_type);
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "the root module's start must be 'func start() -> nil', not '%.*s'",
            (int)found.u8_bytes, found.data);
        core_diagnostics_report_at(
            root ? core_translation_unit_diagnostics(root) : diagnostics,
            root, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0030"),
            core_buffer_string(&message), location);
        core_buffer_release(&message);
        return false;
    }
    const n00bb_native_inputs_t no_native = {0};
    if (!native) native = &no_native;
    core_list_t(core_translation_unit_ptr_t) order =
        core_list_new(core_translation_unit_ptr_t);
    bool ok = link && analysis
        && core_link_unit_root_count(link) == 1
        && n00bb_collect_initializer_order(link, &order);
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
    size_t modules = count + analysis->specializations.len;
    bool lto = parse_cmdr_flag_present(args, CORE_STRING_STATIC("--lto"));
    core_string_t *names = calloc(2 * count + 2, sizeof(*names));
    char **objects = calloc(modules + 4, sizeof(*objects));
    core_buffer_t *lto_modules = lto
        ? calloc(modules + 1, sizeof(*lto_modules)) : nullptr;
    ok = names && objects && (!lto || lto_modules);
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
                ok = n00bb_linked_global_bindings(&view, analysis->cache,
                    order.data, count, &global_bindings,
                    &global_binding_count, diagnostics);
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
            ? CORE_STRING_STATIC("__DATA_CONST") : (core_string_t){},
        analysis->target.object_format == CORE_TARGET_OBJECT_MACH_O
            ? CORE_STRING_STATIC("__n00b_meta")
            : analysis->target.object_format == CORE_TARGET_OBJECT_COFF
                ? CORE_STRING_STATIC(".n00b$M")
                : CORE_STRING_STATIC(".data.rel.ro.n00b_meta"),
        &metadata);
    if (ok) objects[modules] = temporary_object_argument(
        linked_abi, linked_abi_bytes);
    ok = ok && objects[modules];
    core_free(linked_abi);
    size_t initializer_count = 0;
    const core_string_t initializers[] = {
        CORE_STRING_STATIC(N00BB_LITERAL_INIT_NAME),
        CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME),
    };
    for (size_t i = 0; ok && i < count; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        if (!state || !state->ir_module) {
            core_diagnostics_report(diagnostics, order.data[i],
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0012"),
                CORE_STRING_STATIC("native build is missing a module"));
            ok = false;
            break;
        }
        if (state->interface_module && state->package_interface) continue;
        for (size_t k = 0; ok && k < 2; k++) {
            if (!ir_func_find(state->ir_module, initializers[k])) continue;
            names[initializer_count] = qualified_function_name(
                ir_module_name(state->ir_module), initializers[k]);
            ok = names[initializer_count++].data != nullptr;
        }
    }
    bool build_time = n00bb_compile_phase() == PARSE_SYM_PHASE_COMPTIME;
    ir_func_t *entry = ok ? n00bb_root_entry(analysis, build_time
        ? CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME)
        : CORE_STRING_STATIC(N00BB_START_NAME)) : nullptr;
    if (entry) {
        names[initializer_count] = qualified_function_name(
            ir_module_name(entry->owner), entry->name);
        ok = names[initializer_count++].data != nullptr;
    }
    for (size_t i = 0; ok && i < count && build_time; i++) {
        n00bb_tu_state_t *state = tu_state(order.data[i]);
        if (!core_string_eq(ir_module_name(state->ir_module),
                            CORE_STRING_STATIC(N00BB_CAPTURE_MODULE))
            || !ir_func_find(state->ir_module,
                             CORE_STRING_STATIC(N00BB_CAPTURE_FUNCTION)))
            continue;
        names[initializer_count] = core_string_dup(CORE_STRING_STATIC(
            N00BB_CAPTURE_MODULE "." N00BB_CAPTURE_FUNCTION));
        ok = names[initializer_count++].data != nullptr;
    }
    n00bb_storage_origin_context_t origin_context = {
        .has_user_native_objects = parse_cmdr_flag_value_count(args,
            CORE_STRING_STATIC("--link-object")) != 0,
    };
    ir_storage_origin_t *storage_origin = ok
        ? ir_storage_origin_new(
            analysis->emitted_functions.data,
            analysis->emitted_functions.len,
            n00bb_unmanaged_os_mapping, n00bb_native_unmanaged_parameter,
            n00bb_closed_callers, false,
            &origin_context) : nullptr;
    if (ok && !storage_origin) {
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-B0012"),
            CORE_STRING_STATIC("could not analyze native storage origins"));
        ok = false;
    }
    n00bb_llvm_callback_bindings_t *callback_bindings = ok
        ? n00bb_llvm_callback_bindings_new(analysis->emitted_functions.data,
                                           analysis->emitted_functions.len)
        : nullptr;
    if (ok && !callback_bindings) ok = false;
    n00bb_native_frames_t *native_frames = ok
        ? n00bb_native_frames_new(analysis->target.object_format) : nullptr;
    if (ok && !native_frames) ok = false;
    core_string_t executable_path = ok && !lto
        ? n00bb_executable_path(compiler_path) : (core_string_t){};
    n00bb_native_cache_t *native_cache = executable_path.data
        ? n00bb_native_cache_open(executable_path) : nullptr;
    core_free(executable_path.data);
    core_list_t(n00bb_cache_module_object_t) kept =
        core_list_new(n00bb_cache_module_object_t);
    module_emission_t emission = {
        .kept = lto ? nullptr : &kept,
        .analysis = analysis, .args = args,
        .debug_enabled = debug_enabled, .native_cpu = native_cpu,
        .lto = lto, .native_cache = native_cache,
        .storage_origin = storage_origin,
        .callback_bindings = callback_bindings,
        .type_indices = type_indices, .type_index_count = type_index_count,
        .function_indices = function_indices,
        .function_index_count = function_index_count,
        .global_bindings = global_bindings,
        .global_binding_count = global_binding_count,
        .native_frames = native_frames, .diagnostics = diagnostics,
    };
    int64_t requested_workers = parse_cmdr_flag_int(args,
        CORE_STRING_STATIC("--jobs"), 4);
    size_t native_workers = requested_workers > 0
        ? (size_t)requested_workers : 1;
    module_emitter_t emitter;
    module_emitter_start(&emitter, &emission,
                         count + analysis->specializations.len,
                         native_workers);
    for (size_t i = 0; ok && emitter.ok && i < count; i++) {
        core_link_artifact_view_t artifact_view = {0};
        const n00bb_abi_artifact_t *artifact =
            core_link_unit_require_artifact(link,
                order.data[i]->module.identifier,
                CORE_STRING_STATIC("n00bb.abi.object"), 1, &artifact_view)
            ? artifact_view.payload : nullptr;
        module_entry_t root_entry = {
            .initializers = names,
            .initializer_count = initializer_count,
            .metadata_bytes = metadata.size,
            .finalizers = finalizers, .finalizer_count = finalizer_count,
        };
        module_object_t *item = module_emitter_slot(&emitter);
        *item = (module_object_t){
            .module = tu_state(order.data[i])->ir_module,
            .artifact = artifact,
            .object_path = &objects[i],
            .bitcode = lto ? &lto_modules[i] : nullptr,
        };
        n00bb_cache_module_object_t cached = {0};
        if (tu_state(order.data[i])->interface_module
            && n00bb_cache_module_object(analysis->cache,
                ir_module_name(item->module), &cached)) {
            item->cached = cached.native;
            item->cached_bytes = cached.native_bytes;
            item->ok = true;
        }
        else ok = begin_module_object(&emission, item,
            core_translation_unit_diagnostics(order.data[i]),
            order.data[i] == core_link_unit_root(link, 0)
                ? &root_entry : nullptr);
        module_emitter_publish(&emitter);
    }
    for (size_t i = 0; ok && emitter.ok
         && i < analysis->specializations.len; i++) {
        ir_module_t *module = analysis->specializations.data[i]
            .instance->owner;
        module_object_t *item = module_emitter_slot(&emitter);
        *item = (module_object_t){
            .module = module,
            .object_path = &objects[count + i],
            .bitcode = lto ? &lto_modules[count + i] : nullptr,
        };
        n00bb_cache_module_object_t cached = {0};
        if (!lto && n00bb_cache_instance_object(analysis->cache, link,
                analysis, &analysis->specializations.data[i], &cached)) {
            item->cached = cached.native;
            item->cached_bytes = cached.native_bytes;
            item->ok = true;
        }
        else {
            core_link_artifact_view_t artifact_view = {0};
            ok = core_link_unit_require_artifact(link,
                    ir_module_name(module),
                    CORE_STRING_STATIC("n00bb.abi.object"), 1,
                    &artifact_view);
            item->artifact = ok ? artifact_view.payload : nullptr;
            ok = ok && begin_module_object(&emission, item, diagnostics,
                                           nullptr);
        }
        module_emitter_publish(&emitter);
    }
    bool emitted = module_emitter_finish(&emitter);
    ok = ok && emitted;
    if (ok && lto) {
        build_stage = "link-time optimization";
        core_string_t runtime_path = n00bb_archive_path(compiler_path,
            CORE_STRING_STATIC("libn00b-abi-runtime.bc"));
        ok = runtime_path.data && n00bb_read_bitcode(runtime_path,
            &lto_modules[modules]);
        core_free(runtime_path.data);
        size_t input_count = modules + 1 + native->bitcode_count;
        n00bb_llvm_bitcode_input_t *inputs = ok
            ? calloc(input_count, sizeof(*inputs)) : nullptr;
        ok = ok && inputs;
        for (size_t i = 0; ok && i < modules + 1; i++)
            inputs[i] = (n00bb_llvm_bitcode_input_t){
                .data = lto_modules[i].data,
                .length = lto_modules[i].byte_len,
                .native_support = i == modules,
            };
        for (size_t i = 0; ok && i < native->bitcode_count; i++)
            inputs[modules + 1 + i] = (n00bb_llvm_bitcode_input_t){
                .data = native->bitcode[i].data,
                .length = native->bitcode[i].byte_len,
            };
        core_buffer_t object = {0};
        if (ok) ok = n00bb_llvm_backend_link_bitcode(inputs, input_count,
            debug_enabled, native_cpu,
            !parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--gc-conservative-stacks")),
            &object, diagnostics);
        if (ok) ok = n00bb_native_frames_add(native_frames,
            object.data, object.byte_len);
        if (ok) objects[0] = temporary_object_argument(
            object.data, object.byte_len);
        ok = ok && objects[0];
        core_buffer_release(&object);
        free(inputs);
    }
    if (ok && !lto && native->bitcode_count) {
        build_stage = "compiling C shims";
        n00bb_llvm_bitcode_input_t *inputs = calloc(native->bitcode_count,
                                                    sizeof(*inputs));
        ok = inputs != nullptr;
        for (size_t i = 0; ok && i < native->bitcode_count; i++)
            inputs[i] = (n00bb_llvm_bitcode_input_t){
                .data = native->bitcode[i].data,
                .length = native->bitcode[i].byte_len,
            };
        core_buffer_t object = {0};
        if (ok) ok = n00bb_llvm_backend_link_bitcode(inputs,
            native->bitcode_count, debug_enabled, native_cpu, false,
            &object, diagnostics);
        if (ok) objects[modules + 3] = temporary_object_argument(
            object.data, object.byte_len);
        ok = ok && objects[modules + 3];
        core_buffer_release(&object);
        free(inputs);
    }
    if (ok) {
        uint8_t *frame_object = nullptr;
        size_t frame_object_bytes = 0;
        ok = n00bb_native_frames_emit(native_frames, &analysis->target,
            &frame_object, &frame_object_bytes);
        if (ok) objects[modules + 1] = temporary_object_argument(
            frame_object, frame_object_bytes);
        core_free(frame_object);
        ok = ok && objects[modules + 1];
    }
    core_string_t requested = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    core_string_t output_name = requested.data
        ? requested : CORE_STRING_STATIC("a.out");
    char *output = core_string_to_cstr(output_name);
    core_buffer_t staging_template = {0};
    core_buffer_append_string(&staging_template, output_name);
    core_buffer_puts(&staging_template, ".XXXXXX");
    char *staging = output
        ? core_string_to_cstr(core_buffer_string(&staging_template))
        : nullptr;
    core_buffer_release(&staging_template);
    if (staging) {
        int staging_fd = mkstemp(staging);
        if (staging_fd < 0) ok = false;
        else {
            close(staging_fd);
            unlink(staging);
        }
    } else ok = false;
    core_string_t archive_name = n00bb_archive_path(compiler_path,
        CORE_STRING_STATIC("libn00b-abi-runtime.a"));
    char *archive = core_string_to_cstr(archive_name);
    core_free(archive_name.data);
    int link_spawn_error = 0;
    int link_exit_status = -1;
    int output_rename_error = 0;
    if (ok) build_stage = "linking native executable";
    if (ok && archive) {
        const char *linker = N00BB_NATIVE_LINKER;
        int32_t extra_count = parse_cmdr_flag_value_count(args,
            CORE_STRING_STATIC("--link-object"));
        size_t object_count = 0;
        for (size_t i = 0; i < modules + 4; i++)
            if (objects[i]) object_count++;
        size_t native_flags = native->archive_count
            + native->search_path_count + native->library_count
            + 2 * native->framework_count;
        char **arguments = calloc(object_count + (size_t)extra_count
                                      + native_flags
                                      + N00BB_SYSTEM_LIBRARY_COUNT + 6,
                                  sizeof(*arguments));
        char **native_arguments = native_flags
            ? calloc(native_flags, sizeof(*native_arguments)) : nullptr;
        ok = arguments != nullptr && (!native_flags || native_arguments);
        size_t native_used = 0;
        for (size_t i = 0; ok && i < native->archive_count; i++)
            native_arguments[native_used++] = n00bb_flag_argument("",
                native->archives[i]);
        for (size_t i = 0; ok && i < native->search_path_count; i++)
            native_arguments[native_used++] = n00bb_flag_argument("-L",
                native->search_paths[i]);
        for (size_t i = 0; ok && i < native->library_count; i++)
            native_arguments[native_used++] = n00bb_flag_argument("-l",
                native->libraries[i]);
        for (size_t i = 0; ok && i < native->framework_count; i++) {
            native_arguments[native_used++] = core_string_to_cstr(
                CORE_STRING_STATIC("-framework"));
            native_arguments[native_used++] = n00bb_flag_argument("",
                native->frameworks[i]);
        }
        for (size_t i = 0; ok && i < native_used; i++)
            if (!native_arguments[i]) ok = false;
        if (ok) {
            arguments[0] = (char *)linker;
            size_t next = 1;
            for (size_t i = 0; i < modules + 4; i++)
                if (objects[i]) arguments[next++] = objects[i];
            for (int32_t i = 0; i < extra_count; i++) {
                core_string_t value = parse_cmdr_flag_str_at(args,
                    CORE_STRING_STATIC("--link-object"), i);
                arguments[next + (size_t)i] = core_string_to_cstr(value);
                if (!arguments[next + (size_t)i]) ok = false;
            }
            if (ok) {
                size_t tail = next + (size_t)extra_count;
                arguments[tail++] = archive;
                for (size_t i = 0; i < native_used; i++)
                    arguments[tail++] = native_arguments[i];
                for (size_t s = 0; s < N00BB_SYSTEM_LIBRARY_COUNT; s++)
                    arguments[tail++] = (char *)n00bb_system_libraries[s];
                if (analysis->target.object_format == CORE_TARGET_OBJECT_ELF
                    && parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--build-time")))
                    arguments[tail++] = "-rdynamic";
                arguments[tail++] = "-o";
                arguments[tail] = staging;
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
#ifdef __APPLE__
            if (ok && debug_enabled) {
                core_buffer_t bundle = {0};
                core_buffer_append_string(&bundle, output_name);
                core_buffer_puts(&bundle, ".dSYM");
                char *native_bundle = core_string_to_cstr(
                    core_buffer_string(&bundle));
                char *dsymutil[] = {"/usr/bin/dsymutil", (char *)output, "-o",
                                    native_bundle, nullptr};
                pid_t child = 0;
                if (native_bundle && !posix_spawn(&child, dsymutil[0], nullptr,
                                                  nullptr, dsymutil, environ)) {
                    int status = 0;
                    (void)waitpid(child, &status, 0);
                }
                core_free(native_bundle);
                core_buffer_release(&bundle);
            }
#endif
            for (int32_t i = 0; i < extra_count; i++)
                core_free(arguments[next + (size_t)i]);
        }
        for (size_t i = 0; i < native_used; i++)
            core_free(native_arguments[i]);
        free(native_arguments);
        free(arguments);
    } else ok = false;
    if (ok && !lto)
        (void)n00bb_cache_store(analysis->cache, link, analysis, kept.data,
                                kept.len);
    for (size_t i = 0; i < kept.len; i++) {
        free((void *)kept.data[i].native);
        core_free((void *)kept.data[i].mutators);
    }
    core_list_free(kept);
    if (staging) unlink(staging);
    free(staging);
    core_free(archive);
    for (size_t i = 0; i < modules + 4; i++) {
        if (objects && objects[i]) {
            unlink(objects[i]);
            core_free(objects[i]);
        }
    }
    for (size_t i = 0; i < 2 * count + 1; i++)
        if (names) core_free(names[i].data);
    free(objects);
    if (lto_modules) {
        for (size_t i = 0; i < modules + 1; i++)
            core_buffer_release(&lto_modules[i]);
        free(lto_modules);
    }
    free(names);
    free(type_indices);
    for (size_t i = 0; i < global_binding_count; i++)
        core_free(global_bindings[i].native_name.data);
    free(global_bindings);
    free(finalizers);
    free(function_indices);
    n00bb_native_cache_close(native_cache);
    n00bb_native_frames_free(native_frames);
    ir_storage_origin_free(storage_origin);
    n00bb_llvm_callback_bindings_free(callback_bindings);
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
            core_buffer_string(&message));
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
    core_string_t name = {};
    size_t arguments = 0;
    if (!th_named_info(th_resolve(type), &name, nullptr, &arguments)
        || !arguments) return nullptr;
    th_type *base = th_name(sources->types, name);
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
        ? node->num_parents ? parse_tree_scope(node->parents[0]) : nullptr
        : parse_tree_scope(node);
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
    n00bb_abi_index_table_release(&artifact->index_table);
    core_free(artifact->object);
    core_free(artifact);
}

static bool
abi_graph_root(void *context, core_string_t identifier, size_t *index)
{
    n00bb_link_analysis_t *analysis = context;
    if (!analysis || !identifier.data || !index) return false;
    for (size_t i = 0; i < analysis->baked_roots.len; i++) {
        const n00bb_baked_root_t *root = &analysis->baked_roots.data[i];
        if (core_string_eq(root->identifier, identifier)
            && root->root_index < analysis->static_graph_nodes.len) {
            *index = root->root_index;
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

static core_string_t
abi_graph_type_name(void *context, size_t index)
{
    n00bb_link_analysis_t *analysis = context;
    return analysis->static_graph_nodes.data[index].type_name;
}

static bool
abi_graph_slot(void *context, size_t index, n00bb_abi_graph_slot_t *out)
{
    n00bb_link_analysis_t *analysis = context;
    if (index >= analysis->static_graph_slots.len) return false;
    const n00bb_static_graph_slot_t *slot =
        &analysis->static_graph_slots.data[index];
    if (slot->table == N00B_ABI_TABLE_NATIVE_BINDING) {
        *out = (n00bb_abi_graph_slot_t){
            .node = slot->node,
            .offset = slot->offset,
            .table = slot->table,
            .native = slot->name,
        };
        return true;
    }
    bool type = slot->table == N00B_ABI_TABLE_TYPE;
    ir_func_t *function = type ? nullptr
        : n00bb_baked_function(analysis, slot->module, slot->name);
    if (!type && !function) return false;
    function = n00bb_cached_body(function);
    *out = (n00bb_abi_graph_slot_t){
        .node = slot->node,
        .offset = slot->offset,
        .table = slot->table,
        .kind = slot->kind,
        .typehash = slot->typehash,
        .type_name = type ? slot->name : (core_string_t){},
        .module = type ? (core_string_t){} : slot->module,
        .function = type ? (core_string_t){} : slot->name,
        .function_type = function ? function->type : nullptr,
    };
    return true;
}

static uint32_t
abi_graph_flags(void *context, size_t index)
{
    n00bb_link_analysis_t *analysis = context;
    const n00bb_captured_graph_node_t *node =
        &analysis->static_graph_nodes.data[index];
    return (node->immutable ? N00B_ABI_BAKED_NODE_IMMUTABLE : 0)
        | (node->array_backing ? N00B_ABI_BAKED_NODE_ARRAY_BACKING : 0)
        | (node->foreign ? N00B_ABI_BAKED_NODE_FOREIGN : 0);
}

static core_translation_unit_t *
module_unit(n00bb_link_analysis_t *analysis, const ir_module_t *module)
{
    core_string_t name = ir_module_name(module);
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_string_t identifier = analysis->units.data[i]->module.identifier;
        if (tu_state(analysis->units.data[i])->ir_module == module
            || core_string_eq(identifier, name))
            return analysis->units.data[i];
    }
    return nullptr;
}

static bool
emit_specialization_abi(core_link_unit_t *link,
                        n00bb_link_analysis_t *analysis,
                        const n00bb_specialization_t *specialization,
                        bool debug_enabled,
                        n00bb_abi_source_context_t *sources)
{
    ir_module_t *module = specialization->instance->owner;
    core_translation_unit_t *generic = module_unit(analysis,
        specialization->generic->owner);
    core_string_t name = core_string_dup(ir_module_name(module));
    core_source_file_t source = {0};
    core_translation_unit_t unit = {0};
    parse_symtab_t *symbols = parse_symtab_new();
    n00bb_abi_artifact_t *artifact = core_alloc(n00bb_abi_artifact_t);
    n00bb_tu_state_t *generic_state = generic ? tu_state(generic) : nullptr;
    bool valid = generic_state && generic_state->lex
        && generic_state->lex->tokens && symbols && artifact
        && core_source_file_init(&source,
            core_source_file_path(&generic->source),
            core_source_file_contents(&generic->source));
    if (valid)
        core_source_file_set_identity(&source,
            core_source_file_identity(&generic->source));
    valid = valid
        && core_translation_unit_init(&unit,
            &(core_module_spec_t){.identifier = name}, &source)
        && core_translation_unit_finalize_imports(&unit)
        && n00bb_abi_emit_unit(nullptr, &unit, n00bb_package_id(generic),
            symbols, generic_state->lex->tokens, module, &analysis->target,
            debug_enabled ? N00B_ABI_CAP_DEBUG : 0,
            abi_declaration_for_symbol, analysis, nullptr, nullptr,
            abi_type_owner, sources, nullptr,
            &artifact->packed, &artifact->index_table,
            &artifact->object, &artifact->object_bytes);
    if (!valid && unit.diagnostics)
        core_diagnostics_merge(core_link_unit_diagnostics(link),
                               unit.diagnostics);
    if (valid) {
        valid = core_link_unit_add_artifact(link, name,
            CORE_STRING_STATIC("n00bb.abi.object"), 1, 0, artifact,
            CORE_LINK_PAYLOAD_OWNED, free_abi_artifact);
        if (valid) artifact = nullptr;
    }
    free_abi_artifact(artifact);
    core_translation_unit_release(&unit);
    core_source_file_release(&source);
    if (symbols) parse_symtab_free(symbols);
    return valid;
}

bool
n00bb_emit_abi_artifacts(core_link_unit_t *link,
                         n00bb_link_analysis_t *analysis,
                         bool debug_enabled,
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
        .slot_count = analysis->static_graph_slots.len,
        .root = abi_graph_root,
        .node = abi_graph_node,
        .edge = abi_graph_edge,
        .flags = abi_graph_flags,
        .type_name = abi_graph_type_name,
        .slot = abi_graph_slot,
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
        if (!state || !state->ir_module) {
            valid = false;
            break;
        }
        n00bb_abi_artifact_t *artifact = core_alloc(n00bb_abi_artifact_t);
        n00bb_cache_module_object_t cached = {0};
        bool from_cache = state->interface_module
            && n00bb_cache_module_object(analysis->cache,
                ir_module_name(state->ir_module), &cached);
        if (from_cache) {
            artifact->object = core_alloc_array(uint8_t, cached.abi_bytes);
            if (artifact->object)
                memcpy(artifact->object, cached.abi, cached.abi_bytes);
            artifact->object_bytes = cached.abi_bytes;
        }
        bool emitted = from_cache ? artifact->object != nullptr
            : n00bb_abi_emit_unit(link, unit,
            n00bb_package_id(unit),
            state->annotations->symtab, state->lex->tokens,
            state->ir_module,
            &analysis->target,
            debug_enabled ? N00B_ABI_CAP_DEBUG : 0,
            abi_declaration_for_symbol, analysis,
            abi_source_of_node, &sources, abi_type_owner, &sources, &graphs,
            &artifact->packed, &artifact->index_table,
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
    for (size_t i = 0; valid && !root_only
         && i < analysis->specializations.len; i++) {
        valid = emit_specialization_abi(link, analysis,
            &analysis->specializations.data[i], debug_enabled, &sources);
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
    char *path = core_string_to_cstr(requested);
    FILE *file = fopen(path, "wb");
    bool saved = file && fwrite(object, 1, object_bytes, file) == object_bytes;
    if (file && fclose(file)) saved = false;
    if (!saved) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not write ABI metadata '%.*s'",
                           (int)requested.u8_bytes, requested.data);
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
    size_t unit_count = core_link_unit_translation_unit_count(link);
    size_t count = unit_count + analysis->specializations.len;
    n00bb_abi_object_input_t *inputs = core_alloc_array(
        n00bb_abi_object_input_t, count);
    bool valid = unit_count != 0;
    for (size_t i = 0; valid && i < count; i++) {
        core_translation_unit_t *unit = i < unit_count
            ? core_link_unit_translation_unit(link, i) : nullptr;
        core_string_t module = unit ? unit->module.identifier
            : ir_module_name(analysis->specializations
                .data[i - unit_count].instance->owner);
        core_link_artifact_view_t view = {0};
        if ((i < unit_count && !unit)
            || !core_link_unit_require_artifact(link, module,
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
            .module = module,
            .object = artifact->object,
            .object_bytes = artifact->object_bytes,
        };
    }
    uint8_t *object = nullptr;
    size_t object_bytes = 0;
    const ir_module_t **modules = core_alloc_array(const ir_module_t *,
                                                    count);
    if (valid && !modules) valid = false;
    for (size_t i = 0; valid && i < count; i++) {
        if (i >= unit_count) {
            modules[i] = analysis->specializations.data[i - unit_count]
                .instance->owner;
            continue;
        }
        core_translation_unit_t *unit = core_link_unit_translation_unit(link,
            i);
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        if (!state || !state->ir_module) {
            valid = false;
            break;
        }
        modules[i] = state->ir_module;
    }
    n00bb_abi_semantics_t semantics = {
        .environment = analysis->environment,
        .modules = modules,
        .module_count = count,
    };
    core_string_t failure_stage = {};
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
        core_buffer_printf(&message, "could not link library ABI objects%s%.*s",
            failure_stage.data ? " at " : "",
            (int)failure_stage.u8_bytes, failure_stage.data);
        if (plan_failure.reason.data)
            core_buffer_printf(&message,
                ": input %zu table %u row %u (%.*s)",
                plan_failure.input, plan_failure.table_kind,
                plan_failure.row_index, (int)plan_failure.reason.u8_bytes,
                plan_failure.reason.data);
        if (plan_failure.reason.data
            && plan_failure.table_kind == N00B_ABI_TABLE_LAYOUT)
            core_buffer_printf(&message,
                "; owner type %u points to layout %u",
                plan_failure.owner_type_index,
                plan_failure.owner_row_index);
        if (plan_failure.reason.data
            && plan_failure.table_kind == N00B_ABI_TABLE_TYPE)
            core_buffer_printf(&message,
                "; type kind %u flags %u hash %llu",
                plan_failure.type_kind, plan_failure.type_flags,
                (unsigned long long)plan_failure.typehash);
        if (plan_failure.reason.data && plan_failure.input < count) {
            core_translation_unit_t *requester =
                core_link_unit_translation_unit(link, plan_failure.input);
            if (requester && requester->module.identifier.data)
                core_buffer_printf(&message, "; requesting module %.*s",
                    (int)requester->module.identifier.u8_bytes,
                    requester->module.identifier.data);
        }
        if (plan_failure.name.bytes) {
            core_buffer_append_string(&message, plan_failure.table_kind
                    == N00B_ABI_TABLE_REQUIREMENT
                ? CORE_STRING_STATIC("; required symbol ")
                : CORE_STRING_STATIC("; current type "));
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
        if (rebase_failure.reason.data)
            core_buffer_printf(&message,
                ": table %u row %u input %zu section %u (%.*s)",
                rebase_failure.table_kind, rebase_failure.row_index,
                rebase_failure.input, rebase_failure.local_section,
                (int)rebase_failure.reason.u8_bytes,
                rebase_failure.reason.data);
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
