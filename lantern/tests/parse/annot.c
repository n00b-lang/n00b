#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ir/typelang.h"
#include "ir/layout.h"
#include "th/environment.h"
#include "lib/alloc.h"
#include "parse/annot_walk.h"
#include "parse/annotation.h"
#include "parse/bnf.h"
#include "parse/infer.h"
#include "parse/pwz.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"
#include "parse/tree_visit.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
}

static th_ctx *types;

static void
annotate_once(parse_grammar_t *g, parse_tree_t *tree,
              parse_annot_result_t *result, th_ctx *context,
              ir_module_t *module)
{
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result, context, module);
    parse_tree_walk_run(walk, tree);
    parse_tree_walk_free(walk);
}

static parse_annot_result_t *
annotate_new(parse_grammar_t *g, parse_tree_t *tree, th_ctx *context)
{
    if (!g || !tree) return nullptr;
    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    annotate_once(g, tree, result, context, nullptr);
    return result;
}

static bool
is_type(th_type *t, const char *typelang)
{
    return t && types
        && th_hash(types, t) == th_hash(types, ir_type_from_text(
                                                   types,
                                                   S(typelang)));
}

typedef struct {
    parse_tree_t *target;
    parse_tree_t *parent;
    bool saw_parent;
} walk_parent_probe_t;

static bool
capture_walk_parent(parse_visit_t *visit)
{
    walk_parent_probe_t *probe = visit ? visit->thunk : nullptr;
    if (probe && visit->node == probe->target)
        probe->saw_parent = visit->parent == probe->parent;
    return true;
}

static void
test_tree_resolution_is_scope_or_symbol_without_replacing_lexical_scope(void)
{
    parse_scope_t lexical_scope = {0};
    parse_scope_t module_scope = {0};
    parse_sym_entry_t symbol = {0};
    parse_tree_t *node = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){.scope = &lexical_scope}));
    parse_symtab_t *symtab = parse_symtab_new();
    bool scope_set = node
        && parse_tree_set_resolution_scope(node, &module_scope);
    bool lexical_scope_lookup = node && symtab
        && parse_symtab_scope_for_node(symtab, node) == &lexical_scope;
    parse_node_resolution_t scope_ref = parse_tree_resolution(node);
    bool scope_ok = scope_set
        && scope_ref.kind == PARSE_NODE_RESOLUTION_SCOPE
        && scope_ref.target.scope == &module_scope
        && core_tree_node_value(node).scope == &lexical_scope;
    bool symbol_set = node
        && parse_tree_set_resolution_symbol(node, &symbol);
    parse_node_resolution_t symbol_ref = parse_tree_resolution(node);
    bool symbol_ok = symbol_set
        && symbol_ref.kind == PARSE_NODE_RESOLUTION_SYMBOL
        && symbol_ref.target.symbol == &symbol
        && core_tree_node_value(node).sym == &symbol
        && core_tree_node_value(node).scope == &lexical_scope;
    check("tree resolution stores either a target scope or symbol independently of lexical scope",
          scope_ok && symbol_ok && lexical_scope_lookup);
    parse_symtab_free(symtab);
    if (node) {
        core_free(node->node.children);
        core_free(node);
    }
}

static void
test_tree_integer_range_fact(void)
{
    parse_tree_t *node = core_tree_node(parse_nt_node_t,
        parse_token_info_ptr_t, ((parse_nt_node_t){0}));
    int64_t minimum = 0, maximum = 0;
    bool unset = node && !parse_tree_integer_range(node, &minimum, &maximum);
    bool set = node && parse_tree_set_integer_range(node, 2, 7);
    bool invalid_rejected = node
        && !parse_tree_set_integer_range(node, 8, 7);
    bool read = node && parse_tree_integer_range(node, &minimum, &maximum);
    check("tree stores integer range analysis separately from inferred type",
          unset && set && invalid_rejected && read
          && minimum == 2 && maximum == 7);
    if (node) {
        core_free(node->node.children);
        core_free(node);
    }
}

static bool
visit_type_namespace_scope(parse_scope_t *scope, void *context)
{
    size_t *count = context;
    if (!scope->ns
        || !core_string_eq(scope->ns->ns_name, S("type"))) return false;
    (*count)++;
    return true;
}

static bool
stop_namespace_scope_visit(parse_scope_t *scope, void *context)
{
    (void)scope;
    (*(size_t *)context)++;
    return false;
}

static bool
count_namespace_symbol(parse_sym_entry_t *symbol, void *context)
{
    (void)symbol;
    (*(size_t *)context)++;
    return true;
}

static void
test_symbol_namespace_scope_iteration(void)
{
    parse_symtab_t *st = parse_symtab_new();
    parse_symtab_push_scope_all(st, S("module"));
    parse_symtab_push_scope_all(st, S("function"));
    parse_symtab_pop_scope_all(st);

    parse_symtab_ns(st, S("type"));
    size_t count = 0;
    bool all_type_scopes = parse_symtab_visit_namespace_scopes(
        st, S("type"), visit_type_namespace_scope, &count);

    size_t stopped_count = 0;
    bool stopped = parse_symtab_visit_namespace_scopes(
        st, S("type"), stop_namespace_scope_visit, &stopped_count);

    check("namespace scope visitor covers only mirrored namespace scopes",
          all_type_scopes && count == 3);
    check("namespace scope visitor reports early termination",
          !stopped && stopped_count == 1);
    check("namespace scope visitor rejects an unknown namespace",
          !parse_symtab_visit_namespace_scopes(st, S("missing"),
              visit_type_namespace_scope, &count));
    parse_symtab_free(st);
}

static void
test_symbol_namespaces_share_lexical_scopes(void)
{
    parse_symtab_t *st = parse_symtab_new();
    core_string_t type_ns = S("type");
    parse_symtab_push_scope_all(st, S("module"));
    parse_scope_t *module_scope = parse_symtab_current_scope(st, S(""));
    parse_sym_entry_t *value = parse_symtab_add(
        st, S(""), S("Shared"), PARSE_SYM_VARIABLE, nullptr);
    parse_symtab_push_scope_all(st, S("function"));
    parse_scope_t *function_scope = parse_symtab_current_scope(st, S(""));
    parse_sym_entry_t *local_value = parse_symtab_add(
        st, S(""), S("Shared"), PARSE_SYM_VARIABLE, nullptr);
    parse_symtab_pop_scope_all(st);

    parse_sym_entry_t *type = parse_symtab_add(
        st, type_ns, S("Shared"), PARSE_SYM_TYPEDEF, nullptr);
    parse_scope_t *type_function_scope = parse_symtab_scope_in_namespace(
        st, function_scope, type_ns);
    parse_symtab_enter_scope(st, type_ns, type_function_scope);
    parse_sym_entry_t *local_type = parse_symtab_add(
        st, type_ns, S("Shared"), PARSE_SYM_TYPEDEF, nullptr);

    bool pass = value && type && local_type && local_value
        && type_function_scope
        && value->kind == PARSE_SYM_VARIABLE
        && type->kind == PARSE_SYM_TYPEDEF
        && parse_symtab_lookup_scoped(st, module_scope, type_ns, S("Shared"))
            == type
        && parse_symtab_lookup_scoped(st, function_scope, type_ns,
                                      S("Shared")) == local_type
        && parse_symtab_lookup_scoped(st, function_scope, S(""),
                                      S("Shared")) == local_value
        && parse_symtab_scope_in_namespace(st, module_scope, type_ns)
            == type->scope
        && type->scope != value->scope
        && type_function_scope->depth == function_scope->depth
        && local_type->scope == type_function_scope;
    size_t module_matches = 0;
    size_t function_matches = 0;
    size_t peer_scope_matches = 0;
    bool found_module_names = parse_symtab_visit_name_across_namespaces(
        st, value->scope, S("Shared"), count_namespace_symbol,
        &module_matches);
    bool found_function_names = parse_symtab_visit_name_across_namespaces(
        st, function_scope, S("Shared"), count_namespace_symbol,
        &function_matches);
    bool found_peer_scope_names = parse_symtab_visit_name_across_namespaces(
        st, type_function_scope, S("Shared"), count_namespace_symbol,
        &peer_scope_matches);
    pass = pass && found_module_names && module_matches == 2
        && found_function_names && function_matches == 2
        && found_peer_scope_names && peer_scope_matches == 2;
    check("late symbol namespaces mirror lexical scopes without merging tables",
          pass);
    parse_symtab_free(st);
}

