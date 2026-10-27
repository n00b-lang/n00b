#include "toolchain_commands.h"
#include "handoff.h"

#include "cache_location.h"
#include "compiler_cli.h"
#include "diagnostics.h"
#include "n00bb_version.h"
#include "toolchain.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/os.h"
#include "lib/process.h"
#include "compiler_object_io.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PIN_FILE ".n00b-toolchain"

typedef struct {
    core_string_t id;
    core_string_t version;
    core_string_t build;
    core_string_t revision;
    core_string_t date;
} installed_t;

core_list_decl(installed_t);

static core_string_t selection_reason;
static core_string_t selection_problem;
static const core_diag_location_t nowhere = {0};

static core_string_t
read_text(core_string_t path)
{
    char *bytes = nullptr;
    size_t size = 0;
    if (!core_os_read_file(path, &bytes, &size)) return (core_string_t){0};
    return (core_string_t){.data = bytes, .u8_bytes = size};
}

static core_string_t
trimmed(core_string_t text)
{
    size_t start = 0;
    size_t end = text.u8_bytes;
    while (start < end && (text.data[start] == ' ' || text.data[start] == '\t'
                           || text.data[start] == '\n'
                           || text.data[start] == '\r'))
        start++;
    while (end > start && (text.data[end - 1] == ' '
                           || text.data[end - 1] == '\t'
                           || text.data[end - 1] == '\n'
                           || text.data[end - 1] == '\r'))
        end--;
    return core_string_slice(text, start, end - start);
}

static core_string_t
marker_field(core_string_t text, core_string_t key)
{
    size_t at = 0;
    while (at < text.u8_bytes) {
        size_t end = at;
        while (end < text.u8_bytes && text.data[end] != '\n') end++;
        core_string_t line = core_string_slice(text, at, end - at);
        int64_t equals = core_string_find_byte(line, '=');
        if (equals > 0
            && core_string_eq(core_string_slice(line, 0, (size_t)equals), key))
            return core_string_dup(trimmed(core_string_slice(line,
                (size_t)equals + 1, line.u8_bytes - (size_t)equals - 1)));
        at = end + 1;
    }
    return core_string_dup(CORE_STRING_STATIC("?"));
}

void
n00bb_print_toolchain_marker(FILE *out)
{
    fprintf(out, "version=%s\nbuild=%s\nrevision=%s%s\ndate=%s\n",
            N00BB_VERSION, n00bb_build, n00bb_revision,
            n00bb_dirty ? "+dirty" : "", n00bb_build_date);
}

static core_string_t
toolchains_dir(void)
{
    return core_os_path_join(n00bb_toolchain()->store,
                           CORE_STRING_STATIC("toolchains"));
}

static int
compare_numbers(core_string_t a, core_string_t b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.u8_bytes || j < b.u8_bytes) {
        uint64_t x = 0;
        uint64_t y = 0;
        while (i < a.u8_bytes && a.data[i] >= '0' && a.data[i] <= '9')
            x = x * 10 + (uint64_t)(a.data[i++] - '0');
        while (j < b.u8_bytes && b.data[j] >= '0' && b.data[j] <= '9')
            y = y * 10 + (uint64_t)(b.data[j++] - '0');
        if (x != y) return x < y ? -1 : 1;
        if (i < a.u8_bytes) i++;
        if (j < b.u8_bytes) j++;
    }
    return 0;
}

static bool
is_link(const installed_t *toolchain)
{
    return core_string_ends_with(toolchain->id, CORE_STRING_STATIC("-link"));
}

static bool
newer(const installed_t *a, const installed_t *b)
{
    int versions = compare_numbers(a->version, b->version);
    if (versions) return versions > 0;
    int builds = compare_numbers(a->build, b->build);
    if (builds) return builds > 0;
    return !is_link(a) && is_link(b);
}

