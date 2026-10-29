#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <unistd.h>

extern char **environ;

double n00bb_profile_seconds(void);

#include "driver.h"
#include "handoff.h"
#include "lib/os.h"
#include "lib/process.h"
#include "compiler_cli.h"
#include "config.h"
#include "diagnostics.h"
#include "syntax_diagnostics.h"
#include "toolchain_commands.h"
#include "compiler_native_roots.h"
#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_ir_setup.h"
#include "compiler_lower.h"
#include "compiler_cache.h"
#include "compiler_layout.h"
#include "compiler_phases.h"
#include "compiler_abi_driver.h"
#include "compiler_tu_interface.h"
#include "abi_layout.h"
#include "abi_emit.h"
#include "abi_link.h"
#include "abi_semantics.h"
#include "abi_static_graph.h"
#include "abi_record_layout.h"
#include "record_layout_request.h"
#include "enum_value.h"
#include "flagset_value.h"
#include "frame_roots.h"
#include "global_metadata.h"
#include "ir/abcd.h"
#include "llvm_backend.h"
#include "llvm_native_cache.h"
#include "native_frame_maps.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"
#include "storage_selection.h"
#include "specialize.h"
#include "box_calls.h"
#include "compiler_ffi.h"
#include "compiler_baked.h"
#include "lib/alloc.h"
#include "lib/arena.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "lib/list.h"
#include "lib/link_unit.h"
#include "lib/indexed_table.h"
#include "lib/target.h"
#include "lib/object_section.h"
#include "lib/work_queue.h"
#include "diag/diagnostics.h"
#include "parse/commander.h"
#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_dump.h"
#include "parse/tree_print.h"
#include "parse/tree.h"
#include "parse/tree_util.h"
#include "parse/annot_walk.h"
#include "ir/ir.h"
#include "ir/analyses.h"
#include "ir/exec_graph.h"
#include "ir/doms.h"
#include "ir/analyze.h"
#include "ir/layout.h"
#include "ir/typelang.h"
#include "th/environment.h"
#include "scanner/token_stream.h"
#include "scanner/tokenizer.h"
#include "scanner/lex.h"
#include "unicode/encoding.h"
#include "preprocessor.h"
#include "signature_typevars.h"

enum { N00BB_JOB_LOCATE_TU = 1, N00BB_JOB_PREPROCESS = 2 };

typedef struct {
    core_work_job_t *job;
    core_string_t operand;
    bool root_input;
    bool manifest_probe;
    core_module_resolve_status_t resolve_status;
    bool resolve_attempted;
} locate_request_t;
core_list_decl(locate_request_t);

typedef struct {
    core_list_t(locate_request_t) ordered;
    core_dict_t by_job;
    core_diagnostics_t *diagnostics;
} locate_requests_t;

typedef struct {
    n00bb_resolver_t *resolver;
    const n00bb_cache_t *cache;
    bool package_warnings;
    bool changed_files_only;
} warning_policy_t;

static bool closed_world_build;
static bool function_summaries_build;

static bool
same_diagnostic(const core_diagnostic_t *left, const core_diagnostic_t *right)
{
    return left->severity == right->severity
        && core_string_eq(left->code, right->code)
        && core_string_eq(left->message, right->message)
        && core_string_eq(left->path, right->path)
        && left->location.line == right->location.line
        && left->location.column == right->location.column;
}

static bool
already_reported(const core_diagnostics_t *diagnostics,
                 const core_diagnostic_t *item)
{
    for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++)
        if (same_diagnostic(core_diagnostics_at(diagnostics, i), item))
            return true;
    return false;
}

static void
append_diagnostics(core_diagnostics_t *into, const core_diagnostics_t *from,
                   bool keep_warnings, bool skip_reported)
{
    bool keep = true;
    for (size_t i = 0; i < core_diagnostics_count(from); i++) {
        const core_diagnostic_t *item = core_diagnostics_at(from, i);
        bool attached = item->severity == CORE_DIAG_NOTE
            || item->severity == CORE_DIAG_HELP;
        if (!attached)
            keep = (!core_diag_is_advisory(item->severity) || keep_warnings)
                && !(skip_reported && already_reported(into, item));
        if (keep) core_diagnostics_append(into, item);
    }
}

static void
merge_unit_diagnostics(core_diagnostics_t *into,
                       const warning_policy_t *policy,
                       core_translation_unit_t *unit, bool local)
{
    append_diagnostics(into, core_translation_unit_diagnostics(unit),
                       local || policy->package_warnings, false);
    n00bb_tu_state_t *state = tu_state(unit);
    if (!local || policy->changed_files_only || !policy->cache || !state
        || !state->interface_module)
        return;
    core_diagnostics_t *replayed = core_diagnostics_new();
    if (n00bb_cache_replay_warnings(policy->cache, unit, replayed))
        append_diagnostics(into, replayed, true, true);
    core_diagnostics_free(replayed);
}

typedef core_work_job_t *core_work_job_ptr_t;
core_list_decl(core_work_job_ptr_t);
core_list_decl(size_t);

typedef struct {
    core_work_queue_t *queue;
    n00bb_resolver_t *resolver;
    locate_requests_t *requests;
    pthread_mutex_t *requests_lock;
    parse_grammar_t *grammar;
    th_ctx *types;
    n00bb_interface_context_t interfaces;
    const n00bb_cache_t *cache;
    bool lex;
    bool parse;
} locate_worker_context_t;

typedef struct {
    core_work_queue_t *queue;
    n00bb_resolver_t *resolver;
    locate_requests_t *requests;
    pthread_mutex_t *requests_lock;
    locate_worker_context_t *loader;
    size_t worker_count;
    parse_grammar_t *grammar;
    n00bb_preprocessor_t *preprocessor;
    th_ctx *types;
    const n00bb_module_generators_t *generators;
} import_driver_t;

typedef core_module_spec_t n00bb_module_candidate_t;
core_list_decl(n00bb_module_candidate_t);

typedef struct {
    core_list_t(n00bb_module_candidate_t) modules;
} n00bb_discovery_candidates_t;

static locate_request_t *
request_for_job(locate_requests_t *requests, core_work_job_t *job)
{
    size_t index = (size_t)(uintptr_t)core_dict_get(&requests->by_job, job,
                                                    nullptr);
    return index ? &requests->ordered.data[index - 1] : nullptr;
}

static bool
request_snapshot(locate_requests_t *requests,
                 pthread_mutex_t *requests_lock, core_work_job_t *job,
                 core_string_t *operand, bool *root_input)
{
    if (requests_lock) pthread_mutex_lock(requests_lock);
    locate_request_t *request = request_for_job(requests, job);
    if (request) {
        *operand = core_string_dup(request->operand);
        *root_input = request->root_input;
    }
    if (requests_lock) pthread_mutex_unlock(requests_lock);
    return request != nullptr;
}

static bool
add_locate_request_for_module(core_work_queue_t *queue,
                   locate_requests_t *requests,
                   pthread_mutex_t *requests_lock,
                   const core_module_spec_t *module, core_string_t operand,
                   bool root_input, bool allow_existing,
                   bool manifest_probe, core_work_job_t **job_out)
{
    if (requests_lock) pthread_mutex_lock(requests_lock);
    core_work_job_t *job = nullptr;
    core_work_submit_result_t submitted = core_work_submit(queue,
        N00BB_JOB_LOCATE_TU, module, 0, nullptr, 0, &job);
    if (submitted == CORE_WORK_INVALID
        || submitted == CORE_WORK_DEPENDENCY_FAILED) {
        n00bb_internal_error(requests->diagnostics, nullptr,
            (core_diag_location_t){}, "scheduling modules",
            "could not schedule `%.*s`", (int)operand.u8_bytes,
            operand.data);
        if (requests_lock) pthread_mutex_unlock(requests_lock);
        return false;
    }

    locate_request_t *prior = request_for_job(requests, job);
    if (prior) {
        if (!allow_existing && !core_string_eq(prior->operand, operand)) {
            core_string_t name = core_work_job_module(job)->identifier;
            n00bb_report(requests->diagnostics, nullptr,
                N00B_DIAG_DUPLICATE_INPUT_MODULE, (core_diag_location_t){},
                N00B_TEXT_AND_BOTH_NAME_MODULE,
                N00BB_ARGS(ARG_TEXT(prior->operand), ARG_TEXT(operand),
                           ARG_TEXT(name)));
            if (requests_lock) pthread_mutex_unlock(requests_lock);
            return false;
        }
        prior->root_input |= root_input;
        prior->manifest_probe &= manifest_probe;
        if (job_out) *job_out = job;
        if (requests_lock) pthread_mutex_unlock(requests_lock);
        return true;
    }
    core_list_push(requests->ordered, ((locate_request_t){
        .job = job,
        .operand = core_string_dup(operand),
        .root_input = root_input,
        .manifest_probe = manifest_probe,
    }));
    core_dict_add(&requests->by_job, job,
                  (void *)(uintptr_t)requests->ordered.len);
    if (job_out) *job_out = job;
    if (requests_lock) pthread_mutex_unlock(requests_lock);
    return true;
}

static bool
add_locate_request(core_work_queue_t *queue,
                   locate_requests_t *requests,
                   core_string_t operand)
{
    core_module_spec_t module = {0};
    if (!n00bb_operand_module_spec(operand, &module)) {
        n00bb_report(requests->diagnostics, nullptr,
            N00B_DIAG_INVALID_INPUT_NAME, (core_diag_location_t){},
            N00B_TEXT_IS_NEITHER_A_FILE_PATH,
            N00BB_ARGS(ARG_TEXT(operand)));
        n00bb_report_help(requests->diagnostics, N00B_TEXT_PASS_A_PATH_TO_A,
            N00BB_NO_ARGS);
        return false;
    }
    bool ok = add_locate_request_for_module(queue, requests, nullptr, &module,
        operand, true, false, false, nullptr);
    core_module_spec_release(&module);
    return ok;
}

static size_t printed_errors;
static size_t printed_warnings;

static void
print_counted_diagnostics(const core_diagnostics_t *diagnostics,
                          n00bb_diagnostic_filter_t *filter)
{
    for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++) {
        const core_diagnostic_t *item = core_diagnostics_at(diagnostics, i);
        if (filter && !n00bb_diagnostic_visible(filter, item)) continue;
        if (item->severity == CORE_DIAG_WARNING) printed_warnings++;
        else if (item->severity == CORE_DIAG_ERROR
                 || item->severity == CORE_DIAG_FATAL
                 || item->severity == CORE_DIAG_INTERNAL)
            printed_errors++;
    }
    n00bb_print_diagnostics(diagnostics);
}

static void
count_replayed_headers(core_string_t text)
{
    size_t start = 0;
    while (start < text.u8_bytes) {
        int64_t newline = core_string_find_byte(
            core_string_slice(text, start, text.u8_bytes - start), '\n');
        size_t length = newline < 0 ? text.u8_bytes - start : (size_t)newline;
        core_string_t line = core_string_slice(text, start, length);
        start += length + 1;
        if (!line.u8_bytes || line.data[0] == ' ' || line.data[0] == '|'
            || (line.data[0] >= '0' && line.data[0] <= '9'))
            continue;
        if (core_string_starts_with(line, CORE_STRING_STATIC("error"))
            || core_string_starts_with(line,
                   CORE_STRING_STATIC("internal compiler error"))
            || core_string_find(line, CORE_STRING_STATIC(": error")) >= 0
            || core_string_find(line,
                   CORE_STRING_STATIC(": internal compiler error")) >= 0)
            printed_errors++;
        else if (core_string_starts_with(line, CORE_STRING_STATIC("warning"))
                 || core_string_find(line, CORE_STRING_STATIC(": warning"))
                        >= 0)
            printed_warnings++;
    }
}

static void
print_build_summary(core_string_t input)
{
    if (n00bb_diag_render_options().terse && (printed_errors || printed_warnings))
        return;
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    core_buffer_t counts = {0};
    if (printed_errors)
        core_buffer_printf(&counts, "%zu error%s", printed_errors,
                           printed_errors == 1 ? "" : "s");
    if (printed_warnings)
        core_buffer_printf(&counts, "%s%zu warning%s",
                           printed_errors ? ", " : "", printed_warnings,
                           printed_warnings == 1 ? "" : "s");
    if (counts.byte_len)
        n00bb_report(diagnostics, nullptr, N00B_DIAG_BUILD_FAILED,
            (core_diag_location_t){}, N00B_TEXT_COULD_NOT_BUILD,
            N00BB_ARGS(ARG_TEXT(input), ARG_TEXT(core_buffer_string(&counts))));
    else
        n00bb_report(diagnostics, nullptr, N00B_DIAG_BUILD_FAILED,
            (core_diag_location_t){}, N00B_TEXT_COULD_NOT_BUILD_2,
            N00BB_ARGS(ARG_TEXT(input)));
    n00bb_print_diagnostics(diagnostics);
    core_diagnostics_free(diagnostics);
    core_buffer_release(&counts);
}

static void
report_unreadable_input(core_diagnostics_t *diagnostics,
                        n00bb_resolver_t *resolver, core_string_t operand,
                        bool not_found)
{
    core_os_file_info_t status = {};
    errno = 0;
    core_os_lookup_t lookup = core_os_stat(operand, true, &status);
    bool exists = lookup == CORE_OS_FOUND;
    int stat_error = exists ? 0
        : lookup == CORE_OS_MISSING ? ENOENT : (errno ? errno : EIO);
    int open_error = 0;
    bool directory = exists && status.kind == CORE_OS_KIND_DIRECTORY;
    if (exists && !directory) {
        char *bytes = nullptr;
        size_t size = 0;
        errno = 0;
        if (core_os_read_file(operand, &bytes, &size)) core_free(bytes);
        else open_error = errno ? errno : EIO;
    }
    if (directory) {
        n00bb_report(diagnostics, nullptr, N00B_DIAG_INPUT_UNREADABLE,
            (core_diag_location_t){}, N00B_TEXT_CANNOT_READ_IT_IS_A,
            N00BB_ARGS(ARG_TEXT(operand)));
        n00bb_report_help(diagnostics, N00B_TEXT_PASS_A_N_FILE_INSIDE,
            N00BB_NO_ARGS);
        return;
    }
    if (exists || (stat_error && stat_error != ENOENT
                   && stat_error != ENOTDIR)) {
        n00bb_report(diagnostics, nullptr, N00B_DIAG_INPUT_UNREADABLE,
            (core_diag_location_t){}, N00B_TEXT_CANNOT_READ,
            N00BB_ARGS(ARG_TEXT(operand), ARG_ERRNO(open_error ? open_error
                            : stat_error ? stat_error : EIO)));
        return;
    }
    n00bb_report(diagnostics, nullptr, N00B_DIAG_INPUT_NOT_FOUND,
        (core_diag_location_t){}, N00B_TEXT_CANNOT_FIND,
        N00BB_ARGS(ARG_TEXT(operand)));
    if (not_found) n00bb_resolver_note_search(resolver, operand, diagnostics);
}

static void
dump_token(parse_grammar_t *grammar, core_translation_unit_t *unit,
           parse_token_info_t *token)
{
    core_string_t path = core_source_file_path(&unit->source);
    const char *source = core_source_file_contents(&unit->source).data;
    size_t source_size = core_source_file_size(&unit->source);
    core_string_t name = parse_terminal_name(grammar, parse_token_tid(token));
    size_t token_end = parse_token_end_byte(token);
    size_t token_start = parse_token_start_byte(token);
    size_t end = token_end > source_size ? source_size : token_end;
    size_t start = token_start <= end ? token_start : end;
    printf("%.*s:%u:%u %.*s ", (int)path.u8_bytes, path.data,
           parse_token_line(token), parse_token_column(token),
           (int)name.u8_bytes, name.data);
    fputc('"', stdout);
    for (size_t j = start; j < end; j++) {
        unsigned char ch = (unsigned char)source[j];
        if (ch == '\\' || ch == '"') { fputc('\\', stdout); fputc(ch, stdout); }
        else if (ch == '\n') fputs("\\n", stdout);
        else if (ch == '\r') fputs("\\r", stdout);
        else if (ch == '\t') fputs("\\t", stdout);
        else if (ch < 0x20 || ch == 0x7f) printf("\\x%02x", ch);
        else fputc(ch, stdout);
    }
    fputc('"', stdout);
    if (core_option_is_set(parse_token_postfix_modifier(token))) {
        core_string_t modifier =
            core_option_get(parse_token_postfix_modifier(token));
        printf(" modifier='%.*s", (int)modifier.u8_bytes, modifier.data);
    }
    fputc('\n', stdout);
}

static void
dump_tree(parse_grammar_t *grammar, parse_tree_t *tree)
{
    core_string_t text = parse_tree_dump_str(grammar, tree, false);
    if (text.data) {
        fwrite(text.data, 1, text.u8_bytes, stdout);
        core_free(text.data);
    }
}

#define TEXT_ARGS(text) (int)(text).u8_bytes, (text).data

static core_string_t
symbol_kind_name(parse_sym_kind_t kind)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("variable"), CORE_STRING_STATIC("function"),
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("tag"),
        CORE_STRING_STATIC("enum-constant"), CORE_STRING_STATIC("label"),
        CORE_STRING_STATIC("parameter"), CORE_STRING_STATIC("module"),
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : CORE_STRING_STATIC("unknown");
}

static core_string_t
symbol_mutability_name(parse_sym_mutability_t mutability)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("mutable"), CORE_STRING_STATIC("immutable"),
        CORE_STRING_STATIC("const"), CORE_STRING_STATIC("global"),
    };
    return (size_t)mutability < sizeof(names) / sizeof(names[0])
        ? names[mutability] : CORE_STRING_STATIC("unknown");
}

static core_string_t
symbol_phase_name(parse_sym_phase_t phase)
{
    switch (phase) {
    case PARSE_SYM_PHASE_NONE: return CORE_STRING_STATIC("none");
    case PARSE_SYM_PHASE_COMPTIME: return CORE_STRING_STATIC("comptime");
    case PARSE_SYM_PHASE_RUNTIME: return CORE_STRING_STATIC("runtime");
    case PARSE_SYM_PHASE_BOTH: return CORE_STRING_STATIC("comptime+runtime");
    }
    return CORE_STRING_STATIC("invalid");
}

static core_string_t
static_value_kind_name(n00bb_static_value_kind_t kind)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("bool"), CORE_STRING_STATIC("int"),
        CORE_STRING_STATIC("float"), CORE_STRING_STATIC("string"),
        CORE_STRING_STATIC("sequence"), CORE_STRING_STATIC("tuple"),
        CORE_STRING_STATIC("record"), CORE_STRING_STATIC("nil"),
        CORE_STRING_STATIC("variant")};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : CORE_STRING_STATIC("invalid");
}

static core_string_t
shape_extent_kind_name(ir_shape_extent_kind_t kind)
{
    return kind == IR_SHAPE_EXTENT_STATIC ? CORE_STRING_STATIC("static")
        : kind == IR_SHAPE_EXTENT_COMPTIME ? CORE_STRING_STATIC("comptime")
        : kind == IR_SHAPE_EXTENT_RUNTIME ? CORE_STRING_STATIC("runtime")
        : CORE_STRING_STATIC("unresolved");
}

static void
dump_static_value_contents(const n00bb_static_value_t *value, size_t depth)
{
    if (!value) return;
    switch (value->kind) {
    case N00BB_STATIC_VALUE_BOOL:
        printf("%s", value->as.boolean ? "true" : "false");
        break;
    case N00BB_STATIC_VALUE_INT: {
        __uint128_t bits = (__uint128_t)value->as.integer;
        printf("%016llx%016llx",
            (unsigned long long)(bits >> 64),
            (unsigned long long)bits);
        break;
    }
    case N00BB_STATIC_VALUE_FLOAT:
        printf("%a", value->as.real);
        break;
    case N00BB_STATIC_VALUE_STRING:
        printf("%zu bytes", value->as.string.u8_bytes);
        break;
    case N00BB_STATIC_VALUE_NIL:
        printf("nil");
        break;
    case N00BB_STATIC_VALUE_ADDRESS:
        printf("ref %.*s", (int)value->as.address->name.u8_bytes,
               value->as.address->name.data);
        break;
    case N00BB_STATIC_VALUE_VARIANT:
        printf("%.*s=", (int)value->as.variant.arm.u8_bytes,
               value->as.variant.arm.data);
        dump_static_value_contents(value->as.variant.payload, depth + 1);
        break;
    case N00BB_STATIC_VALUE_SEQUENCE:
    case N00BB_STATIC_VALUE_TUPLE:
    case N00BB_STATIC_VALUE_RECORD:
        printf("[");
        for (size_t i = 0; i < value->as.aggregate.count; i++) {
            if (i) printf(",");
            dump_static_value_contents(&value->as.aggregate.items[i],
                                       depth + 1);
        }
        printf("]");
        break;
    }
}

static void
dump_static_value(const n00bb_static_value_t *value)
{
    if (!value) return;
    printf(" static-value=%.*s:",
           TEXT_ARGS(static_value_kind_name(value->kind)));
    dump_static_value_contents(value, 0);
}

static void
dump_symbols(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    parse_symtab_t *symtab = state && state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return;

    core_string_t module = unit->module.identifier;
    printf("symbols %.*s:\n", (int)module.u8_bytes, module.data);
    printf("  imports:");
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++) {
        core_string_t imported =
            core_translation_unit_import(unit, i)->module.identifier;
        printf(" %.*s", (int)imported.u8_bytes, imported.data);
    }
    putchar('\n');
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        printf("  scope #%d depth=%d namespace=%.*s tag=%.*s\n",
               scope->id, scope->depth,
               (int)scope->ns->ns_name.u8_bytes,
               scope->ns->ns_name.data ? scope->ns->ns_name.data : "",
               (int)scope->scope_tag.u8_bytes,
               scope->scope_tag.data ? scope->scope_tag.data : "");
        core_list_t(parse_sym_entry_ptr_t) symbols =
            core_list_new(parse_sym_entry_ptr_t);
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope)
            core_list_push(symbols, symbol);
        for (size_t i = symbols.len; i > 0; i--) {
            parse_sym_entry_t *symbol = symbols.data[i - 1];
            core_string_t kind_name = symbol_kind_name(symbol->kind);
            core_string_t mutability_name =
                symbol_mutability_name(symbol->mutability);
            printf("    %.*s: %.*s, %.*s", (int)symbol->name.u8_bytes,
                   symbol->name.data, TEXT_ARGS(kind_name),
                   TEXT_ARGS(mutability_name));
            if (symbol->type && state->types) {
                core_string_t printed = n00bb_print_type_qualified(state->types,
                                                         symbol->type);
                printf(", type=%.*s", TEXT_ARGS(printed));
            }
            else if (symbol->type)
                printf(", type=<known>");
            else
                printf(", type=<unset>");
            ir_shape_t *array_shape = parse_sym_binding(symbol,
                CORE_STRING_STATIC("ir.array-shape"));
            if (array_shape) {
                printf(", array-shape=rank:%u",
                       ir_shape_rank(array_shape));
                for (uint32_t d = 0; d < ir_shape_rank(array_shape); d++) {
                    ir_shape_extent_t extent;
                    if (!ir_shape_extent(array_shape, d, &extent)) continue;
                    core_string_t kind = shape_extent_kind_name(extent.kind);
                    printf(", dim%u=%.*s", d, TEXT_ARGS(kind));
                    if (extent.producer)
                        printf("(producer=%.*s)",
                               TEXT_ARGS(extent.producer->name));
                    else if (extent.producer_identifier.data)
                        printf("(producer=%.*s)",
                               TEXT_ARGS(extent.producer_identifier));
                }
            }
            if (symbol->member_scope)
                printf(", member-scope=#%d", symbol->member_scope->id);
            if (symbol->owner_type)
                printf(", owner-type=%.*s",
                       (int)symbol->owner_type->name.u8_bytes,
                       symbol->owner_type->name.data);
            if (symbol->kind == PARSE_SYM_ENUM_CONST &&
                symbol->has_comptime_value && symbol->comptime_value) {
                bool flagset = symbol->owner_type
                    && symbol->owner_type->member_scope
                    && core_string_eq(
                        symbol->owner_type->member_scope->scope_tag,
                        CORE_STRING_STATIC("flagset"));
                if (flagset) {
                    const n00bb_flagset_value_t *flag =
                        symbol->comptime_value;
                    printf(", mask=0x%016llx%016llx, flag-kind=%s",
                        (unsigned long long)(flag->mask >> 64),
                        (unsigned long long)flag->mask,
                        flag->atomic ? "atomic" : "composite");
                }
                else {
                    const n00bb_enum_value_t *enum_value =
                        symbol->comptime_value;
                    printf(", value=%s0x%016llx%016llx",
                        enum_value->negative ? "-" : "",
                        (unsigned long long)(enum_value->magnitude >> 64),
                        (unsigned long long)enum_value->magnitude);
                }
            }
            if (symbol->type_is_inferred)
                printf(", type-source=inferred");
            const n00bb_enum_range_t *enum_range = parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.enum-range"));
            if (enum_range) {
                if (!enum_range->magnitude_bits)
                    printf(", enum-value-range=zero-only");
                else
                    printf(", enum-value-range=%s:%llu",
                        enum_range->is_signed ? "i" : "u",
                        (unsigned long long)(enum_range->magnitude_bits
                            + enum_range->is_signed));
            }
            if (symbol->visibility.data)
                printf(", visibility=%.*s", (int)symbol->visibility.u8_bytes,
                       symbol->visibility.data);
            if (symbol->error_type && state->types) {
                core_string_t printed = n00bb_print_type_qualified(state->types,
                                                         symbol->error_type);
                printf(", error=%.*s", TEXT_ARGS(printed));
            }
            if (symbol->direction.data)
                printf(", direction=%.*s", (int)symbol->direction.u8_bytes,
                       symbol->direction.data);
            if (symbol->pass_by.data)
                printf(", pass_by=%.*s", (int)symbol->pass_by.u8_bytes,
                       symbol->pass_by.data);
            if (symbol->is_optional) printf(", optional");
            if (symbol->is_variadic) printf(", variadic");
            if (symbol->error_type && state->types) {
                core_string_t printed = n00bb_print_type_qualified(state->types,
                                                         symbol->error_type);
                printf(", error=%.*s", TEXT_ARGS(printed));
            }
            if (symbol->direction.data)
                printf(", direction=%.*s", (int)symbol->direction.u8_bytes,
                       symbol->direction.data);
            if (symbol->pass_by.data)
                printf(", pass_by=%.*s", (int)symbol->pass_by.u8_bytes,
                       symbol->pass_by.data);
            if (symbol->is_thread_local) printf(", thread-local");
            core_string_t phases = symbol_phase_name(symbol->phases);
            core_string_t mutable_phases =
                symbol_phase_name(symbol->mutable_phases);
            core_string_t initializer_phases =
                symbol_phase_name(symbol->initializer_phases);
            printf(", phases=%.*s, mutable-in=%.*s, initializer-in=%.*s",
                   TEXT_ARGS(phases), TEXT_ARGS(mutable_phases),
                   TEXT_ARGS(initializer_phases));
            core_string_t storage_duration = parse_sym_storage_duration_name(
                symbol->storage_duration);
            printf(", storage-duration=%.*s", TEXT_ARGS(storage_duration));
            printf(", lifetime-region=%llu, aliases=%zu, aliased-by=%zu",
                   (unsigned long long)symbol->lifetime.region,
                   parse_sym_alias_count(symbol),
                   parse_sym_aliased_by_count(symbol));
            size_t complete_lifetime_ranges = 0;
            for (size_t r = 0;
                 r < parse_sym_lifetime_range_count(symbol); r++) {
                const parse_sym_lifetime_range_t *range =
                    parse_sym_lifetime_range_at(symbol, r);
                complete_lifetime_ranges += range && range->complete;
            }
            printf(", lifetime-ranges=%zu, complete-ranges=%zu",
                   parse_sym_lifetime_range_count(symbol),
                   complete_lifetime_ranges);
            printf(", alias-analysis=%s",
                   symbol->alias_analysis_complete ? "complete"
                                                   : "incomplete");
            size_t escaping_aliases = 0;
            size_t escaping_aliased_by = 0;
            for (size_t a = 0; a < parse_sym_alias_count(symbol); a++)
                escaping_aliases +=
                    parse_sym_alias_at(symbol, a)->escapes;
            for (size_t a = 0; a < parse_sym_aliased_by_count(symbol); a++)
                escaping_aliased_by +=
                    parse_sym_aliased_by_at(symbol, a)->escapes;
            printf(", escaping-aliases=%zu, escaping-aliased-by=%zu",
                   escaping_aliases, escaping_aliased_by);
            if (symbol->requested_storage_class.data)
                printf(", requested-storage=%.*s",
                       (int)symbol->requested_storage_class.u8_bytes,
                       symbol->requested_storage_class.data);
            if (symbol->selected_storage_class.data)
                printf(", selected-storage=%.*s",
                       (int)symbol->selected_storage_class.u8_bytes,
                       symbol->selected_storage_class.data);
            if (symbol->storage_class_required)
                printf(", storage-class-required");
            if (symbol->alias_unresolved) printf(", alias-unresolved");
            printf(", uses=%u", symbol->uses);
            parse_token_info_t *token = symbol->decl_node
                ? parse_tree_find_first_terminal(symbol->decl_node) : nullptr;
            if (token)
                printf(", declaration=%u:%u", parse_token_line(token),
                       parse_token_column(token));
            else printf(", declaration=none");
            printf(", definitions=[");
            for (size_t d = 0; d < symbol->definition_count; d++) {
                parse_token_info_t *site = parse_tree_find_first_terminal(
                    symbol->definition_nodes[d]);
                if (d) printf(", ");
                if (site) printf("%u:%u", parse_token_line(site),
                                 parse_token_column(site));
                else printf("?");
            }
            printf("], use-sites=[");
            for (size_t u = 0; u < symbol->use_count; u++) {
                parse_token_info_t *site = parse_tree_find_first_terminal(
                    symbol->use_nodes[u]);
                if (u) printf(", ");
                if (site) printf("%u:%u", parse_token_line(site),
                                 parse_token_column(site));
                else printf("?");
            }
            printf("]");
            if (symbol->is_field || symbol->kind == PARSE_SYM_FUNCTION
                || symbol->kind == PARSE_SYM_TYPEDEF) {
                printf(", node-targets=[");
                for (size_t u = 0; u < symbol->use_count; u++) {
                    parse_tree_t *use = symbol->use_nodes[u];
                    parse_node_resolution_t resolution =
                        parse_tree_resolution(use);
                    parse_token_info_t *site =
                        parse_tree_find_first_terminal(use);
                    if (u) printf(", ");
                    if (site) printf("%u:%u", parse_token_line(site),
                                     parse_token_column(site));
                    else printf("?");
                    printf("->%s", resolution.kind
                            == PARSE_NODE_RESOLUTION_SYMBOL
                        && resolution.target.symbol == symbol
                        && parse_tree_sym(use) == symbol
                            ? "self" : "mismatch");
                }
                printf("]");
            }
            putchar('\n');
        }
        core_list_free(symbols);
    }
}

