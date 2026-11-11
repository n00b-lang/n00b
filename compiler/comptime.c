#include "comptime.h"

#include <unistd.h>

#include "abi_emit.h"
#include "cached_body.h"
#include "compiler_cache.h"
#include "compiler_link.h"
#include "driver.h"
#include "compiler_lower.h"
#include "compiler_lower_emit.h"
#include "config.h"
#include "diagnostics.h"
#include "ir/doms.h"
#include "ir/operands.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "static_branches.h"
#include "ir/sites.h"
#include "ir/summary.h"
#include "ir/walk.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/os.h"
#include "lib/process.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t at;
} reader_t;

typedef struct {
    const uint8_t *bytes;
    size_t size;
    uint32_t alignment;
    uint64_t typehash;
    core_string_t type_name;
    uint32_t flags;
} frozen_node_t;

typedef struct {
    size_t source;
    size_t source_offset;
    size_t target;
    size_t target_offset;
} frozen_edge_t;

typedef struct {
    core_string_t identifier;
    size_t node;
} frozen_root_t;

typedef struct {
    size_t node;
    size_t offset;
    uint32_t table;
    uint32_t kind;
    uint64_t typehash;
    core_string_t type_name;
    core_string_t module;
    core_string_t function;
    core_string_t native;
    uint64_t addend;
} frozen_slot_t;

core_list_decl(frozen_node_t);
core_list_decl(frozen_edge_t);
core_list_decl(frozen_root_t);
core_list_decl(frozen_slot_t);

typedef const ir_value_t *construction_value_t;
core_list_decl(construction_value_t);
typedef ir_instr_t *construction_instr_t;
core_list_decl(construction_instr_t);

typedef struct {
    n00bb_link_analysis_t *analysis;
    char *contents;
    core_list_t(frozen_node_t) nodes;
    core_list_t(frozen_edge_t) edges;
    core_list_t(frozen_root_t) roots;
    core_list_t(frozen_slot_t) slots;
    n00bb_abi_graph_source_t source;
} frozen_graph_t;

static core_dict_t frozen_graphs;
static bool frozen_graphs_ready;

static ir_func_t *
comptime_body(ir_func_t *function, void *raw)
{
    (void)raw;
    return n00bb_cached_body(function);
}

static bool
comptime_reads(ir_func_t *function, void *raw)
{
    bool *found = raw;
    ir_func_t *body = n00bb_cached_body(function);
    for (uint32_t b = 0; body && !*found && b < body->n_blocks; b++)
        for (uint32_t i = 0; !*found && i < body->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = body->blocks[b]->instrs[i];
            *found = ir_pdg_reads_comptime(in) && !ir_annot_get(&in->annots,
                CORE_STRING_STATIC(N00BB_COMPTIME_TRANSITION_KEY));
        }
    return true;
}

static bool
has_const_initializer(ir_module_t *module)
{
    for (size_t f = 0; module && f < ir_func_count(module); f++)
        if (n00bb_const_initializer(ir_func_at(module, f))) return true;
    return false;
}

bool
n00bb_comptime_needed(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = state ? state->ir_module : nullptr;
    ir_func_t *initializer = module ? ir_func_find(module,
        CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)) : nullptr;
    if (!initializer) return false;
    ir_func_t *body = n00bb_cached_body(initializer);
    if (has_const_initializer(module)
        || (body && has_const_initializer(body->owner)))
        return true;
    if (state->link && unit == core_link_unit_root(state->link, 0)
        && ir_func_find(module, CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME)))
        return true;
    bool found = false;
    return ir_walk_reachable_functions(&initializer, 1, comptime_reads,
                                       comptime_body, &found) && found;
}

static bool
read_bytes(reader_t *reader, size_t count, const uint8_t **out)
{
    if (reader->size - reader->at < count) return false;
    *out = reader->data + reader->at;
    reader->at += count;
    return true;
}

static bool
read_word(reader_t *reader, size_t count, uint64_t *out)
{
    const uint8_t *bytes = nullptr;
    if (!read_bytes(reader, count, &bytes)) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < count; i++)
        value |= (uint64_t)bytes[i] << (8 * i);
    *out = value;
    return true;
}

static bool
read_text(reader_t *reader, core_string_t *out)
{
    uint64_t length = 0;
    const uint8_t *bytes = nullptr;
    if (!read_word(reader, 8, &length) || !read_bytes(reader, length, &bytes))
        return false;
    *out = core_string_borrow((const char *)bytes, length);
    return true;
}

static bool
read_external(frozen_graph_t *graph, reader_t *reader, uint64_t kind,
              frozen_slot_t *slot)
{
    bool mach_o = graph->analysis->target.object_format
        == CORE_TARGET_OBJECT_MACH_O;
    core_string_t name = {0};
    if (kind == 1) {
        if (!read_text(reader, &name) || !read_word(reader, 8, &slot->addend))
            return false;
    }
    else if (kind == 3) {
        core_string_t path = {0}, function = {0};
        if (!read_text(reader, &path) || !read_text(reader, &function))
            return false;
        core_translation_unit_t *owner = n00bb_unit_named(graph->analysis, path);
        core_string_t module = owner && tu_state(owner)->ir_module
            ? ir_module_name(tu_state(owner)->ir_module) : path;
        core_buffer_t *symbol = core_buffer_empty();
        core_buffer_append_string(symbol, module);
        core_buffer_putc(symbol, '.');
        core_buffer_append_string(symbol, function);
        name = core_buffer_take_string(symbol);
        slot->addend = 0;
    }
    else {
        core_string_t path = {0};
        uint64_t ordinal = 0;
        if (!read_text(reader, &path) || !read_word(reader, 4, &ordinal)
            || !read_word(reader, 8, &slot->addend)) return false;
        core_translation_unit_t *owner = n00bb_unit_named(graph->analysis, path);
        if (!owner) return false;
        name = n00bb_abi_baked_node_symbol(n00bb_package_id(owner), path,
            (size_t)ordinal, graph->analysis->target.object_format);
        if (!name.data) return false;
    }
    if (mach_o && name.u8_bytes && name.data[0] == '_')
        name = core_string_slice(name, 1, name.u8_bytes - 1);
    slot->native = name;
    return true;
}