static core_list_t(installed_t)
installed_toolchains(void)
{
    core_list_t(installed_t) found = core_list_new(installed_t);
    core_string_t directory = toolchains_dir();
    char *native = core_string_to_cstr(directory);
    DIR *dir = native ? opendir(native) : nullptr;
    core_free(native);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        core_string_t name = core_string_from_cstr(entry->d_name);
        core_string_t root = core_os_path_join(directory, name);
        if (n00bb_toolchain_is_root(root)) {
            core_string_t marker = core_os_path_join(root,
                CORE_STRING_STATIC(N00BB_LAYOUT_MARKER));
            core_string_t text = read_text(marker);
            installed_t toolchain = {
                .id = core_string_dup(name),
                .version = marker_field(text, CORE_STRING_STATIC("version")),
                .build = marker_field(text, CORE_STRING_STATIC("build")),
                .revision = marker_field(text, CORE_STRING_STATIC("revision")),
                .date = marker_field(text, CORE_STRING_STATIC("date")),
            };
            core_list_push(found, toolchain);
            core_free(text.data);
            core_free(marker.data);
        }
        core_free(root.data);
        core_free(name.data);
    }
    if (dir) closedir(dir);
    core_free(directory.data);
    for (size_t i = 1; i < found.len; i++) {
        installed_t current = found.data[i];
        size_t j = i;
        while (j > 0 && newer(&current, &found.data[j - 1])) {
            found.data[j] = found.data[j - 1];
            j--;
        }
        found.data[j] = current;
    }
    return found;
}

static void
release_toolchains(core_list_t(installed_t) *list)
{
    for (size_t i = 0; i < list->len; i++) {
        core_free(list->data[i].id.data);
        core_free(list->data[i].version.data);
        core_free(list->data[i].build.data);
        core_free(list->data[i].revision.data);
        core_free(list->data[i].date.data);
    }
    core_list_free(*list);
}

static core_string_t
marker_target(core_string_t link)
{
#ifdef _WIN32
    return core_os_real_path(link);
#else
    char *native = core_string_to_cstr(link);
    char target[4096];
    ssize_t length = native ? readlink(native, target, sizeof target - 1) : -1;
    core_free(native);
    return length > 0
        ? core_string_dup(core_string_borrow(target, (size_t)length))
        : (core_string_t){0};
#endif
}


static core_string_t
active_id(void)
{
    core_string_t link = core_os_path_join(n00bb_toolchain()->store,
                                         CORE_STRING_STATIC("current"));
    core_string_t text = marker_target(link);
    core_free(link.data);
    if (!text.data) return (core_string_t){0};
    size_t start = text.u8_bytes;
    while (start > 0 && !core_os_is_separator(text.data[start - 1])) start--;
    core_string_t id = core_string_dup(core_string_slice(text, start,
                                                         text.u8_bytes - start));
    core_free(text.data);
    return id;
}

static bool
prefix_matches(const installed_t *toolchain, core_string_t spec)
{
    return core_string_starts_with(toolchain->id, spec);
}

static int
resolve_spec(const core_list_t(installed_t) *list, core_string_t spec)
{
    bool latest = core_string_eq(spec, CORE_STRING_STATIC("latest"));
    for (size_t i = 0; i < list->len; i++)
        if (core_string_eq(list->data[i].id, spec)) return (int)i;
    for (size_t i = 0; i < list->len; i++)
        if ((latest && !is_link(&list->data[i]))
            || core_string_eq(list->data[i].version, spec))
            return (int)i;
    if (latest && list->len) return 0;
    int match = -1;
    for (size_t i = 0; i < list->len; i++) {
        if (!prefix_matches(&list->data[i], spec)) continue;
        if (match >= 0) return -2;
        match = (int)i;
    }
    return match;
}

static core_list_t(core_string_t)
matching_ids(const core_list_t(installed_t) *list, core_string_t only_prefix)
{
    core_list_t(core_string_t) ids = core_list_new(core_string_t);
    for (size_t i = 0; i < list->len; i++)
        if (!only_prefix.u8_bytes
            || prefix_matches(&list->data[i], only_prefix))
            core_list_push(ids, list->data[i].id);
    return ids;
}

static void
note_installed(core_diagnostics_t *d, const core_list_t(installed_t) *list)
{
    core_string_t store = n00bb_toolchain()->store;
    if (!list->len) {
        n00bb_report_note(d, N00B_TEXT_NO_TOOLCHAINS_ARE_INSTALLED_IN,
            N00BB_ARGS(ARG_TEXT(store)));
        return;
    }
    core_list_t(core_string_t) ids = matching_ids(list, (core_string_t){0});
    n00bb_report_note(d, N00B_TEXT_INSTALLED_TOOLCHAINS,
        N00BB_ARGS(ARG_NAMES(ids.data, ids.len)));
    core_list_free(ids);
}

