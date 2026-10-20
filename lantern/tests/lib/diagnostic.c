#include <string.h>

#include "lib/diagnostic.h"
#include "lib/module.h"
#include "harness.h"

static bool
promote_warning(void *context, core_diag_severity_t *severity,
                core_string_t code)
{
    (void)context;
    (void)code;
    if (*severity == CORE_DIAG_WARNING) *severity = CORE_DIAG_ERROR;
    return true;
}

int main(void)
{
    core_module_spec_t module = {0};
    core_source_file_t source = {0};
    core_translation_unit_t unit = {0};
    check("module identity initializes",
          core_module_spec_init(&module, CORE_STRING_STATIC("pkg.mod")));
    check("source initializes", core_source_file_init(
        &source, CORE_STRING_STATIC("pkg/mod.n00b"),
        CORE_STRING_STATIC("")));
    check("translation unit owns its diagnostic sink",
          core_translation_unit_init(&unit, &module, &source));

    core_diagnostics_t *build = core_diagnostics_new();
    core_diagnostics_t *tu = core_translation_unit_diagnostics(&unit);
    core_diagnostics_set_policy(build, promote_warning, nullptr);
    check("TU diagnostic retains its module and source location",
          core_diagnostics_report_at(tu, &unit, CORE_DIAG_ERROR,
              CORE_STRING_STATIC("TEST-E0001"),
              CORE_STRING_STATIC("bad declaration"),
              (core_diag_location_t){.line = 3, .column = 4,
                                     .end_line = 3, .end_column = 8,
                                     .has_location = true}));
    check("TU warnings can be collected before build-level policy runs",
          core_diagnostics_report(tu, nullptr, CORE_DIAG_WARNING,
              CORE_STRING_STATIC("TEST-W0001"),
              CORE_STRING_STATIC("warning")));
    check("TU diagnostics merge into build diagnostics",
          core_diagnostics_merge(build, tu)
              && core_diagnostics_count(build) == 2
              && core_diagnostics_count_severity(build, CORE_DIAG_ERROR) == 2
              && core_diagnostics_failed(build));
    const core_diagnostic_t *diagnostic = core_diagnostics_at(build, 0);
    check("merged diagnostic retains TU identity and path",
          diagnostic
              && core_string_eq(diagnostic->module,
                                CORE_STRING_STATIC("pkg.mod"))
              && core_string_eq(diagnostic->path,
                                CORE_STRING_STATIC("pkg/mod.n00b"))
              && diagnostic->location.has_location
              && diagnostic->location.line == 3
              && diagnostic->location.column == 4);
    diagnostic = core_diagnostics_at(build, 1);
    check("TU sink attributes reports without an explicit unit",
          diagnostic
              && core_string_eq(diagnostic->module,
                                CORE_STRING_STATIC("pkg.mod"))
              && core_string_eq(diagnostic->path,
                                CORE_STRING_STATIC("pkg/mod.n00b")));
    check("a diagnostic can name a file other than its unit's source",
          core_diagnostic_set_path(build, 1,
              CORE_STRING_STATIC("contracts/zlib.writ")));
    diagnostic = core_diagnostics_at(build, 1);
    check("a moved diagnostic keeps its unit and reports the new path",
          diagnostic
              && core_string_eq(diagnostic->module,
                                CORE_STRING_STATIC("pkg.mod"))
              && core_string_eq(diagnostic->path,
                                CORE_STRING_STATIC("contracts/zlib.writ")));
    check("setting the path of a missing diagnostic fails",
          !core_diagnostic_set_path(build, 2,
              CORE_STRING_STATIC("contracts/zlib.writ")));

    core_diagnostics_free(build);
    core_translation_unit_release(&unit);
    core_module_spec_release(&module);
    core_source_file_release(&source);
    return report("lib diagnostics");
}