static bool
read_graph(reader_t *reader, frozen_graph_t *graph)
{
    uint64_t count = 0;
    bool ok = read_word(reader, 8, &count);
    for (uint64_t i = 0; ok && i < count; i++) {
        uint64_t size = 0, alignment = 0, typehash = 0, flags = 0;
        frozen_node_t node = {0};
        ok = read_word(reader, 8, &size) && read_word(reader, 4, &alignment)
            && read_word(reader, 8, &typehash)
            && read_text(reader, &node.type_name)
            && read_word(reader, 4, &flags)
            && read_bytes(reader, size, &node.bytes);
        node.size = size;
        node.alignment = (uint32_t)alignment;
        node.typehash = typehash;
        node.flags = (uint32_t)flags;
        if (!node.type_name.u8_bytes) node.type_name = (core_string_t){0};
        if (ok) core_list_push(graph->nodes, node);
    }
    ok = ok && read_word(reader, 8, &count);
    for (uint64_t i = 0; ok && i < count; i++) {
        uint64_t source = 0, source_offset = 0, target = 0, target_offset = 0;
        ok = read_word(reader, 4, &source)
            && read_word(reader, 8, &source_offset)
            && read_word(reader, 4, &target)
            && read_word(reader, 8, &target_offset)
            && source < graph->nodes.len && target < graph->nodes.len;
        if (ok) core_list_push(graph->edges, ((frozen_edge_t){
            .source = source, .source_offset = source_offset,
            .target = target, .target_offset = target_offset}));
    }
    ok = ok && read_word(reader, 8, &count);
    for (uint64_t i = 0; ok && i < count; i++) {
        core_string_t module = {0}, name = {0};
        uint64_t node = 0;
        ok = read_text(reader, &module) && read_text(reader, &name)
            && read_word(reader, 4, &node) && node < graph->nodes.len;
        if (!ok) break;
        core_buffer_t *identifier = core_buffer_empty();
        core_buffer_printf(identifier, "%.*s:%.*s", (int)module.u8_bytes,
                           module.data, (int)name.u8_bytes, name.data);
        core_list_push(graph->roots, ((frozen_root_t){
            .identifier = core_buffer_take_string(identifier),
            .node = node}));
    }
    ok = ok && read_word(reader, 8, &count);
    for (uint64_t i = 0; ok && i < count; i++) {
        uint64_t node = 0, offset = 0, table = 0, kind = 0;
        frozen_slot_t slot = (frozen_slot_t){0};
        ok = read_word(reader, 4, &node) && read_word(reader, 8, &offset)
            && read_word(reader, 4, &table) && read_word(reader, 4, &kind)
            && node < graph->nodes.len;
        if (ok && table == N00B_ABI_TABLE_TYPE)
            ok = read_word(reader, 8, &slot.typehash)
                && read_text(reader, &slot.type_name);
        else if (ok && table == N00B_ABI_TABLE_NATIVE_BINDING)
            ok = read_external(graph, reader, kind, &slot);
        else if (ok)
            ok = read_text(reader, &slot.module)
                && read_text(reader, &slot.function);
        slot.node = node;
        slot.offset = offset;
        slot.table = (uint32_t)table;
        slot.kind = (uint32_t)kind;
        if (ok) core_list_push(graph->slots, slot);
    }
    return ok;
}

static bool
graph_root(void *context, core_string_t identifier, size_t *index)
{
    frozen_graph_t *graph = context;
    for (size_t i = 0; i < graph->roots.len; i++)
        if (core_string_eq(graph->roots.data[i].identifier, identifier)) {
            *index = graph->roots.data[i].node;
            return true;
        }
    return false;
}

static bool
graph_node(void *context, size_t index, const uint8_t **bytes, size_t *size,
           uint32_t *alignment, uint64_t *typehash)
{
    frozen_graph_t *graph = context;
    if (index >= graph->nodes.len) return false;
    const frozen_node_t *node = &graph->nodes.data[index];
    *bytes = node->bytes;
    *size = node->size;
    *alignment = node->alignment;
    *typehash = node->typehash;
    return true;
}

static bool
graph_edge(void *context, size_t index, size_t *source, size_t *source_offset,
           size_t *target, size_t *target_offset)
{
    frozen_graph_t *graph = context;
    if (index >= graph->edges.len) return false;
    const frozen_edge_t *edge = &graph->edges.data[index];
    *source = edge->source;
    *source_offset = edge->source_offset;
    *target = edge->target;
    *target_offset = edge->target_offset;
    return true;
}

static uint32_t
graph_flags(void *context, size_t index)
{
    frozen_graph_t *graph = context;
    return graph->nodes.data[index].flags;
}

static core_string_t
graph_type_name(void *context, size_t index)
{
    frozen_graph_t *graph = context;
    return graph->nodes.data[index].type_name;
}

