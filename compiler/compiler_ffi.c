#include "compiler_ffi.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compiler_native_roots.h"
#include "compiler_phases.h"
#include "config.h"
#include "llvm_native_cache.h"
#include "lib/alloc.h"
#include "diag/diagnostics.h"
#include "lib/hash.h"
#include "lib/os.h"
#include "lib/process.h"
#include "lib/list.h"
#include "lib/string.h"
#include "diagnostics.h"
#include "driver.h"
#include "writ/gen.h"

typedef struct {
    core_string_t name;
    core_string_t contents;
} ffi_output_t;

core_list_decl(ffi_output_t);
core_list_decl(size_t);
core_list_decl(n00bb_ffi_symbol_t);

static pthread_mutex_t declared_symbols_lock = PTHREAD_MUTEX_INITIALIZER;
static core_list_t(n00bb_ffi_symbol_t) declared_symbols;

static void
remember_declared_symbols(const writ_n00b_externs_t *externs, core_translation_unit_t *unit,
                          core_string_t literal, n00bb_literal_origin_t origin)
{
    pthread_mutex_lock(&declared_symbols_lock);
    if (!declared_symbols.data) {
        declared_symbols = core_list_new(n00bb_ffi_symbol_t);
    }
    for (size_t i = 0; i < externs->count; i++) {
        core_string_t function = core_string_borrow(externs->items[i].name.data, externs->items[i].name.length);
        if (core_string_starts_with(function, CORE_STRING_STATIC("__"))) {
            function = core_string_slice(function, 2, function.u8_bytes - 2);
        }
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%.*s.%.*s", (int)literal.u8_bytes, literal.data, (int)function.u8_bytes,
                           function.data);
        core_list_push(declared_symbols, ((n00bb_ffi_symbol_t){
                                             .symbol = core_string_dup(core_string_borrow(
                                                 externs->items[i].symbol.data, externs->items[i].symbol.length)),
                                             .name   = core_string_dup(core_buffer_string(&name)),
                                             .unit   = unit,
                                             .line   = externs->items[i].declared_at.line
                                                           ? externs->items[i].declared_at.line : origin.line,
                                             .column = externs->items[i].declared_at.line
                                                           ? externs->items[i].declared_at.column : origin.column,
                                         }));
        core_buffer_release(&name);
    }
    pthread_mutex_unlock(&declared_symbols_lock);
}

bool
n00bb_ffi_declared_symbol(core_string_t symbol, n00bb_ffi_symbol_t *out)
{
    bool found = false;
    pthread_mutex_lock(&declared_symbols_lock);
    for (size_t i = 0; !found && i < declared_symbols.len; i++) {
        core_string_t declared = declared_symbols.data[i].symbol;
        bool shim_for_symbol = declared.u8_bytes > symbol.u8_bytes
                            && core_string_ends_with(declared, symbol)
                            && declared.data[declared.u8_bytes - symbol.u8_bytes - 1] == '_';
        if (core_string_eq(declared, symbol) || shim_for_symbol) {
            *out = declared_symbols.data[i];
            found = true;
        }
    }
    pthread_mutex_unlock(&declared_symbols_lock);
    return found;
}

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
    const n00bb_toolchain_t *toolchain = n00bb_toolchain();
    writ_session_add_contract_dir(ffi->session, as_writ(toolchain->writ_contracts));
    core_string_t profile = core_os_path_join(toolchain->writ_profiles,
                                            CORE_STRING_STATIC("n00b.writ"));
    ffi->profiles = writ_unit_new(ffi->session);
    if (writ_unit_add_file(ffi->profiles, as_writ(profile), nullptr) != WRIT_OK) {
        ffi->profiles = nullptr;
    }
    core_free(profile.data);
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

static const n00bb_diag_t writ_codes[] = {
#define WRIT_DIAG(id, code, severity, text) N00B_DIAG_WRIT_##id,
#include "writ/diag_codes.def"
#undef WRIT_DIAG
};

static const n00bb_text_t writ_texts[] = {
#define WRIT_TEXT(id, text) N00B_TEXT_WRIT_##id,
#include "writ/diag_texts.def"
#undef WRIT_TEXT
};

static core_string_t
writ_text(core_text_message_t message)
{
    if (message.text >= WRIT_TEXT_COUNT) return (core_string_t){0};
    core_text_arg_t *args = core_alloc_array(core_text_arg_t, message.args.count);
    for (size_t i = 0; i < message.args.count; i++) {
        args[i] = message.args.at[i];
        if (args[i].kind == CORE_TEXT_ARG_PHRASE && args[i].phrase < WRIT_TEXT_COUNT)
            args[i].phrase = writ_texts[args[i].phrase];
    }
    core_string_t text = n00bb_text(writ_texts[message.text], (n00bb_args_t){args, message.args.count});
    core_free(args);
    return text;
}

