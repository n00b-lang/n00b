#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lower_internal.h"
#include "walk.h"
#include "ir/typelang.h"

#define lower_failure(context, code, span, literal) \
    ((context) && ir_diags_failed((context)->diagnostics) \
        ? (ir_lower_result_t){.status = IR_LOWER_FAILED} \
        : ir_lower_fail((context), (code), (span), CORE_STRING_STATIC(literal)))
#include "ir/typelang.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "parse/symtab.h"
#include "parse/tree_util.h"

typedef struct {
    core_string_t name;
    ir_lower_action_fn callback;
    void *state;
} ir_lower_action_entry_t;

typedef struct {
    parse_annot_kind_t kind;
    ir_lower_visit_fn callback;
    void *state;
} ir_lower_visit_entry_t;

typedef struct {
    core_string_t nonterminal;
    ir_lower_visit_fn callback;
    void *state;
} ir_lower_nt_visit_entry_t;

typedef struct ir_lower_symbol_binding_t {
    parse_sym_entry_t *symbol;
    ir_value_t *value;
    ir_local_t *local;
    ir_value_t *address;
    struct ir_lower_symbol_binding_t *previous_symbol;
    struct ir_lower_symbol_binding_t *previous_local;
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
    core_dict_t suppressed_emits;
    ir_lower_skip_frame_t *skip_values;
    parse_tree_t *active_node;
    bool active_entering;
    bool failed;
    bool terminated;
} ir_lower_tree_state_t;

typedef struct {
    ir_lower_symbol_binding_t *bindings;
    core_dict_t by_symbol;
    core_dict_t by_local;
    core_dict_t active_bindings;
    ir_lower_tree_state_t *tree;
} ir_lower_private_t;

static void
lower_private_cleanup(void *raw)
{
    ir_lower_private_t *state = raw;
    core_dict_free(&state->by_symbol);
    core_dict_free(&state->by_local);
    core_dict_free(&state->active_bindings);
}

static n00bb_lower_context_t *
lower_owner(ir_lower_context_t *context)
{
    return context ? context->user : nullptr;
}

void
ir_lower_context_release(ir_lower_context_t *context)
{
    n00bb_lower_context_t *owner = lower_owner(context);
    if (!owner || !owner->walk) return;
    lower_private_cleanup(owner->walk);
    owner->walk = nullptr;
}

static ir_lower_private_t *
lower_private(ir_lower_context_t *context)
{
    n00bb_lower_context_t *owner = lower_owner(context);
    return owner ? owner->walk : nullptr;
}

static ir_lower_private_t *
lower_ensure_private(ir_lower_context_t *context)
{
    ir_lower_private_t *state = lower_private(context);
    n00bb_lower_context_t *owner = lower_owner(context);
    if (state || !owner || !context->module) return state;
    ir_ctx_t *ctx = ir_module_ctx(context->module);
    state = ir_alloc(ctx, sizeof(*state));
    core_dict_init(&state->by_symbol, nullptr, nullptr);
    core_dict_init(&state->by_local, nullptr, nullptr);
    core_dict_init(&state->active_bindings, nullptr, nullptr);
    ir_ctx_on_free(ctx, lower_private_cleanup, state);
    owner->walk = state;
    return state;
}

static ir_lower_tree_state_t *
lower_active_tree(ir_lower_context_t *context)
{
    ir_lower_private_t *state = lower_private(context);
    return state ? state->tree : nullptr;
}

static parse_tree_t *lower_tree_simple_lvalue(parse_tree_t *node);
static ir_span_t span_of_tree(ir_lower_context_t *context, parse_tree_t *node);
static ir_lower_symbol_binding_t *lower_find_binding(
    ir_lower_context_t *context, parse_sym_entry_t *symbol);

static bool
lower_set_int_annot(ir_ctx_t *ctx, ir_annots_t *annots, core_string_t key,
                    uint64_t value)
{
    return ir_annot_set(ctx, annots, (ir_annot_t){
        .key = key,
        .kind = IR_ANNOT_INT,
        .i = {.mag = value},
    });
}

static bool
lower_set_bool_annot(ir_ctx_t *ctx, ir_annots_t *annots, core_string_t key,
                     bool value)
{
    return ir_annot_set(ctx, annots, (ir_annot_t){
        .key = key,
        .kind = IR_ANNOT_BOOL,
        .b = value,
    });
}

