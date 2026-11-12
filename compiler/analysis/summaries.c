#include "lower/lower_internal.h"

static bool
thread_runtime_entry_function(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol);
static bool
runtime_init_target_add(n00bb_runtime_init_analysis_t *runtime,
                        ir_global_t *global);
static void
runtime_function_add(core_list_t(n00bb_ir_func_ptr_t) *functions,
                     ir_func_t *function);
static uint32_t
n00bb_summary_adjust(void *context, const ir_func_t *function, uint32_t flags);
static th_type *
n00bb_bookkeeping_receiver(const ir_func_t *function);
static uint64_t
n00bb_summary_bookkeeping(void *context, const ir_func_t *function,
                          const ir_summary_t *summary);

static bool
thread_runtime_entry_function(parse_grammar_t *grammar,
                              parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && symbol->decl_node && nt_is(grammar, symbol->decl_node, CORE_STRING_STATIC("func-def"))
        && !symbol_is_private(symbol);
}

void
runtime_init_analysis_release(n00bb_runtime_init_analysis_t *runtime)
{
    core_list_free(runtime->initializer_roots);
    core_list_free(runtime->entry_functions);
    core_dict_free(&runtime->targets);
    core_dict_free(&runtime->reachable);
}

static bool
runtime_init_target_add(n00bb_runtime_init_analysis_t *runtime,
                        ir_global_t *global)
{
    ir_global_t *identity = global ? ir_global_identity(global) : nullptr;
    if (!identity) return false;
    core_dict_put(&runtime->targets, identity, identity);
    return true;
}

bool
runtime_init_is_target(n00bb_runtime_init_analysis_t *runtime,
                       ir_global_t *global)
{
    return global && runtime->summaries
        && core_dict_contains(&runtime->targets, ir_global_identity(global));
}

static void
runtime_function_add(core_list_t(n00bb_ir_func_ptr_t) *functions,
                     ir_func_t *function)
{
    for (size_t i = 0; i < functions->len; i++)
        if (functions->data[i] == function) return;
    core_list_push(*functions, function);
}

bool
runtime_init_entries(parse_grammar_t *grammar,
                     n00bb_link_analysis_t *analysis,
                     n00bb_runtime_init_analysis_t *runtime)
{
    runtime->initializer_roots = core_list_new(n00bb_ir_func_ptr_t);
    runtime->entry_functions = core_list_new(n00bb_ir_func_ptr_t);
    core_dict_init(&runtime->targets, nullptr, nullptr);
    core_dict_init(&runtime->reachable, nullptr, nullptr);
    for (size_t i = 0; i < analysis->units.len; i++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[i]);
        parse_symtab_t *symtab = state && state->annotations
            ? state->annotations->symtab : nullptr;
        if (!symtab) return false;
        for (parse_scope_t *scope = symtab->all_scopes; scope;
             scope = scope->all_next) {
            for (parse_sym_entry_t *symbol = scope->first_in_scope; symbol;
                 symbol = symbol->next_in_scope) {
                if (symbol->kind == PARSE_SYM_VARIABLE
                    && symbol->is_thread_local
                    && is_module_global_symbol(grammar, symbol)) {
                    ir_global_t *global = link_ir_global_for_symbol(analysis,
                                                                    symbol);
                    if (!runtime_init_target_add(runtime, global))
                        return false;
                }
                if (!thread_runtime_entry_function(grammar, symbol)) continue;
                ir_func_t *function = link_ir_function_for_symbol(analysis,
                                                                  symbol);
                if (!function || !function->n_blocks) continue;
                runtime_function_add(&runtime->entry_functions, function);
            }
        }
    }
    return true;
}

bool
runtime_init_analysis_build(
    core_list_t(core_translation_unit_ptr_t) *runtime_initializer_order,
    const core_list_t(n00bb_initializer_read_t) *reads,
    n00bb_runtime_init_analysis_t *runtime)
{
    for (size_t i = 0; i < reads->len; i++)
        if (!runtime_init_target_add(runtime, reads->data[i].global))
            return false;
    if (!core_dict_len(&runtime->targets)) return true;
    for (size_t i = 0; i < runtime_initializer_order->len; i++) {
        n00bb_tu_state_t *state = tu_state(runtime_initializer_order->data[i]);
        ir_func_t *initializer = state && state->ir_module
            ? ir_func_find(state->ir_module, CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME)) : nullptr;
        if (!initializer) return false;
        runtime_function_add(&runtime->initializer_roots, initializer);
    }
    return true;
}

