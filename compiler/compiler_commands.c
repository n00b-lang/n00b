#include "compiler_cli.h"
#include "handoff.h"

#include "cache_location.h"
#include "config.h"
#include "diagnostics.h"
#include "driver.h"
#include "n00bb_version.h"
#include "toolchain_commands.h"

#include "lib/os.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/hash.h"
#include "lib/process.h"
#include "lib/link_unit.h"
#include "lib/string.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static const core_diag_location_t nowhere = {0};

static void
print_footer(FILE *out)
{
    fputs("\nRun 'n00b help <command>' for a command's options, "
          "or 'n00b --help-all' for everything.\n", out);
}

static void
print_summary(parse_cmdr_t *cli, FILE *out)
{
    parse_cmdr_print_summary(cli, out);
    print_footer(out);
}

int
n00bb_finish_command_diagnostics(core_diagnostics_t *diagnostics, int status)
{
    n00bb_print_diagnostics(diagnostics);
    core_diagnostics_free(diagnostics);
    return status;
}

void
n00bb_print_version(FILE *out)
{
    fprintf(out, "n00b %s (build %s, rev %s%s, %s)\n", N00BB_VERSION,
            n00bb_build, n00bb_revision, n00bb_dirty ? ", dirty" : "",
            n00bb_build_date);
    const n00bb_toolchain_t *toolchain = n00bb_toolchain();
    core_string_t id = n00bb_toolchain_id();
    switch (toolchain->mode) {
    case N00BB_TOOLCHAIN_DEV:
        fprintf(out, "dev build from %.*s\n", (int)toolchain->root.u8_bytes,
                toolchain->root.data);
        break;
    case N00BB_TOOLCHAIN_INSTALLED: {
        core_string_t reason = n00bb_toolchain_selection();
        fprintf(out, "toolchain %.*s (%.*s) at %.*s\n", (int)id.u8_bytes,
                id.data, reason.data ? (int)reason.u8_bytes : 6,
                reason.data ? reason.data : "active",
                (int)toolchain->root.u8_bytes, toolchain->root.data);
        break;
    }
    case N00BB_TOOLCHAIN_HOME:
        fprintf(out, "toolchain %.*s at %.*s (from N00B_HOME)\n",
                (int)id.u8_bytes, id.data, (int)toolchain->root.u8_bytes,
                toolchain->root.data);
        break;
    }
}

static void
suggest_commands(core_diagnostics_t *d, core_string_t name)
{
    core_string_t names[64];
    size_t count = n00bb_cli_command_names(names, 64);
    n00bb_diag_did_you_mean(d, name, names, count);
}

static int
help_command(const n00bb_invocation_t *invocation)
{
    int topic = 0;
    for (int i = invocation->command_at + 1; i < invocation->end && !topic;
         i++)
        if (invocation->words[i].u8_bytes
            && invocation->words[i].data[0] != '-') topic = i;
    if (!topic) {
        print_summary(invocation->cli, stdout);
        return 0;
    }
    core_string_t name = invocation->words[topic];
    bool known = parse_cmdr_print_command_help(invocation->cli, name, stdout);
    if (!known) {
        core_diagnostics_t *d = core_diagnostics_new();
        n00bb_report(d, nullptr, N00B_DIAG_UNKNOWN_COMMAND, nowhere,
            N00B_TEXT_THERE_IS_NO_COMMAND_NAMED,
            N00BB_ARGS(ARG_TEXT(name)));
        suggest_commands(d, name);
        n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_TO_LIST, N00BB_NO_ARGS);
        n00bb_finish_command_diagnostics(d, 2);
    }
    return known ? 0 : 2;
}

core_string_t
n00bb_default_output_name(core_string_t input)
{
    size_t end = input.u8_bytes;
    while (end > 1 && input.data[end - 1] == '/') end--;
    core_string_t trimmed = core_string_slice(input, 0, end);
    int64_t slash = core_string_rfind_byte(trimmed, '/');
    core_string_t base = slash >= 0
        ? core_string_slice(trimmed, (size_t)slash + 1,
                            trimmed.u8_bytes - (size_t)slash - 1)
        : trimmed;
    bool source = core_string_ends_with(base, CORE_STRING_STATIC(".n"))
        || core_string_ends_with(base, CORE_STRING_STATIC(".n00b"));
    int64_t dot = core_string_rfind_byte(base, '.');
    core_string_t name = base;
    if (source && dot > 0) name = core_string_slice(base, 0, (size_t)dot);
    else if (!source && dot >= 0)
        name = core_string_slice(base, (size_t)dot + 1,
                                 base.u8_bytes - (size_t)dot - 1);
    if (!name.u8_bytes || core_string_eq(name, CORE_STRING_STATIC(".")))
        name = CORE_STRING_STATIC("a.out");
    return core_string_dup(name);
}

static core_string_t
describe_argument(core_string_t command)
{
    core_string_t argument = n00bb_cli_command_argument(command);
    if (!argument.u8_bytes)
        return n00bb_text(N00B_TEXT_ARGUMENT, N00BB_NO_ARGS);
    core_list_t(core_string_t) names = core_list_new(core_string_t);
    for (int64_t bar; (bar = core_string_find_byte(argument, '|')) >= 0;) {
        core_list_push(names, core_string_slice(argument, 0, (size_t)bar));
        argument = core_string_slice(argument, (size_t)bar + 1,
                                     argument.u8_bytes - (size_t)bar - 1);
    }
    core_list_push(names, argument);
    core_string_t described = n00bb_text(N00B_TEXT_TEXT,
        N00BB_ARGS(ARG_ALTERNATIVES(names.data, names.len)));
    core_list_free(names);
    return described;
}

