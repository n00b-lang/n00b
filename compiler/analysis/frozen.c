#include "compiler_buffer_slices.h"
#include "compiler_visibility.h"
#include "ir/operands.h"
#include "ir/pdg.h"
#include "ir/pdg_memory.h"
#include "lower/lower_internal.h"

typedef struct {
    const ir_value_t  *parameter;
    const ir_global_t *global;
    core_string_t      reference;
    bool               array;
} frozen_root_t;

static bool
frozen(frozen_root_t root)
{
    return root.parameter || root.global || root.reference.data;
}

static bool
read_only_reference(th_ctx *types, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    th_type *item = type ? th_option_item(type) : nullptr;
    if (item) type = th_resolve(item);
    return type && th_kind_of(type) == TH_NAMED
        && n00bb_reference_method(types, type, false, nullptr, nullptr)
        && !n00bb_reference_method(types, type, true, nullptr, nullptr);
}

typedef struct {
    n00bb_link_analysis_t *analysis;
    th_ctx                *types;
    ir_pdg_t              *pdg;
    ir_pdg_memory_t       *memory;
    core_dict_t            seen;
    core_list_t(n00bb_ir_value_ptr_t) pending;
    core_list_t(n00bb_ir_instr_ptr_t) producers;
} frozen_walk_t;

static th_type *
callable_signature(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    for (uint32_t depth = 0; type && depth < 8
         && th_kind_of(type) == TH_INDIRECTION; depth++)
        type = th_resolve(th_ind_referent(type));
    return type && th_kind_of(type) == TH_FUNCTION ? type : nullptr;
}

static const th_param *
formal_of(const ir_value_t *value)
{
    th_type *signature = value->kind == IR_VAL_PARAM && value->owner
        ? callable_signature(value->owner->type) : nullptr;
    return signature && value->index < th_func_nparams(signature)
        ? th_func_param(signature, value->index) : nullptr;
}

static bool
view_parameter(th_ctx *types, const ir_value_t *value)
{
    const th_param *formal = formal_of(value);
    return formal && formal->mode == TH_VIEW
        && n00bb_type_reaches_shared_state(types, formal->type);
}

static bool
lent_array_parameter(const ir_value_t *value)
{
    const th_param *formal = formal_of(value);
    th_type *type = formal && formal->type ? th_resolve(formal->type)
                                           : nullptr;
    uint64_t extent = 0;
    return formal && formal->mode == TH_IN && type
        && th_kind_of(type) == TH_SEQUENCE && !th_seq_extent(type, &extent);
}

static bool
lends_arrays(const ir_func_t *function)
{
    for (uint32_t p = 0; p < function->n_params; p++)
        if (lent_array_parameter(function->params[p])) return true;
    return false;
}

static const ir_global_t *
constant_global(const ir_value_t *value)
{
    const ir_global_t *global = value->kind == IR_VAL_GLOBAL && value->global
        ? ir_global_identity(value->global) : nullptr;
    return global && global->mutability == IR_MUT_CONST
        && ir_span_known(global->span) ? global : nullptr;
}

static bool
carries_address(th_ctx *types, const ir_value_t *value, bool address)
{
    th_type *type = value->type ? th_resolve(value->type) : nullptr;
    if (type && read_only_reference(types, type)) return false;
    return (address && value->kind == IR_VAL_GLOBAL) || !type
        || th_kind_of(type) == TH_INDIRECTION
        || n00bb_type_reaches_shared_state(types, type);
}

static void
push_place(frozen_walk_t *walk, const ir_value_t *value, bool address)
{
    if (value && carries_address(walk->types, value, address)
        && core_dict_add(&walk->seen, (void *)value, (void *)value))
        core_list_push(walk->pending, (ir_value_t *)value);
}

static void
push_value(frozen_walk_t *walk, const ir_value_t *value)
{
    push_place(walk, value, false);
}