static void
suggest_toolchains(core_diagnostics_t *d, const core_list_t(installed_t) *list,
                   core_string_t spec)
{
    size_t count = 2 * list->len + 1;
    core_string_t *names = core_alloc_array(core_string_t, count);
    size_t used = 0;
    names[used++] = CORE_STRING_STATIC("latest");
    for (size_t i = 0; i < list->len; i++) {
        names[used++] = list->data[i].id;
        names[used++] = list->data[i].version;
    }
    n00bb_diag_did_you_mean(d, spec, names, used);
    core_free(names);
}

static void
report_unmatched(core_diagnostics_t *d, const core_list_t(installed_t) *list,
                 core_string_t spec, int status, core_string_t source)
{
    if (status == -2) {
        core_list_t(core_string_t) ids = matching_ids(list, spec);
        n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_AMBIGUOUS, nowhere,
            source.u8_bytes ? N00B_TEXT_TOOLCHAIN_FROM_MATCHES_SEVERAL
                            : N00B_TEXT_TOOLCHAIN_MATCHES_SEVERAL,
            N00BB_ARGS(ARG_TEXT(spec), ARG_TEXT(source),
                       ARG_NAMES(ids.data, ids.len)));
        n00bb_report_help(d, N00B_TEXT_GIVE_MORE_OF_THE_ID, N00BB_NO_ARGS);
        core_list_free(ids);
        return;
    }
    n00bb_report(d, nullptr, source.u8_bytes
                     ? N00B_DIAG_TOOLCHAIN_SELECTION_INVALID
                     : N00B_DIAG_TOOLCHAIN_NOT_FOUND, nowhere,
        source.u8_bytes ? N00B_TEXT_NO_TOOLCHAIN_FROM_MATCHES
                        : N00B_TEXT_NO_TOOLCHAIN_MATCHES,
        N00BB_ARGS(ARG_TEXT(spec), ARG_TEXT(source)));
    note_installed(d, list);
    suggest_toolchains(d, list, spec);
}

static int
unmatched(const core_list_t(installed_t) *list, core_string_t spec,
          int status)
{
    core_diagnostics_t *d = core_diagnostics_new();
    report_unmatched(d, list, spec, status, (core_string_t){0});
    if (!list->len)
        n00bb_report_help(d, N00B_TEXT_INSTALL_ONE_FROM_A_CHECKOUT,
            N00BB_NO_ARGS);
    return n00bb_finish_command_diagnostics(d, 1);
}

static core_string_t
nearest_pin(core_string_t *pin_path)
{
    *pin_path = core_os_find_upward(CORE_STRING_STATIC(PIN_FILE));
    if (!pin_path->data) return (core_string_t){0};
    core_string_t text = read_text(*pin_path);
    core_string_t spec = core_string_dup(trimmed(text));
    core_free(text.data);
    return spec;
}

core_string_t
n00bb_toolchain_selection(void)
{
    return selection_reason;
}

core_string_t
n00bb_toolchain_selection_problem(void)
{
    return selection_problem;
}

static bool
selection_optional(const n00bb_invocation_t *invocation)
{
    static const core_string_t informational[] = {
        CORE_STRING_STATIC("--version"), CORE_STRING_STATIC("-V"),
        CORE_STRING_STATIC("--help"), CORE_STRING_STATIC("-h"),
        CORE_STRING_STATIC("--help-all"),
    };
    static const core_string_t unaffected[] = {
        CORE_STRING_STATIC("version"), CORE_STRING_STATIC("env"),
        CORE_STRING_STATIC("help"), CORE_STRING_STATIC("toolchain"),
        CORE_STRING_STATIC("completions"), CORE_STRING_STATIC("explain"),
    };
    for (size_t i = 0; i < sizeof informational / sizeof informational[0]; i++)
        if (n00bb_invocation_flag(invocation, informational[i],
                                  (core_string_t){}))
            return true;
    for (size_t i = 0; i < sizeof unaffected / sizeof unaffected[0]; i++)
        if (n00bb_invocation_command_is(invocation, unaffected[i]))
            return true;
    return false;
}