static core_diag_location_t
writ_location(writ_span_t span)
{
    if (!span.line) return (core_diag_location_t){0};
    uint64_t width = span.end_byte > span.start_byte ? span.end_byte - span.start_byte : 1;
    return (core_diag_location_t){
        .line         = span.line,
        .column       = span.column,
        .end_line     = span.line,
        .end_column   = span.column + (uint32_t)width,
        .has_location = true,
    };
}

static void
forward_diagnostics(n00bb_ffi_t *ffi, core_translation_unit_t *unit, size_t first, n00bb_literal_origin_t origin)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    for (size_t i = first; i < writ_diag_count(ffi->session); i++) {
        const writ_diag_t *diag = writ_diag_at(ffi->session, i);
        core_diag_location_t location = writ_location(diag->primary.span);
        if (!location.has_location) {
            location = (core_diag_location_t){origin.line, origin.column, origin.line, origin.column, true};
        }
        size_t index = core_diagnostics_count(diagnostics);
        if (diag->code_id >= WRIT_D_COUNT) {
            n00bb_internal_error(diagnostics, unit, location, "checking an 'ffi contract",
                                 "%.*s (%.*s)", (int)diag->message.length, diag->message.data,
                                 (int)diag->code.length, diag->code.data);
            continue;
        }
        n00bb_diag_t id = writ_codes[diag->code_id];
        core_string_t detail = writ_text(diag->detail);
        bool reported = detail.data
            ? n00bb_report(diagnostics, unit, id, location, N00B_TEXT_WRIT_TITLE_DETAIL,
                           N00BB_ARGS(ARG_TEXT(n00bb_diag_title(id)), ARG_TEXT(detail)))
            : n00bb_report(diagnostics, unit, id, location, N00B_TEXT_TEXT,
                           N00BB_ARGS(ARG_TEXT(n00bb_diag_title(id))));
        core_free(detail.data);
        if (!reported) continue;
        if (diag->path.length && core_diagnostics_count(diagnostics) > index) {
            core_diagnostic_set_path(diagnostics, index, as_core(diag->path));
        }
        core_string_t label = writ_text(diag->primary.text);
        core_diag_label(diagnostics, label);
        core_free(label.data);
        for (size_t s = 0; s < diag->secondary_count; s++) {
            label = writ_text(diag->secondary[s].text);
            core_diag_secondary_path(diagnostics, diag->path.length ? as_core(diag->path) : (core_string_t){},
                                     writ_location(diag->secondary[s].span), label);
            core_free(label.data);
        }
        for (size_t n = 0; n < diag->note_count; n++) {
            label = writ_text(diag->notes[n].text);
            core_diag_note(diagnostics, label);
            core_free(label.data);
        }
        for (size_t s = 0; s < diag->suggestion_count; s++) {
            const writ_diag_suggestion_t *suggestion = &diag->suggestions[s];
            label = writ_text(suggestion->text);
            if (label.data)
                n00bb_report_help(diagnostics, N00B_TEXT_TEXT_2,
                    N00BB_ARGS(ARG_TEXT(label), ARG_TEXT(as_core(suggestion->replacement))));
            else
                n00bb_report_help(diagnostics, N00B_TEXT_WRITE,
                    N00BB_ARGS(ARG_TEXT(as_core(suggestion->replacement))));
            core_free(label.data);
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


static bool
write_whole_file(core_string_t path, core_string_t contents)
{
    return core_os_write_file_atomic(path, contents.data, contents.u8_bytes);
}

static void
unlink_path(core_string_t path)
{
    core_os_remove_file(path);
}

static core_string_t
temporary_directory(const char *prefix)
{
    core_string_t root = core_os_temp_root();
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, root);
    core_buffer_printf(&stem, "/%s", prefix);
    core_string_t directory = core_os_make_unique_directory(core_buffer_string(&stem));
    core_buffer_release(&stem);
    core_free(root.data);
    return directory;
}

static void
remove_directory(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    if (native) rmdir(native);
    core_free(native);
    core_free(path.data);
}

static bool
make_directory(core_string_t path)
{
    return core_os_make_directory(path, 0700);
}

static bool
read_whole_file(core_string_t path, core_buffer_t *out)
{
    char *bytes = nullptr;
    size_t size = 0;
    if (!core_os_read_file(path, &bytes, &size)) return false;
    core_buffer_append(out, bytes, size);
    core_free(bytes);
    return out->byte_len != 0;
}

static void
report_error(core_translation_unit_t *unit, n00bb_literal_origin_t origin,
             n00bb_diag_t id, n00bb_text_t message, n00bb_args_t args)
{
    n00bb_report(core_translation_unit_diagnostics(unit), unit, id,
        (core_diag_location_t){origin.line, origin.column, origin.line,
                               origin.column, true},
        message, args);
}

static bool
run_logged(char **arguments, core_string_t log)
{
    if (!arguments[0]) {
        return false;
    }
    core_buffer_t output = {0};
    core_buffer_t errors = {0};
    core_process_status_t status = {0};
    bool ok = !core_process_capture(arguments, nullptr, 0, &output, &errors, &status)
           && status.exited && status.code == 0;
    if (log.data) {
        core_buffer_append(&output, errors.data, errors.byte_len);
        core_os_write_file_atomic(log, output.data, output.byte_len);
    }
    core_buffer_release(&output);
    core_buffer_release(&errors);
    return ok;
}

static bool
library_found(n00bb_ffi_t *ffi, core_string_t library)
{
    if (contains(&ffi->found_libraries, library) || contains(&ffi->transports, library)) {
        return true;
    }
    core_string_t directory = temporary_directory("n00bb-probe.");
    if (!directory.data) {
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
    core_buffer_append_string(&output, directory);
    core_buffer_puts(&output, "/probe");
    core_buffer_t source = {0};
    core_buffer_append_string(&source, directory);
    core_buffer_puts(&source, "/probe.c");
    bool empty = core_os_write_file_atomic(core_buffer_string(&source), "", 0);
    char *native_source = core_string_to_cstr(core_buffer_string(&source));
    core_buffer_release(&source);
    char **arguments = core_alloc_array(char *, flags.len + 10);
    size_t next = 0;
    arguments[next++] = n00bb_toolchain()->linker;
    arguments[next++] = "-shared";
    arguments[next++] = native_source;
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
    bool found = empty && run_logged(arguments, (core_string_t){});
    unlink(native_output);
    if (native_source) unlink(native_source);
    core_free(native_source);
    remove_directory(directory);
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
            report_error(unit, origin, N00B_DIAG_NATIVE_LIBRARY_MISSING,
                         N00B_TEXT_FFI_LIBRARY_NOT_FOUND,
                         N00BB_ARGS(ARG_TEXT(library)));
            core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
            if (ffi->search_paths.len)
                n00bb_report_note(diagnostics,
                    N00B_TEXT_SEARCHED_THE_SYSTEM_LIBRARY_PATHS,
                    N00BB_ARGS(ARG_NAMES(ffi->search_paths.data,
                                         ffi->search_paths.len)));
            n00bb_report_help(diagnostics, N00B_TEXT_INSTALL_LIB_OR_ADD_THE,
                N00BB_ARGS(ARG_TEXT(library)));
            ok = false;
        }
    }
    return ok;
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
    char *arguments[] = {n00bb_toolchain()->cargo, "rustc", "--release", "--lib", "--crate-type", "staticlib", "--manifest-path",
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
    if (ok && status == WRIT_OK && !n00bb_toolchain()->cargo) {
        report_error(unit, origin, N00B_DIAG_RUST_TRANSPORT_UNAVAILABLE,
                     N00B_TEXT_FFI_NEEDS_RUST, N00BB_NO_ARGS);
        n00bb_report_help(core_translation_unit_diagnostics(unit),
            N00B_TEXT_REBUILD_N00B_WITH_N00B_RUST,
            N00BB_NO_ARGS);
        ok = false;
    } else if (ok && status == WRIT_OK && !(root = n00bb_native_cache_root()).data) {
        report_error(unit, origin, N00B_DIAG_TRANSPORT_CACHE_UNAVAILABLE,
                     N00B_TEXT_FFI_NO_CACHE_FOR_RUST, N00BB_NO_ARGS);
        n00bb_report_help(core_translation_unit_diagnostics(unit),
            N00B_TEXT_SET_N00B_CACHE_TO_A_2,
            N00BB_NO_ARGS);
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
        int directory_error = ok ? 0 : errno;
        core_buffer_printf(&directory, "/%016llx%016llx", (unsigned long long)(key >> 64), (unsigned long long)key);
        if (ok && !keep_crate(ffi, record, core_buffer_string(&directory))) {
            core_buffer_t staging = {0};
            core_buffer_append_string(&staging, core_buffer_string(&directory));
            core_buffer_puts(&staging, ".");
            core_string_t made = core_os_make_unique_directory(core_buffer_string(&staging));
            ok = made.data != nullptr;
            core_buffer_release(&staging);
            core_buffer_append_string(&staging, made);
            core_free(made.data);
            ok = ok && write_crate(core_buffer_string(&staging), &crate);
            if (!ok) {
                directory_error = errno ? errno : EIO;
            }
            bool built = ok && build_crate(core_buffer_string(&staging));
            if (built && !core_os_rename_replace(core_buffer_string(&staging),
                                                 core_buffer_string(&directory)))
                core_os_remove_tree(core_buffer_string(&staging), nullptr);
            if (ok && !built) {
                report_error(unit, origin, N00B_DIAG_TRANSPORT_BUILD_FAILED,
                             N00B_TEXT_FFI_CARGO_FAILED, N00BB_NO_ARGS);
                n00bb_report_note(core_translation_unit_diagnostics(unit),
                    N00B_TEXT_CARGO_S_OUTPUT_IS_IN,
                    N00BB_ARGS(ARG_TEXT(core_buffer_string(&staging))));
            }
            ok = built && keep_crate(ffi, record, core_buffer_string(&directory));
            core_buffer_release(&staging);
        }
        if (directory_error) {
            report_error(unit, origin, N00B_DIAG_TRANSPORT_CACHE_UNAVAILABLE,
                         N00B_TEXT_FFI_CANNOT_CREATE_CRATE,
                         N00BB_ARGS(ARG_TEXT(root),
                                    ARG_ERRNO(directory_error)));
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

static void
add_package_contracts(writ_unit_t *literal, core_translation_unit_t *unit)
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
    writ_unit_add_contract_dir(literal, (writ_str_t){dir.data, dir.byte_len});
    core_buffer_release(&dir);
}

static bool
generate_locked(n00bb_ffi_t *ffi, core_translation_unit_t *unit, core_string_t name, core_string_t module,
                n00bb_literal_origin_t origin, core_string_t text, core_buffer_t *source,
                n00bb_native_declarations_t *natives, n00bb_line_origins_t *origins)
{
    size_t first = writ_diag_count(ffi->session);
    writ_unit_t *literal = writ_unit_new(ffi->session);
    if (literal) {
        add_package_contracts(literal, unit);
    }
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
        if (ok) {
            remember_declared_symbols(&externs, unit, name, origin);
        }
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
    core_string_t configured = core_os_environment(CORE_STRING_STATIC("SDKROOT"));
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
    const char *compiler = n00bb_toolchain()->clang;
    if (!compiler) {
        return false;
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
    char *fixed[] = {(char *)compiler, native_target, "-std=c11", "-O2", "-c", "-emit-llvm",
                     "-I", native_directory};
    size_t next = 0;
    for (; next < sizeof(fixed) / sizeof(fixed[0]); next++) {
        arguments[next] = fixed[next];
    }
    if (core_string_find(triple, CORE_STRING_STATIC("windows")) < 0) {
        arguments[next++] = "-fPIC";
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
    core_process_spec_t spec = {.arguments = arguments, .search_path = true};
    core_process_t child;
    core_process_status_t status = {0};
    bool ok = !core_process_start(&spec, &child) && core_process_wait(child, &status)
           && status.exited && status.code == 0;
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
    core_string_t directory_text = temporary_directory("n00bb-shims.");
    if (!directory_text.data) {
        return false;
    }
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
    remove_directory(directory_text);
    return ok;
}

bool
n00bb_ffi_compile_shims(n00bb_ffi_t *ffi, core_string_t triple, core_string_t executable_path,
                        core_list_t(core_buffer_t) *bitcode, core_diagnostics_t *diagnostics)
{
    if (!ffi || !triple.data || !bitcode) {
        return false;
    }
    n00bb_native_cache_t *cache = executable_path.data
        && !n00bb_setting_bool(N00BB_KEY_DEV_NO_NATIVE_CACHE)
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
            if (!n00bb_toolchain()->clang) {
                n00bb_report(diagnostics, nullptr, N00B_DIAG_LINKER_NOT_FOUND,
                    (core_diag_location_t){}, N00B_TEXT_NO_CLANG_WAS_FOUND_TO_2,
                    N00BB_NO_ARGS);
                n00bb_report_help(diagnostics,
                    N00B_TEXT_INSTALL_LLVM_OR_SET_N00B,
                    N00BB_NO_ARGS);
            } else {
                n00bb_report(diagnostics, nullptr,
                    N00B_DIAG_SHIM_COMPILE_FAILED, (core_diag_location_t){},
                    N00B_TEXT_THE_C_CODE_GENERATED_FOR,
                    N00BB_ARGS(ARG_TEXT(ffi->shims.data[i].name)));
                n00bb_report_note(diagnostics,
                    N00B_TEXT_CLANG_S_MESSAGES_ARE_PRINTED,
                    N00BB_NO_ARGS);
            }
        }
    }
    n00bb_native_cache_close(cache);
    return ok;
}
