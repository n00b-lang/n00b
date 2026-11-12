#include "lower/lower_internal.h"

enum {
    LEASE_ADDRESS = 1,
    LEASE_HELD    = 2,
    LEASE_SOURCE  = 4,
};

typedef struct {
    const ir_instr_t *at;
    core_string_t     reason;
} lease_violation_t;

bool
lease_storage_record(th_type *record)
{
    while (record && th_kind_of(th_resolve(record)) == TH_INDIRECTION)
        record = th_ind_referent(th_resolve(record));
    core_string_t name = {0};
    if (!record || !th_named_info(th_resolve(record), &name, nullptr, nullptr))
        return false;
    return core_string_eq(name,
               CORE_STRING_STATIC("builtin.containers.lease.Leasable.__fields"))
        || core_string_eq(name,
               CORE_STRING_STATIC("builtin.containers.lease.Lease.__fields"));
}

static bool
storage_field(const ir_instr_t *in)
{
    const ir_value_t *base = in->op == IR_OP_FIELD && in->n_operands == 2
            && in->operands[1].kind == IR_OPD_NAME
        ? ir_operand_used_value(in, 0) : nullptr;
    return base && base->type
        && core_string_eq(in->operands[1].name, CORE_STRING_STATIC("storage"))
        && lease_storage_record(base->type);
}

static const void *
lease_key(const ir_value_t *value)
{
    return value && value->kind == IR_VAL_LOCAL && value->local
        ? (const void *)value->local : (const void *)value;
}

static uintptr_t
lease_class(core_dict_t *classes, const ir_value_t *value)
{
    bool found = false;
    return value
        ? (uintptr_t)core_dict_get(classes, (void *)lease_key(value), &found)
        : 0;
}

static bool
lease_mark(core_dict_t *classes, const ir_instr_t *in, uintptr_t bits)
{
    const void *key = in->dest_kind == IR_DEST_LOCAL
        ? (const void *)in->dest_local
        : in->dest_kind == IR_DEST_VALUE ? (const void *)in->dest : nullptr;
    if (!key || !bits) return false;
    bool      found = false;
    uintptr_t old   = (uintptr_t)core_dict_get(classes, (void *)key, &found);
    if ((old | bits) == old) return false;
    core_dict_put(classes, (void *)key, (void *)(old | bits));
    return true;
}

static bool
fresh_result(ir_summaries_t *escapes, const ir_instr_t *call)
{
    ir_func_t *callee = ir_call_function(call);
    const ir_summary_t *summary = escapes && callee
        ? ir_summary_of(escapes, n00bb_cached_body(callee)) : nullptr;
    return summary && ir_summary_return(summary).kind == IR_SRETURN_FRESH;
}

static uintptr_t
lease_defines(ir_summaries_t *escapes, core_dict_t *classes,
              const ir_instr_t *in)
{
    if (storage_field(in)) return LEASE_ADDRESS;
    switch (in->op) {
    case IR_OP_COPY:
        return lease_class(classes, ir_operand_used_value(in, 0));
    case IR_OP_LOAD:
        return lease_class(classes, ir_operand_used_value(in, 0))
                & LEASE_ADDRESS ? LEASE_HELD : 0;
    case IR_OP_CALL:
        return fresh_result(escapes, in) ? LEASE_SOURCE : 0;
    default:
        return 0;
    }
}

static void
lease_seed(const ir_func_t *f, core_dict_t *classes)
{
    th_type *type = f->type ? th_resolve(f->type) : nullptr;
    for (uint32_t p = 0; type && p < f->n_params; p++) {
        const th_param *param = th_func_param(type, p);
        if (param && param->mode == TH_CONSUME && f->params[p])
            core_dict_put(classes, f->params[p], (void *)LEASE_SOURCE);
    }
}