typedef struct {
    ir_pdg_target_fn add;
    void            *sink;
} program_targets_t;

static void
program_target(void *context, const ir_func_t *target,
               const ir_summary_t *summary)
{
    (void)summary;
    program_targets_t *targets = context;
    ir_func_t *body = n00bb_cached_body((ir_func_t *)target);
    targets->add(targets->sink, body ? body : (ir_func_t *)target);
}

static bool
program_call_targets(void *context, const ir_instr_t *call,
                     ir_pdg_target_fn add, void *sink)
{
    n00bb_link_analysis_t *analysis = context;
    program_targets_t targets = {.add = add, .sink = sink};
    return ir_summary_call_callees(analysis->summaries, call, program_target,
                                   &targets);
}

static void
program_function_add(core_list_t(n00bb_ir_func_ptr_t) *functions,
                     core_dict_t *seen, ir_func_t *function)
{
    ir_func_t *body = function ? n00bb_cached_body(function) : nullptr;
    if (!body) body = function;
    if (body && body->n_blocks && core_dict_add(seen, body, 1))
        core_list_push(*functions, body);
}

bool
build_program_graph(n00bb_link_analysis_t *analysis,
                    n00bb_runtime_init_analysis_t *runtime)
{
    core_list_t(n00bb_ir_func_ptr_t) functions =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    for (size_t i = 0; i < analysis->emitted_functions.len; i++)
        program_function_add(&functions, &seen,
                             analysis->emitted_functions.data[i]);
    for (size_t i = 0; i < runtime->initializer_roots.len; i++)
        program_function_add(&functions, &seen,
                             runtime->initializer_roots.data[i]);
    for (size_t i = 0; i < runtime->entry_functions.len; i++)
        program_function_add(&functions, &seen,
                             runtime->entry_functions.data[i]);
    ir_pdg_hooks_t hooks = {.context = analysis,
                            .call_targets = program_call_targets};
    ir_group_pdg_free(analysis->program);
    analysis->program = functions.len
        ? ir_group_pdg_of(functions.data, functions.len, &hooks) : nullptr;
    bool built = analysis->program || !functions.len;
    core_list_t(n00bb_ir_func_ptr_t) pending =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; built && i < runtime->initializer_roots.len; i++)
        program_function_add(&pending, &runtime->reachable,
                             runtime->initializer_roots.data[i]);
    for (size_t i = 0; built && i < runtime->entry_functions.len; i++)
        program_function_add(&pending, &runtime->reachable,
                             runtime->entry_functions.data[i]);
    while (pending.len) {
        ir_func_t *function = pending.data[--pending.len];
        for (uint32_t c = 0;
             c < ir_group_pdg_callee_count(analysis->program, function); c++)
            program_function_add(&pending, &runtime->reachable,
                ir_group_pdg_callee_at(analysis->program, function, c));
    }
    core_list_free(pending);
    core_dict_free(&seen);
    core_list_free(functions);
    return built;
}

void
release_program_graph(n00bb_link_analysis_t *analysis,
                      n00bb_runtime_init_analysis_t *runtime)
{
    ir_group_pdg_free(analysis->program);
    analysis->program = nullptr;
    core_dict_free(&runtime->reachable);
    core_dict_init(&runtime->reachable, nullptr, nullptr);
}

bool
runtime_reaches(n00bb_runtime_init_analysis_t *runtime,
                const ir_func_t *function)
{
    ir_func_t *body = function ? n00bb_cached_body((ir_func_t *)function)
                               : nullptr;
    return core_dict_contains(&runtime->reachable,
                              body ? body : (ir_func_t *)function);
}

static uint32_t
n00bb_summary_adjust(void *context, const ir_func_t *function, uint32_t flags)
{
    (void)context;
    return n00bb_llvm_mutator_flags(function, flags);
}

