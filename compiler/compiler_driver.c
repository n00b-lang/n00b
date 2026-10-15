#include <stdio.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

double n00bb_profile_seconds(void);

#include "driver.h"
#include "compiler_cli.h"
#include "compiler_tu.h"
#include "compiler_link.h"
#include "compiler_link_lifetime.h"
#include "compiler_ir_setup.h"
#include "compiler_lower.h"
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
#include "llvm_backend.h"
#include "llvm_native_cache.h"
#include "native_frame_maps.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"
#include "storage_selection.h"
#include "specialize.h"
#include "compiler_ffi.h"
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
#include "lib/diagnostic.h"
#include "logic/clpfd_store.h"
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
#include "rich_string.h"
#include "signature_typevars.h"

enum { N00BB_JOB_LOCATE_TU = 1, N00BB_JOB_PREPROCESS = 2 };

typedef struct {
    core_work_job_t *job;
    char *operand;
    bool root_input;
    core_module_resolve_status_t resolve_status;
    bool resolve_attempted;
} locate_request_t;
core_list_decl(locate_request_t);

typedef struct {
    core_list_t(locate_request_t) ordered;
    core_dict_t by_job;
    core_diagnostics_t *diagnostics;
} locate_requests_t;

typedef core_work_job_t *core_work_job_ptr_t;
core_list_decl(core_work_job_ptr_t);
core_list_decl(size_t);
core_list_decl(uint32_t);

typedef struct {
    core_work_queue_t *queue;
    n00bb_resolver_t *resolver;
    locate_requests_t *requests;
    pthread_mutex_t *requests_lock;
    parse_grammar_t *grammar;
    th_ctx *types;
    n00bb_interface_context_t interfaces;
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
                 char **operand, bool *root_input)
{
    if (requests_lock) pthread_mutex_lock(requests_lock);
    locate_request_t *request = request_for_job(requests, job);
    if (request) {
        *operand = core_strdup(request->operand);
        *root_input = request->root_input;
    }
    if (requests_lock) pthread_mutex_unlock(requests_lock);
    return request != nullptr;
}

static bool
add_locate_request_for_module(core_work_queue_t *queue,
                   locate_requests_t *requests,
                   pthread_mutex_t *requests_lock,
                   const core_module_spec_t *module, const char *operand,
                   bool root_input, bool allow_existing,
                   core_work_job_t **job_out)
{
    if (requests_lock) pthread_mutex_lock(requests_lock);
    core_work_job_t *job = nullptr;
    core_work_submit_result_t submitted = core_work_submit(queue,
        N00BB_JOB_LOCATE_TU, module, 0, nullptr, 0, &job);
    if (submitted == CORE_WORK_INVALID
        || submitted == CORE_WORK_DEPENDENCY_FAILED) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "cannot schedule '%s'", operand);
        core_diagnostics_report(requests->diagnostics, nullptr,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0020"),
            core_string_from_cstr(message.data));
        core_buffer_release(&message);
        if (requests_lock) pthread_mutex_unlock(requests_lock);
        return false;
    }

    locate_request_t *prior = request_for_job(requests, job);
    if (prior) {
        if (!allow_existing && strcmp(prior->operand, operand)) {
            core_string_t name = core_work_job_module(job)->identifier;
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "multiple inputs identify the same module '%.*s'",
                (int)name.u8_bytes, name.data);
            core_diagnostics_report(requests->diagnostics, nullptr,
                CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0021"),
                core_string_from_cstr(message.data));
            core_buffer_release(&message);
            if (requests_lock) pthread_mutex_unlock(requests_lock);
            return false;
        }
        prior->root_input |= root_input;
        if (job_out) *job_out = job;
        if (requests_lock) pthread_mutex_unlock(requests_lock);
        return true;
    }
    core_list_push(requests->ordered, ((locate_request_t){
        .job = job,
        .operand = core_strdup(operand),
        .root_input = root_input,
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
                   const char *operand)
{
    core_module_spec_t module = {0};
    if (!n00bb_operand_module_spec(operand, &module)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "invalid module or file name '%s'",
                           operand);
        core_diagnostics_report(requests->diagnostics, nullptr,
            CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-B0022"),
            core_string_from_cstr(message.data));
        core_buffer_release(&message);
        return false;
    }
    bool ok = add_locate_request_for_module(queue, requests, nullptr, &module,
        operand, true, false, nullptr);
    core_module_spec_release(&module);
    return ok;
}

static void
dump_token(parse_grammar_t *grammar, core_translation_unit_t *unit,
           parse_token_info_t *token)
{
    core_string_t path = core_source_file_path(&unit->source);
    const char *source = core_source_file_contents(&unit->source);
    size_t source_size = core_source_file_size(&unit->source);
    core_string_t name = parse_terminal_name(grammar, token->tid);
    size_t end = token->end_byte > source_size ? source_size : token->end_byte;
    size_t start = token->start_byte <= end ? token->start_byte : end;
    printf("%.*s:%u:%u %.*s ", (int)path.u8_bytes, path.data,
           token->line, token->column, (int)name.u8_bytes, name.data);
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
    if (core_option_is_set(token->postfix_modifier)) {
        core_string_t modifier = core_option_get(token->postfix_modifier);
        printf(" modifier='%.*s", (int)modifier.u8_bytes, modifier.data);
    }
    fputc('\n', stdout);
}

static void
dump_tree(parse_grammar_t *grammar, parse_tree_t *tree)
{
    char *text = parse_tree_dump_str(grammar, tree, false);
    if (text) {
        fputs(text, stdout);
        core_free(text);
    }
}

static const char *
symbol_kind_name(parse_sym_kind_t kind)
{
    static const char *names[] = {
        "variable", "function", "type", "tag", "enum-constant", "label",
        "parameter", "module",
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind]
                                                           : "unknown";
}

static const char *
symbol_mutability_name(parse_sym_mutability_t mutability)
{
    static const char *names[] = {"mutable", "immutable", "const", "global"};
    return (size_t)mutability < sizeof(names) / sizeof(names[0])
        ? names[mutability] : "unknown";
}

static const char *
symbol_phase_name(parse_sym_phase_t phase)
{
    switch (phase) {
    case PARSE_SYM_PHASE_NONE: return "none";
    case PARSE_SYM_PHASE_COMPTIME: return "comptime";
    case PARSE_SYM_PHASE_RUNTIME: return "runtime";
    case PARSE_SYM_PHASE_BOTH: return "comptime+runtime";
    }
    return "invalid";
}

static const char *
static_value_kind_name(n00bb_static_value_kind_t kind)
{
    static const char *names[] = {
        "bool", "int", "float", "string", "sequence", "tuple",
        "record", "set", "dictionary", "nil", "variant"};
    return (size_t)kind < sizeof(names) / sizeof(names[0])
        ? names[kind] : "invalid";
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
    case N00BB_STATIC_VALUE_VARIANT:
        printf("%.*s=", (int)value->as.variant.arm.u8_bytes,
               value->as.variant.arm.data);
        dump_static_value_contents(value->as.variant.payload, depth + 1);
        break;
    case N00BB_STATIC_VALUE_SEQUENCE:
    case N00BB_STATIC_VALUE_TUPLE:
    case N00BB_STATIC_VALUE_RECORD:
    case N00BB_STATIC_VALUE_SET:
    case N00BB_STATIC_VALUE_DICTIONARY:
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
    printf(" static-value=%s:", static_value_kind_name(value->kind));
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
    for (size_t i = 0; i < unit->import_count; i++) {
        core_string_t imported = unit->imports[i]->module.identifier;
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
            printf("    %.*s: %s, %s", (int)symbol->name.u8_bytes,
                   symbol->name.data, symbol_kind_name(symbol->kind),
                   symbol_mutability_name(symbol->mutability));
            if (symbol->type && state->types)
                printf(", type=%s", n00bb_print_type(state->types, symbol->type));
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
                    const char *kind = extent.kind == IR_SHAPE_EXTENT_STATIC
                        ? "static"
                        : extent.kind == IR_SHAPE_EXTENT_COMPTIME
                            ? "comptime"
                        : extent.kind == IR_SHAPE_EXTENT_RUNTIME
                            ? "runtime" : "unresolved";
                    printf(", dim%u=%s", d, kind);
                    if (extent.producer)
                        printf("(producer=%s)", extent.producer->name);
                    else if (extent.producer_identifier)
                        printf("(producer=%s)", extent.producer_identifier);
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
            if (symbol->error_type && state->types)
                printf(", error=%s", n00bb_print_type(state->types,
                                               symbol->error_type));
            if (symbol->direction.data)
                printf(", direction=%.*s", (int)symbol->direction.u8_bytes,
                       symbol->direction.data);
            if (symbol->pass_by.data)
                printf(", pass_by=%.*s", (int)symbol->pass_by.u8_bytes,
                       symbol->pass_by.data);
            if (symbol->is_optional) printf(", optional");
            if (symbol->is_variadic) printf(", variadic");
            if (symbol->error_type && state->types)
                printf(", error=%s", n00bb_print_type(state->types,
                                               symbol->error_type));
            if (symbol->direction.data)
                printf(", direction=%.*s", (int)symbol->direction.u8_bytes,
                       symbol->direction.data);
            if (symbol->pass_by.data)
                printf(", pass_by=%.*s", (int)symbol->pass_by.u8_bytes,
                       symbol->pass_by.data);
            if (symbol->is_thread_local) printf(", thread-local");
            printf(", phases=%s, mutable-in=%s, initializer-in=%s",
                   symbol_phase_name(symbol->phases),
                   symbol_phase_name(symbol->mutable_phases),
                   symbol_phase_name(symbol->initializer_phases));
            printf(", storage-duration=%s",
                   parse_sym_storage_duration_name(
                       symbol->storage_duration));
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
            if (symbol->lifetime.escapes) printf(", may-escape");
            printf(", uses=%u", symbol->uses);
            parse_token_info_t *token = symbol->decl_node
                ? parse_tree_find_first_terminal(symbol->decl_node) : nullptr;
            if (token)
                printf(", declaration=%u:%u", token->line, token->column);
            else printf(", declaration=none");
            printf(", definitions=[");
            for (size_t d = 0; d < symbol->definition_count; d++) {
                parse_token_info_t *site = parse_tree_find_first_terminal(
                    symbol->definition_nodes[d]);
                if (d) printf(", ");
                if (site) printf("%u:%u", site->line, site->column);
                else printf("?");
            }
            printf("], use-sites=[");
            for (size_t u = 0; u < symbol->use_count; u++) {
                parse_token_info_t *site = parse_tree_find_first_terminal(
                    symbol->use_nodes[u]);
                if (u) printf(", ");
                if (site) printf("%u:%u", site->line, site->column);
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
                    if (site) printf("%u:%u", site->line, site->column);
                    else printf("?");
                    printf("->%s", resolution.kind
                            == PARSE_NODE_RESOLUTION_SYMBOL
                        && resolution.target.symbol == symbol
                        && core_tree_node_value(use).sym == symbol
                            ? "self" : "mismatch");
                }
                printf("]");
            }
            putchar('\n');
        }
        core_list_free(symbols);
    }
}