static core_string_t
link_stage_state_name(core_link_stage_state_t state)
{
    switch (state) {
    case CORE_LINK_STAGE_PENDING: return CORE_STRING_STATIC("pending");
    case CORE_LINK_STAGE_RUNNING: return CORE_STRING_STATIC("running");
    case CORE_LINK_STAGE_COMPLETE: return CORE_STRING_STATIC("complete");
    case CORE_LINK_STAGE_UNAVAILABLE: return CORE_STRING_STATIC("unavailable");
    case CORE_LINK_STAGE_FAILED: return CORE_STRING_STATIC("failed");
    }
    return CORE_STRING_STATIC("invalid");
}

static core_string_t
link_contribution_state_name(core_link_contribution_state_t state)
{
    switch (state) {
    case CORE_LINK_CONTRIBUTION_PENDING: return CORE_STRING_STATIC("pending");
    case CORE_LINK_CONTRIBUTION_COMPLETE:
        return CORE_STRING_STATIC("complete");
    case CORE_LINK_CONTRIBUTION_UNAVAILABLE:
        return CORE_STRING_STATIC("unavailable");
    case CORE_LINK_CONTRIBUTION_FAILED: return CORE_STRING_STATIC("failed");
    }
    return CORE_STRING_STATIC("invalid");
}

static core_string_t
link_dependency_state_name(core_link_dependency_state_t state)
{
    switch (state) {
    case CORE_LINK_DEPENDENCY_RESOLVED: return CORE_STRING_STATIC("resolved");
    case CORE_LINK_DEPENDENCY_DEFERRED: return CORE_STRING_STATIC("deferred");
    case CORE_LINK_DEPENDENCY_EXTERNAL: return CORE_STRING_STATIC("external");
    case CORE_LINK_DEPENDENCY_UNAVAILABLE:
        return CORE_STRING_STATIC("unavailable");
    case CORE_LINK_DEPENDENCY_FAILED: return CORE_STRING_STATIC("failed");
    }
    return CORE_STRING_STATIC("invalid");
}

static core_string_t
cleanup_release_policy_name(n00bb_cleanup_release_policy_t policy)
{
    switch (policy) {
    case N00BB_CLEANUP_RELEASE_NONE: return CORE_STRING_STATIC("none");
    case N00BB_CLEANUP_RELEASE_RUNTIME_MANAGED:
        return CORE_STRING_STATIC("runtime-managed");
    case N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE:
        return CORE_STRING_STATIC("unavailable");
    }
    return CORE_STRING_STATIC("invalid");
}

static void
dump_link_unit(core_link_unit_t *link)
{
    if (!link) return;
    puts("link-unit:");
    puts("  stages:");
    for (size_t i = 0; i < core_link_unit_stage_count(link); i++) {
        uint32_t kind = core_link_unit_stage_kind(link, i);
        core_string_t provenance = core_link_unit_stage_provenance(link,
                                                                    kind);
        core_string_t state_name = link_stage_state_name(
            core_link_unit_stage_state(link, kind));
        printf("    %u %.*s (%.*s)\n", kind, TEXT_ARGS(state_name),
               (int)provenance.u8_bytes,
               provenance.data ? provenance.data : "");
    }
    puts("  contributions:");
    for (size_t i = 0; i < core_link_unit_contribution_count(link); i++) {
        core_link_contribution_view_t fact;
        if (!core_link_unit_contribution_at(link, i, &fact)) continue;
        core_string_t state_name = link_contribution_state_name(fact.state);
        printf("    %.*s:%.*s v%u %.*s (%.*s)\n",
               (int)(fact.module.data ? fact.module.u8_bytes : 11),
               fact.module.data ? fact.module.data : "<link-unit>",
               (int)fact.domain.u8_bytes, fact.domain.data, fact.version,
               TEXT_ARGS(state_name),
               (int)fact.provenance.u8_bytes,
               fact.provenance.data ? fact.provenance.data : "");
    }
    static const core_string_t binding_roles[] = {
        CORE_STRING_STATIC("n00bb.ir.function"),
        CORE_STRING_STATIC("n00bb.ir.comptime.function"),
        CORE_STRING_STATIC("n00bb.ir.global"),
        CORE_STRING_STATIC("n00bb.semantic.type"),
        CORE_STRING_STATIC("ir.array-shape"),
        CORE_STRING_STATIC("n00bb.semantic.declaration-facts"),
        CORE_STRING_STATIC("n00bb.ir.context.enter"),
        CORE_STRING_STATIC("n00bb.ir.context.exit"),
        CORE_STRING_STATIC("n00bb.ir.context.fail"),
        CORE_STRING_STATIC("n00bb.ir.context.ensure"),
    };
    puts("  declarations:");
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        core_string_t module = core_link_declaration_module(declaration);
        core_string_t key = core_link_declaration_key(declaration);
        printf("    %.*s:%.*s kind=%u",
               (int)module.u8_bytes, module.data,
               (int)key.u8_bytes, key.data,
               core_link_declaration_kind(declaration));
        for (size_t r = 0;
             r < sizeof(binding_roles) / sizeof(binding_roles[0]); r++) {
            if (core_link_declaration_binding(declaration, binding_roles[r]))
                printf(" binding %.*s", TEXT_ARGS(binding_roles[r]));
        }
        bool has_context_hooks = false;
        for (size_t r = 6; r < sizeof(binding_roles) / sizeof(binding_roles[0]);
             r++)
            has_context_hooks |= core_link_declaration_binding(declaration,
                binding_roles[r]) != nullptr;
        n00bb_link_payload_t *facts = core_link_declaration_binding(
            declaration,
            CORE_STRING_STATIC("n00bb.semantic.declaration-facts"));
        if (facts) {
            core_string_t phases = symbol_phase_name(facts->phases);
            core_string_t storage = parse_sym_storage_duration_name(
                facts->storage_duration);
            printf(" facts[dimensions=%u, initializers=%zu, phases=%.*s, "
                   "storage=%.*s, alias-analysis=%s]",
                   ir_shape_rank(parse_sym_binding(facts->symbol,
                       CORE_STRING_STATIC("ir.array-shape"))),
                   facts->initializers.len,
                   TEXT_ARGS(phases), TEXT_ARGS(storage),
                   facts->alias_analysis_complete ? "complete" : "incomplete");
            if (facts->symbol
                && (facts->symbol->kind == PARSE_SYM_FUNCTION
                    || has_context_hooks))
                printf(" cfg-liveness[symbols=%zu,ranges=%zu,complete-ranges=%zu,starts=%zu,end-points=%zu,end-edges=%zu,missing-starts=%zu,edges=%zu,direct-live=%zu,direct-dead=%zu,direct-unknown=%zu,alias-live=%zu,alias-dead=%zu,alias-unknown=%zu,range-ends=%zu,missing=%zu,owner-mismatch=%zu,empty-functions=%zu,open-blocks=%zu,%s]",
                       facts->cfg_liveness_symbols,
                       facts->cfg_liveness_ranges,
                       facts->cfg_liveness_complete_ranges,
                       facts->cfg_liveness_start_points,
                       facts->cfg_liveness_end_points,
                       facts->cfg_liveness_end_edges,
                       facts->cfg_liveness_missing_starts,
                       facts->cfg_liveness_edges,
                       facts->cfg_liveness_direct_live_edges,
                       facts->cfg_liveness_direct_dead_edges,
                       facts->cfg_liveness_direct_unknown_edges,
                       facts->cfg_liveness_alias_live_edges,
                       facts->cfg_liveness_alias_dead_edges,
                       facts->cfg_liveness_alias_unknown_edges,
                       facts->cfg_liveness_range_ends,
                       facts->cfg_liveness_missing_bindings,
                       facts->cfg_liveness_owner_mismatches,
                       facts->cfg_liveness_empty_functions,
                       facts->cfg_liveness_incomplete_blocks,
                       facts->cfg_liveness_complete
                           ? "complete" : "incomplete");
            if (facts->cleanup_obligations_complete
                || facts->cleanup_facts.len) {
                printf(" cleanup[ranges=%zu,complete-ranges=%zu,"
                       "cfg-edges=%zu,endpoints=%zu,exits=%zu,safe-exits=%zu,"
                       "live-exits=%zu,unknown-exits=%zu,release-none=%zu,"
                       "release-runtime=%zu,release-unavailable=%zu,%s]",
                       facts->cleanup_range_count,
                       facts->cleanup_complete_ranges,
                       facts->cleanup_cfg_edges,
                       facts->cleanup_endpoint_count,
                       facts->cleanup_exit_edges,
                       facts->cleanup_safe_exit_edges,
                       facts->cleanup_live_exit_edges,
                       facts->cleanup_unknown_exit_edges,
                       facts->cleanup_no_release_ranges,
                       facts->cleanup_runtime_managed_ranges,
                       facts->cleanup_unavailable_ranges,
                       facts->cleanup_obligations_complete
                           ? "complete" : "incomplete");
                for (size_t c = 0; c < facts->cleanup_facts.len; c++) {
                    const n00bb_cleanup_fact_t *cleanup =
                        &facts->cleanup_facts.data[c];
                    core_string_t cleanup_phase =
                        symbol_phase_name(cleanup->phase);
                    core_string_t cleanup_storage =
                        parse_sym_storage_duration_name(
                            cleanup->storage_duration);
                    core_string_t cleanup_release =
                        cleanup_release_policy_name(cleanup->release_policy);
                    printf(" cleanup-range[%.*s,phase=%.*s,storage=%.*s,"
                           "release=%.*s,cfg-edges=%zu,endpoints=%zu,point-endpoints=%zu,"
                           "edge-endpoints=%zu,exits=%zu,safe-exits=%zu,"
                           "live-exits=%zu,unknown-exits=%zu,evidence=%s,"
                           "finalizer=unspecified]",
                           (int)cleanup->symbol->name.u8_bytes,
                           cleanup->symbol->name.data,
                           TEXT_ARGS(cleanup_phase),
                           TEXT_ARGS(cleanup_storage),
                           TEXT_ARGS(cleanup_release),
                           cleanup->cfg_edges,
                           cleanup->safe_endpoints,
                           cleanup->point_endpoints,
                           cleanup->edge_endpoints,
                           cleanup->exit_edges,
                           cleanup->safe_exit_edges,
                           cleanup->live_exit_edges,
                           cleanup->unknown_exit_edges,
                           cleanup->range_complete ? "complete"
                                                   : "incomplete");
                }
            }
            if (facts->initializers.len)
                printf(" initializers=%zu", facts->initializers.len);
            dump_static_value(facts->static_value);
        }
        ir_shape_t *array_shape = core_link_declaration_binding(
            declaration, CORE_STRING_STATIC("ir.array-shape"));
        if (array_shape) {
            ir_shape_storage_t storage = {0};
            bool has_storage = ir_shape_storage(array_shape, &storage);
            printf(" array-shape[rank=%u",
                   ir_shape_rank(array_shape));
            if (has_storage)
                printf(",flat-prefix=%u%s", storage.flat_prefix_rank,
                       storage.classification_known ? "" : "?");
            for (uint32_t d = 0; d < ir_shape_rank(array_shape); d++) {
                ir_shape_extent_t extent;
                if (!ir_shape_extent(array_shape, d, &extent)) continue;
                core_string_t kind = shape_extent_kind_name(extent.kind);
                printf(",dim%u=%.*s", d, TEXT_ARGS(kind));
                if (extent.producer)
                    printf(":producer=%.*s", TEXT_ARGS(extent.producer->name));
                else if (extent.producer_identifier.data)
                    printf(":producer=%.*s",
                           TEXT_ARGS(extent.producer_identifier));
            }
            printf("]");
            ir_shape_measure_t element_count;
            if (ir_shape_element_count(array_shape, &element_count)
                && element_count.kind == IR_SHAPE_MEASURE_COMPTIME) {
                printf(" array-element-count=comptime");
                if (element_count.producer)
                    printf(":producer=%.*s",
                           TEXT_ARGS(element_count.producer->name));
            }
        }
        for (size_t site_index = 0;
             site_index < core_link_declaration_site_count(declaration);
             site_index++) {
            core_link_declaration_site_view_t site = {0};
            if (!core_link_declaration_site(declaration, site_index, &site))
                continue;
            core_string_t origin = site.origin == CORE_LINK_ORIGIN_SOURCE
                ? CORE_STRING_STATIC("source")
                : site.origin == CORE_LINK_ORIGIN_LIBRARY
                ? CORE_STRING_STATIC("library")
                : CORE_STRING_STATIC("external-requirement");
            printf(" site-provenance[%.*s]=%.*s", TEXT_ARGS(origin),
                   (int)site.provenance.u8_bytes,
                   site.provenance.data ? site.provenance.data : "");
        }
        putchar('\n');
    }
    puts("  references:");
    for (size_t i = 0; i < core_link_unit_reference_count(link); i++) {
        core_link_reference_t *reference =
            core_link_unit_reference_at_index(link, i);
        if (!reference) continue;
        core_translation_unit_t *referrer =
            core_link_reference_referrer(reference);
        core_link_declaration_t *target =
            core_link_reference_target(reference);
        core_string_t target_module = target
            ? core_link_declaration_module(target)
            : core_link_reference_module(reference);
        core_string_t target_key = target
            ? core_link_declaration_key(target)
            : core_link_reference_key(reference);
        core_string_t state = target ? CORE_STRING_STATIC("resolved")
            : core_link_reference_is_deferred(reference)
            ? CORE_STRING_STATIC("deferred")
            : CORE_STRING_STATIC("unresolved");
        printf("    %.*s %.*s -> %.*s:%.*s", TEXT_ARGS(state),
               (int)(referrer ? referrer->module.identifier.u8_bytes : 0),
               referrer ? referrer->module.identifier.data : "",
               (int)(target_module.data ? target_module.u8_bytes : 9),
               target_module.data ? target_module.data : "<pending>",
               (int)(target_key.data ? target_key.u8_bytes : 9),
               target_key.data ? target_key.data : "<pending>");
        core_diag_location_t location = {0};
        if (core_link_reference_source_info(reference, &location, nullptr)
            && location.has_location)
            printf(" at %u:%u", location.line, location.column);
        if (core_link_reference_is_deferred(reference)) {
            core_string_t reason =
                core_link_reference_deferral_reason(reference);
            if (reason.data)
                printf(" (%.*s)", (int)reason.u8_bytes, reason.data);
        }
        putchar('\n');
    }
    puts("  dependencies:");
    for (size_t i = 0; i < core_link_unit_dependency_count(link); i++) {
        core_link_dependency_view_t edge;
        if (!core_link_unit_dependency_at(link, i, &edge)) continue;
        core_string_t from_module = edge.from
            ? core_link_declaration_module(edge.from) : (core_string_t){0};
        core_string_t from_key = edge.from
            ? core_link_declaration_key(edge.from) : (core_string_t){0};
        core_string_t to_module = core_link_declaration_module(edge.to);
        core_string_t to_key = core_link_declaration_key(edge.to);
#define S CORE_STRING_STATIC
        core_string_t kind = edge.kind == N00BB_LINK_DEPENDENCY_CALL
            ? S("call") : edge.kind == N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
            ? S("trait-dispatch")
            : edge.kind == N00BB_LINK_DEPENDENCY_CONTEXT_CALL
            ? S("context-call")
            : edge.kind == N00BB_LINK_DEPENDENCY_TRAIT_SPECIALIZATION
            ? S("trait-specialization") : S("other");
#undef S
        core_string_t edge_state = link_dependency_state_name(edge.state);
        printf("    %.*s[%.*s] %.*s:%.*s -> %.*s:%.*s",
               TEXT_ARGS(kind), TEXT_ARGS(edge_state),
               (int)(from_module.data ? from_module.u8_bytes : 13),
               from_module.data ? from_module.data : "<initializer>",
               (int)from_key.u8_bytes,
               from_key.data ? from_key.data : "",
               (int)(to_module.data ? to_module.u8_bytes : 12),
               to_module.data ? to_module.data : "<unresolved>",
               (int)to_key.u8_bytes, to_key.data ? to_key.data : "");
        if (edge.source_location.has_location)
            printf(" at %u:%u", edge.source_location.line,
                   edge.source_location.column);
        if (edge.kind == N00BB_LINK_DEPENDENCY_TRAIT_SPECIALIZATION
            && edge.source_site && edge.source_unit) {
            n00bb_trait_specialization_t *fact =
                (n00bb_trait_specialization_t *)edge.source_site;
            n00bb_tu_state_t *state = tu_state(edge.source_unit);
            if (fact->receiver_type && state && state->types) {
                core_string_t receiver = n00bb_print_type_qualified(state->types,
                                                          fact->receiver_type);
                printf(" (receiver=%.*s", TEXT_ARGS(receiver));
            }
            else printf(" (");
            if (fact->trait_site) {
                core_diag_location_t trait_location =
                    link_source_location(fact->trait_site);
                if (trait_location.has_location)
                    printf(" trait-site=%u:%u", trait_location.line,
                           trait_location.column);
            }
            putchar(')');
        }
        if (edge.source_unit) {
            core_string_t path = core_source_file_path(
                &edge.source_unit->source);
            if (path.data)
                printf(" (%.*s)", (int)path.u8_bytes, path.data);
        }
        putchar('\n');
    }
}

static bool
discard_ignored_layout(parse_grammar_t *grammar, parse_token_stream_t *tokens)
{
    int64_t indent = parse_terminal_id(
        grammar, CORE_STRING_STATIC("INDENT"));
    int64_t dedent = parse_terminal_id(
        grammar, CORE_STRING_STATIC("DEDENT"));
    for (int32_t i = 0; i < scan_stream_token_count(tokens);) {
        parse_token_info_t *token = scan_stream_get(tokens, i);
        if (token && parse_token_ignore_if_no_match(token)
            && (parse_token_tid(token) == indent
                || parse_token_tid(token) == dedent)) {
            if (!scan_stream_token_to_trivia(tokens, i)) return false;
            continue;
        }
        i++;
    }
    return true;
}

static void
record_resolve_status(locate_worker_context_t *context,
                      core_work_job_t *job,
                      core_module_resolve_status_t status)
{
    pthread_mutex_lock(context->requests_lock);
    locate_request_t *request = request_for_job(context->requests, job);
    if (request) {
        request->resolve_status = status;
        request->resolve_attempted = true;
    }
    pthread_mutex_unlock(context->requests_lock);
}

static bool
n00bb_type_bracket_disambiguator(void *opaque, core_string_t predicate,
                                 parse_token_info_t *token)
{
    (void)opaque;
    if (core_string_eq(predicate,
                       CORE_STRING_STATIC("n00bb_penalize_nil_identifier"))) {
        core_option_t(core_string_t) value = token
            ? parse_token_value(token) : core_option_none(core_string_t);
        if (!core_option_is_set(value)) return false;
        core_string_t spelling = core_option_get(value);
        return core_string_eq(spelling, CORE_STRING_STATIC("null"))
            || core_string_eq(spelling, CORE_STRING_STATIC("None"));
    }
    return core_string_eq(predicate,
        CORE_STRING_STATIC("n00bb_prefer_type_application"));
}

static bool
make_interface_module_source(locate_worker_context_t *context,
                             const core_module_spec_t *module,
                             core_source_file_t *source)
{
    core_string_t package = interface_package_for_module(
        &context->interfaces, module->identifier);
    if (!package.data) return false;
    core_string_t cached_path = {0};
    uint64_t source_hash = 0;
    core_buffer_t path = {0};
    if (!n00bb_cache_module_source(context->cache, module->identifier,
                                   &cached_path, &source_hash)
        || !cached_path.data)
        core_buffer_printf(&path, "<interface:%.*s>",
            (int)module->identifier.u8_bytes, module->identifier.data);
    else core_buffer_append_string(&path, cached_path);
    bool initialized = core_source_file_init(source,
        core_buffer_string(&path), CORE_STRING_STATIC(""));
    core_buffer_release(&path);
    if (!initialized) return false;
    n00bb_source_file_set_package(source, package);
    n00bb_source_file_mark_interface(source, source_hash);
    return true;
}

static bool
process_locate_job(locate_worker_context_t *context, core_work_job_t *job)
{
    core_string_t operand = {0};
    bool root_input = false;
    if (!request_snapshot(context->requests, context->requests_lock, job,
                          &operand, &root_input)) {
        record_resolve_status(context, job, CORE_MODULE_RESOLVE_ERROR);
        core_work_complete(context->queue, job, false, nullptr);
        return false;
    }
    core_module_spec_t resolved = {0};
    core_source_file_t source = {0};
    const core_module_spec_t *module = core_work_job_module(job);
    bool interface_allowed = !context->cache
        || (module && n00bb_cache_has_module(context->cache,
                                             module->identifier));
    bool logical_import = !root_input && module && operand.data
        && interface_allowed
        && (core_string_eq(operand, module->identifier) || context->cache);
    core_module_resolve_status_t status = CORE_MODULE_NOT_FOUND;
    if (logical_import
        && make_interface_module_source(context, module, &source)) {
        if (core_module_spec_init(&resolved, module->identifier))
            status = CORE_MODULE_RESOLVED;
        else {
            core_source_file_release(&source);
            status = CORE_MODULE_RESOLVE_ERROR;
        }
    }
    else status = n00bb_locate_operand(context->resolver, operand,
                                      &resolved, &source);
    if (status == CORE_MODULE_NOT_FOUND && !root_input && interface_allowed) {
        if (module && make_interface_module_source(context, module, &source)) {
            core_module_spec_release(&resolved);
            if (core_module_spec_init(&resolved, module->identifier))
                status = CORE_MODULE_RESOLVED;
            else {
                core_source_file_release(&source);
                status = CORE_MODULE_RESOLVE_ERROR;
            }
        }
    }
    core_free(operand.data);
    record_resolve_status(context, job, status);
    if (status != CORE_MODULE_RESOLVED) {
        core_module_spec_release(&resolved);
        core_work_complete(context->queue, job, false, nullptr);
        return false;
    }

    core_module_spec_release(&resolved);
    core_module_spec_init(&resolved, core_work_job_module(job)->identifier);
    core_translation_unit_t *unit = core_alloc(core_translation_unit_t);
    bool initialized = core_translation_unit_init(unit, &resolved, &source);
    core_module_spec_release(&resolved);
    if (!initialized) {
        core_source_file_release(&source);
        core_free(unit);
        core_work_complete(context->queue, job, false, nullptr);
        return false;
    }

    bool ok = true;
    if (context->lex) {
        core_string_t contents = core_source_file_contents(&unit->source);
        size_t length = core_source_file_size(&unit->source);
        n00bb_lex_result_t *result = core_alloc(n00bb_lex_result_t);
        memset(result, 0, sizeof(*result));
        result->tokens = scan_tokenize_owned(context->grammar,
            core_string_borrow(contents.data, length));
        if (!result->tokens) {
            core_free(result);
            ok = false;
        }
        else {
            result->tokens->scanner->file = core_option_set(
                core_string_t, core_source_file_path(&unit->source));
            n00bb_tu_state_new(unit, result, context->types);
            (void)scan_scanner_set_recovery(result->tokens->scanner, true);
            while (scan_stream_next(result->tokens)) {}
            if (!discard_ignored_layout(context->grammar, result->tokens)) {
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, (core_diag_location_t){}, "reading source tokens",
                    "could not normalize the layout tokens");
                ok = false;
            }
            parse_scanner_t *scanner = result->tokens->scanner;
            core_string_t text = core_string_borrow(contents.data, length);
            for (size_t w = 0; w < scan_warning_count(scanner); w++)
                n00bb_report_lexical_warning(
                    core_translation_unit_diagnostics(unit), unit, text,
                    scan_warning_at(scanner, w));
            for (size_t r = 0; r < scan_recovery_count(scanner); r++) {
                n00bb_report_lexical_error(
                    core_translation_unit_diagnostics(unit), unit, text,
                    scan_recovery_at(scanner, r));
                ok = false;
            }
            scan_diagnostic_t diagnostic = {0};
            if (scan_diagnostic(scanner, &diagnostic)
                && diagnostic.status != SCAN_STATUS_COMPLETE
                && diagnostic.status != SCAN_STATUS_RECOVERED) {
                n00bb_report_lexical_error(
                    core_translation_unit_diagnostics(unit), unit, text,
                    &diagnostic);
                ok = false;
            }
            if (ok && context->parse) {
                scan_stream_rewind(result->tokens,
                    scan_stream_token_count(result->tokens));
                parse_nt_id_t start = parse_nonterm_id(parse_find_nonterm(
                    context->grammar, CORE_STRING_STATIC("module")));
                result->parser = parse_pwz_new_for_start(context->grammar,
                                                         start);
                parse_pwz_set_disambiguator(result->parser,
                    n00bb_type_bracket_disambiguator, nullptr);
                if (!parse_pwz_parse(result->parser, result->tokens)) {
                    n00bb_report_syntax_errors(&(n00bb_syntax_input_t){
                        .diagnostics = core_translation_unit_diagnostics(unit),
                        .unit = unit,
                        .grammar = context->grammar,
                        .tokens = result->tokens,
                        .parser = result->parser,
                        .disambiguator = n00bb_type_bracket_disambiguator,
                        .text = text,
                        .end_of_file = {
                            .line = result->tokens->scanner->line,
                            .column = result->tokens->scanner->column,
                            .end_line = result->tokens->scanner->line,
                            .end_column = result->tokens->scanner->column,
                            .has_location = true,
                        },
                    });
                    ok = false;
                }
                else parse_pwz_release_parse_state(result->parser);
            }
        }
    }
    (void)root_input;
    if (!core_work_complete_tu(context->queue, job, unit, ok)) return false;
    return core_work_job_state(context->queue, job) == CORE_WORK_SUCCEEDED;
}

static void *
locate_worker(void *opaque)
{
    locate_worker_context_t *context = opaque;
    core_work_job_t *job = nullptr;
    while (core_work_take_kind(context->queue, N00BB_JOB_LOCATE_TU, &job))
        process_locate_job(context, job);
    return nullptr;
}

static void
append_import_names(parse_grammar_t *grammar, parse_tree_t *node,
                    core_buffer_t *name, bool *has_name)
{
    if (!node) return;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        if (token && core_string_eq(parse_terminal_name(grammar,
                                        parse_token_tid(token)),
                                    CORE_STRING_STATIC("IDENTIFIER"))) {
            if (*has_name) core_buffer_putc(name, '.');
            core_string_t text = parse_tree_leaf_text(node);
            core_buffer_append(name, text.data, text.u8_bytes);
            *has_name = true;
        }
        return;
    }
    if (!nt_is(grammar, node, CORE_STRING_STATIC("member-chain"))) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (nt_is(grammar, child, CORE_STRING_STATIC("member-chain")))
            append_import_names(grammar, child, name, has_name);
        else if (core_tree_is_leaf(child))
            append_import_names(grammar, child, name, has_name);
    }
}

static core_diag_location_t
import_location(parse_tree_t *statement)
{
    return statement ? link_source_location(statement)
                     : (core_diag_location_t){0};
}