static void
push_producer(frozen_walk_t *walk, ir_instr_t *producer)
{
    if (producer
        && core_dict_add(&walk->seen, (void *)producer, (void *)producer))
        core_list_push(walk->producers, producer);
}

static void
push_operand(frozen_walk_t *walk, const ir_instr_t *in, uint32_t index)
{
    bool projects = !index && (in->op == IR_OP_FIELD || in->op == IR_OP_INDEX
                               || in->op == IR_OP_OFFSET
                               || in->op == IR_OP_ADDRESS_OF);
    if (index < in->n_operands)
        push_place(walk, ir_operand_value(&in->operands[index]),
                   projects || ir_op_is_address_operand(in->op, index));
}

static void
push_local_writes(frozen_walk_t *walk, ir_instr_t *read, ir_local_t *local)
{
    for (uint32_t i = 0; walk->pdg && i < ir_pdg_in_count(walk->pdg, read);
         i++) {
        const ir_dep_edge_t *edge = ir_pdg_in_at(walk->pdg, read, i);
        if (edge->kind == IR_DEP_DATA && edge->via == local)
            push_producer(walk, edge->from);
    }
}

static bool
push_stored(void *context, ir_instr_t *from, bool exact)
{
    (void)exact;
    int32_t stored = from ? ir_op_stored_operand(from->op) : -1;
    if (stored >= 0) push_operand(context, from, (uint32_t)stored);
    return true;
}

static void
push_loaded(frozen_walk_t *walk, ir_instr_t *in)
{
    int32_t address = ir_op_address_operand(in->op);
    if (address >= 0) push_operand(walk, in, (uint32_t)address);
    uint32_t access = walk->memory
        ? ir_pdg_memory_access_of(walk->memory, in, IR_ACCESS_LOAD)
        : UINT32_MAX;
    if (access != UINT32_MAX)
        ir_pdg_memory_load_sources(walk->memory, access, push_stored, walk);
}

typedef struct {
    frozen_walk_t     *walk;
    const ir_instr_t  *call;
    const ir_global_t *global;
} result_sources_t;

static bool
behind_read_only(th_ctx *types, const ir_func_t *callee,
                 const ir_spath_t *path)
{
    for (uint32_t depth = 0; callee && depth <= path->depth; depth++)
        if (read_only_reference(types,
                ir_summary_path_type(callee, path, depth)))
            return true;
    return false;
}

static void
push_result_sources(void *context, const ir_func_t *target,
                    const ir_summary_t *summary)
{
    result_sources_t *sources = context;
    th_ctx *types = sources->walk->types;
    uint32_t count = summary
        ? ir_summary_flow_count(summary, IR_SFLOW_RESULTS) : 0;
    uint32_t inputs = summary
        ? ir_summary_flow_source_count(summary, IR_SFLOW_RESULTS) : 0;
    for (uint32_t d = 0; d < count; d++)
        for (uint32_t s = 0; s < inputs; s++) {
            if (!ir_summary_flow_has(summary, IR_SFLOW_RESULTS, d, s,
                                     IR_SFLOW_ALIAS)
                || behind_read_only(types, target,
                       ir_summary_flow_to(summary, IR_SFLOW_RESULTS, d)))
                continue;
            const ir_spath_t *from = ir_summary_flow_source(
                summary, IR_SFLOW_RESULTS, s);
            if (from->root == IR_SPLACE_PARAM)
                push_value(sources->walk,
                    ir_call_argument(sources->call, from->param).value);
            else if (from->root == IR_SPLACE_GLOBAL && from->global
                     && !sources->global) {
                const ir_global_t *global = ir_global_identity(from->global);
                if (global->mutability == IR_MUT_CONST
                    && ir_span_known(global->span))
                    sources->global = global;
            }
        }
}