static bool
looks_like_input(core_string_t value)
{
    if (core_string_ends_with(value, CORE_STRING_STATIC(".n"))
        || core_string_ends_with(value, CORE_STRING_STATIC(".n00b")))
        return true;
    char *native = core_string_to_cstr(value);
    struct stat info;
    bool exists = native && !stat(native, &info);
    core_free(native);
    return exists;
}

static void
report_unknown_option(core_diagnostics_t *d, parse_cmdr_error_t error,
                      core_string_t command, bool named)
{
    if (named)
        n00bb_report(d, nullptr, N00B_DIAG_UNKNOWN_OPTION, nowhere,
            N00B_TEXT_HAS_NO_OPTION,
            N00BB_ARGS(ARG_TEXT(command), ARG_TEXT(error.flag)));
    else
        n00bb_report(d, nullptr, N00B_DIAG_UNKNOWN_OPTION, nowhere,
            N00B_TEXT_UNKNOWN_OPTION,
            N00BB_ARGS(ARG_TEXT(error.flag)));
    core_string_t names[128];
    size_t count = n00bb_cli_option_names(command, names, 128);
    core_string_t owner = n00bb_cli_option_command(error.flag);
    if (owner.u8_bytes && !core_string_eq(owner, command))
        n00bb_report_note(d, N00B_TEXT_IS_AN_OPTION_OF_N00B,
            N00BB_ARGS(ARG_TEXT(error.flag), ARG_TEXT(owner)));
    else n00bb_diag_did_you_mean(d, error.flag, names, count);
    if (named)
        n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_TO_LIST_2,
            N00BB_ARGS(ARG_TEXT(command)));
    else
        n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_TO_LIST_3, N00BB_NO_ARGS);
}

static void
report_argument_count(core_diagnostics_t *d, parse_cmdr_error_t error,
                      core_string_t command, bool named)
{
    core_string_t what = describe_argument(command);
    if (error.kind == PARSE_CMDR_ERROR_TOO_FEW_ARGUMENTS) {
        n00bb_report(d, nullptr, N00B_DIAG_MISSING_ARGUMENT, nowhere,
            named ? N00B_TEXT_COMMAND_IS_MISSING_ARGUMENT
                  : N00B_TEXT_NO_TO_WAS_GIVEN,
            N00BB_ARGS(ARG_TEXT(what), ARG_TEXT(command)));
        if (core_string_eq(command, CORE_STRING_STATIC("build"))
            || core_string_eq(command, CORE_STRING_STATIC("run")))
            n00bb_report_help(d, N00B_TEXT_NAME_THE_PROGRAM_TO_AS,
                N00BB_ARGS(ARG_TEXT(command), ARG_TEXT(command)));
        else
            n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_TO_SEE,
                N00BB_ARGS(ARG_TEXT(command)));
    }
    else {
        n00bb_report(d, nullptr, N00B_DIAG_EXTRA_ARGUMENT, nowhere,
            N00B_TEXT_COMMAND_TAKES_AT_MOST,
            N00BB_ARGS(ARG_NUMBER(error.limit), ARG_TEXT(command),
                       ARG_TEXT(what), ARG_NUMBER(error.given)));
        if (error.value.u8_bytes)
            n00bb_report_note(d, N00B_TEXT_THE_FIRST_EXTRA_ARGUMENT_IS,
                N00BB_ARGS(ARG_TEXT(error.value)));
        if (core_string_eq(command, CORE_STRING_STATIC("run")))
            n00bb_report_help(d, N00B_TEXT_PUT_ARGUMENTS_FOR_THE_PROGRAM,
                N00BB_ARGS(ARG_TEXT(error.value)));
    }
    core_free(what.data);
}