static const char *
link_stage_state_name(core_link_stage_state_t state)
{
    switch (state) {
    case CORE_LINK_STAGE_PENDING: return "pending";
    case CORE_LINK_STAGE_RUNNING: return "running";
    case CORE_LINK_STAGE_COMPLETE: return "complete";
    case CORE_LINK_STAGE_UNAVAILABLE: return "unavailable";
    case CORE_LINK_STAGE_FAILED: return "failed";
    }
    return "invalid";
}

static const char *
link_contribution_state_name(core_link_contribution_state_t state)
{
    switch (state) {
    case CORE_LINK_CONTRIBUTION_PENDING: return "pending";
    case CORE_LINK_CONTRIBUTION_COMPLETE: return "complete";
    case CORE_LINK_CONTRIBUTION_UNAVAILABLE: return "unavailable";
    case CORE_LINK_CONTRIBUTION_FAILED: return "failed";
    }
    return "invalid";
}

static const char *
link_dependency_state_name(core_link_dependency_state_t state)
{
    switch (state) {
    case CORE_LINK_DEPENDENCY_RESOLVED: return "resolved";
    case CORE_LINK_DEPENDENCY_DEFERRED: return "deferred";
    case CORE_LINK_DEPENDENCY_EXTERNAL: return "external";
    case CORE_LINK_DEPENDENCY_UNAVAILABLE: return "unavailable";
    case CORE_LINK_DEPENDENCY_FAILED: return "failed";
    }
    return "invalid";
}