static void
test_symtab_phase_lifetime_and_alias_facts(void)
{
    parse_symtab_t *st = parse_symtab_new();
    core_string_t ns = CORE_STRING_STATIC("phase-test");
    parse_sym_entry_t *value = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("value"), PARSE_SYM_VARIABLE, nullptr);
    parse_sym_entry_t *view = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("view"), PARSE_SYM_VARIABLE, nullptr);
    parse_sym_entry_t *safe_value = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("safe_value"), PARSE_SYM_VARIABLE,
        nullptr);
    parse_sym_entry_t *safe_view = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("safe_view"), PARSE_SYM_VARIABLE,
        nullptr);
    parse_sym_entry_t *bridge = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("bridge"), PARSE_SYM_VARIABLE, nullptr);
    parse_sym_entry_t *unknown_value = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("unknown_value"), PARSE_SYM_VARIABLE,
        nullptr);
    parse_sym_entry_t *unbounded_value = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("unbounded_value"), PARSE_SYM_VARIABLE,
        nullptr);
    parse_sym_entry_t *incomplete_value = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("incomplete_value"), PARSE_SYM_VARIABLE,
        nullptr);
    parse_sym_entry_t *unknown_view = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("unknown_view"), PARSE_SYM_VARIABLE,
        nullptr);
    parse_sym_entry_t *null_payload = parse_symtab_add(
        st, ns, CORE_STRING_STATIC("null_payload"), PARSE_SYM_VARIABLE,
        nullptr);
    int compile_value = 11;
    int runtime_init = 12;
    int compile_binding = 13;
    int runtime_binding = 14;
    int runtime_function_a = 17;
    int runtime_function_b = 18;
    int runtime_binding_a = 19;
    int runtime_binding_b = 20;
    int cfg_from = 15;
    int cfg_to = 16;
    int lifetime_entry = 21;
    int lifetime_instruction = 22;
    void *observed_comptime = nullptr;
    void *observed_runtime = nullptr;

    parse_sym_set_phases(value, PARSE_SYM_PHASE_BOTH,
                         PARSE_SYM_PHASE_COMPTIME,
                         PARSE_SYM_PHASE_BOTH);
    parse_sym_set_phase_values(value, &compile_value, &runtime_init);
    parse_sym_set_phase_values_with_presence(null_payload, true, nullptr,
                                              true, nullptr);
    bool phase_bindings_set = parse_sym_set_phase_binding(
        value, PARSE_SYM_PHASE_COMPTIME, &compile_binding)
        && parse_sym_set_phase_binding(value, PARSE_SYM_PHASE_RUNTIME,
                                       &runtime_binding)
        && parse_sym_set_function_binding(value, PARSE_SYM_PHASE_RUNTIME,
                                          &runtime_function_a,
                                          &runtime_binding_a)
        && parse_sym_set_function_binding(value, PARSE_SYM_PHASE_RUNTIME,
                                          &runtime_function_b,
                                          &runtime_binding_b)
        && parse_sym_set_function_binding(value, PARSE_SYM_PHASE_RUNTIME,
                                          &runtime_function_a,
                                          &runtime_binding);
    const parse_sym_function_binding_t *function_binding_a =
        parse_sym_function_binding_at(value, PARSE_SYM_PHASE_RUNTIME, 0);
    const parse_sym_function_binding_t *function_binding_b =
        parse_sym_function_binding_at(value, PARSE_SYM_PHASE_RUNTIME, 1);
    check("symbols retain phase bindings for every owning IR function",
          phase_bindings_set
              && parse_sym_function_binding_count(
                  value, PARSE_SYM_PHASE_RUNTIME) == 2
              && function_binding_a
              && function_binding_a->function == &runtime_function_a
              && function_binding_a->binding == &runtime_binding
              && function_binding_b
              && function_binding_b->function == &runtime_function_b
              && function_binding_b->binding == &runtime_binding_b);
    parse_sym_set_storage_class(value, CORE_STRING_STATIC("gc"), true,
                                CORE_STRING_STATIC("gc"));
    parse_sym_set_storage_duration(value, PARSE_SYM_STORAGE_DYNAMIC);
    parse_sym_set_lifetime(value, (parse_sym_lifetime_t){
        .region = 3,
        .start_point = 10,
        .end_point = 20,
        .finalization_point = 19,
        .has_start = true,
        .has_end = true,
        .has_finalization_point = true,
    });
    bool lifetime_starts = parse_sym_set_lifetime_start(value,
        (parse_sym_lifetime_start_t){
            .phase = PARSE_SYM_PHASE_RUNTIME,
            .function = &runtime_function_a,
            .binding = &runtime_binding,
            .point = &lifetime_entry,
            .at_entry = true,
        })
        && parse_sym_set_lifetime_start(value,
            (parse_sym_lifetime_start_t){
                .phase = PARSE_SYM_PHASE_RUNTIME,
                .function = &runtime_function_b,
                .binding = &runtime_binding_b,
                .point = &lifetime_instruction,
            });
    bool cfg_edges = parse_sym_set_lifetime_edge(value,
        (parse_sym_lifetime_edge_t){
            .function = &runtime_function_a,
            .binding = &runtime_binding,
            .from = &cfg_from,
            .to = &cfg_to,
            .phase = PARSE_SYM_PHASE_RUNTIME,
            .direct = PARSE_SYM_LIVENESS_DEAD,
            .aliases = PARSE_SYM_LIVENESS_LIVE,
            .ends_live_range = true,
        })
        && parse_sym_set_lifetime_edge(value,
            (parse_sym_lifetime_edge_t){
                .function = &runtime_function_a,
                .binding = &runtime_binding,
                .from = &cfg_from,
                .phase = PARSE_SYM_PHASE_COMPTIME,
                .direct = PARSE_SYM_LIVENESS_UNKNOWN,
                .aliases = PARSE_SYM_LIVENESS_UNKNOWN,
                .is_exit = true,
            })
        && parse_sym_set_lifetime_edge(value,
            (parse_sym_lifetime_edge_t){
                .function = &runtime_function_a,
                .binding = &runtime_binding,
                .from = &cfg_from,
                .to = &cfg_to,
                .phase = PARSE_SYM_PHASE_RUNTIME,
                .direct = PARSE_SYM_LIVENESS_LIVE,
                .aliases = PARSE_SYM_LIVENESS_UNKNOWN,
                .ends_live_range = true,
            })
        && parse_sym_set_lifetime_edge(value,
            (parse_sym_lifetime_edge_t){
                .function = &runtime_function_b,
                .binding = &runtime_binding_b,
                .from = &cfg_from,
                .to = &cfg_to,
                .phase = PARSE_SYM_PHASE_RUNTIME,
                .direct = PARSE_SYM_LIVENESS_LIVE,
                .aliases = PARSE_SYM_LIVENESS_LIVE,
            })
        && !parse_sym_set_lifetime_edge(value,
            (parse_sym_lifetime_edge_t){
                .from = &cfg_from,
                .phase = PARSE_SYM_PHASE_RUNTIME,
                .direct = PARSE_SYM_LIVENESS_DEAD,
                .aliases = PARSE_SYM_LIVENESS_DEAD,
            });

    parse_sym_set_phases(view, PARSE_SYM_PHASE_RUNTIME,
                         PARSE_SYM_PHASE_BOTH,
                         PARSE_SYM_PHASE_COMPTIME
                             | PARSE_SYM_PHASE_RUNTIME);
    bool alias_added = parse_symtab_add_alias(
        view, value, PARSE_SYM_ALIAS_DERIVED, 14, true);
    parse_sym_set_lifetime(safe_value, (parse_sym_lifetime_t){
        .region = 4,
        .start_point = 1,
        .end_point = 12,
        .has_start = true,
        .has_end = true,
    });
    parse_sym_set_lifetime(safe_view, (parse_sym_lifetime_t){
        .region = 4,
        .start_point = 2,
        .end_point = 12,
        .has_start = true,
        .has_end = true,
    });
    parse_sym_set_lifetime(bridge, (parse_sym_lifetime_t){
        .region = 4,
        .start_point = 3,
        .end_point = 12,
        .has_start = true,
        .has_end = true,
    });
    bool safe_alias_added = parse_symtab_update_alias_use(
        safe_view, safe_value, PARSE_SYM_ALIAS_DERIVED, 3, 9, false)
        && parse_symtab_update_alias_use(
            safe_view, safe_value, PARSE_SYM_ALIAS_DERIVED, 3, 14, false)
        && parse_symtab_update_alias_use(
            bridge, safe_view, PARSE_SYM_ALIAS_DERIVED, 4, 13, false)
        && parse_symtab_update_alias_use(
            safe_value, bridge, PARSE_SYM_ALIAS_DERIVED, 2, 10, false);
    parse_sym_set_alias_analysis_complete(safe_value, true);
    parse_sym_set_alias_analysis_complete(safe_view, true);
    parse_sym_set_alias_analysis_complete(bridge, true);
    bool unknown_alias_added = parse_symtab_add_alias(
        unknown_view, unknown_value, PARSE_SYM_ALIAS_MAY, 5, false);
    parse_sym_set_alias_analysis_complete(unknown_value, true);
    parse_sym_set_alias_analysis_complete(unknown_view, true);
    parse_sym_set_alias_analysis_complete(unbounded_value, true);
    bool late_edge_invalidates = parse_symtab_add_alias(
        unknown_view, unknown_value, PARSE_SYM_ALIAS_MAY, 6, false)
        && !unknown_view->alias_analysis_complete
        && !unknown_value->alias_analysis_complete;
    parse_sym_set_alias_analysis_complete(unknown_value, true);
    parse_sym_set_alias_analysis_complete(unknown_view, true);

    check("phase-aware symbols expose only available reads",
          parse_sym_can_read(value, PARSE_SYM_PHASE_COMPTIME)
              && parse_sym_can_read(value, PARSE_SYM_PHASE_RUNTIME)
              && !parse_sym_can_read(view, PARSE_SYM_PHASE_COMPTIME));
    check("phase-specific mutability is independent",
          parse_sym_can_write(value, PARSE_SYM_PHASE_COMPTIME)
              && !parse_sym_can_write(value, PARSE_SYM_PHASE_RUNTIME)
              && parse_sym_can_write(view, PARSE_SYM_PHASE_RUNTIME));
    check("symbols retain their lexical lifetime region",
          unknown_view->lifetime.has_region
              && unknown_view->lifetime.region
                    == (uint64_t)unknown_view->scope->id);
    check("static initialization is distinct from reading runtime values",
          parse_sym_can_initialize(view, PARSE_SYM_PHASE_COMPTIME)
              && !parse_sym_can_read(view, PARSE_SYM_PHASE_COMPTIME)
              && !parse_sym_can_write(view, PARSE_SYM_PHASE_COMPTIME));
    check("compile-time value and runtime initializer remain distinct",
          parse_sym_get_comptime_value(value, &observed_comptime)
              && observed_comptime == &compile_value
              && parse_sym_get_runtime_initializer(value, &observed_runtime)
              && observed_runtime == &runtime_init);
    check("explicit presence distinguishes null payloads from missing values",
          parse_sym_get_comptime_value(null_payload, &observed_comptime)
              && !observed_comptime
              && parse_sym_get_runtime_initializer(null_payload,
                                                    &observed_runtime)
              && !observed_runtime
              && !parse_sym_get_comptime_value(incomplete_value,
                                               &observed_comptime)
              && !parse_sym_get_runtime_initializer(incomplete_value,
                                                     &observed_runtime));
    check("one source symbol retains separate phase bindings",
          phase_bindings_set
              && parse_sym_phase_binding(value, PARSE_SYM_PHASE_COMPTIME)
                    == &compile_binding
              && parse_sym_phase_binding(value, PARSE_SYM_PHASE_RUNTIME)
                    == &runtime_binding
              && !parse_sym_phase_binding(value, PARSE_SYM_PHASE_BOTH));
    check("storage request and compiler selection are retained",
          value->storage_class_required
              && core_string_eq(value->requested_storage_class,
                                CORE_STRING_STATIC("gc"))
              && core_string_eq(value->selected_storage_class,
                                CORE_STRING_STATIC("gc")));
    parse_sym_set_storage_class(value, value->requested_storage_class, true,
                                value->requested_storage_class);
    check("storage class setter preserves aliased input strings",
          value->storage_class_required
              && core_string_eq(value->requested_storage_class,
                                CORE_STRING_STATIC("gc"))
              && core_string_eq(value->selected_storage_class,
                                CORE_STRING_STATIC("gc")));
    parse_sym_set_storage_class(value, (core_string_t){0}, true,
                                (core_string_t){0});
    check("required storage remains required when its class is missing",
          value->storage_class_required
              && !value->requested_storage_class.u8_bytes
              && !value->selected_storage_class.u8_bytes);
    parse_sym_set_storage_class(value, CORE_STRING_STATIC("gc"), true,
                                CORE_STRING_STATIC("gc"));
    check("storage duration is tracked separately from storage class",
          value->storage_duration == PARSE_SYM_STORAGE_DYNAMIC
              && !strcmp(parse_sym_storage_duration_name(
                    value->storage_duration), "dynamic")
              && unknown_view->storage_duration
                    == PARSE_SYM_STORAGE_UNSPECIFIED);
    check("lifetime endpoints and finalization point are retained",
          value->lifetime.region == 3 && value->lifetime.has_start
              && value->lifetime.start_point == 10 && value->lifetime.has_end
              && value->lifetime.end_point == 20
              && value->lifetime.has_finalization_point
              && value->lifetime.finalization_point == 19);
    const parse_sym_lifetime_start_t *entry_start =
        parse_sym_lifetime_start_at(value, 0);
    const parse_sym_lifetime_start_t *instruction_start =
        parse_sym_lifetime_start_at(value, 1);
    check("lifetime starts retain function, binding, and IR point identity",
          lifetime_starts && parse_sym_lifetime_start_count(value) == 2
              && entry_start
              && entry_start->function == &runtime_function_a
              && entry_start->binding == &runtime_binding
              && entry_start->point == &lifetime_entry
              && entry_start->at_entry
              && instruction_start
              && instruction_start->function == &runtime_function_b
              && instruction_start->binding == &runtime_binding_b
              && instruction_start->point == &lifetime_instruction
              && !instruction_start->at_entry);
    const parse_sym_lifetime_edge_t *runtime_edge =
        parse_sym_lifetime_edge_at(value, 0);
    const parse_sym_lifetime_edge_t *comptime_exit =
        parse_sym_lifetime_edge_at(value, 1);
    bool lifetime_ends = parse_sym_set_lifetime_end(value,
        (parse_sym_lifetime_end_t){
            .phase = PARSE_SYM_PHASE_RUNTIME,
            .function = &runtime_function_a,
            .binding = &runtime_binding,
            .point = &lifetime_instruction,
        }) && parse_sym_set_lifetime_end(value,
            (parse_sym_lifetime_end_t){
                .phase = PARSE_SYM_PHASE_RUNTIME,
                .function = &runtime_function_b,
                .binding = &runtime_binding_b,
                .from = &cfg_from,
                .to = &cfg_to,
                .on_edge = true,
            }) && !parse_sym_set_lifetime_end(value,
            (parse_sym_lifetime_end_t){
                .phase = PARSE_SYM_PHASE_RUNTIME,
                .function = &runtime_function_a,
                .binding = &runtime_binding,
                .on_edge = true,
            });
    bool ranges_finished =
        parse_sym_finish_lifetime_range(value, PARSE_SYM_PHASE_RUNTIME,
            &runtime_function_a, &runtime_binding, true)
        && parse_sym_finish_lifetime_range(value, PARSE_SYM_PHASE_RUNTIME,
            &runtime_function_b, &runtime_binding_b, true)
        && !parse_sym_finish_lifetime_range(value, PARSE_SYM_PHASE_COMPTIME,
            &runtime_function_a, &runtime_binding, true);
    const parse_sym_lifetime_range_t *range_a =
        parse_sym_lifetime_range_at(value, 0);
    const parse_sym_lifetime_range_t *range_b =
        parse_sym_lifetime_range_at(value, 1);
    const parse_sym_lifetime_range_t *incomplete_range =
        parse_sym_lifetime_range_at(value, 2);
    check("symbols retain deduplicated CFG lifetime facts by phase",
          cfg_edges && parse_sym_lifetime_edge_count(value) == 3
              && runtime_edge && runtime_edge->from == &cfg_from
              && runtime_edge->function == &runtime_function_a
              && runtime_edge->binding == &runtime_binding
              && runtime_edge->to == &cfg_to
              && runtime_edge->phase == PARSE_SYM_PHASE_RUNTIME
              && runtime_edge->direct == PARSE_SYM_LIVENESS_LIVE
              && runtime_edge->aliases == PARSE_SYM_LIVENESS_UNKNOWN
              && runtime_edge->ends_live_range
              && comptime_exit && comptime_exit->is_exit
              && comptime_exit->to == nullptr
              && comptime_exit->phase == PARSE_SYM_PHASE_COMPTIME
              && parse_sym_lifetime_edge_at(value, 2)->function
                    == &runtime_function_b
              && parse_sym_lifetime_edge_at(value, 2)->binding
                    == &runtime_binding_b
              && lifetime_ends && ranges_finished
              && parse_sym_lifetime_range_count(value) == 3
              && range_a && range_a->start_count == 1
              && range_a->edge_count == 1 && range_a->end_count == 1
              && range_a->complete
              && range_b && range_b->start_count == 1
              && range_b->edge_count == 1 && range_b->end_count == 1
              && range_b->complete
              && incomplete_range && !incomplete_range->start_count
              && incomplete_range->edge_count == 1
              && !incomplete_range->complete);
    bool range_reset = parse_sym_begin_lifetime_range(value,
        PARSE_SYM_PHASE_RUNTIME, &runtime_function_a, &runtime_binding);
    range_a = parse_sym_lifetime_range_at(value, 0);
    range_b = parse_sym_lifetime_range_at(value, 1);
    check("restarting one lifetime range clears only its owner facts",
          range_reset && range_a && !range_a->complete
              && !range_a->start_count && !range_a->edge_count
              && !range_a->end_count
              && range_b && range_b->complete
              && range_b->start_count == 1 && range_b->edge_count == 1
              && range_b->end_count == 1);
    const parse_sym_alias_t *forward = parse_sym_alias_at(view, 0);
    const parse_sym_alias_t *reverse = parse_sym_aliased_by_at(value, 0);
    check("alias relationships are queryable in both directions",
          alias_added && parse_sym_alias_count(view) == 1
              && parse_sym_aliased_by_count(value) == 1
              && forward && forward->other == value && forward->escapes
              && reverse && reverse->other == view && reverse->escapes);
    check("escaping or unbounded aliases prevent early finalization",
          !parse_sym_can_finalize_at(value, 30)
              && unknown_alias_added && late_edge_invalidates
              && !parse_sym_can_finalize_at(unknown_value, 15)
              && !parse_sym_can_finalize_at(unbounded_value, 15)
              && !parse_sym_can_finalize_at(incomplete_value, 15));
    const parse_sym_alias_t *bounded_alias =
        parse_sym_alias_at(safe_view, 0);
    check("finalization waits for transitive alias last uses across cycles",
          safe_alias_added && bounded_alias && bounded_alias->has_last_use
              && bounded_alias->last_use == 14
              && !parse_sym_can_finalize_at(safe_value, 14)
              && parse_sym_can_finalize_at(safe_value, 15));

    parse_symtab_free(st);
}

