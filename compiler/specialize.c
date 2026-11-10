#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "specialize.h"
#include "cached_body.h"
#include "compiler_lower_emit.h"
#include "compiler_tu.h"
#include "qualified_names.h"
#include "static_branches.h"
#include "ir/doms.h"
#include "ir/operands.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "ir/typelang.h"
#include "lib/hash.h"
#include "th/environment.h"

typedef struct {
    const th_type *variable;
    th_type *actual;
} type_binding_t;
core_list_decl(type_binding_t);

typedef struct {
    ir_func_t *source;
    ir_func_t *copy;
    th_type *signature;
    size_t parent;
    ir_func_t *site_function;
    ir_span_t site;
} specialization_t;
core_list_decl(specialization_t);

typedef struct {
    th_ctx *types;
    core_list_t(type_binding_t) *bindings;
    bool widen_integers;
} binding_sink_t;

static bool collect_binding(void *context, th_type *variable, th_type *value)
{
    binding_sink_t *sink = context;

    if (!th_is_closed(sink->types, value)) return false;
    for (size_t i = 0; i < sink->bindings->len; i++)
        if (sink->bindings->data[i].variable == variable)
            return th_hash(sink->types, sink->bindings->data[i].actual)
                    == th_hash(sink->types, value)
                || (sink->widen_integers
                    && n00bb_integer_type_promotes_to(value,
                           sink->bindings->data[i].actual));
    core_list_push(*sink->bindings, ((type_binding_t){variable, value}));
    return true;
}

static bool match_type_with(th_ctx *types, th_type *pattern, th_type *actual,
                            core_list_t(type_binding_t) *bindings,
                            bool widen_integers)
{
    size_t mark = bindings->len;
    binding_sink_t sink = {types, bindings, widen_integers};
    bool matched = pattern && actual
        && th_match_each(pattern, actual, collect_binding, &sink);
    if (!matched) bindings->len = mark;
    return matched;
}

static bool match_type(th_ctx *types, th_type *pattern, th_type *actual,
                       core_list_t(type_binding_t) *bindings)
{
    return match_type_with(types, pattern, actual, bindings, false);
}

static bool match_parameters_widening(th_ctx *types, th_type *pattern,
                                      th_type *actual,
                                      core_list_t(type_binding_t) *bindings)
{
    pattern = th_resolve(pattern);
    actual = th_resolve(actual);
    if (th_kind_of(pattern) != TH_FUNCTION || th_kind_of(actual) != TH_FUNCTION
        || th_func_nparams(actual) > th_func_nparams(pattern))
        return false;
    bindings->len = 0;
    bool matched = true;
    for (size_t i = 0; matched && i < th_func_nparams(actual); i++)
        matched = match_type_with(types, th_func_param(pattern, i)->type,
                                  th_func_param(actual, i)->type, bindings,
                                  true);
    th_type *pattern_error = th_func_error(pattern);
    th_type *actual_error = th_func_error(actual);
    return matched
        && match_type_with(types, th_func_ret(pattern), th_func_ret(actual),
                           bindings, true)
        && (!pattern_error || !actual_error
            || match_type_with(types, pattern_error, actual_error, bindings,
                               true));
}

static th_type *widened_signature(th_ctx *types, ir_func_t *target,
                                  core_list_t(type_binding_t) *bindings)
{
    const th_type **from = core_alloc_array(const th_type *, bindings->len);
    th_type **to = core_alloc_array(th_type *, bindings->len);
    th_type *signature = nullptr;
    if (from && to) {
        for (size_t i = 0; i < bindings->len; i++) {
            from[i] = bindings->data[i].variable;
            to[i] = bindings->data[i].actual;
        }
        signature = th_type_clone_replacing(types, target->type, from, to,
                                            bindings->len);
    }
    core_free(from);
    core_free(to);
    return signature && th_is_closed(types, signature) ? signature : nullptr;
}

static bool
type_strictly_contains(th_ctx *types, th_type *outer, uint64_t inner,
                       unsigned depth)
{
    outer = outer ? th_resolve(outer) : nullptr;
    if (!outer || depth > 64) return false;
    switch (th_kind_of(outer)) {
    case TH_SEQUENCE:
        return th_hash(types, th_seq_elem(outer)) == inner
            || type_strictly_contains(types, th_seq_elem(outer), inner,
                                      depth + 1);
    case TH_INDIRECTION:
        return th_hash(types, th_ind_referent(outer)) == inner
            || type_strictly_contains(types, th_ind_referent(outer), inner,
                                      depth + 1);
    case TH_NAMED:
        for (size_t i = 0; i < th_named_nargs(outer); i++) {
            const th_arg *argument = th_named_arg(outer, i);
            if (argument && argument->kind == TH_ARG_TYPE
                && (th_hash(types, argument->type) == inner
                    || type_strictly_contains(types, argument->type, inner,
                                              depth + 1))) return true;
        }
        return false;
    default:
        return false;
    }
}

static const specialization_t *
growing_instantiation(const core_list_t(specialization_t) *specializations,
                      size_t parent, ir_func_t *target, th_type *signature,
                      th_ctx *types)
{
    for (size_t i = parent; i != SIZE_MAX;
         i = specializations->data[i].parent) {
        const specialization_t *item = &specializations->data[i];
        if (item->source != target) continue;
        size_t count = th_func_nparams(signature);
        for (size_t p = 0; p < count
             && p < th_func_nparams(item->signature); p++) {
            const th_param *earlier = th_func_param(item->signature, p);
            const th_param *later = th_func_param(signature, p);
            if (earlier && later && type_strictly_contains(types,
                    later->type, th_hash(types, earlier->type), 0))
                return item;
        }
    }
    return nullptr;
}

static void
report_growing_instantiation(n00bb_specialization_failure_t *report,
                             ir_instr_t *call, ir_func_t *target,
                             const specialization_t *earlier,
                             th_type *signature, th_ctx *types)
{
    if (!report) return;
    report->function = target->name;
    report->before = n00bb_print_type(types, earlier->signature);
    report->after = n00bb_print_type(types, signature);
    report->module = ir_module_name(target->owner);
    report->span = call->span;
    report->located = true;
}

static th_type *
call_operand_type(ir_value_t *value, th_ctx *types)
{
    if (!value) return nullptr;
    if (th_is_closed(types, value->type)) return value->type;
    ir_instr_t *narrow = value->kind == IR_VAL_INSTR ? value->def : nullptr;
    if (!narrow || narrow->op != IR_OP_TYPE_NARROW
        || narrow->n_operands != 2
        || narrow->operands[0].kind != IR_OPD_VALUE
        || narrow->operands[1].kind != IR_OPD_TYPE)
        return value->type;
    ir_value_t *source = narrow->operands[0].value;
    return source && th_is_closed(types, source->type)
        && th_env_type_pattern_accepts(types, narrow->operands[1].type,
                                       source->type)
        ? source->type : value->type;
}

typedef ir_instr_t *selection_t;
core_list_decl(selection_t);

static ir_value_t *
inserted_value(ir_instr_t *before, ir_op_t op, th_type *type)
{
    ir_instr_t *instruction = ir_instr_generated(before->block, op,
        before->span, CORE_STRING_STATIC("specialize"), before->index);
    return instruction
        && ir_instr_set_fresh_dest(instruction, "__n00bb_spec", type)
        ? instruction->dest : nullptr;
}

static ir_value_t *
operand_as(ir_func_t *function, ir_instr_t *before, ir_value_t *value,
           th_type *type)
{
    ir_module_t *module = function->owner;
    if (ir_type_eq(module, value->type, type)) return value;
    ir_class_t source = ir_machine_of(module, value->type).class;
    ir_class_t target = ir_machine_of(module, type).class;
    if ((source != IR_CLASS_SINT && source != IR_CLASS_UINT)
        || (target != IR_CLASS_SINT && target != IR_CLASS_UINT))
        return nullptr;
    ir_value_t *cast = inserted_value(before, IR_OP_CAST, type);
    bool checked = !n00bb_integer_type_promotes_to(value->type, type);
    return cast && ir_instr_operand_value(cast->def, value)
        && (!checked || ir_instr_add_mod(cast->def,
                                         CORE_STRING_STATIC("trap")))
        ? cast : nullptr;
}

