#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir/lower.h"
#include "private/ir/internal.h"
#include "private/parse/grammar.h"
#include "lib/dict.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"

typedef struct {
    const char *name;
    ir_lower_action_fn callback;
    void *state;
} ir_lower_action_entry_t;

typedef struct {
    parse_annot_kind_t kind;
    ir_lower_visit_fn callback;
    void *state;
} ir_lower_visit_entry_t;

typedef struct {
    const char *nonterminal;
    ir_lower_visit_fn callback;
    void *state;
} ir_lower_nt_visit_entry_t;

typedef struct ir_lower_symbol_binding_t {
    parse_sym_entry_t *symbol;
    ir_value_t *value;
    ir_local_t *local;
    ir_value_t *address;
    struct ir_lower_symbol_binding_t *next;
} ir_lower_symbol_binding_t;

typedef struct ir_lower_skip_frame_t {
    parse_tree_t *owner;
    parse_tree_t *reference;
    struct ir_lower_skip_frame_t *next;
} ir_lower_skip_frame_t;

struct ir_lowerer_t {
    ir_ctx_t *ctx;
    ir_lower_action_entry_t *actions;
    size_t action_count;
    size_t action_capacity;
    ir_lower_visit_entry_t *visitors;
    size_t visitor_count;
    size_t visitor_capacity;
    ir_lower_nt_visit_entry_t *nt_visitors;
    size_t nt_visitor_count;
    size_t nt_visitor_capacity;
};

typedef struct {
    ir_lowerer_t *lowerer;
    ir_lower_context_t *context;
    core_dict_t values;
    core_dict_t pruned_nodes;
    core_dict_t custom_assignments;
    core_dict_t handled_assignments;
    ir_lower_skip_frame_t *skip_values;
    parse_tree_t *active_node;
    bool active_entering;
    bool failed;
    bool terminated;
} ir_lower_tree_state_t;

static parse_tree_t *lower_tree_simple_lvalue(parse_tree_t *node);
static ir_lower_symbol_binding_t *lower_find_binding(
    ir_lower_context_t *context, parse_sym_entry_t *symbol);

static bool
lower_set_int_annot(ir_ctx_t *ctx, ir_annots_t *annots, const char *key,
                    uint64_t value)
{
    return ir_annot_set(ctx, annots, (ir_annot_t){
        .key = key,
        .kind = IR_ANNOT_INT,
        .i = {.mag = value},
    });
}

static bool
lower_set_bool_annot(ir_ctx_t *ctx, ir_annots_t *annots, const char *key,
                     bool value)
{
    return ir_annot_set(ctx, annots, (ir_annot_t){
        .key = key,
        .kind = IR_ANNOT_BOOL,
        .b = value,
    });
}

static bool
lower_set_string_annot(ir_ctx_t *ctx, ir_annots_t *annots, const char *key,
                       core_string_t value)
{
    return !value.data || ir_annot_set(ctx, annots, (ir_annot_t){
        .key = key,
        .kind = IR_ANNOT_STR,
        .s = ir_strdup(ctx, value.data),
    });
}

static ir_value_t *
lower_binding_value(ir_lower_context_t *context,
                   ir_lower_symbol_binding_t *binding)
{
    if (!binding) return nullptr;
    if (binding->value) return binding->value;
    return binding->local
        ? ir_local_value(context->function, binding->local) : nullptr;
}

static bool
lower_set_alias_annots(ir_lower_context_t *context, ir_annots_t *annots,
                       const ir_value_t *source, const char *prefix,
                       const parse_sym_alias_t *aliases, size_t count,
                       bool *unresolved)
{
    ir_ctx_t *ctx = context->module->ctx;
    char key[64];
    for (size_t i = 0; i < count; i++) {
        ir_lower_symbol_binding_t *target_binding =
            lower_find_binding(context, aliases[i].other);
        ir_value_t *target = target_binding && target_binding->address
            ? target_binding->address
            : lower_binding_value(context, target_binding);
        bool same_owner = target && source
            && ((target->kind == IR_VAL_GLOBAL
                 && target->global
                 && ir_global_find(context->module, target->name)
                        == target->global)
                || ((target->kind == IR_VAL_LOCAL
                     || target->kind == IR_VAL_PARAM
                     || target->kind == IR_VAL_INSTR)
                    && (source->kind == IR_VAL_LOCAL
                        || source->kind == IR_VAL_PARAM
                        || source->kind == IR_VAL_INSTR)
                    && source->owner
                    && source->owner == target->owner));
        if (!same_owner) {
            *unresolved = true;
            continue;
        }
        snprintf(key, sizeof(key), "ir.%s%zu", prefix, i);
        if (!ir_annot_set(ctx, annots, (ir_annot_t){
                .key = key,
                .kind = IR_ANNOT_VALUE,
                .v = target,
            })) return false;
        snprintf(key, sizeof(key), "ir.%s%zu_kind", prefix, i);
        if (!lower_set_int_annot(ctx, annots, key, aliases[i].kind))
            return false;
        snprintf(key, sizeof(key), "ir.%s%zu_derived", prefix, i);
        if (!lower_set_int_annot(ctx, annots, key, aliases[i].derived_at))
            return false;
        snprintf(key, sizeof(key), "ir.%s%zu_last_use", prefix, i);
        if (aliases[i].has_last_use) {
            if (!lower_set_int_annot(ctx, annots, key, aliases[i].last_use))
                return false;
        }
        snprintf(key, sizeof(key), "ir.%s%zu_has_last_use", prefix, i);
        if (!lower_set_bool_annot(ctx, annots, key,
                                  aliases[i].has_last_use)) return false;
        snprintf(key, sizeof(key), "ir.%s%zu_escapes", prefix, i);
        if (!lower_set_bool_annot(ctx, annots, key, aliases[i].escapes))
            return false;
    }
    return true;
}