static void
report_cli_error(core_diagnostics_t *d, parse_cmdr_result_t *args, int index)
{
    parse_cmdr_error_t error = parse_cmdr_result_error_detail(args, index);
    core_string_t command = parse_cmdr_result_command(args);
    bool named = parse_cmdr_result_command_given(args);
    core_string_t flag = error.flag;
    core_string_t value = error.value;
    switch (error.kind) {
    case PARSE_CMDR_ERROR_UNKNOWN_FLAG:
        report_unknown_option(d, error, command, named);
        break;
    case PARSE_CMDR_ERROR_MISSING_VALUE:
        n00bb_report(d, nullptr, N00B_DIAG_OPTION_MISSING_VALUE, nowhere,
                     N00B_TEXT_NEEDS_A_VALUE,
            N00BB_ARGS(ARG_TEXT(flag)));
        if (core_string_starts_with(flag, CORE_STRING_STATIC("--")))
            n00bb_report_help(d, N00B_TEXT_WRITE_VALUE_OR_VALUE,
                N00BB_ARGS(ARG_TEXT(flag), ARG_TEXT(flag)));
        else
            n00bb_report_help(d, N00B_TEXT_WRITE_VALUE,
                N00BB_ARGS(ARG_TEXT(flag)));
        break;
    case PARSE_CMDR_ERROR_INVALID_INTEGER:
    case PARSE_CMDR_ERROR_INVALID_NUMBER:
        n00bb_report(d, nullptr, N00B_DIAG_OPTION_INVALID_NUMBER, nowhere,
            error.kind == PARSE_CMDR_ERROR_INVALID_INTEGER
                ? N00B_TEXT_TAKES_WHOLE_NUMBER : N00B_TEXT_TAKES_NUMBER,
            N00BB_ARGS(ARG_TEXT(flag), ARG_TEXT(value)));
        if (looks_like_input(value)) {
            n00bb_report_note(d, N00B_TEXT_WAS_READ_AS_THE_VALUE,
                N00BB_ARGS(ARG_TEXT(value), ARG_TEXT(flag)));
            n00bb_report_help(d, N00B_TEXT_GIVE_THE_NUMBER_RIGHT_AFTER,
                N00BB_ARGS(ARG_TEXT(flag), ARG_TEXT(value)));
        }
        break;
    case PARSE_CMDR_ERROR_UNEXPECTED_VALUE:
        n00bb_report(d, nullptr, N00B_DIAG_OPTION_UNEXPECTED_VALUE, nowhere,
                     N00B_TEXT_DOES_NOT_TAKE_A_VALUE,
            N00BB_ARGS(ARG_TEXT(flag)));
        n00bb_report_help(d, N00B_TEXT_WRITE_ON_ITS_OWN,
            N00BB_ARGS(ARG_TEXT(flag)));
        break;
    case PARSE_CMDR_ERROR_TOO_FEW_ARGUMENTS:
    case PARSE_CMDR_ERROR_TOO_MANY_ARGUMENTS:
        report_argument_count(d, error, command, named);
        break;
    case PARSE_CMDR_ERROR_SYNTAX:
        if (value.u8_bytes)
            n00bb_report(d, nullptr, N00B_DIAG_COMMAND_LINE_SYNTAX, nowhere,
                N00B_TEXT_IS_NOT_EXPECTED_HERE,
                N00BB_ARGS(ARG_TEXT(value)));
        else
            n00bb_report(d, nullptr, N00B_DIAG_COMMAND_LINE_SYNTAX, nowhere,
                N00B_TEXT_THE_COMMAND_LINE_ENDS_TOO,
                N00BB_NO_ARGS);
        if (named)
            n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_TO_SEE,
                N00BB_ARGS(ARG_TEXT(command)));
        else
            n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_FOR_USAGE,
                N00BB_NO_ARGS);
        break;
    }
}

void
n00bb_cli_report_errors(parse_cmdr_result_t *args)
{
    core_diagnostics_t *d = core_diagnostics_new();
    for (int32_t i = 0; i < parse_cmdr_result_error_count(args); i++)
        report_cli_error(d, args, i);
    n00bb_finish_command_diagnostics(d, 2);
}

static parse_cmdr_result_t *
parsed_or_report(const n00bb_invocation_t *invocation)
{
    if (parse_cmdr_result_ok(invocation->parsed)) return invocation->parsed;
    n00bb_cli_report_errors(invocation->parsed);
    return nullptr;
}

static bool
usable_directory(core_string_t directory, int *failure)
{
    *failure = 0;
    errno = 0;
    if (!n00bb_make_directories(directory)) {
        *failure = errno ? errno : ENOTDIR;
        return false;
    }
    char *native = core_string_to_cstr(directory);
    bool writable = native && !access(native, W_OK | X_OK);
    if (!writable) *failure = errno ? errno : EACCES;
    core_free(native);
    return writable;
}

static core_string_t
run_output_path(core_string_t input, core_diagnostics_t *d)
{
    core_string_t cache = n00bb_cache_directory(CORE_STRING_STATIC("run"));
    if (!cache.data) {
        n00bb_report(d, nullptr, N00B_DIAG_CACHE_UNAVAILABLE, nowhere,
            N00B_TEXT_THERE_IS_NO_CACHE_DIRECTORY,
            N00BB_NO_ARGS);
        n00bb_report_help(d, N00B_TEXT_SET_N00B_CACHE_TO_A, N00BB_NO_ARGS);
        return cache;
    }
    core_string_t working = core_os_current_directory();
    core_buffer_t key = {0};
    core_buffer_append_string(&key, working);
    core_free(working.data);
    core_buffer_append(&key, "", 1);
    core_buffer_append_string(&key, input);
    uint64_t hash = core_xxh3_64bits_raw(key.data, key.byte_len);
    core_buffer_release(&key);
    core_buffer_t directory = {0};
    core_buffer_printf(&directory, "%.*s/%016llx", (int)cache.u8_bytes,
                       cache.data, (unsigned long long)hash);
    core_free(cache.data);
    core_string_t result = {0};
    int failure = 0;
    if (usable_directory(core_buffer_string(&directory), &failure)) {
        core_string_t name = n00bb_default_output_name(input);
        core_buffer_puts(&directory, "/");
        core_buffer_append_string(&directory, name);
        core_free(name.data);
        result = core_string_dup(core_buffer_string(&directory));
    }
    else {
        n00bb_report(d, nullptr, N00B_DIAG_CACHE_UNAVAILABLE, nowhere,
            N00B_TEXT_CANNOT_CREATE_TO_HOLD_THE,
            N00BB_ARGS(ARG_TEXT(core_buffer_string(&directory)),
                       ARG_ERRNO(failure)));
        n00bb_report_help(d, N00B_TEXT_SET_N00B_CACHE_TO_A, N00BB_NO_ARGS);
    }
    core_buffer_release(&directory);
    return result;
}

