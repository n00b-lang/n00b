#include "types/constrain.h"
#include "ir/pdg.h"
#include "lib/dict.h"

typedef struct {
    ty_solver_t          *s;
    th_ctx               *types;
    const ir_group_pdg_t *g;
    ty_coverage_t        *coverage;
    core_dict_t           schemes;
    uint32_t              component;
    ir_func_t            *function;
} gen_t;

static ty_why_t why_of(const ir_instr_t *in, uint16_t rule)
{
    return (ty_why_t){.at = in, .rule = rule};
}

static th_type *operand_type(const ir_instr_t *in, uint32_t i)
{
    if (i >= in->n_operands) {
        return nullptr;
    }

    const ir_operand_t *o     = &in->operands[i];
    ir_local_t         *local = ir_operand_local(o);

    if (local) {
        return local->type;
    }
    if (o->kind == IR_OPD_TYPE) {
        return o->type;
    }

    ir_value_t *value = o->kind == IR_OPD_NAMED_ARG ? o->named.value
                                                    : ir_operand_value(o);

    return value ? value->type : nullptr;
}

static void skip(gen_t *g, const ir_instr_t *in)
{
    g->coverage->skipped++;
    g->coverage->skipped_by_op[in->op]++;
}

static th_type *referent(th_type *t)
{
    th_type *r = th_resolve(t);

    return r && th_kind_of(r) == TH_INDIRECTION ? th_ind_referent(r) : nullptr;
}

static th_type *element(th_type *t)
{
    th_type *r = th_resolve(t);

    if (r && th_kind_of(r) == TH_INDIRECTION) {
        r = th_resolve(th_ind_referent(r));
    }

    return r && th_kind_of(r) == TH_SEQUENCE ? th_seq_elem(r) : nullptr;
}

static th_type *callee_type(gen_t *g, ir_func_t *target, ir_instr_t *call)
{
    if (ir_group_pdg_component_of(g->g, target) == g->component) {
        return ty_instantiate_declared(g->s, target->type, why_of(call, 2));
    }

    bool     found  = false;
    th_type *scheme = core_dict_get(&g->schemes, target, &found);

    return ty_instantiate(g->s, found ? scheme : target->type,
                          why_of(call, 2));
}

static bool outcome_type(th_type *t)
{
    core_string_t name = {0};

    return t && th_kind_of(t) == TH_NAMED && th_named_info(t, &name, nullptr,
                                                           nullptr)
        && core_string_starts_with(name,
                                   CORE_STRING_STATIC("__lantern_call_outcome_"))
        && th_named_nargs(t) == 2;
}

static th_type *atomic_item(gen_t *g, const ir_instr_t *in, th_type *t)
{
    th_type *item = t && ir_instr_has_mod(in, CORE_STRING_STATIC("atomic"))
                      ? th_builtin_item(g->types, th_resolve(t),
                                        TH_BUILTIN_ATOMIC, 0)
                      : nullptr;

    return item ? item : t;
}

static th_type *open_field(const ir_instr_t *in)
{
    const ir_value_t *address = in->n_operands
                                  ? ir_operand_value(&in->operands[0])
                                  : nullptr;
    th_type          *held    = address && address->kind == IR_VAL_INSTR
                                 && address->def
                                 && address->def->op == IR_OP_FIELD
                                  ? referent(address->type)
                                  : nullptr;

    return held && th_kind_of(th_resolve(held)) == TH_VAR ? th_resolve(held)
                                                          : nullptr;
}

static th_type *call_outcome(gen_t *g, ir_instr_t *call, th_type *type)
{
    th_type *dest = call->dest_kind == IR_DEST_VALUE && call->dest
                      ? th_resolve(call->dest->type)
                      : nullptr;

    if (!outcome_type(dest)) {
        return th_func_ret(type);
    }
    if (g->coverage->outcome_calls.data) {
        core_list_push(g->coverage->outcome_calls, call);
    }
    ty_unify(g->s, th_named_arg(dest, 0)->type, th_func_ret(type),
             why_of(call, 28));
    if (th_func_error(type)) {
        ty_unify(g->s, th_named_arg(dest, 1)->type, th_func_error(type),
                 why_of(call, 28));
    }

    return nullptr;
}