static bool
lower_project_symbol_facts(ir_lower_context_t *context,
                           ir_lower_symbol_binding_t *binding)
{
    parse_sym_entry_t *symbol = binding->symbol;
    ir_value_t *value = binding->address
        ? binding->address : lower_binding_value(context, binding);
    ir_annots_t *annots = ir_value_annots(value);
    if (!annots) return false;

    ir_ctx_t *ctx = context->module->ctx;
    bool unresolved = false;
    ir_phase_access_t available = IR_PHASE_ACCESS_NONE;
    ir_phase_access_t mutable_in = IR_PHASE_ACCESS_NONE;
    ir_phase_access_t initializable_in = IR_PHASE_ACCESS_NONE;
    if (symbol->phases & PARSE_SYM_PHASE_COMPTIME)
        available |= IR_PHASE_ACCESS_COMPTIME;
    if (symbol->phases & PARSE_SYM_PHASE_RUNTIME)
        available |= IR_PHASE_ACCESS_RUNTIME;
    if (symbol->mutable_phases & PARSE_SYM_PHASE_COMPTIME)
        mutable_in |= IR_PHASE_ACCESS_COMPTIME;
    if (symbol->mutable_phases & PARSE_SYM_PHASE_RUNTIME)
        mutable_in |= IR_PHASE_ACCESS_RUNTIME;
    if (symbol->initializer_phases & PARSE_SYM_PHASE_COMPTIME)
        initializable_in |= IR_PHASE_ACCESS_COMPTIME;
    if (symbol->initializer_phases & PARSE_SYM_PHASE_RUNTIME)
        initializable_in |= IR_PHASE_ACCESS_RUNTIME;
    if (binding->local && symbol->kind == PARSE_SYM_PARAM)
        initializable_in |= available;
    if (!ir_value_set_phase_access(ctx, value, available, mutable_in,
                                   initializable_in)
        || !lower_set_string_annot(ctx, annots, "ir.storage_requested",
                                   symbol->requested_storage_class)
        || !lower_set_string_annot(ctx, annots, "ir.storage_selected",
                                   symbol->selected_storage_class)
        || !lower_set_bool_annot(ctx, annots, "ir.storage_required",
                                 symbol->storage_class_required)
        || !lower_set_int_annot(ctx, annots, "ir.storage_duration",
                                symbol->storage_duration)
        || !lower_set_int_annot(ctx, annots, "ir.alias_count",
                                symbol->alias_count)
        || !lower_set_int_annot(ctx, annots, "ir.aliased_by_count",
                                symbol->aliased_by_count)) return false;
    if (symbol->lifetime.has_region
        && !lower_set_int_annot(ctx, annots, "ir.lifetime_region",
                                symbol->lifetime.region)) return false;
    if (symbol->lifetime.has_start
        && !lower_set_int_annot(ctx, annots, "ir.lifetime_start",
                                symbol->lifetime.start_point)) return false;
    if (symbol->lifetime.has_end
        && !lower_set_int_annot(ctx, annots, "ir.lifetime_end",
                                symbol->lifetime.end_point)) return false;
    if (symbol->lifetime.has_finalization_point
        && !lower_set_int_annot(ctx, annots, "ir.finalization_point",
                                symbol->lifetime.finalization_point))
        return false;

    if (!lower_set_alias_annots(context, annots, value, "alias",
                                symbol->aliases, symbol->alias_count,
                                &unresolved)
        || !lower_set_alias_annots(context, annots, value, "aliased_by",
                                   symbol->aliased_by,
                                   symbol->aliased_by_count, &unresolved)
        || !lower_set_bool_annot(ctx, annots, "ir.lifetime_escapes",
                                 symbol->lifetime.escapes || unresolved)
        || !lower_set_bool_annot(ctx, annots,
                                 "ir.alias_analysis_complete",
                                 symbol->alias_analysis_complete
                                     && !unresolved)
        || !lower_set_bool_annot(ctx, annots, "ir.alias_unresolved",
                                 unresolved)) return false;
    return true;
}

ir_lowerer_t *
ir_lowerer_new(ir_ctx_t *ctx)
{
    if (!ctx) return nullptr;
    ir_lowerer_t *lowerer = ir_alloc(ctx, sizeof(*lowerer));
    lowerer->ctx = ctx;
    return lowerer;
}

ir_value_t *
ir_lower_node_value(ir_lower_context_t *context, parse_tree_t *node)
{
    if (!context || !context->tree_values || !node) return nullptr;
    bool found = false;
    ir_value_t *value = core_dict_get(context->tree_values, node, &found);
    return found ? value : nullptr;
}

bool
ir_lower_set_node_value(ir_lower_context_t *context, parse_tree_t *node,
                        ir_value_t *value)
{
    if (!context || !context->tree_values || !context->function || !node
        || !value || value->owner != context->function)
        return false;
    core_dict_put(context->tree_values, node, value);
    return true;
}

bool
ir_lower_skip_value(ir_lower_context_t *context, parse_tree_t *owner,
                    parse_tree_t *node)
{
    if (!context || !context->tree_state || !owner || !node) return false;
    ir_lower_tree_state_t *state = context->tree_state;
    ir_lower_skip_frame_t *mark = core_alloc(ir_lower_skip_frame_t);
    if (!mark) return false;
    *mark = (ir_lower_skip_frame_t){
        .owner = owner,
        .reference = node,
        .next = state->skip_values,
    };
    state->skip_values = mark;
    return true;
}

bool
ir_lower_handle_assignment(ir_lower_context_t *context,
                           parse_tree_t *assignment)
{
    if (!context || !context->tree_state || !assignment) return false;
    ir_lower_tree_state_t *state = context->tree_state;
    if (state->active_node != assignment || !state->active_entering)
        return false;
    core_dict_put(&state->custom_assignments, assignment, (void *)1);
    core_dict_put(&state->handled_assignments, assignment, (void *)1);
    return true;
}

bool
ir_lower_evaluate_assignment_target(ir_lower_context_t *context,
                                    parse_tree_t *assignment)
{
    if (!context || !context->tree_state || !assignment) return false;
    ir_lower_tree_state_t *state = context->tree_state;
    if (state->active_node != assignment || !state->active_entering)
        return false;
    core_dict_put(&state->custom_assignments, assignment, (void *)1);
    return true;
}

bool
ir_lower_prune_node(ir_lower_context_t *context, parse_tree_t *node)
{
    if (!context || !context->tree_state || !node) return false;
    ir_lower_tree_state_t *state = context->tree_state;
    if (state->active_node != node) return false;
    core_dict_put(&state->pruned_nodes, node, (void *)1);
    return true;
}

bool
ir_lower_mark_lvalue(ir_lower_context_t *context, parse_tree_t *owner,
                     parse_tree_t *node)
{
    return ir_lower_skip_value(context, owner, node);
}

ir_value_t *
ir_lower_child_value(ir_lower_context_t *context, parse_visit_t *visit,
                     parse_child_ref_t child)
{
    if (!context || !context->tree_values || !visit || !visit->grammar)
        return nullptr;
    parse_tree_t *node = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, child);
    return ir_lower_node_value(context, node);
}

static bool
lower_bind_symbol(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                  ir_value_t *value, ir_local_t *local,
                  ir_value_t *address, bool scoped)
{
    if (!context || !context->module || !context->function || !symbol
        || (!value && !local) || (address && !local))
        return false;
    if (!scoped) {
        for (ir_lower_symbol_binding_t *binding = context->symbol_bindings;
             binding; binding = binding->next) {
            if (binding->symbol == symbol) return false;
        }
    }
    if ((value && value->owner != context->function
         && !(value->kind == IR_VAL_GLOBAL
              && ir_global_find(context->module, value->name))
         && !(value->kind == IR_VAL_FUNC && value->owner
              && value->owner->owner == context->module))
        || (local && ir_local_find(context->function, local->name) != local)
        || (address && (address->owner != context->function
            || !th_ind_referent(address->type)
            || !ir_type_eq(context->module, th_ind_referent(address->type),
                           local->type))))
        return false;
    if (!scoped && (symbol->kind == PARSE_SYM_VARIABLE
                    || symbol->kind == PARSE_SYM_PARAM)) {
        ir_value_t *phase_binding = address ? address : local
            ? ir_local_value(context->function, local) : value;
        parse_sym_phase_t phase = ir_func_execution_phase(context->function)
                == IR_PHASE_COMPTIME
            ? PARSE_SYM_PHASE_COMPTIME : PARSE_SYM_PHASE_RUNTIME;
        if (!phase_binding
            || !parse_sym_set_phase_binding(symbol, phase, phase_binding)
            || !parse_sym_set_function_binding(symbol, phase,
                                               context->function,
                                               phase_binding))
            return false;
    }
    ir_lower_symbol_binding_t *binding =
        ir_alloc(context->module->ctx, sizeof(*binding));
    *binding = (ir_lower_symbol_binding_t){
        .symbol = symbol,
        .value = value,
        .local = local,
        .address = address,
        .next = context->symbol_bindings,
    };
    context->symbol_bindings = binding;
    if (scoped) return true;
    if (!lower_project_symbol_facts(context, binding)) return false;

    for (ir_lower_symbol_binding_t *at = binding->next; at; at = at->next) {
        bool refers_to_symbol = false;
        for (size_t i = 0; i < at->symbol->alias_count; i++)
            refers_to_symbol |= at->symbol->aliases[i].other == symbol;
        for (size_t i = 0; i < at->symbol->aliased_by_count; i++)
            refers_to_symbol |= at->symbol->aliased_by[i].other == symbol;
        if (refers_to_symbol && !lower_project_symbol_facts(context, at))
            return false;
    }
    return true;
}