static const ir_global_t *
push_call_results(frozen_walk_t *walk, const ir_instr_t *call)
{
    result_sources_t sources = {.walk = walk, .call = call};
    if (!ir_summary_call_callees(walk->analysis->summaries, call,
                                 push_result_sources, &sources))
        for (uint32_t p = 0; p < ir_call_argument_count(call); p++)
            push_value(walk, ir_call_argument(call, p).value);
    return sources.global;
}

static core_string_t
read_only_address(th_ctx *types, const ir_instr_t *call)
{
    const ir_func_t *callee = ir_call_function(call);
    const ir_value_t *receiver = ir_call_argument(call, 0).value;
    if (!callee || !receiver
        || !n00bb_names_function(CORE_STRING_STATIC("__ref_address"),
                                 callee->name)
        || !read_only_reference(types, receiver->type))
        return (core_string_t){0};
    return n00bb_print_type(types, receiver->type);
}

static frozen_root_t
produced_root(frozen_walk_t *walk, ir_instr_t *in)
{
    switch (in->op) {
    case IR_OP_COPY: {
        ir_local_t *local = in->n_operands
            ? ir_operand_local(&in->operands[0]) : nullptr;
        if (local) push_local_writes(walk, in, local);
        else push_operand(walk, in, 0);
        break;
    }
    case IR_OP_ADDRESS_OF:
    case IR_OP_CONSUME:
    case IR_OP_RETAIN_REF:
    case IR_OP_CAST:
    case IR_OP_BITCAST:
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW:
    case IR_OP_NOMINAL_CAST:
    case IR_OP_TYPE_NARROW:
    case IR_OP_FIELD:
    case IR_OP_INDEX:
    case IR_OP_OFFSET:
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
    case IR_OP_EXTRACT:
    case IR_OP_EXTRACT_INDEX:
        push_operand(walk, in, 0);
        break;
    case IR_OP_OPTION_GET_OR_ELSE:
        push_operand(walk, in, 0);
        push_operand(walk, in, 1);
        break;
    case IR_OP_SELECT:
        push_operand(walk, in, 1);
        push_operand(walk, in, 2);
        break;
    case IR_OP_PHI:
    case IR_OP_AGGREGATE:
    case IR_OP_INSERT:
        for (uint32_t i = 0; i < in->n_operands; i++)
            push_operand(walk, in, i);
        break;
    case IR_OP_LOAD:
    case IR_OP_SEQUENCE_GET:
        push_loaded(walk, in);
        break;
    case IR_OP_CALL:
    case IR_OP_CALLBACK_INVOKE: {
        core_string_t reference = read_only_address(walk->types, in);
        if (reference.data) return (frozen_root_t){.reference = reference};
        const ir_global_t *global = push_call_results(walk, in);
        if (global) return (frozen_root_t){.global = global};
        break;
    }
    default:
        break;
    }
    return (frozen_root_t){0};
}