static int
child_status(core_process_status_t status)
{
    return status.exited ? status.code : 128 + status.code;
}

static int
run_command(const n00bb_invocation_t *invocation)
{
    parse_cmdr_result_t *parsed = parsed_or_report(invocation);
    if (!parsed) return 2;
    int argc = invocation->argc;
    char **argv = invocation->argv;
    int end = invocation->end;
    int at = invocation->command_at;
    core_string_t input = core_string_dup(parse_cmdr_arg_str(parsed, 0));
    core_diagnostics_t *d = core_diagnostics_new();
    core_string_t program = run_output_path(input, d);
    core_free(input.data);
    if (!program.data) return n00bb_finish_command_diagnostics(d, 1);
    char *program_path = core_string_to_cstr(program);
    core_free(program.data);
    core_string_t compiler = core_os_executable_path(invocation->argv0);
    char *compiler_path = core_string_to_cstr(compiler);
    core_free(compiler.data);
    char **build = core_alloc_array(char *, (size_t)end + 3);
    int count = 0;
    build[count++] = argv[0];
    for (int i = 1; i < end; i++)
        build[count++] = i == at ? "build" : argv[i];
    build[count++] = "--output";
    build[count++] = program_path;
    build[count] = nullptr;
    core_process_spec_t spec = {.arguments = build};
    core_process_t child;
    core_process_status_t status = {0};
    if (compiler_path) build[0] = compiler_path;
    int spawned = compiler_path ? core_process_start(&spec, &child) : ENOENT;
    core_free(build);
    if (spawned) {
        n00bb_report(d, nullptr, N00B_DIAG_PROCESS_START_FAILED, nowhere,
            N00B_TEXT_CANNOT_START_COMPILER,
            N00BB_ARGS(ARG_NATIVE(compiler_path ? compiler_path : "n00b"),
                       ARG_ERRNO(spawned)));
        core_free(compiler_path);
        core_free(program_path);
        return n00bb_finish_command_diagnostics(d, 1);
    }
    core_free(compiler_path);
    int built = core_process_wait(child, &status) ? child_status(status) : 1;
    if (built) {
        core_free(program_path);
        core_diagnostics_free(d);
        return built;
    }
    int passed = argc - end - (end < argc);
    char **program_args = core_alloc_array(char *, (size_t)passed + 2);
    program_args[0] = program_path;
    for (int i = 0; i < passed; i++) program_args[i + 1] = argv[end + 1 + i];
    program_args[passed + 1] = nullptr;
    fflush(stdout);
    fflush(stderr);
    int failed = core_process_exec(
        &(core_process_spec_t){.arguments = program_args});
    n00bb_report(d, nullptr, N00B_DIAG_PROCESS_START_FAILED, nowhere,
        N00B_TEXT_CANNOT_RUN_THE_BUILT_PROGRAM,
        N00BB_ARGS(ARG_NATIVE(program_path), ARG_ERRNO(failed)));
    core_free(program_args);
    core_free(program_path);
    return n00bb_finish_command_diagnostics(d, 1);
}

static int
manage_command(const n00bb_invocation_t *invocation)
{
    parse_cmdr_result_t *parsed = parsed_or_report(invocation);
    if (!parsed) return 2;
    core_string_t action = core_string_dup(parse_cmdr_arg_str(parsed, 0));
    core_string_t spec = parse_cmdr_arg_count(parsed) > 1
        ? core_string_dup(parse_cmdr_arg_str(parsed, 1)) : (core_string_t){0};
    int status = n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("toolchain"))
        ? n00bb_toolchain_command(action, spec, n00bb_setting_int(
              N00BB_KEY_TOOLCHAIN_KEEP))
        : n00bb_cache_command(action, parse_cmdr_flag_present(parsed,
              CORE_STRING_STATIC("--all")));
    core_free(spec.data);
    core_free(action.data);
    return status;
}

static bool
existing_input(core_string_t word)
{
    static const core_string_t suffixes[] = {
        CORE_STRING_STATIC(""), CORE_STRING_STATIC(".n"),
        CORE_STRING_STATIC(".n00b"),
    };
    bool found = false;
    for (size_t i = 0; !found && i < sizeof(suffixes) / sizeof(suffixes[0]);
         i++) {
        core_buffer_t path = {0};
        core_buffer_append_string(&path, word);
        core_buffer_append_string(&path, suffixes[i]);
        char *native = core_string_to_cstr(core_buffer_string(&path));
        struct stat info;
        found = native && !stat(native, &info);
        core_free(native);
        core_buffer_release(&path);
    }
    return found;
}

static bool
report_mistyped_command(parse_cmdr_result_t *parsed)
{
    if (parse_cmdr_result_command_given(parsed)
        || parse_cmdr_arg_count(parsed) < 1)
        return false;
    core_string_t word = parse_cmdr_arg_str(parsed, 0);
    if (core_string_find_byte(word, '/') >= 0
        || core_string_find_byte(word, '.') >= 0 || existing_input(word))
        return false;
    core_string_t names[64];
    size_t count = n00bb_cli_command_names(names, 64);
    size_t best[1];
    if (!n00bb_closest_names(word, names, count, best, 1)) return false;
    core_diagnostics_t *d = core_diagnostics_new();
    n00bb_report(d, nullptr, N00B_DIAG_UNKNOWN_COMMAND, nowhere,
        N00B_TEXT_IS_NOT_A_COMMAND_A,
        N00BB_ARGS(ARG_TEXT(word)));
    n00bb_diag_did_you_mean(d, word, names, count);
    n00bb_report_help(d, N00B_TEXT_RUN_N00B_HELP_TO_LIST, N00BB_NO_ARGS);
    n00bb_finish_command_diagnostics(d, 2);
    return true;
}

