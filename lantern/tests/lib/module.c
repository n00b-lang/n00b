#include <string.h>

#include "core.h"
#include "lib/module.h"
#include "harness.h"

static void
record_user_data_release(void *data)
{
    (*(int *)data)++;
}

static core_module_resolve_status_t
resolve_memory(void *context, const core_module_spec_t *module,
               core_source_file_t *out)
{
    core_string_t wanted = *(core_string_t *)context;
    if (!core_string_eq(module->identifier, wanted))
        return CORE_MODULE_NOT_FOUND;

    bool ok = core_source_file_init(out, CORE_STRING_STATIC("mem://pkg/math"),
                                    "value = 42", 10);
    return ok ? CORE_MODULE_RESOLVED : CORE_MODULE_RESOLVE_ERROR;
}

static core_module_resolve_status_t
resolve_incomplete(void *context, const core_module_spec_t *module,
                   core_source_file_t *out)
{
    (void)context;
    (void)module;
    out->path = core_string_dup(CORE_STRING_STATIC("mem://incomplete"));
    return CORE_MODULE_RESOLVED;
}

int
main(void)
{
    core_module_spec_t spec = {0};
    check("a nonempty module identifier initializes",
          core_module_spec_init(&spec, CORE_STRING_STATIC("pkg.math")));
    check("module specs compare by logical identifier",
          core_module_spec_eq(&spec, &spec));

    core_string_t wanted = CORE_STRING_STATIC("pkg.math");
    core_module_resolver_t resolver = {
        .resolve = resolve_memory,
        .context = &wanted,
    };
    core_source_file_t source = {0};
    check("the resolver maps a module to a source file",
          core_module_resolve(&resolver, &spec, &source)
              == CORE_MODULE_RESOLVED);
    check("the source keeps its display path",
          core_string_eq(core_source_file_path(&source),
                         CORE_STRING_STATIC("mem://pkg/math")));
    check("the source bytes are available",
          core_source_file_size(&source) == 10
              && !memcmp(core_source_file_contents(&source), "value = 42", 10));

    int released = 0;
    core_module_spec_set_user_data(&spec, &released, record_user_data_release);
    core_source_file_set_user_data(&source, &released,
                                  record_user_data_release);

    core_translation_unit_t unit = {0};
    check("a translation unit takes ownership of its source file",
          core_translation_unit_init(&unit, &spec, &source)
              && !source.path.data && !source.contents);
    check("the translation unit preserves logical and physical identity",
          core_module_spec_eq(&unit.module, &spec)
              && core_string_eq(core_source_file_path(&unit.source),
                                CORE_STRING_STATIC("mem://pkg/math")));
    check("source user data moves into the translation unit",
          core_source_file_user_data(&unit.source) == &released);
    core_translation_unit_set_user_data(&unit, &released,
                                        record_user_data_release);
    check("translation-unit user data is retrievable",
          core_translation_unit_user_data(&unit) == &released);

    core_source_file_t missing = {0};
    core_module_spec_t other = {0};
    core_module_spec_init(&other, CORE_STRING_STATIC("pkg.absent"));
    check("unresolved modules are reported without partial output",
          core_module_resolve(&resolver, &other, &missing)
                  == CORE_MODULE_NOT_FOUND
              && !missing.path.data && !missing.contents);

    core_module_resolver_t broken = {.resolve = resolve_incomplete};
    core_source_file_t invalid = {0};
    check("incomplete resolved files become resolver errors",
          core_module_resolve(&broken, &spec, &invalid)
                  == CORE_MODULE_RESOLVE_ERROR
              && !invalid.path.data && !invalid.contents);

    core_translation_unit_release(&unit);
    check("translation-unit and source payloads are released",
          released == 2);
    core_source_file_release(&missing);
    core_source_file_release(&invalid);
    core_module_spec_release(&other);
    core_module_spec_release(&spec);
    check("module-spec payload is released", released == 3);
    return report("lib-module");
}