static th_type *
n00bb_bookkeeping_receiver(const ir_func_t *function)
{
    th_type *type = function->params[0] && function->params[0]->type
        ? th_resolve(function->params[0]->type) : nullptr;
    while (type && th_kind_of(type) == TH_INDIRECTION)
        type = th_resolve(th_ind_referent(type));
    return type && th_kind_of(type) == TH_NAMED ? type : nullptr;
}

static uint64_t
n00bb_summary_bookkeeping(void *context, const ir_func_t *function,
                          const ir_summary_t *summary)
{
    n00bb_link_analysis_t *analysis = context;
    core_string_t root = function ? n00bb_function_root(function->name)
                                  : (core_string_t){0};
    if (!analysis || !function || !function->n_params
        || (!core_string_eq(root, CORE_STRING_STATIC("__ref_create"))
            && !core_string_eq(root, CORE_STRING_STATIC("__finalize"))))
        return 0;
    for (size_t i = analysis->bookkeeping_writes.len; i > 0; i--)
        if (analysis->bookkeeping_writes.data[i - 1].function == function)
            core_list_delete(analysis->bookkeeping_writes, i - 1);
    th_type *receiver = n00bb_bookkeeping_receiver(function);
    parse_scope_t *fields = receiver
        ? record_scope_for_type(analysis, receiver, nullptr) : nullptr;
    for (uint32_t i = 0; i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if (path->root != IR_SPLACE_PARAM || path->param
            || !(path->flags & IR_SPATH_WRITE)) continue;
        core_string_t offending = {0};
        parse_scope_t *scope = fields;
        th_type *owner = receiver;
        for (uint32_t step = 0; !offending.data && step < path->depth;
             step++) {
            if (n00bb_builtin_member(path->steps[step])) continue;
            parse_sym_entry_t *field = scope
                ? parse_symtab_lookup_in(scope, path->steps[step]) : nullptr;
            if (!field || !symbol_is_private(field)) {
                offending = path->steps[step];
                break;
            }
            th_type *next = field->type ? th_resolve(field->type) : nullptr;
            while (next && th_kind_of(next) == TH_INDIRECTION)
                next = th_resolve(th_ind_referent(next));
            scope = next && th_kind_of(next) == TH_NAMED
                ? record_scope_for_type(analysis, next, nullptr) : nullptr;
            owner = next;
        }
        if (!offending.data) continue;
        core_list_push(analysis->bookkeeping_writes,
            ((n00bb_bookkeeping_write_t){function, offending, owner}));
        break;
    }
    return 1;
}

bool
report_bookkeeping_writes(n00bb_link_analysis_t *analysis)
{
    for (size_t i = 0; i < analysis->bookkeeping_writes.len; i++) {
        n00bb_bookkeeping_write_t *write = &analysis->bookkeeping_writes.data[i];
        core_translation_unit_t *unit = nullptr;
        for (size_t u = 0; !unit && u < analysis->units.len; u++)
            if (tu_state(analysis->units.data[u])->ir_module
                    == write->function->owner)
                unit = analysis->units.data[u];
        if (!unit) continue;
        n00bb_tu_state_t *state = tu_state(unit);
        core_string_t type_text = write->owner && state
            ? n00bb_print_type(state->types, write->owner)
            : CORE_STRING_STATIC("?");
        n00bb_report(core_translation_unit_diagnostics(unit), unit,
            N00B_DIAG_BOOKKEEPING_WRITE, n00bb_span_location(write->function->span),
            N00B_TEXT_BOOKKEEPING_WRITE,
            N00BB_ARGS(ARG_TEXT(n00bb_function_root(write->function->name)),
                       ARG_TEXT(write->field), ARG_TEXT(type_text)));
        n00bb_report_help(core_translation_unit_diagnostics(unit),
            N00B_TEXT_KEEP_BOOKKEEPING_PRIVATE, N00BB_NO_ARGS);
    }
    return analysis->bookkeeping_writes.len == 0;
}