static void
explain_selection_fix(core_diagnostics_t *d, bool from_environment,
                      core_string_t pin_path)
{
    if (from_environment) {
        n00bb_report_help(d, N00B_TEXT_SET_N00B_TOOLCHAIN_TO_AN, N00BB_NO_ARGS);
        return;
    }
    core_string_t directory = core_os_path_parent(pin_path);
    n00bb_report_help(d, N00B_TEXT_IN_RUN_N00B_TOOLCHAIN_PIN,
        N00BB_ARGS(ARG_TEXT(directory)));
    core_free(directory.data);
}

int
n00bb_toolchain_select(const n00bb_invocation_t *invocation, bool *handled)
{
    *handled = false;
    const n00bb_toolchain_t *self = n00bb_toolchain();
    core_string_t inherited = n00bb_handoff()->toolchain_reason;
    if (inherited.u8_bytes) {
        selection_reason = core_string_dup(inherited);
        return 0;
    }
    if (self->mode != N00BB_TOOLCHAIN_INSTALLED) return 0;
    core_string_t spec =
        core_os_environment(CORE_STRING_STATIC("N00B_TOOLCHAIN"));
    core_string_t pin_path = {0};
    bool from_environment = spec.u8_bytes != 0;
    core_buffer_t reason = {0};
    core_buffer_t source = {0};
    if (from_environment) {
        core_buffer_puts(&reason, "selected by N00B_TOOLCHAIN");
        core_buffer_puts(&source, "N00B_TOOLCHAIN");
    }
    else {
        core_free(spec.data);
        spec = nearest_pin(&pin_path);
        if (spec.data) {
            core_buffer_printf(&reason, "pinned by %.*s",
                               (int)pin_path.u8_bytes, pin_path.data);
            core_buffer_printf(&source, "the pin `%.*s`",
                               (int)pin_path.u8_bytes, pin_path.data);
        }
    }
    if (!spec.u8_bytes) {
        core_free(spec.data);
        core_free(pin_path.data);
        core_buffer_release(&reason);
        core_buffer_release(&source);
        return 0;
    }
    core_list_t(installed_t) list = installed_toolchains();
    int index = resolve_spec(&list, spec);
    int status = 0;
    if (index < 0) {
        core_buffer_t problem = {0};
        core_buffer_printf(&problem, "`%.*s` from %.*s matches %s installed "
                           "toolchain", (int)spec.u8_bytes, spec.data,
                           (int)source.byte_len, source.data,
                           index == -2 ? "more than one" : "no");
        core_free(selection_problem.data);
        selection_problem = core_string_dup(core_buffer_string(&problem));
        core_buffer_release(&problem);
        if (!selection_optional(invocation)) {
            core_diagnostics_t *d = core_diagnostics_new();
            report_unmatched(d, &list, spec, index,
                             core_buffer_string(&source));
            explain_selection_fix(d, from_environment, pin_path);
            status = n00bb_finish_command_diagnostics(d, 1);
            *handled = true;
        }
    }
    else if (core_string_eq(list.data[index].id, self->id)) {
        selection_reason = core_string_dup(core_buffer_string(&reason));
    }
    else {
        core_string_t directory = toolchains_dir();
        core_string_t root = core_os_path_join(directory, list.data[index].id);
        core_string_t exe = core_os_path_join(root, CORE_STRING_STATIC("bin/n00b"));
        char *native = core_string_to_cstr(exe);
        n00bb_handoff_set(&n00bb_handoff()->toolchain_reason,
                          core_buffer_string(&reason));
        bool ready = native && n00bb_handoff_pass();
        if (ready) {
            fflush(stdout);
            fflush(stderr);
            invocation->argv[0] = native;
        }
        int failure = ready
            ? core_process_exec(
                  &(core_process_spec_t){.arguments = invocation->argv})
            : ENOMEM;
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_START_FAILED, nowhere,
            N00B_TEXT_CANNOT_RUN_TOOLCHAIN,
            N00BB_ARGS(ARG_TEXT(list.data[index].id),
                ARG_TEXT(core_buffer_string(&reason)), ARG_TEXT(exe),
            ARG_ERRNO(failure)));
        n00bb_report_help(d, N00B_TEXT_REINSTALL_IT_OR_REMOVE_IT,
            N00BB_ARGS(ARG_TEXT(list.data[index].id)));
        status = n00bb_finish_command_diagnostics(d, 1);
        core_free(native);
        core_free(exe.data);
        core_free(root.data);
        core_free(directory.data);
        *handled = true;
    }
    release_toolchains(&list);
    core_buffer_release(&reason);
    core_buffer_release(&source);
    core_free(pin_path.data);
    core_free(spec.data);
    return status;
}