static ir_func_t *
frozen_function(n00bb_link_analysis_t *analysis, core_string_t module,
                core_string_t name)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        ir_module_t *candidate = tu_state(analysis->units.data[i])->ir_module;
        if (!candidate || !core_string_eq(ir_module_name(candidate), module))
            continue;
        ir_func_t *function = ir_func_find(candidate, name);
        return function ? n00bb_cached_body(function) : nullptr;
    }
    return nullptr;
}

static bool
graph_slot(void *context, size_t index, n00bb_abi_graph_slot_t *out)
{
    frozen_graph_t *graph = context;
    if (index >= graph->slots.len) return false;
    const frozen_slot_t *slot = &graph->slots.data[index];
    if (slot->table == N00B_ABI_TABLE_NATIVE_BINDING) {
        *out = (n00bb_abi_graph_slot_t){
            .node = slot->node, .offset = slot->offset,
            .table = slot->table, .native = slot->native,
            .addend = slot->addend,
        };
        return true;
    }
    bool type = slot->table == N00B_ABI_TABLE_TYPE;
    ir_func_t *function = type ? nullptr
        : frozen_function(graph->analysis, slot->module, slot->function);
    if (!type && !function) return false;
    *out = (n00bb_abi_graph_slot_t){
        .node = slot->node,
        .offset = slot->offset,
        .table = slot->table,
        .kind = slot->kind,
        .typehash = slot->typehash,
        .type_name = slot->type_name,
        .module = type ? (core_string_t){0} : slot->module,
        .function = type ? (core_string_t){0} : slot->function,
        .function_type = function ? function->type : nullptr,
    };
    return true;
}

const n00bb_abi_graph_source_t *
n00bb_comptime_graph(core_translation_unit_t *unit)
{
    frozen_graph_t *graph = frozen_graphs_ready
        ? core_dict_get(&frozen_graphs, unit, nullptr) : nullptr;
    return graph ? &graph->source : nullptr;
}

void
n00bb_comptime_function_roots(n00bb_link_analysis_t *analysis,
                              core_list_t(n00bb_ir_func_ptr_t) *roots)
{
    if (!frozen_graphs_ready) return;
    core_dict_foreach(&frozen_graphs, item) {
        frozen_graph_t *graph = item->value;
        for (size_t i = 0; i < graph->slots.len; i++) {
            const frozen_slot_t *slot = &graph->slots.data[i];
            if (slot->table != N00B_ABI_TABLE_FUNCTION) continue;
            ir_func_t *function = frozen_function(analysis, slot->module,
                                                  slot->function);
            if (function) core_list_push(*roots, function);
        }
    }
}

static ir_global_t *
frozen_global(ir_module_t *module, core_string_t name)
{
    ir_global_t *global = ir_global_find(module, name);
    if (global) return global;
    core_buffer_t qualified = {0};
    core_buffer_append_string(&qualified, ir_module_name(module));
    core_buffer_putc(&qualified, '.');
    core_buffer_append_string(&qualified, name);
    global = ir_global_find(module, core_buffer_string(&qualified));
    core_buffer_release(&qualified);
    return global;
}


static bool
adopt_graph(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
            reader_t *reader, char *contents,
            core_list_t(core_translation_unit_ptr_t) *baked)
{
    frozen_graph_t *graph = core_alloc(frozen_graph_t);
    graph->analysis = analysis;
    graph->contents = contents;
    graph->nodes = core_list_new(frozen_node_t);
    graph->edges = core_list_new(frozen_edge_t);
    graph->roots = core_list_new(frozen_root_t);
    graph->slots = core_list_new(frozen_slot_t);
    if (!read_graph(reader, graph)) return false;
    graph->source = (n00bb_abi_graph_source_t){
        .context = graph,
        .node_count = graph->nodes.len,
        .edge_count = graph->edges.len,
        .slot_count = graph->slots.len,
        .root = graph_root, .node = graph_node, .edge = graph_edge,
        .flags = graph_flags, .type_name = graph_type_name,
        .slot = graph_slot,
    };
    if (!frozen_graphs_ready) {
        core_dict_init(&frozen_graphs, nullptr, nullptr);
        frozen_graphs_ready = true;
    }
    core_dict_put(&frozen_graphs, unit, graph);
    ir_module_t *module = tu_state(unit)->ir_module;
    bool changed = false;
    for (size_t i = 0; i < graph->roots.len; i++) {
        core_string_t identifier = graph->roots.data[i].identifier;
        int64_t colon = core_string_rfind_byte(identifier, ':');
        ir_global_t *global = colon < 0 ? nullptr : frozen_global(module,
            core_string_slice(identifier, (size_t)colon + 1,
                              identifier.u8_bytes - (size_t)colon - 1));
        ir_global_init_value_t value = {
            .kind = IR_GLOBAL_INIT_PHASE_RESULT,
            .phase_result.identifier = identifier,
        };
        if (!global) continue;
        if (!ir_global_replace_static_initializer(module, global, &value))
            return false;
        changed = true;
    }
    if (changed) core_list_push(*baked, unit);
    return true;
}