static bool
check_native_roots(core_diagnostics_t *d, parse_cmdr_result_t *parsed)
{
    core_string_t option = CORE_STRING_STATIC("--native-root");
    bool ok = true;
    for (int32_t i = 0; i < parse_cmdr_flag_value_count(parsed, option);
         i++) {
        core_string_t value = parse_cmdr_flag_str_at(parsed, option, i);
        int64_t equals = core_string_find_byte(value, '=');
        core_string_t function = equals >= 0
            ? core_string_slice(value, 0, (size_t)equals) : value;
        core_string_t symbol = equals >= 0
            ? core_string_slice(value, (size_t)equals + 1,
                                value.u8_bytes - (size_t)equals - 1)
            : CORE_STRING_STATIC("-");
        int64_t dot = core_string_rfind_byte(function, '.');
        bool qualified = dot > 0 && (size_t)dot + 1 < function.u8_bytes;
        if (qualified && symbol.u8_bytes) continue;
        n00bb_report(d, nullptr, N00B_DIAG_INVALID_OPTION_VALUE, nowhere,
            qualified ? N00B_TEXT_NATIVE_ROOT_NOTHING_AFTER_EQUALS
                      : N00B_TEXT_NATIVE_ROOT_NOT_QUALIFIED,
            N00BB_ARGS(ARG_TEXT(value)));
        n00bb_report_help(d, N00B_TEXT_WRITE_NATIVE_ROOT_MODULE_FUNCTION,
            N00BB_NO_ARGS);
        ok = false;
    }
    return ok;
}

static bool
check_library_output(core_diagnostics_t *d, parse_cmdr_result_t *parsed)
{
    if (!core_string_eq(parse_cmdr_result_command(parsed),
                        CORE_STRING_STATIC("library"))
        || parse_cmdr_flag_present(parsed, CORE_STRING_STATIC("--output"))
        || parse_cmdr_flag_present(parsed,
                                   CORE_STRING_STATIC("--emit-interface")))
        return true;
    n00bb_report(d, nullptr, N00B_DIAG_LIBRARY_NEEDS_OUTPUT, nowhere,
        N00B_TEXT_LIBRARY_NEEDS_OUTPUT_FILE_FOR_2,
        N00BB_NO_ARGS);
    return false;
}

typedef enum {
    PATH_IS_DIRECTORY,
    PATH_IS_NOT_DIRECTORY,
    PATH_IS_MISSING,
} path_state_t;

static path_state_t
directory_state(core_string_t path)
{
    char *native = core_string_to_cstr(path);
    struct stat info;
    bool exists = native && !stat(native, &info);
    core_free(native);
    if (!exists) return PATH_IS_MISSING;
    return S_ISDIR(info.st_mode) ? PATH_IS_DIRECTORY : PATH_IS_NOT_DIRECTORY;
}

static n00bb_text_t
directory_problem(path_state_t state)
{
    return state == PATH_IS_MISSING ? N00B_TEXT_DOES_NOT_EXIST
                                    : N00B_TEXT_IS_NOT_A_DIRECTORY;
}

static void
describe_setting_source(core_buffer_t *out, n00bb_setting_id_t key)
{
    core_string_t origin = n00bb_setting_origin(key);
    switch (n00bb_setting_source(key)) {
    case N00BB_SETTING_FROM_ENVIRONMENT:
        core_buffer_append_string(out, origin);
        break;
    case N00BB_SETTING_FROM_FLAG:
        core_buffer_printf(out, "`%.*s`", (int)origin.u8_bytes, origin.data);
        break;
    default:
        core_buffer_printf(out, "`%.*s` (%.*s)",
                           (int)n00bb_setting_key(key).u8_bytes,
                           n00bb_setting_key(key).data,
                           (int)origin.u8_bytes, origin.data);
        break;
    }
}

static void
warn_contract_dirs(core_diagnostics_t *d)
{
    n00bb_setting_id_t key = N00BB_KEY_PATHS_CONTRACT_DIRS;
    bool from_flag = n00bb_setting_source(key) == N00BB_SETTING_FROM_FLAG;
    for (size_t i = 0; i < n00bb_setting_count(key); i++) {
        core_string_t value = n00bb_setting_at(key, i);
        path_state_t state = directory_state(value);
        if (state == PATH_IS_DIRECTORY) continue;
        if (from_flag)
            n00bb_report(d, nullptr, N00B_DIAG_CONTRACT_DIR_MISSING, nowhere,
                N00B_TEXT_CONTRACT_DIR_SO_IT_IS,
                N00BB_ARGS(ARG_TEXT(value),
                           ARG_PHRASE_ID(directory_problem(state))));
        else {
            core_buffer_t source = {0};
            describe_setting_source(&source, key);
            n00bb_report(d, nullptr, N00B_DIAG_CONTRACT_DIR_MISSING, nowhere,
                N00B_TEXT_CONTRACT_DIRECTORY_FROM_SO_IT,
                N00BB_ARGS(ARG_TEXT(value),
                           ARG_TEXT(core_buffer_string(&source)),
                           ARG_PHRASE_ID(directory_problem(state))));
            core_buffer_release(&source);
        }
    }
}