static th_type *call_result(gen_t *g, ir_instr_t *call)
{
    ir_value_t *callee = call->n_operands ? ir_operand_value(&call->operands[0])
                                          : nullptr;
    ir_func_t  *target = callee && callee->kind == IR_VAL_FUNC ? callee->owner
                                                               : nullptr;
    th_type    *type   = target ? callee_type(g, target, call) : nullptr;

    if (!type || th_kind_of(th_resolve(type)) != TH_FUNCTION) {
        return nullptr;
    }
    type = th_resolve(type);
    if (!th_is_closed(g->types, target->type)) {
        ir_instr_set_call_signature(call, type);
    }

    uint32_t position = 0;

    for (uint32_t o = 1; o < call->n_operands; o++) {
        const ir_operand_t *operand = &call->operands[o];
        th_type            *actual  = operand_type(call, o);

        if (operand->kind == IR_OPD_NAMED_ARG) {
            const th_kwparam *k = th_func_kw_lookup(type, operand->named.name);
            th_type          *formal = k ? k->type : nullptr;

            for (uint32_t p = 0; !formal && p < th_func_nparams(type)
                                 && p < target->n_params; p++) {
                if (core_string_eq(target->params[p]->name,
                                   operand->named.name)) {
                    formal = th_func_param(type, p)->type;
                }
            }
            if (formal && actual) {
                ty_unify(g->s, formal, actual, why_of(call, 40));
            }
            continue;
        }

        uint32_t        index  = UINT32_MAX;
        ir_value_t     *param  = ir_call_param(target, operand, &position,
                                               &index);
        const th_param *formal = index != UINT32_MAX
                                   ? th_func_param(type, index)
                                   : param && param->is_rest
                                       ? th_func_rest(type)
                                       : nullptr;

        if (operand->kind == IR_OPD_FRESH) {
            actual = referent(actual);
        }
        if (formal && actual) {
            ty_unify(g->s, formal->type, actual, why_of(call, 40));
        }
    }

    return call_outcome(g, call, type);
}

static void dispatch_constraints(gen_t *g, ir_instr_t *in)
{
    if (in->n_operands < 2 || in->operands[0].kind != IR_OPD_NAME
        || !in->dest) {
        skip(g, in);
        return;
    }

    uint32_t    count  = in->n_operands - 1;
    th_param   *params = core_alloc_array(th_param, count);
    th_kwparam *kws    = core_alloc_array(th_kwparam, count);
    uint32_t    n      = 0;
    uint32_t    nkw    = 0;

    for (uint32_t o = 1; o < in->n_operands; o++) {
        if (in->operands[o].kind == IR_OPD_FRESH) {
            params[n++] = (th_param){.type = referent(operand_type(in, o))};
        }
        else if (in->operands[o].kind == IR_OPD_NAMED_ARG) {
            kws[nkw++] = (th_kwparam){.name = in->operands[o].named.name,
                                      .type = operand_type(in, o)};
        }
        else {
            params[n++] = (th_param){.type = operand_type(in, o)};
        }
    }

    th_type *dest    = th_resolve(in->dest->type);
    th_type *shape   = outcome_type(dest)
                         ? th_func_with_error(g->types,
                                              th_named_arg(dest, 0)->type,
                                              th_named_arg(dest, 1)->type,
                                              params, n, nullptr, kws, nkw)
                         : th_func(g->types, in->dest->type, params, n,
                                   nullptr, kws, nkw);
    th_type *subject = params[0].type;

    core_free(params);
    core_free(kws);
    if (outcome_type(dest) && g->coverage->outcome_calls.data) {
        core_list_push(g->coverage->outcome_calls, in);
    }
    const ir_value_t *index  = in->n_operands == 3
                                 ? ir_operand_value(&in->operands[2])
                                 : nullptr;
    const ir_instr_t *literal = index && index->def
                                    && index->def->op == IR_OP_CONST
                                    && index->def->n_operands
                                    && index->def->operands[0].kind
                                           == IR_OPD_CONST
                                ? index->def
                                : nullptr;
    bool positional = literal && !literal->operands[0].lit.is_float
                   && !literal->operands[0].lit.text.data
                   && !literal->operands[0].lit.i.neg
                   && literal->operands[0].lit.i.mag <= UINT32_MAX
                   && core_string_eq(in->operands[0].name,
                                     CORE_STRING_STATIC("__read"));

    ty_require(g->s,
               (ty_pred_t){.kind       = TY_PRED_DISPATCH,
                           .name       = in->operands[0].name,
                           .subject    = subject,
                           .other      = shape,
                           .positional = positional,
                           .index      = positional
                                             ? (uint32_t)literal->operands[0]
                                                   .lit.i.mag
                                             : 0},
               why_of(in, 17));
}