static int
toolchain_list(void)
{
    core_list_t(installed_t) list = installed_toolchains();
    core_string_t active = active_id();
    core_string_t pin_path = {0};
    core_string_t pin = nearest_pin(&pin_path);
    int pinned = pin.data ? resolve_spec(&list, pin) : -1;
    if (!list.len) {
        printf("No toolchains are installed in %.*s.\n",
               (int)n00bb_toolchain()->store.u8_bytes,
               n00bb_toolchain()->store.data);
        printf("Install one from a checkout with './build.sh install'.\n");
    }
    for (size_t i = 0; i < list.len; i++) {
        const installed_t *t = &list.data[i];
        bool is_active = active.data && core_string_eq(active, t->id);
        printf("%s %-24.*s rev %-14.*s %.*s%s%s\n",
               is_active ? "*" : " ", (int)t->id.u8_bytes, t->id.data,
               (int)t->revision.u8_bytes, t->revision.data,
               (int)t->date.u8_bytes, t->date.data,
               is_active ? "  (active)" : "",
               pinned == (int)i ? "  (pinned here)" : "");
    }
    if (pin.data && pinned < 0)
        printf("The pin %.*s names '%.*s', which matches %s installed "
               "toolchain.\n", (int)pin_path.u8_bytes, pin_path.data,
               (int)pin.u8_bytes, pin.data,
               pinned == -2 ? "more than one" : "no");
    release_toolchains(&list);
    core_free(active.data);
    core_free(pin.data);
    core_free(pin_path.data);
    return 0;
}

static int
toolchain_use(core_string_t spec)
{
    core_list_t(installed_t) list = installed_toolchains();
    int index = resolve_spec(&list, spec);
    if (index < 0) {
        int status = unmatched(&list, spec, index);
        release_toolchains(&list);
        return status;
    }
    core_string_t previous = active_id();
    core_string_t store = n00bb_toolchain()->store;
    core_string_t link = core_os_path_join(store, CORE_STRING_STATIC("current"));
    core_string_t staging = core_os_path_join(store,
                                            CORE_STRING_STATIC(".current.tmp"));
    core_buffer_t target = {0};
    core_buffer_puts(&target, "toolchains/");
    core_buffer_append_string(&target, list.data[index].id);
    char *native_staging = core_string_to_cstr(staging);
    unlink(native_staging);
    bool ok = core_os_symlink_directory(core_buffer_string(&target), staging)
        && core_os_rename_replace(staging, link);
    int failure = errno;
    if (ok) {
        printf("The active toolchain is now %.*s",
               (int)list.data[index].id.u8_bytes, list.data[index].id.data);
        if (previous.data && !core_string_eq(previous, list.data[index].id))
            printf(" (it was %.*s; switch back with 'n00b toolchain use "
                   "%.*s')", (int)previous.u8_bytes, previous.data,
                   (int)previous.u8_bytes, previous.data);
        printf(".\n");
    }
    else {
        unlink(native_staging);
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_CHANGE_FAILED, nowhere,
            N00B_TEXT_CANNOT_MAKE_THE_ACTIVE_TOOLCHAIN,
            N00BB_ARGS(ARG_TEXT(list.data[index].id), ARG_TEXT(link),
                       ARG_ERRNO(failure)));
        n00bb_finish_command_diagnostics(d, 1);
    }
    core_free(native_staging);
    core_buffer_release(&target);
    core_free(staging.data);
    core_free(link.data);
    core_free(previous.data);
    release_toolchains(&list);
    return ok ? 0 : 1;
}