static const ir_value_t *
lent_array_root(const ir_value_t *value)
{
    core_list_t(n00bb_ir_value_ptr_t) pending =
        core_list_new(n00bb_ir_value_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_push(pending, (ir_value_t *)value);
    const ir_value_t *found = nullptr;
    while (!found && pending.len) {
        const ir_value_t *at = pending.data[--pending.len];
        if (!at || !core_dict_add(&seen, (void *)at, (void *)at)) continue;
        if (lent_array_parameter(at)) {
            found = at;
            continue;
        }
        const ir_instr_t *in = at->kind == IR_VAL_INSTR ? at->def : nullptr;
        if (!in) continue;
        uint32_t first = 0, last = 0;
        switch (in->op) {
        case IR_OP_COPY:
        case IR_OP_CONSUME:
        case IR_OP_RETAIN_REF:
        case IR_OP_CAST:
        case IR_OP_BITCAST:
        case IR_OP_NOMINAL_CAST:
        case IR_OP_TYPE_NARROW:
        case IR_OP_OPTION_SOME:
        case IR_OP_OPTION_GET:
            last = 1;
            break;
        case IR_OP_OPTION_GET_OR_ELSE:
            last = 2;
            break;
        case IR_OP_SELECT:
            first = 1;
            last = 3;
            break;
        case IR_OP_PHI:
            last = in->n_operands;
            break;
        default:
            break;
        }
        for (uint32_t o = first; o < last && o < in->n_operands; o++)
            core_list_push(pending, ir_operand_value(&in->operands[o]));
    }
    core_dict_free(&seen);
    core_list_free(pending);
    return found;
}

static frozen_root_t
frozen_root(n00bb_link_analysis_t *analysis, ir_func_t *function,
            const ir_value_t *value, bool address)
{
    frozen_walk_t walk = {
        .analysis  = analysis,
        .types     = ir_module_types(function->owner),
        .pdg       = ir_func_pdg(function),
        .pending   = core_list_new(n00bb_ir_value_ptr_t),
        .producers = core_list_new(n00bb_ir_instr_ptr_t),
    };
    walk.memory = walk.pdg ? ir_pdg_memory(walk.pdg) : nullptr;
    core_dict_init(&walk.seen, nullptr, nullptr);
    push_place(&walk, value, address);
    frozen_root_t root = {0};
    while (!frozen(root) && (walk.pending.len || walk.producers.len)) {
        if (walk.producers.len) {
            root = produced_root(&walk,
                                 walk.producers.data[--walk.producers.len]);
            continue;
        }
        const ir_value_t *at = walk.pending.data[--walk.pending.len];
        if (view_parameter(walk.types, at)) root.parameter = at;
        else if (constant_global(at)) root.global = constant_global(at);
        else if (at->kind == IR_VAL_INSTR && at->def)
            push_producer(&walk, at->def);
    }
    core_dict_free(&walk.seen);
    core_list_free(walk.pending);
    core_list_free(walk.producers);
    return root;
}

static core_string_t
root_name(frozen_root_t root)
{
    return root.global ? n00bb_name_leaf(root.global->name)
         : root.parameter ? root.parameter->name : root.reference;
}

static parse_sym_entry_t *
parameter_symbol(n00bb_link_analysis_t *analysis,
                 core_translation_unit_t *unit, const ir_value_t *parameter)
{
    parse_grammar_t *grammar = analysis->grammar;
    parse_sym_entry_t *found = nullptr;
    core_list_t(parse_scope_ptr_t) scopes = module_scopes(tu_state(unit));
    for (size_t s = 0; !found && s < scopes.len; s++)
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             !found && symbol; symbol = symbol->next_in_scope) {
            if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->decl_node)
                continue;
            ir_func_t *function = link_ir_function_for_symbol(analysis,
                                                              symbol);
            if (!function || (function != parameter->owner
                    && n00bb_cached_body(function) != parameter->owner))
                continue;
            core_list_t(parse_sym_entry_ptr_t) parameters =
                core_list_new(parse_sym_entry_ptr_t);
            parse_sym_entry_t *rest = nullptr;
            collect_ir_parameters(grammar, child_nt(grammar,
                symbol->decl_node,
                CORE_STRING_STATIC("func-type-signature")),
                &parameters, &rest);
            for (size_t p = 0; !found && p < parameters.len; p++)
                if (core_string_eq(parameters.data[p]->name,
                                   parameter->name))
                    found = parameters.data[p];
            core_list_free(parameters);
        }
    core_list_free(scopes);
    return found;
}

static void
note_global(n00bb_link_analysis_t *analysis, core_diagnostics_t *diagnostics,
            const ir_global_t *global, n00bb_text_t text)
{
    if (!ir_span_known(global->span)) return;
    n00bb_report_secondary(diagnostics,
        unit_for_ir_module(analysis, global->owner),
        n00bb_span_location(global->span), text,
        N00BB_ARGS(ARG_TEXT(n00bb_name_leaf(global->name))));
}