static bool
effect_function(n00bb_link_analysis_t *analysis, const ir_func_t *callee)
{
    bool found = false;
    ir_func_t *body = n00bb_cached_body((ir_func_t *)callee);
    if (analysis->effect_functions_ready && analysis->user_effects.len)
        (void)core_dict_get(&analysis->effect_functions, body, &found);
    core_string_t native = !found && body && !body->n_blocks
        && analysis->effect_natives.len ? n00bb_native_symbol(body)
                                        : (core_string_t){0};
    for (size_t n = 0; native.u8_bytes && !found
         && n < analysis->effect_natives.len; n++)
        found = core_string_eq(native, analysis->effect_natives.data[n]);
    return found;
}

bool
clause_applies(n00bb_link_analysis_t *analysis,
               const core_list_t(n00bb_effect_clause_t) *clauses,
               const ir_func_t *caller, const ir_func_t *callee)
{
    ir_func_t *body = n00bb_cached_body((ir_func_t *)callee);
    core_translation_unit_t *unit = nullptr;
    bool looked_up = false;
    for (size_t c = 0; c < clauses->len; c++) {
        const n00bb_effect_clause_t *clause = &clauses->data[c];
        if (!n00bb_clause_matches(clause, body)) continue;
        if (!looked_up) {
            unit = caller ? unit_for_module(analysis, caller->owner) : nullptr;
            looked_up = true;
        }
        if (!unit || clause->unit == unit
            || n00bb_unit_reaches_import(unit, clause->unit))
            return true;
    }
    return false;
}

static bool
n00bb_summary_inherits_effect(void *context, const ir_func_t *caller,
                              const ir_func_t *callee, core_string_t effect)
{
    n00bb_bound_t bound;
    if (n00bb_bound_parse(effect, &bound)) return false;
    n00bb_user_effect_t *user = user_effect(context, effect);
    if (user && !n00bb_effect_crosses(user, caller, callee)) return false;
    if (!effect_function(context, callee)) return true;
    return !user || !clause_applies(context, &user->handlers, caller, callee);
}

static void
n00bb_summary_callee_effects(void *context, const ir_func_t *callee,
                             ir_summary_t *caller)
{
    n00bb_link_analysis_t *analysis = context;
    if (!effect_function(analysis, callee)) return;
    const ir_func_t *function = ir_summary_function(caller);
    for (size_t e = 0; e < analysis->user_effects.len; e++) {
        n00bb_user_effect_t *user = &analysis->user_effects.data[e];
        if (!clause_applies(analysis, &user->sources, function, callee)
            || clause_applies(analysis, &user->handlers, function, callee))
            continue;
        if (user->bits)
            ir_summary_set_flags(caller, ir_summary_flags(caller)
                                         | (uint32_t)user->bits);
        else (void)ir_summary_add_effect(caller, user->qualified);
    }
}

static bool
n00bb_summary_owned_field(void *context, th_type *record, core_string_t field)
{
    (void)context;
    return core_string_eq(field, CORE_STRING_STATIC("storage"))
        && lease_storage_record(record);
}

static core_translation_unit_t *
visibility_unit(n00bb_link_analysis_t *analysis, const ir_module_t *module)
{
    bool found = false;
    core_translation_unit_t *unit = core_dict_get(&analysis->visibility_units,
                                                  (void *)module, &found);
    if (found) return unit;
    core_string_t name = ir_module_name((ir_module_t *)module);
    int64_t cut = core_string_find(name, CORE_STRING_STATIC("$spec$"));
    if (cut >= 0) name = core_string_slice(name, 0, (size_t)cut);
    unit = n00bb_unit_named(analysis, name);
    core_dict_put(&analysis->visibility_units, (void *)module, unit);
    return unit;
}

static core_dict_t *
import_closure(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit)
{
    core_dict_t *closure = core_dict_get(&analysis->import_closures, unit,
                                         nullptr);
    if (closure) return closure;
    closure = core_alloc(core_dict_t);
    core_dict_init(closure, nullptr, nullptr);
    core_list_t(core_translation_unit_ptr_t) pending =
        core_list_new(core_translation_unit_ptr_t);
    core_list_push(pending, unit);
    while (pending.len) {
        core_translation_unit_t *next = pending.data[--pending.len];
        if (!core_dict_add(closure, next, next)) continue;
        for (size_t i = 0; i < core_translation_unit_import_count(next); i++)
            core_list_push(pending, core_translation_unit_import(next, i));
    }
    core_list_free(pending);
    core_dict_put(&analysis->import_closures, unit, closure);
    return closure;
}

