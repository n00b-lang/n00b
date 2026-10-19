#include "lib/module.h"

#include "lib/alloc.h"
#include "lib/diagnostic.h"
#include "lib/dict.h"
#include "lib/list.h"

typedef core_translation_unit_t *core_tu_ptr_t;
core_list_decl(core_tu_ptr_t);

typedef struct {
    core_list_t(core_tu_ptr_t) *units;
    core_dict_t *unit_index;
    int *indices;
    int *lowlinks;
    size_t *stack;
    bool *on_stack;
    size_t stack_len;
    int next_index;
    core_translation_unit_scc_fn callback;
    void *context;
    bool succeeded;
} core_tu_scc_ctx_t;

static void
core_tu_collect_imports(core_list_t(core_tu_ptr_t) *units,
                        core_dict_t *unit_index,
                        core_translation_unit_t *unit)
{
    if (!unit || core_dict_get(unit_index, unit, nullptr)) return;
    core_dict_add(unit_index, unit, units->len + 1);
    core_list_push(*units, unit);
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
        core_tu_collect_imports(units, unit_index,
            core_translation_unit_import(unit, i));
}

static size_t
core_tu_index(core_tu_scc_ctx_t *ctx, core_translation_unit_t *unit)
{
    size_t index = (size_t)(uintptr_t)core_dict_get(ctx->unit_index, unit,
                                                     nullptr);
    return index ? index - 1 : SIZE_MAX;
}

static void
core_tu_scc_visit(core_tu_scc_ctx_t *ctx, size_t at)
{
    if (!ctx->succeeded) return;
    ctx->indices[at] = ctx->lowlinks[at] = ctx->next_index++;
    ctx->stack[ctx->stack_len++] = at;
    ctx->on_stack[at] = true;
    core_translation_unit_t *unit = ctx->units->data[at];
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        size_t next = core_tu_index(ctx,
            core_translation_unit_import(unit, i));
        if (next == SIZE_MAX) continue;
        if (ctx->indices[next] < 0) {
            core_tu_scc_visit(ctx, next);
            if (ctx->lowlinks[next] < ctx->lowlinks[at])
                ctx->lowlinks[at] = ctx->lowlinks[next];
        }
        else if (ctx->on_stack[next]
                 && ctx->indices[next] < ctx->lowlinks[at]) {
            ctx->lowlinks[at] = ctx->indices[next];
        }
        if (!ctx->succeeded) return;
    }
    if (ctx->lowlinks[at] != ctx->indices[at]) return;

    core_list_t(core_tu_ptr_t) members = core_list_new(core_tu_ptr_t);
    size_t member;
    do {
        member = ctx->stack[--ctx->stack_len];
        ctx->on_stack[member] = false;
        core_list_push(members, ctx->units->data[member]);
    } while (member != at);
    ctx->succeeded = ctx->callback(ctx->context, members.data,
                                   members.len);
    core_list_free(members);
}

bool
core_module_spec_init(core_module_spec_t *spec, core_string_t identifier)
{
    if (!spec || !identifier.data || !identifier.u8_bytes) return false;
    *spec = (core_module_spec_t){.identifier = core_string_dup(identifier)};
    return true;
}

void
core_module_spec_release(core_module_spec_t *spec)
{
    if (!spec) return;
    if (spec->user_data_free) spec->user_data_free(spec->user_data);
    core_free(spec->identifier.data);
    *spec = (core_module_spec_t){0};
}

void
core_module_spec_set_user_data(core_module_spec_t *spec, void *data,
                               core_module_user_data_free_fn free_fn)
{
    if (!spec) return;
    if (spec->user_data != data && spec->user_data_free)
        spec->user_data_free(spec->user_data);
    spec->user_data = data;
    spec->user_data_free = free_fn;
}

void *
core_module_spec_user_data(const core_module_spec_t *spec)
{
    return spec ? spec->user_data : nullptr;
}

bool
core_module_spec_eq(const core_module_spec_t *a, const core_module_spec_t *b)
{
    return a && b && core_string_eq(a->identifier, b->identifier);
}

bool
core_source_file_init(core_source_file_t *file, core_string_t path,
                      core_string_t contents)
{
    if (!file || !path.data || (!contents.data && contents.u8_bytes))
        return false;
    *file = (core_source_file_t){
        .path = core_string_dup(path),
        .contents = core_buffer_from_bytes(
            contents.data ? contents.data : "", (int64_t)contents.u8_bytes),
    };
    return true;
}