static void
report_write(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
             const ir_instr_t *store, frozen_root_t root)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t name = root_name(root);
    n00bb_report(diagnostics, unit, N00B_DIAG_WRITE_THROUGH_READ_ONLY,
        n00bb_span_location(store->span),
        root.global ? N00B_TEXT_CANNOT_WRITE_THROUGH_CONSTANT
        : root.parameter ? N00B_TEXT_CANNOT_WRITE_THROUGH_VIEW
                         : N00B_TEXT_CANNOT_WRITE_THROUGH_READ_ONLY,
        N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_label(diagnostics, N00B_TEXT_WRITTEN_HERE, N00BB_NO_ARGS);
    parse_sym_entry_t *parameter = root.parameter
        ? parameter_symbol(analysis, unit, root.parameter) : nullptr;
    if (root.global)
        note_global(analysis, diagnostics, root.global,
                    N00B_TEXT_IS_DECLARED_HERE);
    else if (parameter && parameter->decl_node)
        n00bb_report_secondary(diagnostics, parameter->owner_unit,
            link_source_location(parameter->decl_node),
            N00B_TEXT_IS_DECLARED_HERE, N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_help(diagnostics, root.global
        ? N00B_TEXT_DECLARE_WITH_VAR_TO_MAKE
        : root.parameter ? N00B_TEXT_DECLARE_AS_A_MODIFY_PARAMETER
                         : N00B_TEXT_COPY_IT_OUT_TO_CHANGE,
        N00BB_ARGS(ARG_TEXT(name)));
}

static void
report_call_write(n00bb_link_analysis_t *analysis,
                  core_translation_unit_t *unit, const ir_instr_t *call,
                  frozen_root_t root)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t name = root_name(root);
    n00bb_report(diagnostics, unit, N00B_DIAG_VIEW_WRITE,
        n00bb_span_location(call->span),
        root.global ? N00B_TEXT_CALL_MAY_WRITE_CONSTANT
        : root.parameter ? N00B_TEXT_CALL_MAY_WRITE_VIEW
                         : N00B_TEXT_CALL_MAY_WRITE_READ_ONLY,
        N00BB_ARGS(ARG_TEXT(name)));
    if (root.global)
        note_global(analysis, diagnostics, root.global,
                    N00B_TEXT_IS_DECLARED_HERE);
    if (root.parameter)
        n00bb_report_help(diagnostics, N00B_TEXT_TAKE_IT_AS_MODIFY,
                          N00BB_NO_ARGS);
    else
        n00bb_report_help(diagnostics, root.global
            ? N00B_TEXT_WRITE_TO_A_COPY : N00B_TEXT_COPY_IT_OUT_TO_CHANGE,
            N00BB_ARGS(ARG_TEXT(name)));
}

static void
report_escape(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
              const ir_instr_t *offender, frozen_root_t root,
              const ir_global_t *target)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    bool call = offender->op == IR_OP_CALL
        || offender->op == IR_OP_CALLBACK_INVOKE;
    bool shared = !root.parameter;
    n00bb_text_t message = root.global
        ? call ? N00B_TEXT_CALL_MAY_KEEP_CONSTANT : N00B_TEXT_STORES_CONSTANT
        : root.array
        ? call ? N00B_TEXT_CALL_MAY_KEEP_ARRAY : N00B_TEXT_STORES_ARRAY
        : root.parameter
        ? call ? N00B_TEXT_CALL_MAY_KEEP_VIEW : N00B_TEXT_STORES_VIEW
        : call ? N00B_TEXT_CALL_MAY_KEEP_READ_ONLY
               : N00B_TEXT_STORES_READ_ONLY;
    n00bb_report(diagnostics, unit,
        root.array ? N00B_DIAG_ARRAY_KEPT
        : shared ? N00B_DIAG_CONSTANT_ESCAPE : N00B_DIAG_VIEW_ESCAPE,
        n00bb_span_location(offender->span), message,
        N00BB_ARGS(ARG_TEXT(root_name(root))));
    if (root.global)
        note_global(analysis, diagnostics, root.global,
                    N00B_TEXT_IS_DECLARED_HERE);
    if (target)
        note_global(analysis, diagnostics, target, shared
            ? N00B_TEXT_IS_NOT_CONSTANT : N00B_TEXT_OUTLIVES_THE_CALL);
    else if (root.array && !call)
        n00bb_report_secondary(diagnostics, unit,
            n00bb_span_location(offender->span),
            N00B_TEXT_THIS_PLACE_OUTLIVES_THE_CALL, N00BB_NO_ARGS);
    if (root.array)
        n00bb_report_help(diagnostics, N00B_TEXT_CONSUME_OR_COPY_ARRAY,
            N00BB_ARGS(ARG_TEXT(root_name(root))));
    else
        n00bb_report_help(diagnostics, shared
            ? N00B_TEXT_KEEP_IT_AS_A_VIEW
            : N00B_TEXT_STORE_A_COPY_OR_RETURN, N00BB_NO_ARGS);
}