bool
ir_lower_bind_value(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                    ir_value_t *value)
{
    return value && lower_bind_symbol(context, symbol, value, nullptr, nullptr,
                                      false);
}

bool
ir_lower_bind_local(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                    ir_local_t *local)
{
    return local && lower_bind_symbol(context, symbol, nullptr, local, nullptr,
                                      false);
}

bool
ir_lower_rebind_local(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                      ir_local_t *local)
{
    if (!context || !symbol || !local
        || ir_local_find(context->function, local->name) != local)
        return false;
    ir_lower_symbol_binding_t *binding = lower_find_binding(context, symbol);
    if (!binding) return ir_lower_bind_local(context, symbol, local);
    if (symbol->kind == PARSE_SYM_VARIABLE
        || symbol->kind == PARSE_SYM_PARAM) {
        parse_sym_phase_t phase = ir_func_execution_phase(context->function)
                == IR_PHASE_COMPTIME
            ? PARSE_SYM_PHASE_COMPTIME : PARSE_SYM_PHASE_RUNTIME;
        ir_value_t *phase_binding = ir_local_value(context->function, local);
        if (!phase_binding
            || !parse_sym_set_phase_binding(symbol, phase, phase_binding)
            || !parse_sym_set_function_binding(symbol, phase,
                                               context->function,
                                               phase_binding)) return false;
    }
    binding->value = nullptr;
    binding->local = local;
    binding->address = nullptr;
    if (!lower_project_symbol_facts(context, binding)) return false;
    for (ir_lower_symbol_binding_t *at = binding->next; at; at = at->next) {
        bool refers_to_symbol = false;
        for (size_t i = 0; i < at->symbol->alias_count; i++)
            refers_to_symbol |= at->symbol->aliases[i].other == symbol;
        for (size_t i = 0; i < at->symbol->aliased_by_count; i++)
            refers_to_symbol |= at->symbol->aliased_by[i].other == symbol;
        if (refers_to_symbol && !lower_project_symbol_facts(context, at))
            return false;
    }
    return true;
}

bool
ir_lower_bind_local_storage(ir_lower_context_t *context,
                            parse_sym_entry_t *symbol, ir_local_t *local,
                            ir_value_t *address)
{
    return local && address
        && lower_bind_symbol(context, symbol, nullptr, local, address, false);
}

void *
ir_lower_bindings_checkpoint(ir_lower_context_t *context)
{
    return context ? context->symbol_bindings : nullptr;
}

bool
ir_lower_bindings_restore(ir_lower_context_t *context, void *checkpoint)
{
    if (!context) return false;
    ir_lower_symbol_binding_t *mark = checkpoint;
    if (mark) {
        bool present = false;
        for (ir_lower_symbol_binding_t *binding = context->symbol_bindings;
             binding; binding = binding->next) {
            if (binding == mark) {
                present = true;
                break;
            }
        }
        if (!present) return false;
    }
    context->symbol_bindings = mark;
    return true;
}

bool
ir_lower_bind_scoped_value(ir_lower_context_t *context,
                           parse_sym_entry_t *symbol, ir_value_t *value,
                           ir_local_t *local)
{
    ir_lower_symbol_binding_t *prior =
        lower_find_binding(context, symbol);
    ir_value_t *address = prior && prior->local == local
        ? prior->address : nullptr;
    return lower_bind_symbol(context, symbol, value, local, address, true);
}

ir_local_t *
ir_lower_bound_local(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    if (!context || !symbol) return nullptr;
    ir_lower_symbol_binding_t *binding = lower_find_binding(context, symbol);
    return binding ? binding->local : nullptr;
}

ir_value_t *
ir_lower_bound_address(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    if (!context || !symbol) return nullptr;
    ir_lower_symbol_binding_t *binding = lower_find_binding(context, symbol);
    return binding ? binding->address : nullptr;
}

ir_value_t *
ir_lower_local_address(ir_lower_context_t *context, ir_local_t *local)
{
    if (!context || !local) return nullptr;
    for (ir_lower_symbol_binding_t *binding = context->symbol_bindings;
         binding; binding = binding->next) {
        if (binding->local == local) return binding->address;
    }
    return nullptr;
}

ir_value_t *
ir_lower_bound_value(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    if (!context || !symbol) return nullptr;
    return lower_binding_value(context, lower_find_binding(context, symbol));
}

bool
ir_lowerer_register(ir_lowerer_t *lowerer, const char *action,
                    ir_lower_action_fn callback, void *state)
{
    if (!lowerer || !action || !*action || !callback) return false;
    for (size_t i = 0; i < lowerer->action_count; i++) {
        if (!strcmp(lowerer->actions[i].name, action)) return false;
    }
    size_t cap = lowerer->action_capacity;
    lowerer->actions = ir_grow(lowerer->ctx, lowerer->actions, &cap,
                               lowerer->action_count + 1,
                               sizeof(*lowerer->actions));
    if (!lowerer->actions) return false;
    lowerer->action_capacity = cap;
    lowerer->actions[lowerer->action_count++] = (ir_lower_action_entry_t){
        .name = ir_strdup(lowerer->ctx, action),
        .callback = callback,
        .state = state,
    };
    return true;
}

bool
ir_lowerer_register_visit(ir_lowerer_t *lowerer, parse_annot_kind_t kind,
                          ir_lower_visit_fn callback, void *state)
{
    if (!lowerer || kind < 0 || kind >= PARSE_ANNOT_KIND_COUNT || !callback)
        return false;
    size_t cap = lowerer->visitor_capacity;
    lowerer->visitors = ir_grow(lowerer->ctx, lowerer->visitors, &cap,
                                lowerer->visitor_count + 1,
                                sizeof(*lowerer->visitors));
    if (!lowerer->visitors) return false;
    lowerer->visitor_capacity = cap;
    lowerer->visitors[lowerer->visitor_count++] = (ir_lower_visit_entry_t){
        .kind = kind,
        .callback = callback,
        .state = state,
    };
    return true;
}