static bool
integer_arguments_to(ir_func_t *function, ir_instr_t *call, ir_func_t *target,
                     th_type *signature)
{
    ir_module_t *module = function->owner;
    size_t count = th_func_nparams(signature);
    if (call->n_operands != count + 1) return true;
    uint32_t position = 0;
    for (uint32_t o = 1; o < call->n_operands; o++) {
        ir_operand_t *operand = &call->operands[o];
        uint32_t index = UINT32_MAX;
        (void)ir_call_param(target, operand, &position, &index);
        const th_param *formal = index < count
            ? th_func_param(signature, index) : nullptr;
        ir_value_t *argument = ir_operand_value(operand);
        if (!formal || !argument || ir_type_eq(module, argument->type,
                                               formal->type)) continue;
        ir_class_t source = ir_machine_of(module, argument->type).class;
        ir_class_t wanted = ir_machine_of(module, formal->type).class;
        if ((source != IR_CLASS_SINT && source != IR_CLASS_UINT)
            || (wanted != IR_CLASS_SINT && wanted != IR_CLASS_UINT)) continue;
        ir_value_t *converted = operand_as(function, call, argument,
                                           formal->type);
        if (!converted) return false;
        if (operand->kind == IR_OPD_NAMED_ARG) operand->named.value = converted;
        else operand->value = converted;
        ir_instr_use_added(call, o);
    }
    return true;
}

static bool
integer_arguments_as(ir_func_t *function, ir_instr_t *call, ir_func_t *target)
{
    return integer_arguments_to(function, call, target, target->type);
}

typedef core_list_t(n00bb_ir_func_ptr_t) function_list_t;

static th_type *
concrete_call_signature(ir_instr_t *call, ir_func_t *target, th_ctx *types,
                        bool from_operands)
{
    th_type *signature = from_operands ? target->type
        : ir_instr_call_signature(call);
    if (!signature || th_is_closed(types, signature)) return signature;
    size_t count = th_func_nparams(target->type);
    if (call->n_operands != count + 1) return signature;
    core_list_t(type_binding_t) bindings = {0};
    bool matched = true;
    uint32_t position = 0;
    for (size_t i = 0; matched && i < count; i++) {
        ir_operand_t *operand = &call->operands[i + 1];
        uint32_t index = UINT32_MAX;
        ir_value_t *parameter = ir_call_param(target, operand, &position,
                                              &index);
        for (uint32_t p = 0; parameter && index == UINT32_MAX
             && p < target->n_params; p++)
            if (target->params[p] == parameter) index = p;
        if (operand->kind == IR_OPD_MISSING) continue;
        const th_param *formal = index < count
            ? th_func_param(target->type, index) : nullptr;
        ir_value_t *argument = operand->kind == IR_OPD_VALUE
            || operand->kind == IR_OPD_NAMED_ARG
            ? ir_operand_value(operand) : nullptr;
        matched = formal && argument && match_type(types, formal->type,
            call_operand_type(argument, types), &bindings);
    }
    if (matched && bindings.len) {
        const th_type **from = core_alloc_array(const th_type *,
                                                bindings.len);
        th_type **to = core_alloc_array(th_type *, bindings.len);
        matched = from && to;
        for (size_t i = 0; matched && i < bindings.len; i++) {
            from[i] = bindings.data[i].variable;
            to[i] = bindings.data[i].actual;
        }
        th_type *candidate = matched ? th_type_clone_replacing(types,
            target->type, from, to, bindings.len) : nullptr;
        if (candidate && th_is_closed(types, candidate)
            && ir_instr_set_call_signature(call, candidate))
            signature = candidate;
        core_free(from);
        core_free(to);
    }
    core_list_free(bindings);
    return signature;
}

static ir_func_t *
instantiate(ir_ctx_t *context, ir_func_t *target, th_type *actual,
            core_list_t(type_binding_t) *bindings, th_ctx *types,
            core_buffer_t *failure)
{
    core_string_t owner = ir_module_name(target->owner);
    core_buffer_t identity = {0};
    core_buffer_printf(&identity, "%.*s.%.*s:%016" PRIx64,
                       (int)owner.u8_bytes, owner.data,
                       (int)target->name.u8_bytes, target->name.data,
                       th_hash(types, actual));
    core_buffer_t name = {0};
    if (identity.data)
        core_buffer_printf(&name, "%.*s$spec$%016" PRIx64,
            (int)owner.u8_bytes, owner.data,
            core_xxh3_64bits_raw(identity.data, identity.byte_len));
    core_buffer_release(&identity);
    ir_module_t *module = name.data ? ir_module_new(context,
        core_buffer_string(&name),
        ir_module_profile(target->owner), ir_module_target(target->owner))
        : nullptr;
    core_buffer_release(&name);
    const th_type **from = core_alloc_array(const th_type *, bindings->len);
    th_type **to = core_alloc_array(th_type *, bindings->len);
    core_string_t clone_failure = {};
    ir_func_t *copy = nullptr;
    for (int32_t file = 0; module; file++) {
        core_string_t path = ir_file_path(target->owner, file);
        if (!path.data) break;
        (void)ir_file_add(module, path);
    }
    if (module && from && to) {
        for (size_t n = 0; n < bindings->len; n++) {
            from[n] = bindings->data[n].variable;
            to[n] = bindings->data[n].actual;
        }
        copy = ir_func_clone_into(target, module, target->name, from, to,
                                  bindings->len, &clone_failure);
    }
    core_free(from);
    core_free(to);
    if (!copy && failure) {
        core_string_t printed = th_print(types, actual);
        core_string_t reason = clone_failure.data ? clone_failure
            : CORE_STRING_STATIC("allocation failed");
        core_buffer_printf(failure,
            "could not clone %.*s.%.*s for concrete signature %.*s: %.*s",
            (int)owner.u8_bytes, owner.data,
            (int)target->name.u8_bytes, target->name.data,
            (int)printed.u8_bytes, printed.data,
            (int)reason.u8_bytes, reason.data);
    }
    if (copy) n00bb_fold_static_branches(copy, target, context);
    return copy;
}

typedef struct {
    core_list_t(specialization_t) list;
    core_dict_t                   by_copy;
    core_dict_t                   by_signature;
} specializations_t;

static uintptr_t
signature_key(const ir_func_t *source, uint64_t hash)
{
    return ((uintptr_t)source * UINT64_C(0x9e3779b97f4a7c15)
            ^ (uintptr_t)hash) | 1;
}

static void
add_specialization(specializations_t *specializations,
                   specialization_t specialization)
{
    core_list_push(specializations->list, specialization);
    size_t slot = specializations->list.len;
    core_dict_add(&specializations->by_copy, specialization.copy, slot);
    if (!specialization.source) return;
    th_ctx *types = ir_module_types(specialization.source->owner);
    core_dict_add(&specializations->by_signature,
                  signature_key(specialization.source,
                                th_hash(types, specialization.signature)),
                  slot);
}

static void
specializations_free(specializations_t *specializations)
{
    core_list_free(specializations->list);
    core_dict_free(&specializations->by_copy);
    core_dict_free(&specializations->by_signature);
}

static ir_func_t *
find_instance(specializations_t *specializations, ir_func_t *target,
              uint64_t hash, th_ctx *types)
{
    size_t slot = (uintptr_t)core_dict_get(&specializations->by_signature,
        signature_key(target, hash), nullptr);
    if (!slot) return nullptr;
    const specialization_t *first = &specializations->list.data[slot - 1];
    if (first->source == target
        && th_hash(types, first->signature) == hash) return first->copy;
    for (size_t n = 0; n < specializations->list.len; n++)
        if (specializations->list.data[n].source == target
            && th_hash(types, specializations->list.data[n].signature)
                == hash)
            return specializations->list.data[n].copy;
    return nullptr;
}

static size_t
instance_slot(specializations_t *specializations, const ir_func_t *function)
{
    return (uintptr_t)core_dict_get(&specializations->by_copy, function,
                                    nullptr);
}

static const specialization_t *
instance_of(specializations_t *specializations, const ir_func_t *function)
{
    size_t slot = instance_slot(specializations, function);
    return slot ? &specializations->list.data[slot - 1] : nullptr;
}