typedef struct {
    int               decls_seen;
    parse_tree_t *block_node;
    parse_tree_t *cond_node;
    parse_scope_t      *module_scope;
    parse_scope_t      *block_scope;
} probe_t;

static bool
saw_decl(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->decls_seen++;

    return true;
}

static bool
saw_block(parse_visit_t *v)
{
    probe_t *p = (probe_t *)v->thunk;

    p->block_node  = v->node;
    p->block_scope = v->pn->scope;

    return true;
}

static bool
saw_prog(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->module_scope = v->pn->scope;

    return true;
}

static bool
saw_cond(parse_visit_t *v)
{
    ((probe_t *)v->thunk)->cond_node = v->node;

    return true;
}

typedef struct {
    size_t enters;
    size_t exits;
} node_probe_t;

static void
count_annot_nodes(parse_visit_t *v, bool entering, void *context)
{
    node_probe_t *probe = context;
    (void)v;

    if (entering) {
        probe->enters++;
    }
    else {
        probe->exits++;
    }
}

static parse_grammar_t *
load_toy(void)
{
    core_string_t text = slurp("toy.bnf");

    if (!text.data) {
        return nullptr;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(text, S("prog"), g)) {
        check("toy.bnf loads", false);
        parse_grammar_free(g);

        return nullptr;
    }

    return g;
}

static parse_tree_t *
parse_toy(parse_grammar_t *g, const char *start, const char *src,
          parse_pwz_parser_t **p_out,
          parse_token_stream_t **ts_out)
{
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(S(src));
    parse_pwz_parser_t   *p = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, S(start))));

    if (!parse_pwz_parse(p, ts)) {
        return nullptr;
    }

    *p_out  = p;
    *ts_out = ts;

    return parse_pwz_get_tree(p);
}

static parse_tree_t *
find_node(parse_grammar_t *g, parse_tree_t *node, const char *name)
{
    if (!node || core_tree_is_leaf(node)) {
        return nullptr;
    }

    if (core_string_eq(parse_tree_nt_name(g, node),
                       core_string_borrow(name, strlen(name)))) {
        return node;
    }

    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_node(g, core_tree_child(node, i), name);

        if (found) {
            return found;
        }
    }

    return nullptr;
}

static parse_annotation_t *
find_unique_rule_annot(parse_grammar_t *g, const parse_nonterm_t *nt,
                       parse_annot_kind_t kind)
{
    parse_annotation_t *found = nullptr;

    for (size_t i = 0; g && nt && i < core_list_len(nt->rule_ids); i++) {
        parse_annotation_t *candidate = parse_rule_find_annot(
            g, parse_get_rule(g, nt->rule_ids.data[i]), kind);
        if (!candidate) continue;
        if (found) return nullptr;
        found = candidate;
    }

    return found;
}

static void
test_annotations_are_retained_on_the_grammar(parse_grammar_t *g)
{
    parse_annotation_t *tok = parse_grammar_find_annot(g, PARSE_ANNOT_TOKENIZER);

    check("a file-level annotation is retained on the grammar", tok != nullptr);

    if (tok) {
        check("and keeps its argument", core_string_eq(tok->scope_tag, S("toy")));
    }

    parse_nonterm_t *prog = nullptr;

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (core_string_eq(g->nt_list.data[i]->name, S("prog"))) {
            prog = g->nt_list.data[i];
        }
    }

    check("the start non-terminal exists", prog != nullptr);

    if (prog) {
        check("and carries its @scope annotation",
              find_unique_rule_annot(g, prog, PARSE_ANNOT_SCOPE_OPEN) != nullptr);
    }

    parse_annotation_t *a
        = find_unique_rule_annot(g, prog, PARSE_ANNOT_SCOPE_OPEN);

    if (a) {
        check("with the tag it was written with",
              core_string_eq(a->scope_tag, S("module")));
    }
}

typedef struct {
    parse_grammar_t *grammar;
    parse_rule_t    *rule;
    bool             ok;
} annot_lookup_run_t;

static void *
lookup_annotations_concurrently(void *arg)
{
    annot_lookup_run_t *run = arg;
    run->ok = true;

    for (int i = 0; i < 2000; i++) {
        parse_annotation_t *file
            = parse_grammar_find_annot(run->grammar, PARSE_ANNOT_TOKENIZER);
        parse_annotation_t *scope
            = parse_rule_find_annot(run->grammar, run->rule,
                                    PARSE_ANNOT_SCOPE_OPEN);

        if (!file || !scope) {
            run->ok = false;
            break;
        }
    }

    return nullptr;
}

static void
test_annotation_cache_is_thread_safe(parse_grammar_t *g)
{
    parse_nonterm_t *prog = parse_find_nonterm(g, S("prog"));
    enum { N_THREADS = 8 };
    pthread_t threads[N_THREADS];
    annot_lookup_run_t runs[N_THREADS];
    bool created[N_THREADS] = {0};

    parse_grammar_finalize(g);

    parse_rule_t *prog_rule
        = prog && core_list_len(prog->rule_ids)
              ? parse_get_rule(g, prog->rule_ids.data[0]) : nullptr;

    for (int i = 0; i < N_THREADS; i++) {
        runs[i] = (annot_lookup_run_t){.grammar = g, .rule = prog_rule};
        created[i] = pthread_create(&threads[i], nullptr,
                                    lookup_annotations_concurrently,
                                    &runs[i]) == 0;
    }

    bool ok = prog != nullptr;
    for (int i = 0; i < N_THREADS; i++) {
        if (!created[i]) {
            ok = false;
            continue;
        }

        pthread_join(threads[i], nullptr);
        ok &= runs[i].ok;
    }

    check("annotation cache lookups are safe across threads", ok);
}

static void
test_the_walk_builds_a_symbol_table(parse_grammar_t *g)
{
    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree
        = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;", &p, &ts);

    if (!tree) {
        check("the toy program parses", false);

        return;
    }

    types = th_ctx_new();

    probe_t             probe = {0};
    parse_annot_result_t *r     = core_alloc(parse_annot_result_t);

    memset(r, 0, sizeof(*r));
    r->symtab    = parse_symtab_new();
    r->cf_labels = core_alloc(core_dict_t);
    core_dict_init(r->cf_labels, core_hash_word, nullptr);

    parse_tree_walk_t *w = parse_tree_walk_new(g, r);

    parse_annot_register(w, r, types, nullptr);

    check("a visitor can be registered by non-terminal name",
          parse_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe));
    check("registering an unknown non-terminal fails",
          !parse_walk_on_nt(w, S("no-such-rule"), saw_decl, nullptr, &probe));

    parse_walk_on_nt(w, S("block"), nullptr, saw_block, &probe);
    parse_walk_on_nt(w, S("prog"), nullptr, saw_prog, &probe);
    parse_walk_on_nt(w, S("cond"), nullptr, saw_cond, &probe);

    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    parse_tree_t *subtree = nullptr;
    int32_t subtree_index = -1;
    for (size_t i = 0; i < core_tree_num_children(tree); i++) {
        parse_tree_t *child = core_tree_child(tree, i);
        if (core_tree_is_leaf(child)) continue;
        subtree = child;
        subtree_index = (int32_t)i;
        break;
    }
    walk_parent_probe_t parent_probe = {
        .target = subtree,
        .parent = tree,
    };
    parse_tree_walk_t *subtree_walk = parse_tree_walk_new(g, nullptr);
    if (subtree && subtree_walk) {
        parse_walk_on_node(subtree_walk, capture_walk_parent, nullptr,
                           &parent_probe);
        parse_tree_walk_run_with_parent(subtree_walk, subtree, tree, 1,
                                        subtree_index);
    }
    check("a subtree walk preserves its structural parent",
          subtree && subtree_walk && parent_probe.saw_parent
              && core_tree_node_value(subtree).parent == tree);
    parse_tree_walk_free(subtree_walk);

    check("a caller's visitor fires once per matching node",
          probe.decls_seen == 2);
    check("the module scope was opened", probe.module_scope != nullptr);
    check("the block scope was opened", probe.block_scope != nullptr);

    if (probe.module_scope && probe.block_scope) {
        check("the block scope nests inside the module scope",
              probe.block_scope->parent == probe.module_scope);
        check("and the module scope is tagged as the annotation wrote it",
              core_string_eq(probe.module_scope->scope_tag, S("module")));
        check("scope depth counts from the file scope",
              probe.module_scope->depth == 1 && probe.block_scope->depth == 2);
    }

    parse_symtab_t *st = r->symtab;

    parse_sym_entry_t *a = parse_symtab_lookup_scoped(st, probe.module_scope,
                                                  core_string_empty(), S("a"));
    parse_sym_entry_t *b_from_module
        = parse_symtab_lookup_scoped(st, probe.module_scope, core_string_empty(),
                                   S("b"));
    parse_sym_entry_t *b_from_block
        = parse_symtab_lookup_scoped(st, probe.block_scope, core_string_empty(),
                                   S("b"));
    parse_sym_entry_t *a_from_block
        = parse_symtab_lookup_scoped(st, probe.block_scope, core_string_empty(),
                                   S("a"));
    parse_sym_entry_t *c = parse_symtab_lookup_scoped(st, probe.module_scope,
                                                  core_string_empty(), S("c"));

    check("a symbol declared at module scope is found there", a != nullptr);
    check("a symbol declared in a block is not visible outside it",
          b_from_module == nullptr);
    check("but is visible from inside the block", b_from_block != nullptr);
    check("and the enclosing scope is still reachable from the block",
          a_from_block == a);
    parse_tree_t *decl = find_node(g, tree, "decl");
    parse_tree_t *name_node = decl ? find_node(g, decl, "name") : nullptr;
    if (name_node && !core_tree_is_leaf(name_node))
        core_tree_node_value(name_node).scope = probe.module_scope;
    check("a reference test has a nonterminal name node",
          name_node && !core_tree_is_leaf(name_node));
    if (name_node)
        core_tree_node_value(name_node).unresolved_name = S("a");
    check("a reference test uses the declared name",
          name_node && core_string_eq(
              parse_tree_leaf_text(core_tree_first_leaf_node(name_node)), S("a")));
    check("a reference node resolves and retains its symbol",
          name_node && parse_symtab_resolve_node(
              st, name_node, core_string_empty()) == a
              && core_tree_node_value(name_node).sym == a);
    bool deferred_scope_resolves = name_node
        && parse_tree_set_resolution_scope(name_node, probe.block_scope)
        && parse_symtab_resolve_node(st, name_node,
                                     core_string_empty()) == a;
    parse_node_resolution_t resolved_scope = name_node
        ? parse_tree_resolution(name_node) : (parse_node_resolution_t){0};
    check("a deferred scope target resolves through that scope and binds the symbol",
          deferred_scope_resolves
          && resolved_scope.kind == PARSE_NODE_RESOLUTION_SYMBOL
          && resolved_scope.target.symbol == a
          && core_tree_node_value(name_node).scope == probe.module_scope);
    check("a scope is still searchable after the walk popped it",
          b_from_block != nullptr && b_from_block->scope == probe.block_scope);
    check("every declaration was recorded", c != nullptr);

    check("@type's argument is typelang, so it becomes that type",
          is_type(c->type, "int"));
    check("a @declares type child that is not typelang gets no type",
          a && a->type == nullptr);

    check("an annotation can read what a later-written annotation produced",
          c && c->type != nullptr);

    check("the @branch node was found", probe.cond_node != nullptr);

    parse_cf_label_t *cf = parse_cf_label_lookup(r, probe.cond_node);

    check("@branch produces a control flow label", cf != nullptr);

    if (cf) {
        check("of the right kind", cf->kind == PARSE_CF_BRANCH);
        check("with all three arms resolved",
              cf->cond && cf->then_body && cf->else_body
                  && cf->cond != cf->then_body
                  && cf->then_body != cf->else_body);
        check("and each arm is the child the annotation named",
              core_string_eq(parse_tree_leaf_text(core_tree_first_leaf_node(cf->cond)), S("x"))
                  && core_string_eq(
                      parse_tree_leaf_text(core_tree_first_leaf_node(cf->then_body)), S("y"))
                  && core_string_eq(
                      parse_tree_leaf_text(core_tree_first_leaf_node(cf->else_body)),
                      S("z")));
    }

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_a_deep_tree_does_not_overflow_the_stack(parse_grammar_t *g)
{
    const char *want = getenv("IR_DEEP");
    const int   N    = want ? atoi(want) : 600;

    char *src = core_alloc_size(1, (size_t)N * 4 + 1);

    for (int i = 0; i < N; i++) {
        memcpy(src + i * 4, "vaT;", 4);
    }

    src[N * 4] = 0;

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "prog", src, &p, &ts);

    if (!tree) {
        check("a deeply nested program parses", false);
        core_free(src);

        return;
    }

    probe_t          probe = {0};
    parse_tree_walk_t *w     = parse_tree_walk_new(g, nullptr);

    parse_walk_on_nt(w, S("decl"), saw_decl, nullptr, &probe);
    parse_tree_walk_run(w, tree);
    parse_tree_walk_free(w);

    check("a walk over a deeply nested tree visits every node",
          probe.decls_seen == N);

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    core_free(src);
}