static bool
lower_set_string_annot(ir_ctx_t *ctx, ir_annots_t *annots, core_string_t key,
                       core_string_t value)
{
    return !value.data || ir_annot_set(ctx, annots, (ir_annot_t){
        .key = key,
        .kind = IR_ANNOT_STR,
        .s = ir_strdup(ctx, value),
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
lower_project_symbol_facts(ir_lower_context_t *context,
                           ir_lower_symbol_binding_t *binding)
{
    parse_sym_entry_t *symbol = binding->symbol;
    ir_value_t *value = binding->address
        ? binding->address : lower_binding_value(context, binding);
    ir_annots_t *annots = ir_value_annots(value);
    if (!annots) return false;

    ir_ctx_t *ctx = ir_module_ctx(context->module);
    if (!lower_set_string_annot(ctx, annots,
                                   CORE_STRING_STATIC("ir.storage_requested"),
                                   symbol->requested_storage_class)
        || !lower_set_string_annot(ctx, annots,
                                   CORE_STRING_STATIC("ir.storage_selected"),
                                   symbol->selected_storage_class)
        || !lower_set_bool_annot(ctx, annots,
                                 CORE_STRING_STATIC("ir.storage_required"),
                                 symbol->storage_class_required)
        || !lower_set_int_annot(ctx, annots,
                                CORE_STRING_STATIC("ir.storage_duration"),
                                symbol->storage_duration)) return false;
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
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !node) return nullptr;
    bool found = false;
    ir_value_t *value = core_dict_get(&state->values, node, &found);
    return found ? value : nullptr;
}

static bool lower_value_accessible(ir_lower_context_t *context,
                                   ir_value_t *value);

bool
ir_lower_set_node_value(ir_lower_context_t *context, parse_tree_t *node,
                        ir_value_t *value)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !context->function || !node
        || !lower_value_accessible(context, value))
        return false;
    core_dict_put(&state->values, node, value);
    return true;
}

bool
ir_lower_skip_value(ir_lower_context_t *context, parse_tree_t *owner,
                    parse_tree_t *node)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !owner || !node) return false;
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
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !assignment) return false;
    if (state->active_node != assignment || !state->active_entering)
        return false;
    core_dict_put(&state->custom_assignments, assignment, (void *)1);
    core_dict_put(&state->handled_assignments, assignment, (void *)1);
    return true;
}

bool
ir_lower_finish_assignment(ir_lower_context_t *context,
                           parse_tree_t *assignment,
                           parse_tree_t *completed_node)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !assignment || !completed_node) return false;
    for (parse_tree_t *node = completed_node; node;
         node = parse_tree_parent(node)) {
        if (node != assignment) continue;
        core_dict_put(&state->handled_assignments, assignment, (void *)1);
        return true;
    }
    return false;
}

bool
ir_lower_evaluate_assignment_target(ir_lower_context_t *context,
                                    parse_tree_t *assignment)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !assignment) return false;
    if (state->active_node != assignment || !state->active_entering)
        return false;
    core_dict_put(&state->custom_assignments, assignment, (void *)1);
    return true;
}

bool
ir_lower_prune_node(ir_lower_context_t *context, parse_tree_t *node)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !node) return false;
    if (state->active_node != node) return false;
    core_dict_put(&state->pruned_nodes, node, (void *)1);
    return true;
}

bool
ir_lower_suppress_emit(ir_lower_context_t *context, parse_tree_t *node)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    if (!state || !node) return false;
    core_dict_put(&state->suppressed_emits, node, (void *)1);
    return true;
}

bool
ir_lower_emit_suppressed(ir_lower_context_t *context, parse_tree_t *node)
{
    ir_lower_tree_state_t *state = lower_active_tree(context);
    return state && node
        && core_dict_contains(&state->suppressed_emits, node);
}

static bool
lower_bind_symbol(ir_lower_context_t *context, parse_sym_entry_t *symbol,
                  ir_value_t *value, ir_local_t *local,
                  ir_value_t *address, bool scoped)
{
    if (!context || !context->module || !context->function || !symbol
        || (!value && !local) || (address && !local))
        return false;
    ir_lower_private_t *private = lower_ensure_private(context);
    if (!private || (!scoped
        && core_dict_contains(&private->by_symbol, symbol))) return false;
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
                    || symbol->kind == PARSE_SYM_PARAM)
        && !address && !local && !value)
        return false;
    ir_lower_symbol_binding_t *binding =
        ir_alloc(ir_module_ctx(context->module), sizeof(*binding));
    *binding = (ir_lower_symbol_binding_t){
        .symbol = symbol,
        .value = value,
        .local = local,
        .address = address,
        .previous_symbol = core_dict_get(&private->by_symbol, symbol, nullptr),
        .previous_local = local
            ? core_dict_get(&private->by_local, local, nullptr) : nullptr,
        .next = private->bindings,
    };
    private->bindings = binding;
    core_dict_put(&private->by_symbol, symbol, binding);
    if (local) core_dict_put(&private->by_local, local, binding);
    core_dict_put(&private->active_bindings, binding, (void *)1);
    if (scoped) return true;
    return lower_project_symbol_facts(context, binding);
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