static void return_constraints(gen_t *g, ir_instr_t *in)
{
    th_type *type    = g->function->type;
    uint32_t outputs = 0;

    for (size_t i = 0; i < th_func_nparams(type); i++) {
        const th_param *param = th_func_param(type, i);

        outputs += param && th_mode_produces_output(param->mode);
    }

    uint32_t at = in->n_operands > outputs ? 1 : 0;

    ty_unify(g->s, th_func_ret(type),
             at ? operand_type(in, 0) : th_nil(g->types), why_of(in, 3));
    for (size_t i = 0; i < th_func_nparams(type); i++) {
        const th_param *param = th_func_param(type, i);

        if (param && th_mode_produces_output(param->mode)
            && at < in->n_operands) {
            ty_unify(g->s, param->type, operand_type(in, at++),
                     why_of(in, 3));
        }
    }
}

static th_type *phi_result(gen_t *g, ir_instr_t *in)
{
    th_type *joined = nullptr;

    for (uint32_t i = 0; i < in->n_operands; i++) {
        const ir_operand_t *o   = &in->operands[i];
        th_type            *arm = o->kind == IR_OPD_PHI_ARM && o->phi.value
                                    ? o->phi.value->type
                                    : nullptr;

        if (arm) {
            joined = joined ? ty_join(g->s, joined, arm, why_of(in, 4)) : arm;
        }
    }

    return joined;
}

static bool member_flows(gen_t *g, ir_instr_t *in)
{
    th_type *dest = in->dest ? th_resolve(in->dest->type) : nullptr;
    bool     record = dest && th_kind_of(dest) == TH_RECORD
                   && th_record_len(dest) == in->n_operands;
    bool     tuple  = dest && th_builtin_kind_of(dest) == TH_BUILTIN_TUPLE
                   && th_named_nargs(dest) == in->n_operands;

    for (uint32_t i = 0; (record || tuple) && i < in->n_operands; i++) {
        th_type *member = record ? th_record_member(dest, i)->type
                                 : th_named_arg(dest, i)->type;

        ty_unify(g->s, member, operand_type(in, i), why_of(in, 30));
    }

    return record || tuple;
}

static bool literal_flow(gen_t *g, ir_instr_t *in, th_type *item,
                         th_type *slot)
{
    if (!item || th_kind_of(th_resolve(item)) != TH_RECORD
        || th_kind_of(th_resolve(slot)) != TH_VAR) {
        return false;
    }
    ty_require(g->s,
               (ty_pred_t){.kind       = TY_PRED_FLOW,
                           .name       = CORE_STRING_STATIC("flow"),
                           .subject    = item,
                           .other      = slot,
                           .positional = true},
               why_of(in, 29));
    return true;
}

static th_type *aggregate_result(gen_t *g, ir_instr_t *in)
{
    ir_aggregate_form_t form = {0};

    if (member_flows(g, in)) {
        return nullptr;
    }
    th_type *element = in->dest && ir_instr_aggregate_form(in, &form)
                            && form == IR_AGGREGATE_ELEMENTS
                         ? th_seq_elem(th_resolve(in->dest->type))
                         : nullptr;

    for (uint32_t i = 0; element && i < in->n_operands; i++) {
        th_type *item = operand_type(in, i);

        if (!literal_flow(g, in, item, element)) {
            ty_unify(g->s, element, item, why_of(in, 29));
        }
    }
    if (element) {
        return nullptr;
    }
    if (!in->dest || !ir_instr_aggregate_form(in, &form)
        || th_kind_of(th_resolve(in->dest->type)) != TH_VAR) {
        skip(g, in);
        return nullptr;
    }

    bool     pairs     = form == IR_AGGREGATE_PAIRS;
    th_type *joined[2] = {0};
    bool     records   = false;

    for (uint32_t i = 0; i < in->n_operands; i++) {
        th_type *item = operand_type(in, i);

        records = records
               || (item && th_kind_of(th_resolve(item)) == TH_RECORD);
    }
    for (uint32_t i = 0; i < in->n_operands; i++) {
        th_type  *item = operand_type(in, i);
        th_type **into = &joined[pairs ? i & 1u : 0];

        if (records) {
            *into = *into ? *into : th_var(g->types);
            if (!literal_flow(g, in, item, *into)) {
                ty_unify(g->s, *into, item, why_of(in, 29));
            }
            continue;
        }
        *into = *into ? ty_join(g->s, *into, item, why_of(in, 29)) : item;
    }
    ty_require(g->s,
               (ty_pred_t){.kind    = TY_PRED_ELEMENT,
                           .name    = pairs
                                        ? CORE_STRING_STATIC("Dict_Syntax")
                                        : CORE_STRING_STATIC("Sequence_Syntax"),
                           .subject = in->dest->type,
                           .result  = joined[0] ? joined[0] : th_var(g->types),
                           .other   = !pairs     ? nullptr
                                      : joined[1] ? joined[1]
                                                  : th_var(g->types)},
               why_of(in, 29));

    return nullptr;
}