static void
import_report(core_translation_unit_t *unit, parse_tree_t *statement,
              n00bb_diag_t id, n00bb_text_t text, n00bb_args_t args)
{
    n00bb_report(core_translation_unit_diagnostics(unit), unit, id,
                 import_location(statement), text, args);
}

static void
import_internal(core_translation_unit_t *unit, parse_tree_t *statement,
                const char *what)
{
    n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
                         import_location(statement), "resolving imports",
                         "%s", what);
}

static bool
reject_unsupported_import_locator(n00bb_resolver_t *resolver,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *statement,
                                  core_string_t operand)
{
    core_string_t scheme = {0};
    if (n00bb_resolver_supports_locator(resolver, operand, &scheme))
        return false;
    import_report(unit, statement, N00B_DIAG_IMPORT_UNSUPPORTED_LOCATOR,
        N00B_TEXT_USES_THE_UNSUPPORTED_SOURCE_LOCATOR,
        N00BB_ARGS(ARG_TEXT(operand), ARG_TEXT(scheme)));
    return true;
}

static bool
import_request_details(n00bb_resolver_t *resolver, parse_grammar_t *grammar,
                      parse_tree_t *statement,
                      core_translation_unit_t *importer,
                      core_module_spec_t *module, core_string_t *operand_out,
                      core_string_t *source_name_out)
{
    parse_tree_t *member = parse_tree_find_flat_child_by_nt_name(
        grammar, statement, CORE_STRING_STATIC("member-chain"));
    core_buffer_t *name = core_buffer_empty();
    bool has_name = false;
    append_import_names(grammar, member, name, &has_name);
    if (!has_name) {
        core_buffer_free(name);
        import_internal(importer, statement,
                        "an import statement has no module name");
        return false;
    }
    core_string_t source_name = core_string_borrow(name->data, name->byte_len);
    core_string_t package_namespace = n00bb_package_namespace(importer);
    if (package_namespace.u8_bytes) {
        size_t root_len = package_namespace.u8_bytes;
        if (source_name.u8_bytes >= root_len
            && !memcmp(source_name.data, package_namespace.data, root_len)
            && (source_name.u8_bytes == root_len
                || source_name.data[root_len] == '.')) {
            import_report(importer, statement, N00B_DIAG_IMPORT_OWN_NAMESPACE,
                N00B_TEXT_IS_THIS_PACKAGE_S_OWN,
                N00BB_ARGS(ARG_TEXT(source_name)));
            n00bb_report_help(core_translation_unit_diagnostics(importer),
                N00B_TEXT_INSIDE_A_PACKAGE_IMPORT_ITS,
                N00BB_NO_ARGS);
            core_buffer_free(name);
            return false;
        }
    }
    bool internal = n00bb_resolver_internal_import(
        resolver, importer, source_name, module);
    bool initialized = internal || core_module_spec_init(module, source_name);
    if (initialized) {
        *operand_out = core_string_dup(internal ? module->identifier
                                                : source_name);
        *source_name_out = core_string_dup(source_name);
    }
    core_buffer_free(name);
    if (!initialized || !operand_out->data || !source_name_out->data) {
        import_internal(importer, statement,
                        "could not form the imported module's identity");
        return false;
    }
    return true;
}

static bool
prepare_import(void *opaque, parse_grammar_t *grammar, parse_tree_t *statement,
               core_translation_unit_t *importer)
{
    import_driver_t *driver = opaque;
    core_module_spec_t module = {0};
    core_string_t operand = {0};
    core_string_t source_name = {0};
    if (!import_request_details(driver->resolver, grammar, statement, importer,
                                &module, &operand, &source_name)) return false;
    if (reject_unsupported_import_locator(driver->resolver, importer,
                                          statement, operand)) {
        core_free(operand.data);
        core_free(source_name.data);
        core_module_spec_release(&module);
        return false;
    }
    core_work_job_t *job = nullptr;
    bool scheduled = add_locate_request_for_module(driver->queue,
        driver->requests, driver->requests_lock, &module, operand, false,
        true, false, &job);
    core_free(operand.data);
    core_free(source_name.data);
    core_module_spec_release(&module);
    if (!scheduled)
        import_internal(importer, statement,
                        "could not schedule the imported module");
    return scheduled;
}

static bool
collect_discovery_candidate(void *opaque,
                            const core_module_spec_t *candidate)
{
    n00bb_discovery_candidates_t *discovery = opaque;
    n00bb_module_candidate_t copy = {0};
    if (!core_module_spec_init(&copy, candidate->identifier)) return false;
    core_list_push(discovery->modules, copy);
    return true;
}

static bool
failure_already_reported(core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
    return unit && (core_diagnostics_failed(
                        core_translation_unit_diagnostics(unit))
                    || (state && state->import_failure_reported));
}

static void
mark_import_failure_reported(core_translation_unit_t *importer)
{
    n00bb_tu_state_t *state = importer ? tu_state(importer) : nullptr;
    if (state) state->import_failure_reported = true;
}

static bool
manifest_mentions(core_translation_unit_t *manifest, core_string_t package)
{
    if (!manifest || !package.u8_bytes) return false;
    int64_t dot = core_string_rfind_byte(package, '.');
    core_string_t leaf = dot < 0 ? package
        : core_string_slice(package, (size_t)dot + 1,
                            package.u8_bytes - (size_t)dot - 1);
    core_string_t text = core_string_borrow(
        core_source_file_contents(&manifest->source).data,
        core_source_file_size(&manifest->source));
    return core_string_find(text, leaf) >= 0;
}

static void
claim_manifest(import_driver_t *driver, core_work_job_t *job)
{
    pthread_mutex_lock(driver->requests_lock);
    locate_request_t *request = request_for_job(driver->requests, job);
    if (request) request->manifest_probe = false;
    pthread_mutex_unlock(driver->requests_lock);
}

static void
claim_manifest_failure(import_driver_t *driver, core_work_job_t *job,
                       core_translation_unit_t *manifest,
                       core_translation_unit_t *importer)
{
    if (!failure_already_reported(manifest)) return;
    claim_manifest(driver, job);
    mark_import_failure_reported(importer);
}

static bool
locate_job_resolved(import_driver_t *driver, core_work_job_t *job)
{
    pthread_mutex_lock(driver->requests_lock);
    locate_request_t *request = request_for_job(driver->requests, job);
    bool resolved = request && request->resolve_attempted
        && request->resolve_status == CORE_MODULE_RESOLVED;
    pthread_mutex_unlock(driver->requests_lock);
    return resolved;
}

static bool
wait_for_locate_job(import_driver_t *driver, core_work_job_t *job)
{
    while (!driver->worker_count
           && core_work_job_state(driver->queue, job) != CORE_WORK_SUCCEEDED
           && core_work_job_state(driver->queue, job) != CORE_WORK_FAILED) {
        core_work_job_t *ready = nullptr;
        if (!core_work_take_kind(driver->queue, N00BB_JOB_LOCATE_TU, &ready))
            break;
        process_locate_job(driver->loader, ready);
    }
    return core_work_wait(driver->queue, job);
}

static bool
record_import_alias(core_translation_unit_t *importer, core_string_t name,
                    core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(importer);
    if (!state || !unit) return false;
    core_translation_unit_t *prior = core_dict_get(
        &state->import_alias_index, &name, nullptr);
    if (prior) return prior == unit;
    char *stored = core_arena_alloc(&state->semantic_values,
                                    name.u8_bytes + 1);
    if (!stored) return false;
    core_string_t *key = core_arena_alloc(&state->semantic_values,
                                          sizeof(*key));
    if (!key) return false;
    memcpy(stored, name.data, name.u8_bytes);
    stored[name.u8_bytes] = '\0';
    *key = core_string_borrow(stored, name.u8_bytes);
    core_list_push(state->import_aliases, ((n00bb_import_alias_t){
        .name = *key,
        .unit = unit,
    }));
    core_dict_add(&state->import_alias_index, key, unit);
    return true;
}

static bool
record_import_rename(core_translation_unit_t *importer, core_string_t name,
                     core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(importer);
    if (!record_import_alias(importer, name, unit)) return false;
    for (size_t i = 0; i < state->import_aliases.len; i++)
        if (core_string_eq(state->import_aliases.data[i].name, name))
            state->import_aliases.data[i].renamed = true;
    return true;
}

static void
note_import_reach(core_list_t(core_translation_unit_ptr_t) *list,
                  core_translation_unit_t *unit)
{
    for (size_t i = 0; i < list->len; i++)
        if (list->data[i] == unit) return;
    core_list_push(*list, unit);
}

static void
collect_brought_symbols(parse_grammar_t *grammar, parse_tree_t *node,
                        core_list_t(parse_tree_ptr_t) *out)
{
    if (!node || core_tree_is_leaf(node)) return;
    if (nt_is(grammar, node, CORE_STRING_STATIC("brought-symbol"))) {
        core_list_push(*out, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_brought_symbols(grammar, core_tree_child(node, i), out);
}

static void
collect_identifier_leaves(parse_grammar_t *grammar, parse_tree_t *node,
                          core_list_t(parse_tree_ptr_t) *out)
{
    if (!node) return;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        if (token && core_string_eq(parse_terminal_name(grammar,
                parse_token_tid(token)), CORE_STRING_STATIC("IDENTIFIER")))
            core_list_push(*out, node);
        return;
    }
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_identifier_leaves(grammar, core_tree_child(node, i), out);
}

static bool
brought_symbol_names(parse_grammar_t *grammar, parse_tree_t *symbol,
                     core_string_t *remote, core_string_t *local)
{
    core_list_t(parse_tree_ptr_t) names = core_list_new(parse_tree_ptr_t);
    collect_identifier_leaves(grammar, symbol, &names);
    *remote = names.len ? parse_tree_leaf_text(names.data[0])
                        : first_token_text(symbol);
    *local = names.len ? parse_tree_leaf_text(names.data[names.len - 1])
                       : *remote;
    bool renamed = names.len > 1;
    core_list_free(names);
    return renamed;
}

static parse_tree_t *
owning_module_import(parse_grammar_t *grammar, parse_tree_t *node,
                     parse_tree_t *source)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    if (nt_is(grammar, node, CORE_STRING_STATIC("module-import")))
        return core_tree_num_children(node)
            && core_tree_child(node, 0) == source ? node : nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = owning_module_import(grammar,
            core_tree_child(node, i), source);
        if (found) return found;
    }
    return nullptr;
}

static bool
record_import_shape(void *opaque, parse_grammar_t *grammar,
                    parse_tree_t *statement, parse_tree_t *source,
                    core_translation_unit_t *importer,
                    core_translation_unit_t *imported)
{
    (void)opaque;
    n00bb_tu_state_t *state = tu_state(importer);
    if (!state || !nt_is(grammar, statement,
                         CORE_STRING_STATIC("import-stmt"))) return true;
    parse_tree_t *module_import = owning_module_import(grammar, statement,
                                                       source);
    core_string_t alias = module_import
            && core_tree_num_children(module_import) == 3
        ? parse_tree_leaf_text(core_tree_child(module_import, 2))
        : (core_string_t){0};
    if (alias.data && !record_import_rename(importer, alias, imported)) {
        import_report(importer, source, N00B_DIAG_IMPORT_ALIAS_CONFLICT,
            N00B_TEXT_ALREADY_NAMES_ANOTHER_IMPORTED_MODULE,
            N00BB_ARGS(ARG_TEXT(alias)));
        return false;
    }
    note_import_reach(alias.data || !module_import ? &state->restricted_imports
                                                   : &state->path_imports,
                      imported);
    parse_tree_t *brought = child_nt(grammar, statement,
                                     CORE_STRING_STATIC("brought-symbols"));
    bool bring_from = brought != nullptr;
    if (!brought)
        brought = child_nt(grammar, statement,
                           CORE_STRING_STATIC("brought-names"));
    if (!brought) return true;
    core_list_t(parse_tree_ptr_t) symbols = core_list_new(parse_tree_ptr_t);
    collect_brought_symbols(grammar, brought, &symbols);
    core_string_t remote = {0}, local = {0};
    bool all = bring_from && (symbols.len
        ? symbols.len == 1 && !brought_symbol_names(grammar, symbols.data[0],
                                                    &remote, &local)
            && core_string_eq(remote, CORE_STRING_STATIC("all"))
        : core_string_eq(first_token_text(brought),
                         CORE_STRING_STATIC("all")));
    if (all)
        core_list_push(state->brought_names, ((n00bb_brought_name_t){
            .node = statement, .provider = imported, .all = true,
        }));
    for (size_t i = 0; !all && i < symbols.len; i++) {
        (void)brought_symbol_names(grammar, symbols.data[i], &remote,
                                   &local);
        core_list_push(state->brought_names, ((n00bb_brought_name_t){
            .node = symbols.data[i], .provider = imported,
            .local = local, .remote = remote,
        }));
    }
    core_list_free(symbols);
    return true;
}

static bool
load_interface_imports(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit);
static bool
process_postprune_package_syntax(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *tree);

static bool
ensure_preprocessed_import(import_driver_t *driver, parse_grammar_t *grammar,
                           core_work_job_t *locate_job,
                           core_translation_unit_t *unit, parse_tree_t *tree,
                           bool *in_progress)
{
    if (in_progress) *in_progress = false;
    core_work_job_t *dependencies[] = {locate_job};
    core_work_job_t *preprocess_job = nullptr;
    core_work_submit_result_t submitted = core_work_submit(driver->queue,
        N00BB_JOB_PREPROCESS, core_work_job_module(locate_job), 0, dependencies,
        1, &preprocess_job);
    if (submitted == CORE_WORK_INVALID
        || submitted == CORE_WORK_DEPENDENCY_FAILED) return false;
    if (submitted == CORE_WORK_CACHED) {
        core_work_state_t state = core_work_job_state(driver->queue,
                                                      preprocess_job);
        if (in_progress) *in_progress = state != CORE_WORK_SUCCEEDED
            && state != CORE_WORK_FAILED;
        return state != CORE_WORK_FAILED;
    }
    core_work_job_t *ready = nullptr;
    if (!core_work_take_kind(driver->queue, N00BB_JOB_PREPROCESS, &ready)
        || ready != preprocess_job) return false;
    bool ok = n00bb_preprocessor_run(driver->preprocessor, grammar, tree, unit);
    if (ok) ok = process_postprune_package_syntax(grammar, unit, tree);
    n00bb_tu_state_t *state = tu_state(unit);
    if (ok && state && state->interface_module)
        ok = load_interface_imports(driver, grammar, unit);
    return core_work_complete_tu(driver->queue, preprocess_job, unit, ok)
        && core_work_job_state(driver->queue, preprocess_job)
            == CORE_WORK_SUCCEEDED;
}

static core_translation_unit_t *
locate_manifest_candidate(import_driver_t *driver,
                          core_module_spec_t *candidate,
                          core_work_job_t **job_out, parse_tree_t **tree_out,
                          core_buffer_t *declared)
{
    if (!add_locate_request_for_module(driver->queue, driver->requests,
            driver->requests_lock, candidate, candidate->identifier, false,
            true, true, job_out)) {
        *job_out = nullptr;
        return nullptr;
    }
    if (!wait_for_locate_job(driver, *job_out)) {
        core_translation_unit_t *failed = core_work_job_result(driver->queue,
                                                                *job_out);
        *tree_out = nullptr;
        return failed;
    }
    core_translation_unit_t *unit = core_work_job_result(driver->queue,
                                                          *job_out);
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_lex_result_t *lex = state ? state->lex : nullptr;
    *tree_out = lex && lex->parser ? parse_pwz_get_tree(lex->parser)
                                   : nullptr;
    if (!unit || !*tree_out) return unit;
    if (state->interface_module) {
        for (size_t i = 0; declared && i < state->package_declarations.len;
             i++) {
            core_string_t name = state->package_declarations.data[i].name;
            core_buffer_append(declared, name.data, name.u8_bytes);
            core_buffer_putc(declared, '\n');
        }
        return unit;
    }
    core_buffer_t names = {0};
    if (package_tree_syntax_index(driver->grammar, *tree_out, &names)) {
        n00bb_resolver_store_package_syntax(driver->resolver,
            core_source_file_path(&unit->source), names.data,
            names.byte_len);
        if (declared) core_buffer_append(declared, names.data, names.byte_len);
    }
    core_buffer_release(&names);
    return unit;
}

static size_t
direct_subpackage_count(const core_buffer_t *names, core_string_t parent)
{
    core_string_t first = {0};
    size_t start = 0;
    while (start < names->byte_len) {
        const char *end = memchr(names->data + start, '\n',
                                 names->byte_len - start);
        size_t length = end ? (size_t)(end - (names->data + start))
                            : names->byte_len - start;
        core_string_t name = core_string_borrow(names->data + start, length);
        start += length + 1;
        if (length <= parent.u8_bytes + 1
            || !core_string_starts_with(name, parent)
            || name.data[parent.u8_bytes] != '.') continue;
        core_string_t child = core_string_slice(name, parent.u8_bytes + 1,
                                                length - parent.u8_bytes - 1);
        int64_t dot = core_string_find_byte(child, '.');
        if (dot >= 0) child = core_string_slice(child, 0, (size_t)dot);
        if (!first.data) first = child;
        else if (!core_string_eq(first, child)) return 2;
    }
    return first.data ? 1 : 0;
}

static bool collect_package_declarations(import_driver_t *driver,
    core_string_t package_id, core_buffer_t *names, bool first_only);

static bool
accept_package_exports(import_driver_t *driver,
                       core_translation_unit_t *importer,
                       core_translation_unit_t *manifest, core_string_t name)
{
    n00bb_package_declaration_t *package = find_package_declaration(manifest,
                                                                    name);
    if (!package || package->exports.len) return true;
    core_buffer_t names = {0};
    size_t subpackages = collect_package_declarations(driver,
            n00bb_package_id(manifest), &names, false)
        ? direct_subpackage_count(&names, name) : 2;
    core_buffer_release(&names);
    if (subpackages > 1
        || (subpackages && n00bb_system_module(manifest->module.identifier)))
        return true;
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(importer);
    n00bb_report(diagnostics, manifest,
        subpackages ? N00B_DIAG_PACKAGE_SINGLE_SUBPACKAGE
                    : N00B_DIAG_PACKAGE_EXPORTS_NOTHING,
        link_source_location(package->node),
        subpackages ? N00B_TEXT_PACKAGE_HOLDS_ONE_SUBPACKAGE
                    : N00B_TEXT_PACKAGE_HAS_NO_SUBPACKAGES,
        N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_help(diagnostics, subpackages
        ? N00B_TEXT_IMPORT_SUBPACKAGE_DIRECTLY
        : N00B_TEXT_ADD_EXPORT_STATEMENTS, N00BB_ARGS(ARG_TEXT(name)));
    return subpackages != 0;
}

static core_work_job_t *
discover_package_import(import_driver_t *driver,
                        core_translation_unit_t *importer,
                        parse_tree_t *statement,
                        const core_module_spec_t *logical,
                        bool record_alias);

static bool
parent_manifest_declared(import_driver_t *driver,
                         core_translation_unit_t *importer,
                         parse_tree_t *statement, core_string_t name)
{
    int64_t dot = core_string_rfind_byte(name, '.');
    if (dot < 0) return true;
    core_string_t parent = core_string_borrow(name.data, (size_t)dot);
    core_module_spec_t spec = {0};
    bool declared = core_module_spec_init(&spec, parent)
        && discover_package_import(driver, importer, statement, &spec,
                                   false) != nullptr;
    core_module_spec_release(&spec);
    if (declared) return true;
    import_report(importer, statement,
        N00B_DIAG_PACKAGE_PARENT_MANIFEST_MISSING,
        N00B_TEXT_PACKAGE_IS_DECLARED_BUT_NO,
        N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(parent)));
    return false;
}

static core_work_job_t *
discover_package_import(import_driver_t *driver,
                        core_translation_unit_t *importer,
                        parse_tree_t *statement,
                        const core_module_spec_t *logical,
                        bool record_alias)
{
    n00bb_discovery_candidates_t candidates = {
        .modules = core_list_new(n00bb_module_candidate_t),
    };
    core_module_resolver_t resolver = n00bb_resolver_interface(
        driver->resolver);
    core_module_resolve_status_t status = core_module_discover(
        &resolver, logical, collect_discovery_candidate, &candidates);
    core_work_job_t *selected_job = nullptr;
    core_translation_unit_t *selected = nullptr;
    for (size_t i = 0; status == CORE_MODULE_RESOLVED
            && i < candidates.modules.len; i++) {
        core_work_job_t *job = nullptr;
        parse_tree_t *tree = nullptr;
        core_translation_unit_t *unit = locate_manifest_candidate(driver,
            &candidates.modules.data[i], &job, &tree, nullptr);
        if (!job) continue;
        n00bb_tu_state_t *state = tu_state(unit);
        if (!unit || !tree) {
            if (manifest_mentions(unit, logical->identifier))
                claim_manifest_failure(driver, job, unit, importer);
            continue;
        }
        if (state->interface_module
                ? !interface_may_declare_package(&driver->loader->interfaces,
                      unit->module.identifier, logical->identifier)
                : !package_tree_may_declare(driver->grammar, tree,
                                            logical->identifier))
            continue;
        bool in_progress = false;
        if (!ensure_preprocessed_import(driver, driver->grammar,
                                        job, unit, tree, &in_progress)) {
            claim_manifest_failure(driver, job, unit, importer);
            continue;
        }
        if (!in_progress
            && !find_package_declaration(unit, logical->identifier))
            continue;
        if (selected && selected != unit) {
            import_report(importer, statement,
                N00B_DIAG_PACKAGE_MULTIPLE_MANIFESTS,
                N00B_TEXT_PACKAGE_IS_DECLARED_BY_MORE,
                N00BB_ARGS(ARG_TEXT(logical->identifier)));
            core_diagnostics_t *diagnostics =
                core_translation_unit_diagnostics(importer);
            core_string_t first = core_source_file_path(&selected->source);
            core_string_t second = core_source_file_path(&unit->source);
            n00bb_report_note(diagnostics, N00B_TEXT_DECLARED_IN,
                N00BB_ARGS(ARG_TEXT(first)));
            n00bb_report_note(diagnostics, N00B_TEXT_DECLARED_AGAIN_IN,
                N00BB_ARGS(ARG_TEXT(second)));
            selected_job = nullptr;
            selected = nullptr;
            status = CORE_MODULE_RESOLVE_ERROR;
            break;
        }
        selected = unit;
        selected_job = job;
        claim_manifest(driver, job);
    }
    if (selected && (!parent_manifest_declared(driver, importer, statement,
                                               logical->identifier)
                     || !accept_package_exports(driver, importer, selected,
                                                logical->identifier))) {
        mark_import_failure_reported(importer);
        selected_job = nullptr;
        selected = nullptr;
    }
    if (selected && record_alias
        && !record_import_alias(importer, logical->identifier, selected)) {
        import_internal(importer, statement,
                        "could not record a package interface import");
        selected_job = nullptr;
    }
    for (size_t i = 0; i < candidates.modules.len; i++)
        core_module_spec_release(&candidates.modules.data[i]);
    core_list_free(candidates.modules);
    return selected_job;
}

static bool
collect_package_declarations(import_driver_t *driver,
                             core_string_t package_id, core_buffer_t *names,
                             bool first_only)
{
    n00bb_discovery_candidates_t candidates = {
        .modules = core_list_new(n00bb_module_candidate_t),
    };
    bool listed = n00bb_resolver_package_manifests(driver->resolver,
        package_id, collect_discovery_candidate, &candidates);
    for (size_t i = 0; listed && i < candidates.modules.len
             && !(first_only && names->byte_len); i++) {
        core_work_job_t *job = nullptr;
        parse_tree_t *tree = nullptr;
        (void)locate_manifest_candidate(driver, &candidates.modules.data[i],
                                        &job, &tree, names);
    }
    for (size_t i = 0; i < candidates.modules.len; i++)
        core_module_spec_release(&candidates.modules.data[i]);
    core_list_free(candidates.modules);
    return listed;
}

static bool
package_has_manifest(import_driver_t *driver,
                     core_translation_unit_t *imported)
{
    core_string_t package_id = n00bb_package_id(imported);
    bool found = false;
    if (n00bb_resolver_known_manifest(driver->resolver, package_id, &found))
        return found;
    core_buffer_t names = {0};
    bool listed = collect_package_declarations(driver, package_id, &names,
                                               true);
    found = !listed || names.byte_len != 0;
    core_buffer_release(&names);
    if (listed)
        n00bb_resolver_remember_manifest(driver->resolver, package_id, found);
    return found;
}


static bool
load_interface_imports(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit);

static bool
load_import_module(import_driver_t *driver, parse_grammar_t *grammar,
                   parse_tree_t *statement, core_translation_unit_t *importer,
                   core_module_spec_t *module, core_string_t operand,
                   core_string_t source_name,
                   core_translation_unit_t **imported_out,
                   parse_tree_t **tree_out)
{
    if (reject_unsupported_import_locator(driver->resolver, importer,
                                          statement, operand)) {
        return false;
    }
    core_string_t module_path = {0};
    core_string_t directory_path = {0};
    if (statement && n00bb_resolver_ambiguous_module(driver->resolver,
            importer, module->identifier, &module_path, &directory_path)) {
        import_report(importer, statement, N00B_DIAG_MODULE_DIRECTORY_AMBIGUOUS,
            N00B_TEXT_IS_AMBIGUOUS_AND_THE_DIRECTORY,
            N00BB_ARGS(ARG_TEXT(operand), ARG_TEXT(module_path),
                       ARG_TEXT(directory_path)));
        n00bb_report_help(core_translation_unit_diagnostics(importer),
            N00B_TEXT_RENAME_THE_MODULE_OR_THE,
            N00BB_NO_ARGS);
        core_free(module_path.data);
        core_free(directory_path.data);
        return false;
    }
    core_work_job_t *locate_job = nullptr;
    bool scheduled = add_locate_request_for_module(driver->queue,
        driver->requests, driver->requests_lock, module, operand,
        false, true, false, &locate_job);
    if (!scheduled) {
        import_internal(importer, statement,
                        "could not schedule the imported module");
        return false;
    }

    bool located = wait_for_locate_job(driver, locate_job);
    bool resolved = locate_job_resolved(driver, locate_job);    core_translation_unit_t *attempted = core_work_job_result(driver->queue,
                                                              locate_job);
    core_translation_unit_t *direct = located
        ? core_work_job_result(driver->queue, locate_job) : nullptr;
    if (!direct || (statement && !n00bb_same_package(importer, direct))) {
        core_work_job_t *package_job = discover_package_import(
            driver, importer, statement, module, true);
        if (package_job) locate_job = package_job;
        else if (!located) locate_job = nullptr;
    }
    n00bb_tu_state_t *importer_state = tu_state(importer);
    if (!locate_job && ((resolved && failure_already_reported(attempted))
            || (importer_state && importer_state->import_failure_reported))) {
        mark_import_failure_reported(importer);
        return false;
    }
    if (!locate_job) {
        if (resolved)
            import_report(importer, statement, N00B_DIAG_IMPORT_UNREADABLE,
                N00B_TEXT_MODULE_WAS_FOUND_BUT_COULD,
                N00BB_ARGS(ARG_TEXT(operand)));
        else {
            import_report(importer, statement, N00B_DIAG_IMPORT_NOT_FOUND,
                N00B_TEXT_CANNOT_FIND_MODULE,
                N00BB_ARGS(ARG_TEXT(operand)));
            n00bb_resolver_note_roots(driver->resolver,
                core_translation_unit_diagnostics(importer));
        }
        return false;
    }
    core_translation_unit_t *imported = core_work_job_result(
        driver->queue, locate_job);
    n00bb_tu_state_t *state = tu_state(imported);
    n00bb_lex_result_t *lex = state ? state->lex : nullptr;
    parse_tree_t *tree = lex && lex->parser
        ? parse_pwz_get_tree(lex->parser) : nullptr;
    if (!imported) {
        import_internal(importer, statement,
                        "the imported module has no translation unit");
        return false;
    }
    if (!tree) {
        import_internal(importer, statement,
                        "the imported module has no parse tree");
        return false;
    }
    core_string_t imported_name = module->identifier;
    if (statement && source_name.data
        && !core_string_eq(source_name, operand)
        && n00bb_same_package(importer, imported)
        && !record_import_alias(importer, source_name, imported)) {
        import_internal(importer, statement,
                        "could not record an internal module import");
        return false;
    }
    if (!core_translation_unit_add_import(importer, imported)) {
        import_internal(importer, statement,
                        "could not record an imported module");
        return false;
    }

    bool in_progress = false;
    if (!ensure_preprocessed_import(driver, grammar, locate_job,
                                    imported, tree, &in_progress)) {
        if (failure_already_reported(imported))
            mark_import_failure_reported(importer);
        else import_internal(importer, statement,
                             "an imported module failed without a diagnostic");
        return false;
    }
    if (statement && n00bb_same_package(importer, imported)
        && find_package_declaration(imported,
            source_name.data ? source_name : CORE_STRING_STATIC(""))) {
        import_report(importer, statement, N00B_DIAG_IMPORT_OWN_NAMESPACE,
            N00B_TEXT_IS_THIS_PACKAGE_S_OWN,
            N00BB_ARGS(ARG_TEXT(source_name)));
        n00bb_report_help(core_translation_unit_diagnostics(importer),
            N00B_TEXT_INSIDE_A_PACKAGE_IMPORT_ITS,
            N00BB_NO_ARGS);
        return false;
    }
    bool direct_foreign = !n00bb_same_package(importer, imported)
        && !find_package_declaration(imported, imported_name);
    if (direct_foreign
        && (!statement || !package_has_manifest(driver, imported)))
        n00bb_mark_open_module(imported);
    else if (direct_foreign && !state->package_interface && !in_progress) {
        core_string_t package = n00bb_package_namespace(imported);
        import_report(importer, statement, N00B_DIAG_IMPORT_LIBRARY_SOURCE,
            N00B_TEXT_IS_A_SOURCE_MODULE_OF,
            N00BB_ARGS(ARG_TEXT(operand), ARG_TEXT(package)));
        n00bb_report_help(core_translation_unit_diagnostics(importer),
            N00B_TEXT_IMPORT_A_NAME_THAT_PACKAGE,
            N00BB_ARGS(ARG_TEXT(package)));
        return false;
    }
    if (state && state->interface_module && state->package_interface
        && !record_import_alias(importer, imported_name, imported)) {
        import_internal(importer, statement,
                        "could not record a package interface import");
        return false;
    }
    *imported_out = imported;
    *tree_out = tree;
    return true;
}