static parse_nonterm_t *
nt_named(parse_grammar_t *g, const char *name)
{
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (core_string_eq(g->nt_list.data[i]->name, S(name))) {
            return g->nt_list.data[i];
        }
    }

    return nullptr;
}

static void
test_annotation_arguments_only_need_balanced_parens(void)
{
    static const char *src
        = "<top> @declares($1,$2,\"var\") @disambig(pred(a, b, c()), 4) "
          "::= <n> <t>\n"
          "<n> ::= __LOWER\n"
          "<t> ::= __UPPER\n";

    parse_grammar_t *g = parse_grammar_new();

    check("an annotation argument may nest balanced parentheses",
          parse_bnf_load(S(src), S("top"), g));

    parse_nonterm_t *top = nt_named(g, "top");

    check("the annotated non-terminal is there", top != nullptr);

    if (top) {
        parse_annotation_t *d
            = find_unique_rule_annot(g, top, PARSE_ANNOT_DECLARES);

        check("@declares still reads its child references",
              d != nullptr && d->name_ref.kind == PARSE_ROLE_BY_INDEX
                  && d->name_ref.index == 1
                  && d->type_ref.index == 2
                  && core_string_eq(d->sym_kind, S("var")));

        check("the annotation that nests parentheses survives the load",
              find_unique_rule_annot(g, top, PARSE_ANNOT_DISAMBIG) != nullptr);
    }

    parse_grammar_free(g);

    static const char *by_name
        = "<top> @literal(\"int\", $t) ::= <n> <t>\n"
          "<n> ::= __LOWER\n"
          "<t> ::= __UPPER\n";

    parse_grammar_t *g2 = parse_grammar_new();

    check("a reference may name a non-terminal instead of an index",
          parse_bnf_load(S(by_name), S("top"), g2));

    parse_nonterm_t    *t2 = nt_named(g2, "top");
    parse_annotation_t *l  = t2 ? find_unique_rule_annot(g2, t2, PARSE_ANNOT_LITERAL)
                              : nullptr;

    check("and $name carries the name, not an index",
          l != nullptr && l->type_ref.kind == PARSE_ROLE_BY_NAME
              && core_string_eq(l->type_ref.name, S("t"))
              && core_string_eq(l->op_kind, S("int")));

    parse_grammar_free(g2);

    static const char *unbalanced
        = "<top> @disambig(pred(a) ::= <n>\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *bad = parse_grammar_new();

    check("an unbalanced argument region is not a grammar",
          !parse_bnf_load(S(unbalanced), S("top"), bad));

    parse_grammar_free(bad);
}

static void
test_annotation_arguments_carry_integers(void)
{
    static const char *src
        = "<top> @penalty(7) ::= <n>\n"
          "<mid> @disambig(pred, 3) ::= <n>\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *g = parse_grammar_new();

    check("a grammar with integer annotation arguments loads",
          parse_bnf_load(S(src), S("top"), g));

    parse_nonterm_t *top = nt_named(g, "top");
    parse_nonterm_t *mid = nt_named(g, "mid");

    parse_annotation_t *p = top ? find_unique_rule_annot(g, top, PARSE_ANNOT_PENALTY)
                              : nullptr;

    check("@penalty reads the number it was written with",
          p != nullptr && p->penalty_cost == 7);

    parse_annotation_t *d = mid ? find_unique_rule_annot(g, mid, PARSE_ANNOT_DISAMBIG)
                              : nullptr;

    check("@disambig reads its predicate and its cost",
          d != nullptr && core_string_eq(d->disambig_predicate, S("pred"))
              && d->disambig_cost == 3);

    parse_grammar_free(g);

    static const char *const BAD[] = {
        "<top> @penalty(-4) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @penalty(a) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @penalty(1, 2) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @branch($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @assigns($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @scope($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @jump($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @adt(\"enum\", $1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @nospace($1, $2) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @declares(\"v\", $1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @literal(\"int\", t) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @nospace(n) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @varref($) ::= <n>\n<n> ::= __LOWER\n",
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        parse_grammar_t *bad = parse_grammar_new();

        if (parse_bnf_load(S(BAD[i]), S("top"), bad)) {
            printf("FAIL  this should not have loaded: %s", BAD[i]);
            fails++;
        }

        parse_grammar_free(bad);
    }
}

static void
test_emit_annotations_retain_action_and_operands(void)
{
    static const char *const G
        = "<top> @emit(\"add\", $1, $right) ::= <left> <right>\n"
          "<left> ::= __LOWER\n"
          "<right> ::= __UPPER\n";
    parse_grammar_t *g = parse_grammar_new();
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    parse_nonterm_t *top = loaded ? nt_named(g, "top") : nullptr;
    parse_annotation_t *emit = top
        ? find_unique_rule_annot(g, top, PARSE_ANNOT_EMIT) : nullptr;

    check("an emit annotation accepts an action and child references",
          emit && core_string_eq(emit->emit_action, S("add"))
              && core_list_len(emit->emit_operands) == 2);
    check("emit operands retain indexed and named references",
          emit && emit->emit_operands.data[0].kind == PARSE_ROLE_BY_INDEX
              && emit->emit_operands.data[0].index == 1
              && emit->emit_operands.data[1].kind == PARSE_ROLE_BY_NAME
              && core_string_eq(emit->emit_operands.data[1].name, S("right")));

    parse_annotation_t *copy = emit ? parse_annot_clone(emit) : nullptr;
    check("emit annotations clone their action and operand references",
          copy && core_string_eq(copy->emit_action, S("add"))
              && core_list_len(copy->emit_operands) == 2
              && copy->emit_operands.data[1].name.data
                     != emit->emit_operands.data[1].name.data
              && core_string_eq(copy->emit_operands.data[1].name, S("right")));

    parse_annot_free(copy);
    parse_grammar_free(g);

    static const char *const BAD
        = "<top> @emit(add, $1) ::= <left>\n<left> ::= __LOWER\n";
    parse_grammar_t *bad = parse_grammar_new();
    check("an emit action must be a quoted tag",
          !parse_bnf_load(S(BAD), S("top"), bad));
    parse_grammar_free(bad);
}

static void
test_annotation_argument_lists_are_growable(void)
{
    core_buffer_t src = {0};
    static const char prefix[] = "<top> @constrain($1, ";
    core_buffer_append(&src, prefix, sizeof(prefix) - 1);
    for (size_t i = 0; i < 40; i++) {
        if (i) core_buffer_append(&src, "+", 1);
        core_buffer_printf(&src, "Trait%zu", i);
    }
    static const char suffix[] = ") ::= <n>\n<n> ::= __LOWER\n";
    core_buffer_append(&src, suffix, sizeof(suffix) - 1);

    parse_grammar_t *g = parse_grammar_new();
    bool loaded = parse_bnf_load(
        core_string_borrow(src.data, src.byte_len), S("top"), g);
    parse_nonterm_t *top = loaded ? nt_named(g, "top") : nullptr;
    parse_annotation_t *annotation = top
        ? find_unique_rule_annot(g, top, PARSE_ANNOT_CONSTRAIN) : nullptr;

    check("annotation argument lists accept more than 32 entries",
          annotation && core_list_len(annotation->constraints) == 40);

    parse_grammar_free(g);
    core_buffer_release(&src);
}

static void
test_implements_is_checked_not_carried(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <d>\n"
          "<d> @declares($1,\"var\") @type($1, int) @implements($2) "
          "::= \"v\" <n> <i> \";\"\n"
          "<n> ::= __LOWER\n"
          "<i> ::= __UPPER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("an @implements grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "vaR;", &p, &ts);

    if (!tree) {
        check("the declaration parses", false);
        parse_grammar_free(g);

        return;
    }

    th_ctx             *tc = th_ctx_new();
    parse_annot_result_t *r  = parse_annot_result_new(nullptr);

    annotate_once(g, tree, r, tc, nullptr);

    parse_sym_entry_t *d = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !d; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && !strcmp(e->name.data, "a")) {
                d = e;
                break;
            }
        }
    }

    check("the declaration was found", d != nullptr);
    check("@implements against an interface int cannot satisfy is an error",
          th_error_count(tc) > 0);

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_direct_nonterminal_annotation_lookup(void)
{
    static const char *const G
        = "<top> ::= <choice>\n"
          "<choice> @emit(\"identity\", $0) ::= <name>\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    parse_nonterm_t *choice = loaded ? nt_named(g, "choice") : nullptr;
    parse_rule_t *choice_rule = choice
        ? parse_get_rule(g, choice->rule_ids.data[0]) : nullptr;
    check("an exact production query finds its annotation",
          choice_rule
              && parse_rule_find_annot(g, choice_rule, PARSE_ANNOT_EMIT));
    check("a direct nonterminal query does not inherit a production annotation",
          choice && !parse_nt_find_direct_annot(choice, PARSE_ANNOT_EMIT));
    parse_grammar_free(g);
}