static th_type *field_result(const ir_instr_t *in)
{
    th_type *record = th_resolve(referent(operand_type(in, 0)));

    if (!record || th_kind_of(record) != TH_RECORD || in->n_operands < 2
        || in->operands[1].kind != IR_OPD_NAME) {
        return nullptr;
    }

    return th_record_lookup(record, in->operands[1].name);
}

static th_type *result_of(gen_t *g, ir_instr_t *in, bool *handled)
{
    *handled = true;

    switch (in->op) {
    case IR_OP_COPY:
    case IR_OP_CONSUME:
    case IR_OP_RETAIN_REF:
        return operand_type(in, 0);
    case IR_OP_ADD:
    case IR_OP_SUB:
    case IR_OP_MUL:
    case IR_OP_DIV:
    case IR_OP_REM:
    case IR_OP_AND:
    case IR_OP_OR:
    case IR_OP_XOR:
    case IR_OP_SHL:
    case IR_OP_SHR:
        return operand_type(in, 0);
    case IR_OP_NEG:
    case IR_OP_NOT:
        return operand_type(in, 0);
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_LT:
    case IR_OP_LE:
    case IR_OP_GT:
    case IR_OP_GE:
        if (in->n_operands == 2 && operand_type(in, 0)
            && operand_type(in, 1)) {
            ty_unify(g->s, operand_type(in, 0), operand_type(in, 1),
                     why_of(in, 20));
        }
        return nullptr;
    case IR_OP_LOAD:
        if (ir_pdg_reads_comptime(in)) {
            return operand_type(in, 0);
        }
        if ((!referent(operand_type(in, 0)) || open_field(in))
            && in->dest
            && !ir_instr_has_mod(in, CORE_STRING_STATIC("atomic"))) {
            ty_require(g->s,
                       (ty_pred_t){.kind    = TY_PRED_REFERENT,
                                   .name    = CORE_STRING_STATIC("referent"),
                                   .subject = operand_type(in, 0),
                                   .other   = open_field(in),
                                   .result  = in->dest->type},
                       why_of(in, 37));
            return nullptr;
        }
        return atomic_item(g, in, referent(operand_type(in, 0)));
    case IR_OP_ALLOC:
    case IR_OP_ALLOCA:
    case IR_OP_ALLOC_USING:
        if (in->dest && referent(in->dest->type)) {
            ty_unify(g->s, referent(in->dest->type), operand_type(in, 0),
                     why_of(in, 2));
        }
        return nullptr;
    case IR_OP_GLOBAL_STORE:
        if (in->n_operands == 2 && operand_type(in, 0)
            && operand_type(in, 1)) {
            ty_unify(g->s, operand_type(in, 0), operand_type(in, 1),
                     why_of(in, 2));
        }
        return nullptr;
    case IR_OP_STORE: {
        th_type *slot = open_field(in)
                          ? nullptr
                          : atomic_item(g, in, referent(operand_type(in, 0)));

        if (slot) {
            ty_unify(g->s, slot, operand_type(in, 1), why_of(in, 2));
        }
        else if (!ir_instr_has_mod(in, CORE_STRING_STATIC("atomic"))
                 && operand_type(in, 1)) {
            ty_require(g->s,
                       (ty_pred_t){.kind    = TY_PRED_REFERENT,
                                   .name    = CORE_STRING_STATIC("referent"),
                                   .subject = operand_type(in, 0),
                                   .other   = open_field(in),
                                   .result  = operand_type(in, 1)},
                       why_of(in, 37));
        }
        else {
            skip(g, in);
        }
        return nullptr;
    }
    case IR_OP_INDEX: {
        th_type *item = element(operand_type(in, 0));

        if (item && in->dest && referent(in->dest->type)) {
            ty_unify(g->s, referent(in->dest->type), item, why_of(in, 2));
        }
        else if (in->dest && referent(in->dest->type)
                 && referent(operand_type(in, 0))
                 && th_kind_of(th_resolve(referent(operand_type(in, 0))))
                        == TH_VAR) {
            ty_require(g->s,
                       (ty_pred_t){.kind       = TY_PRED_ELEMENT,
                                   .name       = CORE_STRING_STATIC("Index"),
                                   .subject    = referent(operand_type(in, 0)),
                                   .result     = referent(in->dest->type),
                                   .positional = true},
                       why_of(in, 2));
        }
        else {
            skip(g, in);
        }
        return nullptr;
    }
    case IR_OP_FIELD: {
        th_type *member = field_result(in);

        if (member && in->dest && referent(in->dest->type)) {
            ty_unify(g->s, referent(in->dest->type), member, why_of(in, 2));
        }
        else if (in->dest && referent(in->dest->type) && in->n_operands == 2
                 && referent(operand_type(in, 0))
                 && in->operands[1].kind == IR_OPD_NAME) {
            ty_require(g->s,
                       (ty_pred_t){.kind    = TY_PRED_FIELD,
                                   .name    = in->operands[1].name,
                                   .subject = referent(operand_type(in, 0)),
                                   .result  = referent(in->dest->type)},
                       why_of(in, 30));
        }
        else {
            skip(g, in);
        }
        return nullptr;
    }
    case IR_OP_EXTRACT:
        if (in->dest && in->n_operands == 2
            && in->operands[1].kind == IR_OPD_NAME) {
            ty_require(g->s,
                       (ty_pred_t){.kind    = TY_PRED_FIELD,
                                   .name    = in->operands[1].name,
                                   .subject = operand_type(in, 0),
                                   .result  = in->dest->type},
                       why_of(in, 30));
        }
        else {
            skip(g, in);
        }
        return nullptr;
    case IR_OP_SEQUENCE_LENGTH:
        if (in->n_operands == 1) {
            ty_unify(g->s, operand_type(in, 0),
                     th_array(g->types, th_var(g->types)), why_of(in, 29));
        }
        return nullptr;
    case IR_OP_EXTRACT_INDEX:
        if (in->dest && in->n_operands == 2
            && in->operands[1].kind == IR_OPD_CONST) {
            ty_require(g->s,
                       (ty_pred_t){.kind       = TY_PRED_FIELD,
                                   .positional = true,
                                   .index      = (uint32_t)in->operands[1]
                                                     .lit.i.mag,
                                   .subject    = operand_type(in, 0),
                                   .result     = in->dest->type},
                       why_of(in, 30));
        }
        else {
            skip(g, in);
        }
        return nullptr;
    case IR_OP_AGGREGATE:
        return aggregate_result(g, in);
    case IR_OP_SELECT:
        if (in->n_operands >= 3) {
            ty_unify(g->s, operand_type(in, 1), operand_type(in, 2),
                     why_of(in, 4));
        }
        return operand_type(in, 1);
    case IR_OP_PHI:
        return phi_result(g, in);
    case IR_OP_CALL:
        return call_result(g, in);
    case IR_OP_DISPATCH:
        dispatch_constraints(g, in);
        return nullptr;
    case IR_OP_CALL_OUTPUT: {
        ir_value_t *call = ir_operand_value(&in->operands[0]);

        if (call && call->def && call->def->op == IR_OP_DISPATCH
            && call->def->n_operands > 1) {
            return operand_type(call->def, 1);
        }

        size_t            ordinal = 0;
        const ir_instr_t *source  = ir_call_output_source(in, &ordinal);
        th_type          *output  = source ? ir_call_output_type(
                                                 ir_call_function_type(source),
                                                 ordinal)
                                           : nullptr;

        if (output && th_is_closed(g->types, output)) {
            return output;
        }
        if (output && in->dest) {
            ty_require(g->s,
                       (ty_pred_t){.kind       = TY_PRED_FLOW,
                                   .name       = CORE_STRING_STATIC("flow"),
                                   .subject    = output,
                                   .other      = in->dest->type,
                                   .positional = true},
                       why_of(in, 40));
            return nullptr;
        }
        skip(g, in);
        return nullptr;
    }
    case IR_OP_RETURN:
        return_constraints(g, in);
        return nullptr;
    case IR_OP_FAIL:
        if (th_func_error(g->function->type)) {
            ty_unify(g->s, th_func_error(g->function->type),
                     operand_type(in, 0), why_of(in, 28));
        }
        return nullptr;
    case IR_OP_OPTION_GET:
        if (in->dest) {
            ty_unify(g->s, th_option(g->types, in->dest->type),
                     operand_type(in, 0), why_of(in, 11));
        }
        return nullptr;
    case IR_OP_OPTION_SOME:
        if (in->dest && th_option_item(th_resolve(in->dest->type))) {
            ty_unify(g->s, th_option_item(th_resolve(in->dest->type)),
                     operand_type(in, 0), why_of(in, 5));
        }
        return nullptr;
    case IR_OP_CONST:
        if (in->dest && in->n_operands
            && in->operands[0].kind == IR_OPD_CONST
            && !in->operands[0].lit.text.data
            && th_kind_of(th_resolve(in->dest->type)) == TH_VAR) {
            bool real = in->operands[0].lit.is_float;

            ty_require(g->s,
                       (ty_pred_t){.kind    = TY_PRED_IMPLEMENTS,
                                   .name    = real
                                                ? CORE_STRING_STATIC("Some_Float")
                                                : CORE_STRING_STATIC("Some_Integer"),
                                   .subject = in->dest->type},
                       why_of(in, real ? 27 : 20));
        }
        else if (in->dest && !in->n_operands
                 && ir_instr_const_bytes_hex(in).data
                 && th_kind_of(th_resolve(in->dest->type)) == TH_VAR) {
            ty_require(g->s,
                       (ty_pred_t){.kind    = TY_PRED_IMPLEMENTS,
                                   .name    = CORE_STRING_STATIC("String_Syntax"),
                                   .subject = in->dest->type},
                       why_of(in, 29));
        }
        return nullptr;
    case IR_OP_NIL:
    case IR_OP_OPTION_NONE:
    case IR_OP_CAST:
    case IR_OP_BITCAST:
    case IR_OP_NOMINAL_CAST:
        return nullptr;
    case IR_OP_BRANCH:
        ty_require(g->s,
                   (ty_pred_t){.kind    = TY_PRED_CONDITION,
                               .subject = operand_type(in, 0)},
                   why_of(in, 32));
        return nullptr;
    case IR_OP_JUMP:
    case IR_OP_SWITCH:
    case IR_OP_TRAP:
    case IR_OP_UNREACHABLE:
        return nullptr;
    default:
        *handled = false;
        return nullptr;
    }
}