void
core_source_file_release(core_source_file_t *file)
{
    if (!file) return;
    if (file->user_data_free) file->user_data_free(file->user_data);
    core_free(file->path.data);
    core_buffer_free(file->contents);
    *file = (core_source_file_t){0};
}

void
core_source_file_set_user_data(core_source_file_t *file, void *data,
                               core_module_user_data_free_fn free_fn)
{
    if (!file) return;
    if (file->user_data != data && file->user_data_free)
        file->user_data_free(file->user_data);
    file->user_data = data;
    file->user_data_free = free_fn;
}

void *
core_source_file_user_data(const core_source_file_t *file)
{
    return file ? file->user_data : nullptr;
}

core_string_t
core_source_file_path(const core_source_file_t *file)
{
    return file ? file->path : (core_string_t){0};
}

core_string_t
core_source_file_contents(const core_source_file_t *file)
{
    return file && file->contents ? core_buffer_string(file->contents)
                                  : (core_string_t){};
}

size_t
core_source_file_size(const core_source_file_t *file)
{
    return file && file->contents ? file->contents->byte_len : 0;
}

bool
core_translation_unit_init(core_translation_unit_t *unit,
                           const core_module_spec_t *module,
                           core_source_file_t *source)
{
    if (!unit || !module || !module->identifier.data || !source
        || !source->contents || !source->path.data)
        return false;

    *unit = (core_translation_unit_t){
        .module = {.identifier = core_string_dup(module->identifier)},
        .source = *source,
        .diagnostics = core_diagnostics_new(),
    };
    core_dict_init(&unit->imports_by_unit, nullptr, nullptr);
    core_dict_init_string(&unit->imports_by_module);
    core_diagnostics_set_unit(unit->diagnostics, unit);
    *source = (core_source_file_t){0};
    return true;
}

void
core_translation_unit_release(core_translation_unit_t *unit)
{
    if (!unit) return;
    if (unit->user_data_free) unit->user_data_free(unit->user_data);
    core_diagnostics_free(unit->diagnostics);
    core_dict_free(&unit->imports_by_unit);
    core_dict_free(&unit->imports_by_module);
    core_free(unit->imports);
    core_module_spec_release(&unit->module);
    core_source_file_release(&unit->source);
    *unit = (core_translation_unit_t){0};
}

void
core_translation_unit_set_user_data(core_translation_unit_t *unit, void *data,
                                    core_module_user_data_free_fn free_fn)
{
    if (!unit) return;
    if (unit->user_data != data && unit->user_data_free)
        unit->user_data_free(unit->user_data);
    unit->user_data = data;
    unit->user_data_free = free_fn;
}

void *
core_translation_unit_user_data(const core_translation_unit_t *unit)
{
    return unit ? unit->user_data : nullptr;
}

core_diagnostics_t *
core_translation_unit_diagnostics(core_translation_unit_t *unit)
{
    return unit ? unit->diagnostics : nullptr;
}

bool
core_translation_unit_has_fatal_diagnostics(const core_translation_unit_t *unit)
{
    return unit && core_diagnostics_has_fatal(unit->diagnostics);
}

bool
core_translation_unit_add_import(core_translation_unit_t *unit,
                                 core_translation_unit_t *imported)
{
    if (!unit || !imported) return false;
    if (unit->imports_finalized) {
        core_diagnostics_report(unit->diagnostics, unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("LANTERN-L0004"),
            CORE_STRING_STATIC("cannot add an import after translation unit imports are finalized"));
        return false;
    }
    if (core_dict_contains(&unit->imports_by_unit, imported)) return true;
    if (core_dict_contains(&unit->imports_by_module,
                           &imported->module.identifier)) {
        core_diagnostics_report(unit->diagnostics, unit, CORE_DIAG_FATAL,
            CORE_STRING_STATIC("LANTERN-L0001"),
            CORE_STRING_STATIC("translation unit imports distinct sources with the same module identity"));
        return false;
    }
    if (unit->import_count == unit->import_capacity) {
        size_t capacity = unit->import_capacity ? unit->import_capacity * 2 : 4;
        unit->imports = core_realloc(unit->imports,
                                     sizeof(*unit->imports) * capacity);
        unit->import_capacity = capacity;
    }
    unit->imports[unit->import_count++] = imported;
    core_dict_add(&unit->imports_by_unit, imported, imported);
    core_dict_add(&unit->imports_by_module,
                  &imported->module.identifier, imported);
    unit->import_revision++;
    return true;
}