static bool
adopt_state(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
            reader_t *reader, char *contents, core_diagnostics_t *diagnostics,
            core_list_t(core_translation_unit_ptr_t) *baked)
{
    uint64_t sections = 0, changes = 0;
    bool ok = read_word(reader, 4, &sections);
    core_list_t(core_translation_unit_ptr_t) kept =
        core_list_new(core_translation_unit_ptr_t);
    for (uint64_t i = 0; ok && i < sections; i++) {
        core_string_t path = {0};
        ok = read_text(reader, &path);
        core_translation_unit_t *owner = ok ? n00bb_unit_named(analysis, path)
                                            : nullptr;
        bool bakeable = owner && (owner == unit
                                  || !tu_state(owner)->interface_module);
        frozen_graph_t discarded = {
            .nodes = core_list_new(frozen_node_t),
            .edges = core_list_new(frozen_edge_t),
            .roots = core_list_new(frozen_root_t),
            .slots = core_list_new(frozen_slot_t),
        };
        ok = ok && (bakeable
            ? adopt_graph(analysis, owner, reader, contents, baked)
            : read_graph(reader, &discarded));
        if (ok && bakeable) core_list_push(kept, owner);
        for (size_t r = 0; r < discarded.roots.len; r++)
            core_free(discarded.roots.data[r].identifier.data);
        core_list_free(discarded.nodes);
        core_list_free(discarded.edges);
        core_list_free(discarded.roots);
        core_list_free(discarded.slots);
    }
    ok = ok && read_word(reader, 8, &changes);
    core_string_t name = ir_module_name(tu_state(unit)->ir_module);
    bool starts = ir_func_find(tu_state(unit)->ir_module,
        CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME)) != nullptr;
    for (uint64_t i = 0; ok && i < changes; i++) {
        core_string_t path = {0}, global = {0};
        ok = read_text(reader, &path) && read_text(reader, &global);
        core_translation_unit_t *owner = ok ? n00bb_unit_named(analysis, path)
                                            : nullptr;
        bool frozen = false;
        for (size_t k = 0; owner && k < kept.len; k++)
            frozen |= kept.data[k] == owner;
        bool started = ok && starts && owner != unit;
        bool refreezes = started
            && n00bb_cache_has_module(analysis->cache, path);
        if (started) n00bb_cache_skip_store(analysis->cache, path);
        if (refreezes) n00bb_cache_note_refreeze(analysis->cache, path);
        if (ok && !frozen && !refreezes && n00bb_report(diagnostics, unit,
                N00B_DIAG_COMPTIME_IMPORT_CHANGE_DROPPED,
                (core_diag_location_t){0},
                N00B_TEXT_COMPTIME_IMPORT_CHANGE_DROPPED,
                N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(global),
                           ARG_TEXT(path))))
            n00bb_report_help(diagnostics,
                N00B_TEXT_MAKE_THE_CHANGE_IN_COMPTIME_START, N00BB_NO_ARGS);
    }
    core_list_free(kept);
    return ok && reader->at == reader->size;
}

#define comptime_failed(unit, id, text, args)                               \
    (n00bb_report(diagnostics, unit, id, (core_diag_location_t){0}, text,   \
                  args), false)

static bool
magic_is(const uint8_t *magic, core_string_t expected)
{
    return core_string_eq(core_string_borrow((const char *)magic, 8),
                          expected);
}