static bool
call_fits_instance(ir_instr_t *call, const specialization_t *instance,
                   th_ctx *types)
{
    size_t count = th_func_nparams(instance->signature);
    if (call->n_operands != count + 1) return true;
    for (size_t i = 0; i < count; i++) {
        const th_param *wanted = th_func_param(instance->signature, i);
        ir_operand_t *operand = &call->operands[i + 1];
        if (operand->kind != IR_OPD_VALUE || !operand->value || !wanted)
            continue;
        th_type *given = operand->value->type;
        if (th_is_closed(types, given)
            && th_hash(types, wanted->type) != th_hash(types, given))
            return false;
    }
    return true;
}

typedef struct {
    ir_ctx_t *context;
    specializations_t *specializations;
    core_list_t(n00bb_specialization_t) *instances;
    core_list_t(n00bb_ir_func_ptr_t) *functions;
    core_list_t(n00bb_specialization_request_t) *requests;
    core_buffer_t *failure;
} specialize_state_t;

static bool
retarget_reference(specialize_state_t *state, ir_func_t *caller,
                   size_t parent, ir_value_t **slot, th_type *signature)
{
    ir_value_t *reference = *slot;
    ir_func_t *target = reference && reference->kind == IR_VAL_FUNC
        ? n00bb_cached_body(reference->owner) : nullptr;
    if (!target || !target->n_blocks || !signature) return true;
    th_ctx *types = ir_module_types(target->owner);
    if (th_is_closed(types, target->type) || !th_is_closed(types, signature))
        return true;
    uint64_t hash = th_hash(types, signature);
    ir_func_t *copy = find_instance(state->specializations, target, hash,
                                    types);
    if (!copy) {
        core_list_t(type_binding_t) bindings = {0};
        bool matched = match_type(types, target->type, signature, &bindings)
            && bindings.len;
        copy = matched ? instantiate(state->context, target, signature,
            &bindings, types, state->failure) : nullptr;
        core_list_free(bindings);
        if (!matched) return true;
        if (!copy) return false;
        add_specialization(state->specializations,
            ((specialization_t){.source = target, .copy = copy,
                                .signature = signature, .parent = parent}));
        core_list_push(*state->instances,
            ((n00bb_specialization_t){target, copy, signature}));
        core_list_push(*state->functions, copy);
    }
    core_list_push(*state->requests,
        ((n00bb_specialization_request_t){caller->owner, copy}));
    ir_value_t *bound = ir_func_address(copy->owner, copy);
    if (!bound) {
        if (state->failure) {
            core_string_t module = ir_module_name(copy->owner);
            core_buffer_printf(state->failure,
                "could not bind specialized function %.*s.%.*s",
                (int)module.u8_bytes, module.data,
                (int)copy->name.u8_bytes, copy->name.data);
        }
        return false;
    }
    *slot = bound;
    return true;
}

static bool
specialize_callback(specialize_state_t *state, ir_func_t *caller,
                    size_t parent, ir_instr_t *creation)
{
    th_type *handler_signature = nullptr;
    th_type *setup_signature = nullptr;
    if (!ir_instr_callback_contract(creation, nullptr, &handler_signature,
                                    &setup_signature)) return true;
    uint32_t at = setup_signature ? 1 : 0;
    if (setup_signature && creation->n_operands
        && creation->operands[0].kind == IR_OPD_VALUE
        && !retarget_reference(state, caller, parent,
               &creation->operands[0].value, setup_signature)) return false;
    if (creation->n_operands <= at
        || creation->operands[at].kind != IR_OPD_VALUE) return true;
    ir_value_t *reference = creation->operands[at].value;
    ir_instr_t *retained = reference && reference->kind == IR_VAL_INSTR
        ? reference->def : nullptr;
    if (!retained || retained->op != IR_OP_RETAIN_REF
        || retained->n_operands != 1
        || retained->operands[0].kind != IR_OPD_VALUE) return true;
    return retarget_reference(state, caller, parent,
        &retained->operands[0].value, handler_signature);
}

enum { FINALIZE_DEPTH = 16 };

static ir_func_t *
finalize_placeholder(const ir_instr_t *call)
{
    return n00bb_open_call(call, CORE_STRING_STATIC("__n00bb_finalize_open"));
}

typedef struct {
    specialize_state_t *state;
    const n00bb_trait_binder_t *binder;
    core_dict_t        *glue;
    size_t              scanned;
    size_t              locals;
    core_dict_t         heap_types;
    core_dict_t         modules;
    core_dict_t         finalizers;
    n00bb_specialization_failure_t *report;
} finalize_state_t;

static void
collect_modules(finalize_state_t *finalize)
{
    const function_list_t *functions = finalize->state->functions;
    for (; finalize->scanned < functions->len; finalize->scanned++) {
        ir_func_t *function = functions->data[finalize->scanned];
        if (function && function->owner)
            core_dict_add(&finalize->modules, function->owner, 1);
    }
}

static ir_func_t *
bound_implementation(finalize_state_t *finalize, th_ctx *types,
                     th_type *receiver, core_string_t trait,
                     core_string_t method)
{
    const n00bb_trait_binder_t *binder = finalize->binder;
    ir_func_t *target = receiver && binder && binder->implementation
        ? binder->implementation(binder->context, types, receiver, trait,
                                 method)
        : nullptr;
    ir_func_t *body = target ? n00bb_cached_body(target) : nullptr;
    return body && body->n_blocks && th_func_param(target->type, 0)
        ? target : nullptr;
}

static ir_func_t *
finalize_implementation(finalize_state_t *finalize, th_ctx *types,
                        th_type *type)
{
    uint64_t key = th_hash(types, type) | 1;
    bool known = false;
    ir_func_t *target = core_dict_get(&finalize->finalizers,
                                      (const void *)(uintptr_t)key, &known);
    if (known) return target;
    target = bound_implementation(finalize, types, th_ind(types, 0, type),
        CORE_STRING_STATIC("Finalize"), CORE_STRING_STATIC("__finalize"));
    core_dict_put(&finalize->finalizers, (const void *)(uintptr_t)key,
                  target);
    return target;
}

static ir_func_t *
finalizer_implementation(finalize_state_t *finalize, th_ctx *types,
                         th_type *type)
{
    ir_func_t *target = finalize_implementation(finalize, types, type);
    return target && !n00bb_generic_generated_finalizer(target->name)
        ? target : nullptr;
}