static const char *
cleanup_release_policy_name(n00bb_cleanup_release_policy_t policy)
{
    switch (policy) {
    case N00BB_CLEANUP_RELEASE_NONE: return "none";
    case N00BB_CLEANUP_RELEASE_RUNTIME_MANAGED: return "runtime-managed";
    case N00BB_CLEANUP_RELEASE_POLICY_UNAVAILABLE: return "unavailable";
    }
    return "invalid";
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
        printf("    %u %s (%.*s)\n", kind,
               link_stage_state_name(core_link_unit_stage_state(link, kind)),
               (int)provenance.u8_bytes,
               provenance.data ? provenance.data : "");
    }
    puts("  contributions:");
    for (size_t i = 0; i < core_link_unit_contribution_count(link); i++) {
        core_link_contribution_view_t fact;
        if (!core_link_unit_contribution_at(link, i, &fact)) continue;
        printf("    %.*s:%.*s v%u %s (%.*s)\n",
               (int)(fact.module.data ? fact.module.u8_bytes : 11),
               fact.module.data ? fact.module.data : "<link-unit>",
               (int)fact.domain.u8_bytes, fact.domain.data, fact.version,
               link_contribution_state_name(fact.state),
               (int)fact.provenance.u8_bytes,
               fact.provenance.data ? fact.provenance.data : "");
    }
    static const char *binding_roles[] = {
        "n00bb.ir.function",
        "n00bb.ir.comptime.function",
        "n00bb.ir.global",
        "n00bb.semantic.type",
        "ir.array-shape",
        "n00bb.semantic.declaration-facts",
        "n00bb.ir.context.enter",
        "n00bb.ir.context.exit",
        "n00bb.ir.context.error",
        "n00bb.ir.context.ensure",
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
            if (core_link_declaration_binding(declaration,
                    core_string_from_cstr(binding_roles[r])))
                printf(" binding %s", binding_roles[r]);
        }
        bool has_context_hooks = false;
        for (size_t r = 6; r < sizeof(binding_roles) / sizeof(binding_roles[0]);
             r++)
            has_context_hooks |= core_link_declaration_binding(declaration,
                core_string_from_cstr(binding_roles[r])) != nullptr;
        n00bb_link_payload_t *facts = core_link_declaration_binding(
            declaration,
            CORE_STRING_STATIC("n00bb.semantic.declaration-facts"));
        if (facts) {
            printf(" facts[dimensions=%u, initializers=%zu, phases=%s, "
                   "storage=%s, alias-analysis=%s]",
                   ir_shape_rank(parse_sym_binding(facts->symbol,
                       CORE_STRING_STATIC("ir.array-shape"))),
                   facts->initializers.len,
                   symbol_phase_name(facts->phases),
                   parse_sym_storage_duration_name(facts->storage_duration),
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
                    printf(" cleanup-range[%.*s,phase=%s,storage=%s,"
                           "release=%s,cfg-edges=%zu,endpoints=%zu,point-endpoints=%zu,"
                           "edge-endpoints=%zu,exits=%zu,safe-exits=%zu,"
                           "live-exits=%zu,unknown-exits=%zu,evidence=%s,"
                           "finalizer=unspecified]",
                           (int)cleanup->symbol->name.u8_bytes,
                           cleanup->symbol->name.data,
                           symbol_phase_name(cleanup->phase),
                           parse_sym_storage_duration_name(
                               cleanup->storage_duration),
                           cleanup_release_policy_name(
                               cleanup->release_policy),
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
            if (facts->initializers.len) {
                printf(" initializer-phases=[");
                for (size_t p = 0; p < facts->initializers.len; p++) {
                    if (p) printf(",");
                    printf("%s", symbol_phase_name(
                        (parse_sym_phase_t)facts->initializers.data[p]
                            .phase));
                }
                printf("]");
            }
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
                const char *kind = extent.kind == IR_SHAPE_EXTENT_STATIC
                    ? "static"
                    : extent.kind == IR_SHAPE_EXTENT_COMPTIME
                        ? "comptime"
                    : extent.kind == IR_SHAPE_EXTENT_RUNTIME
                        ? "runtime" : "unresolved";
                printf(",dim%u=%s", d, kind);
                if (extent.producer)
                    printf(":producer=%s", extent.producer->name);
                else if (extent.producer_identifier)
                    printf(":producer=%s", extent.producer_identifier);
            }
            printf("]");
            ir_shape_measure_t element_count;
            if (ir_shape_element_count(array_shape, &element_count)
                && element_count.kind == IR_SHAPE_MEASURE_COMPTIME) {
                printf(" array-element-count=comptime");
                if (element_count.producer)
                    printf(":producer=%s", element_count.producer->name);
            }
        }
        for (size_t site_index = 0;
             site_index < core_link_declaration_site_count(declaration);
             site_index++) {
            core_link_declaration_site_view_t site = {0};
            if (!core_link_declaration_site(declaration, site_index, &site))
                continue;
            const char *origin = site.origin == CORE_LINK_ORIGIN_SOURCE
                ? "source" : site.origin == CORE_LINK_ORIGIN_LIBRARY
                ? "library" : "external-requirement";
            printf(" site-provenance[%s]=%.*s", origin,
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
        const char *state = target ? "resolved"
            : core_link_reference_is_deferred(reference) ? "deferred"
            : "unresolved";
        printf("    %s %.*s -> %.*s:%.*s", state,
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
        const char *kind = edge.kind == N00BB_LINK_DEPENDENCY_CALL
            ? "call" : edge.kind == N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
            ? "trait-dispatch"
            : edge.kind == N00BB_LINK_DEPENDENCY_CONTEXT_CALL
            ? "context-call"
            : edge.kind == N00BB_LINK_DEPENDENCY_INITIALIZER_USE
            ? "initializer-use"
            : edge.kind == N00BB_LINK_DEPENDENCY_COMPTIME_INITIALIZER_USE
            ? "comptime-initializer-use"
            : edge.kind == N00BB_LINK_DEPENDENCY_RUNTIME_INITIALIZER_USE
            ? "runtime-initializer-use"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ
            ? "function-global-read"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE
            ? "function-global-write"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE
            ? "function-global-read-write"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS
            ? "function-global-alias"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN
            ? "function-global-unknown"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_SUMMARY
            ? "function-global-read-summary"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY
            ? "function-global-write-summary"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY
            ? "function-global-read-write-summary"
            : edge.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY
            ? "function-global-alias-summary"
            : edge.kind == N00BB_LINK_DEPENDENCY_TRAIT_SPECIALIZATION
            ? "trait-specialization" : "other";
        printf("    %s[%s] %.*s:%.*s -> %.*s:%.*s",
               kind,
               link_dependency_state_name(edge.state),
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
            if (fact->receiver_type && state && state->types)
                printf(" (receiver=%s",
                       n00bb_print_type(state->types, fact->receiver_type));
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
        if (token && token->ignore_if_no_match
            && (token->tid == indent || token->tid == dedent)) {
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
n00bb_type_bracket_disambiguator(void *opaque, const char *predicate,
                                 parse_token_info_t *token)
{
    (void)opaque;
    (void)token;
    return !strcmp(predicate, "n00bb_prefer_type_application");
}

static bool
make_interface_module_source(locate_worker_context_t *context,
                             const core_module_spec_t *module,
                             core_source_file_t *source)
{
    const char *package = interface_package_for_module(
        &context->interfaces, module->identifier);
    if (!package) return false;
    core_buffer_t path = {0};
    core_buffer_printf(&path, "<interface:%.*s>",
        (int)module->identifier.u8_bytes, module->identifier.data);
    bool initialized = core_source_file_init(source,
        core_string_borrow(path.data, path.byte_len), "", 0);
    core_buffer_release(&path);
    if (!initialized) return false;
    n00bb_source_file_set_package(source, package);
    return true;
}

static bool
process_locate_job(locate_worker_context_t *context, core_work_job_t *job)
{
    char *operand = nullptr;
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
    bool logical_import = !root_input && module && operand
        && strlen(operand) == module->identifier.u8_bytes
        && !memcmp(operand, module->identifier.data,
                   module->identifier.u8_bytes);
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
    if (status == CORE_MODULE_NOT_FOUND && !root_input) {
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
    core_free(operand);
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
        const char *contents = core_source_file_contents(&unit->source);
        size_t length = core_source_file_size(&unit->source);
        n00bb_lex_result_t *result = core_alloc(n00bb_lex_result_t);
        memset(result, 0, sizeof(*result));
        result->tokens = scan_tokenize_owned(context->grammar, contents, length);
        if (!result->tokens) {
            core_free(result);
            ok = false;
        }
        else {
            result->tokens->scanner->file = core_option_set(
                core_string_t, core_source_file_path(&unit->source));
            n00bb_tu_state_new(unit, result, context->types);
            while (scan_stream_next(result->tokens)) {}
            if (!discard_ignored_layout(context->grammar, result->tokens)) {
                core_diagnostics_report(core_translation_unit_diagnostics(unit),
                    unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-P0002"),
                    CORE_STRING_STATIC("cannot normalize layout tokens"));
                ok = false;
            }
            scan_diagnostic_t diagnostic = {0};
            if (scan_diagnostic(result->tokens->scanner, &diagnostic) &&
                diagnostic.status != SCAN_STATUS_COMPLETE) {
                core_diagnostics_report(core_translation_unit_diagnostics(unit),
                    unit, CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-L0001"),
                    diagnostic.message);
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
                    int32_t error_pos = parse_pwz_error_pos(result->parser);
                    parse_token_info_t *error_token = error_pos >= 0
                        ? scan_stream_get(result->tokens, error_pos) : nullptr;
                    core_buffer_t message = {0};
                    if (error_token) {
                        core_string_t token_name = parse_terminal_name(
                            context->grammar, error_token->tid);
                        if (token_name.data)
                            core_buffer_printf(&message,
                                "syntax error at line %u, column %u near %.*s",
                                error_token->line, error_token->column,
                                (int)token_name.u8_bytes, token_name.data);
                        else if (error_token->tid > 0 && error_token->tid < 128)
                            core_buffer_printf(&message,
                                "syntax error at line %u, column %u near '%c'",
                                error_token->line, error_token->column,
                                error_token->tid);
                        else core_buffer_printf(&message,
                                "syntax error at line %u, column %u near token",
                                error_token->line, error_token->column);
                    }
                    else
                        core_buffer_printf(&message,
                            "syntax error at end of input (line %u, column %u)",
                            result->tokens->scanner->line,
                            result->tokens->scanner->column);
                    core_diagnostics_report(
                        core_translation_unit_diagnostics(unit), unit,
                        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-P0001"),
                        core_string_from_cstr(message.data));
                    core_buffer_release(&message);
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
        if (token && core_string_eq(parse_terminal_name(grammar, token->tid),
                                    CORE_STRING_STATIC("IDENTIFIER"))) {
            if (*has_name) core_buffer_putc(name, '.');
            core_string_t text = parse_tree_leaf_text(node);
            core_buffer_append(name, text.data, text.u8_bytes);
            *has_name = true;
        }
        return;
    }
    if (!nt_is(grammar, node, "member-chain")) return;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (nt_is(grammar, child, "member-chain"))
            append_import_names(grammar, child, name, has_name);
        else if (core_tree_is_leaf(child))
            append_import_names(grammar, child, name, has_name);
    }
}

static parse_tree_t *
find_string_token(parse_grammar_t *grammar, parse_tree_t *node)
{
    if (!node) return nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_node_value(child).group_top) {
            for (size_t j = 0; j < core_tree_num_children(child); j++) {
                parse_tree_t *item = core_tree_child(child, j);
                parse_token_info_t *token = parse_node_token(item);
                if (token && core_string_eq(
                        parse_terminal_name(grammar, token->tid),
                        CORE_STRING_STATIC("STRING_LIT"))) return item;
            }
            continue;
        }
        parse_token_info_t *token = parse_node_token(child);
        if (token && core_string_eq(parse_terminal_name(grammar, token->tid),
                                    CORE_STRING_STATIC("STRING_LIT")))
            return child;
    }
    return nullptr;
}

static void
import_error(core_translation_unit_t *unit, parse_tree_t *statement,
             const char *message)
{
    parse_token_info_t *token = parse_tree_find_first_terminal(statement);
    core_buffer_t *text = core_buffer_empty();
    if (token)
        core_buffer_printf(text, "%s at line %u, column %u", message,
                           token->line, token->column);
    else core_buffer_append(text, message, strlen(message));
    core_diagnostics_report(core_translation_unit_diagnostics(unit), unit,
        CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-PP0004"),
        core_string_borrow(text->data, text->byte_len));
    core_buffer_free(text);
}

static bool
reject_unsupported_import_locator(n00bb_resolver_t *resolver,
                                  core_translation_unit_t *unit,
                                  parse_tree_t *statement,
                                  const char *operand)
{
    core_string_t scheme = {0};
    if (n00bb_resolver_supports_locator(resolver, operand, &scheme))
        return false;
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "unsupported source locator scheme '%.*s'",
        (int)scheme.u8_bytes, scheme.data);
    import_error(unit, statement, message.data);
    core_buffer_release(&message);
    return true;
}

static bool
import_request_details(n00bb_resolver_t *resolver, parse_grammar_t *grammar,
                      parse_tree_t *statement,
                      core_translation_unit_t *importer,
                      core_module_spec_t *module, char **operand_out,
                      char **source_name_out)
{
    parse_tree_t *member = parse_tree_find_flat_child_by_nt_name(
        grammar, statement, CORE_STRING_STATIC("member-chain"));
    core_buffer_t *name = core_buffer_empty();
    bool has_name = false;
    append_import_names(grammar, member, name, &has_name);
    if (!has_name) {
        core_buffer_free(name);
        import_error(importer, statement, "invalid imported module name");
        return false;
    }
    parse_tree_t *path_token = find_string_token(grammar, statement);
    core_string_t operand = core_string_borrow(name->data, name->byte_len);
    if (path_token) {
        core_string_t raw = parse_tree_leaf_text(path_token);
        if (raw.u8_bytes < 2 || raw.data[0] != '"'
            || raw.data[raw.u8_bytes - 1] != '"') {
            core_buffer_free(name);
            import_error(importer, statement, "invalid imported module path");
            return false;
        }
        operand = core_string_borrow(raw.data + 1, raw.u8_bytes - 2);
    }
    core_string_t source_name = core_string_borrow(name->data, name->byte_len);
    core_string_t package_namespace = n00bb_package_namespace(importer);
    if (!path_token && package_namespace.u8_bytes) {
        size_t root_len = package_namespace.u8_bytes;
        if (source_name.u8_bytes >= root_len
            && !memcmp(source_name.data, package_namespace.data, root_len)
            && (source_name.u8_bytes == root_len
                || source_name.data[root_len] == '.')) {
            core_buffer_free(name);
            import_error(importer, statement,
                "a package cannot import its own external namespace; use its internal module name");
            return false;
        }
    }
    bool internal = !path_token && n00bb_resolver_internal_import(
        resolver, importer, source_name, module);
    bool initialized = internal || core_module_spec_init(module, source_name);
    if (initialized) {
        *operand_out = internal
            ? core_strndup(module->identifier.data, module->identifier.u8_bytes)
            : core_strndup(operand.data, operand.u8_bytes);
        *source_name_out = core_strndup(source_name.data, source_name.u8_bytes);
    }
    core_buffer_free(name);
    if (!initialized || !*operand_out || !*source_name_out) {
        import_error(importer, statement, "cannot form imported module identity");
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
    char *operand = nullptr;
    char *source_name = nullptr;
    if (!import_request_details(driver->resolver, grammar, statement, importer,
                                &module, &operand, &source_name)) return false;
    if (reject_unsupported_import_locator(driver->resolver, importer,
                                          statement, operand)) {
        core_free(operand);
        core_free(source_name);
        core_module_spec_release(&module);
        return false;
    }
    core_work_job_t *job = nullptr;
    bool scheduled = add_locate_request_for_module(driver->queue,
        driver->requests, driver->requests_lock, &module, operand, false,
        true, &job);
    core_free(operand);
    core_free(source_name);
    core_module_spec_release(&module);
    if (!scheduled)
        import_error(importer, statement, "cannot schedule imported module");
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
load_interface_imports(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit);
static bool
process_postprune_package_syntax(import_driver_t *driver,
                                 parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *tree);

static bool
ensure_preprocessed_import(import_driver_t *driver, parse_grammar_t *grammar,
                           core_work_job_t *locate_job,
                           core_translation_unit_t *unit, parse_tree_t *tree)
{
    core_work_job_t *dependencies[] = {locate_job};
    core_work_job_t *preprocess_job = nullptr;
    core_work_submit_result_t submitted = core_work_submit(driver->queue,
        N00BB_JOB_PREPROCESS, core_work_job_module(locate_job), 0, dependencies,
        1, &preprocess_job);
    if (submitted == CORE_WORK_INVALID
        || submitted == CORE_WORK_DEPENDENCY_FAILED) return false;
    if (submitted == CORE_WORK_CACHED)
        return core_work_job_state(driver->queue, preprocess_job)
            != CORE_WORK_FAILED;
    core_work_job_t *ready = nullptr;
    if (!core_work_take_kind(driver->queue, N00BB_JOB_PREPROCESS, &ready)
        || ready != preprocess_job) return false;
    bool ok = n00bb_preprocessor_run(driver->preprocessor, grammar, tree, unit);
    if (ok) ok = process_postprune_package_syntax(driver, grammar, unit, tree);
    n00bb_tu_state_t *state = tu_state(unit);
    if (ok && state && state->interface_module)
        ok = load_interface_imports(driver, grammar, unit);
    return core_work_complete_tu(driver->queue, preprocess_job, unit, ok)
        && core_work_job_state(driver->queue, preprocess_job)
            == CORE_WORK_SUCCEEDED;
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
        core_module_spec_t *candidate = &candidates.modules.data[i];
        char *operand = core_strndup(candidate->identifier.data,
                                     candidate->identifier.u8_bytes);
        core_work_job_t *job = nullptr;
        bool scheduled = add_locate_request_for_module(driver->queue,
            driver->requests, driver->requests_lock, candidate, operand,
            false, true, &job);
        core_free(operand);
        if (!scheduled || !wait_for_locate_job(driver, job)) continue;
        core_translation_unit_t *unit = core_work_job_result(driver->queue,
                                                              job);
        n00bb_tu_state_t *state = tu_state(unit);
        n00bb_lex_result_t *lex = state ? state->lex : nullptr;
        parse_tree_t *tree = lex && lex->parser
            ? parse_pwz_get_tree(lex->parser) : nullptr;
        if (unit && tree) {
            core_buffer_t names = {0};
            if (package_tree_syntax_index(driver->grammar, tree, &names))
                n00bb_resolver_store_package_syntax(driver->resolver,
                    core_source_file_path(&unit->source), names.data,
                    names.byte_len);
            core_buffer_release(&names);
        }
        if (!unit || !tree
            || !package_tree_may_declare(driver->grammar, tree,
                                         logical->identifier)
            || !ensure_preprocessed_import(driver, driver->grammar,
                                           job, unit, tree)
            || !find_package_declaration(unit, logical->identifier))
            continue;
        if (selected && selected != unit) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "package interface '%.*s' has multiple declarations",
                (int)logical->identifier.u8_bytes,
                logical->identifier.data);
            import_error(importer, statement, message.data);
            core_buffer_release(&message);
            selected_job = nullptr;
            selected = nullptr;
            status = CORE_MODULE_RESOLVE_ERROR;
            break;
        }
        selected = unit;
        selected_job = job;
    }
    if (selected && record_alias
        && !record_import_alias(importer, logical->identifier, selected)) {
        import_error(importer, statement,
                     "cannot retain package interface import");
        selected_job = nullptr;
    }
    for (size_t i = 0; i < candidates.modules.len; i++)
        core_module_spec_release(&candidates.modules.data[i]);
    core_list_free(candidates.modules);
    return selected_job;
}

static bool
load_parent_package_dependencies(import_driver_t *driver,
                                 core_translation_unit_t *importer,
                                 parse_tree_t *statement,
                                 core_translation_unit_t *child_unit,
                                 core_string_t child_name)
{
    for (size_t end = 0; end < child_name.u8_bytes; end++) {
        if (child_name.data[end] != '.') continue;
        core_string_t parent_name = core_string_borrow(child_name.data, end);
        n00bb_package_declaration_t *parent =
            find_package_declaration(child_unit, parent_name);
        core_translation_unit_t *parent_unit = child_unit;
        if (!parent) {
            core_module_spec_t module = {0};
            bool initialized = core_module_spec_init(&module, parent_name);
            core_work_job_t *job = initialized
                ? discover_package_import(driver, importer, statement,
                                          &module, false) : nullptr;
            if (initialized) core_module_spec_release(&module);
            parent_unit = job ? core_work_job_result(driver->queue, job)
                              : nullptr;
            parent = parent_unit
                ? find_package_declaration(parent_unit, parent_name)
                : nullptr;
            if (!parent) {
                import_error(importer, statement,
                    "a public child package requires an ancestor manifest");
                return false;
            }
        }
        size_t next = end + 1;
        while (next < child_name.u8_bytes && child_name.data[next] != '.')
            next++;
        core_string_t member = core_string_borrow(child_name.data + end + 1,
                                                   next - end - 1);
        bool exported = false;
        for (size_t i = 0; i < parent->exports.len; i++) {
            n00bb_package_export_t *item = &parent->exports.data[i];
            if (item->subpackage
                && core_string_eq(item->export_name, member)) {
                exported = true;
                break;
            }
        }
        if (!exported) {
            import_error(importer, statement,
                "a public child package must be exported by its parent manifest");
            return false;
        }
        if (parent_unit != child_unit
            && !core_translation_unit_add_import(child_unit, parent_unit)) {
            import_error(importer, statement,
                "cannot retain parent package dependency");
            return false;
        }
    }
    return true;
}

static bool
load_interface_imports(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit);

static bool
load_import_module(import_driver_t *driver, parse_grammar_t *grammar,
                   parse_tree_t *statement, core_translation_unit_t *importer,
                   core_module_spec_t *module, const char *operand,
                   const char *source_name,
                   core_translation_unit_t **imported_out,
                   parse_tree_t **tree_out)
{
    if (reject_unsupported_import_locator(driver->resolver, importer,
                                          statement, operand)) {
        return false;
    }
    core_work_job_t *locate_job = nullptr;
    bool scheduled = add_locate_request_for_module(driver->queue,
        driver->requests, driver->requests_lock, module, operand,
        false, true, &locate_job);
    if (!scheduled) {
        import_error(importer, statement, "cannot schedule imported module");
        return false;
    }

    bool located = wait_for_locate_job(driver, locate_job);
    core_translation_unit_t *direct = located
        ? core_work_job_result(driver->queue, locate_job) : nullptr;
    if (!direct || (statement && !n00bb_same_package(importer, direct))) {
        core_work_job_t *package_job = discover_package_import(
            driver, importer, statement, module, true);
        if (package_job) locate_job = package_job;
        else if (!located) locate_job = nullptr;
    }
    if (!locate_job) {
        import_error(importer, statement, "imported module failed to load");
        return false;
    }
    core_translation_unit_t *imported = core_work_job_result(
        driver->queue, locate_job);
    n00bb_tu_state_t *state = tu_state(imported);
    n00bb_lex_result_t *lex = state ? state->lex : nullptr;
    parse_tree_t *tree = lex && lex->parser
        ? parse_pwz_get_tree(lex->parser) : nullptr;
    if (!imported) {
        import_error(importer, statement, "imported module has no translation unit");
        return false;
    }
    if (!tree) {
        import_error(importer, statement, "imported module has no parse tree");
        return false;
    }
    core_string_t imported_name = module->identifier;
    if (statement && source_name && strcmp(source_name, operand)
        && n00bb_same_package(importer, imported)
        && !record_import_alias(importer,
            core_string_from_cstr(source_name), imported)) {
        import_error(importer, statement,
                     "cannot retain internal module import");
        return false;
    }
    if (!core_translation_unit_add_import(importer, imported)) {
        import_error(importer, statement, "cannot retain imported module");
        return false;
    }

    if (!ensure_preprocessed_import(driver, grammar, locate_job,
                                    imported, tree)) {
        import_error(importer, statement,
                     "imported module failed preprocessing");
        return false;
    }
    if (statement && n00bb_same_package(importer, imported)
        && find_package_declaration(imported,
            core_string_from_cstr(source_name ? source_name : ""))) {
        import_error(importer, statement,
            "a package cannot import its own external namespace; use its internal module name");
        return false;
    }
    if (statement && !state->interface_module
        && !n00bb_same_package(importer, imported)
        && !find_package_declaration(imported, imported_name)) {
        import_error(importer, statement,
            "a library source module cannot be imported from another source package; import its package manifest");
        return false;
    }
    if (find_package_declaration(imported, imported_name)
        && !load_parent_package_dependencies(driver, importer, statement,
                                              imported, imported_name))
        return false;

    *imported_out = imported;
    *tree_out = tree;
    return true;
}

static bool
append_dotted_reference(parse_grammar_t *grammar, parse_tree_t *node,
                        core_buffer_t *name)
{
    if (!node) return false;
    if (core_tree_is_leaf(node)) {
        parse_token_info_t *token = parse_node_token(node);
        if (!token || !core_string_eq(parse_terminal_name(grammar,
                token->tid), CORE_STRING_STATIC("IDENTIFIER"))) return false;
        core_string_t text = parse_tree_leaf_text(node);
        core_buffer_append(name, text.data, text.u8_bytes);
        return true;
    }
    size_t count = core_tree_num_children(node);
    if (count == 1)
        return append_dotted_reference(grammar, core_tree_child(node, 0),
                                       name);
    parse_token_info_t *separator = count == 3
        ? parse_node_token(core_tree_child(node, 1)) : nullptr;
    if (!separator || separator->tid != '.') return false;
    if (!append_dotted_reference(grammar, core_tree_child(node, 0), name))
        return false;
    core_buffer_putc(name, '.');
    return append_dotted_reference(grammar, core_tree_child(node, 2), name);
}

static bool
load_package_reference(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit, parse_tree_t *node,
                       core_string_t path)
{
    for (size_t end = 0; end < path.u8_bytes; end++) {
        if (path.data[end] != '.') continue;
        size_t child_end = end + 1;
        while (child_end < path.u8_bytes && path.data[child_end] != '.')
            child_end++;
        core_string_t parent_name = core_string_borrow(path.data, end);
        core_translation_unit_t *parent_unit = find_direct_import(unit,
                                                                   parent_name);
        n00bb_package_declaration_t *parent = parent_unit
            ? find_package_declaration(parent_unit, parent_name) : nullptr;
        if (!parent) continue;
        core_string_t child_member = core_string_borrow(path.data + end + 1,
                                                        child_end - end - 1);
        bool exported = false;
        for (size_t i = 0; i < parent->exports.len; i++) {
            n00bb_package_export_t *item = &parent->exports.data[i];
            if (item->subpackage
                && core_string_eq(item->export_name, child_member)) {
                exported = true;
                break;
            }
        }
        if (!exported) continue;
        core_string_t child_name = core_string_borrow(path.data, child_end);
        if (find_direct_import(unit, child_name)) continue;
        core_module_spec_t module = {0};
        char *operand = core_strndup(child_name.data, child_name.u8_bytes);
        core_translation_unit_t *imported = nullptr;
        parse_tree_t *imported_tree = nullptr;
        bool loaded = operand && core_module_spec_init(&module, child_name)
            && load_import_module(driver, grammar, node, unit, &module,
                                  operand, operand, &imported,
                                  &imported_tree);
        core_module_spec_release(&module);
        core_free(operand);
        if (!loaded) return false;
    }
    return true;
}

static bool
load_referenced_subpackages(import_driver_t *driver,
                            parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            core_list_t(parse_tree_ptr_t) *references)
{
    n00bb_tu_state_t *state = tu_state(unit);
    bool has_parent = false;
    for (size_t i = 0; state && i < state->import_aliases.len; i++) {
        n00bb_import_alias_t *alias = &state->import_aliases.data[i];
        n00bb_package_declaration_t *package =
            find_package_declaration(alias->unit, alias->name);
        for (size_t e = 0; package && e < package->exports.len; e++)
            if (package->exports.data[e].subpackage) {
                has_parent = true;
                break;
            }
        if (has_parent) break;
    }
    if (!has_parent) return true;
    for (size_t i = 0; i < references->len; i++) {
        parse_tree_t *node = references->data[i];
        core_buffer_t name = {0};
        bool dotted = append_dotted_reference(grammar, node, &name)
            && memchr(name.data, '.', name.byte_len) != nullptr;
        bool ok = !dotted || load_package_reference(driver, grammar, unit,
            node, core_string_borrow(name.data, name.byte_len));
        core_buffer_release(&name);
        if (!ok) return false;
    }
    return true;
}

static void
collect_postprune_package_syntax(parse_grammar_t *grammar,
    parse_tree_t *node, bool inside_package,
    core_list_t(parse_tree_ptr_t) *declarations,
    core_list_t(parse_tree_ptr_t) *exports,
    core_list_t(parse_tree_ptr_t) *references)
{
    if (!node || core_tree_is_leaf(node)) return;
    bool package = nt_is(grammar, node, "package-decl");
    if (package && !inside_package) core_list_push(*declarations, node);
    if (nt_is(grammar, node, "export-stmt")) core_list_push(*exports, node);
    for (size_t i = 0; i < core_tree_num_children(node); i++)
        collect_postprune_package_syntax(grammar, core_tree_child(node, i),
            inside_package || package, declarations, exports, references);
    if (nt_is(grammar, node, "postfix-expr")
        || nt_is(grammar, node, "member-chain")
        || nt_is(grammar, node, "dotted-name"))
        core_list_push(*references, node);
}

static bool
process_postprune_package_syntax(import_driver_t *driver,
                                 parse_grammar_t *grammar,
                                 core_translation_unit_t *unit,
                                 parse_tree_t *tree)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !tree) return false;
    core_list_t(parse_tree_ptr_t) declarations = core_list_new(parse_tree_ptr_t);
    core_list_t(parse_tree_ptr_t) exports = core_list_new(parse_tree_ptr_t);
    core_list_t(parse_tree_ptr_t) references = core_list_new(parse_tree_ptr_t);
    collect_postprune_package_syntax(grammar, tree, false, &declarations,
                                     &exports, &references);
    for (size_t i = 0; i < state->package_declarations.len; i++)
        core_list_free(state->package_declarations.data[i].exports);
    state->package_declarations.len = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < declarations.len; i++)
        ok = cache_package_declaration_syntax(grammar, unit,
                                               declarations.data[i]);
    for (size_t i = 0; ok && i < exports.len; i++)
        ok = cache_package_export_syntax(grammar, unit, exports.data[i]);
    if (ok && !state->interface_module)
        ok = load_referenced_subpackages(driver, grammar, unit, &references);
    core_list_free(references);
    core_list_free(exports);
    core_list_free(declarations);
    return ok;
}