static void
collect_postprune_package_syntax(parse_grammar_t *grammar,
    parse_tree_t *node, bool inside_package,
    core_list_t(parse_tree_ptr_t) *declarations,
    core_list_t(parse_tree_ptr_t) *exports)
{
    if (!node || core_tree_is_leaf(node)) return;
    bool package = nt_is(grammar, node, CORE_STRING_STATIC("package-decl"));
    if (package && !inside_package) core_list_push(*declarations, node);
    if (nt_is(grammar, node, CORE_STRING_STATIC("export-stmt")))
        core_list_push(*exports, node);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_postprune_package_syntax(grammar, core_tree_child(node, i),
            inside_package || package, declarations, exports);
}

static bool
process_postprune_package_syntax(parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *tree)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !tree) return false;
    core_list_t(parse_tree_ptr_t) declarations = core_list_new(parse_tree_ptr_t);
    core_list_t(parse_tree_ptr_t) exports = core_list_new(parse_tree_ptr_t);
    collect_postprune_package_syntax(grammar, tree, false, &declarations,
                                     &exports);
    for (size_t i = 0; i < state->package_declarations.len; i++)
        core_list_free(state->package_declarations.data[i].exports);
    state->package_declarations.len = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < declarations.len; i++)
        ok = cache_package_declaration_syntax(grammar, unit,
                                               declarations.data[i]);
    for (size_t i = 0; ok && i < exports.len; i++)
        ok = cache_package_export_syntax(grammar, unit, exports.data[i]);
    core_list_free(exports);
    core_list_free(declarations);
    return ok;
}

static bool
read_alias_text(const uint8_t *data, size_t size, size_t *at,
                core_string_t *out)
{
    if (size - *at < 4) return false;
    uint32_t length = read_u32_le(data + *at);
    *at += 4;
    if (length > size - *at) return false;
    *out = core_string_borrow((const char *)data + *at, length);
    *at += length;
    return true;
}

static bool
restore_interface_import_aliases(import_driver_t *driver,
                                 core_translation_unit_t *unit)
{
    core_string_t module_name = unit->module.identifier;
    size_t size = 0;
    const uint8_t *data = module_name.data
        ? interface_owned_metadata(&driver->loader->interfaces,
            TH_ENV_METADATA_ENVIRONMENT, module_name,
            CORE_STRING_STATIC("n00b.module-import-aliases"), 1, &size)
        : nullptr;
    if (!data) return true;
    if (size < 4) return false;
    uint32_t count = read_u32_le(data);
    size_t at = 4;
    for (uint32_t i = 0; i < count; i++) {
        core_string_t alias = {0}, target = {0};
        if (!read_alias_text(data, size, &at, &alias)
            || !read_alias_text(data, size, &at, &target)) return false;
        core_translation_unit_t *imported = nullptr;
        for (size_t k = 0; !imported
             && k < core_translation_unit_import_count(unit); k++) {
            core_translation_unit_t *candidate =
                core_translation_unit_import(unit, k);
            if (core_string_eq(candidate->module.identifier, target))
                imported = candidate;
        }
        if (!imported || !record_import_alias(unit, alias, imported)) {
            import_internal(unit, nullptr,
                "a cached module interface has an unresolved import alias");
            return false;
        }
    }
    return at == size;
}

static bool
load_interface_imports(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit)
{
    core_string_t module = unit->module.identifier;
    if (n00bb_cache_has_module(driver->loader->cache, module)
        && !interface_restore_package_declarations(
            &driver->loader->interfaces, unit)) {
        import_internal(unit, nullptr,
            "a cached module interface has malformed package declarations");
        return false;
    }
    n00bb_cache_declaration_sites(driver->loader->cache, module,
                                  &tu_state(unit)->declaration_sites);
    core_string_t module_name = module;
    size_t size = 0;
    const uint8_t *data = module_name.data
        ? interface_owned_metadata(&driver->loader->interfaces,
            TH_ENV_METADATA_ENVIRONMENT, module_name,
            CORE_STRING_STATIC("n00b.module-imports"), 1, &size)
        : nullptr;
    if (!data && module_name.data)
        data = interface_owned_metadata(&driver->loader->interfaces,
            TH_ENV_METADATA_ENVIRONMENT, module_name,
            CORE_STRING_STATIC("n00b.package-imports"), 1, &size);
    bool module_interface = module_name.data
        && interface_owned_metadata(&driver->loader->interfaces,
            TH_ENV_METADATA_ENVIRONMENT, module_name,
            CORE_STRING_STATIC("n00b.module-interface"), 1, nullptr);
    bool package_only = !data && module_name.data && !module_interface;
    tu_state(unit)->package_interface = !module_interface;
    if (package_only) return true;
    if (!data || size < 4) {
        import_internal(unit, nullptr,
                        "a cached module interface has no import graph");
        return false;
    }
    size_t offset = 4;
    uint32_t count = read_u32_le(data);
    for (uint32_t i = 0; i < count; i++) {
        if (size - offset < 4) break;
        uint32_t length = read_u32_le(data + offset);
        offset += 4;
        if (!length || length > size - offset
            || memchr(data + offset, '\0', length)) break;
        core_string_t name = core_string_borrow(
            (const char *)data + offset, length);
        core_module_spec_t spec = {0};
        core_translation_unit_t *imported = nullptr;
        parse_tree_t *tree = nullptr;
        bool ok = core_module_spec_init(&spec, name)
            && load_import_module(driver, grammar, nullptr, unit, &spec,
                                  name, (core_string_t){0}, &imported, &tree);
        core_module_spec_release(&spec);
        if (!ok) return false;
        offset += length;
        if (i + 1 == count && offset == size)
            return restore_interface_import_aliases(driver, unit);
    }
    if (!count && offset == size)
        return restore_interface_import_aliases(driver, unit);
    import_internal(unit, nullptr,
                    "a cached module interface has a malformed import graph");
    return false;
}

static n00bb_literal_origin_t
literal_origin(core_translation_unit_t *unit, parse_token_info_t *token)
{
    const char *contents = core_source_file_contents(&unit->source).data;
    uint32_t line = parse_token_line(token);
    uint32_t column = parse_token_column(token);
    size_t content_start = 0;
    parse_token_content_span(token, &content_start, nullptr);
    for (size_t at = parse_token_start_byte(token); at < content_start; at++) {
        if (contents[at] == '\n') {
            line++;
            column = 1;
        }
        else column++;
    }
    return (n00bb_literal_origin_t){
        .host_path = core_source_file_path(&unit->source),
        .line = line,
        .column = column,
        .byte = content_start,
    };
}

static void
module_literal_identifier(core_buffer_t *out, core_string_t importer,
                          core_string_t modifier, core_string_t name)
{
    size_t leaf = importer.u8_bytes;
    while (leaf && importer.data[leaf - 1] != '.') leaf--;
    core_buffer_printf(out, "%.*s__%.*s_%.*s_%.*s",
        (int)leaf, importer.data, (int)modifier.u8_bytes, modifier.data,
        (int)(importer.u8_bytes - leaf), importer.data + leaf,
        (int)name.u8_bytes, name.data);
}

static bool
show_module_literal(n00bb_resolver_t *resolver, core_string_t modifier,
                    core_string_t qualified)
{
    size_t split = qualified.u8_bytes;
    while (split && qualified.data[split - 1] != '.') split--;
    if (split < 2 || split == qualified.u8_bytes) return false;
    core_buffer_t identifier = {0};
    module_literal_identifier(&identifier,
        core_string_borrow(qualified.data, split - 1), modifier,
        core_string_borrow(qualified.data + split,
                           qualified.u8_bytes - split));
    core_string_t text = {0};
    bool found = n00bb_resolver_generated_text(resolver,
        core_string_borrow(identifier.data, identifier.byte_len), &text);
    if (found) fwrite(text.data, 1, text.u8_bytes, stdout);
    core_buffer_release(&identifier);
    return found;
}

static bool
load_module_literal(void *opaque, parse_grammar_t *grammar,
                    core_translation_unit_t *importer, parse_tree_t *literal,
                    core_string_t modifier, core_string_t name,
                    core_translation_unit_t **module_out,
                    parse_tree_t **tree_out)
{
    import_driver_t *driver = opaque;
    const n00bb_module_generator_t *generator = nullptr;
    for (size_t i = 0; driver->generators && i < driver->generators->count; i++)
        if (core_string_eq(driver->generators->items[i].modifier, modifier))
            generator = &driver->generators->items[i];
    parse_token_info_t *token = parse_node_token(literal);
    if (!generator || !token) return false;
    const char *contents = core_source_file_contents(&importer->source).data;
    size_t content_start = 0;
    size_t content_end = 0;
    parse_token_content_span(token, &content_start, &content_end);
    core_string_t text = core_string_borrow(contents + content_start,
        content_end - content_start);
    core_buffer_t identifier = {0};
    module_literal_identifier(&identifier, importer->module.identifier,
                              modifier, name);
    core_string_t id = core_string_borrow(identifier.data, identifier.byte_len);
    core_buffer_t source = {0};
    n00bb_native_declarations_t natives = {0};
    n00bb_line_origins_t origins = {0};
    if (!generator->generate(generator->context, importer, name, id,
            literal_origin(importer, token), text, &source, &natives,
            &origins)) {
        n00bb_native_declarations_release(&natives);
        n00bb_line_origins_release(&origins);
        core_buffer_release(&source);
        core_buffer_release(&identifier);
        return false;
    }
    core_module_spec_t module = {0};
    bool ok = n00bb_resolver_add_generated(driver->resolver, id,
                  core_string_borrow(source.data ? source.data : "", source.byte_len),
                  importer, natives, origins)
        && core_module_spec_init(&module, id)
        && load_import_module(driver, grammar, literal, importer, &module,
               core_buffer_string(&identifier), (core_string_t){0},
               module_out, tree_out)
        && record_import_alias(importer, name, *module_out);
    core_module_spec_release(&module);
    core_buffer_release(&identifier);
    core_buffer_release(&source);
    return ok;
}

static bool
load_import(void *opaque, parse_grammar_t *grammar, parse_tree_t *statement,
            core_translation_unit_t *importer,
            core_translation_unit_t **imported_out,
            parse_tree_t **tree_out)
{
    import_driver_t *driver = opaque;
    core_module_spec_t module = {0};
    core_string_t operand = {0};
    core_string_t source_name = {0};
    if (!import_request_details(driver->resolver, grammar, statement, importer,
                                &module, &operand, &source_name)) return false;
    bool loaded = load_import_module(driver, grammar, statement, importer,
        &module, operand, source_name, imported_out, tree_out);
    core_free(operand.data);
    core_free(source_name.data);
    core_module_spec_release(&module);
    return loaded;
}

static bool
load_runtime_module(import_driver_t *driver, parse_grammar_t *grammar,
                    core_translation_unit_t *importer,
                    core_string_t module_name)
{
    if (core_string_eq(importer->module.identifier, module_name)) return true;
    core_module_spec_t module = {0};
    if (!core_module_spec_init(&module, module_name)) return false;
    core_translation_unit_t *imported = nullptr;
    parse_tree_t *tree = nullptr;
    bool loaded = load_import_module(driver, grammar, nullptr, importer,
        &module, module_name, (core_string_t){0}, &imported, &tree);
    core_module_spec_release(&module);
    return loaded;
}

static bool
load_implicit_builtin(void *opaque, parse_grammar_t *grammar,
                      core_translation_unit_t *importer,
                      core_string_t module_name)
{
    core_string_t name = importer->module.identifier;
    n00bb_tu_state_t *importer_state = tu_state(importer);
    if (importer_state && importer_state->interface_module) return true;
    if (name.u8_bytes >= sizeof("builtin.") - 1
        && !memcmp(name.data, "builtin.", sizeof("builtin.") - 1))
        return true;
    return load_runtime_module(opaque, grammar, importer, module_name);
}

typedef struct {
    core_work_queue_t *queue;
    locate_requests_t *requests;
    size_t root_request_count;
    parse_grammar_t *grammar;
    ir_ctx_t *ir_context;
    const th_env *const *linked_interfaces;
    size_t linked_interface_count;
    const n00bb_cache_t *cache;
    const core_string_t *native_roots;
    size_t native_root_count;
    bool may_link_further;
    n00bb_resolver_t *resolver;
} program_link_inputs_t;

static bool
program_has_semantic_errors(const program_link_inputs_t *inputs)
{
    for (size_t i = 0; i < inputs->requests->ordered.len; i++) {
        core_translation_unit_t *unit = core_work_job_result(inputs->queue,
            inputs->requests->ordered.data[i].job);
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        if (state && state->semantic_errors) return true;
    }
    return false;
}

static bool
link_located_program(const program_link_inputs_t *inputs,
                     th_env **environment_out, core_link_unit_t **link_out,
                     n00bb_link_analysis_t **analysis_out)
{
    bool profile_compile = n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE);
    double profile_tick = profile_compile ? n00bb_profile_seconds() : 0;
    core_work_queue_t *queue = inputs->queue;
    locate_requests_t *requests = inputs->requests;
    const n00bb_cache_t *cache = inputs->cache;
    size_t linked_interface_count = inputs->linked_interface_count;
    size_t cached_count = n00bb_cache_interface_count(cache);
    size_t first_cached = linked_interface_count - cached_count;
    size_t program_interface_count = first_cached;
    const th_env **program_interfaces = core_alloc_array(
        const th_env *, linked_interface_count + 1);
    for (size_t i = 0; program_interfaces && i < first_cached; i++)
        program_interfaces[i] = inputs->linked_interfaces[i];
    for (size_t c = 0; program_interfaces && c < cached_count; c++) {
        bool used = false;
        for (size_t r = 0; !used && r < requests->ordered.len; r++) {
            core_translation_unit_t *unit = core_work_job_result(
                queue, requests->ordered.data[r].job);
            used = unit && n00bb_cache_interface_has_module(cache,
                c, unit->module.identifier);
        }
        if (used)
            program_interfaces[program_interface_count++] =
                inputs->linked_interfaces[first_cached + c];
    }
    core_link_unit_t *link = core_link_unit_new();
    n00bb_link_analysis_t *analysis = n00bb_link_analysis_new(
        inputs->grammar, inputs->ir_context, program_interfaces,
        program_interface_count, inputs->native_roots,
        inputs->native_root_count, inputs->may_link_further,
        inputs->resolver);
    core_free(program_interfaces);
    if (analysis) {
        analysis->cache = (n00bb_cache_t *)cache;
        analysis->closed_world = closed_world_build;
        analysis->keep_summaries = function_summaries_build;
    }
    bool success = link != nullptr;
    for (size_t i = 0; i < inputs->root_request_count; i++) {
        if (!requests->ordered.data[i].root_input) continue;
        core_translation_unit_t *root = core_work_job_result(
            queue, requests->ordered.data[i].job);
        if (!root || !core_link_unit_add_root(link, root))
            success = false;
    }
    static const uint32_t symbol_prerequisites[] = {
        N00BB_LINK_STAGE_INPUTS,
    };
    static const uint32_t semantic_prerequisites[] = {
        N00BB_LINK_STAGE_SYMBOLS,
    };
    static const uint32_t ir_binding_prerequisites[] = {
        N00BB_LINK_STAGE_CONTRIBUTIONS,
    };
    static const uint32_t cfg_liveness_prerequisites[] = {
        N00BB_LINK_STAGE_IR_BINDINGS,
    };
    static const uint32_t cleanup_obligations_prerequisites[] = {
        N00BB_LINK_STAGE_CFG_LIVENESS,
    };
    static const uint32_t storage_lifetimes_prerequisites[] = {
        N00BB_LINK_STAGE_CLEANUP_OBLIGATIONS,
    };
    static const uint32_t contribution_prerequisites[] = {
        N00BB_LINK_STAGE_SEMANTICS,
    };
    bool stages_registered = success
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_INPUTS, nullptr, 0,
            CORE_STRING_STATIC("n00bb.input-closure.v1"), false,
            n00bb_link_stage_inputs, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_SYMBOLS, symbol_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.symbols.v1"), true,
            n00bb_link_stage_symbols, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_SEMANTICS, semantic_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.semantic-analysis.v1"), true,
            n00bb_link_stage_semantics, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_CONTRIBUTIONS,
            contribution_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.analysis-coverage.v1"), true,
            n00bb_link_stage_contributions, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_IR_BINDINGS, ir_binding_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.ir-bindings.v1"), true,
            n00bb_link_stage_ir_bindings, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_CFG_LIVENESS,
            cfg_liveness_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.cfg-liveness.v1"), true,
            n00bb_link_stage_cfg_liveness, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_CLEANUP_OBLIGATIONS,
            cleanup_obligations_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.cleanup-obligations.v1"),
            true, n00bb_link_stage_cleanup_obligations, analysis)
        && core_link_unit_register_stage(link,
            N00BB_LINK_STAGE_STORAGE_LIFETIMES,
            storage_lifetimes_prerequisites, 1,
            CORE_STRING_STATIC("n00bb.storage-lifetimes.v1"),
            true, n00bb_link_stage_storage_lifetimes, analysis);
    if (stages_registered && program_has_semantic_errors(inputs)) {
        (void)core_link_unit_run_stage(link, N00BB_LINK_STAGE_SEMANTICS);
        success = false;
    }
    else success = stages_registered
        && core_link_unit_run_stage(link, N00BB_LINK_STAGE_CONTRIBUTIONS)
        && !core_link_unit_has_fatal_diagnostics(link);
    if (profile_compile)
        fprintf(stderr, "PROFILE link-stages %.3f\n",
                n00bb_profile_seconds() - profile_tick);
    if (environment_out) *environment_out = analysis->environment;
    if (link_out) *link_out = link;
    else core_link_unit_free(link);
    if (analysis_out) *analysis_out = analysis;
    else n00bb_link_analysis_free(analysis);
    return success;
}

static bool collect_located_diagnostics;

static bool
run_locate_jobs(core_work_queue_t *queue, n00bb_resolver_t *resolver,
                locate_requests_t *requests,
                parse_grammar_t *grammar, bool lex, bool show_tokens,
                bool parse, bool show_tree, bool preprocess,
                bool show_preprocessed, bool show_symbols,
                bool defer_diagnostics, bool may_link_further,
                bool native_build,
                const core_string_t *native_roots, size_t native_root_count,
                const th_env *const *linked_interfaces,
                size_t linked_interface_count, const n00bb_cache_t *cache,
                const warning_policy_t *warnings,
                const n00bb_preprocessor_options_t *preprocessor_options,
                ir_ctx_t *ir_context, th_ctx *types, size_t worker_limit,
                th_env **environment_out, core_link_unit_t **link_out,
                n00bb_link_analysis_t **analysis_out)
{
    bool profile_compile = n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE);
    double profile_tick = profile_compile ? n00bb_profile_seconds() : 0;
    if (environment_out) *environment_out = nullptr;
    if (link_out) *link_out = nullptr;
    if (analysis_out) *analysis_out = nullptr;
    bool success = true;
    size_t root_request_count = requests->ordered.len;
    core_diagnostics_t *diags = core_diagnostics_new();
    pthread_mutex_t requests_lock;
    pthread_mutex_init(&requests_lock, nullptr);
    locate_worker_context_t loader = {
        .queue = queue,
        .resolver = resolver,
        .requests = requests,
        .requests_lock = &requests_lock,
        .grammar = grammar,
        .types = types,
        .interfaces = {
            .ir_context = ir_context,
            .linked_interfaces = linked_interfaces,
            .linked_interface_count = linked_interface_count,
            .cache = cache,
        },
        .cache = cache,
        .lex = lex,
        .parse = parse,
    };
    pthread_t *worker_threads = worker_limit
        ? core_alloc_array(pthread_t, worker_limit) : nullptr;
    size_t worker_count = 0;
    for (; worker_count < worker_limit; worker_count++)
        if (!n00bb_start_worker(&worker_threads[worker_count],
                                locate_worker, &loader))
            break;
    import_driver_t import_driver = {
        .queue = queue,
        .resolver = resolver,
        .requests = requests,
        .requests_lock = &requests_lock,
        .loader = &loader,
        .worker_count = worker_count,
        .grammar = grammar,
        .types = types,
    };
    n00bb_preprocessor_options_t active_options = *preprocessor_options;
    active_options.load_import = load_import;
    active_options.load_implicit_import = load_implicit_builtin;
    active_options.import_failure_reported = failure_already_reported;
    active_options.import_context = &import_driver;
    active_options.prepare_import = prepare_import;
    active_options.record_import = record_import_shape;
    active_options.selected_body = n00bb_tu_remember_selected_when_body;
    active_options.build_time_use = n00bb_tu_remember_build_time_use;
    active_options.selected_body_context = nullptr;
    active_options.register_litmod = register_user_litmod;
    active_options.register_litmod_alias = register_user_litmod_alias;
    active_options.litmod_context = nullptr;
    active_options.load_module_literal = load_module_literal;
    import_driver.generators = preprocessor_options->module_literal_context;
    n00bb_preprocessor_t *preprocessor =
        n00bb_preprocessor_new(&active_options);
    import_driver.preprocessor = preprocessor;
    for (size_t request_ix = 0; request_ix < root_request_count; request_ix++) {
        locate_request_t *request = &requests->ordered.data[request_ix];
        core_work_job_t *job = request->job;
        if (!worker_count) {
            while (core_work_job_state(queue, job) != CORE_WORK_SUCCEEDED &&
                   core_work_job_state(queue, job) != CORE_WORK_FAILED) {
                core_work_job_t *ready = nullptr;
                if (!core_work_take_kind(queue, N00BB_JOB_LOCATE_TU, &ready))
                    break;
                process_locate_job(&loader, ready);
            }
        }
        bool job_ok = core_work_wait(queue, job);
        core_translation_unit_t *unit = core_work_job_result(queue, job);
        if (!job_ok) success = false;
        if (!unit) {
            core_string_t operand = request->operand;
            core_module_resolve_status_t status = request->resolve_attempted
                ? request->resolve_status : CORE_MODULE_RESOLVE_ERROR;
            report_unreadable_input(diags, resolver, operand,
                                    status == CORE_MODULE_NOT_FOUND);
            continue;
        }
        n00bb_tu_state_t *state = tu_state(unit);
        n00bb_lex_result_t *lex_result = state ? state->lex : nullptr;
        if (show_tokens && request->root_input && lex_result) {
            int32_t count = scan_stream_token_count(lex_result->tokens);
            for (int32_t i = 0; i < count; i++) {
                parse_token_info_t *token = scan_stream_get(lex_result->tokens, i);
                if (token) dump_token(grammar, unit, token);
            }
        }
        if (show_tree && request->root_input && lex_result &&
            lex_result->parser && job_ok)
            dump_tree(grammar, parse_pwz_get_tree(lex_result->parser));
        if (!lex) {
            core_string_t path = core_source_file_path(&unit->source);
            const core_module_spec_t *module = core_work_job_module(job);
            printf("located %.*s from %.*s (%zu bytes)\n",
                   (int)module->identifier.u8_bytes, module->identifier.data,
                   (int)path.u8_bytes, path.data,
                   core_source_file_size(&unit->source));
        }
    }
    if (profile_compile) {
        double now = n00bb_profile_seconds();
        fprintf(stderr, "PROFILE roots %.3f modules=%zu\n",
            now - profile_tick, requests->ordered.len);
        profile_tick = now;
    }

    if (preprocess && parse) {
        bool runtime_support_loaded = false;
        for (size_t i = 0; i < root_request_count; i++) {
            if (!requests->ordered.data[i].root_input) continue;
            core_work_job_t *dependencies[] = {requests->ordered.data[i].job};
            core_work_job_t *preprocess_job = nullptr;
            core_work_submit_result_t submitted = core_work_submit(queue,
                N00BB_JOB_PREPROCESS,
                core_work_job_module(requests->ordered.data[i].job), 0, dependencies,
                1, &preprocess_job);
            if (submitted == CORE_WORK_INVALID
                || submitted == CORE_WORK_DEPENDENCY_FAILED) {
                success = false;
                continue;
            }
            if (submitted == CORE_WORK_SUBMITTED) {
                core_work_job_t *ready = nullptr;
                if (!core_work_take_kind(queue, N00BB_JOB_PREPROCESS, &ready) ||
                    ready != preprocess_job) {
                    success = false;
                    continue;
                }
                core_translation_unit_t *unit = core_work_job_result(
                    queue, requests->ordered.data[i].job);
                n00bb_tu_state_t *state = tu_state(unit);
                n00bb_lex_result_t *lex = state ? state->lex : nullptr;
                parse_tree_t *tree = lex && lex->parser
                    ? parse_pwz_get_tree(lex->parser) : nullptr;
                bool job_ok = tree && n00bb_preprocessor_run(
                    preprocessor, grammar, tree, unit);
                if (job_ok)
                    job_ok = process_postprune_package_syntax(grammar, unit,
                                                              tree);
                if (job_ok && native_build && !runtime_support_loaded) {
                    job_ok = load_runtime_module(&import_driver, grammar,
                            unit, CORE_STRING_STATIC("sys.gc.alloc"))
                        && load_runtime_module(&import_driver, grammar,
                            unit, CORE_STRING_STATIC("sys.marshal"))
                        && load_runtime_module(&import_driver, grammar,
                            unit, CORE_STRING_STATIC("sys.box"));
                    runtime_support_loaded = job_ok;
                }
                if (job_ok) job_ok = !core_diagnostics_failed(
                    core_translation_unit_diagnostics(unit));
                if (job_ok && show_preprocessed)
                    dump_tree(grammar, tree);
                if (!core_work_complete_tu(queue, preprocess_job, unit,
                                           job_ok)
                    || core_work_job_state(queue, preprocess_job)
                        != CORE_WORK_SUCCEEDED) job_ok = false;
                if (!job_ok) success = false;
            }
            else if (core_work_job_state(queue, preprocess_job)
                     == CORE_WORK_FAILED) success = false;
        }
        n00bb_preprocessor_free(preprocessor);
        preprocessor = nullptr;
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE preprocess %.3f modules=%zu\n",
            now - profile_tick, requests->ordered.len);
            profile_tick = now;
        }
        if (success) {
            for (size_t i = 0; i < root_request_count; i++) {
                if (!requests->ordered.data[i].root_input) continue;
                core_translation_unit_t *unit = core_work_job_result(
                    queue, requests->ordered.data[i].job);
                if (!semantic_annotate_closure(grammar, types, unit,
                        finish_semantic_unit, &loader.interfaces))
                    success = false;
            }
        }
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE annotate %.3f modules=%zu\n",
                now - profile_tick, requests->ordered.len);
            profile_tick = now;
        }
        if (success) {
            program_link_inputs_t link_inputs = {
                .queue = queue, .requests = requests,
                .root_request_count = root_request_count,
                .grammar = grammar, .ir_context = ir_context,
                .linked_interfaces = linked_interfaces,
                .linked_interface_count = linked_interface_count,
                .cache = cache,
                .native_roots = native_roots,
                .native_root_count = native_root_count,
                .may_link_further = may_link_further,
                .resolver = resolver,
            };
            success = link_located_program(&link_inputs, environment_out,
                                           link_out, analysis_out);
        }
        core_work_queue_close(queue);
    }
    else core_work_queue_close(queue);

    for (size_t i = 0; i < worker_count; i++)
        pthread_join(worker_threads[i], nullptr);
    core_free(worker_threads);
    pthread_mutex_destroy(&requests_lock);

    for (size_t i = 0; i < requests->ordered.len; i++) {
        core_translation_unit_t *unit = core_work_job_result(
            queue, requests->ordered.data[i].job);
        if (unit && show_symbols) dump_symbols(unit);
        bool local = false;
        for (size_t r = 0; unit && !local && r < requests->ordered.len; r++) {
            core_translation_unit_t *root = requests->ordered.data[r].root_input
                ? core_work_job_result(queue, requests->ordered.data[r].job)
                : nullptr;
            local = root && n00bb_resolver_same_root(resolver, root, unit);
        }
        if (unit && !requests->ordered.data[i].manifest_probe)
            merge_unit_diagnostics(diags, warnings, unit, local);
    }
    for (size_t i = 0; !success && !core_diagnostics_failed(diags)
         && !core_diagnostics_failed(requests->diagnostics)
         && i < requests->ordered.len; i++) {
        core_translation_unit_t *unit = core_work_job_result(
            queue, requests->ordered.data[i].job);
        if (unit && requests->ordered.data[i].manifest_probe)
            merge_unit_diagnostics(diags, warnings, unit, true);
    }

    n00bb_preprocessor_free(preprocessor);
    bool analysis_owns_diagnostics = defer_diagnostics
        && analysis_out && *analysis_out;
    bool optimistic = n00bb_cache_optimistic(cache);
    if (!analysis_owns_diagnostics
        && (optimistic || collect_located_diagnostics))
        core_diagnostics_merge(requests->diagnostics, diags);
    else if (!analysis_owns_diagnostics && core_diagnostics_count(diags)) {
        fflush(stdout);
        n00bb_resolver_remap_diagnostics(resolver, diags);
        print_counted_diagnostics(diags, nullptr);
    }
    core_diagnostics_free(diags);
    return success;
}

