#include "compiler_ffi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compiler_native_roots.h"
#include "compiler_phases.h"
#include "llvm_native_cache.h"
#include "lib/alloc.h"
#include "lib/diagnostic.h"
#include "lib/hash.h"
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
    core_list_t(core_string_t)     archives;
    core_list_t(core_string_t)     libraries;
    core_list_t(core_string_t)     found_libraries;
    core_list_t(core_string_t)     transports;
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

static bool
contains(const core_list_t(core_string_t) *list, core_string_t value)
{
    for (size_t i = 0; i < list->len; i++) {
        if (core_string_eq(list->data[i], value)) {
            return true;
        }
    }
    return false;
}

static void
push_unique(core_list_t(core_string_t) *list, writ_str_t text)
{
    if (!contains(list, as_core(text))) {
        core_list_push(*list, core_string_dup(as_core(text)));
    }
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
        .archives      = core_list_new(core_string_t),
        .libraries     = core_list_new(core_string_t),
        .found_libraries = core_list_new(core_string_t),
        .transports    = core_list_new(core_string_t),
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
    free_strings(&ffi->archives);
    free_strings(&ffi->libraries);
    free_strings(&ffi->found_libraries);
    free_strings(&ffi->transports);
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
        if (!contains(&ffi->transports, as_core(links->libraries[i]))) {
            push_unique(&ffi->libraries, links->libraries[i]);
        }
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
keep_externs(const writ_n00b_externs_t *externs, n00bb_native_declarations_t *natives)
{
    *natives = (n00bb_native_declarations_t){0};
    if (!externs->count) {
        return true;
    }
    natives->items = core_alloc_array(n00bb_native_declaration_t, externs->count);
    if (!natives->items) {
        return false;
    }
    for (size_t i = 0; i < externs->count; i++) {
        natives->items[i] = (n00bb_native_declaration_t){
            .name      = core_string_dup(as_core(externs->items[i].name)),
            .symbol    = core_string_dup(as_core(externs->items[i].symbol)),
            .signature = core_string_dup(as_core(externs->items[i].signature)),
        };
    }
    natives->count = externs->count;
    return true;
}

static bool
register_exports(const writ_n00b_externs_t *externs)
{
    for (size_t i = 0; i < externs->exports_count; i++) {
        char *function = strndup(externs->exports[i].function.data, externs->exports[i].function.length);
        char *symbol = strndup(externs->exports[i].symbol.data, externs->exports[i].symbol.length);
        bool registered = function && symbol && n00bb_native_root_register(function, symbol);
        free(function);
        free(symbol);
        if (!registered) {
            return false;
        }
    }
    return true;
}

extern char **environ;

static bool
write_whole_file(const char *path, core_string_t contents)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    bool ok = fwrite(contents.data, 1, contents.u8_bytes, file) == contents.u8_bytes;
    return fclose(file) == 0 && ok;
}

static bool
read_whole_file(const char *path, core_buffer_t *out)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    char chunk[65536];
    size_t count = 0;
    while ((count = fread(chunk, 1, sizeof chunk, file)) != 0) {
        core_buffer_append(out, chunk, count);
    }
    bool ok = !ferror(file);
    fclose(file);
    return ok && out->byte_len;
}

static void
report_error(core_translation_unit_t *unit, n00bb_literal_origin_t origin, const char *code, const char *message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit, CORE_DIAG_ERROR,
                               core_string_borrow(code, strlen(code)), core_string_borrow(message, strlen(message)),
                               (core_diag_location_t){origin.line, origin.column, origin.line, origin.column, true});
}

static bool
run_logged(char **arguments, const char *log)
{
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        return false;
    }
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log ? log : "/dev/null", O_WRONLY | O_CREAT | O_TRUNC,
                                     0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t child = 0;
    int status = 0;
    bool ok = !posix_spawnp(&child, arguments[0], &actions, nullptr, arguments, environ)
           && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    posix_spawn_file_actions_destroy(&actions);
    return ok;
}

