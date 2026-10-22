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
core_list_decl(size_t);

typedef struct {
    core_string_t              prefix;
    core_list_t(size_t)        shims;
    core_list_t(core_string_t) archives;
    core_list_t(core_string_t) libraries;
    core_list_t(core_string_t) frameworks;
    core_list_t(core_string_t) search_paths;
    bool                       exports;
    bool                       used;
} ffi_literal_t;

core_list_decl(ffi_literal_t);

struct n00bb_ffi {
    pthread_mutex_t                lock;
    writ_session_t                *session;
    writ_unit_t                   *profiles;
    core_list_t(core_string_t)     contract_dirs;
    core_list_t(core_string_t)     package_dirs;
    core_list_t(core_string_t)     inputs;
    core_list_t(ffi_output_t)      shims;
    core_list_t(core_string_t)     headers;
    core_list_t(core_string_t)     archives;
    core_list_t(core_string_t)     libraries;
    core_list_t(core_string_t)     found_libraries;
    core_list_t(core_string_t)     transports;
    core_list_t(core_string_t)     frameworks;
    core_list_t(core_string_t)     search_paths;
    size_t                         literals;
    core_list_t(ffi_literal_t)     records;
    core_list_t(size_t)            selected_shims;
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
        .headers       = core_list_new(core_string_t),
        .archives      = core_list_new(core_string_t),
        .libraries     = core_list_new(core_string_t),
        .found_libraries = core_list_new(core_string_t),
        .transports    = core_list_new(core_string_t),
        .frameworks    = core_list_new(core_string_t),
        .search_paths  = core_list_new(core_string_t),
        .records       = core_list_new(ffi_literal_t),
        .selected_shims = core_list_new(size_t),
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
    free_strings(&ffi->headers);
    free_strings(&ffi->archives);
    free_strings(&ffi->libraries);
    free_strings(&ffi->found_libraries);
    free_strings(&ffi->transports);
    free_strings(&ffi->frameworks);
    free_strings(&ffi->search_paths);
    for (size_t i = 0; i < ffi->records.len; i++) {
        ffi_literal_t *record = &ffi->records.data[i];
        core_free(record->prefix.data);
        core_list_free(record->shims);
        free_strings(&record->archives);
        free_strings(&record->libraries);
        free_strings(&record->frameworks);
        free_strings(&record->search_paths);
    }
    core_list_free(ffi->records);
    core_list_free(ffi->selected_shims);
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
    core_string_t profile = CORE_STRING_STATIC(N00BB_WRIT_PROFILE_DIR "/n00b.writ");
    ffi->profiles = writ_unit_new(ffi->session);
    if (writ_unit_add_file(ffi->profiles, as_writ(profile), nullptr) != WRIT_OK) {
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
keep_links(n00bb_ffi_t *ffi, ffi_literal_t *record, const writ_link_requirements_t *links)
{
    for (size_t i = 0; i < links->libraries_count; i++) {
        if (!contains(&ffi->transports, as_core(links->libraries[i]))) {
            push_unique(&record->libraries, links->libraries[i]);
        }
    }
    for (size_t i = 0; i < links->frameworks_count; i++) {
        push_unique(&record->frameworks, links->frameworks[i]);
    }
    for (size_t i = 0; i < links->search_paths_count; i++) {
        push_unique(&record->search_paths, links->search_paths[i]);
    }
}

static void
push_all(core_list_t(core_string_t) *list, const core_list_t(core_string_t) *values)
{
    for (size_t i = 0; i < values->len; i++) {
        push_unique(list, as_writ(values->data[i]));
    }
}

static void
clear_strings(core_list_t(core_string_t) *list)
{
    for (size_t i = 0; i < list->len; i++) {
        core_free((void *)list->data[i].data);
    }
    list->len = 0;
}

static void
refresh_selection(n00bb_ffi_t *ffi)
{
    clear_strings(&ffi->archives);
    clear_strings(&ffi->libraries);
    clear_strings(&ffi->frameworks);
    clear_strings(&ffi->search_paths);
    ffi->selected_shims.len = 0;
    bool hosted = !n00bb_native_root_hosted_hidden();
    for (size_t i = 0; i < ffi->records.len; i++) {
        const ffi_literal_t *record = &ffi->records.data[i];
        if (!record->used) {
            continue;
        }
        for (size_t j = 0; j < record->shims.len; j++) {
            size_t shim = record->shims.data[j];
            if (hosted || !core_string_ends_with(ffi->shims.data[shim].name,
                              CORE_STRING_STATIC("_n00b_export.c")))
                core_list_push(ffi->selected_shims, shim);
        }
        push_all(&ffi->archives, &record->archives);
        push_all(&ffi->libraries, &record->libraries);
        push_all(&ffi->frameworks, &record->frameworks);
        push_all(&ffi->search_paths, &record->search_paths);
    }
}

void
n00bb_ffi_select(n00bb_ffi_t *ffi, const core_string_t *symbols, size_t count)
{
    if (!ffi) {
        return;
    }
    bool hosted = !n00bb_native_root_hosted_hidden();
    core_dict_t by_prefix;
    core_dict_init_string(&by_prefix);
    for (size_t i = 0; i < ffi->records.len; i++) {
        ffi_literal_t *record = &ffi->records.data[i];
        record->used = record->exports && hosted;
        core_dict_put(&by_prefix, &record->prefix, record);
    }
    core_string_t stem = CORE_STRING_STATIC("n00b_ffi_");
    for (size_t j = 0; j < count; j++) {
        core_string_t symbol = symbols[j];
        if (!core_string_starts_with(symbol, stem)) continue;
        size_t end = stem.u8_bytes;
        while (end < symbol.u8_bytes && symbol.data[end] >= '0'
               && symbol.data[end] <= '9') end++;
        if (end == stem.u8_bytes || end >= symbol.u8_bytes
            || symbol.data[end] != '_') continue;
        core_string_t prefix = core_string_slice(symbol, 0, end);
        bool found = false;
        ffi_literal_t *record = core_dict_get(&by_prefix, &prefix, &found);
        if (found) record->used = true;
    }
    core_dict_free(&by_prefix);
    refresh_selection(ffi);
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
            .pure      = externs->items[i].pure,
        };
    }
    natives->count = externs->count;
    return true;
}