static void
release_requests(core_work_queue_t *queue,
                 locate_requests_t *requests)
{
    for (size_t i = 0; i < requests->ordered.len; i++) {
        core_translation_unit_t *unit =
            core_work_job_result(queue, requests->ordered.data[i].job);
        if (unit) {
            core_translation_unit_release(unit);
            core_free(unit);
        }
        core_free(requests->ordered.data[i].operand.data);
    }
    core_list_free(requests->ordered);
    core_dict_free(&requests->by_job);
}

double
n00bb_profile_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static bool
n00bb_format_write(core_string_t target, core_string_t contents)
{
    core_os_file_info_t info = {};
    core_os_lookup_t found = core_os_stat(target, true, &info);
    if (found == CORE_OS_FAILED
        || (found == CORE_OS_FOUND && info.kind != CORE_OS_KIND_FILE))
        return false;
    unsigned mode = found == CORE_OS_FOUND ? info.mode : 0644;
    core_buffer_t temporary_name = {0};
    core_buffer_append_string(&temporary_name, target);
    core_buffer_puts(&temporary_name, ".fmt.");
    core_string_t made = {};
    int fd = core_os_make_unique_file(core_buffer_string(&temporary_name),
                                      &made);
    core_buffer_release(&temporary_name);
    bool ok = fd >= 0 && core_os_set_mode(fd, mode)
        && core_os_write_all(fd, contents.data, contents.u8_bytes);
    if (fd >= 0 && !core_os_close(fd)) ok = false;
    if (ok) ok = core_os_rename_replace(made, target);
    if (!ok && fd >= 0) core_os_remove_file(made);
    core_free(made.data);
    return ok;
}

static bool
n00bb_format_layout_token(parse_token_info_t *token, int64_t newline,
                          int64_t indent, int64_t dedent)
{
    if (!token) return true;
    int32_t tid = parse_token_tid(token);
    return tid == PARSE_TOK_EOF || tid == newline || tid == indent
        || tid == dedent;
}

static parse_token_info_t *
n00bb_format_next_token(parse_token_stream_t *tokens, int32_t *index,
                        int64_t newline, int64_t indent, int64_t dedent)
{
    while (*index < scan_stream_token_count(tokens)) {
        parse_token_info_t *token = scan_stream_get(tokens, (*index)++);
        if (!n00bb_format_layout_token(token, newline, indent, dedent))
            return token;
    }
    return nullptr;
}

static size_t
n00bb_format_newline_count(const char *text, size_t length)
{
    size_t count = 0;
    for (size_t i = 0; i < length; i++)
        if (text[i] == '\n') count++;
    return count;
}

static core_string_t
n00bb_format_restore_breaks(parse_grammar_t *grammar,
                            core_string_t source, core_string_t printed_text)
{
    const char *printed = printed_text.data;
    size_t printed_len = printed_text.u8_bytes;
    parse_token_stream_t *original = scan_tokenize_owned(grammar, source);
    parse_token_stream_t *rendered = scan_tokenize_owned(grammar,
                                                         printed_text);
    if (!original || !rendered) {
        scan_token_stream_free(original);
        scan_token_stream_free(rendered);
        return (core_string_t){0};
    }
    while (scan_stream_next(original)) {}
    while (scan_stream_next(rendered)) {}
    int64_t newline = parse_terminal_id(grammar,
        CORE_STRING_STATIC("NEWLINE"));
    int64_t indent = parse_terminal_id(grammar,
        CORE_STRING_STATIC("INDENT"));
    int64_t dedent = parse_terminal_id(grammar,
        CORE_STRING_STATIC("DEDENT"));
    core_buffer_t result = {0};
    int32_t original_index = 0, rendered_index = 0;
    parse_token_info_t *prior_original = nullptr;
    parse_token_info_t *prior_rendered = nullptr;
    bool prior_value_sigil = false;
    bool valid = true;
    for (;;) {
        parse_token_info_t *from = n00bb_format_next_token(original,
            &original_index, newline, indent, dedent);
        parse_token_info_t *to = n00bb_format_next_token(rendered,
            &rendered_index, newline, indent, dedent);
        if (!from || !to) {
            valid = from == to;
            break;
        }
        size_t from_start = parse_token_start_byte(from);
        size_t from_end = parse_token_end_byte(from);
        size_t to_start = parse_token_start_byte(to);
        size_t to_end = parse_token_end_byte(to);
        size_t prior_original_start = prior_original
            ? parse_token_start_byte(prior_original) : 0;
        size_t prior_original_end = prior_original
            ? parse_token_end_byte(prior_original) : 0;
        size_t from_len = from_end - from_start;
        size_t to_len = to_end - to_start;
        if (parse_token_tid(from) != parse_token_tid(to) || from_len != to_len
            || from_end > source.u8_bytes
            || to_end > printed_len
            || memcmp(source.data + from_start,
                      printed + to_start, from_len)) {
            valid = false;
            break;
        }
        size_t gap_start = prior_rendered
            ? parse_token_end_byte(prior_rendered) : 0;
        size_t gap_end = to_start;
        if (gap_end < gap_start) { valid = false; break; }
        size_t gap_len = gap_end - gap_start;
        size_t printed_lines = n00bb_format_newline_count(
            printed + gap_start, gap_len);
        size_t source_lines = prior_original
            ? n00bb_format_newline_count(
                source.data + prior_original_end,
                from_start - prior_original_end) : 0;
        bool has_comment = false;
        for (size_t j = 0; j < gap_len; j++)
            if (printed[gap_start + j] == '/' || printed[gap_start + j] == '#')
                has_comment = true;
        bool comma = to_len == 1 && printed[to_start] == ',';
        bool aligned_colon = prior_original && to_len == 1
            && printed[to_start] == ':'
            && source_lines == 0 && printed_lines == 0;
        bool channel_break = (to_len == 2
                              && !memcmp(printed + to_start, "->", 2))
            || (to_len == 4
                && !memcmp(printed + to_start, "fail", 4))
            || (prior_original
                && prior_original_end - prior_original_start == 2
                && !memcmp(source.data + prior_original_start,
                           "->", 2));
        bool value_sigil = false;
        if (to_len == 2
            && !memcmp(printed + to_start, "->", 2) && prior_rendered) {
            size_t prior_start = parse_token_start_byte(prior_rendered);
            size_t prior_end = parse_token_end_byte(prior_rendered);
            value_sigil = prior_end == prior_start + 1
                && printed[prior_start] == '{';
        }
        if (!value_sigil && from_len == 2
            && !memcmp(source.data + from_start, "->", 2)) {
            size_t line_start = from_start;
            while (line_start && source.data[line_start - 1] != '\n')
                line_start--;
            bool leading = true;
            for (size_t j = line_start; j < from_start && leading; j++)
                leading = source.data[j] == ' ' || source.data[j] == '\t';
            bool follows_params = prior_original
                && prior_original_end > prior_original_start
                && source.data[prior_original_end - 1] == ')';
            value_sigil = leading && !follows_params;
        }
        if (value_sigil) {
            size_t line_start = prior_rendered
                ? parse_token_start_byte(prior_rendered) : 0;
            while (line_start && printed[line_start - 1] != '\n')
                line_start--;
            size_t spaces = 4;
            while (line_start + spaces - 4 < printed_len
                   && printed[line_start + spaces - 4] == ' ')
                spaces++;
            core_buffer_putc(&result, '\n');
            for (size_t j = 0; j < spaces; j++)
                core_buffer_putc(&result, ' ');
        }
        else if (prior_value_sigil) {
            core_buffer_putc(&result, ' ');
        }
        else if (aligned_colon && from_start >= prior_original_end) {
            size_t original_gap = from_start - prior_original_end;
            bool spaces_only = original_gap > gap_len;
            for (size_t j = 0; j < original_gap && spaces_only; j++)
                spaces_only = source.data[prior_original_end + j] == ' ';
            if (spaces_only)
                core_buffer_append(&result,
                    source.data + prior_original_end, original_gap);
            else core_buffer_append(&result, printed + gap_start, gap_len);
        }
        else if (comma && !printed_lines && !has_comment) {
            /* A separator belongs to the preceding item. */
        }
        else if (channel_break && !printed_lines && !has_comment)
            core_buffer_append(&result, printed + gap_start, gap_len);
        else if (prior_original && source_lines > printed_lines
                 && has_comment) {
            for (size_t j = printed_lines; j < source_lines; j++)
                core_buffer_putc(&result, '\n');
            core_buffer_append(&result, printed + gap_start, gap_len);
        }
        else if (prior_original && source_lines > printed_lines
                 && !has_comment) {
            for (size_t j = 0; j < source_lines; j++)
                core_buffer_putc(&result, '\n');
            size_t spaces = 0;
            if (printed_lines) {
                size_t j = gap_end;
                while (j > gap_start && printed[j - 1] == ' ') {
                    spaces++;
                    j--;
                }
            }
            else {
                size_t j = from_start;
                while (j > 0 && source.data[j - 1] == ' ') {
                    spaces++;
                    j--;
                }
            }
            for (size_t j = 0; j < spaces; j++)
                core_buffer_putc(&result, ' ');
        }
        else core_buffer_append(&result, printed + gap_start, gap_len);
        core_buffer_append(&result, printed + to_start, to_len);
        prior_original = from;
        prior_rendered = to;
        prior_value_sigil = value_sigil;
    }
    if (valid) {
        size_t suffix = prior_rendered ? parse_token_end_byte(prior_rendered)
                                       : 0;
        core_buffer_append(&result, printed + suffix, printed_len - suffix);
        while (result.byte_len && result.data[result.byte_len - 1] == '\n')
            result.data[--result.byte_len] = '\0';
        core_buffer_putc(&result, '\n');
    }
    scan_token_stream_free(original);
    scan_token_stream_free(rendered);
    if (!valid) {
        core_buffer_release(&result);
        return (core_string_t){0};
    }
    return core_buffer_string(&result);
}

typedef struct {
    const char *start;
    size_t length;
    size_t colon;
    size_t type_start;
    size_t type_end;
    size_t equal;
    size_t brace;
    size_t indent;
    int kind;
} n00bb_format_line_t;

#define n00bb_format_word(text, length, at, lit) \
    n00bb_format_word_text((text), (length), (at), CORE_STRING_STATIC(lit))

static bool
n00bb_format_word_text(const char *text, size_t length, size_t *at,
                       core_string_t word)
{
    size_t count = word.u8_bytes;
    if (*at + count > length || memcmp(text + *at, word.data, count))
        return false;
    if (*at + count < length) {
        char c = text[*at + count];
        if (c != ' ' && c != '\t') return false;
    }
    *at += count;
    while (*at < length && text[*at] == ' ') (*at)++;
    return true;
}

static void
n00bb_format_line_classify(n00bb_format_line_t *line, bool in_record)
{
    size_t at = 0;
    while (at < line->length && line->start[at] == ' ') at++;
    line->indent = at;
    bool top_level = at == 0;
    if (!in_record && line->length) {
        size_t name = at;
        if (top_level) {
            n00bb_format_word(line->start, line->length, &name, "private");
        }
        size_t keyword = name;
        if (top_level && n00bb_format_word(line->start, line->length,
                              &keyword, "enum")) {
            for (size_t i = keyword; i < line->length; i++)
                if (line->start[i] == '{') {
                    line->kind = 3;
                    line->brace = i;
                    return;
                }
        }
        keyword = name;
        bool variable = n00bb_format_word(line->start, line->length,
                                          &keyword, "const")
            || n00bb_format_word(line->start, line->length,
                                 &keyword, "var")
            || n00bb_format_word(line->start, line->length,
                                 &keyword, "let");
        if (variable) at = keyword;
        else return;
    }
    if (in_record && !top_level) {
        size_t name = at;
        n00bb_format_word(line->start, line->length, &name, "private");
        at = name;
    }
    size_t name_start = at;
    while (at < line->length) {
        char c = line->start[at];
        if (!(c == '_' || (c >= 'a' && c <= 'z')
              || (c >= 'A' && c <= 'Z')
              || (at > name_start && c >= '0' && c <= '9')))
            break;
        at++;
    }
    if (at == name_start || at >= line->length || line->start[at] != ':')
        return;
    line->kind = in_record ? 2 : 1;
    line->colon = at;
    at++;
    while (at < line->length && line->start[at] == ' ') at++;
    line->type_start = at;
    line->type_end = line->length;
    line->equal = line->length;
    if (line->kind == 1)
        for (size_t i = at; i < line->length; i++)
            if (line->start[i] == '=') {
                line->equal = i;
                line->type_end = i;
                while (line->type_end > at
                       && line->start[line->type_end - 1] == ' ')
                    line->type_end--;
                break;
            }
}

static core_string_t
n00bb_format_align(core_string_t input)
{
    const char *text = input.data;
    size_t length = input.u8_bytes, count = 0;
    for (size_t i = 0; i < length; i++)
        if (text[i] == '\n') count++;
    n00bb_format_line_t *lines = core_alloc_array(n00bb_format_line_t,
                                                  count + 1);
    size_t used = 0, start = 0;
    bool in_record = false;
    for (size_t i = 0; i <= length; i++) {
        if (i != length && text[i] != '\n') continue;
        n00bb_format_line_t *line = &lines[used++];
        *line = (n00bb_format_line_t){
            .start = text + start,
            .length = i - start,
        };
        if (!in_record) {
            size_t at = 0;
            n00bb_format_word(line->start, line->length, &at, "private");
            if (n00bb_format_word(line->start, line->length, &at, "record")
                || n00bb_format_word(line->start, line->length, &at,
                                     "struct")
                || n00bb_format_word(line->start, line->length, &at,
                                     "object"))
                for (size_t j = at; j < line->length; j++)
                    if (line->start[j] == '{') in_record = true;
        }
        else if (line->length && line->start[0] == '}')
            in_record = false;
        n00bb_format_line_classify(line, in_record);
        start = i + 1;
    }
    core_buffer_t result = {0};
    for (size_t i = 0; i < used;) {
        size_t end = i + 1, maximum = 0, type_width = 0;
        int kind = lines[i].kind;
        while (kind && end < used
               && ((lines[end].kind == kind
                    && lines[end].indent == lines[i].indent)
                   || (kind == 1 && !lines[end].length)))
            end++;
        for (size_t j = i; j < end; j++) {
            if (lines[j].kind != kind) continue;
            size_t at = kind == 3 ? lines[j].brace : lines[j].colon;
            if (at > maximum) maximum = at;
            if (kind == 1 && lines[j].equal < lines[j].length
                && lines[j].type_end - lines[j].type_start > type_width)
                type_width = lines[j].type_end - lines[j].type_start;
        }
        for (size_t j = i; j < end; j++) {
            n00bb_format_line_t *line = &lines[j];
            if ((kind == 1 || kind == 2) && line->kind == kind) {
                core_buffer_append(&result, line->start, line->colon + 1);
                for (size_t col = line->colon + 1; col < maximum + 2; col++)
                    core_buffer_putc(&result, ' ');
                if (kind == 1 && line->equal < line->length) {
                    core_buffer_append(&result,
                        line->start + line->type_start,
                        line->type_end - line->type_start);
                    size_t spaces = type_width + 1
                        - (line->type_end - line->type_start);
                    for (size_t col = 0; col < spaces; col++)
                        core_buffer_putc(&result, ' ');
                    core_buffer_append(&result, line->start + line->equal,
                        line->length - line->equal);
                }
                else core_buffer_append(&result,
                    line->start + line->type_start,
                    line->length - line->type_start);
            }
            else if (kind == 3) {
                size_t before = line->brace;
                while (before && line->start[before - 1] == ' ') before--;
                core_buffer_append(&result, line->start, before);
                for (size_t col = before; col < maximum; col++)
                    core_buffer_putc(&result, ' ');
                core_buffer_append(&result, line->start + line->brace,
                    line->length - line->brace);
            }
            else core_buffer_append(&result, line->start, line->length);
            if (j + 1 < used) core_buffer_putc(&result, '\n');
        }
        i = end;
    }
    core_free(lines);
    return core_buffer_string(&result);
}

typedef struct {
    size_t start;
    size_t end;
    size_t indent;
} n00bb_signature_line_t;

static size_t
n00bb_signature_open(const char *text, n00bb_signature_line_t *line)
{
    size_t at = line->indent;
    if (at == line->end - line->start
        || text[line->start + at] == '/') return SIZE_MAX;
    for (;;) {
        size_t before = at;
        if (n00bb_format_word(text + line->start,
                              line->end - line->start, &at, "private")
            || n00bb_format_word(text + line->start,
                                 line->end - line->start, &at, "inline"))
            continue;
        at = before;
        break;
    }
    if (!n00bb_format_word(text + line->start,
                           line->end - line->start, &at, "func"))
        return SIZE_MAX;
    for (size_t pos = line->start + at; pos < line->end; pos++)
        if (text[pos] == '(') return pos;
    return SIZE_MAX;
}

static size_t
n00bb_signature_close(parse_token_stream_t *tokens, size_t open)
{
    int32_t count = scan_stream_token_count(tokens);
    int32_t first = 0, after = count;
    while (first < after) {
        int32_t middle = first + (after - first) / 2;
        parse_token_info_t *token = scan_stream_get(tokens, middle);
        if (parse_token_start_byte(token) < open) first = middle + 1;
        else after = middle;
    }
    parse_token_info_t *opening = first < count
        ? scan_stream_get(tokens, first) : nullptr;
    if (!opening || parse_token_start_byte(opening) != open
        || parse_token_tid(opening) != '(') return SIZE_MAX;
    size_t depth = 1;
    for (int32_t i = first + 1; i < count; i++) {
        parse_token_info_t *token = scan_stream_get(tokens, i);
        int32_t tid = parse_token_tid(token);
        if (tid == '(') depth++;
        else if (tid == ')' && !--depth)
            return parse_token_start_byte(token);
    }
    return SIZE_MAX;
}

static size_t
n00bb_signature_colon(const char *text, size_t start, size_t end)
{
    for (size_t at = start; at < end; at++) {
        if (text[at] == '=' || text[at] == '(' || text[at] == '[')
            break;
        if (text[at] == ':') return at;
    }
    return SIZE_MAX;
}

static void
n00bb_signature_row(const char *text, n00bb_signature_line_t *line,
                    size_t first, size_t last, size_t open, size_t close,
                    size_t row, size_t *start, size_t *end)
{
    *start = row == first ? open + 1 : line->start + line->indent;
    *end = row == last ? close : line->end;
    while (*start < *end && text[*start] == ' ') (*start)++;
    while (*end > *start && text[*end - 1] == ' ') (*end)--;
}

static core_string_t
n00bb_format_signatures(parse_grammar_t *grammar, core_string_t input)
{
    parse_token_stream_t *tokens = scan_tokenize_owned(grammar, input);
    if (!tokens) return (core_string_t){0};
    while (scan_stream_next(tokens)) {}
    const char *text = input.data;
    size_t length = input.u8_bytes;
    size_t count = n00bb_format_newline_count(text, length) + 1;
    n00bb_signature_line_t *lines = core_alloc_array(
        n00bb_signature_line_t, count);
    size_t used = 0, start = 0;
    for (size_t at = 0; at <= length; at++) {
        if (at != length && text[at] != '\n') continue;
        n00bb_signature_line_t *line = &lines[used++];
        *line = (n00bb_signature_line_t){.start = start, .end = at};
        while (start + line->indent < at
               && text[start + line->indent] == ' ') line->indent++;
        start = at + 1;
    }
    core_buffer_t result = {0};
    for (size_t first = 0; first < count;) {
        size_t open = n00bb_signature_open(text, &lines[first]);
        size_t close = open == SIZE_MAX ? SIZE_MAX
            : n00bb_signature_close(tokens, open);
        size_t last = first;
        while (close != SIZE_MAX && last + 1 < count
               && close > lines[last].end) last++;
        bool wrapped = close != SIZE_MAX && last > first;
        bool move_first = close != SIZE_MAX && last == first
            && close - lines[first].start + 1 > 88;
        if (wrapped || move_first) {
            bool comment = false;
            for (size_t at = open + 1; at + 1 < close; at++)
                if (text[at] == '/'
                    && (text[at + 1] == '/' || text[at + 1] == '*'))
                    comment = true;
            if (comment) wrapped = move_first = false;
        }
        size_t maximum = 0;
        if (wrapped || move_first) {
            for (size_t row = first; row <= last; row++) {
                size_t row_start, row_end;
                n00bb_signature_row(text, &lines[row], first, last,
                                    open, close, row, &row_start, &row_end);
                size_t colon = n00bb_signature_colon(text, row_start,
                                                      row_end);
                if (colon == SIZE_MAX) continue;
                size_t prefix = colon - row_start;
                while (prefix && text[row_start + prefix - 1] == ' ')
                    prefix--;
                if (prefix > maximum) maximum = prefix;
            }
        }
        if (wrapped) {
            size_t first_start, first_end;
            n00bb_signature_row(text, &lines[first], first, last,
                                open, close, first, &first_start, &first_end);
            size_t parameter_column = first_start == first_end
                ? lines[first].indent + 4
                : open - lines[first].start + 1;
            for (size_t row = first; row <= last; row++) {
                size_t row_start, row_end;
                n00bb_signature_row(text, &lines[row], first, last,
                                    open, close, row, &row_start, &row_end);
                if (row_start == row_end) continue;
                size_t colon = n00bb_signature_colon(text, row_start,
                                                      row_end);
                size_t width = colon == SIZE_MAX ? row_end - row_start
                    : maximum + row_end - colon;
                if (parameter_column + width > 88) {
                    move_first = true;
                    parameter_column = lines[first].indent + 4;
                    break;
                }
            }
        }
        if (!wrapped && !move_first) {
            core_buffer_append(&result, text + lines[first].start,
                               lines[first].end - lines[first].start);
            if (first + 1 < count) core_buffer_putc(&result, '\n');
            first++;
            continue;
        }
        size_t first_start, first_end;
        n00bb_signature_row(text, &lines[first], first, last,
                            open, close, first, &first_start, &first_end);
        size_t parameter_column = move_first || first_start == first_end
            ? lines[first].indent + 4 : open - lines[first].start + 1;
        for (size_t row = first; row <= last; row++) {
            size_t row_start, row_end;
            n00bb_signature_row(text, &lines[row], first, last,
                                open, close, row, &row_start, &row_end);
            if (row == first) {
                core_buffer_append(&result, text + lines[row].start,
                                   open + 1 - lines[row].start);
                if (move_first && row_start < row_end)
                    core_buffer_putc(&result, '\n');
            }
            if (row_start < row_end) {
                if (row != first || move_first)
                    for (size_t col = 0; col < parameter_column; col++)
                        core_buffer_putc(&result, ' ');
                size_t colon = n00bb_signature_colon(text, row_start,
                                                      row_end);
                if (colon == SIZE_MAX)
                    core_buffer_append(&result, text + row_start,
                                       row_end - row_start);
                else {
                    size_t prefix = colon - row_start;
                    while (prefix && text[row_start + prefix - 1] == ' ')
                        prefix--;
                    core_buffer_append(&result, text + row_start, prefix);
                    for (size_t col = prefix; col < maximum; col++)
                        core_buffer_putc(&result, ' ');
                    core_buffer_putc(&result, ':');
                    size_t type = colon + 1;
                    while (type < row_end && text[type] == ' ') type++;
                    if (type < row_end) core_buffer_putc(&result, ' ');
                    core_buffer_append(&result, text + type, row_end - type);
                }
            }
            if (row == last) {
                if (row_start == row_end && row != first)
                    for (size_t col = 0;
                         col < open - lines[first].start; col++)
                        core_buffer_putc(&result, ' ');
                core_buffer_append(&result, text + close,
                                   lines[row].end - close);
            }
            if (row + 1 < count) core_buffer_putc(&result, '\n');
        }
        first = last + 1;
    }
    core_free(lines);
    scan_token_stream_free(tokens);
    return core_buffer_string(&result);
}

static core_string_t
n00bb_format_channels(parse_grammar_t *grammar, core_string_t input)
{
    parse_token_stream_t *tokens = scan_tokenize_owned(grammar, input);
    if (!tokens) return (core_string_t){0};
    while (scan_stream_next(tokens)) {}
    const char *text = input.data;
    core_buffer_t result = {0};
    size_t length = input.u8_bytes, start = 0, paren_column = 0;
    size_t signature_close = SIZE_MAX;
    for (size_t i = 0; i <= length; i++) {
        if (i != length && text[i] != '\n') continue;
        const char *line = text + start;
        size_t line_length = i - start, indent = 0;
        while (indent < line_length && line[indent] == ' ') indent++;
        n00bb_signature_line_t signature = {
            .start = start, .end = i, .indent = indent,
        };
        size_t open = n00bb_signature_open(text, &signature);
        if (open != SIZE_MAX) {
            paren_column = open - start;
            signature_close = n00bb_signature_close(tokens, open);
        }
        if (line_length > 88 && signature_close >= start
            && signature_close < i) {
            size_t channel_start = signature_close + 1;
            while (channel_start < i && text[channel_start] == ' ')
                channel_start++;
            bool arrow = channel_start + 1 < i
                && text[channel_start] == '-'
                && text[channel_start + 1] == '>';
            bool fail_channel = channel_start + 4 < i
                && !memcmp(text + channel_start, "fail:", 5);
            size_t body_end = i;
            while (body_end > channel_start && text[body_end - 1] == ' ')
                body_end--;
            bool opens_body = body_end > channel_start
                && text[body_end - 1] == '{';
            if (arrow) {
                size_t result_start = channel_start + 2;
                while (result_start < i && text[result_start] == ' ')
                    result_start++;
                core_buffer_append(&result, line,
                    signature_close + 1 - start);
                core_buffer_append(&result, " ->\n", 4);
                for (size_t j = 0; j <= paren_column; j++)
                    core_buffer_putc(&result, ' ');
                core_buffer_append(&result, text + result_start,
                    i - result_start);
                if (i < length) core_buffer_putc(&result, '\n');
                start = i + 1;
                continue;
            }
            if (fail_channel && opens_body) {
                core_buffer_append(&result, line,
                    signature_close + 1 - start);
                core_buffer_putc(&result, '\n');
                for (size_t j = 0; j < paren_column; j++)
                    core_buffer_putc(&result, ' ');
                core_buffer_append(&result, text + channel_start,
                    i - channel_start);
                if (i < length) core_buffer_putc(&result, '\n');
                start = i + 1;
                continue;
            }
        }
        bool channel = (line_length - indent >= 2
                && line[indent] == '-' && line[indent + 1] == '>')
            || (line_length - indent >= 5
                && !memcmp(line + indent, "fail:", 5));
        if (channel && line[indent] == '-' && result.byte_len
            && result.data[result.byte_len - 1] == '\n') {
            size_t prior = result.byte_len - 1;
            while (prior && result.data[prior - 1] == ' ') prior--;
            channel = !prior || result.data[prior - 1] != '{';
        }
        bool opening_brace = line_length - indent == 1
            && line[indent] == '{';
        bool joined = false;
        if ((channel || opening_brace) && result.byte_len
            && result.data[result.byte_len - 1] == '\n') {
            size_t prior_end = result.byte_len - 1;
            size_t prior_start = prior_end;
            while (prior_start && result.data[prior_start - 1] != '\n')
                prior_start--;
            size_t prior_len = prior_end - prior_start;
            while (prior_len && result.data[prior_start + prior_len - 1] == ' ')
                prior_len--;
            bool previous_closes_params = prior_len
                && result.data[prior_start + prior_len - 1] == ')';
            core_string_t prior_text = core_string_slice(
                core_buffer_string(&result), prior_start,
                result.byte_len - prior_start);
            bool previous_has_channel = prior_len
                && core_string_find(prior_text,
                                    CORE_STRING_STATIC("-> ")) >= 0
                && !memchr(result.data + prior_start, '{', prior_len);
            if (((channel && previous_closes_params)
                 || (opening_brace && (previous_closes_params
                                       || previous_has_channel)))
                && prior_len + 1 + line_length - indent <= 88) {
                result.byte_len = prior_start + prior_len;
                result.data[result.byte_len] = '\0';
                core_buffer_putc(&result, ' ');
                core_buffer_append(&result, line + indent,
                                   line_length - indent);
                joined = true;
            }
        }
        bool leading_arrow = channel && line[indent] == '-';
        if (!joined && leading_arrow && result.byte_len
            && result.data[result.byte_len - 1] == '\n') {
            size_t prior_end = result.byte_len - 1;
            while (prior_end && result.data[prior_end - 1] == ' ')
                prior_end--;
            result.byte_len = prior_end;
            result.data[result.byte_len] = '\0';
            core_buffer_append(&result, " ->\n", 4);
            size_t result_start = indent + 2;
            while (result_start < line_length && line[result_start] == ' ')
                result_start++;
            for (size_t j = 0; j <= paren_column; j++)
                core_buffer_putc(&result, ' ');
            core_buffer_append(&result, line + result_start,
                               line_length - result_start);
            joined = true;
        }
        if (!joined) {
            if (channel && paren_column) {
                for (size_t j = 0; j < paren_column; j++)
                    core_buffer_putc(&result, ' ');
                core_buffer_append(&result, line + indent,
                                   line_length - indent);
            }
            else core_buffer_append(&result, line, line_length);
        }
        if (i < length) core_buffer_putc(&result, '\n');
        start = i + 1;
    }
    scan_token_stream_free(tokens);
    return core_buffer_string(&result);
}

