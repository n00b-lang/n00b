#include "compiler_ffi.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/diagnostic.h"
#include "lib/list.h"
#include "lib/string.h"
#include "driver.h"
#include "writ/gen.h"

typedef struct {
    core_string_t name;
    core_string_t contents;
} ffi_output_t;

core_list_decl(ffi_output_t);

struct n00bb_ffi {
    pthread_mutex_t                lock;
    writ_session_t                *session;
    writ_unit_t                   *profiles;
    core_list_t(core_string_t)     contract_dirs;
    core_list_t(core_string_t)     package_dirs;
    core_list_t(core_string_t)     inputs;
    core_list_t(ffi_output_t)      shims;
    core_list_t(core_string_t)     libraries;
    core_list_t(core_string_t)     frameworks;
    core_list_t(core_string_t)     search_paths;
    size_t                         literals;
};

static writ_str_t
as_writ(core_string_t text)
{
    return (writ_str_t){text.data, text.u8_bytes};
}

static core_string_t
as_core(writ_str_t text)
{
    return core_string_borrow(text.data, text.length);
}

static void
push_unique(core_list_t(core_string_t) *list, writ_str_t text)
{
    core_string_t value = as_core(text);
    for (size_t i = 0; i < list->len; i++) {
        if (core_string_eq(list->data[i], value)) {
            return;
        }
    }
    core_list_push(*list, core_string_dup(value));
}

static void
free_strings(core_list_t(core_string_t) *list)
{
    for (size_t i = 0; i < list->len; i++) {
        core_free((void *)list->data[i].data);
    }
    core_list_free(*list);
}

n00bb_ffi_t *
n00bb_ffi_new(void)
{
    n00bb_ffi_t *ffi = core_alloc(n00bb_ffi_t);
    if (!ffi) {
        return nullptr;
    }
    *ffi = (n00bb_ffi_t){
        .contract_dirs = core_list_new(core_string_t),
        .package_dirs  = core_list_new(core_string_t),
        .inputs       = core_list_new(core_string_t),
        .shims         = core_list_new(ffi_output_t),
        .libraries     = core_list_new(core_string_t),
        .frameworks    = core_list_new(core_string_t),
        .search_paths  = core_list_new(core_string_t),
    };
    pthread_mutex_init(&ffi->lock, nullptr);
    return ffi;
}

void
n00bb_ffi_free(n00bb_ffi_t *ffi)
{
    if (!ffi) {
        return;
    }
    writ_session_free(ffi->session);
    free_strings(&ffi->contract_dirs);
    free_strings(&ffi->package_dirs);
    free_strings(&ffi->inputs);
    for (size_t i = 0; i < ffi->shims.len; i++) {
        core_free((void *)ffi->shims.data[i].name.data);
        core_free((void *)ffi->shims.data[i].contents.data);
    }
    core_list_free(ffi->shims);
    free_strings(&ffi->libraries);
    free_strings(&ffi->frameworks);
    free_strings(&ffi->search_paths);
    pthread_mutex_destroy(&ffi->lock);
    core_free(ffi);
}

bool
n00bb_ffi_add_contract_dir(n00bb_ffi_t *ffi, core_string_t dir)
{
    if (!ffi || !dir.u8_bytes) {
        return false;
    }
    core_list_push(ffi->contract_dirs, core_string_dup(dir));
    return true;
}

static bool
open_session(n00bb_ffi_t *ffi)
{
    if (ffi->session) {
        return ffi->profiles != nullptr;
    }
    ffi->session = writ_session_new(nullptr);
    if (!ffi->session) {
        return false;
    }
    for (size_t i = 0; i < ffi->contract_dirs.len; i++) {
        writ_session_add_contract_dir(ffi->session, as_writ(ffi->contract_dirs.data[i]));
    }
    const char *profile = N00BB_WRIT_PROFILE_DIR "/n00b.writ";
    ffi->profiles = writ_unit_new(ffi->session);
    if (writ_unit_add_file(ffi->profiles, (writ_str_t){profile, strlen(profile)}, nullptr) != WRIT_OK) {
        ffi->profiles = nullptr;
    }
    return ffi->profiles != nullptr;
}