static bool
register_exports(const writ_n00b_externs_t *externs, core_string_t module)
{
    for (size_t i = 0; i < externs->exports_count; i++) {
        writ_str_t name = externs->exports[i].function;
        core_buffer_t function = {0};
        if (memchr(name.data, '.', name.length)) {
            core_buffer_printf(&function, "%.*s", (int)name.length, name.data);
        } else {
            core_buffer_printf(&function, "%.*s.%.*s", (int)module.u8_bytes, module.data, (int)name.length,
                               name.data);
        }
        core_string_t symbol = as_core(externs->exports[i].symbol);
        bool registered = function.data && symbol.data
                       && n00bb_native_root_register_hosted(core_buffer_string(&function), symbol);
        core_buffer_release(&function);
        if (!registered) {
            return false;
        }
    }
    return true;
}

extern char **environ;

static bool
write_whole_file(core_string_t path, core_string_t contents)
{
    char *native_path = core_string_to_cstr(path);
    FILE *file = native_path ? fopen(native_path, "wb") : nullptr;
    core_free(native_path);
    if (!file) {
        return false;
    }
    bool ok = fwrite(contents.data, 1, contents.u8_bytes, file) == contents.u8_bytes;
    return fclose(file) == 0 && ok;
}

static void
unlink_path(core_string_t path)
{
    char *native_path = core_string_to_cstr(path);
    if (native_path) {
        unlink(native_path);
    }
    core_free(native_path);
}

static bool
make_directory(core_string_t path)
{
    char *native_path = core_string_to_cstr(path);
    bool ok = native_path && (!mkdir(native_path, 0700) || errno == EEXIST);
    core_free(native_path);
    return ok;
}