static th_type *
finalize_signature(th_ctx *types, th_type *type)
{
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_param parameter = {.mode = TH_CONSUME, .type = th_ind(types, 0, type)};
    return parameter.type
        ? th_func_with_error(types, nil_type, nil_type, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
}

typedef struct {
    finalize_state_t *finalize;
    th_ctx           *types;
} finalizer_probe_t;

static bool
has_finalizer(void *context, th_type *type)
{
    finalizer_probe_t *probe = context;
    return finalizer_implementation(probe->finalize, probe->types, type)
        != nullptr;
}

static bool
finalizable(finalize_state_t *finalize, th_ctx *types, th_type *type)
{
    finalizer_probe_t probe = {finalize, types};
    return n00bb_finalize_reaches(types, type, false, true, has_finalizer,
                                  &probe);
}

static ir_func_t *finalize_target(finalize_state_t *finalize,
                                  ir_module_t *module, th_type *type,
                                  ir_span_t span, size_t depth);

static ir_func_t *
concrete_instance(finalize_state_t *finalize, ir_module_t *requester,
                  ir_func_t *target, th_type *actual)
{
    th_ctx *types = ir_module_types(requester);
    specializations_t *specializations = finalize->state->specializations;
    target = n00bb_cached_body(target);
    ir_func_t *copy = actual ? find_instance(specializations, target,
        th_hash(types, actual), types) : nullptr;
    if (copy || !actual) return copy;
    core_list_t(type_binding_t) bindings = {0};
    bool matched = match_type(types, target->type, actual, &bindings)
        && bindings.len;
    copy = matched ? instantiate(finalize->state->context, target, actual,
        &bindings, types, finalize->state->failure) : nullptr;
    core_list_free(bindings);
    if (!copy) return nullptr;
    add_specialization(specializations,
        ((specialization_t){.source = target, .copy = copy,
                            .signature = actual, .parent = SIZE_MAX}));
    core_list_push(*finalize->state->instances,
        ((n00bb_specialization_t){target, copy, actual}));
    core_list_push(*finalize->state->functions, copy);
    core_list_push(*finalize->state->requests,
        ((n00bb_specialization_request_t){requester, copy}));
    return copy;
}

static ir_func_t *
concrete_finalizer(finalize_state_t *finalize, ir_module_t *requester,
                   ir_func_t *target, th_type *type)
{
    if (th_is_closed(ir_module_types(target->owner), target->type))
        return target;
    return concrete_instance(finalize, requester, target,
        finalize_signature(ir_module_types(requester), type));
}

static ir_func_t *
concrete_hook(finalize_state_t *finalize, ir_module_t *requester,
              ir_func_t *target, th_type *receiver)
{
    th_ctx *types = ir_module_types(target->owner);
    if (th_is_closed(types, target->type)) return target;
    const th_param *self = th_func_param(target->type, 0);
    core_list_t(type_binding_t) bindings = {0};
    bool matched = self && match_type(types, self->type, receiver,
                                      &bindings) && bindings.len;
    const th_type **from = matched
        ? core_alloc_array(const th_type *, bindings.len) : nullptr;
    th_type **to = matched ? core_alloc_array(th_type *, bindings.len)
                           : nullptr;
    for (size_t i = 0; from && to && i < bindings.len; i++) {
        from[i] = bindings.data[i].variable;
        to[i] = bindings.data[i].actual;
    }
    th_type *actual = from && to ? th_type_clone_replacing(types,
        target->type, from, to, bindings.len) : nullptr;
    core_free(from);
    core_free(to);
    core_list_free(bindings);
    return actual && th_is_closed(types, actual)
        ? concrete_instance(finalize, requester, target, actual) : nullptr;
}

static bool
heap_fields_finalize(finalize_state_t *finalize, th_ctx *types,
                     th_type *type)
{
    th_type *shape = th_fields_record(types, type);
    for (size_t i = 0; shape && i < th_record_len(shape); i++) {
        const th_member *member = th_record_member(shape, i);
        if (member && finalizable(finalize, types, member->type))
            return true;
    }
    return false;
}

static bool
heap_finalizer_instance(finalize_state_t *finalize, ir_func_t *caller,
                        th_type *type, size_t depth)
{
    th_ctx *types = ir_module_types(caller->owner);
    type = type ? th_resolve(type) : nullptr;
    if (!type || depth >= FINALIZE_DEPTH) return true;
    if (th_kind_of(type) == TH_INDIRECTION)
        return heap_finalizer_instance(finalize, caller,
                                       th_ind_referent(type), depth + 1);
    if (th_kind_of(type) != TH_NAMED || !th_named_nargs(type)
        || !th_is_closed(types, type)
        || !core_dict_add(&finalize->heap_types, th_hash(types, type) | 1, 1))
        return true;
    for (size_t i = 0; i < th_named_nargs(type); i++) {
        const th_arg *argument = th_named_arg(type, i);
        if (argument && argument->kind == TH_ARG_TYPE
            && !heap_finalizer_instance(finalize, caller, argument->type,
                                        depth + 1)) return false;
    }
    ir_func_t *target = finalize_implementation(finalize, types, type);
    target = target ? n00bb_cached_body(target) : nullptr;
    if (!target || (n00bb_generic_generated_finalizer(target->name)
                    && !heap_fields_finalize(finalize, types, type)))
        return true;
    return concrete_finalizer(finalize, caller->owner, target, type)
        != nullptr;
}

static ir_value_t *
glue_value(ir_block_t *block, ir_op_t op, th_type *type, ir_span_t span)
{
    ir_instr_t *in = ir_instr_add(block, op);
    if (!in || !ir_instr_set_fresh_dest(in, "__n00bb_glue", type))
        return nullptr;
    in->span = span;
    return in->dest;
}

static bool
glue_call(finalize_state_t *finalize, ir_func_t *glue, ir_block_t *block,
          ir_value_t *address, th_type *type, ir_span_t span, size_t depth)
{
    th_ctx *types = ir_module_types(glue->owner);
    if (!finalizable(finalize, types, type)) return true;
    ir_func_t *target = finalize_target(finalize, glue->owner, type, span,
                                        depth + 1);
    ir_value_t *callee = target
        ? ir_func_address(target->owner, target) : nullptr;
    ir_value_t *consumed = callee
        ? glue_value(block, IR_OP_CONSUME, address->type, span) : nullptr;
    ir_value_t *done = consumed && ir_instr_operand_value(consumed->def,
                                                         address)
        ? glue_value(block, IR_OP_CALL,
                     th_name(types, CORE_STRING_STATIC("nil")), span)
        : nullptr;
    th_type *signature = done ? finalize_signature(types, type) : nullptr;
    return signature && ir_instr_operand_value(done->def, callee)
        && ir_instr_operand_value(done->def, consumed)
        && ir_instr_set_call_signature(done->def, signature);
}

static bool
glue_copy_call(finalize_state_t *finalize, ir_func_t *glue,
               ir_block_t *block, ir_value_t *value, th_type *type,
               ir_span_t span, size_t depth)
{
    th_ctx *types = ir_module_types(glue->owner);
    if (!finalizable(finalize, types, type)) return true;
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_glue_%zu", finalize->locals++);
    ir_local_t *local = name.data
        ? ir_local_add(glue, core_buffer_string(&name), type) : nullptr;
    core_buffer_release(&name);
    ir_instr_t *copy = local ? ir_instr_add(block, IR_OP_COPY) : nullptr;
    if (!copy || !ir_instr_set_dest_local(copy, local)
        || !ir_instr_operand_value(copy, value)) return false;
    copy->span = span;
    ir_value_t *address = glue_value(block, IR_OP_ADDRESS_OF,
                                     th_ind(types, 0, type), span);
    return address && ir_instr_operand_local(address->def, local)
        && glue_call(finalize, glue, block, address, type, span, depth);
}

static bool
glue_body(finalize_state_t *finalize, ir_func_t *glue, ir_value_t *self,
          th_type *type, ir_span_t span, size_t depth)
{
    th_ctx *types = ir_module_types(glue->owner);
    ir_block_t *block = ir_block_add(glue, CORE_STRING_STATIC("entry"));
    if (!block) return false;
    th_type *item = th_option_item(type);
    size_t count = 0;
    th_type *shape = item || th_tuple_element(type, 0, &count)
        ? nullptr : n00bb_finalize_record_shape(types, type, true, true);
    if (shape) {
        for (size_t i = 0; i < th_record_len(shape); i++) {
            const th_member *member = th_record_member(shape, i);
            if (!member || !finalizable(finalize, types, member->type))
                continue;
            ir_value_t *field = glue_value(block, IR_OP_FIELD,
                th_ind(types, 0, member->type), span);
            if (!field || !ir_instr_operand_value(field->def, self)
                || !ir_instr_operand_name(field->def, member->name)
                || !glue_call(finalize, glue, block, field, member->type,
                              span, depth)) return false;
        }
    }
    else {
        ir_value_t *value = glue_value(block, IR_OP_LOAD, type, span);
        if (!value || !ir_instr_operand_value(value->def, self)) return false;
        for (size_t i = 0; !item && i < count; i++) {
            th_type *element = th_tuple_element(type, i, nullptr);
            ir_value_t *part = glue_value(block, IR_OP_EXTRACT_INDEX,
                                          element, span);
            if (!part || !ir_instr_operand_value(part->def, value)
                || !ir_instr_operand_uint(part->def, i)
                || !glue_copy_call(finalize, glue, block, part, element,
                                   span, depth)) return false;
        }
        if (item) {
            ir_value_t *absent = glue_value(block, IR_OP_OPTION_IS_NIL,
                th_name(types, CORE_STRING_STATIC("bool")), span);
            ir_block_t *present = ir_block_add(glue,
                                               CORE_STRING_STATIC("present"));
            ir_block_t *join = ir_block_add(glue, CORE_STRING_STATIC("done"));
            ir_instr_t *branch = absent && present && join
                && ir_instr_operand_value(absent->def, value)
                ? ir_instr_add(block, IR_OP_BRANCH) : nullptr;
            if (!branch || !ir_instr_operand_value(branch, absent)
                || !ir_instr_operand_block(branch, join)
                || !ir_instr_operand_block(branch, present)) return false;
            branch->span = span;
            ir_value_t *payload = glue_value(present, IR_OP_OPTION_GET, item,
                                             span);
            ir_instr_t *jump = payload
                && ir_instr_operand_value(payload->def, value)
                && glue_copy_call(finalize, glue, present, payload, item,
                                  span, depth)
                ? ir_instr_add(present, IR_OP_JUMP) : nullptr;
            if (!jump || !ir_instr_operand_block(jump, join)) return false;
            jump->span = span;
            block = join;
        }
    }
    ir_instr_t *ret = ir_instr_add(block, IR_OP_RETURN);
    if (ret) ret->span = span;
    return ret != nullptr;
}

static ir_func_t *
finalize_target(finalize_state_t *finalize, ir_module_t *module,
                th_type *type, ir_span_t span, size_t depth)
{
    th_ctx *types = ir_module_types(module);
    th_type *resolved = th_resolve(type);
    ir_func_t *implementation = finalizer_implementation(finalize,
                                                         types, resolved);
    if (implementation || depth >= FINALIZE_DEPTH) return implementation;
    uint64_t hash = th_hash(types, resolved);
    ir_func_t *glue = core_dict_get(finalize->glue, hash | 1, nullptr);
    if (glue) return glue;
    th_type *signature = finalize_signature(types, resolved);
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_finalize_glue#%016llx",
                       (unsigned long long)hash);
    ir_func_t *existing = name.data
        ? ir_func_find(module, core_buffer_string(&name)) : nullptr;
    if (existing) {
        core_buffer_release(&name);
        core_dict_add(finalize->glue, hash | 1, existing);
        return existing;
    }
    glue = signature && name.data ? ir_func_add(module,
        core_buffer_string(&name), signature, IR_LINK_INTERNAL) : nullptr;
    core_buffer_release(&name);
    ir_value_t *self = glue ? ir_param_add(glue, CORE_STRING_STATIC("self"),
        th_func_param(signature, 0)->type) : nullptr;
    if (!self) return nullptr;
    core_dict_add(finalize->glue, hash | 1, glue);
    if (!glue_body(finalize, glue, self, resolved, span, depth))
        return nullptr;
    core_list_push(*finalize->state->functions, glue);
    return glue;
}