typedef struct {
    bool               outlives;
    bool               writable;
    const ir_global_t *global;
} store_target_t;

static void
classify_place(store_target_t *out, const ir_place_t *place)
{
    if (place->kind == IR_PLACE_LOCAL) return;
    out->outlives = true;
    const ir_global_t *global = (place->kind == IR_PLACE_GLOBAL
                                 || place->kind == IR_PLACE_THREAD)
        && place->root && place->root->global
        ? ir_global_identity(place->root->global) : nullptr;
    if (global && global->mutability == IR_MUT_CONST) return;
    out->writable = true;
    if (!out->global) out->global = global;
}

static store_target_t
store_target(ir_places_t *places, const ir_value_t *address)
{
    const uint32_t *ids = nullptr;
    uint32_t count = 0;
    store_target_t out = {0};
    if (!ir_places_of(places, address, &ids, &count) || !count)
        return (store_target_t){.outlives = true, .writable = true};
    for (uint32_t i = 0; i < count; i++)
        classify_place(&out, ir_place_at(places, ids[i]));
    return out;
}

static bool
check_memory_write(n00bb_link_analysis_t *analysis,
                   core_translation_unit_t *unit, ir_func_t *function,
                   ir_places_t *places, ir_instr_t *in)
{
    uint8_t access = ir_op_memory_access(in->op);
    int32_t address = ir_op_address_operand(in->op);
    if (!(access & IR_MEM_WRITE) || (access & IR_MEM_RELEASE) || address < 0
        || (uint32_t)address >= in->n_operands)
        return true;
    const ir_value_t *target = ir_operand_value(&in->operands[address]);
    if (!target) return true;
    bool replaces = in->op == IR_OP_GLOBAL_STORE
        || (in->op == IR_OP_STORE && target->kind == IR_VAL_GLOBAL);
    if (!replaces) {
        frozen_root_t root = frozen_root(analysis, function, target, true);
        if (frozen(root)) {
            report_write(analysis, unit, in, root);
            return false;
        }
    }
    int32_t stored = ir_op_stored_operand(in->op);
    const ir_value_t *value = stored >= 0 && (uint32_t)stored < in->n_operands
        ? ir_operand_value(&in->operands[stored]) : nullptr;
    th_ctx *types = ir_module_types(function->owner);
    if (!value || !value->type
        || !n00bb_type_reaches_shared_state(types, value->type)
        || read_only_reference(types, value->type))
        return true;
    store_target_t kept = {0};
    if (in->op == IR_OP_GLOBAL_STORE && target->global)
        classify_place(&kept, &(ir_place_t){.kind = IR_PLACE_GLOBAL,
                                            .root = target});
    else kept = store_target(places, target);
    if (!kept.outlives) return true;
    frozen_root_t root = frozen_root(analysis, function, value, false);
    if (!root.parameter && !(frozen(root) && kept.writable)) {
        const ir_value_t *array = lends_arrays(function)
            ? lent_array_root(value) : nullptr;
        if (!array) return true;
        root = (frozen_root_t){.parameter = array, .array = true};
    }
    report_escape(analysis, unit, in, root, kept.global);
    return false;
}