static bool
remove_reporting(core_diagnostics_t *d, core_string_t id, core_string_t path,
                 n00bb_text_t what)
{
    core_os_removal_t removal;
    bool ok = core_os_remove_tree(path, &removal);
    if (!ok)
        n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_CHANGE_FAILED, nowhere,
            N00B_TEXT_COULD_NOT_REMOVE_OF_TOOLCHAIN,
            N00BB_ARGS(ARG_PHRASE_ID(what), ARG_TEXT(id),
                       ARG_TEXT(removal.failed_path),
                       ARG_ERRNO(removal.error)));
    core_os_removal_release(&removal);
    return ok;
}

static bool
remove_toolchain(core_diagnostics_t *d, core_string_t id)
{
    core_string_t directory = toolchains_dir();
    core_string_t root = core_os_path_join(directory, id);
    bool ok = remove_reporting(d, id, root, N00B_TEXT_PHRASE_THE_FILES);
    core_string_t cache = n00bb_cache_root();
    if (cache.data) {
        core_string_t keyed = core_os_path_join(cache, id);
        ok = remove_reporting(d, id, keyed, N00B_TEXT_PHRASE_THE_CACHE) && ok;
        core_free(keyed.data);
    }
    core_free(cache.data);
    core_free(root.data);
    core_free(directory.data);
    return ok;
}

static int
toolchain_remove(core_string_t spec)
{
    core_list_t(installed_t) list = installed_toolchains();
    int index = resolve_spec(&list, spec);
    if (index < 0) {
        int status = unmatched(&list, spec, index);
        release_toolchains(&list);
        return status;
    }
    core_string_t id = list.data[index].id;
    core_string_t active = active_id();
    core_diagnostics_t *d = core_diagnostics_new();
    int status = 0;
    if (active.data && core_string_eq(active, id)) {
        n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_REMOVE_ACTIVE, nowhere,
            N00B_TEXT_IS_THE_ACTIVE_TOOLCHAIN_AND,
            N00BB_ARGS(ARG_TEXT(id)));
        n00bb_report_help(d, N00B_TEXT_SWITCH_TO_ANOTHER_WITH_N00B,
            N00BB_NO_ARGS);
        status = 1;
    }
    else if (remove_toolchain(d, id)) {
        printf("Removed toolchain %.*s and its cache.\n", (int)id.u8_bytes,
               id.data);
    }
    else status = 1;
    core_free(active.data);
    release_toolchains(&list);
    return n00bb_finish_command_diagnostics(d, status);
}

static int
toolchain_prune(int64_t keep)
{
    if (keep < 0) {
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_INVALID_OPTION_VALUE, nowhere,
            N00B_TEXT_KEEP_MUST_BE_0_OR,
            N00BB_ARGS(ARG_NUMBER((long long)keep)));
        return n00bb_finish_command_diagnostics(d, 2);
    }
    core_list_t(installed_t) list = installed_toolchains();
    core_string_t active = active_id();
    core_diagnostics_t *d = core_diagnostics_new();
    size_t removed = 0;
    bool ok = true;
    for (size_t i = 0; i < list.len; i++) {
        int64_t newer_same_version = 0;
        for (size_t j = 0; j < i; j++)
            newer_same_version += core_string_eq(list.data[j].version,
                                                 list.data[i].version);
        bool is_active = active.data
            && core_string_eq(active, list.data[i].id);
        if (newer_same_version < keep || is_active) continue;
        if (!remove_toolchain(d, list.data[i].id)) {
            ok = false;
            continue;
        }
        printf("Removed toolchain %.*s.\n", (int)list.data[i].id.u8_bytes,
               list.data[i].id.data);
        removed++;
    }
    if (!removed && ok) printf("Nothing to prune.\n");
    core_free(active.data);
    release_toolchains(&list);
    return n00bb_finish_command_diagnostics(d, ok ? 0 : 1);
}