static parse_pwz_parser_t *
n00bb_format_parse(parse_grammar_t *grammar, core_string_t text,
                   parse_token_stream_t **tokens_out)
{
    parse_token_stream_t *tokens = scan_tokenize_owned(grammar, text);
    if (!tokens) return nullptr;
    while (scan_stream_next(tokens)) {}
    bool ok = discard_ignored_layout(grammar, tokens);
    scan_diagnostic_t diagnostic = {0};
    if (scan_diagnostic(tokens->scanner, &diagnostic)
        && diagnostic.status != SCAN_STATUS_COMPLETE)
        ok = false;
    parse_pwz_parser_t *parser = nullptr;
    if (ok) {
        scan_stream_rewind(tokens, scan_stream_token_count(tokens));
        parse_nt_id_t start = parse_nonterm_id(parse_find_nonterm(
            grammar, CORE_STRING_STATIC("module")));
        parser = parse_pwz_new_for_start(grammar, start);
        parse_pwz_set_disambiguator(parser,
            n00bb_type_bracket_disambiguator, nullptr);
        ok = parse_pwz_parse(parser, tokens);
    }
    if (ok) {
        *tokens_out = tokens;
        return parser;
    }
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    return nullptr;
}

static bool
n00bb_format_parses(parse_grammar_t *grammar, core_string_t text)
{
    parse_token_stream_t *tokens = nullptr;
    parse_pwz_parser_t *parser = n00bb_format_parse(grammar, text, &tokens);
    bool ok = parser != nullptr;
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    return ok;
}

typedef struct {
    size_t at;
    size_t end;
    core_string_t type;
} n00bb_format_type_insertion_t;

static int
n00bb_format_type_insertion_compare(const void *left, const void *right)
{
    const n00bb_format_type_insertion_t *a = left;
    const n00bb_format_type_insertion_t *b = right;
    return a->at < b->at ? -1 : a->at > b->at ? 1 : 0;
}

static void
n00bb_format_type_names(parse_grammar_t *grammar, parse_tree_t *node,
                        core_string_t source,
                        n00bb_format_type_insertion_t *insertions,
                        size_t *used, size_t capacity, bool in_type)
{
    if (!node) return;
    if (!core_tree_is_leaf(node)) {
        in_type = in_type
            || nt_is(grammar, node, CORE_STRING_STATIC("type-spec"))
            || nt_is(grammar, node, CORE_STRING_STATIC("one-tspec"));
        for (size_t i = 0; i < core_tree_num_children(node); i++)
            n00bb_format_type_names(grammar, core_tree_child(node, i),
                source, insertions, used, capacity, in_type);
        return;
    }
    if (!in_type || *used == capacity) return;
    parse_token_info_t *token = parse_node_token(node);
    if (!token) return;
    size_t start = parse_token_start_byte(token);
    size_t end = parse_token_end_byte(token);
    if (end > source.u8_bytes || (start && source.data[start - 1] == '.'))
        return;
    size_t length = end - start;
    core_string_t replacement = {0};
    if (length == 3 && !memcmp(source.data + start, "u64", 3))
        replacement = CORE_STRING_STATIC("uint");
    else if (length == 3
             && !memcmp(source.data + start, "i64", 3))
        replacement = CORE_STRING_STATIC("int");
    if (replacement.data)
        insertions[(*used)++] = (n00bb_format_type_insertion_t){
            .at = start,
            .end = end,
            .type = core_string_dup(replacement),
        };
}

static core_string_t
n00bb_format_canonical_type(core_string_t printed)
{
    core_buffer_t result = {0};
    const char *type = printed.data;
    size_t length = printed.u8_bytes;
    for (size_t i = 0; i < length;) {
        bool left = i == 0 || !(type[i - 1] == '.'
            || type[i - 1] == '_'
            || (type[i - 1] >= 'a' && type[i - 1] <= 'z')
            || (type[i - 1] >= 'A' && type[i - 1] <= 'Z')
            || (type[i - 1] >= '0' && type[i - 1] <= '9'));
        bool right = i + 3 <= length && (i + 3 == length
            || !(type[i + 3] == '_'
                 || (type[i + 3] >= 'a' && type[i + 3] <= 'z')
                 || (type[i + 3] >= 'A' && type[i + 3] <= 'Z')
                 || (type[i + 3] >= '0' && type[i + 3] <= '9')));
        if (left && right && !memcmp(type + i, "u64", 3)) {
            core_buffer_puts(&result, "uint");
            i += 3;
        }
        else if (left && right && !memcmp(type + i, "i64", 3)) {
            core_buffer_puts(&result, "int");
            i += 3;
        }
        else core_buffer_putc(&result, type[i++]);
    }
    return core_buffer_string(&result);
}

static core_string_t
n00bb_format_inferred_types(parse_grammar_t *grammar,
                            n00bb_tu_state_t *state, core_string_t source)
{
    parse_symtab_t *symtab = state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return core_string_dup(source);
    size_t capacity = state->lex && state->lex->tokens
        ? (size_t)scan_stream_token_count(state->lex->tokens) : 0;
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next)
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope)
            capacity++;
    n00bb_format_type_insertion_t *insertions =
        core_alloc_array(n00bb_format_type_insertion_t, capacity + 1);
    size_t used = 0;
    if (state->lex && state->lex->parser)
        n00bb_format_type_names(grammar,
            parse_pwz_get_tree(state->lex->parser), source,
            insertions, &used, capacity, false);
    for (parse_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
             symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_VARIABLE || !symbol->type
                || !symbol->decl_node
                || !nt_is(grammar, symbol->decl_node,
                          CORE_STRING_STATIC("sym-decl"))
                || child_nt(grammar, symbol->decl_node,
                            CORE_STRING_STATIC("type-spec"))
                || !core_tree_num_children(symbol->decl_node))
                continue;
            parse_tree_t *name = core_tree_child(symbol->decl_node, 0);
            parse_token_info_t *token = core_tree_is_leaf(name)
                ? parse_node_token(name) : nullptr;
            size_t start = token ? parse_token_start_byte(token) : 0;
            size_t end = token ? parse_token_end_byte(token) : 0;
            if (!token || end <= start
                || end > source.u8_bytes
                || end - start != symbol->name.u8_bytes
                || memcmp(source.data + start,
                          symbol->name.data, symbol->name.u8_bytes))
                continue;
            core_string_t printed = n00bb_print_type_qualified(state->types,
                                                     symbol->type);
            if (!printed.data || !printed.u8_bytes
                || core_string_find_byte(printed, '<') >= 0
                || core_string_find_byte(printed, '$') >= 0
                || core_string_find_byte(printed, '%') >= 0)
                continue;
            if (used == capacity) continue;
            insertions[used++] = (n00bb_format_type_insertion_t){
                .at = end,
                .end = end,
                .type = n00bb_format_canonical_type(printed),
            };
        }
    }
    qsort(insertions, used, sizeof(*insertions),
          n00bb_format_type_insertion_compare);
    core_buffer_t result = {0};
    size_t cursor = 0;
    for (size_t i = 0; i < used; i++) {
        if (insertions[i].at >= cursor && insertions[i].end <= source.u8_bytes) {
            core_buffer_append(&result, source.data + cursor,
                               insertions[i].at - cursor);
            if (insertions[i].at == insertions[i].end)
                core_buffer_puts(&result, ": ");
            core_buffer_append_string(&result, insertions[i].type);
            cursor = insertions[i].end;
        }
        core_free(insertions[i].type.data);
    }
    core_buffer_append(&result, source.data + cursor,
                       source.u8_bytes - cursor);
    core_free(insertions);
    return result.data ? core_buffer_string(&result)
                       : core_string_dup(source);
}

static size_t
utf8_sequence_length(unsigned char lead)
{
    return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
}

static bool
rich_tag_closes(const char *text, size_t at, size_t end, bool guillemet)
{
    if (at + 1 >= end) return false;
    return guillemet
        ? (unsigned char)text[at] == 0xc2
            && (unsigned char)text[at + 1] == 0xbb
        : text[at] == ':' && text[at + 1] == ']';
}

static void
format_rich_body(core_buffer_t *out, const char *text, size_t start,
                 size_t end, bool ascii)
{
    size_t i = start;
    while (i < end) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\\') {
            size_t length = i + 1 < end
                ? 1 + utf8_sequence_length((unsigned char)text[i + 1]) : 1;
            if (i + length > end) length = end - i;
            core_buffer_append(out, text + i, length);
            i += length;
            continue;
        }
        bool guillemet = c == 0xc2 && i + 1 < end
            && (unsigned char)text[i + 1] == 0xab;
        bool colon = c == '[' && i + 1 < end && text[i + 1] == ':';
        size_t close = i + 2;
        while ((guillemet || colon) && close < end
               && !rich_tag_closes(text, close, end, guillemet))
            close++;
        if (!(guillemet || colon) || close >= end) {
            core_buffer_putc(out, (char)c);
            i++;
            continue;
        }
        size_t tag = i + 2;
        bool fits = true;
        for (size_t j = tag; fits && j < close; j++)
            fits = !rich_tag_closes(text, j, close, !ascii);
        if (fits) {
            core_buffer_append(out, ascii ? "[:" : "\xc2\xab", 2);
            core_buffer_append(out, text + tag, close - tag);
            core_buffer_append(out, ascii ? ":]" : "\xc2\xbb", 2);
        }
        else core_buffer_append(out, text + i, close + 2 - i);
        i = close + 2;
    }
}

static core_string_t
n00bb_format_rich_tags(parse_grammar_t *grammar, core_string_t input,
                       bool ascii)
{
    parse_token_stream_t *tokens = scan_tokenize_owned(grammar, input);
    if (!tokens) return (core_string_t){0};
    while (scan_stream_next(tokens)) {}
    core_buffer_t out = {0};
    size_t copied = 0;
    int32_t count = scan_stream_token_count(tokens);
    for (int32_t i = 0; i < count; i++) {
        parse_token_info_t *token = scan_stream_get(tokens, i);
        if (!token || !core_option_is_set(parse_token_postfix_modifier(token))
            || !core_string_eq(
                   core_option_get(parse_token_postfix_modifier(token)),
                   CORE_STRING_STATIC("r"))
            || !core_string_eq(parse_terminal_name(grammar,
                   parse_token_tid(token)), CORE_STRING_STATIC("STRING_LIT"))
            || parse_token_start_byte(token) < copied
            || parse_token_end_byte(token) > input.u8_bytes) continue;
        size_t start = parse_token_start_byte(token) + 1;
        size_t end = parse_token_end_byte(token);
        while (end > start && input.data[end - 1] != '"') end--;
        if (end == start) continue;
        end--;
        core_buffer_append(&out, input.data + copied, start - copied);
        format_rich_body(&out, input.data, start, end, ascii);
        copied = end;
    }
    core_buffer_append(&out, input.data + copied, input.u8_bytes - copied);
    scan_token_stream_free(tokens);
    core_buffer_ensure(&out, 1);
    return core_buffer_string(&out);
}

static bool
n00bb_format_units(parse_grammar_t *grammar, core_work_queue_t *queue,
                   locate_requests_t *requests, parse_cmdr_result_t *args)
{
    bool check = parse_cmdr_flag_present(args, CORE_STRING_STATIC("--check"));
    bool normalize = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--normalize"));
    bool ascii_tags = n00bb_setting_bool(N00BB_KEY_FMT_ASCII_TAGS);
    core_string_t output = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    if (output.data && (check || parse_cmdr_arg_count(args) != 1)) {
        n00bb_report(requests->diagnostics, nullptr,
            N00B_DIAG_FMT_OUTPUT_ARGUMENTS, (core_diag_location_t){},
            check ? N00B_TEXT_FMT_OUTPUT_WITH_CHECK
                  : N00B_TEXT_FMT_OUTPUT_ONE_INPUT, N00BB_NO_ARGS);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < requests->ordered.len; i++) {
        locate_request_t *request = &requests->ordered.data[i];
        if (!request->root_input) continue;
        core_translation_unit_t *unit = core_work_job_result(queue,
            request->job);
        n00bb_tu_state_t *state = unit ? tu_state(unit) : nullptr;
        parse_tree_t *tree = state && state->lex && state->lex->parser
            ? parse_pwz_get_tree(state->lex->parser) : nullptr;
        if (!unit || !tree) {
            ok = false;
            continue;
        }
        core_string_t path = core_source_file_path(&unit->source);
        core_string_t source = core_string_borrow(
            core_source_file_contents(&unit->source).data,
            core_source_file_size(&unit->source));
        core_string_t normalized = normalize
            ? n00bb_format_inferred_types(grammar, state, source)
            : (core_string_t){0};
        parse_token_stream_t *normalized_tokens = nullptr;
        parse_pwz_parser_t *normalized_parser = normalized.data
            ? n00bb_format_parse(grammar, normalized, &normalized_tokens)
            : nullptr;
        if (normalized.data && !normalized_parser) {
            n00bb_internal_error(requests->diagnostics, unit,
                (core_diag_location_t){}, "formatting",
                "the normalized form of `%.*s` does not parse",
                (int)path.u8_bytes, path.data);
            core_free(normalized.data);
            ok = false;
            continue;
        }
        core_string_t print_source = normalized.data ? normalized : source;
        if (normalized_parser) tree = parse_pwz_get_tree(normalized_parser);
        parse_print_opts_t options = {
            .source = print_source,
            .width = 88,
            .indent = 4,
        };
        core_string_t printed = parse_tree_print(grammar, tree, &options);
        core_string_t restored = printed.data
            ? n00bb_format_restore_breaks(grammar, print_source, printed)
            : (core_string_t){0};
        parse_pwz_free(normalized_parser);
        scan_token_stream_free(normalized_tokens);
        core_free(normalized.data);
        core_free(printed.data);
        core_string_t aligned = restored.data ? n00bb_format_align(restored)
                                              : (core_string_t){0};
        core_free(restored.data);
        core_string_t signatures = aligned.data
            ? n00bb_format_signatures(grammar, aligned) : (core_string_t){0};
        core_free(aligned.data);
        core_string_t channels = signatures.data
            ? n00bb_format_channels(grammar, signatures) : (core_string_t){0};
        core_free(signatures.data);
        core_string_t formatted = channels.data
            ? n00bb_format_rich_tags(grammar, channels, ascii_tags)
            : (core_string_t){0};
        core_free(channels.data);
        if (!formatted.data || !n00bb_format_parses(grammar, formatted)) {
            n00bb_internal_error(requests->diagnostics, unit,
                (core_diag_location_t){}, "formatting",
                "the formatted form of `%.*s` does not parse",
                (int)path.u8_bytes, path.data);
            core_free(formatted.data);
            ok = false;
            continue;
        }
        bool changed = !core_string_eq(formatted, source);
        if (changed || output.data) {
            if (check) {
                printf("%.*s\n", (int)path.u8_bytes, path.data);
                ok = false;
            }
            else {
                core_string_t target = output.data ? output : path;
                if (!n00bb_format_write(target, formatted)) {
                    n00bb_report(requests->diagnostics, nullptr,
                        N00B_DIAG_FMT_WRITE_FAILED, (core_diag_location_t){},
                        N00B_TEXT_CANNOT_WRITE,
                        N00BB_ARGS(ARG_TEXT(target),
                                   ARG_ERRNO(errno ? errno : EIO)));
                    ok = false;
                }
            }
        }
        core_free(formatted.data);
    }
    return ok;
}

extern char **environ;

static bool
build_time_failure(core_diagnostics_t *diagnostics, n00bb_text_t what)
{
    int error = errno ? errno : EIO;
    n00bb_report(diagnostics, nullptr, N00B_DIAG_BUILD_TIME_RUN_FAILED,
        (core_diag_location_t){}, N00B_TEXT_BUILD_TIME_FAILURE,
        N00BB_ARGS(ARG_PHRASE_ID(what), ARG_ERRNO(error)));
    return false;
}

static bool
build_time_unreported(core_diagnostics_t *diagnostics)
{
    n00bb_internal_error(diagnostics, nullptr, (core_diag_location_t){},
                         "compiling the build-time program",
                         "the build-time compile failed without a diagnostic");
    return false;
}

static bool
build_time_run_status(core_diagnostics_t *diagnostics,
                      core_process_status_t status, bool started)
{
    if (!started)
        return build_time_failure(diagnostics,
                                  N00B_TEXT_PHRASE_COULD_NOT_START_THE_BUILD);
    if (!status.exited)
        n00bb_report(diagnostics, nullptr, N00B_DIAG_BUILD_TIME_RUN_FAILED,
            (core_diag_location_t){}, N00B_TEXT_THE_BUILD_TIME_PROGRAM_WAS,
            N00BB_ARGS(ARG_NUMBER(status.code),
                       ARG_NATIVE(core_process_signal_name(status.code))));
    else
        n00bb_report(diagnostics, nullptr, N00B_DIAG_BUILD_TIME_RUN_FAILED,
            (core_diag_location_t){}, N00B_TEXT_THE_BUILD_TIME_PROGRAM_EXITED,
            N00BB_ARGS(ARG_NUMBER(status.code)));
    n00bb_report_note(diagnostics, N00B_TEXT_THE_PROGRAM_IS_RUN_WHILE,
        N00BB_NO_ARGS);
    return false;
}

static bool
succeeded(core_process_t child)
{
    core_process_status_t status = {0};
    return core_process_wait(child, &status) && status.exited
        && status.code == 0;
}

static int build_time_capture = -1;
static bool build_time_capture_full;

enum { FORKED_BUILD_TIME_FALLBACK = 75 };

static bool forked_build_time;

static void
exec_compiler(core_string_t compiler, char **argv)
{
    if (forked_build_time) {
        fflush(stdout);
        fflush(stderr);
        _exit(FORKED_BUILD_TIME_FALLBACK);
    }
    char *native = core_string_to_cstr(compiler);
    if (!native) return;
    n00bb_handoff_t *handoff = n00bb_handoff();
    handoff->capture_fd = build_time_capture;
    handoff->capture_full = build_time_capture_full;
    n00bb_handoff_pass();
    fflush(stdout);
    fflush(stderr);
    core_process_redirect_t capture = {
        build_time_capture, build_time_capture,
    };
    char *invoked = argv[0];
    argv[0] = native;
    core_process_exec(&(core_process_spec_t){
        .arguments = argv,
        .redirects = &capture,
        .redirect_count = build_time_capture >= 0,
    });
    argv[0] = invoked;
    core_free(native);
}

static bool
reaches_speculative_module(const n00bb_cache_t *cache,
                           core_translation_unit_t *unit, core_dict_t *seen)
{
    if (!unit || core_dict_contains(seen, unit)) return false;
    core_dict_put(seen, unit, (void *)1);
    n00bb_tu_state_t *state = tu_state(unit);
    if (state && state->interface_module
        && n00bb_cache_module_speculative(cache, unit->module.identifier))
        return true;
    for (size_t i = 0; i < core_translation_unit_import_count(unit); i++)
        if (reaches_speculative_module(cache,
                core_translation_unit_import(unit, i), seen))
            return true;
    return false;
}

static core_translation_unit_t *
requested_unit_named(core_work_queue_t *queue,
                     const locate_requests_t *requests, core_string_t module)
{
    for (size_t r = 0; module.data && r < requests->ordered.len; r++) {
        core_translation_unit_t *unit = core_work_job_result(queue,
            requests->ordered.data[r].job);
        if (unit && core_string_eq(unit->module.identifier, module))
            return unit;
    }
    return nullptr;
}

static core_string_t
strict_retry_reason(const n00bb_cache_t *cache, core_work_queue_t *queue,
                    const locate_requests_t *requests,
                    const core_diagnostics_t *outer)
{
    bool any_error = core_diagnostics_failed(outer);
    bool speculative = false;
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t r = 0; !speculative && r < requests->ordered.len; r++) {
        core_translation_unit_t *unit = core_work_job_result(queue,
            requests->ordered.data[r].job);
        if (!unit || !core_diagnostics_failed(
                core_translation_unit_diagnostics(unit)))
            continue;
        any_error = true;
        speculative = reaches_speculative_module(cache, unit, &seen);
    }
    for (size_t i = 0; !speculative && i < core_diagnostics_count(outer); i++) {
        const core_diagnostic_t *item = core_diagnostics_at(outer, i);
        if (item->severity == CORE_DIAG_ERROR
            || item->severity == CORE_DIAG_FATAL)
            speculative = reaches_speculative_module(cache,
                requested_unit_named(queue, requests, item->module), &seen);
    }
    core_dict_free(&seen);
    if (speculative)
        return CORE_STRING_STATIC(
            "an error depends on a module reused from an optimistic cache entry");
    if (!any_error)
        return CORE_STRING_STATIC("the optimistic build failed without a diagnostic");
    return (core_string_t){0};
}

static void
restart_with_strict_cache(char **argv, core_string_t argv0)
{
    core_string_t compiler = core_os_executable_path(argv0);
    if (!compiler.data) return;
    n00bb_handoff()->cache_strict = true;
    exec_compiler(compiler, argv);
    core_free(compiler.data);
}

static void
restart_with_build_time(char **argv, core_string_t argv0, bool literal)
{
    core_string_t compiler = core_os_executable_path(argv0);
    if (!compiler.data) return;
    n00bb_handoff()->phase_order = literal ? N00BB_PHASE_ORDER_LITERAL_FIRST
                                           : N00BB_PHASE_ORDER_BUILD_TIME_FIRST;
    build_time_capture = -1;
    exec_compiler(compiler, argv);
    core_free(compiler.data);
}

static void
restart_excluding_cached(char **argv, core_string_t argv0,
                         const core_buffer_t *stale)
{
    n00bb_handoff_t *handoff = n00bb_handoff();
    core_buffer_t excluded = {0};
    core_buffer_append_string(&excluded, handoff->cache_exclude);
    if (handoff->cache_exclude.u8_bytes) core_buffer_putc(&excluded, ',');
    core_buffer_append(&excluded, stale->data, stale->byte_len);
    n00bb_handoff_set(&handoff->cache_exclude, core_buffer_string(&excluded));
    core_buffer_release(&excluded);
    core_string_t compiler = core_os_executable_path(argv0);
    if (!compiler.data) return;
    exec_compiler(compiler, argv);
    core_free(compiler.data);
}

static bool
diagnostic_is_error(const core_diagnostic_t *diagnostic)
{
    return diagnostic->severity == CORE_DIAG_ERROR
        || diagnostic->severity == CORE_DIAG_FATAL
        || diagnostic->severity == CORE_DIAG_INTERNAL;
}


static void
put_handoff_u32(core_buffer_t *out, uint32_t value)
{
    core_buffer_append(out, (const char *)&value, sizeof(value));
}

static void
put_handoff_text(core_buffer_t *out, core_string_t text)
{
    put_handoff_u32(out, (uint32_t)text.u8_bytes);
    if (text.u8_bytes) core_buffer_append(out, text.data, text.u8_bytes);
}

static void
put_handoff_location(core_buffer_t *out, core_diag_location_t location)
{
    put_handoff_u32(out, location.line);
    put_handoff_u32(out, location.column);
    put_handoff_u32(out, location.end_line);
    put_handoff_u32(out, location.end_column);
    put_handoff_u32(out, location.has_location);
}

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t at;
    bool failed;
} handoff_reader_t;

static uint32_t
read_handoff_u32(handoff_reader_t *r)
{
    uint32_t value = 0;
    if (r->failed || r->length - r->at < sizeof(value)) {
        r->failed = true;
        return 0;
    }
    memcpy(&value, r->data + r->at, sizeof(value));
    r->at += sizeof(value);
    return value;
}

static core_string_t
read_handoff_text(handoff_reader_t *r)
{
    uint32_t length = read_handoff_u32(r);
    if (r->failed || r->length - r->at < length) {
        r->failed = true;
        return (core_string_t){0};
    }
    core_string_t text = core_string_borrow((const char *)r->data + r->at,
                                            length);
    r->at += length;
    return text;
}

static core_diag_location_t
read_handoff_location(handoff_reader_t *r)
{
    core_diag_location_t location = {0};
    location.line = read_handoff_u32(r);
    location.column = read_handoff_u32(r);
    location.end_line = read_handoff_u32(r);
    location.end_column = read_handoff_u32(r);
    location.has_location = read_handoff_u32(r) != 0;
    return location;
}

static bool
write_check_handoff(const core_diagnostics_t *errors)
{
    core_buffer_t out = {0};
    put_handoff_u32(&out, (uint32_t)core_diagnostics_count(errors));
    for (size_t i = 0; i < core_diagnostics_count(errors); i++) {
        const core_diagnostic_t *item = core_diagnostics_at(errors, i);
        put_handoff_u32(&out, (uint32_t)item->severity);
        put_handoff_text(&out, item->code);
        put_handoff_text(&out, item->message);
        put_handoff_text(&out, item->label);
        put_handoff_text(&out, item->module);
        put_handoff_text(&out, item->path);
        put_handoff_location(&out, item->location);
        put_handoff_u32(&out, (uint32_t)item->parts.len);
        for (size_t p = 0; p < item->parts.len; p++) {
            const core_diag_part_t *part = &item->parts.data[p];
            put_handoff_u32(&out, (uint32_t)part->kind);
            put_handoff_text(&out, part->text);
            put_handoff_text(&out, part->path);
            put_handoff_location(&out, part->location);
        }
    }
    core_string_t root = core_os_temp_root();
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, root);
    core_buffer_puts(&stem, "/n00bb-check-");
    core_free(root.data);
    core_string_t made = {};
    int fd = core_os_make_unique_file(core_buffer_string(&stem), &made);
    core_buffer_release(&stem);
    bool ok = fd >= 0 && core_os_write_all(fd, out.data, out.byte_len);
    if (fd >= 0) core_os_close(fd);
    if (ok) n00bb_handoff_set(&n00bb_handoff()->check_diagnostics, made);
    if (!ok && fd >= 0) core_os_remove_file(made);
    core_free(made.data);
    core_buffer_release(&out);
    return ok;
}

static bool
merge_check_handoff(core_diagnostics_t *into)
{
    n00bb_handoff_t *handoff = n00bb_handoff();
    core_string_t file = handoff->check_diagnostics;
    if (!file.u8_bytes) return false;
    handoff->check_diagnostics = (core_string_t){};
    n00bb_handoff_pass();
    char *bytes = nullptr;
    size_t length = 0;
    if (!core_os_read_file(file, &bytes, &length)) length = 0;
    core_os_remove_file(file);
    core_free(file.data);
    handoff_reader_t r = {.data = (const uint8_t *)bytes, .length = length};
    uint32_t count = read_handoff_u32(&r);
    bool merged = false;
    for (uint32_t i = 0; !r.failed && i < count; i++) {
        core_diagnostic_t item = {
            .severity = (core_diag_severity_t)read_handoff_u32(&r),
            .code = read_handoff_text(&r),
            .message = read_handoff_text(&r),
            .label = read_handoff_text(&r),
            .module = read_handoff_text(&r),
            .path = read_handoff_text(&r),
        };
        item.location = read_handoff_location(&r);
        uint32_t parts = read_handoff_u32(&r);
        item.parts = core_list_new(core_diag_part_t);
        for (uint32_t p = 0; !r.failed && p < parts; p++) {
            core_diag_part_t part = {
                .kind = (core_diag_part_kind_t)read_handoff_u32(&r),
                .text = read_handoff_text(&r),
                .path = read_handoff_text(&r),
            };
            part.location = read_handoff_location(&r);
            core_list_push(item.parts, part);
        }
        if (!r.failed && !already_reported(into, &item)) {
            core_diagnostics_append(into, &item);
            merged = merged || diagnostic_is_error(&item);
        }
        core_list_free(item.parts);
    }
    core_free(bytes);
    return merged;
}