bool
ir_lowerer_register_nt_visit(ir_lowerer_t *lowerer, const char *nonterminal,
                             ir_lower_visit_fn callback, void *state)
{
    if (!lowerer || !nonterminal || !*nonterminal || !callback) return false;
    size_t cap = lowerer->nt_visitor_capacity;
    lowerer->nt_visitors = ir_grow(lowerer->ctx, lowerer->nt_visitors, &cap,
        lowerer->nt_visitor_count + 1, sizeof(*lowerer->nt_visitors));
    if (!lowerer->nt_visitors) return false;
    lowerer->nt_visitor_capacity = cap;
    lowerer->nt_visitors[lowerer->nt_visitor_count++] =
        (ir_lower_nt_visit_entry_t){
            .nonterminal = ir_strdup(lowerer->ctx, nonterminal),
            .callback = callback,
            .state = state,
        };
    return true;
}

static ir_lower_result_t
lower_failure(ir_lower_context_t *context, ir_diagcode_t code,
              ir_span_t span, const char *message)
{
    if (context && context->diagnostics) {
        ir_diag_t *diagnostic = ir_diag_begin(context->diagnostics, code,
                                              span, nullptr);
        ir_diag_message(diagnostic, context->diagnostics, message);
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

static ir_instr_t *
lower_append_instruction(ir_lower_context_t *context, ir_op_t op,
                        ir_span_t span, bool terminator)
{
    if (!context || !context->module || !context->function || !context->block
        || context->function->owner != context->module
        || context->block->owner != context->function) {
        lower_failure(context, IR_DIAG_INTERNAL, span,
                      "lowering context objects do not belong together");
        return nullptr;
    }
    if (!context->reachable || ir_block_terminator(context->block)) {
        lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                      "cannot emit an instruction in an unreachable or terminated block");
        return nullptr;
    }
    if (ir_op_is_terminator(op) != terminator) {
        lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                      terminator
                          ? "terminator helper requires a terminator operation"
                          : "instruction helper cannot emit a terminator operation");
        return nullptr;
    }
    ir_instr_t *instruction = ir_instr_add(context->block, op);
    if (!instruction) {
        lower_failure(context, IR_DIAG_INTERNAL, span,
                      "could not append an IR instruction");
        return nullptr;
    }
    instruction->span = span;
    if (terminator) context->reachable = false;
    return instruction;
}

ir_instr_t *
ir_lower_emit_instruction(ir_lower_context_t *context, ir_op_t op,
                          ir_span_t span)
{
    return lower_append_instruction(context, op, span, false);
}

ir_instr_t *
ir_lower_emit_terminator(ir_lower_context_t *context, ir_op_t op,
                         ir_span_t span)
{
    return lower_append_instruction(context, op, span, true);
}

static ir_lower_symbol_binding_t *
lower_find_binding(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    for (parse_sym_entry_t *view = symbol; view;
         view = parse_sym_binding_origin(view)) {
        for (ir_lower_symbol_binding_t *binding = context->symbol_bindings;
             binding; binding = binding->next) {
            if (binding->symbol == view) return binding;
        }
    }
    return nullptr;
}

static bool
lower_value_accessible(ir_lower_context_t *context, ir_value_t *value)
{
    if (!context || !value) return false;
    if (value->owner == context->function) return true;
    if (value->kind == IR_VAL_GLOBAL)
        return ir_global_find(context->module, value->name) != nullptr;
    return value->kind == IR_VAL_FUNC && value->owner
        && value->owner->owner == context->module;
}

static parse_annotation_t *
lower_find_annotation(parse_visit_t *visit, parse_annot_kind_t kind)
{
    parse_annotation_t *annotation = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule, kind) : nullptr;
    if (!annotation && visit->nt)
        annotation = parse_nt_find_direct_annot(visit->nt, kind);
    return annotation;
}

static ir_value_t *
lower_emit_local_read(ir_lower_context_t *context, ir_local_t *local,
                     ir_span_t span)
{
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_COPY);
    char name[48];
    do {
        snprintf(name, sizeof(name), "__lowered_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, name));
    if (!instruction || !ir_instr_set_dest(instruction, name, local->type)
        || !ir_instr_operand_local(instruction, local))
        return nullptr;
    instruction->span = span;
    return instruction->dest;
}

static ir_value_t *
lower_emit_address_read(ir_lower_context_t *context, ir_value_t *address,
                       th_type *type, ir_span_t span)
{
    if (!address || !type) return nullptr;
    ir_instr_t *instruction = ir_instr_add(context->block, IR_OP_LOAD);
    char name[48];
    do {
        snprintf(name, sizeof(name), "__lowered_%llu",
                 (unsigned long long)context->next_value_id++);
    } while (ir_value_find(context->function, name));
    if (!instruction || !ir_instr_set_dest(instruction, name, type)
        || !ir_instr_operand_value(instruction, address)) return nullptr;
    instruction->span = span;
    return instruction->dest;
}

static bool
lower_parse_integer(core_string_t text, ir_lit_t *literal)
{
    if (!text.data || !text.u8_bytes) return false;
    size_t i = 0;
    unsigned base = 10;
    bool hex = false;
    if (text.u8_bytes >= 2 && text.data[0] == '0') {
        if (text.data[1] == 'x' || text.data[1] == 'X') {
            base = 16;
            hex = true;
            i = 2;
        }
        else if (text.data[1] == 'b' || text.data[1] == 'B') {
            base = 2;
            i = 2;
        }
        else if (text.data[1] == 'o' || text.data[1] == 'O') {
            base = 8;
            i = 2;
        }
    }
    ir_uint_t value = 0;
    bool saw_digit = false;
    for (; i < text.u8_bytes; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '_') continue;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (digit >= base || value > (~(ir_uint_t)0 - digit) / base)
            return false;
        value = value * base + digit;
        saw_digit = true;
    }
    if (!saw_digit) return false;
    *literal = (ir_lit_t){.mag = value, .hex = hex};
    return true;
}