typedef struct {
    n00bb_link_analysis_t *analysis;
    n00bb_flow_t           flow;
    bool                   known;
} call_flow_t;

static th_type *
dereferenced(th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    while (type && th_kind_of(type) == TH_INDIRECTION)
        type = th_resolve(th_ind_referent(type));
    return type;
}

static parse_scope_t *
fields_of(n00bb_link_analysis_t *analysis, th_type *type)
{
    type = dereferenced(type);
    return type && th_kind_of(type) == TH_NAMED
        ? record_scope_for_type(analysis, type, nullptr) : nullptr;
}

static bool
bookkeeping_path(n00bb_link_analysis_t *analysis, const ir_func_t *callee,
                 const ir_spath_t *path)
{
    const ir_value_t *root = path->param < callee->n_params
        ? callee->params[path->param] : nullptr;
    parse_scope_t *scope = root ? fields_of(analysis, root->type) : nullptr;
    for (uint32_t step = 0; scope && step < path->depth; step++) {
        if (n00bb_builtin_member(path->steps[step])) continue;
        parse_sym_entry_t *field = parse_symtab_lookup_in(scope,
                                                          path->steps[step]);
        if (!field) return false;
        if (atomic_type(dereferenced(field->type)))
            return symbol_is_private(field);
        scope = fields_of(analysis, field->type);
    }
    return false;
}

static uint64_t
bookkeeping_only(n00bb_link_analysis_t *analysis, const ir_summary_t *summary)
{
    const ir_func_t *callee = ir_summary_function(summary);
    uint64_t kept = 0, written = 0;
    for (uint32_t i = 0; callee && i < ir_summary_path_count(summary); i++) {
        const ir_spath_t *path = ir_summary_path_at(summary, i);
        if (!(path->flags & IR_SPATH_WRITE)) continue;
        if (path->root != IR_SPLACE_PARAM) return 0;
        if (path->param >= 64) continue;
        uint64_t bit = UINT64_C(1) << path->param;
        if (bookkeeping_path(analysis, callee, path)) kept |= bit;
        else written |= bit;
    }
    return kept & ~written;
}

static void
merge_callee_flow(void *context, const ir_func_t *target,
                  const ir_summary_t *summary)
{
    (void)target;
    call_flow_t *merged = context;
    n00bb_flow_t flow;
    n00bb_summary_flow(summary, &flow);
    uint64_t bookkeeping = flow.opaque ? 0
        : bookkeeping_only(merged->analysis, summary);
    flow.mutates[N00BB_FLOW_SELF] &= ~bookkeeping;
    flow.mutates[N00BB_FLOW_CONTENTS] &= ~bookkeeping;
    for (int kind = 0; kind < N00BB_FLOW_KINDS; kind++) {
        merged->flow.mutates[kind] |= flow.mutates[kind];
        merged->flow.escapes[kind] |= flow.escapes[kind];
        merged->flow.leaks[kind] |= flow.leaks[kind];
    }
    merged->flow.opaque |= flow.opaque;
    merged->known = true;
}

static bool
kind_bit(const uint64_t *bits, int kind, uint32_t parameter)
{
    return parameter >= 64 ? bits[kind] == UINT64_MAX
                           : (bits[kind] >> parameter) & 1;
}

static bool
parameter_bit(const uint64_t *bits, uint32_t parameter)
{
    return kind_bit(bits, N00BB_FLOW_SELF, parameter)
        || kind_bit(bits, N00BB_FLOW_CONTENTS, parameter);
}