static bool resolve_open_reference(finalize_state_t *finalize,
                                   ir_instr_t *call);

static bool
specialize_finalizers(finalize_state_t *finalize, ir_func_t *caller)
{
    th_ctx *types = ir_module_types(caller->owner);
    core_list_t(selection_t) calls = core_list_new(selection_t);
    for (uint32_t b = 0; b < caller->n_blocks; b++)
        for (uint32_t i = 0; i < caller->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = caller->blocks[b]->instrs[i];
            if (!resolve_open_reference(finalize, in)) {
                core_list_free(calls);
                return false;
            }
            if (finalize_placeholder(in)) core_list_push(calls, in);
            ir_annot_t *copy = ir_annot_get(&in->annots,
                CORE_STRING_STATIC("n00bb.place_copy"));
            if (!copy || copy->kind != IR_ANNOT_STR || !in->dest
                || !finalizable(finalize, types, in->dest->type))
                continue;
            const specialization_t *instance = instance_of(
                finalize->state->specializations, caller);
            ir_func_t *source = instance && instance->source
                ? instance->source : caller;
            n00bb_specialization_failure_t *report = finalize->report;
            if (report) {
                report->function = source->name;
                report->module = ir_module_name(source->owner);
                report->before = n00bb_print_type(types, in->dest->type);
                report->after = copy->s;
                report->span = in->span;
                report->located = true;
                report->finalized_copy = true;
            }
            core_list_free(calls);
            return false;
        }
    bool valid = true;
    for (uint32_t p = 0; valid && p < caller->n_params; p++)
        valid = heap_finalizer_instance(finalize, caller,
                                        caller->params[p]->type, 0);
    for (uint32_t b = 0; valid && b < caller->n_blocks; b++)
        for (uint32_t i = 0; valid && i < caller->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = caller->blocks[b]->instrs[i];
            if (in->dest)
                valid = heap_finalizer_instance(finalize, caller,
                                                in->dest->type, 0);
        }
    for (size_t c = 0; valid && c < calls.len; c++) {
        ir_instr_t *call = calls.data[c];
        ir_value_t *consumed = ir_operand_value(&call->operands[1]);
        th_type *reference = consumed && consumed->type
            ? th_resolve(consumed->type) : nullptr;
        th_type *type = reference && th_kind_of(reference) == TH_INDIRECTION
            ? th_resolve(th_ind_referent(reference)) : nullptr;
        if (!type || !th_is_closed(types, type)) continue;
        if (!finalizable(finalize, types, type)) {
            ir_instr_t *consume = consumed->kind == IR_VAL_INSTR
                ? consumed->def : nullptr;
            valid = ir_instr_erase(call);
            if (valid && consume && consume->op == IR_OP_CONSUME)
                valid = ir_instr_erase(consume);
            continue;
        }
        ir_func_t *target = finalize_target(finalize, caller->owner, type,
                                            call->span, 0);
        ir_value_t *bound = target
            ? ir_func_address(target->owner, target) : nullptr;
        th_type *signature = bound ? finalize_signature(types, type)
                                   : nullptr;
        valid = signature != nullptr;
        if (valid) {
            call->operands[0].value = bound;
            ir_instr_use_added(call, 0);
            valid = ir_instr_set_call_signature(call, signature);
        }
    }
    core_list_free(calls);
    return valid;
}

static ir_func_t *
hook_implementation(finalize_state_t *finalize, th_ctx *types,
                    th_type *receiver, core_string_t method)
{
    return bound_implementation(finalize, types, receiver,
                                (core_string_t){0}, method);
}

static bool
resolve_open_reference(finalize_state_t *finalize, ir_instr_t *call)
{
    bool mutable = n00bb_open_call(call,
        CORE_STRING_STATIC("__n00bb_ref_address_mut_open")) != nullptr;
    if (!mutable && !n00bb_open_call(call,
            CORE_STRING_STATIC("__n00bb_ref_address_open")))
        return true;
    ir_value_t *reference = ir_operand_value(&call->operands[1]);
    th_ctx *types = ir_module_types(call->block->owner->owner);
    th_type *receiver = reference ? th_resolve(reference->type) : nullptr;
    if (!receiver || !th_is_closed(types, receiver)) return true;
    if (th_kind_of(receiver) == TH_INDIRECTION) {
        call->op = IR_OP_COPY;
        call->operands[0] = call->operands[1];
        call->n_operands = 1;
        ir_instr_use_added(call, 0);
        return true;
    }
    ir_func_t *target = hook_implementation(finalize, types, receiver,
        mutable ? CORE_STRING_STATIC("__ref_address_mut")
                : CORE_STRING_STATIC("__ref_address"));
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_param parameter = {.mode = TH_IN, .type = receiver};
    th_type *signature = target && call->dest
        ? th_func_with_error(types, call->dest->type, nil_type, &parameter,
                             1, nullptr, nullptr, 0) : nullptr;
    ir_value_t *bound = signature
        ? ir_func_address(target->owner, target) : nullptr;
    if (!bound) return false;
    call->operands[0].value = bound;
    ir_instr_use_added(call, 0);
    return ir_instr_set_call_signature(call, signature);
}

static bool
record_boxed_hook(finalize_state_t *finalize, ir_module_t *module,
                  th_type *type, core_string_t root,
                  core_list_t(n00bb_boxed_finalizer_t) *boxed)
{
    th_ctx *types = ir_module_types(module);
    uint64_t hash = th_hash(types, type);
    for (size_t i = 0; i < boxed->len; i++)
        if (boxed->data[i].typehash == hash) return true;
    ir_func_t *target = hook_implementation(finalize, types, type, root);
    ir_func_t *function = target
        ? concrete_hook(finalize, module, target, type) : nullptr;
    if (function) core_list_push(*boxed,
        ((n00bb_boxed_finalizer_t){hash, function}));
    return true;
}