static void
lower_reindex_local(ir_lower_private_t *private, ir_local_t *local)
{
    if (!local) return;
    ir_lower_symbol_binding_t *head = nullptr;
    ir_lower_symbol_binding_t *previous = nullptr;
    for (ir_lower_symbol_binding_t *at = private->bindings; at; at = at->next) {
        if (at->local != local) continue;
        if (!head) head = at;
        if (previous) previous->previous_local = at;
        previous = at;
    }
    if (previous) previous->previous_local = nullptr;
    if (head) core_dict_put(&private->by_local, local, head);
    else core_dict_remove(&private->by_local, local);
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
    ir_local_t *old_local = binding->local;
    binding->value = nullptr;
    binding->local = local;
    binding->address = nullptr;
    if (old_local != local) {
        ir_lower_private_t *private = lower_private(context);
        lower_reindex_local(private, old_local);
        lower_reindex_local(private, local);
    }
    return lower_project_symbol_facts(context, binding);
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
    ir_lower_private_t *private = lower_private(context);
    return private ? private->bindings : nullptr;
}

bool
ir_lower_bindings_restore(ir_lower_context_t *context, void *checkpoint)
{
    if (!context) return false;
    ir_lower_private_t *private = lower_private(context);
    ir_lower_symbol_binding_t *mark = checkpoint;
    if (!private) return !mark;
    if (mark && !core_dict_contains(&private->active_bindings, mark))
        return false;
    while (private->bindings != mark) {
        ir_lower_symbol_binding_t *binding = private->bindings;
        if (binding->previous_symbol)
            core_dict_put(&private->by_symbol, binding->symbol,
                          binding->previous_symbol);
        else core_dict_remove(&private->by_symbol, binding->symbol);
        if (binding->local) {
            if (binding->previous_local)
                core_dict_put(&private->by_local, binding->local,
                              binding->previous_local);
            else core_dict_remove(&private->by_local, binding->local);
        }
        core_dict_remove(&private->active_bindings, binding);
        private->bindings = binding->next;
    }
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
    ir_lower_private_t *private = lower_private(context);
    ir_lower_symbol_binding_t *binding = private
        ? core_dict_get(&private->by_local, local, nullptr) : nullptr;
    return binding ? binding->address : nullptr;
}

ir_value_t *
ir_lower_bound_value(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    if (!context || !symbol) return nullptr;
    return lower_binding_value(context, lower_find_binding(context, symbol));
}

