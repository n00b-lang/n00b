#include "lib/module.h"

#include "lib/alloc.h"
#include "lib/diagnostic.h"

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
                      const char *contents, size_t byte_len)
{
    if (!file || !path.data || (!contents && byte_len)) return false;
    *file = (core_source_file_t){
        .path = core_string_dup(path),
        .contents = core_buffer_from_bytes(contents ? contents : "",
                                           (int64_t)byte_len),
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

const char *
core_source_file_contents(const core_source_file_t *file)
{
    return file && file->contents ? file->contents->data : nullptr;
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
    *source = (core_source_file_t){0};
    return true;
}

void
core_translation_unit_release(core_translation_unit_t *unit)
{
    if (!unit) return;
    if (unit->user_data_free) unit->user_data_free(unit->user_data);
    core_diagnostics_free(unit->diagnostics);
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