static bool
record_boxed_addresses(finalize_state_t *finalize, n00bb_boxed_hooks_t *boxed)
{
    collect_modules(finalize);
    core_list_t(n00bb_ir_func_ptr_t) modules =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_foreach(&finalize->modules, item)
        core_list_push(modules, (ir_func_t *)item->key);
    bool valid = true;
    for (size_t m = 0; valid && m < modules.len; m++) {
        ir_module_t *module = (ir_module_t *)modules.data[m];
        th_ctx *types = ir_module_types(module);
        for (size_t t = 0; valid && t < ir_type_count(module); t++) {
            th_type *type = ir_type_at(module, t);
            type = type ? th_resolve(type) : nullptr;
            if (!type || th_kind_of(type) != TH_NAMED
                || !th_is_closed(types, type)) continue;
            valid = record_boxed_hook(finalize, module, type,
                    CORE_STRING_STATIC("__ref_address"), &boxed->addresses)
                && record_boxed_hook(finalize, module, type,
                    CORE_STRING_STATIC("__ref_address_mut"),
                    &boxed->mutable_addresses);
        }
    }
    core_list_free(modules);
    return valid;
}

static bool
record_boxed_finalizers(finalize_state_t *finalize,
                        core_list_t(n00bb_boxed_finalizer_t) *boxed)
{
    collect_modules(finalize);
    core_dict_t recorded;
    core_dict_init(&recorded, nullptr, nullptr);
    for (size_t i = 0; i < boxed->len; i++)
        core_dict_add(&recorded, boxed->data[i].typehash | 1, 1);
    core_list_t(n00bb_ir_func_ptr_t) modules =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_foreach(&finalize->modules, item)
        core_list_push(modules, (ir_func_t *)item->key);
    bool valid = true;
    for (size_t m = 0; valid && m < modules.len; m++) {
        ir_module_t *module = (ir_module_t *)modules.data[m];
        th_ctx *types = ir_module_types(module);
        ir_span_t span = ir_module_span(module);
        span.implied = true;
        for (size_t t = 0; valid && t < ir_type_count(module); t++) {
            th_type *type = ir_type_at(module, t);
            type = type ? th_resolve(type) : nullptr;
            if (!type || !th_is_closed(types, type)
                || !finalizable(finalize, types, type)) continue;
            uint64_t hash = th_hash(types, type);
            if (!core_dict_add(&recorded, hash | 1, 1)) continue;
            ir_func_t *target = finalize_target(finalize, module, type,
                                                span, 0);
            ir_func_t *function = target
                ? concrete_finalizer(finalize, module, target, type)
                : nullptr;
            valid = function != nullptr;
            if (valid) core_list_push(*boxed,
                ((n00bb_boxed_finalizer_t){hash, function}));
        }
    }
    core_list_free(modules);
    core_dict_free(&recorded);
    return valid;
}

static bool
bind_trait_method(const n00bb_trait_binder_t *binder, ir_func_t *caller,
                  ir_instr_t *selection)
{
    th_ctx *types = ir_module_types(caller->owner);
    ir_value_t *receiver = binder && binder->implementation
            && selection->n_operands == 4 && selection->dest
            && selection->operands[0].kind == IR_OPD_NAME
            && selection->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&selection->operands[2]) : nullptr;
    for (unsigned depth = 0; receiver && depth < 16
         && !th_is_closed(types, receiver->type) && receiver->def
         && (receiver->def->op == IR_OP_TYPE_NARROW
             || receiver->def->op == IR_OP_COPY)
         && receiver->def->n_operands >= 1; depth++) {
        ir_value_t *source = ir_operand_value(&receiver->def->operands[0]);
        if (!source) break;
        receiver = source;
    }
    ir_func_t *target = receiver && th_is_closed(types, receiver->type)
        ? binder->implementation(binder->context, types,
                                 th_resolve(receiver->type),
                                 selection->operands[0].name,
                                 selection->operands[1].name)
        : nullptr;
    ir_value_t *address = target ? ir_func_address(target->owner, target)
                                 : nullptr;
    if (!address) return false;
    bool closed = th_is_closed(ir_module_types(target->owner), target->type);
    for (uint32_t b = 0; b < caller->n_blocks; b++)
        for (uint32_t i = 0; i < caller->blocks[b]->n_instrs; i++) {
            ir_instr_t *call = caller->blocks[b]->instrs[i];
            if (call->op != IR_OP_CALL || !call->n_operands
                || ir_operand_value(&call->operands[0]) != selection->dest)
                continue;
            if ((closed && !ir_instr_set_call_signature(call, target->type))
                || !n00bb_place_named_arguments(call, target))
                return false;
            th_type *signature = closed ? nullptr
                : ir_instr_call_signature(call);
            core_list_t(type_binding_t) bindings = {0};
            th_type *widened = signature && th_is_closed(types, signature)
                    && !match_type(types, target->type, signature, &bindings)
                    && match_parameters_widening(types, target->type,
                                                 signature, &bindings)
                ? widened_signature(types, target, &bindings) : nullptr;
            core_list_free(bindings);
            if (widened && (!ir_instr_set_call_signature(call, widened)
                            || !integer_arguments_to(caller, call, target,
                                                     widened)))
                return false;
        }
    ir_value_replace_uses(caller, selection->dest, address, nullptr, nullptr);
    return ir_instr_erase(selection);
}

static th_type *
selected_result(ir_func_t *caller, const ir_instr_t *selection)
{
    for (uint32_t b = 0; b < caller->n_blocks; b++)
        for (uint32_t i = 0; i < caller->blocks[b]->n_instrs; i++) {
            const ir_instr_t *call = caller->blocks[b]->instrs[i];
            if (call->op == IR_OP_CALL && call->dest && call->n_operands
                && ir_operand_value(&call->operands[0]) == selection->dest)
                return call->dest->type;
        }
    return nullptr;
}

static bool
dispatched_at_runtime(th_ctx *types, th_type *type)
{
    type = th_resolve(type);
    th_kind kind = type ? th_kind_of(type) : TH_VAR;
    bool open_variant = false;
    th_named_info(type, nullptr, &open_variant, nullptr);
    return !type || !th_is_closed(types, type) || kind == TH_TRAIT
        || kind == TH_SUM || kind == TH_VAR || open_variant
        || th_builtin_applied(type, TH_BUILTIN_ANY, 0);
}

static bool
failed_step(core_buffer_t *failure, const char *step,
            const ir_func_t *function)
{
    if (failure && !failure->byte_len && !function)
        core_buffer_printf(failure, "%s", step);
    else if (failure && !failure->byte_len) {
        core_string_t module = ir_module_name(function->owner);
        core_buffer_printf(failure, "%s in %.*s.%.*s", step,
                           (int)module.u8_bytes, module.data,
                           (int)function->name.u8_bytes, function->name.data);
    }
    return false;
}

static bool
record_boxed_hooks(finalize_state_t *finalize, n00bb_boxed_hooks_t *boxed,
                   core_buffer_t *failure)
{
    return (record_boxed_finalizers(finalize, &boxed->finalizers)
            || failed_step(failure, "could not record boxed finalizers",
                           nullptr))
        && (record_boxed_addresses(finalize, boxed)
            || failed_step(failure, "could not record boxed reference hooks",
                           nullptr));
}

static void
locate_selection_failure(specializations_t *specializations,
                         ir_func_t *caller, const ir_instr_t *selection,
                         n00bb_specialization_failure_t *report)
{
    const specialization_t *instance = instance_of(specializations, caller);
    ir_func_t *source = instance && instance->source ? instance->source
                                                     : caller;
    report->function = source->name;
    report->module = ir_module_name(source->owner);
    report->span = selection->span;
    report->located = true;
    const specialization_t *user = instance && instance->site_function
        ? instance_of(specializations, instance->site_function) : nullptr;
    ir_func_t *site = user && user->source ? user->source
                                           : instance ? instance->site_function
                                                      : nullptr;
    if (site && ir_span_known(instance->site)) {
        report->site_module = ir_module_name(site->owner);
        report->site = instance->site;
        report->site_located = true;
    }
}