static bool
finish_check_comptime_phase(char **argv, core_string_t argv0,
                            const core_diagnostics_t *diagnostics,
                            n00bb_diagnostic_filter_t *filter)
{
    core_diagnostics_t *errors = core_diagnostics_new();
    for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++)
        if (diagnostic_is_error(core_diagnostics_at(diagnostics, i)))
            core_diagnostics_append(errors,
                                    core_diagnostics_at(diagnostics, i));
    bool failed = core_diagnostics_count(errors) != 0;
    bool handed_off = failed && write_check_handoff(errors);
    if (failed && !handed_off) print_counted_diagnostics(errors, filter);
    core_diagnostics_free(errors);
    if (failed && !handed_off) return true;
    core_string_t compiler = core_os_executable_path(argv0);
    n00bb_handoff()->check_runtime_phase = true;
    if (compiler.data) exec_compiler(compiler, argv);
    core_free(compiler.data);
    return false;
}

static char **
check_arguments_for(int argc, char **argv, parse_cmdr_result_t *args,
                    int32_t input_count, int32_t kept)
{
    char **arguments = core_alloc_array(char *, (size_t)argc + 1);
    int count = 0;
    for (int j = 0; j < argc; j++) {
        core_string_t argument = core_string_from_cstr(argv[j]);
        bool dropped = false;
        for (int32_t i = 0; j > 0 && !dropped && i < input_count; i++)
            dropped = i != kept
                && core_string_eq(argument, parse_cmdr_arg_str(args, i));
        core_free(argument.data);
        if (!dropped) arguments[count++] = argv[j];
    }
    return arguments;
}

static int
check_each_input(int argc, char **argv, core_string_t argv0,
                 parse_cmdr_result_t *args, int32_t input_count)
{
    core_string_t compiler_path = core_os_executable_path(argv0);
    char *compiler = core_string_to_cstr(compiler_path);
    core_free(compiler_path.data);
    if (!compiler) return 1;
    int32_t limit = core_os_cpu_count();
    core_process_t *running = core_alloc_array(core_process_t,
                                               (size_t)input_count);
    int *outputs = core_alloc_array(int, (size_t)input_count);
    int32_t started = 0;
    size_t active = 0;
    int worst = 0;
    while (started < input_count || active) {
        while (started < input_count && (int32_t)active < limit) {
            int output = core_os_temp_fd();
            char **arguments = check_arguments_for(argc, argv, args,
                                                   input_count, started);
            arguments[0] = compiler;
            core_process_redirect_t redirects[2] = {{1, output}, {2, output}};
            core_process_spec_t spec = {
                .arguments = arguments, .redirects = redirects,
                .redirect_count = 2,
            };
            bool spawned = output >= 0
                && !core_process_start(&spec, &running[active]);
            core_free(arguments);
            outputs[started] = output;
            if (spawned) active++;
            else worst = 3;
            started++;
        }
        size_t done = 0;
        core_process_status_t status = {0};
        if (!active || !core_process_wait_any(running, active, &done, &status))
            break;
        running[done] = running[--active];
        int code = status.exited ? status.code : 3;
        if (code > worst) worst = code;
    }
    fflush(stdout);
    for (int32_t i = 0; i < input_count; i++) {
        int output = outputs[i];
        if (output < 0) continue;
        char buffer[8192];
        ssize_t got = 0;
        if (lseek(output, 0, SEEK_SET) == 0)
            while ((got = read(output, buffer, sizeof(buffer))) > 0)
                fwrite(buffer, 1, (size_t)got, stderr);
        close(output);
    }
    core_free(running);
    core_free(outputs);
    core_free(compiler);
    return worst;
}

static int
build_time_log(void)
{
    return core_os_temp_fd();
}

static void
replay_build_time_log(int log_fd, bool *reported)
{
    if (log_fd < 0 || lseek(log_fd, 0, SEEK_SET) != 0) return;
    core_buffer_t log = {0};
    char chunk[4096];
    ssize_t got;
    while ((got = read(log_fd, chunk, sizeof(chunk))) > 0)
        core_buffer_append(&log, chunk, (size_t)got);
    if (log.byte_len) fwrite(log.data, 1, log.byte_len, stderr);
    core_string_t text = core_buffer_string(&log);
    count_replayed_headers(text);
    *reported = core_string_find(text, CORE_STRING_STATIC("error:")) >= 0
        || core_string_find(text, CORE_STRING_STATIC("error N00B")) >= 0
        || core_string_find(text, CORE_STRING_STATIC("error IR-")) >= 0
        || core_string_find(text, CORE_STRING_STATIC("error LANTERN-")) >= 0
        || core_string_find(text, CORE_STRING_STATIC("internal compiler error"))
               >= 0;
    core_buffer_release(&log);
}

static char **
build_time_arguments(int argc, char **argv, char *compiler,
                     char *native_executable, bool literal, size_t *count_out)
{
    char **arguments = calloc((size_t)argc + 6, sizeof(*arguments));
    if (!arguments) return nullptr;
    size_t count = 0;
    arguments[count++] = compiler;
    arguments[count++] = "build";
    for (int i = 1; i < argc; i++) {
        core_string_t argument = core_string_from_cstr(argv[i]);
        bool output = core_string_eq(argument, CORE_STRING_STATIC("--output"))
            || core_string_eq(argument, CORE_STRING_STATIC("-o"))
            || core_string_eq(argument,
                              CORE_STRING_STATIC("--emit-interface"));
        bool dropped = (i == 1
                && (core_string_eq(argument, CORE_STRING_STATIC("build"))
                    || core_string_eq(argument, CORE_STRING_STATIC("lower"))
                    || core_string_eq(argument, CORE_STRING_STATIC("check"))
                    || core_string_eq(argument,
                                      CORE_STRING_STATIC("library"))
                    || core_string_eq(argument,
                                      CORE_STRING_STATIC("compile"))))
            || core_string_starts_with(argument, CORE_STRING_STATIC("--dump-"));
        core_free(argument.data);
        if (output) {
            i++;
            continue;
        }
        if (dropped) continue;
        arguments[count++] = argv[i];
    }
    arguments[count++] = "--build-time";
    if (literal) arguments[count++] = "--literal-stage";
    arguments[count++] = "--output";
    arguments[count++] = native_executable;
    *count_out = count;
    return arguments;
}

static bool
compile_build_time_program(int argc, char **argv, core_string_t argv0,
                           core_string_t executable, bool literal,
                           bool *reported)
{
    core_string_t compiler_path = core_os_executable_path(argv0);
    char *compiler = core_string_to_cstr(compiler_path);
    core_free(compiler_path.data);
    char *native_executable = core_string_to_cstr(executable);
    size_t count = 0;
    char **arguments = compiler && native_executable
        ? build_time_arguments(argc, argv, compiler, native_executable,
                               literal, &count)
        : nullptr;
    bool ok = arguments != nullptr;
    int log_fd = ok ? build_time_log() : -1;
    core_process_redirect_t redirects[2] = {{1, log_fd}, {2, log_fd}};
    core_process_spec_t spec = {
        .arguments = arguments, .redirects = redirects, .redirect_count = 2,
    };
    core_process_t child;
    ok = ok && log_fd >= 0 && !core_process_start(&spec, &child)
        && succeeded(child);
    *reported = false;
    if (!ok) replay_build_time_log(log_fd, reported);
    if (log_fd >= 0) close(log_fd);
    free(arguments);
    core_free(native_executable);
    core_free(compiler);
    return ok;
}

static bool
run_build_time_program(core_string_t executable, int capture_fd,
                       core_diagnostics_t *diagnostics)
{
    char *native = core_string_to_cstr(executable);
    if (!native) return build_time_run_status(diagnostics, (core_process_status_t){}, false);
    char *arguments[] = {native, nullptr};
    core_process_redirect_t redirects[1] = {{3, capture_fd}};
    core_process_spec_t spec = {
        .arguments = arguments, .redirects = redirects, .redirect_count = 1,
    };
    core_process_t child;
    int spawn_error = core_process_start(&spec, &child);
    core_free(native);
    if (spawn_error) {
        errno = spawn_error;
        return build_time_run_status(diagnostics, (core_process_status_t){},
                                     false);
    }
    core_process_status_t status = {0};
    if (!core_process_wait(child, &status))
        return build_time_failure(diagnostics,
                                  N00B_TEXT_PHRASE_COULD_NOT_WAIT_FOR_THE);
    if (status.exited && status.code == 0) return true;
    return build_time_run_status(diagnostics, status, true);
}

static bool
read_capture(int capture_fd, bool full)
{
    core_buffer_t capture = {0};
    char chunk[65536];
    bool ok = lseek(capture_fd, 0, SEEK_SET) == 0;
    ssize_t got;
    while (ok && (got = read(capture_fd, chunk, sizeof(chunk))) > 0)
        core_buffer_append(&capture, chunk, (size_t)got);
    if (ok) n00bb_set_baked_capture((const uint8_t *)capture.data,
                                    capture.byte_len, full);
    core_buffer_release(&capture);
    return ok;
}

static bool
run_build_time_stage(int argc, char **argv, core_string_t argv0, bool literal,
                     core_diagnostics_t *diagnostics, bool *reported)
{
    n00bb_handoff_t *handoff = n00bb_handoff();
    if (handoff->capture_fd >= 0) {
        build_time_capture = (int)handoff->capture_fd;
        build_time_capture_full = handoff->capture_full;
        handoff->capture_fd = -1;
        n00bb_handoff_pass();
        return read_capture(build_time_capture, build_time_capture_full)
            || build_time_failure(diagnostics,
                N00B_TEXT_PHRASE_COULD_NOT_READ_THE_VALUES);
    }
    core_string_t root = core_os_temp_root();
    core_buffer_t stem = {0};
    core_buffer_append_string(&stem, root);
    core_buffer_puts(&stem, "/n00bb-build-time-");
    core_free(root.data);
    core_string_t executable_path = {};
    int executable_fd = core_os_make_unique_file(core_buffer_string(&stem),
                                                 &executable_path);
    core_buffer_release(&stem);
    if (executable_fd < 0)
        return build_time_failure(diagnostics,
            N00B_TEXT_PHRASE_COULD_NOT_CREATE_A_TEMPORARY);
    close(executable_fd);
    char *executable = core_string_to_cstr(executable_path);
    int capture_fd = core_os_temp_fd();
    bool ok = capture_fd >= 0 && executable;
    if (!ok) build_time_failure(diagnostics,
        N00B_TEXT_PHRASE_COULD_NOT_CREATE_A_TEMPORARY_2);
    bool child_reported = false;
    if (ok && !compile_build_time_program(argc, argv, argv0,
            executable_path, literal, &child_reported)) {
        *reported = child_reported;
        ok = child_reported ? false : build_time_unreported(diagnostics);
    }
    struct stat program = {0};
    bool needed = ok && !stat(executable, &program) && program.st_size;
    if (ok && needed
        && !run_build_time_program(executable_path, capture_fd, diagnostics))
        ok = false;
    if (ok && !read_capture(capture_fd, !literal))
        ok = build_time_failure(diagnostics,
            N00B_TEXT_PHRASE_COULD_NOT_READ_THE_VALUES);
    if (ok) {
        build_time_capture = capture_fd;
        build_time_capture_full = !literal;
    }
    else if (capture_fd >= 0) close(capture_fd);
    n00bb_remove_executable(executable_path);
    core_free(executable);
    core_free(executable_path.data);
    return ok;
}

static bool
build_time_needs_own_front_end(const n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        if (!state || state->build_time_use.has_location
            || state->interface_module) return true;
    }
    return false;
}

static bool
load_build_time_capture(n00bb_link_analysis_t *analysis,
                        core_diagnostics_t *diagnostics)
{
    return n00bb_baked_graph_load(analysis)
        || build_time_failure(diagnostics,
            N00B_TEXT_PHRASE_COULD_NOT_READ_THE_VALUES);
}

static bool
spawn_build_time_stage(int argc, char **argv, core_string_t argv0,
                       n00bb_link_analysis_t *analysis,
                       core_diagnostics_t *diagnostics, bool *reported)
{
    return run_build_time_stage(argc, argv, argv0, false, diagnostics,
                                reported)
        && load_build_time_capture(analysis, diagnostics);
}

static bool
fork_build_time_stage(int argc, char **argv, core_string_t argv0,
                      parse_cmdr_t *cli, parse_cmdr_result_t **args,
                      core_link_unit_t *link,
                      n00bb_link_analysis_t *analysis,
                      core_diagnostics_t *diagnostics, bool *reported)
{
    *reported = false;
#ifdef _WIN32
    (void)cli;
    (void)args;
    (void)link;
    return spawn_build_time_stage(argc, argv, argv0, analysis, diagnostics,
                                  reported);
#else
    if (build_time_needs_own_front_end(analysis))
        return spawn_build_time_stage(argc, argv, argv0, analysis,
                                      diagnostics, reported);
    static const char executable_template[] = "/tmp/n00bb-build-time-XXXXXX";
    size_t executable_length = sizeof(executable_template) - 1;
    char *executable = core_strndup(executable_template, executable_length);
    int executable_fd = executable ? mkstemp(executable) : -1;
    if (executable_fd < 0) {
        core_free(executable);
        return build_time_failure(diagnostics,
            N00B_TEXT_PHRASE_COULD_NOT_CREATE_A_TEMPORARY);
    }
    close(executable_fd);
    char capture_path[] = "/tmp/n00bb-build-time-capture-XXXXXX";
    int capture_fd = mkstemp(capture_path);
    if (capture_fd >= 0) unlink(capture_path);
    int log_fd = build_time_log();
    core_string_t compiler_path = core_os_executable_path(argv0);
    char *compiler = core_string_to_cstr(compiler_path);
    core_free(compiler_path.data);
    size_t count = 0;
    char **arguments = compiler
        ? build_time_arguments(argc, argv, compiler, executable, false,
                               &count)
        : nullptr;
    pid_t pid = -1;
    if (capture_fd >= 0 && log_fd >= 0 && arguments) {
        fflush(stdout);
        fflush(stderr);
        pid = fork();
    }
    if (pid == 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
        close(capture_fd);
        forked_build_time = true;
        n00bb_set_build_time(true);
        n00bb_native_root_hide_hosted(true);
        parse_cmdr_result_t *build_args = parse_cmdr_parse(cli,
            (int)count - 1, (const char **)arguments + 1);
        if (!parse_cmdr_result_ok(build_args)
            || !core_link_unit_invalidate_stage(link,
                   N00BB_LINK_STAGE_CONTRIBUTIONS)
            || !core_link_unit_run_stage(link,
                   N00BB_LINK_STAGE_CONTRIBUTIONS)
            || core_link_unit_has_fatal_diagnostics(link))
            _exit(FORKED_BUILD_TIME_FALLBACK);
        *args = build_args;
        return true;
    }
    free(arguments);
    core_free(compiler);
    int status = 0;
    bool exited = pid > 0 && waitpid(pid, &status, 0) == pid
        && WIFEXITED(status);
    bool fallback = pid < 0
        || (exited && WEXITSTATUS(status) == FORKED_BUILD_TIME_FALLBACK);
    bool ok = !fallback && exited && WEXITSTATUS(status) == 0;
    if (!fallback && !ok) {
        replay_build_time_log(log_fd, reported);
        if (!*reported) build_time_unreported(diagnostics);
    }
    if (log_fd >= 0) close(log_fd);
    core_string_t executable_text = core_string_borrow(executable,
                                                       executable_length);
    struct stat program = {0};
    bool needed = ok && !stat(executable, &program) && program.st_size;
    if (ok && needed
        && !run_build_time_program(executable_text, capture_fd, diagnostics))
        ok = false;
    if (ok && needed && !read_capture(capture_fd, true))
        ok = build_time_failure(diagnostics,
            N00B_TEXT_PHRASE_COULD_NOT_READ_THE_VALUES);
    if (ok && needed) {
        build_time_capture = capture_fd;
        build_time_capture_full = true;
    }
    else if (capture_fd >= 0) close(capture_fd);
    n00bb_remove_executable(core_string_borrow(executable, executable_length));
    core_free(executable);
    if (fallback)
        return spawn_build_time_stage(argc, argv, argv0, analysis,
                                      diagnostics, reported);
    return ok && load_build_time_capture(analysis, diagnostics);
#endif
}

static bool
locate_cached_module(void *context, core_string_t module, core_string_t *path)
{
    core_module_spec_t spec = {0};
    core_source_file_t source = {0};
    bool found = n00bb_locate_operand(context, module, &spec, &source)
        == CORE_MODULE_RESOLVED;
    if (found) {
        *path = core_string_dup(core_source_file_path(&source));
        core_source_file_release(&source);
    }
    core_module_spec_release(&spec);
    return found;
}

static n00bb_cache_t *
open_object_cache(n00bb_resolver_t *resolver, parse_cmdr_result_t *args,
                  ir_ctx_t *ir_context)
{
    core_target_desc_t target;
    core_string_t input = parse_cmdr_arg_str(args, 0);
    core_string_t operand = input.data ? input : CORE_STRING_STATIC("");
    core_module_spec_t module = {0};
    core_source_file_t source = {0};
    bool located = resolver && n00bb_output_target_profile(&target)
        && n00bb_locate_operand(resolver, operand, &module, &source)
            == CORE_MODULE_RESOLVED;
    n00bb_cache_t *cache = nullptr;
    if (located) {
        core_string_t root = core_source_file_path(&source);
        cache = n00bb_cache_open(root, args, target.triple, ir_context,
                                 locate_cached_module, resolver);
        core_source_file_release(&source);
        core_module_spec_release(&module);
    }
    return cache;
}

static size_t
report_specialization_gaps(n00bb_link_analysis_t *analysis)
{
    size_t gaps = 0;
    for (size_t i = 0; i < analysis->reachable_functions.len; i++) {
        ir_func_t *function = analysis->reachable_functions.data[i];
        if (!function || !function->n_blocks || !function->owner) continue;
        th_ctx *types = ir_module_types(function->owner);
        if (th_is_closed(types, function->type)) continue;
        core_string_t module = ir_module_name(function->owner);
        core_string_t printed = th_print(types, function->type);
        fprintf(stderr, "specialization gap: %.*s.%.*s: %.*s\n",
                TEXT_ARGS(module), TEXT_ARGS(function->name),
                TEXT_ARGS(printed));
        gaps++;
    }
    return gaps;
}

static bool
verify_analyses(n00bb_link_analysis_t *analysis,
                core_diagnostics_t *diagnostics)
{
    bool ok = true;
    for (size_t i = 0; i < analysis->emitted_functions.len; i++) {
        ir_func_t *function = analysis->emitted_functions.data[i];
        if (!function || !function->n_blocks) continue;
        ir_memssa_t *memssa = ir_func_memssa(function);
        if (!memssa || ir_memssa_verify(memssa)) continue;
        n00bb_internal_error(diagnostics, nullptr, (core_diag_location_t){},
            "verifying analyses", "memory SSA for `%.*s` failed verification",
            TEXT_ARGS(function->name));
        ok = false;
    }
    return ok;
}

static void
uncheck_wide_narrowing(n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->emitted_functions.len; i++) {
        ir_func_t *function = analysis->emitted_functions.data[i];
        ir_module_t *module = function ? function->owner : nullptr;
        for (uint32_t b = 0; module && b < function->n_blocks; b++)
            for (uint32_t n = 0; n < function->blocks[b]->n_instrs; n++) {
                ir_instr_t *cast = function->blocks[b]->instrs[n];
                ir_value_t *source = cast->op == IR_OP_CAST
                    && cast->n_operands == 1 && cast->dest
                    ? ir_operand_value(&cast->operands[0]) : nullptr;
                if (!source || ir_machine_of(module, source->type).width != 128
                    || ir_machine_of(module, cast->dest->type).width >= 128)
                    continue;
                uint32_t kept = 0;
                for (uint32_t m = 0; m < cast->n_mods; m++)
                    if (!core_string_eq(cast->mods[m],
                                        CORE_STRING_STATIC("trap")))
                        cast->mods[kept++] = cast->mods[m];
                cast->n_mods = kept;
            }
    }
}

static bool
specialize_emitted(n00bb_link_analysis_t *analysis,
                   const n00bb_specialization_seed_t *seeds,
                   size_t seed_count, n00bb_specialization_failure_t *failure,
                   core_diagnostics_t *diagnostics)
{
    core_list_t(n00bb_ir_func_ptr_t) callers =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; i < analysis->emitted_functions.len; i++)
        core_list_push(callers, analysis->emitted_functions.data[i]);
    core_dict_t visited;
    core_dict_init(&visited, nullptr, nullptr);
    bool ok = true;
    while (ok && callers.len) {
        ok = n00bb_specialize_calls(analysis->ir_context, &callers,
                seeds, seed_count, &analysis->specializations,
                &analysis->specialization_requests, failure)
            && build_reachable_functions(analysis, &analysis->units,
                                         diagnostics);
        seed_count = 0;
        for (size_t i = 0; i < callers.len; i++)
            core_dict_put(&visited, callers.data[i], callers.data[i]);
        callers.len = 0;
        for (size_t i = 0; ok && i < analysis->emitted_functions.len; i++) {
            ir_func_t *function = analysis->emitted_functions.data[i];
            if (!core_dict_contains(&visited, function))
                core_list_push(callers, function);
        }
    }
    core_dict_free(&visited);
    core_list_free(callers);
    return ok;
}

static void
optimize_range_checks(n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->emitted_functions.len; i++) {
        ir_func_t *function = analysis->emitted_functions.data[i];
        if (!function || !function->n_blocks) continue;
        size_t scope = ir_analysis_scope_begin();
        (void)ir_abcd_fold(function);
        (void)ir_abcd_hoist(function);
        ir_analysis_scope_end(scope);
    }
}

static bool
specialize_calls(n00bb_link_analysis_t *analysis,
                 core_diagnostics_t *diagnostics)
{
    n00bb_specialization_failure_t failure = {0};
    core_list_t(n00bb_specialization_seed_t) seeds =
        core_list_new(n00bb_specialization_seed_t);
    n00bb_cache_seeds(analysis->cache, ir_ctx_types(analysis->ir_context),
                      n00bb_source_unit_module, analysis, &seeds);
    bool ok = specialize_emitted(analysis, seeds.data, seeds.len, &failure,
                                 diagnostics)
        && n00bb_rewrite_box_operations(analysis)
        && specialize_emitted(analysis, nullptr, 0, &failure, diagnostics);
    core_list_free(seeds);
    if (ok) ok = publish_declared_aggregate_layouts(analysis)
        && publish_interned_variant_layouts(analysis);
    core_translation_unit_t *site = nullptr;
    for (size_t i = 0; !ok && failure.located && !site
         && i < analysis->units.len; i++)
        if (core_string_eq(analysis->units.data[i]->module.identifier,
                           failure.module))
            site = analysis->units.data[i];
    if (!ok && failure.located)
        n00bb_report(site ? core_translation_unit_diagnostics(site)
                          : diagnostics, site,
            N00B_DIAG_GENERIC_SPECIALIZATION_FAILED, (core_diag_location_t){
                .line = failure.span.start_line,
                .column = failure.span.start_col,
                .end_line = failure.span.end_line,
                .end_column = failure.span.end_col,
                .has_location = site && failure.span.start_line != 0,
            }, N00B_TEXT_GROWING_INSTANTIATION,
            N00BB_ARGS(ARG_TEXT(failure.function), ARG_TEXT(failure.before),
                       ARG_TEXT(failure.after)));
    else if (!ok && failure.message.byte_len)
        n00bb_internal_error(diagnostics, nullptr, (core_diag_location_t){},
                             "specializing generic calls", "%.*s",
                             (int)failure.message.byte_len,
                             failure.message.data);
    else if (!ok && !core_diagnostics_failed(diagnostics))
        n00bb_internal_error(diagnostics, nullptr, (core_diag_location_t){},
                             "specializing generic calls",
                             "specialization failed without a diagnostic");
    core_buffer_release(&failure.message);
    return ok;
}

static void
print_usage_diagnostic(n00bb_diag_t id, n00bb_text_t text, n00bb_args_t args)
{
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    n00bb_report(diagnostics, nullptr, id, (core_diag_location_t){}, text,
                 args);
    n00bb_print_diagnostics(diagnostics);
    core_diagnostics_free(diagnostics);
}