bool
n00bb_comptime_run(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit, const char *program,
                   bool entry, const char *frozen,
                   core_diagnostics_t *diagnostics,
                   core_list_t(core_translation_unit_ptr_t) *baked,
                   bool *exited)
{
    n00bb_tu_state_t *state = tu_state(unit);
    ir_module_t *module = state->ir_module;
    core_string_t name = ir_module_name(module);
    core_buffer_t key = {0};
    core_buffer_printf(&key, "%llx",
        (unsigned long long)n00bb_comptime_module_key(name));
    core_string_t root = core_os_temp_root();
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, root);
    core_buffer_puts(&stem, "/n00bb-comptime-");
    core_free(root.data);
    core_string_t made = {0};
    int fd = core_os_make_unique_file(core_buffer_string(&stem), &made);
    core_buffer_release(&stem);
    if (fd >= 0) close(fd);
    char *output = fd >= 0 ? core_string_to_cstr(made) : nullptr;
    bool keeps = entry && ir_func_find(module,
        CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME));
    char *arguments[] = {(char *)program, key.data, output,
                         keeps ? "start" : entry ? "entry" : "module",
                         (char *)frozen, nullptr};
    core_process_t child = {0};
    core_process_status_t status = {0};
    bool timed_out = false;
    int64_t seconds = n00bb_setting_int(N00BB_KEY_BUILD_COMPTIME_TIMEOUT);
    bool ran = output && !core_process_start(
            &(core_process_spec_t){.arguments = arguments}, &child)
        && core_process_wait_timeout(child,
               seconds > 0 ? (uint64_t)seconds * 1000 : 0, &status,
               &timed_out);
    char *contents = nullptr;
    size_t size = 0;
    bool read = ran && !timed_out && status.exited && !status.code
        && core_os_read_file(made, &contents, &size);
    if (made.data) core_os_remove_file(made);
    core_free(made.data);
    core_free(output);
    core_buffer_release(&key);
    reader_t reader = {.data = (const uint8_t *)contents, .size = size};
    const uint8_t *magic = nullptr;
    bool ok = false;
    bool adopted = false;
    if (!ran)
        ok = comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
            N00B_TEXT_COMPTIME_RUN_NOT_STARTED, N00BB_ARGS(ARG_TEXT(name)));
    else if (timed_out)
        ok = comptime_failed(unit, N00B_DIAG_COMPTIME_TIMEOUT,
            N00B_TEXT_COMPTIME_RUN_TIMED_OUT,
            N00BB_ARGS(ARG_TEXT(name), ARG_NUMBER(seconds)));
    else if (!status.exited)
        ok = comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
            N00B_TEXT_COMPTIME_RUN_SIGNALED,
            N00BB_ARGS(ARG_TEXT(name), ARG_NUMBER(status.code),
                       ARG_NATIVE(core_process_signal_name(status.code))));
    else if (status.code)
        ok = comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
            N00B_TEXT_COMPTIME_RUN_EXITED,
            N00BB_ARGS(ARG_TEXT(name), ARG_NUMBER(status.code)));
    else if (!read || !read_bytes(&reader, 8, &magic))
        ok = comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
            N00B_TEXT_COMPTIME_RUN_NO_STATE, N00BB_ARGS(ARG_TEXT(name)));
    else if (magic_is(magic, CORE_STRING_STATIC("N00BCTEX"))) {
        uint64_t code = 0;
        (void)read_word(&reader, 8, &code);
        ok = comptime_failed(unit, entry ? N00B_DIAG_COMPTIME_RUN_FAILED
                                         : N00B_DIAG_COMPTIME_EARLY_EXIT,
            entry ? N00B_TEXT_COMPTIME_RUN_EXITED
                  : N00B_TEXT_COMPTIME_MODULE_EXITED,
            N00BB_ARGS(ARG_TEXT(name), ARG_NUMBER((int64_t)code)));
    }
    else if (magic_is(magic, CORE_STRING_STATIC("N00BCTFL"))) {
        core_string_t path = {0}, global = {0}, message = {0};
        ok = read_text(&reader, &path) && read_text(&reader, &global)
                && read_text(&reader, &message)
            ? comptime_failed(unit, N00B_DIAG_COMPTIME_NOT_FREEZABLE,
                  N00B_TEXT_COMPTIME_NOT_FREEZABLE,
                  N00BB_ARGS(ARG_TEXT(global), ARG_TEXT(path),
                             ARG_TEXT(message)))
            : comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
                  N00B_TEXT_COMPTIME_RUN_NO_STATE,
                  N00BB_ARGS(ARG_TEXT(name)));
    }
    else if (magic_is(magic, CORE_STRING_STATIC("N00BCT03"))
             || magic_is(magic, CORE_STRING_STATIC("N00BCTX3"))) {
        *exited = magic_is(magic, CORE_STRING_STATIC("N00BCTX3"));
        ok = adopt_state(analysis, unit, &reader, contents, diagnostics,
                         baked);
        adopted = ok;
        if (!ok) ok = comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
            N00B_TEXT_COMPTIME_RUN_NO_STATE, N00BB_ARGS(ARG_TEXT(name)));
    }
    else
        ok = comptime_failed(unit, N00B_DIAG_COMPTIME_RUN_FAILED,
            N00B_TEXT_COMPTIME_RUN_NO_STATE, N00BB_ARGS(ARG_TEXT(name)));
    if (!adopted) core_free(contents);
    return ok;
}

typedef struct {
    core_dict_t counts;
    bool opaque;
} touches_t;

static void
touch(touches_t *touches, const ir_global_t *global, uintptr_t weight)
{
    if (!global) {
        touches->opaque = true;
        return;
    }
    ir_global_t *identity = ir_global_identity(global);
    uintptr_t count = (uintptr_t)core_dict_get(&touches->counts, identity,
                                               nullptr);
    core_dict_put(&touches->counts, identity, count + weight);
}

static void
touch_summary(void *context, const ir_func_t *target,
              const ir_summary_t *summary)
{
    (void)target;
    touches_t *touches = context;
    if (!summary || ir_summary_effect_bits(summary)
            & (IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_CALLS_CALLBACKS)) {
        touches->opaque = true;
        return;
    }
    uint32_t count = ir_summary_path_count(summary);
    for (uint32_t i = 0; i < count; i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if (path->root == IR_SPLACE_UNKNOWN) touches->opaque = true;
        else if (path->root == IR_SPLACE_GLOBAL
                 || path->root == IR_SPLACE_THREAD)
            touch(touches, path->global, 2);
    }
}

static bool
calls_native(const ir_instr_t *call, core_string_t name)
{
    const ir_func_t *target = ir_call_function(call);
    return target && core_string_eq(target->name, name);
}

static bool
comptime_only_call(const ir_instr_t *call)
{
    return calls_native(call, CORE_STRING_STATIC(N00BB_COMPTIME_FREEZE_NATIVE))
        || calls_native(call, CORE_STRING_STATIC(N00BB_COMPTIME_START_NAME))
        || ir_pdg_asks_frozen(call);
}

static void
touch_function(ir_summaries_t *summaries, ir_func_t *function,
               touches_t *touches)
{
    for (uint32_t b = 0; function && b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            for (uint32_t k = 0; k < in->n_operands; k++) {
                const ir_value_t *value =
                    ir_operand_referenced_value(&in->operands[k]);
                if (value && value->kind == IR_VAL_GLOBAL)
                    touch(touches, value->global, 1);
            }
            if (in->op == IR_OP_CALLBACK_INVOKE) touches->opaque = true;
            if (in->op == IR_OP_CALL && !comptime_only_call(in)
                && !ir_summary_call_callees(summaries, in, touch_summary,
                                            touches))
                touches->opaque = true;
        }
}

static bool
const_global(const ir_global_t *global)
{
    return global && ir_global_identity(global)->mutability == IR_MUT_CONST;
}