static bool
missing_implementation(const n00bb_trait_binder_t *binder,
                       specializations_t *specializations, ir_func_t *caller,
                       ir_instr_t *selection,
                       n00bb_specialization_failure_t *report)
{
    th_ctx *types = ir_module_types(caller->owner);
    ir_value_t *receiver = binder && binder->implementation
            && selection->n_operands == 4
            && selection->operands[0].kind == IR_OPD_NAME
            && selection->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&selection->operands[2]) : nullptr;
    if (!receiver || dispatched_at_runtime(types, receiver->type)
        || binder->implementation(binder->context, types,
               th_resolve(receiver->type), selection->operands[0].name,
               selection->operands[1].name))
        return false;
    if (!report) return true;
    core_buffer_t used = {0};
    core_buffer_append_string(&used, selection->operands[0].name);
    core_buffer_putc(&used, '.');
    core_buffer_append_string(&used, selection->operands[1].name);
    locate_selection_failure(specializations, caller, selection, report);
    report->before = n00bb_print_type(types, receiver->type);
    report->after = core_string_dup(core_buffer_string(&used));
    report->trait = core_string_dup(selection->operands[0].name);
    report->missing_implementation = true;
    core_buffer_release(&used);
    return true;
}

static bool
operands_mismatched(const n00bb_trait_binder_t *binder,
                    specializations_t *specializations, ir_func_t *caller,
                    ir_instr_t *selection,
                    n00bb_specialization_failure_t *report)
{
    ir_module_t *module = caller->owner;
    th_ctx *types = ir_module_types(module);
    ir_value_t *receiver = binder && binder->implementation
            && selection->n_operands == 4
            && selection->operands[0].kind == IR_OPD_NAME
            && selection->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&selection->operands[2]) : nullptr;
    core_string_t method = receiver ? selection->operands[1].name
                                    : (core_string_t){0};
    const n00bb_binary_operator_t *binary = receiver
        ? n00bb_binary_operator_method(method) : nullptr;
    core_string_t spelling = binary ? binary->spelling
        : core_string_eq(method, CORE_STRING_STATIC("__equal"))
        ? CORE_STRING_STATIC("==")
        : core_string_eq(method, CORE_STRING_STATIC("__cmp"))
        ? CORE_STRING_STATIC("<")
        : (core_string_t){0};
    if (!spelling.u8_bytes || (binary && binary->shift)
        || dispatched_at_runtime(types, receiver->type)
        || (binder->flagset
            && binder->flagset(binder->context, receiver->type)))
        return false;
    ir_func_t *target = binder->implementation(binder->context, types,
        th_resolve(receiver->type), selection->operands[0].name, method);
    th_type *signature = target ? th_resolve(target->type) : nullptr;
    bool primitive = !target && n00bb_primitive_operator(
        ir_machine_of(module, receiver->type).class, method);
    th_type *wanted = primitive ? receiver->type
        : signature && th_kind_of(signature) == TH_FUNCTION
            && th_func_nparams(signature) == 2
            && th_is_closed(types, signature)
        ? th_func_param(signature, 1)->type : nullptr;
    for (uint32_t b = 0; wanted && b < caller->n_blocks; b++)
        for (uint32_t i = 0; i < caller->blocks[b]->n_instrs; i++) {
            ir_instr_t *call = caller->blocks[b]->instrs[i];
            const ir_value_t *right = call->op == IR_OP_CALL
                    && call->n_operands == 3
                    && ir_operand_value(&call->operands[0]) == selection->dest
                ? ir_operand_value(&call->operands[2]) : nullptr;
            if (!right || !th_is_closed(types, right->type)
                || ir_type_eq(module, right->type, wanted)
                || (!primitive
                    && ir_machine_of(module, right->type).class
                           == ir_machine_of(module, wanted).class))
                continue;
            const ir_instr_t *literal = right->def
                    && right->def->op == IR_OP_CONST
                    && right->def->n_operands == 1
                    && right->def->operands[0].kind == IR_OPD_CONST
                    && !right->def->operands[0].lit.is_float
                    && !right->def->operands[0].lit.text.data
                ? right->def : nullptr;
            ir_interval_t range = {0};
            bool fits = literal
                && ir_interval_of_integer_type(module, wanted, &range)
                && ir_interval_contains(range, ir_interval_point(
                       ir_lit_signed(literal->operands[0].lit.i)));
            ir_value_t *converted = fits
                ? inserted_value(call, IR_OP_CONST, wanted)
                : primitive
                    && n00bb_integer_type_promotes_to(right->type, wanted)
                ? operand_as(caller, call, (ir_value_t *)right, wanted)
                : nullptr;
            if (fits && converted
                && !ir_instr_operand_int_lit(converted->def,
                                             literal->operands[0].lit.i))
                converted = nullptr;
            if (converted) {
                call->operands[2].value = converted;
                ir_instr_uses_changed(call);
                i++;
                continue;
            }
            if (!report) return true;
            locate_selection_failure(specializations, caller, selection,
                                     report);
            report->before = n00bb_print_type(types, receiver->type);
            report->other = n00bb_print_type(types, right->type);
            report->after = spelling;
            report->mismatched_operands = true;
            return true;
        }
    return false;
}

static bool
lower_primitive_trait_call(const n00bb_trait_binder_t *binder,
                           ir_func_t *caller, ir_instr_t *selection)
{
    ir_module_t *module = caller->owner;
    ir_value_t *receiver = selection->n_operands == 4 && selection->dest
            && selection->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&selection->operands[2]) : nullptr;
    core_string_t method = receiver ? selection->operands[1].name
                                    : (core_string_t){0};
    if (!receiver || !th_is_closed(ir_module_types(module), receiver->type))
        return false;
    bool flagset = binder && binder->flagset
        && binder->flagset(binder->context, receiver->type);
    if (!flagset && !n00bb_primitive_operator(
            ir_machine_of(module, receiver->type).class, method))
        return false;
    bool compare = !flagset
        && core_string_eq(method, CORE_STRING_STATIC("__cmp"));
    th_type *comparison = compare ? selected_result(caller, selection)
                                  : nullptr;
    parse_sym_entry_t *const *members = comparison && binder
            && binder->comparison
        ? binder->comparison(binder->context, comparison) : nullptr;
    if (compare && (!members || !members[0] || !members[1] || !members[2]))
        return false;
    ir_block_t *scratch = ir_block_add(caller,
        CORE_STRING_STATIC("__n00bb_primitive"));
    if (!scratch) return false;
    ir_lower_context_t context = {
        .module = module, .function = caller, .block = scratch,
        .reachable = true,
    };
    bool ok = true;
    core_dict_t replacements;
    core_dict_init(&replacements, nullptr, nullptr);
    for (uint32_t b = 0; ok && b < caller->n_blocks; b++)
        for (uint32_t i = 0; ok && i < caller->blocks[b]->n_instrs; i++) {
            ir_instr_t *call = caller->blocks[b]->instrs[i];
            if (call->op != IR_OP_CALL || !call->dest
                || call->n_operands < 2 || call->n_operands > 3
                || ir_operand_value(&call->operands[0]) != selection->dest)
                continue;
            ir_value_t *left = ir_value_replacement(&replacements,
                ir_operand_value(&call->operands[1]));
            ir_value_t *right = call->n_operands == 3
                ? ir_value_replacement(&replacements,
                                       ir_operand_value(&call->operands[2]))
                : nullptr;
            ir_value_t *value = flagset
                ? n00bb_emit_flagset_operator(&context, method,
                                              call->dest->type, left, right,
                                              call->span)
                : compare
                ? right ? n00bb_emit_three_way(&context, call->dest->type,
                                               members, left, right,
                                               call->span)
                        : nullptr
                : n00bb_emit_primitive(&context, method, call->dest->type,
                                       left, right, call->span);
            ok = value != nullptr;
            while (ok && scratch->n_instrs)
                ok = ir_instr_move(scratch->instrs[0], call->block,
                                   call->index);
            if (!ok) break;
            core_dict_put(&replacements, call->dest, value);
            ok = ir_instr_erase(call);
            i = value->def->index;
        }
    ir_func_replace_values(caller, &replacements);
    core_dict_free(&replacements);
    return ok && ir_block_erase(scratch) && ir_instr_erase(selection);
}