static void
test_an_adt_builds_a_variant_type(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <adt>\n"
          "<adt> @adt(\"union\", $1, \"arm\", $0) ::= \"u\" <n> \"{\" <as> \"}\"\n"
          "<as> ::= <arm> <as> | <arm>\n"
          "<arm> @declares($1,\"var\") @type($1, int) ::= <n> \";\"\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("an adt grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "ut{a;b;}", &p, &ts);

    if (!tree) {
        check("the adt parses", false);
        parse_grammar_free(g);

        return;
    }

    types = types ? types : th_ctx_new();

    parse_annot_result_t *r = parse_annot_result_new(nullptr);

    annotate_once(g, tree, r, types, nullptr);

    th_type *var = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !var; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->kind == PARSE_SYM_TAG && e->type) {
                var = e->type;
                break;
            }
        }
    }

    check("@adt gave its tag symbol a type", var != nullptr);
    bool is_variant = false;

    check("and the type reached th as a variant",
          var && th_named_info(var, nullptr, nullptr, &is_variant, nullptr)
              && is_variant);
    check("with one argument per arm", var && th_named_nargs(var) == 2);

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_the_repr_annotations_reach_the_layout_descriptor(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <rec> <access>\n"
          "<rec> @scope(\"record\") @record($1) @size(8) @alignment(4) "
          "::= \"r\" <n> \"{\" <fs> \"}\"\n"
          "<fs> ::= <f> <fs> | <meth> <fs> | <f> | <meth>\n"
          "<f> @field($1) @offset(0) @width(4) ::= <n> \";\"\n"
          "<meth> @method($1) @type($1, \"() -> int\") ::= \"m\" <n>\n"
          "<access> @infer(\"method_return($0)\") ::= <use> %\".\" <n>\n"
          "<use> @varref($0) ::= <n>\n"
          "<n> ::= __LOWER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    bool grammar_loaded = parse_bnf_load(S(G), S("top"), g);
    check("a record grammar with representation pins loads", grammar_loaded);
    if (!grammar_loaded) {
        fprintf(stderr, "record grammar load error: %s\n", parse_bnf_error());
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "rt{a;mf}t.f", &p, &ts);

    if (!tree) {
        check("the record parses", false);
        parse_grammar_free(g);

        return;
    }

    ir_ctx_t    *ctx = ir_ctx_new();
    ir_module_t *m   = ir_module_new(ctx, "m", &ir_profile_bce, "host");

    parse_annot_result_t *r = parse_annot_result_new(nullptr);

    annotate_once(g, tree, r, ir_module_types(m), m);

    th_type *rec = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s && !rec; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->type && th_kind_of(e->type) == TH_RECORD) {
                rec = e->type;
                break;
            }
        }
    }

    check("@record established a record type", rec != nullptr);

    ir_descriptor_t *d = rec ? ir_layout_of(m, rec, nullptr) : nullptr;

    check("the annotations reached a layout descriptor", d != nullptr);
    check("@size pinned the descriptor's size",
          d && ir_layout_is_settled(&d->size) && d->size.value == 8);
    check("@alignment pinned the descriptor's alignment",
          d && ir_layout_is_settled(&d->align) && d->align.value == 4);

    th_type *method = rec ? th_method_lookup(ir_module_types(m), rec, "f")
                          : nullptr;
    th_type *method_return = method ? th_func_ret(method) : nullptr;
    const char *return_name = nullptr;
    size_t return_name_len = 0;

    check("@method registers its signature on the record type",
          method != nullptr);
    check("the registered method preserves its return type",
          method_return
              && th_named_info(method_return, &return_name, &return_name_len,
                               nullptr, nullptr)
              && return_name_len == 3 && !memcmp(return_name, "int", 3));

    parse_tree_t *access = find_node(g, tree, "access");
    th_type *access_type = access ? core_tree_node_value(access).type : nullptr;
    const char *access_name = nullptr;
    size_t access_name_len = 0;

    check("method_return resolves through the receiver method registry",
          access_type
              && th_named_info(access_type, &access_name, &access_name_len,
                               nullptr, nullptr)
              && access_name_len == 3 && !memcmp(access_name, "int", 3));

    parse_annot_result_free(r);
    ir_ctx_free(ctx);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_trait_method_inference_uses_visible_requirement(void)
{
    static const char *const G =
        "<top> @tokenizer(\"toy\") ::= <postfix-expr>\n"
        "<postfix-expr> @infer(\"return_of($0)\") ::= "
        "<postfix-expr> %\"(\" %\")\"\n"
        "<postfix-expr> @infer(\"field_return($0)\") ::= "
        "<use> %\".\" <n>\n"
        "<use> @infer(\"`a\") ::= <n>\n"
        "<n> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    check("trait method inference grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = parse_toy(g, "top", "x.r()", &parser, &tokens);
    check("trait method call inference expression parses", tree != nullptr);
    if (!tree) {
        if (parser) parse_pwz_free(parser);
        if (tokens) scan_token_stream_free(tokens);
        parse_grammar_free(g);
        return;
    }

    th_ctx *definitions = th_ctx_new();
    th_type *self = th_var(definitions);
    th_param required_param = {.mode = TH_IN, .type = self};
    th_member requirement = {
        .name = "r",
        .type = th_func(definitions, th_name(definitions, "string"),
                        &required_param, 1, nullptr, nullptr, 0),
    };
    th_env *environment = th_env_new();
    th_param implementation_param = {
        .mode = TH_IN, .type = th_name(definitions, "int")};
    bool registered =
        th_env_register_trait(environment, "Render", "traits", "base",
                              TH_ENV_PUBLIC,
                              th_record(definitions, &requirement, 1)) &&
        th_env_register_function(
            environment, "base.int.render", "r", "ints", "base",
            TH_ENV_PUBLIC,
            th_func(definitions, th_name(definitions, "string"),
                    &implementation_param, 1, nullptr, nullptr, 0)) &&
        th_env_freeze(environment);
    th_ctx *inference = th_ctx_new();
    bool attached = registered && th_ctx_set_environment(
        inference, environment, "client", "base");
    parse_annot_result_t *annotations = parse_annot_result_new(nullptr);
    if (attached) annotate_once(g, tree, annotations, inference, nullptr);
    parse_tree_t *access = tree;
    parse_tree_t *use = find_node(g, tree, "use");
    th_type *actual = access ? core_tree_node_value(access).type : nullptr;
    th_type *receiver = use ? core_tree_node_value(use).type : nullptr;
    const char *obligations[1] = {0};
    check("unique trait method infers its return and retains receiver obligation",
          attached && actual && receiver &&
              th_unify(inference, actual,
                       th_name(inference, "string")) != nullptr &&
              th_infer_ifaces(inference, receiver, obligations, 1) == 1 &&
              !strcmp(obligations[0], "Render"));
    check("trait method obligation rejects a type with no implementation",
          receiver && !th_unify(inference, receiver,
                                th_name(inference, "string")));
    check("later receiver unification discharges the trait obligation",
          receiver && th_unify_in_place(inference, receiver,
                                        th_name(inference, "int")));

    parse_pwz_parser_t *multi_parser = nullptr;
    parse_token_stream_t *multi_tokens = nullptr;
    parse_tree_t *multi_tree = parse_toy(g, "top", "x.r()", &multi_parser,
                                          &multi_tokens);
    th_ctx *multi_definitions = th_ctx_new();
    th_type *render_self = th_var(multi_definitions);
    th_type *other_self = th_var(multi_definitions);
    th_env *multi_env = th_env_new();
    th_type *int_type = th_name(multi_definitions, "int");
    th_type *string_type = th_name(multi_definitions, "string");
    th_type *bool_type = th_name(multi_definitions, "bool");
    th_param render_param = {.mode = TH_IN, .type = int_type};
    bool multi_registered = multi_tree &&
        th_env_register_trait(
            multi_env, "Render", "traits", "base", TH_ENV_PUBLIC,
            th_record(multi_definitions,
                      &(th_member){.name = "r", .type = th_func(
                          multi_definitions, string_type,
                          (th_param[]){{.mode = TH_IN, .type = render_self}},
                          1, nullptr, nullptr, 0)}, 1)) &&
        th_env_register_trait(
            multi_env, "OtherRender", "traits", "base", TH_ENV_PUBLIC,
            th_record(multi_definitions,
                      &(th_member){.name = "r", .type = th_func(
                          multi_definitions, bool_type,
                          (th_param[]){{.mode = TH_IN, .type = other_self}},
                          1, nullptr, nullptr, 0)}, 1)) &&
        th_env_register_function(
            multi_env, "base.int.render", "r", "ints", "base",
            TH_ENV_PUBLIC,
            th_func(multi_definitions, string_type, &render_param, 1,
                    nullptr, nullptr, 0)) &&
        th_env_freeze(multi_env);
    th_ctx *multi_inference = th_ctx_new();
    bool multi_attached = multi_registered && th_ctx_set_environment(
        multi_inference, multi_env, "client", "base");
    parse_annot_result_t *multi_annotations = parse_annot_result_new(nullptr);
    if (multi_attached) annotate_once(g, multi_tree, multi_annotations,
                                     multi_inference, nullptr);
    parse_tree_t *multi_access = multi_tree;
    parse_tree_t *multi_use = find_node(g, multi_tree, "use");
    th_type *multi_actual = multi_access
        ? core_tree_node_value(multi_access).type : nullptr;
    th_type *multi_receiver = multi_use
        ? core_tree_node_value(multi_use).type : nullptr;
    check("ambiguous visible method names retain a deferred result obligation",
          multi_attached && multi_actual && multi_receiver &&
              th_unify_in_place(multi_inference, multi_receiver,
                                th_name(multi_inference, "int")) &&
              th_unify(multi_inference, multi_actual,
                       th_name(multi_inference, "string")));
    parse_annot_result_free(multi_annotations);
    th_ctx_free(multi_inference);
    th_env_free(multi_env);
    th_ctx_free(multi_definitions);
    parse_pwz_free(multi_parser);
    scan_token_stream_free(multi_tokens);

    parse_annot_result_free(annotations);
    th_ctx_free(inference);
    th_env_free(environment);
    th_ctx_free(definitions);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(g);
}

static void
test_ordering_inference_uses_comparable_trait(void)
{
    static const char *const G =
        "<top> @tokenizer(\"toy\") ::= <comparison>\n"
        "<comparison> @infer(\"compare($0, $2)\") ::= <left> %\"<\" <right>\n"
        "<left> @infer(\"Meter\") ::= __LOWER\n"
        "<right> @infer(\"Meter\") ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    check("ordering inference grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = parse_toy(g, "top", "x<x", &parser, &tokens);
    check("ordering expression parses", tree != nullptr);
    if (!tree) {
        if (parser) parse_pwz_free(parser);
        if (tokens) scan_token_stream_free(tokens);
        parse_grammar_free(g);
        return;
    }

    th_ctx *definitions = th_ctx_new();
    th_type *self = th_var(definitions);
    th_param required_params[] = {
        {.mode = TH_IN, .type = self},
        {.mode = TH_IN, .type = self},
    };
    th_type *compare_code = th_bitint(definitions, 2, true);
    th_member requirement = {
        .name = "cmp",
        .type = th_func(definitions,
                        compare_code, required_params, 2, nullptr, nullptr, 0),
    };
    th_param implementation_params[] = {
        {.mode = TH_IN, .type = th_name(definitions, "Meter")},
        {.mode = TH_IN, .type = th_name(definitions, "Meter")},
    };
    th_env *environment = th_env_new();
    bool trait_registered = th_env_register_trait(environment,
        "Comparable", "traits", "base", TH_ENV_PUBLIC,
        th_record(definitions, &requirement, 1));
    bool function_registered = trait_registered &&
        th_env_register_function(environment, "base.meter.cmp", "cmp",
            "meter", "base", TH_ENV_PUBLIC,
            th_func(definitions, compare_code,
                    implementation_params, 2, nullptr, nullptr, 0));
    bool registered = function_registered && th_env_freeze(environment);
    th_ctx *inference = th_ctx_new();
    bool attached = registered &&
        th_ctx_set_environment(inference, environment, "client", "base");
    parse_annot_result_t *annotations = parse_annot_result_new(nullptr);
    if (attached) annotate_once(g, tree, annotations, inference, nullptr);
    parse_tree_t *comparison = find_node(g, tree, "comparison");
    th_type *actual = comparison
        ? core_tree_node_value(comparison).type : nullptr;
    check("Comparable test declarations register", registered);
    check("Comparable return type parses as i:2", compare_code != nullptr);
    check("Comparable trait registers", trait_registered);
    check("Comparable candidate registers", function_registered);
    check("Comparable environment attaches", attached);
    check("comparison node receives an inferred type", actual != nullptr);
    check("ordering inference validates Comparable.cmp and returns bool",
          attached && actual &&
              th_unify(inference, actual, th_name(inference, "bool")));

    parse_annot_result_free(annotations);
    th_ctx_free(inference);
    th_env_free(environment);
    th_ctx_free(definitions);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(g);
}