static bool
load_interface_imports(import_driver_t *driver, parse_grammar_t *grammar,
                       core_translation_unit_t *unit)
{
    core_string_t module = unit->module.identifier;
    char *module_name = core_strndup(module.data, module.u8_bytes);
    size_t size = 0;
    const uint8_t *data = module_name
        ? interface_owned_metadata(&driver->loader->interfaces,
            TH_ENV_METADATA_ENVIRONMENT, module_name,
            "n00b.module-imports", 1, &size)
        : nullptr;
    bool package_only = !data && module_name
        && !interface_owned_metadata(&driver->loader->interfaces,
            TH_ENV_METADATA_ENVIRONMENT, module_name,
            "n00b.module-interface", 1, nullptr);
    core_free(module_name);
    if (package_only) return true;
    if (!data || size < 4) {
        import_error(unit, nullptr, "interface module has no import graph");
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
        char *operand = core_strndup(name.data, name.u8_bytes);
        core_translation_unit_t *imported = nullptr;
        parse_tree_t *tree = nullptr;
        bool ok = operand && core_module_spec_init(&spec, name)
            && load_import_module(driver, grammar, nullptr, unit, &spec,
                                  operand, nullptr, &imported, &tree);
        core_module_spec_release(&spec);
        core_free(operand);
        if (!ok) return false;
        offset += length;
        if (i + 1 == count && offset == size) return true;
    }
    if (!count && offset == size) return true;
    import_error(unit, nullptr, "interface module has malformed import graph");
    return false;
}