int
n00bb_compiler_main(int argc, char **argv, core_string_t argv0)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    n00bb_invocation_t invocation;
    n00bb_invocation_open(&invocation, argc, argv, argv0);
    n00bb_config_load(&invocation);
    n00bb_llvm_set_profiling(
        n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE));
    n00bb_llvm_set_opt_level(
        (int)n00bb_setting_int(N00BB_KEY_DEV_OPT_LEVEL));
    n00bb_install_crash_handler();
    n00bb_toolchain_init(argv0);
    bool handled = false;
    int selected = n00bb_toolchain_select(&invocation, &handled);
    if (!handled) {
        n00bb_register_builtin_types();
        selected = n00bb_cli_preflight(&invocation, &handled);
    }
    if (!handled && !parse_cmdr_result_ok(invocation.parsed)) {
        n00bb_cli_report_errors(invocation.parsed);
        selected = 2;
        handled = true;
    }
    if (handled) {
        n00bb_invocation_close(&invocation);
        return selected;
    }
    parse_cmdr_t *cli = invocation.cli;
    parse_cmdr_result_t *args = invocation.parsed;

    closed_world_build = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--closed-world"));
    function_summaries_build = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--function-summaries"));
    bool requested_native = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--native"));
    bool requested_generic = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--generic"));
    if (requested_native && requested_generic) {
        print_usage_diagnostic(N00B_DIAG_CONFLICTING_CPU_OPTIONS,
            N00B_TEXT_NATIVE_AND_GENERIC_CANNOT_BE,
            N00BB_NO_ARGS);
        n00bb_invocation_close(&invocation);
        return 2;
    }
    core_target_desc_t output_target;
    bool native_cpu = n00bb_setting_bool(N00BB_KEY_BUILD_NATIVE)
        && !n00bb_cross_target(&output_target);

    core_string_t command = parse_cmdr_result_command(args);

    int32_t input_count = parse_cmdr_arg_count(args);
    if (core_string_eq(command, CORE_STRING_STATIC("check"))
        && input_count > 1) {
        int status = check_each_input(argc, argv, argv0, args, input_count);
        n00bb_invocation_close(&invocation);
        return status;
    }
    core_work_queue_t *queue = core_work_queue_new();
    core_diagnostics_t *cli_diagnostics = core_diagnostics_new();
    locate_requests_t requests = {
        .ordered = core_list_new(locate_request_t),
        .diagnostics = cli_diagnostics,
    };
    core_dict_init(&requests.by_job, nullptr, nullptr);
    bool scheduled = true;
    for (int32_t i = 0; i < input_count; i++) {
        core_string_t input = parse_cmdr_arg_str(args, i);
        scheduled = add_locate_request(queue, &requests,
            input.data ? input : CORE_STRING_STATIC("")) && scheduled;
    }

    size_t reported = core_diagnostics_count(cli_diagnostics);
    n00bb_resolver_t *resolver = n00bb_resolver_new(argv0, cli_diagnostics);
    if (!resolver) {
        n00bb_report(cli_diagnostics, nullptr,
            N00B_DIAG_WORKING_DIRECTORY_UNAVAILABLE, (core_diag_location_t){},
            N00B_TEXT_CANNOT_DETERMINE_THE_CURRENT_DIRECTORY,
            N00BB_ARGS(ARG_ERRNO(errno)));
        scheduled = false;
    }
    else if (core_diagnostics_count(cli_diagnostics) > reported)
        scheduled = false;
    bool lex_command = core_string_eq(command, CORE_STRING_STATIC("lex"));
    bool parse_command = core_string_eq(command, CORE_STRING_STATIC("parse"));
    bool fmt_command = core_string_eq(command, CORE_STRING_STATIC("fmt"));
    bool normalize_fmt = fmt_command && parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--normalize"));
    bool locate_only = core_string_eq(command, CORE_STRING_STATIC("locate-tu"));
    bool show_ffi = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--show-ffi"));
    bool preprocess_command = show_ffi
        || core_string_eq(command, CORE_STRING_STATIC("preprocess"));
    bool check_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("check"));
    bool lower_command = check_command || (!show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("lower")));
    collect_located_diagnostics = check_command;
    n00bb_cache_set_check_only(check_command);
    n00bb_set_baked_values_opaque(check_command);
    bool build_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("build"));
    bool library_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("library"));
    bool compile_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("compile"));
    bool emit_interface = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--emit-interface"));
    core_string_t interface_output = emit_interface
        ? parse_cmdr_flag_str(args,
            CORE_STRING_STATIC("--emit-interface")) : (core_string_t){0};
    bool options_valid = true;
    bool no_comptime = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--no-comptime"));
    bool check_comptime = check_command && !no_comptime
        && !n00bb_handoff()->check_runtime_phase;
    bool build_time = check_comptime || parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--build-time"));
    n00bb_set_build_time(build_time);
    n00bb_set_literal_stage(parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--literal-stage")));
    n00bb_native_root_hide_hosted(build_time);
    if (build_command && !build_time && input_count > 1) {
        core_list_t(core_string_t) extra = core_list_new(core_string_t);
        for (int32_t i = 1; i < input_count; i++)
            core_list_push(extra, parse_cmdr_arg_str(args, i));
        n00bb_report(cli_diagnostics, nullptr, N00B_DIAG_BUILD_EXTRA_OPERANDS,
            (core_diag_location_t){}, N00B_TEXT_BUILD_EXTRA_OPERANDS,
            N00BB_ARGS(ARG_NAMES(extra.data, extra.len),
                       ARG_TEXT(parse_cmdr_arg_str(args, 0))));
        n00bb_report_help(cli_diagnostics,
            N00B_TEXT_BUILD_EACH_PROGRAM_SEPARATELY, N00BB_NO_ARGS);
        core_list_free(extra);
        options_valid = false;
    }
    bool build_time_reported = false;
    n00bb_phase_order_t phase_order = n00bb_handoff()->phase_order;
    bool full_first = phase_order == N00BB_PHASE_ORDER_BUILD_TIME_FIRST;
    bool literal_first = phase_order == N00BB_PHASE_ORDER_LITERAL_FIRST;
    bool inherited_capture = n00bb_handoff()->capture_fd >= 0;
    bool deferred_build_time = build_command && !build_time && !no_comptime
        && !inherited_capture && !full_first && !literal_first;
    bool upfront_build_time = (build_command || lower_command
            || library_command || compile_command)
        && (inherited_capture || full_first || literal_first);
    if (options_valid && upfront_build_time && !build_time
        && !run_build_time_stage(argc, argv, argv0, literal_first,
                                 cli_diagnostics, &build_time_reported))
        options_valid = false;
    int32_t native_root_count = parse_cmdr_flag_value_count(args,
        CORE_STRING_STATIC("--native-root"));
    core_string_t *native_roots = native_root_count
        ? core_alloc_array(core_string_t, (size_t)native_root_count) : nullptr;
    int32_t named_root_count = 0;
    for (int32_t i = 0; i < native_root_count; i++) {
        core_string_t root = parse_cmdr_flag_str_at(args,
            CORE_STRING_STATIC("--native-root"), i);
        int64_t alias = root.data ? core_string_find_byte(root, '=') : -1;
        if (alias < 0) {
            native_roots[named_root_count++] = root;
            continue;
        }
        core_string_t qualified = core_string_slice(root, 0, (size_t)alias);
        core_string_t symbol = core_string_slice(root, (size_t)alias + 1,
            root.u8_bytes - (size_t)alias - 1);
        if (!n00bb_native_root_register(qualified, symbol)) {
            n00bb_report(cli_diagnostics, nullptr,
                N00B_DIAG_INVALID_NATIVE_ROOT, (core_diag_location_t){},
                N00B_TEXT_NATIVE_ROOT_CANNOT_BE_REGISTERED,
                N00BB_ARGS(ARG_TEXT(root)));
            n00bb_report_help(cli_diagnostics,
                N00B_TEXT_WRITE_NATIVE_ROOT_MODULE_FUNCTION_2,
                N00BB_NO_ARGS);
            options_valid = false;
        }
    }
    native_root_count = named_root_count;
    if (emit_interface && !library_command) {
        n00bb_report(cli_diagnostics, nullptr,
            N00B_DIAG_OPTION_REQUIRES_LIBRARY, (core_diag_location_t){},
            N00B_TEXT_EMIT_INTERFACE_ONLY_WORKS_WITH,
            N00BB_NO_ARGS);
        options_valid = false;
    }
    int32_t interface_count = parse_cmdr_flag_value_count(args,
        CORE_STRING_STATIC("--link-interface"));
    const th_env **linked_interfaces = interface_count
        ? core_alloc_array(const th_env *, (size_t)interface_count) : nullptr;
    for (int32_t i = 0; i < interface_count; i++) {
        core_string_t argument = parse_cmdr_flag_str_at(args,
            CORE_STRING_STATIC("--link-interface"), i);
        core_string_t path = argument.data ? argument
                                           : CORE_STRING_STATIC("");
        core_object_archive_error_t archive_error =
            CORE_OBJECT_ARCHIVE_OK;
        linked_interfaces[i] = th_env_load(path, &archive_error);
        if (!linked_interfaces[i]) {
            core_string_t reason =
                core_object_archive_error_string(archive_error);
            n00bb_report(cli_diagnostics, nullptr,
                N00B_DIAG_INTERFACE_UNREADABLE, (core_diag_location_t){},
                N00B_TEXT_CANNOT_LOAD_SEMANTIC_INTERFACE,
                N00BB_ARGS(ARG_TEXT(path), ARG_TEXT(reason)));
            options_valid = false;
        }
    }
    bool show_tokens = lex_command || parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-tokens"));
    bool show_tree = parse_command || parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-tree"));
    bool show_preprocessed = (preprocess_command && !show_ffi)
        || parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-preprocessed"));
    bool show_symbols = parse_cmdr_flag_present(
        args, CORE_STRING_STATIC("--dump-symbols"));
    int64_t requested_jobs = n00bb_setting_int(N00BB_KEY_BUILD_JOBS);
    if (requested_jobs < 0 || requested_jobs > 128) {
        n00bb_report(cli_diagnostics, nullptr, N00B_DIAG_JOBS_OUT_OF_RANGE,
            (core_diag_location_t){}, N00B_TEXT_JOBS_IS_OUTSIDE_0_TO,
            N00BB_ARGS(ARG_NUMBER((long long)requested_jobs)));
        scheduled = false;
    }
    n00bb_setting_id_t defines_key = N00BB_KEY_BUILD_DEFINES;
    size_t define_count = n00bb_setting_count(defines_key);
    core_string_t *definitions = define_count
        ? core_alloc_array(core_string_t, define_count) : nullptr;
    for (size_t i = 0; i < define_count; i++)
        definitions[i] = n00bb_setting_at(defines_key, i);
    n00bb_setting_id_t preferences_key = N00BB_KEY_SYNTAX_PREFERENCES;
    size_t syntax_preference_count = n00bb_setting_count(preferences_key);
    core_string_t *syntax_preferences = syntax_preference_count
        ? core_alloc_array(core_string_t, syntax_preference_count)
        : nullptr;
    for (size_t i = 0; i < syntax_preference_count; i++)
        syntax_preferences[i] = n00bb_setting_at(preferences_key, i);
    n00bb_ffi_t *ffi = n00bb_ffi_new();
    n00bb_setting_id_t contract_dirs_key = N00BB_KEY_PATHS_CONTRACT_DIRS;
    for (size_t i = 0; i < n00bb_setting_count(contract_dirs_key); i++)
        n00bb_ffi_add_contract_dir(ffi,
                                   n00bb_setting_at(contract_dirs_key, i));
    const n00bb_module_generator_t generator_items[] = {
        {CORE_STRING_STATIC("ffi"), n00bb_ffi_generate, ffi},
    };
    const n00bb_module_generators_t generators = {
        generator_items, sizeof(generator_items) / sizeof(generator_items[0]),
    };
    const core_string_t module_modifiers[] = {CORE_STRING_STATIC("ffi")};
    n00bb_preprocessor_options_t preprocessor_options = {
        .definitions = definitions,
        .definition_count = define_count,
        .syntax_preferences = syntax_preferences,
        .syntax_preference_count = syntax_preference_count,
        .disable_syntax_warnings = !n00bb_setting_bool(
            N00BB_KEY_SYNTAX_WARNINGS),
        .release_mode = n00bb_setting_bool(
            N00BB_KEY_BUILD_RELEASE),
        .build_time = build_time,
        .module_modifiers = module_modifiers,
        .module_modifier_count = 1,
        .module_literal_context = &generators,
    };
    parse_grammar_t *grammar = nullptr;
    th_env *environment = nullptr;
    core_link_unit_t *link = nullptr;
    n00bb_link_analysis_t *link_analysis = nullptr;
    ir_ctx_t *ir_context = ir_ctx_new();
    th_ctx *types = ir_ctx_types(ir_context);
    th_install_nfc(types);
    n00bb_cache_t *cache = (build_command || check_command) && scheduled
        && options_valid && input_count == 1 && !interface_count
        ? open_object_cache(resolver, args, ir_context) : nullptr;
    size_t linked_count = (size_t)interface_count
        + n00bb_cache_interface_count(cache);
    if (linked_count > (size_t)interface_count) {
        const th_env **combined = core_alloc_array(const th_env *,
                                                   linked_count);
        for (int32_t i = 0; i < interface_count; i++)
            combined[i] = linked_interfaces[i];
        for (size_t i = (size_t)interface_count; i < linked_count; i++)
            combined[i] = n00bb_cache_interface(cache,
                                                i - (size_t)interface_count);
        core_free(linked_interfaces);
        linked_interfaces = combined;
    }
    if (!locate_only) {
        grammar = parse_grammar_new();
        parse_grammar_set_error_recovery(grammar, false);
        core_string_t grammar_path = core_string_dup(
            n00bb_toolchain()->grammar);
        if (!grammar_path.data || !parse_bnf_load_file(
                grammar_path, CORE_STRING_STATIC("module"), grammar)) {
            core_string_t error = parse_bnf_error();
            n00bb_report(cli_diagnostics, nullptr,
                N00B_DIAG_TOOLCHAIN_GRAMMAR_UNAVAILABLE,
                (core_diag_location_t){},
                N00B_TEXT_CANNOT_LOAD_THE_N00B_GRAMMAR,
                N00BB_ARGS(ARG_TEXT(grammar_path)));
            if (error.u8_bytes)
                n00bb_report_note(cli_diagnostics, N00B_TEXT_TEXT,
                    N00BB_ARGS(ARG_TEXT(error)));
            scheduled = false;
        }
        else {
            parse_grammar_finalize(grammar);
            if (!scan_grammar_lex_compiled(grammar)) {
                core_string_t error = scan_lex_compile_error();
                n00bb_report(cli_diagnostics, nullptr,
                    N00B_DIAG_TOOLCHAIN_GRAMMAR_UNAVAILABLE,
                    (core_diag_location_t){},
                    N00B_TEXT_CANNOT_BUILD_THE_LEXER_FOR,
                    N00BB_ARGS(ARG_TEXT(grammar_path)));
                if (error.u8_bytes)
                    n00bb_report_note(cli_diagnostics, N00B_TEXT_TEXT,
                        N00BB_ARGS(ARG_TEXT(error)));
                scheduled = false;
            }
        }
        core_free(grammar_path.data);
    }
    bool profile_compile = n00bb_setting_bool(N00BB_KEY_DEV_PROFILE_COMPILE);
    double profile_tick = profile_compile ? n00bb_profile_seconds() : 0;
    bool needs_packages = preprocess_command || show_symbols
        || (!locate_only && !lex_command && !parse_command
            && (!fmt_command || normalize_fmt));
    if (scheduled && options_valid && needs_packages
        && !n00bb_resolver_prepare_packages(resolver)) {
        n00bb_report(cli_diagnostics, nullptr, N00B_DIAG_PACKAGE_SCAN_FAILED,
            (core_diag_location_t){},
            N00B_TEXT_CANNOT_SCAN_THE_PACKAGE_DIRECTORIES,
            N00BB_ARGS(ARG_ERRNO(errno ? errno : EIO)));
        n00bb_resolver_note_roots(resolver, cli_diagnostics);
        scheduled = false;
    }
    warning_policy_t warnings = {
        .resolver = resolver,
        .cache = cache,
        .package_warnings = n00bb_setting_bool(
            N00BB_KEY_WARNINGS_PACKAGE),
        .changed_files_only = parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--warn-changed-files-only")),
    };
    bool located = scheduled && options_valid && run_locate_jobs(queue, resolver, &requests,
        grammar, !locate_only, show_tokens,
        !locate_only && !lex_command, show_tree,
        needs_packages,
        show_preprocessed, show_symbols,
        lower_command || build_command || library_command || compile_command,
        library_command || compile_command,
        build_command,
        native_roots, (size_t)native_root_count,
        linked_interfaces, linked_count, cache, &warnings,
        &preprocessor_options,
        ir_context, types, (size_t)requested_jobs, &environment, &link,
        &link_analysis);
    if (link_analysis) link_analysis->cache = cache;
    bool optimistic_cache = n00bb_cache_optimistic(cache);
    if (optimistic_cache && !build_time_reported) {
        bool assumptions = n00bb_cache_assumptions_hold(cache, link,
                                                        link_analysis);
        core_string_t reason = !located
            ? strict_retry_reason(cache, queue, &requests, cli_diagnostics)
            : !assumptions
                ? CORE_STRING_STATIC("cached interface assumptions do not hold")
                : (core_string_t){0};
        if (reason.data) {
            n00bb_cache_log_restart(reason);
            restart_with_strict_cache(argv, argv0);
        }
    }
    if (cache && located) {
        core_buffer_t stale = {0};
        n00bb_cache_stale_modules(cache, &stale);
        if (stale.byte_len) restart_excluding_cached(argv, argv0, &stale);
        core_buffer_release(&stale);
    }
    if (located && deferred_build_time && link_analysis)
    {
        located = fork_build_time_stage(argc, argv, argv0, cli,
            &invocation.parsed, link, link_analysis, cli_diagnostics,
            &build_time_reported);
        args = invocation.parsed;
    }
    if (parse_cmdr_flag_present(args, CORE_STRING_STATIC("--show-ffi"))
        && !show_module_literal(resolver, CORE_STRING_STATIC("ffi"),
               parse_cmdr_flag_str(args, CORE_STRING_STATIC("--show-ffi")))) {
        core_string_t requested = parse_cmdr_flag_str(args,
            CORE_STRING_STATIC("--show-ffi"));
        n00bb_report(cli_diagnostics, nullptr, N00B_DIAG_FFI_LITERAL_NOT_FOUND,
            (core_diag_location_t){}, N00B_TEXT_NO_FFI_LITERAL_IS_NAMED,
            N00BB_ARGS(ARG_TEXT(requested)));
        located = false;
    }
    if (located && fmt_command)
        located = n00bb_format_units(grammar, queue, &requests, args);
    if (profile_compile) {
        double now = n00bb_profile_seconds();
        fprintf(stderr, "PROFILE jobs %.3f\n", now - profile_tick);
        profile_tick = now;
    }
    if (located && link_analysis)
        link_analysis->dump_ranges = parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--dump-ranges"));
    if (located && (lower_command || build_command || compile_command
                    || library_command))
        located = lower_requests_to_ir(grammar, link,
            link_analysis, ir_context, cli_diagnostics,
            (!build_command
                && parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-ir")))
                || (lower_command && !check_command
                    && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-control-flow"))
                    && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-pdg"))
                    && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-summaries"))
                    && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-ranges"))),
            parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--dump-control-flow")),
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-pdg")),
            parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--dump-summaries")),
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-layout")));
    if (link_analysis && !build_time && !check_command && no_comptime
        && (link_analysis->build_time_needed
            || link_analysis->literal_needed)) {
        n00bb_report(cli_diagnostics, nullptr, N00B_DIAG_COMPTIME_DISABLED,
            (core_diag_location_t){}, N00B_TEXT_THIS_PROGRAM_COMPUTES_VALUES_AT,
            N00BB_NO_ARGS);
        n00bb_report_help(cli_diagnostics, N00B_TEXT_BUILD_WITHOUT_NO_COMPTIME,
            N00BB_NO_ARGS);
        located = false;
    }
    else if (link_analysis && !build_time && !check_command
        && (link_analysis->build_time_needed
            || link_analysis->literal_needed)) {
        restart_with_build_time(argv, argv0,
                                !link_analysis->build_time_needed);
        n00bb_internal_error(cli_diagnostics, nullptr,
            (core_diag_location_t){}, "restarting for the build-time run",
            "could not restart the compiler: %s",
            strerror(errno ? errno : EIO));
        located = false;
    }
    if (located && cache && !check_command) {
        bool recorded = n00bb_cache_record_summaries(cache, link,
                                                     link_analysis);
        if (optimistic_cache && !build_time_reported
            && (!recorded || !n00bb_cache_summary_assumptions_hold(cache))) {
            n00bb_cache_log_restart(CORE_STRING_STATIC(
                "cached summary assumptions do not hold"));
            restart_with_strict_cache(argv, argv0);
        }
    }
    core_string_t header_output = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    if (located && header_output.data
        && !n00bb_ffi_write_headers(ffi, header_output)) {
        n00bb_report(cli_diagnostics, nullptr, N00B_DIAG_HEADER_WRITE_FAILED,
            (core_diag_location_t){}, N00B_TEXT_CANNOT_WRITE_THE_GENERATED_C,
            N00BB_ARGS(ARG_TEXT(header_output),
                       ARG_ERRNO(errno ? errno : EIO)));
        located = false;
    }
    if (profile_compile) {
        double now = n00bb_profile_seconds();
        fprintf(stderr, "PROFILE lower %.3f\n", now - profile_tick);
        profile_tick = now;
    }
    if (located && library_command && emit_interface && link_analysis) {
        th_env *interface_environment = link_analysis->interface_environment;
        located = interface_environment
            && th_env_freeze(interface_environment);
        if (!located)
            n00bb_internal_error(cli_diagnostics, nullptr,
                (core_diag_location_t){}, "writing the semantic interface",
                "could not archive the compile-time metadata");
    }
    if (located && library_command && emit_interface) {
        core_string_t path = interface_output.data ? interface_output
                                                   : CORE_STRING_STATIC("");
        core_object_archive_error_t archive_error =
            CORE_OBJECT_ARCHIVE_OK;
        th_env *interface_environment = link_analysis
            ? link_analysis->interface_environment : nullptr;
        bool saved = interface_environment
            && th_env_save(interface_environment, path, 0, &archive_error);
        if (!saved) {
            core_string_t reason =
                core_object_archive_error_string(archive_error);
            n00bb_report(cli_diagnostics, nullptr,
                N00B_DIAG_INTERFACE_WRITE_FAILED, (core_diag_location_t){},
                N00B_TEXT_CANNOT_WRITE_SEMANTIC_INTERFACE,
                N00BB_ARGS(ARG_TEXT(path), ARG_TEXT(reason)));
            located = false;
        }
    }
    bool abi_emitted = false;
    bool specialize = !parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--no-specialize"));
    bool emit_program = located && build_command
        && (n00bb_compile_phase() == PARSE_SYM_PHASE_RUNTIME
            || n00bb_build_time_run_needed(link_analysis));
    if (located && emit_program && cache && build_time) {
        core_buffer_t objectless = {0};
        n00bb_cache_objectless_modules(cache, &objectless);
        if (objectless.byte_len)
            restart_excluding_cached(argv, argv0, &objectless);
        core_buffer_release(&objectless);
    }
    if (located && emit_program && cache)
        (void)n00bb_cache_snapshot(cache, link);
    if (located && build_command && build_time && !emit_program && cache
        && n00bb_cache_snapshot(cache, link))
        (void)n00bb_cache_store(cache, link, link_analysis, nullptr, 0);
    if (located && check_command && cache
        && n00bb_cache_snapshot(cache, link))
        (void)n00bb_cache_store(cache, link, link_analysis, nullptr, 0);
    if (emit_program && specialize)
        located = specialize_calls(link_analysis, cli_diagnostics);
    if (located && emit_program && specialize
        && parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--report-specialization-gaps")))
        (void)report_specialization_gaps(link_analysis);
    if (located && emit_program && !specialize)
        located = n00bb_rewrite_box_operations(link_analysis)
            && publish_declared_aggregate_layouts(link_analysis)
            && publish_interned_variant_layouts(link_analysis);
    if (located && emit_program && n00bb_setting_bool(
            N00BB_KEY_BUILD_UNCHECKED_WIDE_NARROWING))
        uncheck_wide_narrowing(link_analysis);
    if (located && emit_program) {
        optimize_range_checks(link_analysis);
        located = n00bb_recompute_summaries(link_analysis);
    }
    if (located && emit_program && parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--verify-analyses")))
        located = verify_analyses(link_analysis, cli_diagnostics);
    if (located && build_command
        && parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-ir")))
        located = n00bb_dump_ir(link_analysis);
    if (emit_program) {
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE specialize %.3f\n", now - profile_tick);
            profile_tick = now;
        }
    }
    if (located && emit_program) {
        located = n00bb_emit_abi_artifacts(link, link_analysis,
            !preprocessor_options.release_mode, false);
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE abi %.3f\n", now - profile_tick);
            profile_tick = now;
        }
        core_list_t(core_buffer_t) shim_bitcode =
            core_list_new(core_buffer_t);
        core_string_t compiler_executable = located
            ? core_os_executable_path(argv0) : (core_string_t){0};
        if (located) {
            core_list_t(n00bb_ir_func_ptr_t) *reachable =
                &link_analysis->emitted_functions;
            core_string_t *externs = core_alloc_array(core_string_t,
                reachable->len + 1);
            size_t extern_count = 0;
            for (size_t i = 0; externs && i < reachable->len; i++)
                if (!reachable->data[i]->n_blocks
                    && reachable->data[i]->name.data)
                    externs[extern_count++] = reachable->data[i]->name;
            n00bb_ffi_select(ffi, externs, extern_count);
            core_free(externs);
        }
        if (located && n00bb_ffi_shim_count(ffi)) {
            char *shim_triple = n00bb_llvm_target_triple(
                link_analysis->target.triple);
            core_string_t shim_text = core_string_from_cstr(shim_triple);
            core_free(shim_triple);
            located = shim_text.data && n00bb_ffi_compile_shims(ffi,
                shim_text, compiler_executable, &shim_bitcode,
                cli_diagnostics);
            core_free(shim_text.data);
        }
        core_free(compiler_executable.data);
        core_list_t(core_string_t) archives = core_list_new(core_string_t);
        core_list_t(core_string_t) libraries = core_list_new(core_string_t);
        core_list_t(core_string_t) frameworks = core_list_new(core_string_t);
        core_list_t(core_string_t) search_paths =
            core_list_new(core_string_t);
        for (size_t i = 0; i < n00bb_ffi_archive_count(ffi); i++)
            core_list_push(archives, n00bb_ffi_archive_at(ffi, i));
        for (size_t i = 0; i < n00bb_ffi_library_count(ffi); i++)
            core_list_push(libraries, n00bb_ffi_library_at(ffi, i));
        for (size_t i = 0; i < n00bb_ffi_framework_count(ffi); i++)
            core_list_push(frameworks, n00bb_ffi_framework_at(ffi, i));
        for (size_t i = 0; i < n00bb_ffi_search_path_count(ffi); i++)
            core_list_push(search_paths, n00bb_ffi_search_path_at(ffi, i));
        n00bb_native_inputs_t native_inputs = {
            .bitcode = shim_bitcode.data,
            .bitcode_count = shim_bitcode.len,
            .archives = archives.data,
            .archive_count = archives.len,
            .libraries = libraries.data,
            .library_count = libraries.len,
            .frameworks = frameworks.data,
            .framework_count = frameworks.len,
            .search_paths = search_paths.data,
            .search_path_count = search_paths.len,
        };
        if (located)
            located = n00bb_build_executable(link, link_analysis, args,
                !preprocessor_options.release_mode, native_cpu, argv0,
                &native_inputs, cli_diagnostics);
        for (size_t i = 0; i < shim_bitcode.len; i++)
            core_buffer_release(&shim_bitcode.data[i]);
        core_list_free(shim_bitcode);
        core_list_free(archives);
        core_list_free(libraries);
        core_list_free(frameworks);
        core_list_free(search_paths);
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE native %.3f\n", now - profile_tick);
            profile_tick = now;
        }
    }
    if (located && compile_command) {
        located = n00bb_emit_abi_artifacts(link, link_analysis,
            !preprocessor_options.release_mode, true);
        if (located)
            abi_emitted = located = n00bb_write_compile_abi_artifact(
                link, args, cli_diagnostics);
    }
    if (located && library_command && !emit_interface
        && parse_cmdr_flag_present(args, CORE_STRING_STATIC("--output"))) {
        located = n00bb_emit_abi_artifacts(link, link_analysis,
            !preprocessor_options.release_mode, false);
        if (located)
            abi_emitted = located = n00bb_write_library_abi_artifact(link,
                link_analysis, &link_analysis->target, args, cli_diagnostics);
    }
    if (located && parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--dump-link-unit")))
        dump_link_unit(link);
    if (located && !locate_only && !lex_command && !parse_command
        && !preprocess_command && !lower_command && !fmt_command
        && !(library_command && emit_interface) && !abi_emitted
        && !build_command) {
        n00bb_report(cli_diagnostics, nullptr,
            N00B_DIAG_COMMAND_NOT_IMPLEMENTED, (core_diag_location_t){},
            N00B_TEXT_N00B_IS_NOT_IMPLEMENTED_YET,
            N00BB_ARGS(ARG_TEXT(command)));
        located = false;
    }

    bool diagnostics_deferred = lower_command || build_command
        || library_command || compile_command;
    if (diagnostics_deferred && !build_time_reported) {
        if (link_analysis) {
            core_translation_unit_t *root = link_analysis->units.len
                ? link_analysis->units.data[0] : nullptr;
            for (size_t i = 0; i < link_analysis->units.len; i++) {
                core_translation_unit_t *unit = link_analysis->units.data[i];
                merge_unit_diagnostics(cli_diagnostics, &warnings, unit,
                    n00bb_resolver_same_root(resolver, root, unit));
            }
        }
        if (link)
            core_diagnostics_merge(cli_diagnostics,
                                   core_link_unit_diagnostics(link));
    }
    core_string_t late_retry = optimistic_cache && !located
        && !build_time_reported
        ? strict_retry_reason(cache, queue, &requests, cli_diagnostics)
        : (core_string_t){0};
    core_buffer_t unloadable = {0};
    n00bb_cache_unloadable_modules(cache, &unloadable);
    if (unloadable.byte_len) {
        n00bb_cache_log_restart(CORE_STRING_STATIC(
            "cached IR names a function this program does not define"));
        restart_excluding_cached(argv, argv0, &unloadable);
    }
    core_buffer_release(&unloadable);

    core_link_unit_free(link);
    th_env_free(environment);
    n00bb_diagnostic_filter_t *diagnostic_filter =
        n00bb_diagnostic_filter_take(link_analysis);
    core_diagnostics_set_filter(cli_diagnostics, n00bb_diagnostic_visible,
                                diagnostic_filter);
    n00bb_link_analysis_free(link_analysis);
    n00bb_cache_free(cache);
    release_requests(queue, &requests);
    for (int32_t i = 0; i < interface_count; i++)
        th_env_free((th_env *)linked_interfaces[i]);
    core_free(linked_interfaces);
    core_free(native_roots);
    if (late_retry.data) {
        n00bb_cache_log_restart(late_retry);
        restart_with_strict_cache(argv, argv0);
    }
    if (!check_comptime && merge_check_handoff(cli_diagnostics))
        located = false;
    if (core_diagnostics_count(cli_diagnostics) || check_comptime) {
        n00bb_resolver_remap_diagnostics(resolver, cli_diagnostics);
        bool continued = check_comptime
            && finish_check_comptime_phase(argv, argv0, cli_diagnostics,
                                           diagnostic_filter);
        if (continued) located = false;
        else
            print_counted_diagnostics(cli_diagnostics, diagnostic_filter);
    }
    if (!located && build_command && !build_time && !forked_build_time
        && input_count >= 1) {
        core_string_t input = parse_cmdr_arg_str(args, 0);
        print_build_summary(input.data ? input : CORE_STRING_STATIC(""));
    }
    int exit_status = located ? 0 : 1;
    for (size_t i = 0; i < core_diagnostics_count(cli_diagnostics); i++)
        if (core_diagnostics_at(cli_diagnostics, i)->severity
            == CORE_DIAG_INTERNAL)
            exit_status = 3;
    core_diagnostics_free(cli_diagnostics);
    n00bb_diagnostic_filter_free(diagnostic_filter);
    ir_ctx_free(ir_context);
    parse_grammar_free(grammar);
    n00bb_resolver_free(resolver);
    core_work_queue_free(queue);
    n00bb_ffi_free(ffi);
    core_free(definitions);
    core_free(syntax_preferences);
    n00bb_invocation_close(&invocation);
    return exit_status;
}