typedef void (*package_path_visit_t)(void *context, core_string_t entry,
                                     core_string_t resolved);

static void
each_package_path(package_path_visit_t visit, void *context)
{
    n00bb_setting_id_t key = N00BB_KEY_PATHS_PATH;
    core_string_t cwd = core_os_current_directory();
    for (size_t i = 0; i < n00bb_setting_count(key); i++) {
        core_string_t entry = n00bb_setting_at(key, i);
        core_string_t resolved = core_os_is_absolute(entry)
            ? core_string_dup(entry) : core_os_path_join(cwd, entry);
        visit(context, entry, resolved);
        core_free(resolved.data);
    }
    core_free(cwd.data);
}

static void
warn_package_path(void *context, core_string_t entry, core_string_t resolved)
{
    path_state_t state = directory_state(resolved);
    if (state == PATH_IS_DIRECTORY) return;
    core_buffer_t source = {0};
    describe_setting_source(&source, N00BB_KEY_PATHS_PATH);
    n00bb_report(context, nullptr, N00B_DIAG_PACKAGE_PATH_MISSING, nowhere,
        N00B_TEXT_ENTRY_SO_IT_IS_SKIPPED,
        N00BB_ARGS(ARG_TEXT(core_buffer_string(&source)), ARG_TEXT(entry),
                   ARG_PHRASE_ID(directory_problem(state))));
    core_buffer_release(&source);
}

static n00bb_text_t
cache_problem(core_string_t cache, int *failure)
{
    if (!cache.data) return N00B_TEXT_NO_CACHE_DIRECTORY;
    return usable_directory(cache, failure) ? N00B_TEXT_COUNT
                                            : N00B_TEXT_CACHE_NOT_USABLE;
}

static void
warn_environment(core_diagnostics_t *d, parse_cmdr_result_t *parsed)
{
    if (n00bb_handoff()->capture_fd >= 0
        || parse_cmdr_flag_present(parsed, CORE_STRING_STATIC("--build-time")))
        return;
    warn_contract_dirs(d);
    each_package_path(warn_package_path, d);
    core_string_t cache = n00bb_cache_directory(CORE_STRING_STATIC(""));
    int failure = 0;
    n00bb_text_t problem = cache_problem(cache, &failure);
    if (problem != N00B_TEXT_COUNT) {
        n00bb_setting_id_t key = N00BB_KEY_PATHS_CACHE;
        bool named = n00bb_setting_source(key) != N00BB_SETTING_FROM_DEFAULT;
        core_buffer_t source = {0};
        if (named) describe_setting_source(&source, key);
        n00bb_report(d, nullptr, N00B_DIAG_CACHE_UNUSABLE, nowhere,
            problem == N00B_TEXT_NO_CACHE_DIRECTORY ? problem
                : named ? N00B_TEXT_CANNOT_USE_CACHE_FROM
                        : N00B_TEXT_CANNOT_USE_CACHE,
            N00BB_ARGS(ARG_TEXT(cache), ARG_ERRNO(failure),
                       ARG_TEXT(core_buffer_string(&source))));
        n00bb_report_note(d, N00B_TEXT_THE_BUILD_CONTINUES_WITHOUT_A,
            N00BB_NO_ARGS);
        core_buffer_release(&source);
    }
    core_free(cache.data);
}

static int
check_build_request(const n00bb_invocation_t *invocation, bool *handled)
{
    parse_cmdr_result_t *parsed = parsed_or_report(invocation);
    if (!parsed) return 2;
    int status = 0;
    if (report_mistyped_command(parsed)) status = 2;
    core_diagnostics_t *d = core_diagnostics_new();
    if (!status) {
        bool roots_valid = check_native_roots(d, parsed);
        bool output_valid = check_library_output(d, parsed);
        if (!roots_valid || !output_valid) status = 2;
    }
    if (!status) warn_environment(d, parsed);
    n00bb_finish_command_diagnostics(d, status);
    *handled = status != 0;
    return status;
}