typedef struct {
    core_list_t(ffi_output_t) files;
} capture_t;

static writ_status_t
capture_file(void *context, writ_str_t path, writ_str_t contents)
{
    capture_t *capture = context;
    core_list_push(capture->files, ((ffi_output_t){core_string_dup(as_core(path)),
                                                   core_string_dup(as_core(contents))}));
    return WRIT_OK;
}

static void
forward_diagnostics(n00bb_ffi_t *ffi, core_translation_unit_t *unit, size_t first, n00bb_literal_origin_t origin)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    for (size_t i = first; i < writ_diag_count(ffi->session); i++) {
        const writ_diag_t *diag = writ_diag_at(ffi->session, i);
        core_diag_severity_t severity = diag->severity == WRIT_ERROR     ? CORE_DIAG_ERROR
                                        : diag->severity == WRIT_WARNING ? CORE_DIAG_WARNING
                                                                         : CORE_DIAG_NOTE;
        core_diag_location_t location = {
            .line         = diag->primary.span.line,
            .column       = diag->primary.span.column,
            .end_line     = diag->primary.span.line,
            .end_column   = diag->primary.span.column,
            .has_location = diag->primary.span.line != 0,
        };
        if (!location.has_location) {
            location = (core_diag_location_t){origin.line, origin.column, origin.line, origin.column, true};
        }
        size_t index = core_diagnostics_count(diagnostics);
        core_diagnostics_report_at(diagnostics, unit, severity, as_core(diag->code), as_core(diag->message),
                                   location);
        if (diag->path.length && core_diagnostics_count(diagnostics) > index) {
            core_diagnostic_set_path(diagnostics, index, as_core(diag->path));
        }
    }
}

static void
keep_links(n00bb_ffi_t *ffi, const writ_link_requirements_t *links)
{
    for (size_t i = 0; i < links->libraries_count; i++) {
        push_unique(&ffi->libraries, links->libraries[i]);
    }
    for (size_t i = 0; i < links->frameworks_count; i++) {
        push_unique(&ffi->frameworks, links->frameworks[i]);
    }
    for (size_t i = 0; i < links->search_paths_count; i++) {
        push_unique(&ffi->search_paths, links->search_paths[i]);
    }
}

static bool
ends_with(core_string_t text, const char *suffix)
{
    size_t length = strlen(suffix);
    return text.u8_bytes >= length && !memcmp(text.data + text.u8_bytes - length, suffix, length);
}

static bool
generate_locked(n00bb_ffi_t *ffi, core_translation_unit_t *unit, core_string_t name, n00bb_literal_origin_t origin,
                core_string_t text, core_buffer_t *source)
{
    size_t first = writ_diag_count(ffi->session);
    writ_unit_t *literal = writ_unit_new(ffi->session);
    writ_origin_t where = {.host_path = as_writ(origin.host_path), .first_line = origin.line,
                           .first_column = origin.column, .first_byte = origin.byte};
    writ_platform_t *platform = nullptr;
    const writ_model_t *model = nullptr;
    const writ_binding_t *binding = nullptr;
    writ_str_t language = {"n00b", 4};
    bool ok = writ_unit_add_embedded(literal, language, where, as_writ(text), nullptr) == WRIT_OK
           && writ_platform_from_triple(ffi->session, writ_platform_host_triple(), &platform) == WRIT_OK
           && writ_resolve(literal, platform, &model) == WRIT_OK;
    if (ok) {
        writ_bind_request_t request = {.struct_size = sizeof(request), .profiles = ffi->profiles,
                                       .binding = literal, .language = language};
        ok = writ_bind(model, &request, &binding) == WRIT_OK;
    }
    for (size_t i = 0; i < writ_unit_file_count(literal); i++) {
        push_unique(&ffi->inputs, writ_unit_file_at(literal, i));
    }
    capture_t capture = {core_list_new(ffi_output_t)};
    writ_link_requirements_t links = {0};
    if (ok) {
        core_buffer_t prefix = {0};
        core_buffer_printf(&prefix, "n00b_ffi_%zu", ffi->literals++);
        writ_gen_n00b_request_t request = {.struct_size = sizeof(request), .binding = binding,
                                           .module_name = as_writ(name),
                                           .shim_prefix = (writ_str_t){prefix.data, prefix.byte_len}};
        writ_output_t output = {capture_file, &capture};
        ok = writ_gen_n00b(&request, &output, &links) == WRIT_OK;
        core_buffer_release(&prefix);
    }
    for (size_t i = 0; ok && i < capture.files.len; i++) {
        ffi_output_t *file = &capture.files.data[i];
        if (ends_with(file->name, ".n")) {
            core_buffer_append(source, file->contents.data, file->contents.u8_bytes);
        } else if (!ends_with(file->name, ".map")) {
            core_list_push(ffi->shims, *file);
            *file = (ffi_output_t){0};
        }
    }
    for (size_t i = 0; i < capture.files.len; i++) {
        core_free((void *)capture.files.data[i].name.data);
        core_free((void *)capture.files.data[i].contents.data);
    }
    core_list_free(capture.files);
    if (ok) {
        keep_links(ffi, &links);
    }
    forward_diagnostics(ffi, unit, first, origin);
    return ok;
}