static void
test_index_inference_uses_index_load_trait(void)
{
    static const char *const G =
        "<top> @tokenizer(\"toy\") ::= <access>\n"
        "<access> @infer(\"element_of($0, load, store)\") ::= <receiver> %\"[\" <index-contents> %\"]\"\n"
        "<index-contents> ::= <expression>\n"
        "<receiver> @infer(\"OpaqueBox[int]\") ::= __LOWER\n"
        "<expression> @infer(\"int\") ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    check("index inference grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = parse_toy(g, "top", "a[i]", &parser, &tokens);
    check("single-index expression parses", tree != nullptr);
    if (!tree) {
        if (parser) parse_pwz_free(parser);
        if (tokens) scan_token_stream_free(tokens);
        parse_grammar_free(g);
        return;
    }

    th_ctx *definitions = th_ctx_new();
    th_type *receiver_var = th_var(definitions);
    th_type *string_type = th_name(definitions, "string");
    th_type *int_type = th_name(definitions, "int");
    th_type *indexable = th_apply(
        definitions, "OpaqueBox",
        (th_arg[]){{.kind = TH_ARG_TYPE, .type = int_type}}, 1);
    th_param required_params[] = {
        {.mode = TH_IN, .type = receiver_var},
        {.mode = TH_IN, .type = int_type},
    };
    th_param implementation_params[] = {
        {.mode = TH_IN, .type = indexable},
        {.mode = TH_IN, .type = int_type},
    };
    th_member required = {
        .name = "load",
        .type = th_func(definitions, th_var(definitions), required_params, 2,
                        nullptr, nullptr, 0),
    };
    th_env *environment = th_env_new();
    bool registered = th_env_register_trait(
        environment, "Index_Load", "builtins", "base", TH_ENV_PUBLIC,
        th_record(definitions, &required, 1)) &&
        th_env_register_function(
            environment, "base.opaque_box.load", "load", "opaque_box",
            "base", TH_ENV_PUBLIC,
            th_func(definitions, string_type, implementation_params, 2,
                    nullptr, nullptr, 0)) && th_env_freeze(environment);
    th_ctx *inference = th_ctx_new();
    bool attached = registered && th_ctx_set_environment(
        inference, environment, "client", "base");
    parse_annot_result_t *annotations = parse_annot_result_new(nullptr);
    if (attached) annotate_once(g, tree, annotations, inference, nullptr);
    parse_tree_t *access = find_node(g, tree, "access");
    th_type *actual = access ? core_tree_node_value(access).type : nullptr;
    check("index result comes from the Index_Load contract",
          attached && actual &&
              th_hash(inference, actual) ==
                  th_hash(inference, th_name(inference, "string")));

    parse_annot_result_free(annotations);
    th_ctx_free(inference);
    th_env_free(environment);
    th_ctx_free(definitions);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(g);
}

static void
test_slice_inference_uses_index_slice_trait(void)
{
    static const char *const G =
        "<top> @tokenizer(\"toy\") ::= <access>\n"
        "<access> @infer(\"slice_of($0, slice)\") ::= <receiver> %\"[\" <index-contents> %\"]\"\n"
        "<index-contents> ::= <expression> %\":\" <expression>\n"
        "                   | <expression> %\":\"\n"
        "                   | %\":\" <expression>\n"
        "                   | %\":\"\n"
        "<receiver> @infer(\"OpaqueBox[int]\") ::= __LOWER\n"
        "<expression> @infer(\"int\") ::= __DIGIT\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    check("slice inference grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(g);
        return;
    }

    th_ctx *definitions = th_ctx_new();
    th_type *receiver_var = th_var(definitions);
    th_type *item_var = th_var(definitions);
    th_arg view_item = {.kind = TH_ARG_TYPE, .type = item_var};
    th_type *view_of_item = th_apply(definitions, "view", &view_item, 1);
    th_type *integer = th_name(definitions, "int");
    th_param requirement_params[] = {
        {.mode = TH_IN, .type = receiver_var},
        {.mode = TH_IN, .type = integer},
        {.mode = TH_IN, .type = integer},
    };
    th_member requirement = {
        .name = "slice",
        .type = th_func(definitions, view_of_item, requirement_params, 3,
                        nullptr, nullptr, 0),
    };
    th_type *int_box = th_apply(
        definitions, "OpaqueBox",
        (th_arg[]){{.kind = TH_ARG_TYPE, .type = integer}}, 1);
    th_arg view_of_int_arg = {.kind = TH_ARG_TYPE, .type = integer};
    th_type *view_of_int = th_apply(definitions, "view", &view_of_int_arg, 1);
    th_param implementation_params[] = {
        {.mode = TH_IN, .type = int_box},
        {.mode = TH_IN, .type = integer},
        {.mode = TH_IN, .type = integer},
    };
    th_env *environment = th_env_new();
    bool registered = th_env_register_trait(
        environment, "Index_Slice", "builtins", "base", TH_ENV_PUBLIC,
        th_record(definitions, &requirement, 1)) &&
        th_env_register_function(
            environment, "base.opaque_box.slice", "slice", "opaque_box",
            "base", TH_ENV_PUBLIC,
            th_func(definitions, view_of_int, implementation_params, 3,
                    nullptr, nullptr, 0)) && th_env_freeze(environment);
    th_ctx *inference = th_ctx_new();
    bool attached = registered && th_ctx_set_environment(
        inference, environment, "client", "base");
    const char *sources[] = {"a[2:4]", "a[2:]", "a[:4]", "a[:]"};
    bool all_inferred = attached;
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        parse_pwz_parser_t *parser = nullptr;
        parse_token_stream_t *tokens = nullptr;
        parse_tree_t *tree = parse_toy(g, "top", sources[i], &parser, &tokens);
        parse_annot_result_t *annotations = parse_annot_result_new(nullptr);
        bool ok = tree != nullptr;
        if (ok) annotate_once(g, tree, annotations, inference, nullptr);
        parse_tree_t *access = tree ? find_node(g, tree, "access") : nullptr;
        th_type *actual = access ? core_tree_node_value(access).type : nullptr;
        all_inferred = all_inferred && ok && actual &&
            th_hash(inference, actual) == th_hash(inference, view_of_int);
        parse_annot_result_free(annotations);
        if (parser) parse_pwz_free(parser);
        if (tokens) scan_token_stream_free(tokens);
    }
    check("Index_Slice declares int bounds and a view result", registered);
    check("all four slice forms infer view[T] through Index_Slice.slice",
          all_inferred);

    th_ctx_free(inference);
    th_env_free(environment);
    th_ctx_free(definitions);
    parse_grammar_free(g);
}

static void
test_the_representation_and_declaration_annotations(void)
{
    static const char *const G
        = "<top> @scope(\"module\") ::= <d> <b> <p>\n"
          "<d> @declares($1,\"var\") @mutability($1,\"const\") "
          "@constrain($1, == int) @namespace(\"tag\") ::= \"v\" <n> \";\"\n"
          "<b> @size($2) @width($2) ::= \"b\" %\":\" <num>\n"
          "<p> @declares($1,\"param\") @direction($1,\"out\") "
          "@alignment(8) @offset(sizeof(int)) @pass_by(\"ref\") ::= \"p\" <n> \";\"\n"
          "<n> ::= __LOWER\n"
          "<num> ::= __DIGIT\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    check("a grammar using every Part 2 annotation loads",
          parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_tree_t   *tree = parse_toy(g, "top", "va;b:4pq;", &p, &ts);

    if (!tree) {
        check("the program parses", false);
        parse_grammar_free(g);

        return;
    }

    types = types ? types : th_ctx_new();

    parse_annot_result_t *r = annotate_new(g, tree, types);

    parse_sym_entry_t *a = nullptr;
    parse_sym_entry_t *q = nullptr;

    for (parse_scope_t *s = r->symtab->all_scopes; s; s = s->all_next) {
        for (parse_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (e->name.data && !strcmp(e->name.data, "a")) {
                a = e;
            }

            if (e->name.data && !strcmp(e->name.data, "q")) {
                q = e;
            }
        }
    }

    check("@mutability sets the symbol's mutability",
          a && a->mutability == PARSE_SYM_CONST);
    th_constraint con  = {0};
    bool          got  = a && a->type
                      && th_var_constraint_count(a->type) == 1
                      && th_var_constraint_at(a->type, 0, &con);

    check("@constrain put a constraint on the symbol's type variable", got);
    check("and it is the == constraint the annotation wrote",
          got && con.kind == TH_CON_UNIFIES);
    check("@direction records the parameter's direction as a th_mode",
          q && q->has_mode && q->mode == TH_OUT);
    check("@direction preserves the source-level mode spelling",
          q && core_string_eq(q->direction, S("out")));
    check("@pass_by records a separate parameter passing strategy",
          q && core_string_eq(q->pass_by, S("ref")));
    check("@namespace put the declaration in the namespace it named",
          a && a->scope && a->scope->ns
              && a->scope->ns->ns_name.data
              && !strcmp(a->scope->ns->ns_name.data, "tag"));

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_initial_symbol_walk_callback(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "vaT;{vbU;}?xyztcV;",
                                   &p, &ts);
    check("initial-symbol sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    node_probe_t probe = {0};
    parse_annot_result_set_node_callback(result, count_annot_nodes, &probe);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result, nullptr, nullptr);
    parse_tree_walk_run(walk, tree);

    check("annotation walk visits each nonterminal on entry and exit",
          probe.enters > 0 && probe.enters == probe.exits);
    check("annotation walk creates its module scope",
          core_tree_node_value(tree).scope != nullptr);
    parse_tree_t *decl = find_node(g, tree, "decl");
    check("ordinary nodes retain their enclosing lexical scope",
          decl && core_tree_node_value(decl).scope
              == core_tree_node_value(tree).scope);
    check("annotation walk records the declared name",
          parse_symtab_lookup_scoped(result->symtab,
              core_tree_node_value(tree).scope, S(""), S("a")) != nullptr);
    parse_sym_entry_t *inferred = parse_symtab_lookup_scoped(
        result->symtab, core_tree_node_value(tree).scope, S(""), S("a"));
    parse_sym_mark_type_inferred(inferred);
    check("inferred type state is stored on its symbol",
          inferred && inferred->type_is_inferred);
    check("symbol retains its declaration node",
          inferred && inferred->decl_node != nullptr);
    check("symbol accepts a definition site", parse_sym_add_definition(inferred, tree));
    check("duplicate definition sites are ignored", parse_sym_add_definition(inferred, tree));
    check("symbol accepts a use site", parse_sym_add_use(inferred, tree));
    check("duplicate use sites are ignored", parse_sym_add_use(inferred, tree));
    check("symbol definition and use sites are stored without duplicates",
          inferred && inferred->definition_count == 1
              && inferred->definition_nodes[0] == tree
              && inferred->use_count == 1 && inferred->use_nodes[0] == tree);
    check("a classified write can be removed from use sites",
          parse_sym_remove_use(inferred, tree)
              && inferred->use_count == 0 && inferred->uses == 0);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static void
test_selected_type_annotations_per_node(parse_grammar_t *g)
{
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "prog", "tbI;", &p, &ts);
    check("per-node type annotation sample parses", tree != nullptr);
    if (!tree) return;

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result, types, nullptr);
    const parse_annot_kind_t kinds[] = {
        PARSE_ANNOT_SCOPE_OPEN,
        PARSE_ANNOT_DECLARES,
        PARSE_ANNOT_TYPE,
    };
    check("type annotation subset is accepted",
          parse_walk_set_annotation_order(walk, kinds,
                                          sizeof(kinds) / sizeof(kinds[0])));
    parse_tree_walk_run(walk, tree);

    parse_scope_t *scope = core_tree_node_value(tree).scope;
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        result->symtab, scope, S(""), S("b"));
    check("selected declaration and type annotations bind an explicitly typed symbol",
          symbol && is_type(symbol->type, "int"));
    check("per-node entry and exit preserve the module scope tree",
          scope && core_string_eq(scope->scope_tag, S("module"))
              && parse_symtab_current_scope(result->symtab, S(""))
                     == scope->parent);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
}

static th_type *
resolve_test_type_node(th_ctx *tc, parse_grammar_t *grammar,
                      parse_tree_t *node, void *context)
{
    (void)grammar;
    (void)node;
    (*(size_t *)context)++;
    return th_name(tc, "resolver_type");
}