static ir_lower_result_t
lower_builtin_action(ir_lower_context_t *context, const char *action,
                     parse_visit_t *visit, size_t operand_count,
                     ir_value_t *const *operands, th_type *result_type,
                     ir_span_t span)
{
    if (!strcmp(action, "identity")) {
        if (operand_count == 1 && operands && operands[0])
            return (ir_lower_result_t){IR_LOWER_VALUE, operands[0]};
        if (operand_count == 1 && operands && !operands[0])
            return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "identity lowering requires one child value");
    }
    if (!strcmp(action, "reference")) {
        parse_node_resolution_t resolution = visit && visit->node
            ? parse_tree_resolution(visit->node)
            : (parse_node_resolution_t){0};
        parse_sym_entry_t *symbol = resolution.kind
                == PARSE_NODE_RESOLUTION_SYMBOL
            ? resolution.target.symbol
            : resolution.kind == PARSE_NODE_RESOLUTION_SCOPE ? nullptr
            : visit && visit->pn ? visit->pn->sym : nullptr;
        ir_lower_symbol_binding_t *binding =
            lower_find_binding(context, symbol);
        if (!binding && symbol
            && (symbol->kind == PARSE_SYM_MODULE
                || symbol->kind == PARSE_SYM_TYPEDEF))
            return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
        if (!binding)
            return lower_failure(context, IR_DIAG_UNDEFINED_SYMBOL, span,
                                 "reference has no IR binding in this lowering context");
        ir_value_t *value = binding->value ? binding->value
            : binding->address
                ? lower_emit_address_read(context, binding->address,
                                          binding->local->type, span)
                : lower_emit_local_read(context, binding->local, span);
        if (!value)
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit a local read");
        if (result_type && !ir_type_eq(context->module, value->type,
                                       result_type))
            return lower_failure(context, IR_DIAG_TYPE_MISMATCH, span,
                                 "reference and inferred node types differ");
        return (ir_lower_result_t){IR_LOWER_VALUE, value};
    }
    if (!strcmp(action, "literal")) {
        if (operand_count || !result_type)
            return lower_failure(context, IR_DIAG_VERIFY_TYPED, span,
                                 "literal lowering needs a result type and no child values");
        parse_annotation_t *literal_annotation =
            lower_find_annotation(visit, PARSE_ANNOT_LITERAL);
        parse_token_info_t *token = parse_tree_find_first_terminal(
            visit ? visit->node : nullptr);
        if (!literal_annotation || !token
            || !core_option_is_set(token->value))
            return lower_failure(context, IR_DIAG_TYPE_LITERAL, span,
                                 "literal metadata or token value is missing");
        core_string_t text = core_option_get(token->value);
        ir_instr_t *instruction = ir_lower_emit_instruction(
            context, IR_OP_CONST, span);
        if (!instruction) return (ir_lower_result_t){.status = IR_LOWER_FAILED};
        char name[48];
        do {
            snprintf(name, sizeof(name), "__lowered_%llu",
                     (unsigned long long)context->next_value_id++);
        } while (ir_value_find(context->function, name));
        if (!ir_instr_set_dest(instruction, name, result_type))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit a literal value");
        bool emitted = false;
        if (core_string_eq(literal_annotation->op_kind,
                           CORE_STRING_STATIC("bool"))) {
            if (core_string_eq(text, CORE_STRING_STATIC("true")))
                emitted = ir_instr_operand_int(instruction, 1);
            else if (core_string_eq(text, CORE_STRING_STATIC("false")))
                emitted = ir_instr_operand_int(instruction, 0);
        }
        else if (core_string_eq(literal_annotation->op_kind,
                                CORE_STRING_STATIC("int"))) {
            ir_lit_t value;
            if (lower_parse_integer(text, &value))
                emitted = ir_instr_operand_int_lit(instruction, value);
        }
        else if (core_string_eq(literal_annotation->op_kind,
                                CORE_STRING_STATIC("f64"))
                 || core_string_eq(literal_annotation->op_kind,
                                   CORE_STRING_STATIC("float"))) {
            errno = 0;
            char *end = nullptr;
            double value = strtod(text.data, &end);
            emitted = !errno && end == text.data + text.u8_bytes
                && ir_instr_operand_float(instruction, value);
        }
        if (!emitted)
            return lower_failure(context, IR_DIAG_TYPE_LITERAL, span,
                                 "literal spelling is not supported by generic scalar lowering");
        return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

ir_lower_result_t
ir_lower_emit(ir_lowerer_t *lowerer, ir_lower_context_t *context,
              const parse_annotation_t *annotation, parse_visit_t *visit,
              ir_value_t *const *operands, size_t operand_count,
              th_type *result_type, ir_span_t span)
{
    if (!lowerer || !context || !annotation
        || annotation->kind != PARSE_ANNOT_EMIT
        || !annotation->emit_action.data || !context->module
        || !context->function || !context->block || !context->reachable)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (context->function->owner != context->module
        || context->block->owner != context->function)
        return lower_failure(context, IR_DIAG_INTERNAL, span,
                             "lowering context objects do not belong together");
    if (core_list_len(annotation->emit_operands) != operand_count)
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "resolved operand count does not match the emit annotation");

    const char *action = annotation->emit_action.data;
    for (size_t i = 0; i < lowerer->action_count; i++) {
        ir_lower_action_entry_t *entry = &lowerer->actions[i];
        if (strcmp(entry->name, action)) continue;
        ir_lower_result_t result = entry->callback(
            context, visit, operands, operand_count, result_type, span,
            entry->state);
        if (result.status > IR_LOWER_FAILED
            || ((result.status == IR_LOWER_VALUE)
                != (result.value != nullptr))
            || (result.value && result.value->owner != context->function))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "lowering callback returned an inconsistent result");
        if (result.status == IR_LOWER_TERMINATED) context->reachable = false;
        return result;
    }

    if (!strcmp(action, "identity") || !strcmp(action, "reference")
        || !strcmp(action, "literal")) {
        ir_lower_result_t result = lower_builtin_action(
            context, action, visit, operand_count, operands, result_type, span);
        if (result.status == IR_LOWER_TERMINATED)
            context->reachable = false;
        return result;
    }

    ir_op_t op;
    if (!ir_op_lookup(action, &op))
        return lower_failure(context, IR_DIAG_UNKNOWN_OPCODE, span,
                             "no lowering callback or IR operation matches this action");
    if (operand_count > UINT32_MAX
        || !ir_op_accepts_operands(op, (uint32_t)operand_count))
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "lowered operation has an invalid operand count");
    for (size_t i = 0; i < operand_count; i++) {
        ir_opdkind_t kind = ir_op_operand_kind(op, (uint32_t)i);
        if (!operands || !lower_value_accessible(context, operands[i])
            || (kind != IR_OPD_VALUE && kind != IR_OPD_ARG)) {
            char message[160];
            snprintf(message, sizeof(message),
                     "declarative action '%s' has no accessible value for operand %zu",
                     action, i);
            return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                                 message);
        }
    }
    bool has_result = ir_op_has_result(op);
    if (has_result && !result_type)
        return lower_failure(context, IR_DIAG_VERIFY_TYPED, span,
                             "lowered operation result type does not match its IR signature");

    ir_instr_t *instruction = ir_op_is_terminator(op)
        ? ir_lower_emit_terminator(context, op, span)
        : ir_lower_emit_instruction(context, op, span);
    if (!instruction) return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    for (size_t i = 0; i < operand_count; i++) {
        if (!ir_instr_operand_value(instruction, operands[i]))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not attach a lowered IR operand");
    }

    ir_lower_result_t result = {.status = IR_LOWER_NO_VALUE};
    if (has_result) {
        char name[48];
        do {
            snprintf(name, sizeof(name), "__lowered_%llu",
                     (unsigned long long)context->next_value_id++);
        } while (ir_value_find(context->function, name));
        if (!ir_instr_set_dest(instruction, name, result_type))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not bind the lowered IR result");
        result.status = IR_LOWER_VALUE;
        result.value = instruction->dest;
    }
    if (ir_op_is_terminator(op)) {
        result.status = IR_LOWER_TERMINATED;
        result.value = nullptr;
    }
    return result;
}