static void
reads_constants(void *context, const ir_func_t *target,
                const ir_summary_t *summary)
{
    (void)target;
    bool *pure = context;
    uint32_t count = summary ? ir_summary_path_count(summary) : 0;
    if (!summary) *pure = false;
    for (uint32_t i = 0; *pure && i < count; i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        *pure = path->root != IR_SPLACE_UNKNOWN
            && path->root != IR_SPLACE_THREAD
            && (path->root != IR_SPLACE_GLOBAL || const_global(path->global));
    }
}

static bool
pure_instruction(ir_summaries_t *summaries, const ir_instr_t *in)
{
    if (in->dest_kind != IR_DEST_VALUE || in->op == IR_OP_PHI
        || ir_pdg_reads_comptime(in) || (in->op == IR_OP_LOAD && in->n_mods))
        return false;
    if (in->op != IR_OP_CALL) {
        const ir_opinfo_t *info = ir_op_info(in->op);
        return info && !(info->flags & IR_OPF_SIDEEFFECT);
    }
    bool pure = true;
    return ir_summary_call_pure(summaries, in)
        && ir_summary_call_callees(summaries, in, reads_constants, &pure)
        && pure;
}

typedef struct {
    ir_summaries_t   *summaries;
    const ir_instr_t *root;
    core_dict_t       seen;
    core_dict_t       objects;
    core_list_t(construction_value_t) pending;
} purity_t;

static bool pure_value(purity_t *purity, const ir_value_t *value);

typedef struct {
    bool     pure;
    uint32_t object;
} writer_check_t;

static void
writes_only_object(void *context, const ir_func_t *target,
                   const ir_summary_t *summary)
{
    (void)target;
    const uint32_t effects = IR_SUMMARY_MAY_FAIL | IR_SUMMARY_MAY_BLOCK
        | IR_SUMMARY_DIVERGES | IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_MAY_IO
        | IR_SUMMARY_CALLS_FINALIZERS | IR_SUMMARY_STARTS_THREADS
        | IR_SUMMARY_OBSERVES;
    writer_check_t *check = context;
    check->pure = check->pure && summary
        && !(ir_summary_flags(summary) & effects);
    for (uint32_t i = 0; check->pure && i < ir_summary_path_count(summary);
         i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        bool writes = path->flags & IR_SPATH_WRITE;
        check->pure = path->root == IR_SPLACE_HEAP
            || (path->root == IR_SPLACE_PARAM
                && (!writes || path->param == check->object))
            || (path->root == IR_SPLACE_GLOBAL && !writes
                && const_global(path->global));
    }
}

static bool
object_writer(purity_t *purity, const ir_instr_t *call,
              const ir_value_t *object)
{
    if (call->op != IR_OP_CALL || call->n_operands < 2
        || ir_operand_value(&call->operands[1]) != object
        || ir_pdg_reads_comptime(call))
        return false;
    for (uint32_t k = 2; k < call->n_operands; k++) {
        const ir_value_t *argument =
            ir_operand_referenced_value(&call->operands[k]);
        if (argument == object || !pure_value(purity, argument)) return false;
    }
    writer_check_t check = {.pure = true, .object = 0};
    return ir_summary_call_callees(purity->summaries, call,
                                   writes_only_object, &check)
        && check.pure;
}

static void
note_object(purity_t *purity, const ir_value_t *value)
{
    if (!core_dict_contains(&purity->objects, (void *)value)) {
        core_dict_put(&purity->objects, (void *)value, 1);
        core_list_push(purity->pending, value);
    }
}

static bool
object_use(ir_instr_t *in, uint32_t operand, void *state)
{
    purity_t *purity = state;
    const ir_value_t *object = ir_operand_value(&in->operands[operand]);
    bool alias = in->op == IR_OP_CAST || in->op == IR_OP_NOMINAL_CAST
        || in->op == IR_OP_COPY || in->op == IR_OP_LOAD;
    if (in == purity->root
        || (!alias && core_dict_contains(&purity->seen, in)))
        return true;
    switch (in->op) {
    case IR_OP_CAST:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_COPY:
        if (in->dest_kind != IR_DEST_VALUE || !in->dest) return false;
        note_object(purity, in->dest);
        break;
    case IR_OP_LOAD:
        if (operand != 0 || in->n_mods || !in->dest) return false;
        note_object(purity, in->dest);
        break;
    case IR_OP_STORE: {
        if (in->n_operands != 2) return false;
        const ir_value_t *place = ir_operand_value(&in->operands[0]);
        const ir_value_t *stored = ir_operand_value(&in->operands[1]);
        if (operand == 1) {
            if (!place || !place->def || place->def->op != IR_OP_ALLOC)
                return false;
            core_dict_put(&purity->seen, place->def, 1);
            note_object(purity, place);
        }
        else if (!core_dict_contains(&purity->objects, (void *)stored)
                 && !pure_value(purity, stored))
            return false;
        break;
    }
    case IR_OP_CALL:
        if (!object_writer(purity, in, object)) return false;
        break;
    default:
        return false;
    }
    core_dict_put(&purity->seen, in, 1);
    return true;
}

static bool
fresh_object(purity_t *purity, const ir_instr_t *allocation)
{
    ir_func_t *function = allocation->block ? allocation->block->owner
                                            : nullptr;
    if (!function || allocation->n_mods) return false;
    note_object(purity, allocation->dest);
    while (purity->pending.len) {
        const ir_value_t *object =
            purity->pending.data[--purity->pending.len];
        if (!ir_value_visit_uses(function, object, object_use, purity))
            return false;
    }
    return true;
}