size_t
core_translation_unit_import_count(const core_translation_unit_t *unit)
{
    return unit ? unit->import_count : 0;
}

core_translation_unit_t *
core_translation_unit_import(const core_translation_unit_t *unit,
                             size_t index)
{
    return unit && index < unit->import_count ? unit->imports[index] : nullptr;
}

core_translation_unit_t *
core_translation_unit_find_import(const core_translation_unit_t *unit,
                                  core_string_t module)
{
    return unit && module.data
        ? core_dict_get((core_dict_t *)&unit->imports_by_module,
                        &module, nullptr) : nullptr;
}

uint64_t
core_translation_unit_import_revision(const core_translation_unit_t *unit)
{
    return unit ? unit->import_revision : 0;
}

bool
core_translation_unit_finalize_imports(core_translation_unit_t *unit)
{
    if (!unit) return false;
    unit->imports_finalized = true;
    return true;
}

bool
core_translation_unit_imports_finalized(const core_translation_unit_t *unit)
{
    return unit && unit->imports_finalized;
}

bool
core_translation_units_visit_sccs(core_translation_unit_t *const *roots,
                                   size_t root_count,
                                   core_translation_unit_scc_fn callback,
                                   void *context)
{
    if ((!roots && root_count) || !root_count || !callback) return false;
    core_list_t(core_tu_ptr_t) units = core_list_new(core_tu_ptr_t);
    core_dict_t unit_index;
    core_dict_init(&unit_index, nullptr, nullptr);
    for (size_t i = 0; i < root_count; i++) {
        if (!roots[i]) {
            core_dict_free(&unit_index);
            core_list_free(units);
            return false;
        }
        core_tu_collect_imports(&units, &unit_index, roots[i]);
    }
    core_tu_scc_ctx_t state = {
        .units = &units,
        .unit_index = &unit_index,
        .indices = core_alloc_array(int, units.len),
        .lowlinks = core_alloc_array(int, units.len),
        .stack = core_alloc_array(size_t, units.len),
        .on_stack = core_alloc_array(bool, units.len),
        .callback = callback,
        .context = context,
        .succeeded = true,
    };
    memset(state.on_stack, 0, units.len * sizeof(bool));
    for (size_t i = 0; i < units.len; i++) state.indices[i] = -1;
    for (size_t i = 0; i < units.len && state.succeeded; i++)
        if (state.indices[i] < 0) core_tu_scc_visit(&state, i);
    core_free(state.indices);
    core_free(state.lowlinks);
    core_free(state.stack);
    core_free(state.on_stack);
    core_dict_free(&unit_index);
    core_list_free(units);
    return state.succeeded;
}

bool
core_translation_unit_visit_sccs(core_translation_unit_t *root,
                                 core_translation_unit_scc_fn callback,
                                 void *context)
{
    return root && core_translation_units_visit_sccs(&root, 1, callback,
                                                     context);
}

core_module_resolve_status_t
core_module_resolve(const core_module_resolver_t *resolver,
                    const core_module_spec_t *module, core_source_file_t *out)
{
    if (!resolver || !resolver->resolve || !module || !module->identifier.data
        || !out)
        return CORE_MODULE_RESOLVE_ERROR;

    *out = (core_source_file_t){0};
    core_module_resolve_status_t status =
        resolver->resolve(resolver->context, module, out);
    if (status != CORE_MODULE_RESOLVED && status != CORE_MODULE_NOT_FOUND
        && status != CORE_MODULE_RESOLVE_ERROR) {
        core_source_file_release(out);
        return CORE_MODULE_RESOLVE_ERROR;
    }
    if (status == CORE_MODULE_RESOLVED
        && (!out->path.data || !out->contents)) {
        core_source_file_release(out);
        return CORE_MODULE_RESOLVE_ERROR;
    }
    if (status != CORE_MODULE_RESOLVED) core_source_file_release(out);
    return status;
}

core_module_resolve_status_t
core_module_discover(const core_module_resolver_t *resolver,
                     const core_module_spec_t *module,
                     core_module_discovery_emit_fn emit,
                     void *emit_context)
{
    if (!resolver || !resolver->discover || !module || !module->identifier.data
        || !module->identifier.u8_bytes || !emit)
        return CORE_MODULE_NOT_FOUND;
    return resolver->discover(resolver->context, module, emit, emit_context);
}