static bool
lower_tree_track_assignment(ir_lower_tree_state_t *state,
                            parse_visit_t *visit,
                            parse_annotation_t *annotation, bool entering)
{
    if (annotation->kind != PARSE_ANNOT_ASSIGNS) return true;
    if (entering) {
        if (core_dict_contains(&state->custom_assignments, visit->node))
            return true;
        parse_tree_t *target = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, annotation->name_ref);
        parse_tree_t *reference = lower_tree_simple_lvalue(target);
        if (!reference) return true;
        ir_lower_skip_frame_t *frame = core_alloc(ir_lower_skip_frame_t);
        *frame = (ir_lower_skip_frame_t){
            .owner = visit->node,
            .reference = reference,
            .next = state->skip_values,
        };
        state->skip_values = frame;
        return true;
    }
    ir_lower_skip_frame_t **at = &state->skip_values;
    while (*at && (*at)->owner != visit->node) at = &(*at)->next;
    if (*at) {
        ir_lower_skip_frame_t *frame = *at;
        *at = frame->next;
        core_free(frame);
    }
    core_dict_remove(&state->custom_assignments, visit->node);
    return true;
}

static bool
lower_tree_hooks(ir_lower_tree_state_t *state, parse_visit_t *visit,
                 bool entering)
{
    size_t nt_count = visit->nt ? parse_nt_annot_count(visit->nt) : 0;
    size_t rule_count = visit->rule ? parse_rule_annot_count(visit->rule) : 0;
    size_t total = nt_count + rule_count;
    bool ok = true;
    for (size_t k = 0; k < total; k++) {
        size_t annotation_ix = entering ? k : total - 1 - k;
        parse_annotation_t *annotation = annotation_ix < nt_count
            ? parse_nt_annot_at(visit->nt, annotation_ix)
            : parse_rule_annot_at(visit->rule, annotation_ix - nt_count);
        for (size_t j = 0; j < state->lowerer->visitor_count; j++) {
            size_t visitor_ix = entering
                ? j : state->lowerer->visitor_count - 1 - j;
            ir_lower_visit_entry_t *entry =
                &state->lowerer->visitors[visitor_ix];
            if (entry->kind != annotation->kind) continue;
            visit->annot = annotation;
            if (!entry->callback(state->context, visit, entering,
                                 entry->state))
                ok = false;
        }
        if (!lower_tree_track_assignment(state, visit, annotation, entering))
            ok = false;
    }
    visit->annot = nullptr;
    if (!ok) {
        state->failed = true;
        parse_tree_walk_stop(visit->walk);
    }
    return ok;
}

static bool
lower_tree_nt_hooks(ir_lower_tree_state_t *state, parse_visit_t *visit,
                    bool entering)
{
    if (!visit->nt) return true;
    core_string_t name = parse_tree_nt_name(visit->grammar, visit->node);
    for (size_t k = 0; k < state->lowerer->nt_visitor_count; k++) {
        size_t i = entering ? k : state->lowerer->nt_visitor_count - 1 - k;
        ir_lower_nt_visit_entry_t *entry =
            &state->lowerer->nt_visitors[i];
        if (!core_string_eq(name,
                            core_string_from_cstr(entry->nonterminal)))
            continue;
        visit->annot = nullptr;
        if (!entry->callback(state->context, visit, entering, entry->state)) {
            state->failed = true;
            parse_tree_walk_stop(visit->walk);
            return false;
        }
    }
    return true;
}

static bool
lower_tree_enter(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    state->active_node = visit->node;
    state->active_entering = true;
    bool ok = lower_tree_nt_hooks(state, visit, true);
    state->active_node = nullptr;
    state->active_entering = false;
    if (!ok) return false;
    if (core_dict_contains(&state->pruned_nodes, visit->node)) return false;
    state->active_node = visit->node;
    state->active_entering = true;
    ok = lower_tree_hooks(state, visit, true);
    state->active_node = nullptr;
    state->active_entering = false;
    return ok;
}

static parse_token_info_t *
last_terminal(parse_tree_t *node)
{
    if (!node) return nullptr;
    if (core_tree_is_leaf(node)) return parse_node_token(node);
    for (size_t i = core_tree_num_children(node); i > 0; i--) {
        parse_token_info_t *token = last_terminal(core_tree_child(node, i - 1));
        if (token) return token;
    }
    return nullptr;
}

static ir_span_t
span_of_tree(ir_lower_context_t *context, parse_tree_t *node)
{
    if (!context || context->file_id < 0) return IR_SPAN_UNKNOWN;
    parse_token_info_t *first = parse_tree_find_first_terminal(node);
    parse_token_info_t *last = last_terminal(node);
    if (!first || !last || first->start_byte > UINT32_MAX
        || last->end_byte > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(context->file_id, (uint32_t)first->start_byte,
                        (uint32_t)last->end_byte, first->line, first->column,
                        last->endline, last->endcol);
}

static parse_tree_t *
lower_tree_simple_lvalue(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        parse_sym_entry_t *symbol = core_tree_node_value(node).sym;
        if (symbol) return node;
        if (core_tree_num_children(node) != 1) return nullptr;
        node = core_tree_child(node, 0);
    }
    return nullptr;
}

static bool
lower_symbol_defines_at(const parse_sym_entry_t *symbol, parse_tree_t *node)
{
    if (!symbol || !node) return false;
    for (size_t i = 0; i < symbol->definition_count; i++)
        if (symbol->definition_nodes[i] == node) return true;
    return false;
}

static bool
lower_tree_is_suppressed_value(ir_lower_tree_state_t *state,
                                parse_tree_t *node)
{
    for (ir_lower_skip_frame_t *target = state->skip_values;
         target; target = target->next)
        for (parse_tree_t *ancestor = node; ancestor;
             ancestor = core_tree_node_value(ancestor).parent)
            if (target->reference == ancestor) return true;
    return false;
}

static void
lower_tree_clear_lvalues(ir_lower_tree_state_t *state, parse_tree_t *owner)
{
    ir_lower_skip_frame_t **at = &state->skip_values;
    while (*at) {
        if ((*at)->owner != owner) {
            at = &(*at)->next;
            continue;
        }
        ir_lower_skip_frame_t *mark = *at;
        *at = mark->next;
        core_free(mark);
    }
}