bool n00bb_specialize_calls(ir_ctx_t *context,
    core_list_t(n00bb_ir_func_ptr_t) *functions,
    const n00bb_specialization_seed_t *seeds, size_t seed_count,
    core_list_t(n00bb_specialization_t) *instances,
    core_list_t(n00bb_specialization_request_t) *requests,
    n00bb_boxed_hooks_t *boxed_hooks,
    const n00bb_trait_binder_t *binder,
    n00bb_specialization_failure_t *report)
{
    if (!context || !functions || !instances || !requests
        || (seed_count && !seeds)) return false;
    core_buffer_t *failure = report ? &report->message : nullptr;
    specializations_t specializations = {0};
    core_dict_init(&specializations.by_copy, nullptr, nullptr);
    core_dict_init(&specializations.by_signature, nullptr, nullptr);
    core_dict_t inline_bodies;
    core_dict_init(&inline_bodies, nullptr, nullptr);
    core_dict_t glue;
    core_dict_init(&glue, nullptr, nullptr);
    specialize_state_t state = {
        .context = context, .specializations = &specializations,
        .instances = instances, .functions = functions,
        .requests = requests, .failure = failure,
    };
    finalize_state_t finalize = {
        .state = &state, .binder = binder, .glue = &glue, .report = report,
    };
    core_dict_init(&finalize.heap_types, nullptr, nullptr);
    core_dict_init(&finalize.modules, nullptr, nullptr);
    core_dict_init(&finalize.finalizers, nullptr, nullptr);
    bool valid = true;
    for (size_t i = 0; i < instances->len; i++)
        add_specialization(&specializations, ((specialization_t){
            .source = instances->data[i].generic,
            .copy = instances->data[i].instance,
            .signature = instances->data[i].signature, .parent = SIZE_MAX}));
    for (size_t s = 0; valid && s < seed_count; s++) {
        ir_func_t *target = n00bb_cached_body(seeds[s].generic);
        th_type *actual = seeds[s].signature;
        th_ctx *types = target ? ir_module_types(target->owner) : nullptr;
        if (!target || !target->n_blocks || !actual
            || find_instance(&specializations, target,
                             th_hash(types, actual), types)) continue;
        core_list_t(type_binding_t) bindings = {0};
        bool identity = th_hash(types, actual) == th_hash(types, target->type);
        bool matched = identity
            || (match_type(types, target->type, actual, &bindings)
                && bindings.len);
        ir_func_t *copy = matched ? instantiate(context, target, actual,
            &bindings, types, failure) : nullptr;
        core_list_free(bindings);
        valid = !matched || copy != nullptr;
        if (!copy) continue;
        add_specialization(&specializations,
            ((specialization_t){.source = target, .copy = copy,
                                .signature = actual, .parent = SIZE_MAX}));
        core_list_push(*instances,
            ((n00bb_specialization_t){target, copy, actual}));
        core_list_push(*functions, copy);
    }
    for (size_t index = 0; valid; index++) {
        if (index == functions->len) {
            if (!boxed_hooks) break;
            valid = record_boxed_hooks(&finalize, boxed_hooks, failure);
            if (!valid || index == functions->len) break;
        }
        ir_func_t *caller = functions->data[index];
        size_t parent = instance_slot(&specializations, caller) - 1;
        th_ctx *caller_types = ir_module_types(caller->owner);
        bool open_caller = !th_is_closed(caller_types, caller->type)
            && parent == SIZE_MAX;
        if (!open_caller) {
            valid = specialize_finalizers(&finalize, caller)
                || failed_step(failure, "could not specialize finalizer calls",
                               caller);
            if (!valid) break;
        }
        for (uint32_t b = 0; valid && b < caller->n_blocks; b++) {
            ir_block_t *block = caller->blocks[b];
            for (uint32_t i = 0; valid && i < block->n_instrs; i++) {
                ir_instr_t *call = block->instrs[i];
                if (call->op == IR_OP_CALLBACK_CREATE) {
                    if (!open_caller)
                        valid = specialize_callback(&state, caller, parent,
                                                    call)
                            || failed_step(failure,
                                   "could not specialize a callback", caller);
                    continue;
                }
                if (call->op == IR_OP_TRAIT_METHOD && !open_caller
                    && operands_mismatched(binder, &specializations, caller,
                                           call, report)) {
                    valid = false;
                    break;
                }
                if (call->op == IR_OP_TRAIT_METHOD && !open_caller
                    && (bind_trait_method(binder, caller, call)
                        || lower_primitive_trait_call(binder, caller,
                                                      call))) {
                    i--;
                    continue;
                }
                if (call->op == IR_OP_TRAIT_METHOD && !open_caller
                    && missing_implementation(binder, &specializations,
                                              caller, call, report)) {
                    valid = false;
                    break;
                }
                if (call->op != IR_OP_CALL || !call->n_operands) continue;
                ir_value_t *callee = ir_operand_value(&call->operands[0]);
                ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
                    ? n00bb_cached_body(callee->owner) : nullptr;
                const specialization_t *instance = target
                    ? instance_of(&specializations, target) : nullptr;
                bool retargeted = instance && !call_fits_instance(call,
                    instance, ir_module_types(target->owner));
                if (retargeted) target = instance->source;
                else if (instance) continue;
                if (!target || !target->n_blocks) continue;
                th_ctx *types = ir_module_types(target->owner);
                if (open_caller && target == callee->owner
                    && !th_is_closed(types, target->type)) continue;
                if (th_is_closed(types, target->type)) {
                    valid = open_caller
                        || integer_arguments_as(caller, call, target)
                        || failed_step(failure,
                               "could not convert integer arguments", caller);
                    if (ir_func_is_always_inline(target)
                        && core_dict_add(&inline_bodies, target, 1))
                        core_list_push(*functions, target);
                    continue;
                }
                th_type *actual = open_caller
                    ? nullptr : concrete_call_signature(call, target, types,
                                                        retargeted);
                core_list_t(type_binding_t) bindings = {0};
                bool concrete = actual && th_is_closed(types, actual)
                    && match_type(types, target->type, actual, &bindings)
                    && bindings.len;
                if (!concrete && actual && th_is_closed(types, actual)
                    && match_type_with(types, target->type, actual,
                                       &bindings, true)
                    && bindings.len) {
                    th_type *widened = widened_signature(types, target,
                                                         &bindings);
                    concrete = widened
                        && ir_instr_set_call_signature(call, widened);
                    if (concrete) actual = widened;
                    else bindings.len = 0;
                }
                const specialization_t *grown = concrete
                    ? growing_instantiation(&specializations.list, parent,
                                            target, actual, types)
                    : nullptr;
                if (grown) {
                    report_growing_instantiation(report, call, target,
                                                 grown, actual, types);
                    core_list_free(bindings);
                    valid = false;
                    break;
                }
                if (!concrete) {
                    actual = target->type;
                    bindings.len = 0;
                }
                ir_func_t *copy = find_instance(&specializations, target,
                    th_hash(types, actual), types);
                if (!copy) {
                    copy = instantiate(context, target, actual, &bindings,
                                       types, failure);
                    valid = copy != nullptr;
                    if (valid) {
                        add_specialization(&specializations,
                            ((specialization_t){.source = target,
                                .copy = copy, .signature = actual,
                                .parent = parent, .site_function = caller,
                                .site = call->span}));
                        core_list_push(*instances,
                            ((n00bb_specialization_t){target, copy,
                                                      actual}));
                        core_list_push(*functions, copy);
                    }
                }
                core_list_free(bindings);
                if (!valid) break;
                core_list_push(*requests,
                    ((n00bb_specialization_request_t){caller->owner, copy}));
                ir_value_t *bound = ir_func_address(copy->owner, copy);
                valid = bound != nullptr;
                if (!valid && failure) {
                    core_string_t module = ir_module_name(copy->owner);
                    core_buffer_printf(failure,
                        "could not bind specialized function %.*s.%.*s",
                        (int)module.u8_bytes, module.data,
                        (int)copy->name.u8_bytes, copy->name.data);
                }
                if (valid) {
                    call->operands[0].value = bound;
                    ir_instr_use_added(call, 0);
                }
                if (valid && !open_caller
                    && th_is_closed(ir_module_types(copy->owner), copy->type))
                    valid = integer_arguments_as(caller, call, copy)
                        || failed_step(failure,
                               "could not convert integer arguments", caller);
            }
        }
    }
    specializations_free(&specializations);
    core_dict_free(&inline_bodies);
    core_dict_free(&glue);
    core_dict_free(&finalize.finalizers);
    core_dict_free(&finalize.heap_types);
    core_dict_free(&finalize.modules);
    return valid;
}