static int
toolchain_pin(core_string_t spec)
{
    core_list_t(installed_t) list = installed_toolchains();
    int index = resolve_spec(&list, spec);
    if (index < 0) {
        int status = unmatched(&list, spec, index);
        release_toolchains(&list);
        return status;
    }
    release_toolchains(&list);
    core_string_t cwd = core_os_current_directory();
    core_string_t path = core_os_path_join(cwd, CORE_STRING_STATIC(PIN_FILE));
    core_buffer_t pin = {0};
    core_buffer_append_string(&pin, spec);
    core_buffer_putc(&pin, '\n');
    errno = 0;
    bool ok = core_os_write_file_atomic(path, pin.data, pin.byte_len);
    int failure = ok ? 0 : errno;
    core_buffer_release(&pin);
    if (ok)
        printf("Pinned %.*s to toolchain %.*s in %s.\n",
               (int)cwd.u8_bytes, cwd.data, (int)spec.u8_bytes, spec.data,
               PIN_FILE);
    else {
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_CHANGE_FAILED, nowhere,
            N00B_TEXT_CANNOT_WRITE_THE_TOOLCHAIN_PIN,
            N00BB_ARGS(ARG_TEXT(path), ARG_ERRNO(failure ? failure : EIO)));
        n00bb_finish_command_diagnostics(d, 1);
    }
    core_free(path.data);
    core_free(cwd.data);
    return ok ? 0 : 1;
}

static int
toolchain_unpin(void)
{
    core_string_t cwd = core_os_current_directory();
    core_string_t path = core_os_path_join(cwd, CORE_STRING_STATIC(PIN_FILE));
    char *native = core_string_to_cstr(path);
    int status = 0;
    if (native && !unlink(native)) {
        printf("Removed the toolchain pin in %.*s.\n", (int)cwd.u8_bytes,
               cwd.data);
    }
    else {
        int failure = errno;
        core_diagnostics_t *d = core_diagnostics_new();
        if (failure != ENOENT) {
            n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_CHANGE_FAILED, nowhere,
                N00B_TEXT_CANNOT_REMOVE_THE_TOOLCHAIN_PIN,
                N00BB_ARGS(ARG_TEXT(path), ARG_ERRNO(failure)));
        }
        else {
            n00bb_report(d, nullptr, N00B_DIAG_NO_TOOLCHAIN_PIN, nowhere,
                N00B_TEXT_THERE_IS_NO_TOOLCHAIN_PIN,
                N00BB_ARGS(ARG_TEXT(cwd)));
            core_string_t pin_path = {0};
            core_string_t pin = nearest_pin(&pin_path);
            if (pin.data) {
                core_string_t holder = core_os_path_parent(pin_path);
                n00bb_report_note(d, N00B_TEXT_THE_PIN_IN_EFFECT_HERE,
                    N00BB_ARGS(ARG_TEXT(pin_path)));
                n00bb_report_help(d, N00B_TEXT_RUN_N00B_TOOLCHAIN_UNPIN_IN,
                    N00BB_ARGS(ARG_TEXT(holder)));
                core_free(holder.data);
            }
            core_free(pin.data);
            core_free(pin_path.data);
        }
        status = n00bb_finish_command_diagnostics(d, 1);
    }
    core_free(native);
    core_free(path.data);
    core_free(cwd.data);
    return status;
}

static const core_string_t toolchain_actions[] = {
    CORE_STRING_STATIC("list"), CORE_STRING_STATIC("use"),
    CORE_STRING_STATIC("remove"), CORE_STRING_STATIC("prune"),
    CORE_STRING_STATIC("pin"), CORE_STRING_STATIC("unpin"),
};

int
n00bb_unknown_action(core_string_t command, core_string_t action,
                     const core_string_t *choices, size_t count)
{
    core_diagnostics_t *d = core_diagnostics_new();
    n00bb_report(d, nullptr, N00B_DIAG_UNKNOWN_ACTION, nowhere,
        N00B_TEXT_HAS_NO_ACTION,
        N00BB_ARGS(ARG_TEXT(command), ARG_TEXT(action)));
    n00bb_diag_did_you_mean(d, action, choices, count);
    core_buffer_t names = {0};
    for (size_t i = 0; i < count; i++)
        core_buffer_printf(&names, "%s`%.*s`",
                           i == 0 ? "" : i + 1 == count ? " or " : ", ",
                           (int)choices[i].u8_bytes, choices[i].data);
    n00bb_report_note(d, N00B_TEXT_TAKES,
        N00BB_ARGS(ARG_TEXT(command), ARG_TEXT(core_buffer_string(&names))));
    core_buffer_release(&names);
    return n00bb_finish_command_diagnostics(d, 2);
}