static void
register_link_problem_codes(void)
{
    static const struct {
        core_link_problem_t problem;
        n00bb_diag_t id;
    } codes[] = {
        {CORE_LINK_PROBLEM_SAME_MODULE, N00B_DIAG_SAME_MODULE_TWO_FILES},
        {CORE_LINK_PROBLEM_DUPLICATE_DEFINITION,
         N00B_DIAG_DUPLICATE_DEFINITION},
        {CORE_LINK_PROBLEM_CONFLICTING_DECLARATION,
         N00B_DIAG_CONFLICTING_DECLARATIONS},
        {CORE_LINK_PROBLEM_UNRESOLVED_REFERENCE,
         N00B_DIAG_UNRESOLVED_DECLARATION},
        {CORE_LINK_PROBLEM_DUPLICATE_ARTIFACT,
         N00B_DIAG_MODULE_SUPPLIED_TWICE},
        {CORE_LINK_PROBLEM_ARTIFACT_VERSION, N00B_DIAG_STALE_DESCRIPTION},
        {CORE_LINK_PROBLEM_INTERNAL, N00B_DIAG_INTERNAL},
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
        core_link_set_problem_code(codes[i].problem,
                                   n00bb_diag_code(codes[i].id));
}

int
n00bb_cli_preflight(n00bb_invocation_t *invocation, bool *handled)
{
    *handled = true;
    register_link_problem_codes();
    parse_cmdr_t *cli = invocation->cli;
    int at = invocation->command_at;
    if (invocation->argc <= 1) {
        print_summary(cli, stderr);
        return 2;
    }
    if (n00bb_invocation_flag(invocation, CORE_STRING_STATIC("--help-all"), (core_string_t){})) {
        parse_cmdr_print_all(cli, stdout);
        print_footer(stdout);
        return 0;
    }
    if (n00bb_invocation_flag(invocation, CORE_STRING_STATIC("--version"),
             CORE_STRING_STATIC("-V"))
        || n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("version"))) {
        if (n00bb_invocation_flag(invocation, CORE_STRING_STATIC("--marker"), (core_string_t){}))
            n00bb_print_toolchain_marker(stdout);
        else
            n00bb_print_version(stdout);
        return 0;
    }
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("help"))) return help_command(invocation);
    if (n00bb_invocation_flag(invocation, CORE_STRING_STATIC("--help"),
             CORE_STRING_STATIC("-h"))) {
        if (!at) print_summary(cli, stdout);
        else parse_cmdr_print_command_help(cli, invocation->words[at], stdout);
        return 0;
    }
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("explain")) && at + 1 < invocation->end)
        return n00bb_explain_command(invocation->words[at + 1]);
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("completions")) && at + 1 < invocation->end) {
        static const core_string_t shells[] = {
            CORE_STRING_STATIC("bash"), CORE_STRING_STATIC("zsh"),
            CORE_STRING_STATIC("fish"), CORE_STRING_STATIC("powershell"),
        };
        core_string_t shell = invocation->words[at + 1];
        return n00bb_print_completions(stdout, shell) ? 0
            : n00bb_unknown_action(CORE_STRING_STATIC("completions"), shell,
                                   shells, sizeof(shells) / sizeof(shells[0]));
    }
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("run"))) return run_command(invocation);
    int settings_status = n00bb_config_report();
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("env")))
        return n00bb_env_command(invocation->argv0);
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("config"))) return n00bb_config_command();
    if (n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("toolchain")) || n00bb_invocation_command_is(invocation, CORE_STRING_STATIC("cache")))
        return manage_command(invocation);
    if (settings_status) return settings_status;
    return check_build_request(invocation, handled);
}

bool
n00bb_stderr_color(void)
{
    core_string_t mode = n00bb_setting_string(
        N00BB_KEY_DIAGNOSTICS_COLOR);
    if (core_string_eq(mode, CORE_STRING_STATIC("always"))) return true;
    if (core_string_eq(mode, CORE_STRING_STATIC("never"))) return false;
    core_string_t terminal = core_os_environment(CORE_STRING_STATIC("TERM"));
    bool dumb = core_string_eq(terminal, CORE_STRING_STATIC("dumb"));
    core_free(terminal.data);
    if (dumb) return false;
    return isatty(STDERR_FILENO);
}

core_diag_render_t
n00bb_diag_render_options(void)
{
    bool terse = n00bb_setting_bool(N00BB_KEY_DIAGNOSTICS_TERSE);
    bool forced = core_string_eq(
        n00bb_setting_string(N00BB_KEY_DIAGNOSTICS_COLOR),
        CORE_STRING_STATIC("always"));
    return (core_diag_render_t){
        .color = (forced || !terse) && n00bb_stderr_color(),
        .terse = terse,
        .hints = n00bb_setting_bool(N00BB_KEY_DIAGNOSTICS_HINTS),
        .sorted = true,
        .words = n00bb_diag_words(),
    };
}

void
n00bb_print_diagnostics(const core_diagnostics_t *diagnostics)
{
    core_string_t text = core_diagnostics_render(diagnostics,
                                                 n00bb_diag_render_options());
    if (text.data) {
        fwrite(text.data, 1, text.u8_bytes, stderr);
        core_free(text.data);
    }
}

static int env_problems;

static void
env_line(const char *label, core_string_t value, const char *problem)
{
    if (problem) env_problems++;
    if (value.data && problem)
        printf("  %-10s %.*s  (problem: %s)\n", label, (int)value.u8_bytes,
               value.data, problem);
    else if (problem)
        printf("  %-10s (problem: %s)\n", label, problem);
    else
        printf("  %-10s %.*s\n", label, (int)value.u8_bytes, value.data);
}

static void
env_row(const char *label, core_string_t value, bool present,
        const char *missing)
{
    env_line(label, value, value.data && present ? nullptr : missing);
}

static void
env_tool(const char *label, const char *path, n00bb_setting_id_t key,
         const char *missing)
{
    core_string_t value = core_string_from_cstr(path);
    core_buffer_t problem = {0};
    if (!path) core_buffer_printf(&problem, "%s", missing);
    else if (!n00bb_toolchain_runnable(path)) {
        if (n00bb_setting_source(key) != N00BB_SETTING_FROM_DEFAULT) {
            describe_setting_source(&problem, key);
            core_buffer_puts(&problem, " does not name a runnable program");
        }
        else core_buffer_puts(&problem, "not a runnable program");
    }
    char *text = problem.byte_len
        ? core_string_to_cstr(core_buffer_string(&problem)) : nullptr;
    env_line(label, value, text);
    core_free(text);
    core_buffer_release(&problem);
    core_free(value.data);
}