static bool
contents_carry_addresses(th_ctx *types, th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    while (type && th_kind_of(type) == TH_INDIRECTION)
        type = th_resolve(th_ind_referent(type));
    return !type || th_kind_of(type) != TH_SEQUENCE
        || n00bb_type_reaches_shared_state(types, th_seq_elem(type));
}

static bool
keeps_argument(th_ctx *types, const n00bb_flow_t *flow, uint32_t parameter,
               const ir_value_t *argument)
{
    return kind_bit(flow->escapes, N00BB_FLOW_SELF, parameter)
        || kind_bit(flow->leaks, N00BB_FLOW_SELF, parameter)
        || ((kind_bit(flow->escapes, N00BB_FLOW_CONTENTS, parameter)
             || kind_bit(flow->leaks, N00BB_FLOW_CONTENTS, parameter))
            && contents_carry_addresses(types, argument->type));
}

static bool
check_call(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
           ir_func_t *function, ir_instr_t *call)
{
    call_flow_t merged = {.analysis = analysis};
    bool resolved = ir_summary_call_callees(analysis->summaries, call,
                                            merge_callee_flow, &merged);
    bool opaque = !resolved || !merged.known || merged.flow.opaque;
    const ir_value_t *callee = ir_call_callee(call);
    th_type *signature = callee ? callable_signature(callee->type) : nullptr;
    for (uint32_t p = 0; p < ir_call_argument_count(call); p++) {
        if (signature && p < th_func_nparams(signature)
            && th_func_param(signature, p)->mode == TH_VIEW)
            continue;
        const ir_value_t *argument = ir_call_argument(call, p).value;
        th_ctx *types = ir_module_types(function->owner);
        if (!argument || read_only_reference(types, argument->type)) continue;
        bool writes = opaque || parameter_bit(merged.flow.mutates, p);
        bool keeps = opaque
            || keeps_argument(types, &merged.flow, p, argument);
        bool keeps_self = opaque
            || kind_bit(merged.flow.escapes, N00BB_FLOW_SELF, p)
            || kind_bit(merged.flow.leaks, N00BB_FLOW_SELF, p);
        if (!writes && !keeps) continue;
        frozen_root_t root = frozen_root(analysis, function, argument, false);
        if (writes && frozen(root)) {
            report_call_write(analysis, unit, call, root);
            return false;
        }
        if (keeps && frozen(root)) {
            report_escape(analysis, unit, call, root, nullptr);
            return false;
        }
        const ir_value_t *array = keeps_self && lends_arrays(function)
            ? lent_array_root(argument) : nullptr;
        if (array) {
            report_escape(analysis, unit, call,
                (frozen_root_t){.parameter = array, .array = true}, nullptr);
            return false;
        }
    }
    return true;
}

static bool
check_function(n00bb_link_analysis_t *analysis, core_translation_unit_t *unit,
               ir_func_t *function)
{
    if (!function || !function->n_blocks) return true;
    bool ok = true;
    size_t scope = ir_analysis_scope_begin();
    ir_places_t *places = ir_func_places(function);
    for (uint32_t b = 0; places && b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = function->blocks[b]->instrs[i];
            if (in->op == IR_OP_CALL || in->op == IR_OP_CALLBACK_INVOKE)
                ok = check_call(analysis, unit, function, in) && ok;
            else
                ok = check_memory_write(analysis, unit, function, places, in)
                    && ok;
        }
    ir_analysis_scope_end(scope);
    return ok;
}

bool
validate_frozen_writes(n00bb_link_analysis_t *analysis)
{
    bool ok = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        n00bb_tu_state_t *state = tu_state(unit);
        ir_module_t *module = state && !state->interface_module
            ? unit_module(state) : nullptr;
        for (size_t f = 0; module && f < ir_func_count(module); f++)
            ok = check_function(analysis, unit, ir_func_at(module, f)) && ok;
    }
    return ok;
}