static ir_lower_result_t
lower_tree_assignment(ir_lower_tree_state_t *state, parse_visit_t *visit,
                      parse_annotation_t *annotation, ir_span_t span)
{
    parse_tree_t *target = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, annotation->name_ref);
    parse_tree_t *source = parse_tree_resolve_child_ref(
        visit->grammar, visit->node, annotation->value_ref);
    parse_sym_entry_t *symbol = nullptr;
    bool initializes = core_string_eq(
        parse_tree_nt_name(visit->grammar, visit->node),
        CORE_STRING_STATIC("sym-decl"));
    if (initializes)
        symbol = core_tree_node_value(visit->node).sym;
    else {
        parse_tree_t *lvalue = lower_tree_simple_lvalue(target);
        symbol = lvalue ? core_tree_node_value(lvalue).sym : nullptr;
        if (!symbol && visit->pn) symbol = visit->pn->sym;
    }
    initializes = initializes
        || (symbol && (symbol->decl_node == visit->node
            || lower_find_annotation(visit, PARSE_ANNOT_DECLARES)
            || lower_symbol_defines_at(symbol, visit->node)));
    ir_lower_symbol_binding_t *binding =
        lower_find_binding(state->context, symbol);
    bool found = false;
    ir_value_t *value = source
        ? core_dict_get(&state->values, source, &found) : nullptr;
    if (!binding || !found || !value)
        return lower_failure(state->context, IR_DIAG_VERIFY_PARSED, span,
                             "assignment needs a bound destination and value");
    if (!initializes) {
        ir_execution_phase_t phase = ir_func_execution_phase(
            state->context->function);
        ir_value_t *phase_binding = binding->address
            ? binding->address : lower_binding_value(state->context, binding);
        if (phase_binding && phase_binding->kind == IR_VAL_GLOBAL
            && phase_binding->global)
            initializes = !ir_global_can_write(phase_binding->global, phase)
                && ir_global_can_initialize(phase_binding->global, phase);
        else if (phase_binding)
            initializes = !ir_value_can_write(phase_binding, phase)
                && ir_value_can_initialize(phase_binding, phase);
    }
    if (binding->address) {
        th_type *referent = th_ind_referent(binding->address->type);
        if (!referent || !ir_type_eq(state->context->module, referent,
                                     value->type))
            return lower_failure(state->context, IR_DIAG_TYPE_MISMATCH, span,
                                 "assigned value does not match the stored type");
        ir_instr_t *instruction = ir_instr_add(state->context->block,
                                               IR_OP_STORE);
        if (!instruction
            || !ir_instr_set_binding_initialization(
                instruction, initializes)
            || !ir_instr_operand_value(instruction, binding->address)
            || !ir_instr_operand_value(instruction, value))
            return lower_failure(state->context, IR_DIAG_INTERNAL, span,
                                 "could not emit an address-backed assignment");
        instruction->span = span;
    }
    else if (binding->local) {
        if (!ir_type_eq(state->context->module, binding->local->type,
                        value->type))
            return lower_failure(state->context, IR_DIAG_TYPE_MISMATCH, span,
                                 "assigned value does not match the local type");
        ir_instr_t *instruction = ir_instr_add(state->context->block,
                                               IR_OP_COPY);
        if (!instruction
            || !ir_instr_set_binding_initialization(
                instruction, initializes)
            || !ir_instr_set_dest_local(instruction, binding->local)
            || !ir_instr_operand_value(instruction, value))
            return lower_failure(state->context, IR_DIAG_INTERNAL, span,
                                 "could not emit a local assignment");
        instruction->span = span;
    }
    else if (binding->value && binding->value->kind == IR_VAL_GLOBAL) {
        ir_instr_t *instruction = ir_instr_add(state->context->block,
                                               IR_OP_GLOBAL_STORE);
        if (!instruction || !ir_instr_operand_value(instruction,
                                                     binding->value)
            || !ir_instr_set_binding_initialization(
                instruction, initializes)
            || !ir_instr_operand_value(instruction, value))
            return lower_failure(state->context, IR_DIAG_INTERNAL, span,
                                 "could not emit a global assignment");
        instruction->span = span;
    }
    else {
        return lower_failure(state->context, IR_DIAG_VERIFY_PARSED, span,
                             "assignment destination is not writable storage");
    }
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

static bool
lower_tree_node(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    if (core_dict_contains(&state->pruned_nodes, visit->node)) {
        bool ok = lower_tree_nt_hooks(state, visit, false);
        core_dict_remove(&state->pruned_nodes, visit->node);
        if (!ir_lower_node_value(state->context, visit->node))
            core_dict_put(&state->values, visit->node, (void *)0);
        lower_tree_clear_lvalues(state, visit->node);
        return ok;
    }
    state->active_node = visit->node;
    state->active_entering = false;
    bool hooks_ok = lower_tree_hooks(state, visit, false);
    bool nt_hooks_ok = lower_tree_nt_hooks(state, visit, false);
    state->active_node = nullptr;
    state->active_entering = false;
    lower_tree_clear_lvalues(state, visit->node);
    if (state->failed || !hooks_ok || !nt_hooks_ok) return false;
    if (core_dict_contains(&state->handled_assignments, visit->node)) {
        core_dict_remove(&state->handled_assignments, visit->node);
        core_dict_put(&state->values, visit->node, (void *)0);
        return true;
    }
    if (ir_lower_node_value(state->context, visit->node)) return true;
    parse_annotation_t *annotation = visit->rule
        ? parse_rule_find_annot(visit->grammar, visit->rule,
                                PARSE_ANNOT_EMIT)
        : nullptr;
    if (!annotation && visit->nt)
        annotation = parse_nt_find_direct_annot(visit->nt,
                                                PARSE_ANNOT_EMIT);

    parse_annotation_t *assignment = annotation ? nullptr
        : lower_find_annotation(visit, PARSE_ANNOT_ASSIGNS);

    if (annotation && core_string_eq(annotation->emit_action,
                                      CORE_STRING_STATIC("reference"))
        && lower_tree_is_suppressed_value(state, visit->node)) {
        _core_dict_put(&state->values, visit->node, nullptr);
        return true;
    }

    if (!annotation && !assignment && state->context->reachable
        && ir_lower_node_value(state->context, visit->node))
        return true;

    if ((!annotation && !assignment) || !state->context->reachable) {
        _core_dict_put(&state->values, visit->node, nullptr);
        if (!state->context->reachable) state->terminated = true;
        return true;
    }

    if (assignment) {
        ir_lower_result_t result = lower_tree_assignment(
            state, visit, assignment, span_of_tree(state->context, visit->node));
        if (result.status == IR_LOWER_FAILED) {
            state->failed = true;
            parse_tree_walk_stop(visit->walk);
            return false;
        }
        _core_dict_put(&state->values, visit->node, result.value);
        return true;
    }

    size_t count = core_list_len(annotation->emit_operands);
    ir_value_t **operands = count
        ? core_alloc_array(ir_value_t *, count) : nullptr;
    for (size_t i = 0; i < count; i++) {
        parse_tree_t *child = parse_tree_resolve_child_ref(
            visit->grammar, visit->node, annotation->emit_operands.data[i]);
        bool found = false;
        operands[i] = child
            ? core_dict_get(&state->values, child, &found) : nullptr;
        if (!found) {
            core_free(operands);
            lower_failure(state->context, IR_DIAG_VERIFY_PARSED,
                          span_of_tree(state->context, visit->node),
                          "emit annotation refers to a child with no lowered value");
            state->failed = true;
            parse_tree_walk_stop(visit->walk);
            return false;
        }
    }

    ir_lower_result_t result = ir_lower_emit(
        state->lowerer, state->context, annotation, visit, operands, count,
        visit->pn ? visit->pn->type : nullptr,
        span_of_tree(state->context, visit->node));
    core_free(operands);
    if (result.status == IR_LOWER_FAILED) {
        state->failed = true;
        parse_tree_walk_stop(visit->walk);
        return false;
    }
    state->terminated |= result.status == IR_LOWER_TERMINATED;
    core_dict_put(&state->values, visit->node, result.value);
    return true;
}

static bool
lower_rule_has_named_child(parse_grammar_t *grammar, const parse_rule_t *rule,
                           int64_t wanted, bool *visited)
{
    for (size_t i = 0; rule && i < core_list_len(rule->contents); i++) {
        const parse_match_t *match = &rule->contents.data[i];
        if (match->kind == PARSE_MATCH_NT && match->nt_id == wanted)
            return true;
        if (match->kind != PARSE_MATCH_GROUP) continue;

        parse_rule_group_t *group = match->group;
        if (!group || group->contents_id < 0
            || (size_t)group->contents_id >= core_list_len(grammar->nt_list)
            || visited[group->contents_id])
            continue;
        visited[group->contents_id] = true;
        parse_nonterm_t *group_nt = parse_get_nonterm(
            grammar, group->contents_id);
        for (size_t j = 0; group_nt && j < core_list_len(group_nt->rule_ids);
             j++) {
            if (lower_rule_has_named_child(grammar,
                    parse_get_rule(grammar, group_nt->rule_ids.data[j]),
                    wanted, visited))
                return true;
        }
    }
    return false;
}