static core_string_t
call_misuse(core_dict_t *classes, const ir_instr_t *call)
{
    th_type *type = ir_call_function_type(call);
    type = type ? th_resolve(type) : nullptr;
    for (uint32_t p = 0; p < ir_call_argument_count(call); p++) {
        ir_call_arg_t argument = ir_call_argument(call, p);
        if (!(lease_class(classes, argument.value) & LEASE_HELD)) continue;
        const th_param *param = type ? th_func_param(type, p) : nullptr;
        if (!param || param->mode == TH_CONSUME || param->mode == TH_OUT
            || param->mode == TH_INOUT)
            return CORE_STRING_STATIC("hands the array in `storage` to a "
                                      "call that may keep it");
    }
    return (core_string_t){0};
}

static core_string_t
store_misuse(core_dict_t *classes, const ir_instr_t *in)
{
    uintptr_t target = lease_class(classes, ir_operand_used_value(in, 0));
    uintptr_t value  = lease_class(classes, ir_operand_used_value(in, 1));
    if (value & LEASE_ADDRESS)
        return CORE_STRING_STATIC("keeps the address of `storage`");
    if (target & LEASE_ADDRESS) {
        if (value & (LEASE_HELD | LEASE_SOURCE)) return (core_string_t){0};
        return CORE_STRING_STATIC("stores an array into `storage` that "
                                  "something else may still hold");
    }
    if (value & LEASE_HELD)
        return CORE_STRING_STATIC("keeps the array in `storage` somewhere "
                                  "else");
    return (core_string_t){0};
}

static core_string_t
lease_misuse(core_dict_t *classes, const ir_instr_t *in)
{
    if (in->op == IR_OP_STORE) return store_misuse(classes, in);
    if (in->op == IR_OP_CALL) return call_misuse(classes, in);
    if (in->op == IR_OP_COPY || in->op == IR_OP_INDEX
        || in->op == IR_OP_SEQUENCE_LENGTH)
        return (core_string_t){0};
    for (uint32_t o = 0; o < in->n_operands; o++)
        if (lease_class(classes, ir_operand_used_value(in, o)) & LEASE_HELD)
            return CORE_STRING_STATIC("uses the array in `storage` as a "
                                      "value");
    return (core_string_t){0};
}

static bool
lease_check_function(ir_summaries_t *escapes, const ir_func_t *f,
                     lease_violation_t *out)
{
    bool touches = false;
    for (uint32_t b = 0; !touches && b < f->n_blocks; b++)
        for (uint32_t i = 0; !touches && i < f->blocks[b]->n_instrs; i++)
            touches = storage_field(f->blocks[b]->instrs[i]);
    if (!touches) return true;
    core_dict_t classes;
    core_dict_init(&classes, nullptr, nullptr);
    lease_seed(f, &classes);
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t b = 0; b < f->n_blocks; b++)
            for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = f->blocks[b]->instrs[i];
                changed |= lease_mark(&classes, in,
                                      lease_defines(escapes, &classes, in));
            }
    }
    bool clean = true;
    for (uint32_t b = 0; clean && b < f->n_blocks; b++)
        for (uint32_t i = 0; clean && i < f->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in     = f->blocks[b]->instrs[i];
            core_string_t     reason = lease_misuse(&classes, in);
            if (!reason.data) continue;
            *out  = (lease_violation_t){in, reason};
            clean = false;
        }
    core_dict_free(&classes);
    return clean;
}

bool
check_lease_storage(n00bb_link_analysis_t *analysis)
{
    bool clean = true;
    for (size_t u = 0; u < analysis->units.len; u++) {
        core_translation_unit_t *unit  = analysis->units.data[u];
        n00bb_tu_state_t        *state = tu_state(unit);
        if (!state || !state->ir_module || state->interface_module) continue;
        for (size_t f = 0; f < ir_func_count(state->ir_module); f++) {
            const ir_func_t  *function = ir_func_at(state->ir_module, f);
            lease_violation_t violation;
            if (!function
                || lease_check_function(analysis->escapes, function,
                                        &violation))
                continue;
            clean = false;
            n00bb_report(core_translation_unit_diagnostics(unit), unit,
                N00B_DIAG_LEASE_STORAGE,
                n00bb_span_location(violation.at->span),
                N00B_TEXT_LEASE_STORAGE,
                N00BB_ARGS(ARG_TEXT(n00bb_function_root(function->name)),
                           ARG_TEXT(violation.reason)));
        }
    }
    return clean;
}