static bool
read_whole_file(core_string_t path, core_buffer_t *out)
{
    char *native_path = core_string_to_cstr(path);
    FILE *file = native_path ? fopen(native_path, "rb") : nullptr;
    core_free(native_path);
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
report_error(core_translation_unit_t *unit, n00bb_literal_origin_t origin, core_string_t code, core_string_t message)
{
    core_diagnostics_report_at(core_translation_unit_diagnostics(unit), unit, CORE_DIAG_ERROR, code, message,
                               (core_diag_location_t){origin.line, origin.column, origin.line, origin.column, true});
}

static bool
run_logged(char **arguments, core_string_t log)
{
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        return false;
    }
    char *native_log = core_string_to_cstr(log.data ? log : CORE_STRING_STATIC("/dev/null"));
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, native_log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t child = 0;
    int status = 0;
    bool ok = !posix_spawnp(&child, arguments[0], &actions, nullptr, arguments, environ)
           && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    posix_spawn_file_actions_destroy(&actions);
    core_free(native_log);
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
    core_buffer_append(&output, directory, sizeof(directory) - 1);
    core_buffer_puts(&output, "/probe");
    char **arguments = core_alloc_array(char *, flags.len + 10);
    size_t next = 0;
    arguments[next++] = N00BB_NATIVE_LINKER;
    arguments[next++] = "-shared";
    arguments[next++] = "-x";
    arguments[next++] = "c";
    arguments[next++] = "/dev/null";
    size_t first_owned = next;
    for (size_t i = 0; i < flags.len; i++) {
        arguments[next++] = core_string_to_cstr(core_buffer_string(&flags.data[i]));
    }
    arguments[next++] = core_string_to_cstr(core_buffer_string(&named));
    size_t owned_end = next;
    char *native_output = core_string_to_cstr(core_buffer_string(&output));
    arguments[next++] = "-o";
    arguments[next++] = native_output;
    arguments[next] = nullptr;
    bool found = run_logged(arguments, (core_string_t){});
    unlink(native_output);
    rmdir(directory);
    for (size_t i = first_owned; i < owned_end; i++) {
        core_free(arguments[i]);
    }
    core_free(native_output);
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
            report_error(unit, origin, CORE_STRING_STATIC("N00BB-F0001"), core_buffer_string(&message));
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

void
n00bb_remove_tree(const char *path)
{
    if (path) nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static bool
write_crate(core_string_t directory, const capture_t *crate)
{
    bool ok = true;
    for (size_t i = 0; ok && i < crate->files.len; i++) {
        const ffi_output_t *file = &crate->files.data[i];
        core_buffer_t path = {0};
        core_buffer_append_string(&path, directory);
        core_buffer_putc(&path, '/');
        core_buffer_append_string(&path, file->name);
        for (size_t at = directory.u8_bytes + 1; ok && at < path.byte_len; at++) {
            if (path.data[at] == '/') {
                ok = make_directory(core_string_borrow(path.data, at));
            }
        }
        ok = ok && write_whole_file(core_buffer_string(&path), file->contents);
        core_buffer_release(&path);
    }
    return ok;
}

static core_string_t
staticlib_name(core_string_t directory)
{
    core_buffer_t release = {0};
    core_buffer_append_string(&release, directory);
    core_buffer_puts(&release, "/target/release");
    char *native_release = core_string_to_cstr(core_buffer_string(&release));
    DIR *listing = native_release ? opendir(native_release) : nullptr;
    core_free(native_release);
    core_buffer_release(&release);
    if (!listing) {
        return (core_string_t){};
    }
    core_string_t name = {};
    for (struct dirent *entry = readdir(listing); entry && !name.data; entry = readdir(listing)) {
        core_string_t candidate = core_string_from_cstr(entry->d_name);
        if (candidate.u8_bytes > 5 && core_string_starts_with(candidate, CORE_STRING_STATIC("lib"))
            && core_string_ends_with(candidate, CORE_STRING_STATIC(".a"))) {
            name = candidate;
        } else {
            core_free(candidate.data);
        }
    }
    closedir(listing);
    return name;
}

static bool
build_crate(core_string_t directory)
{
    core_buffer_t manifest = {0}, log = {0}, text = {0}, record = {0};
    core_buffer_append_string(&manifest, directory);
    core_buffer_puts(&manifest, "/Cargo.toml");
    core_buffer_append_string(&log, directory);
    core_buffer_puts(&log, "/cargo.log");
    char *native_manifest = core_string_to_cstr(core_buffer_string(&manifest));
    char *arguments[] = {N00BB_CARGO, "rustc", "--release", "--lib", "--crate-type", "staticlib", "--manifest-path",
                         native_manifest, "--", "--print", "native-static-libs", nullptr};
    bool ok = run_logged(arguments, core_buffer_string(&log)) && read_whole_file(core_buffer_string(&log), &text);
    core_free(native_manifest);
    core_string_t output = core_buffer_string(&text);
    core_string_t marker = CORE_STRING_STATIC("native-static-libs:");
    int64_t found = ok ? core_string_find(output, marker) : -1;
    core_string_t archive = ok ? staticlib_name(directory) : (core_string_t){};
    ok = found >= 0 && archive.data;
    if (ok) {
        size_t start = (size_t)found + marker.u8_bytes;
        size_t stop = start;
        while (stop < output.u8_bytes && output.data[stop] && output.data[stop] != '\r' && output.data[stop] != '\n') {
            stop++;
        }
        core_buffer_append_string(&record, archive);
        core_buffer_putc(&record, '\n');
        core_buffer_append(&record, output.data + start, stop - start);
        core_buffer_putc(&record, '\n');
        core_buffer_t path = {0};
        core_buffer_append_string(&path, directory);
        core_buffer_puts(&path, "/link");
        ok = write_whole_file(core_buffer_string(&path), core_buffer_string(&record));
        core_buffer_release(&path);
    }
    core_free(archive.data);
    core_buffer_release(&manifest);
    core_buffer_release(&log);
    core_buffer_release(&text);
    core_buffer_release(&record);
    return ok;
}

static bool
is_link_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool
keep_crate(n00bb_ffi_t *ffi, ffi_literal_t *literal, core_string_t directory)
{
    core_buffer_t path = {0}, record = {0};
    core_buffer_append_string(&path, directory);
    core_buffer_puts(&path, "/link");
    bool ok = read_whole_file(core_buffer_string(&path), &record);
    core_buffer_release(&path);
    core_string_t text = core_buffer_string(&record);
    int64_t newline = ok ? core_string_find_byte(text, '\n') : -1;
    ok = newline >= 0;
    if (ok) {
        core_string_t name = core_string_slice(text, 0, (size_t)newline);
        int64_t terminator = core_string_find_byte(name, '\0');
        if (terminator >= 0) {
            name.u8_bytes = (size_t)terminator;
        }
        core_string_t flags = core_string_slice(text, (size_t)newline + 1, text.u8_bytes - (size_t)newline - 1);
        int64_t flags_end = core_string_find_byte(flags, '\0');
        if (flags_end >= 0) {
            flags.u8_bytes = (size_t)flags_end;
        }
        core_buffer_t archive = {0};
        core_buffer_append_string(&archive, directory);
        core_buffer_puts(&archive, "/target/release/");
        core_buffer_append_string(&archive, name);
        push_unique(&literal->archives, as_writ(core_buffer_string(&archive)));
        core_buffer_release(&archive);
        push_unique(&ffi->transports, as_writ(core_string_slice(name, 3, name.u8_bytes - 5)));
        bool framework = false;
        size_t at = 0;
        while (at < flags.u8_bytes) {
            while (at < flags.u8_bytes && is_link_space(flags.data[at])) {
                at++;
            }
            size_t start = at;
            while (at < flags.u8_bytes && !is_link_space(flags.data[at])) {
                at++;
            }
            if (at == start) {
                break;
            }
            core_string_t token = core_string_slice(flags, start, at - start);
            if (framework) {
                push_unique(&literal->frameworks, as_writ(token));
                framework = false;
            } else if (core_string_eq(token, CORE_STRING_STATIC("-framework"))) {
                framework = true;
            } else if (token.u8_bytes > 2 && core_string_starts_with(token, CORE_STRING_STATIC("-l"))) {
                push_unique(&literal->libraries, as_writ(core_string_slice(token, 2, token.u8_bytes - 2)));
            } else if (token.u8_bytes > 2 && core_string_starts_with(token, CORE_STRING_STATIC("-L"))) {
                push_unique(&literal->search_paths, as_writ(core_string_slice(token, 2, token.u8_bytes - 2)));
            }
        }
    }
    core_buffer_release(&record);
    return ok;
}

static bool
keep_transport(n00bb_ffi_t *ffi, ffi_literal_t *record, core_translation_unit_t *unit,
               n00bb_literal_origin_t origin, const writ_model_t *model)
{
    capture_t crate = {core_list_new(ffi_output_t)};
    writ_gen_rust_request_t request = {.struct_size = sizeof(request)};
    writ_output_t output = {capture_file, &crate};
    writ_status_t status = writ_gen_rust(model, &request, &output);
    bool ok = status == WRIT_OK || status == WRIT_INVALID_ARGUMENT;
    core_string_t root = {};
    if (ok && status == WRIT_OK && !*N00BB_CARGO) {
        report_error(unit, origin, CORE_STRING_STATIC("N00BB-F0002"),
                     CORE_STRING_STATIC("this 'ffi literal needs a Rust transport crate, but this n00bb was built without Rust transports"));
        ok = false;
    } else if (ok && status == WRIT_OK && !(root = n00bb_native_cache_root()).data) {
        report_error(unit, origin, CORE_STRING_STATIC("N00BB-F0003"),
                     CORE_STRING_STATIC("no cache directory is available for the Rust transport crate"));
        ok = false;
    } else if (ok && status == WRIT_OK) {
        core_buffer_t payload = {0}, directory = {0};
        for (size_t i = 0; i < crate.files.len; i++) {
            core_string_t name = crate.files.data[i].name;
            core_string_t contents = crate.files.data[i].contents;
            core_buffer_append_string(&payload, name);
            if (name.data) {
                core_buffer_append(&payload, "", 1);
            }
            core_buffer_append_string(&payload, contents);
            if (contents.data) {
                core_buffer_append(&payload, "", 1);
            }
        }
        core_uint128_t key = core_xxh3_128bits_raw(payload.data, payload.byte_len);
        core_buffer_append_string(&directory, root);
        core_buffer_puts(&directory, "/rust");
        ok = make_directory(core_buffer_string(&directory));
        core_buffer_printf(&directory, "/%016llx%016llx", (unsigned long long)(key >> 64), (unsigned long long)key);
        if (ok && !keep_crate(ffi, record, core_buffer_string(&directory))) {
            core_buffer_t staging = {0};
            core_buffer_append_string(&staging, core_buffer_string(&directory));
            core_buffer_puts(&staging, ".XXXXXX");
            char *native_staging = core_string_to_cstr(core_buffer_string(&staging));
            ok = native_staging && mkdtemp(native_staging);
            if (ok) {
                memcpy(staging.data, native_staging, staging.byte_len);
            }
            ok = ok && write_crate(core_buffer_string(&staging), &crate);
            bool built = ok && build_crate(core_buffer_string(&staging));
            char *native_directory = core_string_to_cstr(core_buffer_string(&directory));
            if (built && rename(native_staging, native_directory)) {
                n00bb_remove_tree(native_staging);
            }
            core_free(native_directory);
            core_free(native_staging);
            if (ok && !built) {
                core_buffer_t message = {0};
                core_buffer_puts(&message, "building the Rust transport crate failed; cargo output is in ");
                core_buffer_append_string(&message, core_buffer_string(&staging));
                core_buffer_puts(&message, "/cargo.log");
                report_error(unit, origin, CORE_STRING_STATIC("N00BB-F0004"), core_buffer_string(&message));
                core_buffer_release(&message);
            }
            ok = built && keep_crate(ffi, record, core_buffer_string(&directory));
            core_buffer_release(&staging);
        }
        core_buffer_release(&payload);
        core_buffer_release(&directory);
    }
    core_free(root.data);
    for (size_t i = 0; i < crate.files.len; i++) {
        core_free((void *)crate.files.data[i].name.data);
        core_free((void *)crate.files.data[i].contents.data);
    }
    core_list_free(crate.files);
    return ok;
}

static const char *
last_of(const char *start, const char *stop, char wanted)
{
    while (stop > start && stop[-1] != wanted) {
        stop--;
    }
    return stop > start ? stop - 1 : nullptr;
}

static bool
keep_origins(core_string_t map, n00bb_line_origins_t *origins)
{
    size_t lines = 0;
    for (size_t i = 0; i < map.u8_bytes; i++) {
        lines += map.data[i] == '\n';
    }
    origins->items = lines ? core_alloc_array(n00bb_line_origin_t, lines) : nullptr;
    if (lines && !origins->items) {
        return false;
    }
    const char *cursor = map.data;
    const char *end = map.data + map.u8_bytes;
    while (cursor < end) {
        const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        const char *stop = newline ? newline : end;
        const char *place = memchr(cursor, '\t', (size_t)(stop - cursor));
        const char *name = place ? last_of(place + 1, stop, '\t') : nullptr;
        const char *column = name ? last_of(place + 1, name, ':') : nullptr;
        const char *line = column ? last_of(place + 1, column, ':') : nullptr;
        if (line) {
            origins->items[origins->count++] = (n00bb_line_origin_t){
                .line          = (uint32_t)core_string_strtoull(
                    core_string_borrow(cursor, (size_t)(stop - cursor)), 10),
                .path          = core_string_dup(core_string_borrow(place + 1, (size_t)(line - place - 1))),
                .origin_line   = (uint32_t)core_string_strtoull(
                    core_string_borrow(line + 1, (size_t)(stop - line - 1)), 10),
                .origin_column = (uint32_t)core_string_strtoull(
                    core_string_borrow(column + 1, (size_t)(stop - column - 1)), 10),
            };
        }
        cursor = stop + 1;
    }
    return true;
}

static bool
generate_locked(n00bb_ffi_t *ffi, core_translation_unit_t *unit, core_string_t name, core_string_t module,
                n00bb_literal_origin_t origin, core_string_t text, core_buffer_t *source,
                n00bb_native_declarations_t *natives, n00bb_line_origins_t *origins)
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
    ffi_literal_t record = {
        .shims        = core_list_new(size_t),
        .archives     = core_list_new(core_string_t),
        .libraries    = core_list_new(core_string_t),
        .frameworks   = core_list_new(core_string_t),
        .search_paths = core_list_new(core_string_t),
        .used         = true,
    };
    if (ok) {
        core_buffer_t prefix = {0};
        core_buffer_printf(&prefix, "n00b_ffi_%zu", ffi->literals++);
        record.prefix = core_string_dup(core_buffer_string(&prefix));
        writ_gen_n00b_request_t request = {.struct_size = sizeof(request), .binding = binding,
                                           .module_name = as_writ(name),
                                           .shim_prefix = (writ_str_t){prefix.data, prefix.byte_len},
                                           .type_namespace = as_writ(module)};
        writ_output_t output = {capture_file, &capture};
        writ_n00b_externs_t externs = {0};
        ok = writ_gen_n00b(&request, &output, &links, &externs) == WRIT_OK && keep_externs(&externs, natives)
          && register_exports(&externs, module);
        record.exports = externs.exports_count != 0;
        if (ok && externs.header.length) {
            push_unique(&ffi->headers, externs.header);
        }
        core_buffer_release(&prefix);
    }
    for (size_t i = 0; ok && i < capture.files.len; i++) {
        ffi_output_t *file = &capture.files.data[i];
        if (core_string_ends_with(file->name, CORE_STRING_STATIC(".n"))) {
            core_buffer_append(source, file->contents.data, file->contents.u8_bytes);
        } else if (core_string_ends_with(file->name, CORE_STRING_STATIC(".map"))) {
            ok = keep_origins(file->contents, origins);
        } else {
            core_list_push(record.shims, ffi->shims.len);
            core_list_push(ffi->shims, *file);
            *file = (ffi_output_t){0};
        }
    }
    for (size_t i = 0; i < capture.files.len; i++) {
        core_free((void *)capture.files.data[i].name.data);
        core_free((void *)capture.files.data[i].contents.data);
    }
    core_list_free(capture.files);
    ok = ok && keep_transport(ffi, &record, unit, origin, model);
    if (ok) {
        keep_links(ffi, &record, &links);
        ok = check_libraries(ffi, unit, origin, &links);
    }
    if (record.prefix.data) {
        core_list_push(ffi->records, record);
        refresh_selection(ffi);
    } else {
        core_list_free(record.shims);
        free_strings(&record.archives);
        free_strings(&record.libraries);
        free_strings(&record.frameworks);
        free_strings(&record.search_paths);
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
                   n00bb_native_declarations_t *natives, n00bb_line_origins_t *origins)
{
    n00bb_ffi_t *ffi = context;
    if (!ffi || !unit || !source || !natives || !origins) {
        return false;
    }
    pthread_mutex_lock(&ffi->lock);
    bool ok = open_session(ffi);
    if (ok) {
        add_package_contracts(ffi, unit);
        ok = generate_locked(ffi, unit, name, module, origin, text, source, natives, origins);
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

bool
n00bb_ffi_write_headers(const n00bb_ffi_t *ffi, core_string_t output)
{
    size_t cut = output.u8_bytes;
    while (cut && output.data[cut - 1] != '/') {
        cut--;
    }
    bool ok = true;
    for (size_t h = 0; ffi && ok && h < ffi->headers.len; h++) {
        core_string_t header = ffi->headers.data[h];
        for (size_t i = 0; i < ffi->shims.len; i++) {
            if (!core_string_eq(ffi->shims.data[i].name, header)) {
                continue;
            }
            core_buffer_t path = {0};
            core_buffer_printf(&path, "%.*s%.*s", (int)cut, output.data, (int)header.u8_bytes, header.data);
            ok = path.data && write_whole_file(core_buffer_string(&path), ffi->shims.data[i].contents);
            core_buffer_release(&path);
            break;
        }
    }
    return ok;
}

size_t
n00bb_ffi_shim_count(const n00bb_ffi_t *ffi)
{
    return ffi ? ffi->selected_shims.len : 0;
}

n00bb_ffi_file_t
n00bb_ffi_shim_at(const n00bb_ffi_t *ffi, size_t index)
{
    const ffi_output_t *shim = &ffi->shims.data[ffi->selected_shims.data[index]];
    return (n00bb_ffi_file_t){shim->name, shim->contents};
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

static core_string_t
sdk_root(void)
{
#ifdef __APPLE__
    core_string_t configured = core_string_from_cstr(getenv("SDKROOT"));
    if (configured.u8_bytes) {
        return configured;
    }
    core_free(configured.data);
    FILE *query = popen("/usr/bin/xcrun --show-sdk-path 2>/dev/null", "r");
    if (!query) {
        return (core_string_t){};
    }
    char path[4096] = {0};
    bool read = fgets(path, sizeof path, query) != nullptr;
    pclose(query);
    core_string_t result = read ? core_string_from_cstr(path) : (core_string_t){};
    while (result.u8_bytes
           && (result.data[result.u8_bytes - 1] == '\n' || result.data[result.u8_bytes - 1] == '\r')) {
        result.data[--result.u8_bytes] = '\0';
    }
    if (!result.u8_bytes) {
        core_free(result.data);
        return (core_string_t){};
    }
    return result;
#else
    return (core_string_t){};
#endif
}

static void
append_payload_text(core_buffer_t *payload, core_string_t text)
{
    core_buffer_append_string(payload, text);
    if (text.data) {
        core_buffer_append(payload, "", 1);
    }
}

static bool
run_clang(const n00bb_ffi_t *ffi, core_string_t triple, core_string_t directory, core_string_t source,
          core_string_t output)
{
    const char *compiler = getenv("N00BB_CC");
    if (!compiler) {
        compiler = N00BB_LLVM_CLANG;
    }
    core_buffer_t target = {0};
    core_buffer_puts(&target, "--target=");
    core_buffer_append_string(&target, triple);
    size_t owned_count = ffi->contract_dirs.len + 5;
    char **owned = core_alloc_array(char *, owned_count);
    char **arguments = core_alloc_array(char *, 16 + 2 * ffi->contract_dirs.len);
    if (!arguments || !owned) {
        core_free(arguments);
        core_free(owned);
        core_buffer_release(&target);
        return false;
    }
    size_t held = 0;
    char *native_directory = owned[held++] = core_string_to_cstr(directory);
    char *native_target = owned[held++] = core_string_to_cstr(core_buffer_string(&target));
    char *fixed[] = {(char *)compiler, native_target, "-std=c11", "-O2", "-fPIC", "-c", "-emit-llvm",
                     "-I", native_directory};
    size_t next = 0;
    for (; next < sizeof(fixed) / sizeof(fixed[0]); next++) {
        arguments[next] = fixed[next];
    }
    for (size_t i = 0; i < ffi->contract_dirs.len; i++) {
        arguments[next++] = "-I";
        arguments[next++] = owned[held++] = core_string_to_cstr(ffi->contract_dirs.data[i]);
    }
    core_string_t sysroot = sdk_root();
    if (sysroot.data) {
        arguments[next++] = "-isysroot";
        arguments[next++] = owned[held++] = core_string_to_cstr(sysroot);
    }
    arguments[next++] = owned[held++] = core_string_to_cstr(source);
    arguments[next++] = "-o";
    arguments[next++] = owned[held++] = core_string_to_cstr(output);
    arguments[next] = nullptr;
    pid_t child = 0;
    int status = 0;
    bool ok = !posix_spawnp(&child, compiler, nullptr, nullptr, arguments, environ)
           && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    for (size_t i = 0; i < held; i++) {
        core_free(owned[i]);
    }
    core_free(owned);
    core_free(sysroot.data);
    core_free(arguments);
    core_buffer_release(&target);
    return ok;
}

static core_uint128_t
shim_key(const n00bb_ffi_t *ffi, const n00bb_native_cache_t *cache, core_string_t triple, size_t index)
{
    core_buffer_t payload = {0};
    core_buffer_puts(&payload, "n00bb-shim-v1");
    const ffi_output_t *shim = &ffi->shims.data[index];
    append_payload_text(&payload, shim->name);
    append_payload_text(&payload, shim->contents);
    for (size_t i = 0; i < ffi->contract_dirs.len; i++) {
        append_payload_text(&payload, ffi->contract_dirs.data[i]);
    }
    for (size_t i = 0; i < ffi->shims.len; i++) {
        if (core_string_ends_with(ffi->shims.data[i].name, CORE_STRING_STATIC(".h"))) {
            append_payload_text(&payload, ffi->shims.data[i].name);
            append_payload_text(&payload, ffi->shims.data[i].contents);
        }
    }
    core_uint128_t key = n00bb_native_cache_key(cache, payload.data, payload.byte_len, triple,
                                                CORE_STRING_STATIC("shim"), CORE_STRING_STATIC(""), false, false);
    core_buffer_release(&payload);
    return key;
}

static bool
compile_uncached(const n00bb_ffi_t *ffi, core_string_t triple, size_t index, core_buffer_t *bitcode)
{
    char directory[] = "/tmp/n00bb-shims.XXXXXX";
    if (!mkdtemp(directory)) {
        return false;
    }
    core_string_t directory_text = core_string_borrow(directory, sizeof(directory) - 1);
    core_list_t(core_buffer_t) written = core_list_new(core_buffer_t);
    bool ok = true;
    for (size_t i = 0; ok && i < ffi->shims.len; i++) {
        const ffi_output_t *shim = &ffi->shims.data[i];
        if (i != index && !core_string_ends_with(shim->name, CORE_STRING_STATIC(".h"))) {
            continue;
        }
        core_buffer_t path = {0};
        core_buffer_append_string(&path, directory_text);
        core_buffer_putc(&path, '/');
        core_buffer_append_string(&path, shim->name);
        ok = write_whole_file(core_buffer_string(&path), shim->contents);
        core_list_push(written, path);
    }
    core_buffer_t source = {0}, output = {0};
    core_buffer_append_string(&source, directory_text);
    core_buffer_putc(&source, '/');
    core_buffer_append_string(&source, ffi->shims.data[index].name);
    core_buffer_append_string(&output, core_buffer_string(&source));
    core_buffer_puts(&output, ".bc");
    ok = ok
      && run_clang(ffi, triple, directory_text, core_buffer_string(&source), core_buffer_string(&output))
      && read_whole_file(core_buffer_string(&output), bitcode);
    unlink_path(core_buffer_string(&output));
    for (size_t i = 0; i < written.len; i++) {
        unlink_path(core_buffer_string(&written.data[i]));
        core_buffer_release(&written.data[i]);
    }
    core_list_free(written);
    core_buffer_release(&source);
    core_buffer_release(&output);
    rmdir(directory);
    return ok;
}

bool
n00bb_ffi_compile_shims(n00bb_ffi_t *ffi, core_string_t triple, core_string_t executable_path,
                        core_list_t(core_buffer_t) *bitcode, core_diagnostics_t *diagnostics)
{
    if (!ffi || !triple.data || !bitcode) {
        return false;
    }
    n00bb_native_cache_t *cache = executable_path.data && !getenv("N00BB_NO_NATIVE_CACHE")
                                    ? n00bb_native_cache_open(executable_path)
                                    : nullptr;
    bool ok = true;
    for (size_t selected = 0; ok && selected < ffi->selected_shims.len; selected++) {
        size_t i = ffi->selected_shims.data[selected];
        if (!core_string_ends_with(ffi->shims.data[i].name, CORE_STRING_STATIC(".c"))) {
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