static bool
lower_ref_valid_for_rule(parse_grammar_t *grammar, const parse_rule_t *rule,
                         parse_child_ref_t ref)
{
    if (!parse_child_ref_is_set(ref)) return false;
    if (ref.kind == PARSE_ROLE_BY_INDEX) {
        size_t child_count = 0;
        for (size_t i = 0; rule && i < core_list_len(rule->contents); i++)
            child_count += rule->contents.data[i].kind != PARSE_MATCH_EMPTY;
        return (size_t)ref.index < child_count;
    }
    if (ref.kind != PARSE_ROLE_BY_NAME) return false;

    parse_nonterm_t *target = parse_find_nonterm(grammar, ref.name);
    if (!target) return false;
    size_t count = core_list_len(grammar->nt_list);
    bool *visited = core_alloc_array(bool, count);
    memset(visited, 0, count * sizeof(*visited));
    bool found = lower_rule_has_named_child(grammar, rule, target->id,
                                            visited);
    core_free(visited);
    return found;
}

static bool
lower_has_action(const ir_lowerer_t *lowerer, const char *action)
{
    for (size_t i = 0; i < lowerer->action_count; i++)
        if (!strcmp(lowerer->actions[i].name, action)) return true;
    return false;
}

static bool
lower_validate_emit(ir_lowerer_t *lowerer, ir_lower_context_t *context,
                    parse_grammar_t *grammar, const parse_rule_t *rule,
                    const parse_annotation_t *annotation)
{
    if (!annotation || annotation->kind != PARSE_ANNOT_EMIT) return true;
    const char *action = annotation->emit_action.data;
    size_t operand_count = core_list_len(annotation->emit_operands);
    for (size_t i = 0; i < operand_count; i++) {
        if (!lower_ref_valid_for_rule(grammar, rule,
                                      annotation->emit_operands.data[i])) {
            lower_failure(context, IR_DIAG_VERIFY_PARSED, IR_SPAN_UNKNOWN,
                          "emit annotation has a child reference that does not exist in its production");
            return false;
        }
    }
    if (!action) {
        lower_failure(context, IR_DIAG_VERIFY_PARSED, IR_SPAN_UNKNOWN,
                      "emit annotation has no action name");
        return false;
    }

    if (lower_has_action(lowerer, action)) return true;
    if (!strcmp(action, "identity")) {
        if (operand_count == 1) return true;
    }
    else if (!strcmp(action, "reference") || !strcmp(action, "literal")) {
        if (!operand_count) return true;
    }
    else {
        ir_op_t op;
        if (ir_op_lookup(action, &op)
            && operand_count <= UINT32_MAX
            && ir_op_accepts_operands(op, (uint32_t)operand_count))
            return true;
    }

    lower_failure(context, IR_DIAG_VERIFY_PARSED, IR_SPAN_UNKNOWN,
                  "emit annotation has an unknown action or invalid operand count");
    return false;
}

static bool
lower_validate_grammar(ir_lowerer_t *lowerer, ir_lower_context_t *context,
                       parse_grammar_t *grammar)
{
    parse_grammar_finalize(grammar);
    for (size_t i = 0; i < core_list_len(grammar->rules); i++) {
        parse_rule_t *rule = &grammar->rules.data[i];
        for (size_t j = 0; j < parse_rule_annot_count(rule); j++) {
            parse_annotation_t *annotation = parse_rule_annot_at(rule, j);
            if (!lower_validate_emit(lowerer, context, grammar, rule,
                                     annotation))
                return false;
        }
    }

    for (size_t i = 0; i < core_list_len(grammar->nt_list); i++) {
        parse_nonterm_t *nt = grammar->nt_list.data[i];
        for (size_t j = 0; j < parse_nt_annot_count(nt); j++) {
            parse_annotation_t *annotation = parse_nt_annot_at(nt, j);
            if (!annotation || annotation->kind != PARSE_ANNOT_EMIT) continue;
            for (size_t k = 0; k < core_list_len(nt->rule_ids); k++) {
                parse_rule_t *rule = parse_get_rule(grammar,
                                                     nt->rule_ids.data[k]);
                if (!lower_validate_emit(lowerer, context, grammar, rule,
                                         annotation))
                    return false;
            }
        }
    }
    return true;
}

ir_lower_result_t
ir_lower_tree(ir_lowerer_t *lowerer, ir_lower_context_t *context,
              parse_grammar_t *grammar, parse_tree_t *root)
{
    if (!lowerer || !context || !grammar || !root)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (!lower_validate_grammar(lowerer, context, grammar))
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    ir_lower_tree_state_t state = {
        .lowerer = lowerer,
        .context = context,
    };
    void *outer_tree_values = context->tree_values;
    void *outer_tree_state = context->tree_state;
    core_dict_init(&state.values, nullptr, nullptr);
    core_dict_init(&state.pruned_nodes, nullptr, nullptr);
    core_dict_init(&state.custom_assignments, nullptr, nullptr);
    core_dict_init(&state.handled_assignments, nullptr, nullptr);
    context->tree_values = &state.values;
    context->tree_state = &state;
    parse_tree_walk_t *walk = parse_tree_walk_new(grammar, &state);
    parse_walk_on_node(walk, lower_tree_enter, lower_tree_node, nullptr);
    parse_tree_t *parent = nullptr;
    int32_t child_index = -1;
    int32_t depth = 0;
    if (!core_tree_is_leaf(root)) {
        parent = core_tree_node_value(root).parent;
        if (parent && !core_tree_is_leaf(parent)) {
            for (size_t i = 0; i < core_tree_num_children(parent); i++) {
                if (core_tree_child(parent, i) == root) {
                    child_index = (int32_t)i;
                    break;
                }
            }
            if (child_index < 0) parent = nullptr;
            for (parse_tree_t *at = parent; at;
                 at = core_tree_is_leaf(at)
                     ? nullptr : core_tree_node_value(at).parent)
                depth++;
        }
    }
    parse_tree_walk_run_with_parent(walk, root, parent, depth, child_index);
    bool found = false;
    ir_value_t *value = core_dict_get(&state.values, root, &found);
    parse_tree_walk_free(walk);
    context->tree_values = outer_tree_values;
    context->tree_state = outer_tree_state;
    core_dict_free(&state.values);
    core_dict_free(&state.pruned_nodes);
    core_dict_free(&state.custom_assignments);
    core_dict_free(&state.handled_assignments);
    while (state.skip_values) {
        ir_lower_skip_frame_t *target = state.skip_values;
        state.skip_values = target->next;
        core_free(target);
    }
    if (state.failed || !found)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (state.terminated || !context->reachable)
        return (ir_lower_result_t){.status = IR_LOWER_TERMINATED};
    return value ? (ir_lower_result_t){IR_LOWER_VALUE, value}
                 : (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}