static void
test_frontend_type_node_resolver(void)
{
    parse_grammar_t *grammar = load_toy();
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = grammar
        ? parse_toy(grammar, "prog", "taI;", &parser, &tokens) : nullptr;
    th_ctx *tc = th_ctx_new();
    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    size_t resolver_calls = 0;
    parse_annot_result_set_type_node_resolver(result,
        resolve_test_type_node, &resolver_calls);
    parse_tree_t *type_node = tree ? find_node(grammar, tree, "type")
                                   : nullptr;
    th_type *resolved = parse_annot_resolve_type_node(result, tc, grammar,
                                                       type_node);
    bool resolver_type = resolved
        && th_hash(tc, resolved) == th_hash(tc, th_name(tc, "resolver_type"));
    check("frontend type-node resolver is invoked for a type annotation",
          type_node && resolver_calls == 1);
    check("frontend type-node resolver supplies the annotation type",
          resolver_type);
    th_type *cached_type = th_name(tc, "cached_type");
    if (type_node) core_tree_node_value(type_node).type = cached_type;
    parse_annot_result_set_type_node_resolver(result, nullptr, nullptr);
    resolved = parse_annot_resolve_type_node(result, tc, grammar, type_node);
    check("frontend type-node resolution uses the node's cached type",
          resolved && th_hash(tc, resolved) == th_hash(tc, cached_type));
    if (type_node) core_tree_node_value(type_node).type = nullptr;
    resolved = parse_annot_resolve_type_node(result, tc, grammar, type_node);
    check("frontend type-node resolution does not re-walk uncached syntax",
          resolved == nullptr);
    parse_annot_result_free(result);
    th_ctx_free(tc);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_inference_result_is_propagated(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <decl>\n"
          "<decl> @declares($1, \"var\") @infer(\"$1 unify int\") "
          "::= \"v\" <name> \";\"\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("inference propagation grammar loads", parse_bnf_load(S(G), S("top"), g));
    if (!parse_find_nonterm(g, S("top"))) {
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "va;", &p, &ts);
    check("inference propagation sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result, types, nullptr);
    const parse_annot_kind_t kinds[] = {
        PARSE_ANNOT_SCOPE_OPEN,
        PARSE_ANNOT_DECLARES,
        PARSE_ANNOT_INFER,
    };
    check("inference annotation subset is accepted",
          parse_walk_set_annotation_order(walk, kinds,
                                          sizeof(kinds) / sizeof(kinds[0])));
    parse_tree_walk_run(walk, tree);

    parse_scope_t *scope = core_tree_node_value(tree).scope;
    parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
        result->symtab, scope, S(""), S("a"));
    check("the infer annotation's unified result reaches its symbol",
          symbol && is_type(symbol->type, "int"));

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_symbol_records_declaration_definition_and_use_nodes(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <decl> <read>\n"
          "<decl> @declares($1, \"var\") @assigns($1, $3) ::= "
          "\"v\" <name> \"=\" <name> \";\"\n"
          "<read> ::= \"r\" <ref> \";\"\n"
          "<ref> @varref($0) ::= <name>\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(S(G), S("top"), grammar);
    check("symbol site grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(grammar);
        return;
    }

    parse_pwz_parser_t *parser;
    parse_token_stream_t *tokens;
    parse_tree_t *tree = parse_toy(grammar, "top", "va=b;ra;",
                                   &parser, &tokens);
    check("symbol site sample parses", tree != nullptr);
    if (tree) {
        parse_annot_result_t *result = annotate_new(grammar, tree, nullptr);
        parse_scope_t *scope = core_tree_node_value(tree).scope;
        parse_sym_entry_t *symbol = parse_symtab_lookup_scoped(
            result->symtab, scope, S(""), S("a"));
        parse_tree_t *declaration = find_node(grammar, tree, "decl");
        parse_tree_t *use = find_node(grammar, tree, "ref");
        check("symbol retains its declaration node",
              symbol && symbol->decl_node == declaration);
        check("assignment annotation records the definition node",
              symbol && symbol->definition_count == 1
                  && symbol->definition_nodes[0] == declaration);
        check("variable-reference annotation records the use node",
              symbol && symbol->use_count == 1
                  && symbol->use_nodes[0] == use && symbol->uses == 1);
        parse_annot_result_free(result);
        parse_pwz_free(parser);
        scan_token_stream_free(tokens);
    }
    parse_grammar_free(grammar);
}

static parse_tree_t *
last_node(parse_grammar_t *g, parse_tree_t *node, const char *name)
{
    if (!node || core_tree_is_leaf(node)) return nullptr;
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_tree_t *found = last_node(g, core_tree_child(node, i - 1),
                                        name);
        if (found) return found;
    }
    return parse_tree_nt_name(g, node).data
        && core_string_eq(parse_tree_nt_name(g, node), S(name))
        ? node : nullptr;
}

typedef enum {
    INITIALIZER_TARGET_NONE,
    INITIALIZER_TARGET_SELF,
    INITIALIZER_TARGET_EARLIER,
} initializer_target_t;

static initializer_target_t
initializer_reference_target(const char *point)
{
    core_buffer_t text = {0};
    core_buffer_printf(&text,
        "@tokenizer(\"toy\")\n%s"
        "<top> @scope(\"module\") ::= <decl> <decl>\n"
        "<decl> @declares($1, \"var\") @assigns($1, $3) ::= "
        "\"v\" <name> \"=\" <ref> \";\"\n"
        "<ref> @varref($0) ::= <name>\n"
        "<name> ::= __LOWER\n", point);
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(core_string_borrow(text.data,
                                                    text.byte_len),
                                 S("top"), grammar);
    core_buffer_release(&text);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "va=b;va=a;", &parser, &tokens)
        : nullptr;
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree, nullptr) : nullptr;
    parse_tree_t *declaration = tree ? last_node(grammar, tree, "decl")
                                     : nullptr;
    parse_tree_t *use = tree ? last_node(grammar, tree, "ref") : nullptr;
    parse_node_resolution_t resolution = use ? parse_tree_resolution(use)
        : (parse_node_resolution_t){0};
    parse_sym_entry_t *target = resolution.kind
            == PARSE_NODE_RESOLUTION_SYMBOL
        ? resolution.target.symbol : nullptr;
    bool self = target && target->decl_node == declaration;
    bool earlier = target && target->decl_node != declaration
        && target->decl_node;
    if (result) parse_annot_result_free(result);
    if (parser) parse_pwz_free(parser);
    if (tokens) scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
    return self ? INITIALIZER_TARGET_SELF
        : earlier ? INITIALIZER_TARGET_EARLIER : INITIALIZER_TARGET_NONE;
}

static void
test_declaration_point_hides_a_binding_in_its_initializer(void)
{
    check("by default a binding is visible in its own initializer",
          initializer_reference_target("") == INITIALIZER_TARGET_SELF);
    check("a declarator declaration point keeps the default",
          initializer_reference_target(
              "@declaration_point(\"declarator\")\n")
              == INITIALIZER_TARGET_SELF);
    check("after_initializer resolves the initializer to the earlier binding",
          initializer_reference_target(
              "@declaration_point(\"after_initializer\")\n")
              == INITIALIZER_TARGET_EARLIER);
}