static bool
n00bb_module_sees(void *context, const ir_module_t *viewer,
                  const ir_module_t *other)
{
    n00bb_link_analysis_t *analysis = context;
    core_translation_unit_t *from = visibility_unit(analysis, viewer);
    core_translation_unit_t *to   = visibility_unit(analysis, other);
    return !from || !to || from == to
        || core_dict_contains(import_closure(analysis, from), to);
}

void
bind_summary_hooks(n00bb_link_analysis_t *analysis, bool defer_trap_proofs)
{
    if (!analysis->visibility_ready) {
        core_dict_init(&analysis->visibility_units, nullptr, nullptr);
        core_dict_init(&analysis->import_closures, nullptr, nullptr);
        analysis->visibility_ready = true;
    }
    ir_reinterpretations_set_visibility(analysis->ir_context,
                                        n00bb_module_sees, analysis);
    analysis->summary_hooks = (ir_summary_hooks_t){
        .context = analysis,
        .call_targets = n00bb_summary_call_targets,
        .call_flags = n00bb_summary_call_flags,
        .call_finalizers = n00bb_summary_call_finalizers,
        .owned_field = n00bb_summary_owned_field,
        .axiom = n00bb_summary_axiom,
        .adjust = n00bb_summary_adjust,
        .entries = n00bb_summary_entries,
        .global_reachable = n00bb_summary_global_reachable,
        .bookkeeping = n00bb_summary_bookkeeping,
        .inherits_effect = n00bb_summary_inherits_effect,
        .defer_trap_proofs = defer_trap_proofs,
    };
    collect_user_effects(analysis);
    if (analysis->user_effects.len)
        analysis->summary_hooks.callee_effects = n00bb_summary_callee_effects;
    if (analysis->bounded_targets_ready)
        core_dict_free(&analysis->bounded_targets);
    core_dict_init(&analysis->bounded_targets, nullptr, nullptr);
    analysis->bounded_targets_ready = true;
    collect_callback_handlers(analysis);
    analysis->entry_owner = nullptr;
}

bool
build_function_summaries(n00bb_link_analysis_t *analysis,
    const core_list_t(n00bb_ir_func_ptr_t) *initializers,
    const core_list_t(n00bb_ir_func_ptr_t) *entries)
{
    core_list_t(n00bb_ir_func_ptr_t) roots =
        core_list_new(n00bb_ir_func_ptr_t);
    for (size_t i = 0; i < analysis->emitted_functions.len; i++)
        core_list_push(roots, analysis->emitted_functions.data[i]);
    for (size_t i = 0; i < analysis->cached_functions.len; i++)
        core_list_push(roots, analysis->cached_functions.data[i]);
    for (size_t i = 0; i < initializers->len; i++)
        core_list_push(roots, initializers->data[i]);
    for (size_t i = 0; i < entries->len; i++)
        core_list_push(roots, entries->data[i]);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (!state || !state->ir_module || state->interface_module) continue;
        th_ctx *types = ir_module_types(state->ir_module);
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
            ir_func_t *body = n00bb_cached_body(
                ir_func_at(state->ir_module, f));
            if (body && body->n_blocks && th_is_closed(types, body->type))
                core_list_push(roots, body);
        }
    }
    push_effect_query_subjects(analysis, &roots);
    push_effect_check_subjects(analysis, &roots);
    push_effects_of_roots(analysis, &roots);
    bind_summary_hooks(analysis, false);
    ir_summaries_t *previous = analysis->summaries;
    if (previous && ir_analysis_summaries() == previous)
        ir_analysis_bind_summaries(nullptr);
    analysis->summaries = ir_summaries_recompute(roots.data, roots.len,
                                                 &analysis->summary_hooks,
                                                 previous);
    ir_summaries_free(previous);
    ir_analysis_bind_summaries(analysis->summaries);
    core_list_free(roots);
    return analysis->summaries != nullptr;
}