bool
ir_lowerer_register(ir_lowerer_t *lowerer, core_string_t action,
                    ir_lower_action_fn callback, void *state)
{
    if (!lowerer || !action.data || !action.u8_bytes || !callback)
        return false;
    for (size_t i = 0; i < lowerer->action_count; i++) {
        if (core_string_eq(lowerer->actions[i].name, action)) return false;
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
ir_lowerer_register_nt_visit(ir_lowerer_t *lowerer, core_string_t nonterminal,
                             ir_lower_visit_fn callback, void *state)
{
    if (!lowerer || !nonterminal.data || !nonterminal.u8_bytes || !callback)
        return false;
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














static ir_lower_symbol_binding_t *
lower_find_binding(ir_lower_context_t *context, parse_sym_entry_t *symbol)
{
    ir_lower_private_t *private = lower_private(context);
    n00bb_lower_context_t *owner = lower_owner(context);
    bool bind_external = owner && owner->bind_external;
    if (!private && bind_external)
        private = lower_ensure_private(context);
    if (!private) return nullptr;
    for (parse_sym_entry_t *view = symbol; view;
         view = parse_sym_binding_origin(view)) {
        ir_lower_symbol_binding_t *binding = core_dict_get(
            &private->by_symbol, view, nullptr);
        if (binding) return binding;
        if (bind_external && bind_missing_external(context, view)) {
            binding = core_dict_get(&private->by_symbol, view, nullptr);
            if (binding) return binding;
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
        && value->owner->owner
        && ir_module_ctx(value->owner->owner) == ir_module_ctx(context->module);
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




static ir_lower_result_t
lower_builtin_action(ir_lower_context_t *context, const n00bb_emit_t *emit,
                     parse_visit_t *visit, size_t operand_count,
                     ir_value_t *const *operands, th_type *result_type,
                     ir_span_t span)
{
    core_string_t action = emit->action;
    if (core_string_eq(action, CORE_STRING_STATIC("identity"))) {
        if (operand_count == 1 && operands && operands[0])
            return (ir_lower_result_t){IR_LOWER_VALUE, operands[0]};
        if (operand_count == 1 && operands && !operands[0])
            return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "identity lowering requires one child value");
    }
    if (core_string_eq(action, CORE_STRING_STATIC("reference"))) {
        parse_node_resolution_t resolution = visit && visit->node
            ? parse_tree_resolution(visit->node)
            : (parse_node_resolution_t){0};
        if (resolution.kind == PARSE_NODE_RESOLUTION_SCOPE)
            return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
        parse_sym_entry_t *symbol = resolution.kind
                == PARSE_NODE_RESOLUTION_SYMBOL
            ? resolution.target.symbol
            : visit ? parse_tree_sym(visit->node) : nullptr;
        __int128 folded = 0;
        if (visit && n00bb_untyped_integer_const(visit->grammar, symbol,
                                                 &folded))
            return n00bb_emit_untyped_const(context, folded, span);
        parse_sym_entry_t *named = symbol ? canonical_symbol(symbol) : nullptr;
        if (named && named->kind == PARSE_SYM_FUNCTION) {
            lower_report(context, N00B_DIAG_FUNCTION_NAME_AS_VALUE, span,
                         N00B_TEXT_FUNCTION_NAME_AS_VALUE,
                         N00BB_ARGS(ARG_TEXT(named->name)));
            lower_help(context, N00B_TEXT_WRITE_REF_FUNCTION,
                       N00BB_ARGS(ARG_TEXT(named->name)));
            return (ir_lower_result_t){.status = IR_LOWER_FAILED};
        }
        if (symbol && symbol->kind == PARSE_SYM_ENUM_CONST
            && symbol->owner_type) {
            ir_value_t *member = n00bb_emit_enum_member(context, symbol, span);
            return member ? (ir_lower_result_t){IR_LOWER_VALUE, member}
                : lower_internal(context, span,
                                 "could not emit an enum or flagset member");
        }
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
                ? n00bb_emit_indirect_read(context, binding->address,
                                           binding->local->type, span)
                : n00bb_emit_local_read(context, binding->local, span);
        if (!value)
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit a local read");
        th_type *option_item = result_type && value->type
            ? th_option_item(th_resolve(value->type)) : nullptr;
        if (option_item && ir_type_eq(context->module, option_item,
                                      result_type)) {
            ir_instr_t *narrowed = ir_lower_emit_instruction(context,
                IR_OP_OPTION_GET, span);
            if (!narrowed
                || !ir_lower_set_temp_dest(context, narrowed,
                       CORE_STRING_STATIC("__narrowed_"), result_type)
                || !ir_instr_operand_value(narrowed, value))
                return lower_failure(context, IR_DIAG_INTERNAL, span,
                                     "could not read a narrowed option");
            value = narrowed->dest;
        }
        return (ir_lower_result_t){IR_LOWER_VALUE, value};
    }
    if (core_string_eq(action, CORE_STRING_STATIC("literal"))) {
        if (operand_count || !result_type)
            return lower_failure(context, IR_DIAG_VERIFY_TYPED, span,
                                 "literal lowering needs a result type and no child values");
        core_string_t kind = emit->literal;
        parse_token_info_t *token = parse_tree_find_first_terminal(
            visit ? visit->node : nullptr);
        core_option_t(core_string_t) token_value = token
            ? parse_token_value(token) : core_option_none(core_string_t);
        if (!kind.data || !core_option_is_set(token_value))
            return lower_failure(context, IR_DIAG_TYPE_LITERAL, span,
                                 "literal metadata or token value is missing");
        core_string_t text = core_option_get(token_value);
        if (core_string_eq(kind, CORE_STRING_STATIC("bool")))
            result_type = th_name(ir_module_types(context->module),
                                  CORE_STRING_STATIC("bool"));
        ir_instr_t *instruction = ir_lower_emit_instruction(
            context, IR_OP_CONST, span);
        if (!instruction) return (ir_lower_result_t){.status = IR_LOWER_FAILED};
        if (!ir_lower_set_temp_dest(context, instruction,
                                    CORE_STRING_STATIC("__lowered_"),
                                    result_type))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit a literal value");
        bool emitted = false;
        if (core_string_eq(kind, CORE_STRING_STATIC("bool"))) {
            if (core_string_eq(text, CORE_STRING_STATIC("true")))
                emitted = ir_instr_operand_int(instruction, 1);
            else if (core_string_eq(text, CORE_STRING_STATIC("false")))
                emitted = ir_instr_operand_int(instruction, 0);
        }
        else if (core_string_eq(kind, CORE_STRING_STATIC("int"))) {
            ir_lit_t value;
            if (n00bb_parse_switch_integer(text, &value))
                emitted = ir_instr_operand_int_lit(instruction, value);
        }
        else if (core_string_eq(kind, CORE_STRING_STATIC("f64"))) {
            char *digits = core_alloc_array(char, text.u8_bytes + 1);
            size_t length = 0;
            for (size_t i = 0; i < text.u8_bytes; i++)
                if (text.data[i] != '_') digits[length++] = text.data[i];
            double value = 0.0;
            size_t consumed = 0;
            bool parsed =
                core_string_parse_f64(core_string_borrow(digits, length),
                                      &value, &consumed)
                && consumed == length;
            emitted = parsed && ir_instr_operand_float(instruction, value);
            core_free(digits);
        }
        if (!emitted)
            return lower_failure(context, IR_DIAG_TYPE_LITERAL, span,
                                 "literal spelling is not supported by generic scalar lowering");
        return (ir_lower_result_t){IR_LOWER_VALUE, instruction->dest};
    }
    return (ir_lower_result_t){.status = IR_LOWER_FAILED};
}

static ir_lower_result_t
ir_lower_emit(ir_lowerer_t *lowerer, ir_lower_context_t *context,
              const n00bb_emit_t *emit, parse_visit_t *visit,
              ir_value_t *const *operands, size_t operand_count,
              th_type *result_type, ir_span_t span)
{
    if (!lowerer || !context || !emit || !emit->action.data
        || !context->module || !context->function || !context->block
        || !context->reachable)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    if (context->function->owner != context->module
        || context->block->owner != context->function)
        return lower_failure(context, IR_DIAG_INTERNAL, span,
                             "lowering context objects do not belong together");

    core_string_t action = emit->action;
    for (size_t i = 0; i < lowerer->action_count; i++) {
        ir_lower_action_entry_t *entry = &lowerer->actions[i];
        if (!core_string_eq(entry->name, action)) continue;
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

    return lower_builtin_action(context, emit, visit, operand_count,
                                operands, result_type, span);
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
lower_tree_run_visit(parse_visit_t *visit, bool entering, bool nonterminal)
{
    ir_lower_tree_state_t *state = visit->user;
    ir_lower_visit_fn callback;
    void *callback_state;
    if (nonterminal) {
        ir_lower_nt_visit_entry_t *entry = visit->thunk;
        callback = entry->callback;
        callback_state = entry->state;
    }
    else {
        ir_lower_visit_entry_t *entry = visit->thunk;
        callback = entry->callback;
        callback_state = entry->state;
    }
    parse_tree_t *previous_node = state->active_node;
    bool previous_entering = state->active_entering;
    state->active_node = visit->node;
    state->active_entering = entering;
    bool ok = callback(state->context, visit, entering, callback_state);
    state->active_node = previous_node;
    state->active_entering = previous_entering;
    if (!ok) {
        state->failed = true;
        parse_tree_walk_stop(visit->walk);
        return false;
    }
    if (entering && core_dict_contains(&state->pruned_nodes, visit->node)) {
        for (parse_annot_kind_t kind = 0; kind < PARSE_ANNOT_KIND_COUNT;
             kind++)
            parse_visit_suppress_annotation(visit, kind);
        return false;
    }
    return true;
}

static bool lower_tree_annot_pre(parse_visit_t *visit)
{
    return lower_tree_run_visit(visit, true, false);
}

static bool lower_tree_annot_post(parse_visit_t *visit)
{
    return lower_tree_run_visit(visit, false, false);
}

static bool lower_tree_nt_pre(parse_visit_t *visit)
{
    return lower_tree_run_visit(visit, true, true);
}

static bool lower_tree_nt_post(parse_visit_t *visit)
{
    return lower_tree_run_visit(visit, false, true);
}

static parse_tree_t *
lower_only_nonterminal_child(parse_tree_t *node)
{
    parse_tree_t *only = nullptr;
    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *child = core_tree_child(node, i);
        if (core_tree_is_leaf(child)) continue;
        if (only) return nullptr;
        only = child;
    }
    return only;
}

static parse_tree_t *
lower_discarded_expression(parse_grammar_t *grammar, parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        n00bb_emit_t emit;
        bool emits = n00bb_node_emit(grammar, node, &emit);
        parse_tree_t *inner = nullptr;
        if (emits && core_string_eq(emit.action,
                                    CORE_STRING_STATIC("identity")))
            inner = core_tree_child(node, emit.operands[0]);
        else if (!emits)
            inner = lower_only_nonterminal_child(node);
        if (!inner || core_tree_is_leaf(inner)) return node;
        node = inner;
    }
    return node;
}

static bool lower_literal_handler_call(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        if (n00bb_node_literal_site(node)) {
            return true;
        }
        if (core_tree_num_children(node) != 1) {
            return false;
        }
        node = core_tree_child(node, 0);
    }
    return false;
}

static bool lower_tree_discard_post(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    if (state->failed || !state->context->reachable
        || !state->context->diagnostics)
        return true;
    parse_tree_t *expression = lower_discarded_expression(visit->grammar,
        parse_tree_resolve_child_ref(visit->grammar, visit->node,
                                     visit->annot->value_ref));
    const parse_rule_t *rule = expression && !core_tree_is_leaf(expression)
        ? parse_tree_rule(visit->grammar, expression) : nullptr;
    bool found = false;
    ir_value_t *value = expression
        ? core_dict_get(&state->values, expression, &found) : nullptr;
    th_type *type = found && value ? value->type : nullptr;
    if (!expression || lower_literal_handler_call(expression)) return true;
    if (rule && parse_rule_find_annot(visit->grammar, rule,
                                      PARSE_ANNOT_CALL)) {
        ir_instr_t *call  = found && value && value->kind == IR_VAL_INSTR
                              ? value->def : nullptr;
        if (call && (call->op == IR_OP_CALL || call->op == IR_OP_DISPATCH
                     || call->op == IR_OP_CALLBACK_INVOKE))
            ir_annot_set(ir_module_ctx(state->context->module), &call->annots,
                         (ir_annot_t){
                             .key  = CORE_STRING_STATIC("ir.result_discarded"),
                             .kind = IR_ANNOT_BOOL,
                             .b    = true});
        return true;
    }
    if (type && th_is_nil(th_resolve(type))) return true;
    ir_diag_t *diagnostic = ir_diag_begin(state->context->diagnostics,
        IR_DIAG_UNUSED_RESULT, span_of_tree(state->context, expression),
        IR_NO_LABEL);
    ir_diag_message(diagnostic, state->context->diagnostics,
        IR_MESSAGE(UNUSED_RESULT));
    return true;
}

static bool lower_tree_assign_pre(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    return lower_tree_track_assignment(state, visit, visit->annot, true);
}

static bool lower_tree_assign_post(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    return lower_tree_track_assignment(state, visit, visit->annot, false);
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
    if (!first || !last || parse_token_start_byte(first) > UINT32_MAX
        || parse_token_end_byte(last) > UINT32_MAX)
        return IR_SPAN_UNKNOWN;
    return ir_span_make(context->file_id,
                        (uint32_t)parse_token_start_byte(first),
                        (uint32_t)parse_token_end_byte(last),
                        parse_token_line(first), parse_token_column(first),
                        parse_token_endline(last), parse_token_endcol(last));
}

static parse_tree_t *
lower_tree_simple_lvalue(parse_tree_t *node)
{
    while (node && !core_tree_is_leaf(node)) {
        parse_sym_entry_t *symbol = parse_tree_sym(node);
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
             ancestor = parse_tree_parent(ancestor))
            if (target->reference == ancestor) return true;
    return false;
}

static bool
inside_effect_query(parse_grammar_t *grammar, parse_tree_t *node)
{
    for (parse_tree_t *ancestor = node ? parse_tree_parent(node) : nullptr;
         ancestor; ancestor = parse_tree_parent(ancestor))
        if (core_string_eq(parse_tree_nt_name(grammar, ancestor),
                           CORE_STRING_STATIC("effect-query")))
            return true;
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


static ir_lower_result_t lower_write_binding_value(
    ir_lower_context_t *context, ir_lower_symbol_binding_t *binding,
    ir_value_t *value, bool initializes, ir_span_t span);

static ir_lower_result_t
lower_write_binding(ir_lower_context_t *context,
                    ir_lower_symbol_binding_t *binding, ir_value_t *value,
                    bool initializes, ir_span_t span)
{
    n00bb_lower_context_t *owner = lower_owner(context);
    bool track = owner && owner->track_writes;
    if (track && !n00bb_write_binding(context, binding->symbol, false))
        return lower_failure(context, IR_DIAG_INTERNAL, span,
                             "could not prepare the assigned binding");
    ir_lower_result_t result = lower_write_binding_value(context, binding,
                                                         value, initializes,
                                                         span);
    if (result.status != IR_LOWER_FAILED && track
        && !n00bb_write_binding(context, binding->symbol, true))
        return lower_failure(context, IR_DIAG_INTERNAL, span,
                             "could not record the assigned binding");
    return result;
}

static ir_lower_result_t
lower_write_binding_value(ir_lower_context_t *context,
                          ir_lower_symbol_binding_t *binding,
                          ir_value_t *value, bool initializes, ir_span_t span)
{
    if (!initializes) {
        ir_value_t *target = binding->address
            ? binding->address : lower_binding_value(context, binding);
        if (target && target->kind == IR_VAL_GLOBAL && target->global)
            initializes = target->global->mutability != IR_MUT_MUTABLE;
        else if (target && target->local)
            initializes = target->local->mutability != IR_MUT_MUTABLE;
    }
    if (binding->address) {
        if (!n00bb_emit_store(context, binding->address, value, initializes,
                              span))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit an address-backed assignment");
    }
    else if (binding->local) {
        if (!n00bb_emit_local_assign(context, binding->local, value,
                                     initializes, span))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit a local assignment");
    }
    else if (binding->value && binding->value->kind == IR_VAL_GLOBAL) {
        ir_instr_t *instruction = ir_instr_add(context->block,
                                               IR_OP_GLOBAL_STORE);
        if (!instruction || !ir_instr_operand_value(instruction,
                                                     binding->value)
            || !ir_instr_set_binding_initialization(
                instruction, initializes)
            || !ir_instr_operand_value(instruction, value))
            return lower_failure(context, IR_DIAG_INTERNAL, span,
                                 "could not emit a global assignment");
        instruction->span = span;
    }
    else {
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "assignment destination is not writable storage");
    }
    return (ir_lower_result_t){.status = IR_LOWER_NO_VALUE};
}

ir_lower_result_t
ir_lower_assign_symbol(ir_lower_context_t *context,
                       parse_sym_entry_t *symbol, ir_value_t *value,
                       bool initializes, ir_span_t span)
{
    ir_lower_symbol_binding_t *binding = context && symbol
        ? lower_find_binding(context, symbol) : nullptr;
    if (!binding || !value)
        return lower_failure(context, IR_DIAG_VERIFY_PARSED, span,
                             "assignment needs a bound destination and value");
    return lower_write_binding(context, binding, value, initializes, span);
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
        symbol = parse_tree_sym(visit->node);
    else {
        parse_tree_t *lvalue = lower_tree_simple_lvalue(target);
        symbol = lvalue ? parse_tree_sym(lvalue) : nullptr;
        if (!symbol) symbol = parse_tree_sym(visit->node);
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
    return lower_write_binding(state->context, binding, value, initializes,
                               span);
}

static ir_lower_result_t
lower_tree_recover(ir_lower_tree_state_t *state, parse_visit_t *visit,
                   ir_lower_result_t result)
{
    if (result.status != IR_LOWER_FAILED
        || !state->context->reachable
        || !core_diagnostics_failed(lower_diagnostics(state->context)))
        return result;
    return lower_placeholder(state->context,
        n00bb_node_result_type(state->context, visit->node),
        span_of_tree(state->context, visit->node));
}

static bool
lower_tree_node(parse_visit_t *visit)
{
    ir_lower_tree_state_t *state = visit->user;
    if (core_dict_contains(&state->pruned_nodes, visit->node)) {
        core_dict_remove(&state->pruned_nodes, visit->node);
        if (!ir_lower_node_value(state->context, visit->node))
            core_dict_put(&state->values, visit->node, (void *)0);
        lower_tree_clear_lvalues(state, visit->node);
        return true;
    }
    lower_tree_clear_lvalues(state, visit->node);
    if (state->failed) return false;
    if (core_dict_contains(&state->handled_assignments, visit->node)) {
        core_dict_remove(&state->handled_assignments, visit->node);
        core_dict_put(&state->values, visit->node, (void *)0);
        return true;
    }
    if (core_dict_contains(&state->suppressed_emits, visit->node)
        || inside_effect_query(visit->grammar, visit->node)) {
        core_dict_put(&state->values, visit->node, (void *)0);
        return true;
    }
    if (ir_lower_node_value(state->context, visit->node)) return true;
    n00bb_emit_t emit;
    bool emits = n00bb_node_emit(visit->grammar, visit->node, &emit);

    parse_annotation_t *assignment = emits ? nullptr
        : lower_find_annotation(visit, PARSE_ANNOT_ASSIGNS);

    if (emits && core_string_eq(emit.action, CORE_STRING_STATIC("reference"))
        && lower_tree_is_suppressed_value(state, visit->node)) {
        _core_dict_put(&state->values, visit->node, nullptr);
        return true;
    }

    if (!emits && !assignment && state->context->reachable
        && ir_lower_node_value(state->context, visit->node))
        return true;

    if ((!emits && !assignment) || !state->context->reachable) {
        _core_dict_put(&state->values, visit->node, nullptr);
        if (!state->context->reachable) state->terminated = true;
        return true;
    }

    if (assignment) {
        ir_lower_result_t result = lower_tree_recover(state, visit,
            lower_tree_assignment(state, visit, assignment,
                span_of_tree(state->context, visit->node)));
        if (result.status == IR_LOWER_FAILED) {
            state->failed = true;
            parse_tree_walk_stop(visit->walk);
            return false;
        }
        _core_dict_put(&state->values, visit->node, result.value);
        return true;
    }

    size_t count = emit.operand_count;
    ir_value_t *operands[2] = {0};
    for (size_t i = 0; i < count; i++) {
        parse_tree_t *child = core_tree_child(visit->node, emit.operands[i]);
        bool found = false;
        operands[i] = child
            ? core_dict_get(&state->values, child, &found) : nullptr;
        if (!found) {
            lower_failure(state->context, IR_DIAG_VERIFY_PARSED,
                          span_of_tree(state->context, visit->node),
                          "an operand child has no lowered value");
            state->failed = true;
            parse_tree_walk_stop(visit->walk);
            return false;
        }
    }

    ir_lower_result_t result = lower_tree_recover(state, visit, ir_lower_emit(
        state->lowerer, state->context, &emit, visit, operands, count,
        n00bb_node_result_type(state->context, visit->node),
        span_of_tree(state->context, visit->node)));
    if (result.status == IR_LOWER_FAILED) {
        state->failed = true;
        parse_tree_walk_stop(visit->walk);
        return false;
    }
    state->terminated |= result.status == IR_LOWER_TERMINATED;
    core_dict_put(&state->values, visit->node, result.value);
    return true;
}

ir_lower_result_t
ir_lower_tree(ir_lowerer_t *lowerer, ir_lower_context_t *context,
              parse_grammar_t *grammar, parse_tree_t *root)
{
    if (!lowerer || !context || !grammar || !root)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    ir_lower_private_t *private = lower_ensure_private(context);
    if (!private)
        return (ir_lower_result_t){.status = IR_LOWER_FAILED};
    ir_lower_tree_state_t state = {
        .lowerer = lowerer,
        .context = context,
    };
    ir_lower_tree_state_t *outer_tree = private->tree;
    core_dict_init(&state.values, nullptr, nullptr);
    core_dict_init(&state.pruned_nodes, nullptr, nullptr);
    core_dict_init(&state.custom_assignments, nullptr, nullptr);
    core_dict_init(&state.handled_assignments, nullptr, nullptr);
    core_dict_init(&state.suppressed_emits, nullptr, nullptr);
    private->tree = &state;
    parse_tree_walk_t *walk = parse_tree_walk_new(grammar, &state);
    parse_walk_on_annot_phase(walk, PARSE_ANNOT_ASSIGNS, 0, nullptr,
                        lower_tree_assign_post, nullptr);
    for (size_t i = 0; i < lowerer->visitor_count; i++) {
        ir_lower_visit_entry_t *entry = &lowerer->visitors[i];
        parse_walk_on_annot_phase(walk, entry->kind, 0, lower_tree_annot_pre,
                            lower_tree_annot_post, entry);
    }
    parse_walk_on_annot_phase(walk, PARSE_ANNOT_ASSIGNS, 0, lower_tree_assign_pre,
                        nullptr, nullptr);
    parse_walk_on_annot_phase(walk, PARSE_ANNOT_DISCARDS, 0, nullptr,
                        lower_tree_discard_post, nullptr);
    for (size_t i = 0; i < lowerer->nt_visitor_count; i++) {
        ir_lower_nt_visit_entry_t *entry = &lowerer->nt_visitors[i];
        if (!parse_walk_on_nt(walk, entry->nonterminal, lower_tree_nt_pre,
                lower_tree_nt_post, entry)) {
            core_buffer_t message = {0};
            core_buffer_puts(&message,
                "lowering callback names absent nonterminal '");
            core_buffer_append_string(&message, entry->nonterminal);
            core_buffer_puts(&message, "'");
            ir_lower_fail(context, IR_DIAG_VERIFY_PARSED,
                               IR_SPAN_UNKNOWN, core_buffer_string(&message));
            core_buffer_release(&message);
            state.failed = true;
            break;
        }
    }
    parse_walk_on_node(walk, nullptr, lower_tree_node, nullptr);
    parse_tree_t *parent = nullptr;
    int32_t child_index = -1;
    int32_t depth = 0;
    if (!core_tree_is_leaf(root)) {
        parent = parse_tree_parent(root);
        if (parent && !core_tree_is_leaf(parent)) {
            for (size_t i = 0; i < core_tree_num_children(parent); i++) {
                if (core_tree_child(parent, i) == root) {
                    child_index = (int32_t)i;
                    break;
                }
            }
            if (child_index < 0) parent = nullptr;
            for (parse_tree_t *at = parent; at; at = parse_tree_parent(at))
                depth++;
        }
    }
    if (!state.failed)
        parse_tree_walk_run_with_parent(walk, root, parent, depth, child_index);
    bool found = false;
    ir_value_t *value = core_dict_get(&state.values, root, &found);
    parse_tree_walk_free(walk);
    private->tree = outer_tree;
    core_dict_free(&state.values);
    core_dict_free(&state.pruned_nodes);
    core_dict_free(&state.custom_assignments);
    core_dict_free(&state.handled_assignments);
    core_dict_free(&state.suppressed_emits);
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