static void
test_symbol_lookup_instantiates_schemes(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <expr>\n"
          "<expr> @declares($1, \"function\") @visibility(\"public\") "
          "::= \"v\" <name> \";\"\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("scheme lookup grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "va;", &p, &ts);
    check("scheme lookup sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    th_ctx *tc = th_ctx_new();
    parse_annot_result_t *result = annotate_new(g, tree, tc);
    parse_scope_t *scope = core_tree_node_value(tree).scope;
    parse_tree_t *expr_node = find_node(g, tree, "expr");
    th_type *variable = th_var(tc);
    th_param param = {.mode = TH_IN, .type = variable};
    th_type *identity = th_func(tc, variable, &param, 1, nullptr, nullptr, 0);
    parse_symtab_enter_scope(result->symtab, S(""), scope);
    parse_sym_entry_t *symbol = parse_symtab_lookup_in(scope, S("a"));
    symbol->type = th_generalize(tc, identity);
    parse_symtab_enter_scope(result->symtab, S(""), scope->parent);

    parse_infer_expr_t *lookup = parse_infer_parse(S("lookup($1)"));
    parse_visit_t visit = {
        .grammar = g,
        .node = expr_node,
        .pn = &core_tree_node_value(expr_node),
    };
    visit.pn->scope = scope;
    bool ok_a = false, ok_b = false;
    th_type *use_a = parse_infer_eval(lookup, &visit, tc, result->symtab,
                                     &ok_a);
    th_type *use_b = parse_infer_eval(lookup, &visit, tc, result->symtab,
                                     &ok_b);
    check("each lookup instantiates the exported scheme", ok_a && ok_b
              && use_a && use_b && !th_is_scheme(use_a)
              && !th_is_scheme(use_b) && use_a != use_b);
    check("per-node annotation clients retain declared visibility",
          core_string_eq(symbol->visibility, S("public")));

    parse_infer_free(lookup);
    parse_annot_result_free(result);
    th_ctx_free(tc);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_generic_call_result_tracks_argument_type(void)
{
    static const char *const G =
        "<top> @tokenizer(\"toy\") ::= <postfix-expr>\n"
        "<postfix-expr> ::= <postfix-expr> %\"(\" <call-args> %\")\" | <name>\n"
        "<call-args> ::= <positional-args>\n"
        "<positional-args> ::= <expression>\n"
        "<expression> ::= <name>\n"
        "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    check("generic call inference grammar loads", loaded);
    if (!loaded) {
        parse_grammar_free(g);
        return;
    }

    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = parse_toy(g, "top", "f(x)", &parser, &tokens);
    check("generic call inference sample parses", tree != nullptr);
    if (!tree) {
        if (parser) parse_pwz_free(parser);
        if (tokens) scan_token_stream_free(tokens);
        parse_grammar_free(g);
        return;
    }

    parse_tree_t *call = find_node(g, tree, "postfix-expr");
    parse_tree_t *callee = call && core_tree_num_children(call) == 4
        ? core_tree_child(call, 0) : nullptr;
    parse_tree_t *argument = find_node(g, tree, "expression");
    th_ctx *types = th_ctx_new();
    th_type *item = th_var(types);
    th_type *argument_type = th_var(types);
    th_param parameter = {.mode = TH_IN, .type = item};
    parse_symtab_t *symbols = parse_symtab_new();
    parse_sym_entry_t *function = parse_symtab_add(symbols, S("external"),
        S("f"), PARSE_SYM_FUNCTION, nullptr);
    if (function)
        function->type = th_func(types, item, &parameter, 1,
                                 nullptr, nullptr, 0);
    bool attached = callee && function && function->type
        && parse_tree_set_resolution_symbol(callee, function);
    parse_infer_expr_t *lookup = parse_infer_parse(S("lookup($0)"));
    parse_visit_t callee_visit = {
        .grammar = g,
        .node = callee,
        .pn = callee ? &core_tree_node_value(callee) : nullptr,
    };
    bool lookup_valid = false;
    if (attached && lookup)
        core_tree_node_value(callee).type = parse_infer_eval(lookup,
            &callee_visit, types, symbols, &lookup_valid);
    if (argument)
        core_tree_node_value(argument).type = argument_type;
    parse_infer_expr_t *inference = parse_infer_parse(S("return_of($0)"));
    parse_visit_t visit = {
        .grammar = g,
        .node = call,
        .pn = call ? &core_tree_node_value(call) : nullptr,
    };
    bool valid = false;
    th_type *result = call && callee && argument && inference
        ? parse_infer_eval(inference, &visit, types, nullptr, &valid)
        : nullptr;
    th_type *uint_type = th_name(types, "uint");
    check("generic call binds its result to the argument node",
          attached && lookup_valid && valid && result && uint_type
              && th_unify_in_place(types, argument_type, uint_type)
              && th_resolve(result) == th_resolve(uint_type));

    parse_infer_free(lookup);
    parse_infer_free(inference);
    parse_symtab_free(symbols);
    th_ctx_free(types);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(g);
}

static void
test_error_propagation_preserves_return_type(void)
{
    static const char *const G =
        "@tokenizer(\"toy\")\n"
        "<top> ::= <postfix>\n"
        "<postfix> ::= <inner> \"!\"\n"
        "<inner> ::= \"x\"\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("error propagation test grammar loads",
          parse_bnf_load(S(G), S("top"), g));
    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "x!", &p, &ts);
    check("error propagation sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    th_ctx *tc = th_ctx_new();
    parse_tree_t *postfix = find_node(g, tree, "postfix");
    parse_tree_t *inner = postfix && core_tree_num_children(postfix)
        ? core_tree_child(postfix, 0) : nullptr;
    if (inner && !core_tree_is_leaf(inner))
        core_tree_node_value(inner).type = th_name(tc, "i32");
    parse_visit_t visit = {
        .grammar = g,
        .node = postfix,
        .pn = postfix ? &core_tree_node_value(postfix) : nullptr,
    };
    parse_infer_expr_t *projection = parse_infer_parse(
        S("unwrap_result($0)"));
    bool ok = false;
    th_type *result = projection && postfix
        ? parse_infer_eval(projection, &visit, tc, nullptr, &ok) : nullptr;
    check("postfix error propagation preserves the ordinary return type",
          ok && result && th_hash(tc, result)
              == th_hash(tc, th_name(tc, "i32")));

    parse_infer_free(projection);
    th_ctx_free(tc);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_indirection_inference_constructors(void)
{
    static const char *const G =
        "@tokenizer(\"toy\")\n"
        "<top> ::= <inner>\n"
        "<inner> ::= \"x\"\n";
    static const struct {
        const char *name;
        uint8_t attrs;
    } constructors[] = {
        {"ref", 0},
        {"ptr", TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE},
        {"mutref", TH_EXCLUSIVE},
        {"own", TH_OWNING},
        {"gcref", TH_NULLABLE | TH_RESEATABLE},
        {"tmpref", TH_TEMPORARY},
    };

    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    parse_pwz_parser_t *p = nullptr;
    parse_token_stream_t *ts = nullptr;
    parse_tree_t *tree = loaded ? parse_toy(g, "top", "x", &p, &ts) : nullptr;
    th_ctx *tc = th_ctx_new();
    parse_tree_t *inner = tree ? core_tree_child(tree, 0) : nullptr;
    if (inner && !core_tree_is_leaf(inner))
        core_tree_node_value(inner).type = th_name(tc, "int");
    parse_visit_t visit = {
        .grammar = g,
        .node = tree,
        .pn = tree ? &core_tree_node_value(tree) : nullptr,
    };
    bool all = loaded && tree && inner;
    for (size_t i = 0; all && i < sizeof(constructors) / sizeof(constructors[0]);
         i++) {
        char source[32];
        snprintf(source, sizeof(source), "%s[$0]", constructors[i].name);
        parse_infer_expr_t *expr = parse_infer_parse(S(source));
        bool ok = false;
        th_type *type = expr
            ? parse_infer_eval(expr, &visit, tc, nullptr, &ok) : nullptr;
        uint8_t attrs = UINT8_MAX;
        all = ok && type && th_kind_of(type) == TH_INDIRECTION
            && th_ind_attrs(type, &attrs) && attrs == constructors[i].attrs
            && th_hash(tc, th_ind_referent(type))
                   == th_hash(tc, th_name(tc, "int"));
        parse_infer_free(expr);
    }
    check("inference applications for built-in indirections make TH_INDIRECTION types",
          all);
    th_ctx_free(tc);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_binary_method_result_inference(void)
{
    static const char *const G =
        "@tokenizer(\"toy\")\n"
        "<top> ::= <left> \"+\" <right>\n"
        "<left> ::= \"x\"\n"
        "<right> ::= \"y\"\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    bool loaded = parse_bnf_load(S(G), S("top"), g);
    parse_pwz_parser_t *p = nullptr;
    parse_token_stream_t *ts = nullptr;
    parse_tree_t *tree = loaded ? parse_toy(g, "top", "x+y", &p, &ts)
                                : nullptr;
    th_ctx *tc = th_ctx_new();
    th_type *receiver = th_var(tc);
    parse_tree_t *left = tree ? core_tree_child(tree, 0) : nullptr;
    parse_tree_t *right = tree ? core_tree_child(tree, 2) : nullptr;
    if (left && !core_tree_is_leaf(left))
        core_tree_node_value(left).type = receiver;
    if (right && !core_tree_is_leaf(right))
        core_tree_node_value(right).type = th_name(tc, "string");
    parse_visit_t visit = {
        .grammar = g,
        .node = tree,
        .pn = tree ? &core_tree_node_value(tree) : nullptr,
    };
    parse_infer_expr_t *expr = parse_infer_parse(
        S("method_result($0, $2, __add)"));
    bool ok = false;
    th_type *result = expr && tree
        ? parse_infer_eval(expr, &visit, tc, nullptr, &ok) : nullptr;
    th_constraint constraint = {0};
    const th_param *argument = th_var_constraint_at(receiver, 0,
            &constraint) && constraint.kind == TH_CON_METHOD
        ? th_func_param(constraint.method.call, 0) : nullptr;
    check("binary method inference constrains the receiver and leaves its result open",
          ok && result && th_kind_of(result) == TH_VAR
              && result != receiver
              && th_var_constraint_count(receiver) == 1
              && constraint.method.name
              && !strcmp(constraint.method.name, "__add")
              && constraint.method.result == result
              && argument && th_hash(tc, argument->type)
                  == th_hash(tc, th_name(tc, "string")));
    parse_infer_free(expr);
    th_ctx_free(tc);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_function_visibility_does_not_leak_to_parameters(void)
{
    static const char *const G =
        "@tokenizer(\"toy\")\n"
        "<top> @scope(\"module\") ::= <func>\n"
        "<func> @scope(\"function\", $2) @declares($2, \"function\") ::= "
        "<func-prelude> <name> <param> \";\"\n"
        "<func-prelude> @visibility(\"public\") ::= \"P\"\n"
        "<param> @declares($1, \"param\") ::= <name>\n"
        "<name> ::= __LOWER\n";
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(S(G), S("top"), grammar);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "Pfx;", &parser, &tokens)
        : nullptr;
    th_ctx *types = th_ctx_new();
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree, types) : nullptr;
    parse_tree_t *func_node = result ? find_node(grammar, tree, "func") : nullptr;
    parse_sym_entry_t *function = func_node
        ? core_tree_node_value(func_node).sym : nullptr;
    check("function-prelude visibility binds to the function declaration",
          loaded && tree && function
              && core_string_eq(function->visibility, S("public")));
    parse_annot_result_free(result);
    th_ctx_free(types);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_visibility_prefix_binds_to_non_function_declaration(void)
{
    static const char *const G =
        "@tokenizer(\"toy\")\n"
        "<top> @scope(\"module\") ::= <trait>\n"
        "<trait> @scope(\"trait\", $2) @declares($2) ::= "
        "<visibility> <name> \";\"\n"
        "<visibility> @visibility(\"public\") ::= \"P\"\n"
        "<name> ::= __LOWER\n";
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(S(G), S("top"), grammar);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "Pf;", &parser, &tokens) : nullptr;
    th_ctx *types = th_ctx_new();
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree, types) : nullptr;
    parse_tree_t *trait_node = tree ? find_node(grammar, tree, "trait") : nullptr;
    parse_sym_entry_t *trait = trait_node
        ? core_tree_node_value(trait_node).sym : nullptr;
    check("non-function visibility grammar loads", loaded);
    check("non-function visibility sample parses", tree != nullptr);
    check("non-function declaration symbol is attached", trait != nullptr);
    check("visibility prefix binds to a non-function declaration",
          trait && core_string_eq(trait->visibility, S("public")));
    parse_annot_result_free(result);
    th_ctx_free(types);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_field_visibility_in_non_type_tagged_scope(void)
{
    static const char *const G =
        "@tokenizer(\"toy\")\n"
        "<top> @scope(\"module\") ::= <record>\n"
        "<record> @namespace(\"type\") @scope(\"record\", $1) "
        "@type($1) @exposes ::= "
        "<public> <name> \"{\" <field> \"}\"\n"
        "<field> @field($1) ::= <field-mod> <name>\n"
        "<field-mod> ::= <private>\n"
        "<public> @visibility(\"public\") ::= \"P\"\n"
        "<private> @visibility(\"private\") ::= \"p\"\n"
        "<name> ::= __LOWER\n";
    parse_grammar_t *grammar = parse_grammar_new();
    parse_grammar_set_error_recovery(grammar, false);
    bool loaded = parse_bnf_load(S(G), S("top"), grammar);
    parse_pwz_parser_t *parser = nullptr;
    parse_token_stream_t *tokens = nullptr;
    parse_tree_t *tree = loaded
        ? parse_toy(grammar, "top", "Pr{pf}", &parser, &tokens) : nullptr;
    th_ctx *types = th_ctx_new();
    parse_annot_result_t *result = tree
        ? annotate_new(grammar, tree, types) : nullptr;
    parse_scope_t *module_scope = tree
        ? core_tree_node_value(tree).scope : nullptr;
    parse_sym_entry_t *record = result && module_scope
        ? parse_symtab_lookup_scoped(result->symtab, module_scope, S("type"),
                                     S("r")) : nullptr;
    parse_sym_entry_t *field = record && record->member_scope
        ? parse_symtab_lookup_in(record->member_scope, S("f")) : nullptr;
    check("non-type-tagged member visibility grammar loads", loaded);
    check("non-type-tagged member visibility sample parses", tree != nullptr);
    check("record keeps its own visibility",
          record && core_string_eq(record->visibility, S("public")));
    check("field receives its own visibility",
          field && core_string_eq(field->visibility, S("private")));
    parse_annot_result_free(result);
    th_ctx_free(types);
    parse_pwz_free(parser);
    scan_token_stream_free(tokens);
    parse_grammar_free(grammar);
}

static void
test_scoped_declaration_is_bound_outside_its_body(void)
{
    static const char *const G
        = "@tokenizer(\"toy\")\n"
          "<top> @scope(\"module\") ::= <func>\n"
          "<func> @scope(\"function\", $1) @declares($1, \"function\") "
          "::= \"f\" <name>\n"
          "<name> ::= __LOWER\n";
    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);
    check("scoped-declaration grammar loads", parse_bnf_load(S(G), S("top"), g));

    parse_pwz_parser_t *p;
    parse_token_stream_t *ts;
    parse_tree_t *tree = parse_toy(g, "top", "fa", &p, &ts);
    check("scoped-declaration sample parses", tree != nullptr);
    if (!tree) {
        parse_grammar_free(g);
        return;
    }

    parse_annot_result_t *result = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, result);
    parse_annot_register(walk, result, nullptr, nullptr);
    parse_tree_walk_run(walk, tree);
    parse_tree_t *func = find_node(g, tree, "func");
    parse_sym_entry_t *symbol = func
        ? core_tree_node_value(func).sym : nullptr;

    check("function symbol is provisionally stored in its enclosing module",
          symbol && symbol->kind == PARSE_SYM_FUNCTION
              && symbol->scope == core_tree_node_value(tree).scope);
    check("function body still has its own scope",
          func && core_tree_node_value(func).scope
              && core_tree_node_value(func).scope
                     != core_tree_node_value(tree).scope);

    parse_tree_walk_free(walk);
    parse_annot_result_free(result);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_part_two_rejections(void)
{
    static const char *const BAD[] = {
        "<top> @mutability($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @namespace($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @direction(\"out\") ::= <n>\n<n> ::= __LOWER\n",
        "<top> @constrain($1) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @constrain($1, ?? int) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @size() ::= <n>\n<n> ::= __LOWER\n",
        "<top> @size(sizeof) ::= <n>\n<n> ::= __LOWER\n",
        "<top> @alignment(1, 2) ::= <n>\n<n> ::= __LOWER\n",
        "@declaration_point(\"later\")\n<top> ::= <n>\n<n> ::= __LOWER\n",
    };

    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        parse_grammar_t *bad = parse_grammar_new();

        if (parse_bnf_load(S(BAD[i]), S("top"), bad)) {
            printf("FAIL  this should not have loaded: %s", BAD[i]);
            fails++;
        }

        parse_grammar_free(bad);
    }
}

static void
test_pass_by_modes(void)
{
    static const char *const MODES[] = {
        "in", "out", "inout", "ref", "consume"
    };
    bool all_load = true;
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
        core_buffer_t grammar_text = {0};
        core_buffer_printf(&grammar_text,
            "<top> @tokenizer(\"toy\") @scope(\"module\") ::= <p>\n"
            "<p> @declares($1,\"param\") @pass_by(\"%s\") ::= \"v\" <n> \";\"\n"
            "<n> ::= __LOWER\n", MODES[i]);
        parse_grammar_t *g = parse_grammar_new();
        parse_grammar_set_error_recovery(g, false);
        all_load = all_load && parse_bnf_load(
            core_string_borrow(grammar_text.data, grammar_text.byte_len),
            S("top"), g);
        parse_grammar_free(g);
        core_buffer_release(&grammar_text);
    }
    check("pass_by accepts all parameter passing modes", all_load);
}

int
main(void)
{
    parse_grammar_t *g = load_toy();

    if (!g) {
        return 1;
    }

    test_annotation_arguments_only_need_balanced_parens();
    test_annotation_arguments_carry_integers();
    test_emit_annotations_retain_action_and_operands();
    test_direct_nonterminal_annotation_lookup();
    test_annotation_argument_lists_are_growable();
    test_the_representation_and_declaration_annotations();
    test_initial_symbol_walk_callback(g);
    test_symbol_records_declaration_definition_and_use_nodes();
    test_declaration_point_hides_a_binding_in_its_initializer();
    test_selected_type_annotations_per_node(g);
    test_frontend_type_node_resolver();
    test_inference_result_is_propagated();
    test_generic_call_result_tracks_argument_type();
    test_symbol_lookup_instantiates_schemes();
    test_error_propagation_preserves_return_type();
    test_indirection_inference_constructors();
    test_binary_method_result_inference();
    test_function_visibility_does_not_leak_to_parameters();
    test_visibility_prefix_binds_to_non_function_declaration();
    test_field_visibility_in_non_type_tagged_scope();
    test_scoped_declaration_is_bound_outside_its_body();
    test_the_repr_annotations_reach_the_layout_descriptor();
    test_trait_method_inference_uses_visible_requirement();
    test_ordering_inference_uses_comparable_trait();
    test_index_inference_uses_index_load_trait();
    test_slice_inference_uses_index_slice_trait();
    test_an_adt_builds_a_variant_type();
    test_implements_is_checked_not_carried();
    test_part_two_rejections();
    test_pass_by_modes();
    test_annotations_are_retained_on_the_grammar(g);
    test_annotation_cache_is_thread_safe(g);
    test_the_walk_builds_a_symbol_table(g);
    test_symtab_phase_lifetime_and_alias_facts();
    test_symbol_namespaces_share_lexical_scopes();
    test_symbol_namespace_scope_iteration();
    test_tree_resolution_is_scope_or_symbol_without_replacing_lexical_scope();
    test_tree_integer_range_fact();
    test_a_deep_tree_does_not_overflow_the_stack(g);

    parse_grammar_free(g);

    return report("fe annot");
}