int
n00bb_toolchain_command(core_string_t action, core_string_t spec,
                        int64_t keep)
{
    size_t action_count = sizeof(toolchain_actions)
                        / sizeof(toolchain_actions[0]);
    bool known = false;
    for (size_t i = 0; i < action_count; i++)
        known = known || core_string_eq(action, toolchain_actions[i]);
    if (!known)
        return n00bb_unknown_action(CORE_STRING_STATIC("toolchain"), action,
                                    toolchain_actions, action_count);
    bool needs_spec = core_string_eq(action, CORE_STRING_STATIC("use"))
        || core_string_eq(action, CORE_STRING_STATIC("remove"))
        || core_string_eq(action, CORE_STRING_STATIC("pin"));
    if (needs_spec != (spec.u8_bytes != 0)) {
        core_diagnostics_t *d = core_diagnostics_new();
        if (needs_spec) {
            n00bb_report(d, nullptr, N00B_DIAG_TOOLCHAIN_NEEDS_SPEC, nowhere,
                N00B_TEXT_TOOLCHAIN_NEEDS_THE_TOOLCHAIN_TO,
                N00BB_ARGS(ARG_TEXT(action)));
            n00bb_report_help(d, N00B_TEXT_NAME_IT_BY_ID_VERSION,
                N00BB_NO_ARGS);
        }
        else {
            n00bb_report(d, nullptr, N00B_DIAG_EXTRA_ARGUMENT, nowhere,
                N00B_TEXT_TOOLCHAIN_TAKES_NO_TOOLCHAIN_BUT,
                N00BB_ARGS(ARG_TEXT(action), ARG_TEXT(spec)));
        }
        return n00bb_finish_command_diagnostics(d, 2);
    }
    if (core_string_eq(action, CORE_STRING_STATIC("list")))
        return toolchain_list();
    if (core_string_eq(action, CORE_STRING_STATIC("use")))
        return toolchain_use(spec);
    if (core_string_eq(action, CORE_STRING_STATIC("remove")))
        return toolchain_remove(spec);
    if (core_string_eq(action, CORE_STRING_STATIC("prune")))
        return toolchain_prune(keep);
    if (core_string_eq(action, CORE_STRING_STATIC("pin")))
        return toolchain_pin(spec);
    return toolchain_unpin();
}

int
n00bb_cache_command(core_string_t action, bool all)
{
    static const core_string_t cache_actions[] = {
        CORE_STRING_STATIC("clean"),
    };
    if (!core_string_eq(action, CORE_STRING_STATIC("clean")))
        return n00bb_unknown_action(CORE_STRING_STATIC("cache"), action,
                                    cache_actions, 1);
    core_string_t target = all ? n00bb_cache_root()
                               : n00bb_cache_directory(CORE_STRING_STATIC(""));
    if (!target.data) {
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_CACHE_UNAVAILABLE, nowhere,
            N00B_TEXT_NO_CACHE_DIRECTORY,
            N00BB_NO_ARGS);
        return n00bb_finish_command_diagnostics(d, 1);
    }
    core_os_removal_t removal;
    bool ok = core_os_remove_tree(target, &removal);
    int status = 0;
    if (!removal.existed)
        printf("Nothing to clean: %.*s does not exist.\n",
               (int)target.u8_bytes, target.data);
    else if (ok)
        printf("Removed %.*s.\n", (int)target.u8_bytes, target.data);
    else {
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_CACHE_REMOVE_FAILED, nowhere,
            N00B_TEXT_COULD_NOT_REMOVE_THE_CACHE,
            N00BB_ARGS(ARG_TEXT(target), ARG_TEXT(removal.failed_path),
                       ARG_ERRNO(removal.error)));
        status = n00bb_finish_command_diagnostics(d, 1);
    }
    core_os_removal_release(&removal);
    core_free(target.data);
    return status;
}