static n00bb_literal_origin_t
literal_origin(core_translation_unit_t *unit, parse_token_info_t *token)
{
    const char *contents = core_source_file_contents(&unit->source);
    uint32_t line = token->line;
    uint32_t column = token->column;
    for (size_t at = token->start_byte; at < token->content_start_byte; at++) {
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
        .byte = token->content_start_byte,
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
    const char *contents = core_source_file_contents(&importer->source);
    core_string_t text = core_string_borrow(contents + token->content_start_byte,
        token->content_end_byte - token->content_start_byte);
    core_buffer_t identifier = {0};
    module_literal_identifier(&identifier, importer->module.identifier,
                              modifier, name);
    core_string_t id = core_string_borrow(identifier.data, identifier.byte_len);
    core_buffer_t source = {0};
    n00bb_native_declarations_t natives = {0};
    if (!generator->generate(generator->context, importer, name, id,
            literal_origin(importer, token), text, &source, &natives)) {
        n00bb_native_declarations_release(&natives);
        core_buffer_release(&source);
        core_buffer_release(&identifier);
        return false;
    }
    core_module_spec_t module = {0};
    bool ok = n00bb_resolver_add_generated(driver->resolver, id,
                  core_string_borrow(source.data ? source.data : "", source.byte_len),
                  importer, natives)
        && core_module_spec_init(&module, id)
        && load_import_module(driver, grammar, literal, importer, &module,
               identifier.data, nullptr, module_out, tree_out)
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
    char *operand = nullptr;
    char *source_name = nullptr;
    if (!import_request_details(driver->resolver, grammar, statement, importer,
                                &module, &operand, &source_name)) return false;
    bool loaded = load_import_module(driver, grammar, statement, importer,
        &module, operand, source_name, imported_out, tree_out);
    core_free(operand);
    core_free(source_name);
    core_module_spec_release(&module);
    return loaded;
}

static bool
load_implicit_builtin(void *opaque, parse_grammar_t *grammar,
                      core_translation_unit_t *importer,
                      core_string_t module_name)
{
    core_string_t name = importer->module.identifier;
    if (core_string_eq(name, module_name)) return true;
    if (name.u8_bytes >= sizeof("builtin.") - 1
        && !memcmp(name.data, "builtin.", sizeof("builtin.") - 1))
        return true;
    import_driver_t *driver = opaque;
    core_module_spec_t module = {0};
    if (!core_module_spec_init(&module, module_name)) return false;
    char *operand = core_strndup(module_name.data, module_name.u8_bytes);
    if (!operand) {
        core_module_spec_release(&module);
        return false;
    }
    core_translation_unit_t *imported = nullptr;
    parse_tree_t *tree = nullptr;
    bool loaded = load_import_module(driver, grammar, nullptr, importer,
        &module, operand, nullptr, &imported, &tree);
    core_free(operand);
    core_module_spec_release(&module);
    return loaded;
}



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
                size_t linked_interface_count,
                const n00bb_preprocessor_options_t *preprocessor_options,
                ir_ctx_t *ir_context, th_ctx *types, size_t worker_limit,
                th_env **environment_out, core_link_unit_t **link_out,
                n00bb_link_analysis_t **analysis_out)
{
    bool profile_compile = getenv("N00BB_PROFILE_COMPILE") != nullptr;
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
        },
        .lex = lex,
        .parse = parse,
    };
    pthread_t *worker_threads = worker_limit
        ? core_alloc_array(pthread_t, worker_limit) : nullptr;
    size_t worker_count = 0;
    for (; worker_count < worker_limit; worker_count++)
        if (pthread_create(&worker_threads[worker_count], nullptr,
                           locate_worker, &loader) != 0)
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
    active_options.import_context = &import_driver;
    active_options.prepare_import = prepare_import;
    active_options.selected_body = n00bb_tu_remember_selected_when_body;
    active_options.selected_body_context = nullptr;
    active_options.register_litmod = register_user_litmod;
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
            const char *operand = request->operand;
            core_module_resolve_status_t status = request->resolve_attempted
                ? request->resolve_status : CORE_MODULE_RESOLVE_ERROR;
            core_string_t searched = n00bb_resolver_search_description(
                resolver, operand);
            core_buffer_t message = {0};
            core_buffer_printf(&message, "%s '%s'",
                status == CORE_MODULE_NOT_FOUND ? "cannot locate" : "cannot read",
                operand);
            if (searched.data) core_buffer_printf(&message, "\n%s", searched.data);
            core_diagnostics_report(diags, nullptr, CORE_DIAG_ERROR,
                status == CORE_MODULE_NOT_FOUND
                    ? CORE_STRING_STATIC("N00BB-B0001")
                    : CORE_STRING_STATIC("N00BB-B0002"),
                core_string_from_cstr(message.data));
            core_buffer_release(&message);
            core_free(searched.data);
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
                    job_ok = process_postprune_package_syntax(&import_driver,
                        grammar, unit, tree);
                if (job_ok && native_build && !runtime_support_loaded) {
                    job_ok = load_implicit_builtin(&import_driver, grammar,
                        unit, CORE_STRING_STATIC("sys.gc.alloc"));
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
            core_link_unit_t *link = core_link_unit_new();
            n00bb_link_analysis_t *analysis = n00bb_link_analysis_new(
                grammar, ir_context, linked_interfaces,
                linked_interface_count, native_roots, native_root_count,
                may_link_further, resolver);
            success = link != nullptr;
            for (size_t i = 0; i < root_request_count; i++) {
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
            success = stages_registered
                && core_link_unit_run_stage(link,
                    N00BB_LINK_STAGE_CONTRIBUTIONS)
                && !core_link_unit_has_fatal_diagnostics(link);
            if (profile_compile) {
                double now = n00bb_profile_seconds();
                fprintf(stderr, "PROFILE link-stages %.3f\n",
                    now - profile_tick);
                profile_tick = now;
            }
            if (environment_out) *environment_out = analysis->environment;
            if (link_out) *link_out = link;
            else core_link_unit_free(link);
            if (analysis_out) *analysis_out = analysis;
            else n00bb_link_analysis_free(analysis);
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
        if (unit)
            core_diagnostics_merge(diags,
                core_translation_unit_diagnostics(unit));
    }

    n00bb_preprocessor_free(preprocessor);
    bool diagnostics_have_outer_owner = defer_diagnostics
        && analysis_out && *analysis_out;
    if (!diagnostics_have_outer_owner && core_diagnostics_count(diags)) {
        fflush(stdout);
        char *text = core_diagnostics_render_text(diags);
        if (text) { fputs(text, stderr); core_free(text); }
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
        core_free(requests->ordered.data[i].operand);
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
n00bb_format_write(const char *path, const char *text, size_t length)
{
    struct stat info;
    mode_t mode = 0644;
    if (!stat(path, &info)) {
        if (!S_ISREG(info.st_mode)) return false;
        mode = info.st_mode & 07777;
    }
    else if (errno != ENOENT) return false;
    size_t path_len = strlen(path);
    char *temporary = core_alloc_array(char, path_len + 16);
    snprintf(temporary, path_len + 16, "%s.fmt.XXXXXX", path);
    int fd = mkstemp(temporary);
    bool ok = fd >= 0 && !fchmod(fd, mode);
    for (size_t written = 0; ok && written < length;) {
        ssize_t count = write(fd, text + written, length - written);
        if (count <= 0) ok = false;
        else written += (size_t)count;
    }
    if (fd >= 0 && close(fd)) ok = false;
    if (ok) ok = !rename(temporary, path);
    if (!ok && fd >= 0) unlink(temporary);
    core_free(temporary);
    return ok;
}

static bool
n00bb_format_layout_token(parse_token_info_t *token, int64_t newline,
                          int64_t indent, int64_t dedent)
{
    return !token || token->tid == PARSE_TOK_EOF
        || token->tid == newline || token->tid == indent
        || token->tid == dedent;
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

static char *
n00bb_format_restore_breaks(parse_grammar_t *grammar,
                            core_string_t source, const char *printed)
{
    size_t printed_len = strlen(printed);
    parse_token_stream_t *original = scan_tokenize_owned(grammar,
        source.data, source.u8_bytes);
    parse_token_stream_t *rendered = scan_tokenize_owned(grammar,
        printed, printed_len);
    if (!original || !rendered) {
        scan_token_stream_free(original);
        scan_token_stream_free(rendered);
        return nullptr;
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
        size_t from_len = from->end_byte - from->start_byte;
        size_t to_len = to->end_byte - to->start_byte;
        if (from->tid != to->tid || from_len != to_len
            || from->end_byte > source.u8_bytes
            || to->end_byte > printed_len
            || memcmp(source.data + from->start_byte,
                      printed + to->start_byte, from_len)) {
            valid = false;
            break;
        }
        size_t gap_start = prior_rendered ? prior_rendered->end_byte : 0;
        size_t gap_end = to->start_byte;
        if (gap_end < gap_start) { valid = false; break; }
        size_t gap_len = gap_end - gap_start;
        size_t printed_lines = n00bb_format_newline_count(
            printed + gap_start, gap_len);
        size_t source_lines = prior_original
            ? n00bb_format_newline_count(
                source.data + prior_original->end_byte,
                from->start_byte - prior_original->end_byte) : 0;
        bool has_comment = false;
        for (size_t j = 0; j < gap_len; j++)
            if (printed[gap_start + j] == '/' || printed[gap_start + j] == '#')
                has_comment = true;
        bool comma = to_len == 1 && printed[to->start_byte] == ',';
        bool aligned_colon = prior_original && to_len == 1
            && printed[to->start_byte] == ':'
            && source_lines == 0 && printed_lines == 0;
        bool channel_break = (to_len == 2
                              && !memcmp(printed + to->start_byte, "->", 2))
            || (to_len == 4
                && !memcmp(printed + to->start_byte, "fail", 4))
            || (prior_original
                && prior_original->end_byte - prior_original->start_byte == 2
                && !memcmp(source.data + prior_original->start_byte,
                           "->", 2));
        if (aligned_colon && from->start_byte >= prior_original->end_byte) {
            size_t original_gap = from->start_byte
                - prior_original->end_byte;
            bool spaces_only = original_gap > gap_len;
            for (size_t j = 0; j < original_gap && spaces_only; j++)
                spaces_only = source.data[prior_original->end_byte + j] == ' ';
            if (spaces_only)
                core_buffer_append(&result,
                    source.data + prior_original->end_byte, original_gap);
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
                size_t j = from->start_byte;
                while (j > 0 && source.data[j - 1] == ' ') {
                    spaces++;
                    j--;
                }
            }
            for (size_t j = 0; j < spaces; j++)
                core_buffer_putc(&result, ' ');
        }
        else core_buffer_append(&result, printed + gap_start, gap_len);
        core_buffer_append(&result, printed + to->start_byte, to_len);
        prior_original = from;
        prior_rendered = to;
    }
    if (valid) {
        size_t suffix = prior_rendered ? prior_rendered->end_byte : 0;
        core_buffer_append(&result, printed + suffix, printed_len - suffix);
        while (result.byte_len && result.data[result.byte_len - 1] == '\n')
            result.data[--result.byte_len] = '\0';
        core_buffer_putc(&result, '\n');
    }
    scan_token_stream_free(original);
    scan_token_stream_free(rendered);
    if (!valid) {
        core_buffer_release(&result);
        return nullptr;
    }
    return result.data;
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

static bool
n00bb_format_word(const char *text, size_t length, size_t *at,
                  const char *word)
{
    size_t count = strlen(word);
    if (*at + count > length || memcmp(text + *at, word, count))
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
            n00bb_format_word(line->start, line->length, &name, "public");
            if (name == at)
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
        n00bb_format_word(line->start, line->length, &name, "public");
        if (name == at)
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

static char *
n00bb_format_align(const char *text)
{
    size_t length = strlen(text), count = 0;
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
            n00bb_format_word(line->start, line->length, &at, "public");
            if (!at)
                n00bb_format_word(line->start, line->length, &at,
                                  "private");
            if (n00bb_format_word(line->start, line->length, &at, "record"))
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
    return result.data;
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
                              line->end - line->start, &at, "public")
            || n00bb_format_word(text + line->start,
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
n00bb_signature_close(const char *text, size_t length, size_t open)
{
    size_t depth = 1;
    char quote = 0;
    for (size_t at = open + 1; at < length; at++) {
        char ch = text[at];
        if (quote) {
            if (ch == '\\' && at + 1 < length) at++;
            else if (ch == quote) quote = 0;
        }
        else if (ch == '"' || ch == '\'') quote = ch;
        else if (ch == '(') depth++;
        else if (ch == ')' && !--depth) return at;
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

static char *
n00bb_format_signatures(const char *text)
{
    size_t length = strlen(text);
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
            : n00bb_signature_close(text, length, open);
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
    return result.data;
}

static char *
n00bb_format_channels(const char *text)
{
    core_buffer_t result = {0};
    size_t length = strlen(text), start = 0, paren_column = 0;
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
            signature_close = n00bb_signature_close(text, length, open);
        }
        if (line_length > 88 && signature_close >= start
            && signature_close < i) {
            size_t channel_start = signature_close + 1;
            while (channel_start < i && text[channel_start] == ' ')
                channel_start++;
            bool has_channel = channel_start + 1 < i
                && text[channel_start] == '-'
                && text[channel_start + 1] == '>';
            has_channel = has_channel || (channel_start + 4 < i
                && !memcmp(text + channel_start, "fail:", 5));
            if (has_channel) {
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
            bool previous_has_channel = prior_len
                && strstr(result.data + prior_start, "-> ")
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
    return result.data;
}

static parse_pwz_parser_t *
n00bb_format_parse(parse_grammar_t *grammar, const char *text,
                   parse_token_stream_t **tokens_out)
{
    parse_token_stream_t *tokens = scan_tokenize_owned(grammar,
        text, strlen(text));
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
n00bb_format_parses(parse_grammar_t *grammar, const char *text)
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
    char *type;
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
        in_type = in_type || nt_is(grammar, node, "type-spec")
            || nt_is(grammar, node, "one-tspec");
        for (size_t i = 0; i < core_tree_num_children(node); i++)
            n00bb_format_type_names(grammar, core_tree_child(node, i),
                source, insertions, used, capacity, in_type);
        return;
    }
    if (!in_type || *used == capacity) return;
    parse_token_info_t *token = parse_node_token(node);
    if (!token || token->end_byte > source.u8_bytes
        || (token->start_byte && source.data[token->start_byte - 1] == '.'))
        return;
    size_t length = token->end_byte - token->start_byte;
    const char *replacement = nullptr;
    if (length == 3 && !memcmp(source.data + token->start_byte, "u64", 3))
        replacement = "uint";
    else if (length == 3
             && !memcmp(source.data + token->start_byte, "i64", 3))
        replacement = "int";
    if (replacement)
        insertions[(*used)++] = (n00bb_format_type_insertion_t){
            .at = token->start_byte,
            .end = token->end_byte,
            .type = core_strdup(replacement),
        };
}

static char *
n00bb_format_canonical_type(const char *type)
{
    core_buffer_t result = {0};
    size_t length = strlen(type);
    for (size_t i = 0; i < length;) {
        bool left = i == 0 || !(type[i - 1] == '.'
            || type[i - 1] == '_'
            || (type[i - 1] >= 'a' && type[i - 1] <= 'z')
            || (type[i - 1] >= 'A' && type[i - 1] <= 'Z')
            || (type[i - 1] >= '0' && type[i - 1] <= '9'));
        bool right = i + 3 <= length && !(type[i + 3] == '_'
            || (type[i + 3] >= 'a' && type[i + 3] <= 'z')
            || (type[i + 3] >= 'A' && type[i + 3] <= 'Z')
            || (type[i + 3] >= '0' && type[i + 3] <= '9'));
        if (left && right && !strncmp(type + i, "u64", 3)) {
            core_buffer_puts(&result, "uint");
            i += 3;
        }
        else if (left && right && !strncmp(type + i, "i64", 3)) {
            core_buffer_puts(&result, "int");
            i += 3;
        }
        else core_buffer_putc(&result, type[i++]);
    }
    return result.data;
}

static char *
n00bb_format_inferred_types(parse_grammar_t *grammar,
                            n00bb_tu_state_t *state, core_string_t source)
{
    parse_symtab_t *symtab = state->annotations
        ? state->annotations->symtab : nullptr;
    if (!symtab) return core_strndup(source.data, source.u8_bytes);
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
                || !nt_is(grammar, symbol->decl_node, "sym-decl")
                || child_nt(grammar, symbol->decl_node, "type-spec")
                || !core_tree_num_children(symbol->decl_node))
                continue;
            parse_tree_t *name = core_tree_child(symbol->decl_node, 0);
            parse_token_info_t *token = core_tree_is_leaf(name)
                ? parse_node_token(name) : nullptr;
            if (!token || token->end_byte <= token->start_byte
                || token->end_byte > source.u8_bytes
                || token->end_byte - token->start_byte
                    != symbol->name.u8_bytes
                || memcmp(source.data + token->start_byte,
                          symbol->name.data, symbol->name.u8_bytes))
                continue;
            const char *printed = n00bb_print_type(state->types,
                                                   symbol->type);
            if (!printed || !*printed || strchr(printed, '<')
                || strchr(printed, '$') || strchr(printed, '%'))
                continue;
            if (used == capacity) continue;
            insertions[used++] = (n00bb_format_type_insertion_t){
                .at = token->end_byte,
                .end = token->end_byte,
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
            core_buffer_puts(&result, insertions[i].type);
            cursor = insertions[i].end;
        }
        core_free(insertions[i].type);
    }
    core_buffer_append(&result, source.data + cursor,
                       source.u8_bytes - cursor);
    core_free(insertions);
    return result.data ? result.data
        : core_strndup(source.data, source.u8_bytes);
}

static bool
n00bb_format_units(parse_grammar_t *grammar, core_work_queue_t *queue,
                   locate_requests_t *requests, parse_cmdr_result_t *args)
{
    bool check = parse_cmdr_flag_present(args, CORE_STRING_STATIC("--check"));
    bool normalize = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--normalize"));
    core_string_t output = parse_cmdr_flag_str(args,
        CORE_STRING_STATIC("--output"));
    if (output.data && (check || parse_cmdr_arg_count(args) != 1)) {
        fprintf(stderr, "n00bb fmt: --output requires one input and no --check\n");
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
            core_source_file_contents(&unit->source),
            core_source_file_size(&unit->source));
        char *normalized = normalize
            ? n00bb_format_inferred_types(grammar, state, source) : nullptr;
        parse_token_stream_t *normalized_tokens = nullptr;
        parse_pwz_parser_t *normalized_parser = normalized
            ? n00bb_format_parse(grammar, normalized, &normalized_tokens)
            : nullptr;
        if (normalized && !normalized_parser) {
            fprintf(stderr, "n00bb fmt: cannot normalize %.*s\n",
                (int)path.u8_bytes, path.data);
            core_free(normalized);
            ok = false;
            continue;
        }
        core_string_t print_source = normalized
            ? core_string_borrow(normalized, strlen(normalized)) : source;
        if (normalized_parser) tree = parse_pwz_get_tree(normalized_parser);
        parse_print_opts_t options = {
            .source = print_source,
            .width = 88,
            .indent = 4,
        };
        char *printed = parse_tree_print(grammar, tree, &options);
        char *restored = printed
            ? n00bb_format_restore_breaks(grammar, print_source, printed)
            : nullptr;
        parse_pwz_free(normalized_parser);
        scan_token_stream_free(normalized_tokens);
        core_free(normalized);
        core_free(printed);
        char *aligned = restored ? n00bb_format_align(restored) : nullptr;
        core_free(restored);
        char *signatures = aligned ? n00bb_format_signatures(aligned) : nullptr;
        core_free(aligned);
        char *formatted = signatures
            ? n00bb_format_channels(signatures) : nullptr;
        core_free(signatures);
        if (!formatted || !n00bb_format_parses(grammar, formatted)) {
            fprintf(stderr, "n00bb fmt: cannot print %.*s\n",
                (int)path.u8_bytes, path.data);
            core_free(formatted);
            ok = false;
            continue;
        }
        size_t length = strlen(formatted);
        bool changed = length != source.u8_bytes
            || memcmp(formatted, source.data, length);
        if (changed || output.data) {
            if (check) {
                printf("%.*s\n", (int)path.u8_bytes, path.data);
                ok = false;
            }
            else {
                char *target = output.data
                    ? core_strndup(output.data, output.u8_bytes)
                    : core_strndup(path.data, path.u8_bytes);
                if (!n00bb_format_write(target, formatted, length)) {
                    fprintf(stderr, "n00bb fmt: cannot write %s\n", target);
                    ok = false;
                }
                core_free(target);
            }
        }
        core_free(formatted);
    }
    return ok;
}

int
n00bb_compiler_main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    parse_cmdr_t *cli = n00bb_make_cli();
    if (argc == 1 || n00bb_is_help_request(argc, argv)) {
        parse_cmdr_print_usage(cli, argc == 1 ? stderr : stdout);
        parse_cmdr_free(cli);
        return argc == 1 ? 2 : 0;
    }

    parse_cmdr_result_t *args =
        parse_cmdr_parse(cli, argc - 1, (const char **)argv + 1);
    if (!parse_cmdr_result_ok(args)) {
        for (int32_t i = 0; i < parse_cmdr_result_error_count(args); i++) {
            core_string_t error = parse_cmdr_result_error(args, i);
            fprintf(stderr, "n00bb: %.*s\n", (int)error.u8_bytes, error.data);
        }
        parse_cmdr_print_usage(cli, stderr);
        parse_cmdr_result_free(args);
        parse_cmdr_free(cli);
        return 2;
    }

    bool requested_native = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--native"));
    bool requested_generic = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--generic"));
    if (requested_native && requested_generic) {
        fprintf(stderr, "n00bb: --native and --generic are mutually exclusive\n");
        parse_cmdr_result_free(args);
        parse_cmdr_free(cli);
        return 2;
    }
#ifdef __APPLE__
    bool native_cpu = !requested_generic;
#else
    bool native_cpu = requested_native;
#endif

    core_string_t command = parse_cmdr_result_command(args);
    bool is_default = !command.data;
    if (is_default) command = CORE_STRING_STATIC("build");

    int32_t input_count = parse_cmdr_arg_count(args);
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
        char *operand = core_strndup(input.data, input.u8_bytes);
        scheduled = add_locate_request(queue, &requests, operand) && scheduled;
        core_free(operand);
    }

    n00bb_resolver_t *resolver = n00bb_resolver_new(argv[0]);
    if (!resolver) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0023"),
            CORE_STRING_STATIC("cannot determine the current directory"));
        scheduled = false;
    }
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
    bool lower_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("lower"));
    bool build_command = !show_ffi && (is_default
        || core_string_eq(command, CORE_STRING_STATIC("build")));
    bool library_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("library"));
    bool compile_command = !show_ffi
        && core_string_eq(command, CORE_STRING_STATIC("compile"));
    bool emit_interface = parse_cmdr_flag_present(args,
        CORE_STRING_STATIC("--emit-interface"));
    bool interface_ir_dump = emit_interface
        && (parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-ir"))
            || parse_cmdr_flag_present(args,
                                       CORE_STRING_STATIC("--dump-control-flow"))
            || parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-pdg"))
            || parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-layout")));
    core_string_t interface_output = emit_interface
        ? parse_cmdr_flag_str(args,
            CORE_STRING_STATIC("--emit-interface")) : (core_string_t){0};
    bool options_valid = true;
    if (!fmt_command && (parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--check"))
        || parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--normalize")))) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0003"),
            CORE_STRING_STATIC("--check and --normalize are only valid with the fmt command"));
        options_valid = false;
    }
    if (!build_command && parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--link-object"))) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0017"),
            CORE_STRING_STATIC("--link-object is only valid with the build command"));
        options_valid = false;
    }
    if (!build_command && parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--native-root"))) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0018"),
            CORE_STRING_STATIC("--native-root is only valid with the build command"));
        options_valid = false;
    }
    if (!build_command && parse_cmdr_flag_present(args,
            CORE_STRING_STATIC("--lto"))) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0017"),
            CORE_STRING_STATIC("--lto is only valid with the build command"));
        options_valid = false;
    }
    int32_t native_root_count = parse_cmdr_flag_value_count(args,
        CORE_STRING_STATIC("--native-root"));
    core_string_t *native_roots = native_root_count
        ? core_alloc_array(core_string_t, (size_t)native_root_count) : nullptr;
    for (int32_t i = 0; i < native_root_count; i++)
        native_roots[i] = parse_cmdr_flag_str_at(args,
            CORE_STRING_STATIC("--native-root"), i);
    if (emit_interface && !library_command) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0003"),
            CORE_STRING_STATIC("--emit-interface is only valid with the library command"));
        options_valid = false;
    }
    int32_t interface_count = parse_cmdr_flag_value_count(args,
        CORE_STRING_STATIC("--link-interface"));
    const th_env **linked_interfaces = interface_count
        ? core_alloc_array(const th_env *, (size_t)interface_count) : nullptr;
    for (int32_t i = 0; i < interface_count; i++) {
        core_string_t argument = parse_cmdr_flag_str_at(args,
            CORE_STRING_STATIC("--link-interface"), i);
        char *path = core_strndup(argument.data, argument.u8_bytes);
        core_object_archive_error_t archive_error =
            CORE_OBJECT_ARCHIVE_OK;
        linked_interfaces[i] = th_env_load(path, &archive_error);
        if (!linked_interfaces[i]) {
            core_buffer_t message = {0};
            core_buffer_printf(&message, "cannot load semantic interface '%s': %s",
                path, core_object_archive_error_string(archive_error));
            core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
                CORE_STRING_STATIC("N00BB-B0004"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            options_valid = false;
        }
        core_free(path);
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
    int64_t requested_jobs = parse_cmdr_flag_int(
        args, CORE_STRING_STATIC("--jobs"), 4);
    if (requested_jobs < 0 || requested_jobs > 128) {
        fprintf(stderr, "n00bb: --jobs must be between 0 and 128\n");
        scheduled = false;
    }
    int32_t define_count = parse_cmdr_flag_value_count(
        args, CORE_STRING_STATIC("--define"));
    const char **definitions = define_count
        ? core_alloc_array(const char *, (size_t)define_count) : nullptr;
    for (int32_t i = 0; i < define_count; i++)
        definitions[i] = parse_cmdr_flag_str_at(
            args, CORE_STRING_STATIC("--define"), i).data;
    n00bb_ffi_t *ffi = n00bb_ffi_new();
    int32_t contract_dir_count = parse_cmdr_flag_value_count(
        args, CORE_STRING_STATIC("--contract-dir"));
    for (int32_t i = 0; i < contract_dir_count; i++)
        n00bb_ffi_add_contract_dir(ffi, parse_cmdr_flag_str_at(
            args, CORE_STRING_STATIC("--contract-dir"), i));
    const n00bb_module_generator_t generator_items[] = {
        {CORE_STRING_STATIC("ffi"), n00bb_ffi_generate, ffi},
    };
    const n00bb_module_generators_t generators = {
        generator_items, sizeof(generator_items) / sizeof(generator_items[0]),
    };
    const core_string_t module_modifiers[] = {CORE_STRING_STATIC("ffi")};
    n00bb_preprocessor_options_t preprocessor_options = {
        .definitions = definitions,
        .definition_count = (size_t)define_count,
        .release_mode = parse_cmdr_flag_present(
            args, CORE_STRING_STATIC("--release")),
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
    if (!locate_only) {
        grammar = parse_grammar_new();
        parse_grammar_set_error_recovery(grammar, false);
        char *grammar_path = n00bb_grammar_path(argv[0]);
        if (!grammar_path || !parse_bnf_load_file(
                grammar_path, CORE_STRING_STATIC("module"), grammar)) {
            const char *error = parse_bnf_error();
            fprintf(stderr, "n00bb: cannot load n00b grammar%s%s\n",
                    error ? ": " : "", error ? error : "");
            scheduled = false;
        }
        else {
            parse_grammar_finalize(grammar);
            if (!scan_grammar_lex_compiled(grammar)) {
                const char *error = scan_lex_compile_error();
                fprintf(stderr, "n00bb: cannot compile lexer%s%s\n",
                        error ? ": " : "", error ? error : "");
                scheduled = false;
            }
        }
        core_free(grammar_path);
    }
    bool profile_compile = getenv("N00BB_PROFILE_COMPILE") != nullptr;
    double profile_tick = profile_compile ? n00bb_profile_seconds() : 0;
    bool needs_packages = preprocess_command || show_symbols
        || (!locate_only && !lex_command && !parse_command
            && (!fmt_command || normalize_fmt));
    if (scheduled && options_valid && needs_packages
        && !n00bb_resolver_prepare_packages(resolver)) {
        core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0024"),
            CORE_STRING_STATIC("cannot refresh the package source inventory"));
        scheduled = false;
    }
    bool located = scheduled && options_valid && run_locate_jobs(queue, resolver, &requests,
        grammar, !locate_only, show_tokens,
        !locate_only && !lex_command, show_tree,
        needs_packages,
        show_preprocessed, show_symbols,
        lower_command || build_command || library_command || compile_command,
        library_command || compile_command,
        build_command,
        native_roots, (size_t)native_root_count,
        linked_interfaces, (size_t)interface_count,
        &preprocessor_options,
        ir_context, types, (size_t)requested_jobs, &environment, &link,
        &link_analysis);
    if (parse_cmdr_flag_present(args, CORE_STRING_STATIC("--show-ffi"))
        && !show_module_literal(resolver, CORE_STRING_STATIC("ffi"),
               parse_cmdr_flag_str(args, CORE_STRING_STATIC("--show-ffi")))) {
        fprintf(stderr, "n00bb: no 'ffi literal named %s\n",
                parse_cmdr_flag_str(args, CORE_STRING_STATIC("--show-ffi")).data);
        located = false;
    }
    if (located && fmt_command)
        located = n00bb_format_units(grammar, queue, &requests, args);
    if (profile_compile) {
        double now = n00bb_profile_seconds();
        fprintf(stderr, "PROFILE jobs %.3f\n", now - profile_tick);
        profile_tick = now;
    }
    bool interface_closure_only = library_command && emit_interface
        && interface_requires_comptime_ir(link_analysis);
    if (located && (lower_command || build_command || compile_command
                    || (library_command && (!emit_interface
                                            || interface_ir_dump
                                            || interface_closure_only))))
        located = lower_requests_to_ir(grammar, link,
            link_analysis, ir_context, cli_diagnostics,
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-ir"))
                || (lower_command && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-control-flow"))
                    && !parse_cmdr_flag_present(args,
                        CORE_STRING_STATIC("--dump-pdg"))),
            parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--dump-control-flow")),
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-pdg")),
            parse_cmdr_flag_present(args, CORE_STRING_STATIC("--dump-layout")),
            interface_closure_only);
    if (profile_compile) {
        double now = n00bb_profile_seconds();
        fprintf(stderr, "PROFILE lower %.3f\n", now - profile_tick);
        profile_tick = now;
    }
    if (located && library_command && emit_interface && link_analysis) {
        th_env *interface_environment = link_analysis->interface_environment;
        located = register_unit_comptime_ir_metadata(link_analysis, link)
            && interface_environment
            && th_env_freeze(interface_environment);
        if (!located)
            core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
                CORE_STRING_STATIC("N00BB-B0005"),
                CORE_STRING_STATIC(
                    "could not archive compile-time IR metadata"));
    }
    if (located && library_command && emit_interface) {
        char *path = core_strndup(interface_output.data,
                                  interface_output.u8_bytes);
        core_object_archive_error_t archive_error =
            CORE_OBJECT_ARCHIVE_OK;
        th_env *interface_environment = link_analysis
            ? link_analysis->interface_environment : nullptr;
        bool saved = interface_environment
            && th_env_save(interface_environment, path, 0, &archive_error);
        if (!saved) {
            core_buffer_t message = {0};
            core_buffer_printf(&message, "cannot write semantic interface '%s': %s",
                path, core_object_archive_error_string(archive_error));
            core_diagnostics_report(cli_diagnostics, nullptr, CORE_DIAG_ERROR,
                CORE_STRING_STATIC("N00BB-B0005"),
                core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            located = false;
        }
        core_free(path);
    }
    bool abi_emitted = false;
    if (located && (build_command || compile_command
                    || (library_command && !emit_interface))) {
        located = n00bb_execute_comptime(link, link_analysis,
            !preprocessor_options.release_mode, native_cpu, argv[0],
            cli_diagnostics);
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE comptime %.3f\n", now - profile_tick);
            profile_tick = now;
        }
        if (located && !n00bb_finalize_comptime_layout(link_analysis)) {
            core_diagnostics_report(cli_diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0013"),
                CORE_STRING_STATIC("could not finalize layout from compile-time results"));
            located = false;
        }
    }
    if (located && build_command) {
        if (!parse_cmdr_flag_present(args,
                CORE_STRING_STATIC("--no-specialize"))) {
            core_buffer_t failure = {0};
            located = n00bb_specialize_runtime_calls(
                &link_analysis->runtime_reachable_functions, &failure);
            if (located) {
                core_list_t(core_translation_unit_ptr_t) order =
                    core_list_new(core_translation_unit_ptr_t);
                located = n00bb_collect_initializer_order(link,
                    "n00bb.runtime-module-initializer-order", &order)
                    && build_phase_reachable_functions(link_analysis,
                        &order, false, cli_diagnostics);
                core_list_free(order);
            }
            if (located) located = publish_declared_aggregate_layouts(
                link_analysis)
                && publish_interned_variant_layouts(link_analysis);
            if (!located) core_diagnostics_report(cli_diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0012"),
                core_string_from_cstr(failure.data ? failure.data
                    : "could not specialize runtime calls"));
            core_buffer_release(&failure);
        }
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE specialize %.3f\n", now - profile_tick);
            profile_tick = now;
        }
    }
    if (located && build_command) {
        located = n00bb_emit_abi_artifacts(link, link_analysis,
            !preprocessor_options.release_mode, true, false);
        if (profile_compile) {
            double now = n00bb_profile_seconds();
            fprintf(stderr, "PROFILE abi %.3f\n", now - profile_tick);
            profile_tick = now;
        }
        core_list_t(core_buffer_t) shim_bitcode =
            core_list_new(core_buffer_t);
        char *compiler_executable = located
            ? n00bb_executable_path(argv[0]) : nullptr;
        if (located && n00bb_ffi_shim_count(ffi))
            located = n00bb_ffi_compile_shims(ffi,
                link_analysis->target.triple, compiler_executable,
                &shim_bitcode, cli_diagnostics);
        core_free(compiler_executable);
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
                !preprocessor_options.release_mode, native_cpu, argv[0],
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
            !preprocessor_options.release_mode, false, true);
        if (located)
            abi_emitted = located = n00bb_write_compile_abi_artifact(
                link, args, cli_diagnostics);
    }
    if (located && library_command && !emit_interface
        && parse_cmdr_flag_present(args, CORE_STRING_STATIC("--output"))) {
        located = n00bb_emit_abi_artifacts(link, link_analysis,
            !preprocessor_options.release_mode, false, false);
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
        fprintf(stderr,
                "n00bb: translation units are located; '%.*s' processing is not implemented yet\n",
                (int)command.u8_bytes, command.data);
        located = false;
    }

    bool diagnostics_deferred = lower_command || build_command
        || library_command || compile_command;
    if (diagnostics_deferred) {
        if (link_analysis) {
            for (size_t i = 0; i < link_analysis->units.len; i++)
                core_diagnostics_merge(cli_diagnostics,
                    core_translation_unit_diagnostics(
                        link_analysis->units.data[i]));
        }
        if (link)
            core_diagnostics_merge(cli_diagnostics,
                                   core_link_unit_diagnostics(link));
    }

    core_link_unit_free(link);
    th_env_free(environment);
    n00bb_link_analysis_free(link_analysis);
    release_requests(queue, &requests);
    for (int32_t i = 0; i < interface_count; i++)
        th_env_free((th_env *)linked_interfaces[i]);
    core_free(linked_interfaces);
    core_free(native_roots);
    if (core_diagnostics_count(cli_diagnostics)) {
        char *text = core_diagnostics_render_text(cli_diagnostics);
        if (text) { fputs(text, stderr); core_free(text); }
    }
    core_diagnostics_free(cli_diagnostics);
    ir_ctx_free(ir_context);
    parse_grammar_free(grammar);
    n00bb_resolver_free(resolver);
    core_work_queue_free(queue);
    n00bb_ffi_free(ffi);
    core_free(definitions);
    parse_cmdr_result_free(args);
    parse_cmdr_free(cli);
    return located ? 0 : 1;
}