static bool
library_found(n00bb_ffi_t *ffi, core_string_t library)
{
    if (contains(&ffi->found_libraries, library) || contains(&ffi->transports, library)) {
        return true;
    }
    char directory[] = "/tmp/n00bb-probe.XXXXXX";
    if (!mkdtemp(directory)) {
        return false;
    }
    core_list_t(core_buffer_t) flags = core_list_new(core_buffer_t);
    for (size_t i = 0; i < ffi->search_paths.len; i++) {
        core_buffer_t flag = {0};
        core_buffer_printf(&flag, "-L%.*s", (int)ffi->search_paths.data[i].u8_bytes, ffi->search_paths.data[i].data);
        core_list_push(flags, flag);
    }
    core_buffer_t named = {0}, output = {0};
    core_buffer_printf(&named, "-l%.*s", (int)library.u8_bytes, library.data);
    core_buffer_printf(&output, "%s/probe", directory);
    char **arguments = core_alloc_array(char *, flags.len + 10);
    size_t next = 0;
    arguments[next++] = N00BB_NATIVE_LINKER;
    arguments[next++] = "-shared";
    arguments[next++] = "-x";
    arguments[next++] = "c";
    arguments[next++] = "/dev/null";
    for (size_t i = 0; i < flags.len; i++) {
        arguments[next++] = flags.data[i].data;
    }
    arguments[next++] = named.data;
    arguments[next++] = "-o";
    arguments[next++] = output.data;
    arguments[next] = nullptr;
    bool found = run_logged(arguments, nullptr);
    unlink(output.data);
    rmdir(directory);
    core_free(arguments);
    for (size_t i = 0; i < flags.len; i++) {
        core_buffer_release(&flags.data[i]);
    }
    core_list_free(flags);
    core_buffer_release(&named);
    core_buffer_release(&output);
    if (found) {
        core_list_push(ffi->found_libraries, core_string_dup(library));
    }
    return found;
}

static bool
check_libraries(n00bb_ffi_t *ffi, core_translation_unit_t *unit, n00bb_literal_origin_t origin,
                const writ_link_requirements_t *links)
{
    bool ok = true;
    for (size_t i = 0; i < links->libraries_count; i++) {
        core_string_t library = as_core(links->libraries[i]);
        if (!library_found(ffi, library)) {
            core_buffer_t message = {0};
            core_buffer_printf(&message, "native library '%.*s' required by this 'ffi literal is not available to link",
                               (int)library.u8_bytes, library.data);
            report_error(unit, origin, "N00BB-F0001", message.data);
            core_buffer_release(&message);
            ok = false;
        }
    }
    return ok;
}

static int
remove_entry(const char *path, const struct stat *status, int kind, struct FTW *walk)
{
    (void)status;
    (void)kind;
    (void)walk;
    return remove(path);
}

static bool
write_crate(const char *directory, const capture_t *crate)
{
    bool ok = true;
    for (size_t i = 0; ok && i < crate->files.len; i++) {
        const ffi_output_t *file = &crate->files.data[i];
        core_buffer_t path = {0};
        core_buffer_printf(&path, "%s/%.*s", directory, (int)file->name.u8_bytes, file->name.data);
        for (char *at = path.data + strlen(directory) + 1; ok && *at; at++) {
            if (*at == '/') {
                *at = '\0';
                ok = !mkdir(path.data, 0700) || errno == EEXIST;
                *at = '/';
            }
        }
        ok = ok && write_whole_file(path.data, file->contents);
        core_buffer_release(&path);
    }
    return ok;
}

static char *
staticlib_name(const char *directory)
{
    core_buffer_t release = {0};
    core_buffer_printf(&release, "%s/target/release", directory);
    DIR *listing = opendir(release.data);
    core_buffer_release(&release);
    if (!listing) {
        return nullptr;
    }
    char *name = nullptr;
    for (struct dirent *entry = readdir(listing); entry && !name; entry = readdir(listing)) {
        size_t length = strlen(entry->d_name);
        if (length > 5 && !strncmp(entry->d_name, "lib", 3) && !strcmp(entry->d_name + length - 2, ".a")) {
            name = core_strdup(entry->d_name);
        }
    }
    closedir(listing);
    return name;
}