static void
env_package_path(void *context, core_string_t entry, core_string_t resolved)
{
    (void)context;
    path_state_t state = directory_state(resolved);
    const char *problem = state == PATH_IS_MISSING ? "does not exist"
        : state == PATH_IS_NOT_DIRECTORY ? "not a directory" : nullptr;
    if (!problem && !core_os_is_absolute(entry)) {
        core_string_t cwd = core_os_current_directory();
        core_string_t canonical = core_os_real_path(resolved);
        core_string_t real_cwd = core_os_real_path(cwd);
        bool inside = canonical.data && real_cwd.data
            && core_string_starts_with(canonical, real_cwd)
            && (canonical.u8_bytes == real_cwd.u8_bytes
                || core_os_is_separator(canonical.data[real_cwd.u8_bytes]));
        if (!inside)
            problem = "a relative entry must stay inside the current "
                      "directory";
        core_free(real_cwd.data);
        core_free(canonical.data);
        core_free(cwd.data);
    }
    env_line("N00B_PATH", entry, problem);
}

static void
env_package_roots(void)
{
    const n00bb_toolchain_t *toolchain = n00bb_toolchain();
    printf("\npackage roots\n");
    printf("  %-10s %.*s\n", "stdlib", (int)toolchain->stdlib.u8_bytes,
           toolchain->stdlib.data);
    core_string_t cwd = core_os_current_directory();
    printf("  %-10s %.*s\n", "default", (int)cwd.u8_bytes, cwd.data);
    core_free(cwd.data);
    each_package_path(env_package_path, nullptr);
}

static void
env_path_check(core_string_t argv0)
{
    core_string_t self = core_os_executable_path(argv0);
    char *found = n00bb_toolchain_find_on_path(CORE_STRING_STATIC("n00b"));
    core_string_t found_path = core_string_from_cstr(found);
    core_string_t first = found ? core_os_real_path(found_path)
                                : (core_string_t){0};
    core_free(found_path.data);
    printf("\nPATH\n");
    if (!found)
        printf("  n00b is not on PATH; add the directory holding it to "
               "PATH to run it by name\n");
    else if (self.data && core_string_eq(first, self))
        printf("  n00b on PATH is this toolchain\n");
    else {
        env_problems++;
        printf("  n00b on PATH is %s, not this one (%.*s)\n", found,
               (int)self.u8_bytes, self.data);
    }
    free(found);
    core_free(first.data);
    core_free(self.data);
}

int
n00bb_env_command(core_string_t argv0)
{
    const n00bb_toolchain_t *toolchain = n00bb_toolchain();
    n00bb_print_version(stdout);
    printf("\ntoolchain\n");
    core_string_t selection = n00bb_toolchain_selection_problem();
    if (selection.data) {
        char *problem = core_string_to_cstr(selection);
        env_line("selection", (core_string_t){0}, problem);
        core_free(problem);
    }
    env_row("stdlib", toolchain->stdlib,
            core_os_is_directory(toolchain->stdlib),
            "the standard library is not installed");
    core_string_t archive = core_os_path_join(toolchain->runtime,
        CORE_STRING_STATIC(N00BB_RUNTIME_ARCHIVE));
    env_row("runtime", archive, core_os_is_regular_file(archive),
            "the runtime library is not installed");
    core_free(archive.data);
    env_row("grammar", toolchain->grammar,
            core_os_is_regular_file(toolchain->grammar),
            "the grammar is not installed");
    env_row("profiles", toolchain->writ_profiles,
            core_os_is_directory(toolchain->writ_profiles),
            "writ profiles are not installed");
    env_row("contracts", toolchain->writ_contracts,
            core_os_is_directory(toolchain->writ_contracts),
            "writ contracts are not installed");
    printf("\ntools\n");
    n00bb_setting_id_t clang_key = N00BB_KEY_TOOLS_CLANG;
    env_tool("clang", toolchain->clang, clang_key,
             "install LLVM or set N00B_CLANG");
    if (n00bb_setting_source(clang_key) != N00BB_SETTING_FROM_DEFAULT) {
        core_buffer_t source = {0};
        describe_setting_source(&source, clang_key);
        printf("  %-10s %s  (%.*s, as above)\n", "linker", toolchain->linker,
               (int)source.byte_len, source.data);
        core_buffer_release(&source);
    }
    else
        env_tool("linker", toolchain->linker, clang_key,
                 "install LLVM or set N00B_CLANG");
    if (toolchain->cargo)
        env_tool("cargo", toolchain->cargo, N00BB_KEY_TOOLS_CARGO,
                 "");
    else
        printf("  %-10s (off: rebuild with N00B_RUST=1 for Rust transports)\n",
               "cargo");
    env_package_roots();
    core_string_t cache = n00bb_cache_directory(CORE_STRING_STATIC(""));
    int failure = 0;
    n00bb_text_t problem = cache_problem(cache, &failure);
    core_string_t problem_text = problem == N00B_TEXT_COUNT
        ? (core_string_t){0}
        : n00bb_text(problem == N00B_TEXT_NO_CACHE_DIRECTORY
                         ? problem : N00B_TEXT_NOT_USABLE,
                     N00BB_ARGS(ARG_ERRNO(failure)));
    char *problem_native = problem_text.data
        ? core_string_to_cstr(problem_text) : nullptr;
    printf("\ncache\n");
    env_line("directory", cache, problem_native);
    core_free(problem_native);
    core_free(problem_text.data);
    core_free(cache.data);
    env_path_check(argv0);
    if (env_problems)
        printf("\n%d problem%s found\n", env_problems,
               env_problems == 1 ? "" : "s");
    return env_problems ? 1 : 0;
}