static void
add_package_contracts(n00bb_ffi_t *ffi, core_translation_unit_t *unit)
{
    core_string_t package = n00bb_package_id(unit);
    core_buffer_t dir = {0};
    if (core_string_eq(package, CORE_STRING_STATIC("default"))) {
        core_string_t path = core_source_file_path(&unit->source);
        size_t cut = path.u8_bytes;
        while (cut && path.data[cut - 1] != '/') {
            cut--;
        }
        core_buffer_printf(&dir, "%.*scontracts", (int)cut, path.data);
    } else {
        core_buffer_printf(&dir, "%.*s/contracts", (int)package.u8_bytes, package.data);
    }
    writ_str_t added = {dir.data, dir.byte_len};
    size_t before = ffi->package_dirs.len;
    push_unique(&ffi->package_dirs, added);
    if (ffi->package_dirs.len > before) {
        writ_session_add_contract_dir(ffi->session, added);
    }
    core_buffer_release(&dir);
}

bool
n00bb_ffi_generate(void *context, core_translation_unit_t *unit, core_string_t name, n00bb_literal_origin_t origin,
                   core_string_t text, core_buffer_t *source)
{
    n00bb_ffi_t *ffi = context;
    if (!ffi || !unit || !source) {
        return false;
    }
    pthread_mutex_lock(&ffi->lock);
    bool ok = open_session(ffi);
    if (ok) {
        add_package_contracts(ffi, unit);
        ok = generate_locked(ffi, unit, name, origin, text, source);
    }
    pthread_mutex_unlock(&ffi->lock);
    return ok;
}

size_t
n00bb_ffi_input_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->inputs.len : 0;
}

core_string_t
n00bb_ffi_input_at(const n00bb_ffi_t *ffi, size_t index)
{
    return ffi->inputs.data[index];
}

size_t
n00bb_ffi_shim_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->shims.len : 0;
}

n00bb_ffi_file_t
n00bb_ffi_shim_at(const n00bb_ffi_t *ffi, size_t index)
{
    return (n00bb_ffi_file_t){ffi->shims.data[index].name, ffi->shims.data[index].contents};
}

size_t
n00bb_ffi_library_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->libraries.len : 0;
}

core_string_t
n00bb_ffi_library_at(const n00bb_ffi_t *ffi, size_t index)
{
    return ffi->libraries.data[index];
}

size_t
n00bb_ffi_framework_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->frameworks.len : 0;
}

core_string_t
n00bb_ffi_framework_at(const n00bb_ffi_t *ffi, size_t index)
{
    return ffi->frameworks.data[index];
}

size_t
n00bb_ffi_search_path_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->search_paths.len : 0;
}

core_string_t
n00bb_ffi_search_path_at(const n00bb_ffi_t *ffi, size_t index)
{
    return ffi->search_paths.data[index];
}