static void constrain_instr(gen_t *g, ir_instr_t *in)
{
    bool     handled = false;
    th_type *result  = result_of(g, in, &handled);

    g->coverage->instructions++;
    if (!handled) {
        skip(g, in);
        return;
    }
    if (!result) {
        return;
    }
    if (in->dest_kind == IR_DEST_VALUE && in->dest) {
        ty_equate(g->s, in->dest->type, result, why_of(in, 2));
    }
    else if (in->dest_kind == IR_DEST_LOCAL && in->dest_local) {
        ty_unify(g->s, in->dest_local->type, result, why_of(in, 2));
    }
}

static void constrain_function(gen_t *g, ir_func_t *f)
{
    g->function = f;
    if (ir_span_known(f->span)) {
        ty_declare_rigid(g->s, f->type);
    }
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        for (uint32_t k = 0; k < f->blocks[b]->n_instrs; k++) {
            constrain_instr(g, f->blocks[b]->instrs[k]);
        }
    }
}

bool ty_constrain_group(ty_solver_t *s, const ir_group_pdg_t *g,
                        ty_coverage_t *coverage)
{
    if (!s || !g || !coverage) {
        return false;
    }

    gen_t gen = {.s = s, .types = ty_solver_types(s), .g = g,
                 .coverage = coverage};

    core_dict_init(&gen.schemes, nullptr, nullptr);

    for (uint32_t c = 0; c < ir_group_pdg_component_count(g); c++) {
        gen.component = c;
        for (uint32_t i = 0; i < ir_group_pdg_component_size(g, c); i++) {
            constrain_function(&gen, ir_group_pdg_component_at(g, c, i));
        }
        ty_solve(s);
        for (uint32_t i = 0; i < ir_group_pdg_component_size(g, c); i++) {
            ir_func_t *f = ir_group_pdg_component_at(g, c, i);

            core_dict_put(&gen.schemes, f, ty_generalize(s, f->type));
        }
    }
    core_dict_free(&gen.schemes);

    return ty_failure_count(s) == 0;
}