static bool
build_crate(const char *directory)
{
    core_buffer_t manifest = {0}, log = {0}, text = {0}, record = {0};
    core_buffer_printf(&manifest, "%s/Cargo.toml", directory);
    core_buffer_printf(&log, "%s/cargo.log", directory);
    char *arguments[] = {N00BB_CARGO, "rustc", "--release", "--lib", "--crate-type", "staticlib", "--manifest-path",
                         manifest.data, "--", "--print", "native-static-libs", nullptr};
    bool ok = run_logged(arguments, log.data) && read_whole_file(log.data, &text);
    core_buffer_append(&text, "", 1);
    const char *marker = "native-static-libs:";
    const char *flags = ok ? strstr(text.data, marker) : nullptr;
    char *archive = ok ? staticlib_name(directory) : nullptr;
    ok = flags && archive;
    if (ok) {
        flags += strlen(marker);
        core_buffer_printf(&record, "%s\n%.*s\n", archive, (int)strcspn(flags, "\r\n"), flags);
        core_buffer_t path = {0};
        core_buffer_printf(&path, "%s/link", directory);
        ok = write_whole_file(path.data, core_string_borrow(record.data, record.byte_len));
        core_buffer_release(&path);
    }
    core_free(archive);
    core_buffer_release(&manifest);
    core_buffer_release(&log);
    core_buffer_release(&text);
    core_buffer_release(&record);
    return ok;
}

static bool
keep_crate(n00bb_ffi_t *ffi, const char *directory)
{
    core_buffer_t path = {0}, record = {0};
    core_buffer_printf(&path, "%s/link", directory);
    bool ok = read_whole_file(path.data, &record);
    core_buffer_append(&record, "", 1);
    core_buffer_release(&path);
    char *flags = ok ? strchr(record.data, '\n') : nullptr;
    ok = flags != nullptr;
    if (ok) {
        *flags++ = '\0';
        core_buffer_t archive = {0};
        core_buffer_printf(&archive, "%s/target/release/%s", directory, record.data);
        push_unique(&ffi->archives, (writ_str_t){archive.data, archive.byte_len});
        core_buffer_release(&archive);
        push_unique(&ffi->transports, (writ_str_t){record.data + 3, strlen(record.data) - 5});
        bool framework = false;
        for (char *token = strtok(flags, " \t\r\n"); token; token = strtok(nullptr, " \t\r\n")) {
            if (framework) {
                push_unique(&ffi->frameworks, (writ_str_t){token, strlen(token)});
                framework = false;
            } else if (!strcmp(token, "-framework")) {
                framework = true;
            } else if (!strncmp(token, "-l", 2) && token[2]) {
                push_unique(&ffi->libraries, (writ_str_t){token + 2, strlen(token + 2)});
            } else if (!strncmp(token, "-L", 2) && token[2]) {
                push_unique(&ffi->search_paths, (writ_str_t){token + 2, strlen(token + 2)});
            }
        }
    }
    core_buffer_release(&record);
    return ok;
}