static bool
pure_value(purity_t *purity, const ir_value_t *value)
{
    if (!value || value->kind == IR_VAL_FUNC) return true;
    if (value->kind == IR_VAL_GLOBAL) return const_global(value->global);
    if (value->kind != IR_VAL_INSTR || !value->def) return false;
    if (core_dict_contains(&purity->seen, value->def)) return true;
    core_dict_put(&purity->seen, value->def, 1);
    const ir_instr_t *in = value->def;
    if (in->op == IR_OP_ALLOC && in->dest_kind == IR_DEST_VALUE)
        return fresh_object(purity, in);
    if (!pure_instruction(purity->summaries, in)) return false;
    for (uint32_t k = 0; k < in->n_operands; k++)
        if (!pure_value(purity,
                        ir_operand_referenced_value(&in->operands[k])))
            return false;
    return true;
}

static bool
always_runs(ir_func_t *function, ir_block_t *target, bool comptime)
{
    ir_block_t *entry = ir_block_entry(function);
    if (!entry || entry == target) return entry != nullptr;
    bool *seen = core_alloc_array(bool, function->n_blocks);
    ir_block_t **stack = core_alloc_array(ir_block_t *, function->n_blocks);
    size_t depth = 0;
    stack[depth++] = entry;
    seen[entry->index] = true;
    bool escapes = false;
    while (depth && !escapes) {
        ir_instr_t *term = ir_block_terminator(stack[--depth]);
        escapes = !term || term->op == IR_OP_RETURN;
        uint32_t taken = term ? ir_phase_successor(term, comptime)
                              : UINT32_MAX;
        bool fixed = taken != UINT32_MAX;
        uint32_t count = term ? ir_succ_count(term) : 0;
        for (uint32_t s = 0; !escapes && s < count; s++) {
            ir_block_t *next = ir_succ_at(term, s);
            if ((fixed && s != taken) || !next || next == target
                || seen[next->index]) continue;
            seen[next->index] = true;
            stack[depth++] = next;
        }
    }
    core_free(stack);
    core_free(seen);
    return !escapes;
}

bool
n00bb_comptime_seeds(core_translation_unit_t *unit, ir_global_t *global)
{
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    ir_module_t *module = state ? state->ir_module : nullptr;
    ir_func_t *initializer = module ? n00bb_cached_body(ir_func_find(module,
        CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME))) : nullptr;
    ir_global_t *identity = global ? ir_global_identity(global) : nullptr;
    if (!initializer || !identity || !n00bb_comptime_needed(unit))
        return false;
    for (uint32_t b = 0; b < initializer->n_blocks; b++) {
        ir_block_t *block = initializer->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];
            const ir_value_t *target = in->op == IR_OP_GLOBAL_STORE
                    && in->n_operands == 2
                ? ir_operand_value(&in->operands[0]) : nullptr;
            if (target && target->kind == IR_VAL_GLOBAL && target->global
                && ir_global_identity(target->global) == identity
                && always_runs(initializer, block, true))
                return true;
        }
    }
    return false;
}

static void
remove_dead(ir_summaries_t *summaries, ir_func_t *function,
            const ir_value_t *value)
{
    if (!value || value->kind != IR_VAL_INSTR || !value->def
        || ir_value_has_uses(function, value)
        || !pure_instruction(summaries, value->def)) return;
    ir_instr_t *in = value->def;
    uint32_t count = in->n_operands;
    const ir_value_t **operands = core_alloc_array(const ir_value_t *,
                                                   count ? count : 1);
    for (uint32_t k = 0; k < count; k++)
        operands[k] = ir_operand_referenced_value(&in->operands[k]);
    if (ir_instr_erase(in))
        for (uint32_t k = 0; k < count; k++)
            remove_dead(summaries, function, operands[k]);
    core_free(operands);
}

static void
remove_construction(ir_func_t *function, purity_t *purity)
{
    core_list_t(construction_instr_t) members =
        core_list_new(construction_instr_t);
    core_dict_foreach(&purity->seen, item)
        core_list_push(members, (ir_instr_t *)item->key);
    for (bool removed = true; removed;) {
        removed = false;
        for (size_t i = 0; i < members.len; i++) {
            ir_instr_t *in = members.data[i];
            if (!in || (in->dest_kind == IR_DEST_VALUE && in->dest
                        && ir_value_has_uses(function, in->dest)))
                continue;
            if (ir_instr_erase(in)) {
                members.data[i] = nullptr;
                removed = true;
            }
        }
    }
    core_list_free(members);
}

static bool
prunable_store(purity_t *purity, ir_module_t *module,
               ir_func_t *function, ir_instr_t *in, touches_t *touches)
{
    if (in->op != IR_OP_GLOBAL_STORE || in->n_operands != 2
        || !ir_instr_is_binding_initialization(in)) return false;
    const ir_value_t *target = ir_operand_value(&in->operands[0]);
    ir_global_t *global = target && target->kind == IR_VAL_GLOBAL
        ? target->global : nullptr;
    ir_global_t *identity = global ? ir_global_identity(global) : nullptr;
    if (!identity || identity->owner != module
        || !identity->has_static_initializer
        || identity->static_initializer->kind != IR_GLOBAL_INIT_PHASE_RESULT
        || ir_global_storage_duration(identity) == IR_STORAGE_DURATION_THREAD
        || (uintptr_t)core_dict_get(&touches->counts, identity, nullptr) != 1
        || !always_runs(function, in->block, true)
        || !always_runs(function, in->block, false)) return false;
    purity->root = in;
    return pure_value(purity, ir_operand_value(&in->operands[1]));
}