static bool
keep_transport(n00bb_ffi_t *ffi, core_translation_unit_t *unit, n00bb_literal_origin_t origin,
               const writ_model_t *model)
{
    capture_t crate = {core_list_new(ffi_output_t)};
    writ_gen_rust_request_t request = {.struct_size = sizeof(request)};
    writ_output_t output = {capture_file, &crate};
    writ_status_t status = writ_gen_rust(model, &request, &output);
    bool ok = status == WRIT_OK || status == WRIT_INVALID_ARGUMENT;
    char *root = nullptr;
    if (ok && status == WRIT_OK && !*N00BB_CARGO) {
        report_error(unit, origin, "N00BB-F0002",
                     "this 'ffi literal needs a Rust transport crate, but this n00bb was built without Rust transports");
        ok = false;
    } else if (ok && status == WRIT_OK && !(root = n00bb_native_cache_root())) {
        report_error(unit, origin, "N00BB-F0003", "no cache directory is available for the Rust transport crate");
        ok = false;
    } else if (ok && status == WRIT_OK) {
        core_buffer_t payload = {0}, directory = {0};
        for (size_t i = 0; i < crate.files.len; i++) {
            core_buffer_append(&payload, crate.files.data[i].name.data, crate.files.data[i].name.u8_bytes + 1);
            core_buffer_append(&payload, crate.files.data[i].contents.data, crate.files.data[i].contents.u8_bytes + 1);
        }
        core_uint128_t key = core_xxh3_128bits_raw(payload.data, payload.byte_len);
        core_buffer_printf(&directory, "%s/rust", root);
        ok = !mkdir(directory.data, 0700) || errno == EEXIST;
        core_buffer_printf(&directory, "/%016llx%016llx", (unsigned long long)(key >> 64), (unsigned long long)key);
        if (ok && !keep_crate(ffi, directory.data)) {
            core_buffer_t staging = {0};
            core_buffer_printf(&staging, "%s.XXXXXX", directory.data);
            ok = mkdtemp(staging.data) && write_crate(staging.data, &crate);
            bool built = ok && build_crate(staging.data);
            if (built && rename(staging.data, directory.data)) {
                nftw(staging.data, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
            }
            if (ok && !built) {
                core_buffer_t message = {0};
                core_buffer_printf(&message, "building the Rust transport crate failed; cargo output is in %s/cargo.log",
                                   staging.data);
                report_error(unit, origin, "N00BB-F0004", message.data);
                core_buffer_release(&message);
            }
            ok = built && keep_crate(ffi, directory.data);
            core_buffer_release(&staging);
        }
        core_buffer_release(&payload);
        core_buffer_release(&directory);
    }
    core_free(root);
    for (size_t i = 0; i < crate.files.len; i++) {
        core_free((void *)crate.files.data[i].name.data);
        core_free((void *)crate.files.data[i].contents.data);
    }
    core_list_free(crate.files);
    return ok;
}

static bool
generate_locked(n00bb_ffi_t *ffi, core_translation_unit_t *unit, core_string_t name, core_string_t module,
                n00bb_literal_origin_t origin, core_string_t text, core_buffer_t *source,
                n00bb_native_declarations_t *natives)
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
                                           .shim_prefix = (writ_str_t){prefix.data, prefix.byte_len},
                                           .type_namespace = as_writ(module)};
        writ_output_t output = {capture_file, &capture};
        writ_n00b_externs_t externs = {0};
        ok = writ_gen_n00b(&request, &output, &links, &externs) == WRIT_OK && keep_externs(&externs, natives)
          && register_exports(&externs);
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
    ok = ok && keep_transport(ffi, unit, origin, model);
    if (ok) {
        keep_links(ffi, &links);
        ok = check_libraries(ffi, unit, origin, &links);
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
n00bb_ffi_generate(void *context, core_translation_unit_t *unit, core_string_t name, core_string_t module,
                   n00bb_literal_origin_t origin, core_string_t text, core_buffer_t *source,
                   n00bb_native_declarations_t *natives)
{
    n00bb_ffi_t *ffi = context;
    if (!ffi || !unit || !source || !natives) {
        return false;
    }
    pthread_mutex_lock(&ffi->lock);
    bool ok = open_session(ffi);
    if (ok) {
        add_package_contracts(ffi, unit);
        ok = generate_locked(ffi, unit, name, module, origin, text, source, natives);
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
n00bb_ffi_archive_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->archives.len : 0;
}

core_string_t
n00bb_ffi_archive_at(const n00bb_ffi_t *ffi, size_t index)
{
    return ffi->archives.data[index];
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

static char *
sdk_root(void)
{
#ifdef __APPLE__
    const char *configured = getenv("SDKROOT");
    if (configured && *configured) {
        return core_strdup(configured);
    }
    FILE *query = popen("/usr/bin/xcrun --show-sdk-path 2>/dev/null", "r");
    if (!query) {
        return nullptr;
    }
    char path[4096] = {0};
    bool read = fgets(path, sizeof path, query) != nullptr;
    pclose(query);
    size_t length = read ? strlen(path) : 0;
    while (length && (path[length - 1] == '\n' || path[length - 1] == '\r')) {
        path[--length] = '\0';
    }
    return length ? core_strdup(path) : nullptr;
#else
    return nullptr;
#endif
}

static bool
run_clang(const char *triple, const char *directory, const char *source, const char *output)
{
    const char *compiler = getenv("N00BB_CC");
    if (!compiler) {
        compiler = N00BB_LLVM_CLANG;
    }
    core_buffer_t target = {0};
    core_buffer_printf(&target, "--target=%s", triple);
    char *arguments[16] = {(char *)compiler, target.data, "-std=c11", "-O2", "-fPIC", "-c", "-emit-llvm",
                           "-I", (char *)directory};
    size_t next = 9;
    char *sysroot = sdk_root();
    if (sysroot) {
        arguments[next++] = "-isysroot";
        arguments[next++] = sysroot;
    }
    arguments[next++] = (char *)source;
    arguments[next++] = "-o";
    arguments[next++] = (char *)output;
    arguments[next] = nullptr;
    pid_t child = 0;
    int status = 0;
    bool ok = !posix_spawnp(&child, compiler, nullptr, nullptr, arguments, environ)
           && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    core_free(sysroot);
    core_buffer_release(&target);
    return ok;
}

static core_uint128_t
shim_key(const n00bb_ffi_t *ffi, const n00bb_native_cache_t *cache, const char *triple, size_t index)
{
    core_buffer_t payload = {0};
    core_buffer_printf(&payload, "n00bb-shim-v1");
    const ffi_output_t *shim = &ffi->shims.data[index];
    core_buffer_append(&payload, shim->name.data, shim->name.u8_bytes + 1);
    core_buffer_append(&payload, shim->contents.data, shim->contents.u8_bytes + 1);
    for (size_t i = 0; i < ffi->shims.len; i++) {
        if (ends_with(ffi->shims.data[i].name, ".h")) {
            core_buffer_append(&payload, ffi->shims.data[i].name.data, ffi->shims.data[i].name.u8_bytes + 1);
            core_buffer_append(&payload, ffi->shims.data[i].contents.data,
                               ffi->shims.data[i].contents.u8_bytes + 1);
        }
    }
    core_uint128_t key = n00bb_native_cache_key(cache, payload.data, payload.byte_len, triple, "shim", "", false,
                                                false);
    core_buffer_release(&payload);
    return key;
}

static bool
compile_uncached(const n00bb_ffi_t *ffi, const char *triple, size_t index, core_buffer_t *bitcode)
{
    char directory[] = "/tmp/n00bb-shims.XXXXXX";
    if (!mkdtemp(directory)) {
        return false;
    }
    core_list_t(core_string_t) written = core_list_new(core_string_t);
    bool ok = true;
    for (size_t i = 0; ok && i < ffi->shims.len; i++) {
        const ffi_output_t *shim = &ffi->shims.data[i];
        if (i != index && !ends_with(shim->name, ".h")) {
            continue;
        }
        core_buffer_t path = {0};
        core_buffer_printf(&path, "%s/%.*s", directory, (int)shim->name.u8_bytes, shim->name.data);
        ok = write_whole_file(path.data, shim->contents);
        core_list_push(written, core_string_dup(core_string_borrow(path.data, path.byte_len)));
        core_buffer_release(&path);
    }
    core_buffer_t source = {0}, output = {0};
    core_buffer_printf(&source, "%s/%.*s", directory, (int)ffi->shims.data[index].name.u8_bytes,
                       ffi->shims.data[index].name.data);
    core_buffer_printf(&output, "%s.bc", source.data);
    ok = ok && run_clang(triple, directory, source.data, output.data) && read_whole_file(output.data, bitcode);
    unlink(output.data);
    for (size_t i = 0; i < written.len; i++) {
        unlink(written.data[i].data);
        core_free((void *)written.data[i].data);
    }
    core_list_free(written);
    core_buffer_release(&source);
    core_buffer_release(&output);
    rmdir(directory);
    return ok;
}

bool
n00bb_ffi_compile_shims(n00bb_ffi_t *ffi, const char *triple, const char *executable_path,
                        core_list_t(core_buffer_t) *bitcode, core_diagnostics_t *diagnostics)
{
    if (!ffi || !triple || !bitcode) {
        return false;
    }
    n00bb_native_cache_t *cache = executable_path && !getenv("N00BB_NO_NATIVE_CACHE")
                                    ? n00bb_native_cache_open(executable_path)
                                    : nullptr;
    bool ok = true;
    for (size_t i = 0; ok && i < ffi->shims.len; i++) {
        if (!ends_with(ffi->shims.data[i].name, ".c")) {
            continue;
        }
        core_uint128_t key = cache ? shim_key(ffi, cache, triple, i) : 0;
        core_buffer_t compiled = {0};
        bool cached = cache && n00bb_native_cache_read(cache, key, &compiled);
        if (!cached) {
            ok = compile_uncached(ffi, triple, i, &compiled);
            if (ok && cache) {
                n00bb_native_cache_write(cache, key, compiled.data, compiled.byte_len);
            }
        }
        if (ok) {
            core_list_push(*bitcode, compiled);
        } else {
            core_buffer_release(&compiled);
            core_buffer_t message = {0};
            core_buffer_printf(&message, "could not compile the C shim %.*s",
                               (int)ffi->shims.data[i].name.u8_bytes, ffi->shims.data[i].name.data);
            core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0025"),
                                    core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
        }
    }
    n00bb_native_cache_close(cache);
    return ok;
}