static bool
fold_runtime_branches(ir_func_t *function)
{
    bool folded = false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_instr_t *term = ir_block_terminator(function->blocks[b]);
        uint32_t taken = term && term->op == IR_OP_BRANCH
            ? ir_phase_successor(term, false) : UINT32_MAX;
        ir_block_t *next = taken != UINT32_MAX ? ir_succ_at(term, taken)
                                               : nullptr;
        if (!next) continue;
        term->op = IR_OP_JUMP;
        term->n_operands = 0;
        folded = ir_instr_operand_block(term, next) || folded;
    }
    return folded;
}

static bool
function_referenced(ir_module_t *module, const ir_func_t *target)
{
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = function->blocks[b]->instrs[i];
                for (uint32_t o = 0; o < in->n_operands; o++) {
                    const ir_value_t *value = ir_operand_value(
                        &in->operands[o]);
                    if (value && value->kind == IR_VAL_FUNC
                        && value->owner == target) return true;
                }
            }
    }
    return false;
}

static void
empty_function(ir_func_t *function)
{
    ir_block_t *entry = ir_block_entry(function);
    if (!entry) return;
    for (uint32_t b = function->n_blocks; b-- > 0;) {
        ir_block_t *block = function->blocks[b];
        while (block->n_instrs
               && ir_instr_erase(block->instrs[block->n_instrs - 1])) {}
        if (block != entry) (void)ir_block_erase(block);
    }
    (void)ir_instr_add(entry, IR_OP_RETURN);
}

static void
prune_module(ir_summaries_t *summaries, ir_module_t *module,
             touches_t *touches)
{
    ir_func_t *initializer = ir_func_find(module,
        CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME));
    for (uint32_t b = 0; !touches->opaque && initializer
         && b < initializer->n_blocks; b++) {
        ir_block_t *block = initializer->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs;) {
            ir_instr_t *in = block->instrs[i];
            purity_t purity = {.summaries = summaries,
                               .pending = core_list_new(construction_value_t)};
            core_dict_init(&purity.seen, nullptr, nullptr);
            core_dict_init(&purity.objects, nullptr, nullptr);
            bool pruned = prunable_store(&purity, module, initializer, in,
                                         touches);
            core_string_t name = pruned
                ? ir_operand_value(&in->operands[0])->global->name
                : (core_string_t){0};
            pruned = pruned && ir_instr_erase(in);
            if (pruned) {
                if (n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE))
                    fprintf(stderr, "PROFILE comptime-pruned %.*s\n",
                            (int)name.u8_bytes, name.data);
                remove_construction(initializer, &purity);
            }
            core_dict_free(&purity.seen);
            core_dict_free(&purity.objects);
            core_list_free(purity.pending);
            i = pruned ? 0 : i + 1;
        }
    }
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *function = ir_func_at(module, f);
        if (fold_runtime_branches(function))
            n00bb_drop_unreachable_blocks(function);
        for (uint32_t b = 0; b < function->n_blocks; b++)
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];
                if (!ir_pdg_reads_comptime(in)) continue;
                in->op = IR_OP_CONST;
                in->n_operands = 0;
                (void)ir_instr_operand_int(in, 0);
            }
    }
    for (size_t f = 0; f < ir_func_count(module); f++) {
        ir_func_t *constants = ir_func_at(module, f);
        if (n00bb_const_initializer(constants) && constants->n_blocks
            && !function_referenced(module, constants))
            empty_function(constants);
    }
}

void
n00bb_comptime_prune(n00bb_link_analysis_t *analysis,
                     core_translation_unit_t *const *units, size_t count)
{
    touches_t touches = {0};
    core_dict_init(&touches.counts, nullptr, nullptr);
    for (size_t i = 0; i < analysis->initializer_order.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->initializer_order.data[i]);
        if (!state || !state->ir_module || state->interface_module) continue;
        touch_function(analysis->summaries, ir_func_find(state->ir_module,
            CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)), &touches);
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++)
            if (n00bb_const_initializer(ir_func_at(state->ir_module, f)))
                touch_function(analysis->summaries,
                               ir_func_at(state->ir_module, f), &touches);
    }
    for (size_t i = 0; i < count; i++)
        prune_module(analysis->summaries, tu_state(units[i])->ir_module,
                     &touches);
    core_dict_free(&touches.counts);
}

void
n00bb_comptime_drop_declarations(n00bb_link_analysis_t *analysis)
{
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_func_t *initializer = state && state->ir_module
                && !state->interface_module && !n00bb_comptime_needed(unit)
            ? ir_func_find(state->ir_module,
                           CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME))
            : nullptr;
        for (uint32_t b = 0; initializer && b < initializer->n_blocks; b++) {
            ir_block_t *block = initializer->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs;) {
                ir_instr_t *in = block->instrs[i];
                if (in->n_operands != 2
                    || !ir_annot_get(&in->annots, N00BB_DECLARATION_KEY)) {
                    i++;
                    continue;
                }
                const ir_value_t *place = ir_operand_value(&in->operands[0]);
                const ir_value_t *value = ir_operand_value(&in->operands[1]);
                if (!ir_instr_erase(in)) {
                    i++;
                    continue;
                }
                remove_dead(nullptr, initializer, value);
                remove_dead(nullptr, initializer, place);
                i = 0;
            }
        }
    }
}
