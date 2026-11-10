#include "compiler_rewrite.h"

#include "cached_body.h"
#include "compiler_lower_emit.h"
#include "compiler_tu.h"
#include "compiler_types.h"
#include "flagset_value.h"
#include "storage_selection.h"
#include "ir/analyses.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "ir/pdg_memory.h"
#include "ir/vn.h"

typedef ir_value_t *n00bb_rewrite_value_ptr_t;
core_list_decl(n00bb_rewrite_value_ptr_t);

typedef struct {
    n00bb_lower_context_t *lower;
    core_translation_unit_t *unit;
    ir_ctx_t *ir_context;
    parse_grammar_t *grammar;
    ir_lower_context_t context;
    ir_block_t *scratch;
    uint64_t next_tail;
    const ir_pdg_t *pdg;
    core_list_t(n00bb_ir_instr_ptr_t) *retyped;
    core_dict_t *sources;
    n00bb_comparison_t *comparison;
    const n00bb_dispatch_index_t *dispatch;
    core_dict_t *requests;
    core_list_t(n00bb_ir_instr_ptr_t) *request_calls;
    core_list_t(n00bb_ir_instr_ptr_t) *pruned;
    core_dict_t *doomed;
    core_list_t(n00bb_ir_instr_ptr_t) *demotions;
    core_list_t(n00bb_rewrite_value_ptr_t) abandoned;
    bool writing;
    ir_value_t *payload_holder;
    ir_local_t *payload;
} n00bb_rewrite_t;

typedef n00bb_rewrite_t *n00bb_rewrite_ptr_t;

typedef enum : uint8_t {
    N00BB_ORDER_LESS,
    N00BB_ORDER_EQUAL,
    N00BB_ORDER_GREATER,
} n00bb_order_t;
core_list_decl(n00bb_rewrite_ptr_t);

typedef bool (*n00bb_sink_fn)(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                              uint32_t o, th_type *target);
typedef bool (*n00bb_site_fn)(n00bb_rewrite_t *rewrite, ir_instr_t *in);

static bool
n00bb_rewrite_open(n00bb_rewrite_t *rewrite, n00bb_link_analysis_t *analysis,
                   parse_grammar_t *grammar, core_translation_unit_t *unit,
                   ir_func_t *function)
{
    *rewrite = (n00bb_rewrite_t){
        .lower = n00bb_solved_lower_new(analysis, grammar, unit),
        .unit = unit,
        .ir_context = analysis->ir_context,
        .grammar = grammar,
        .scratch = ir_block_add(function,
                                CORE_STRING_STATIC("__n00bb_rewrite")),
        .abandoned = core_list_new(n00bb_rewrite_value_ptr_t),
    };
    rewrite->context = (ir_lower_context_t){
        .module = function->owner,
        .function = function,
        .block = rewrite->scratch,
        .diagnostics = attach_ir_diags(analysis->ir_context, nullptr, unit,
                                       nullptr),
        .user = rewrite->lower,
        .reachable = true,
    };
    return rewrite->lower && rewrite->scratch;
}

static bool
n00bb_rewrite_splice(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    while (rewrite->scratch->n_instrs)
        if (!ir_instr_move(rewrite->scratch->instrs[0], in->block, in->index))
            return false;
    return true;
}

static ir_block_t *
n00bb_rewrite_split(n00bb_rewrite_t *rewrite, ir_instr_t *in);

static bool
n00bb_rewrite_replace(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                      ir_value_t *value)
{
    if (!value) return false;
    ir_block_t *last = rewrite->context.block;
    if (last != rewrite->scratch) {
        ir_block_t *tail = n00bb_rewrite_split(rewrite, in);
        if (!tail || !n00bb_rewrite_splice(rewrite, in)) return false;
        rewrite->context.block = last;
        bool joined = n00bb_emit_jump(&rewrite->context, tail, in->span);
        rewrite->context.block = rewrite->scratch;
        rewrite->context.reachable = true;
        if (!joined) return false;
    }
    else if (!n00bb_rewrite_splice(rewrite, in)) return false;
    ir_value_replace_uses(rewrite->context.function, in->dest, value,
                          nullptr, nullptr);
    return ir_instr_erase(in);
}

static ir_value_t *
n00bb_operand_value(ir_instr_t *in, uint32_t o)
{
    ir_operand_t *operand = o < in->n_operands ? &in->operands[o] : nullptr;
    return !operand || ir_operand_local(operand) ? nullptr
        : operand->kind == IR_OPD_NAMED_ARG ? operand->named.value
        : ir_operand_value(operand);
}

static ir_instr_t *
n00bb_local_write(const ir_pdg_t *pdg, ir_instr_t *read, ir_local_t *local)
{
    ir_instr_t *write = nullptr;
    for (uint32_t i = 0; pdg && i < ir_pdg_in_count(pdg, read); i++) {
        const ir_dep_edge_t *edge = ir_pdg_in_at(pdg, read, i);
        if (edge->kind != IR_DEP_DATA || edge->via != local) continue;
        if (write && write != edge->from) return nullptr;
        write = edge->from;
    }
    return write && write->op == IR_OP_COPY && write->n_operands == 1
        ? write : nullptr;
}

static ir_instr_t *
n00bb_value_origin(const ir_pdg_t *pdg, ir_value_t *value)
{
    for (size_t depth = 0; value && value->def && depth < 64; depth++) {
        ir_instr_t *def = value->def;
        if (def->op == IR_OP_AGGREGATE || def->op == IR_OP_CALL)
            return def->dest == value ? def : nullptr;
        if (def->op != IR_OP_COPY || def->n_operands != 1) return nullptr;
        ir_local_t *local = ir_operand_local(&def->operands[0]);
        ir_instr_t *write = local ? n00bb_local_write(pdg, def, local)
                                  : nullptr;
        if (local && !write) return nullptr;
        value = ir_operand_value(local ? &write->operands[0]
                                       : &def->operands[0]);
    }
    return nullptr;
}

static void
n00bb_retype_closure(n00bb_rewrite_t *rewrite, ir_instr_t *aggregate,
                     th_type *target)
{
    th_ctx *types = ir_module_types(rewrite->context.module);
    core_list_t(n00bb_ir_instr_ptr_t) work = core_list_new(n00bb_ir_instr_ptr_t);
    core_dict_t seen;
    core_dict_init(&seen, nullptr, nullptr);
    core_list_push(work, aggregate);
    while (work.len) {
        ir_instr_t *at = work.data[--work.len];
        bool visited = false;
        core_dict_get(&seen, at, &visited);
        if (visited) continue;
        core_dict_put(&seen, at, at);
        if (at->dest_kind == IR_DEST_LOCAL && at->dest_local)
            at->dest_local->type = target;
        else if (at->dest && at->op == IR_OP_ADDRESS_OF) {
            uint8_t attrs = 0;
            th_ind_attrs(th_resolve(at->dest->type), &attrs);
            at->dest->type = th_ind(types, attrs, target);
            continue;
        }
        else if (at->dest)
            at->dest->type = target;
        for (uint32_t i = 0; i < ir_pdg_out_count(rewrite->pdg, at); i++) {
            const ir_dep_edge_t *edge = ir_pdg_out_at(rewrite->pdg, at, i);
            if (edge->kind == IR_DEP_DATA && (edge->to->op == IR_OP_COPY
                                              || edge->to->op
                                                  == IR_OP_ADDRESS_OF))
                core_list_push(work, edge->to);
        }
    }
    core_dict_free(&seen);
    core_list_free(work);
}

static bool n00bb_plan_operand(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                               uint32_t o, th_type *target);
static th_type *n00bb_literal_element(n00bb_rewrite_t *rewrite,
                                      th_type *container);
static void n00bb_report_unknown_method(n00bb_rewrite_t *rewrite,
                                        ir_instr_t *in, core_string_t method,
                                        ir_value_t *receiver,
                                        th_type *receiver_type);
static bool n00bb_rewrite_members(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                                  n00bb_sink_fn visit);

static bool
n00bb_retype_literal(n00bb_rewrite_t *rewrite, ir_instr_t *aggregate,
                     th_type *target)
{
    th_type *literal = th_resolve(aggregate->dest->type);
    th_type *shape = n00bb_record_shape(tu_state(rewrite->unit), target);
    size_t count = th_kind_of(literal) == TH_RECORD
        ? th_record_len(literal) : 0;
    if (!count || !shape || th_kind_of(shape) != TH_RECORD
        || th_record_len(shape) != count || aggregate->n_operands != count)
        return false;
    ir_operand_t *ordered = core_alloc_array(ir_operand_t, count);
    bool complete = true;
    for (size_t i = 0; complete && i < count; i++) {
        core_string_t name = th_record_member(shape, i)->name;
        size_t j = 0;
        while (j < count
               && !core_string_eq(th_record_member(literal, j)->name, name))
            j++;
        complete = j < count;
        if (complete) ordered[i] = aggregate->operands[j];
    }
    for (size_t i = 0; complete && i < count; i++)
        aggregate->operands[i] = ordered[i];
    if (complete) ir_instr_uses_changed(aggregate);
    core_free(ordered);
    if (!complete) return false;
    n00bb_retype_closure(rewrite, aggregate, target);
    core_list_push(*rewrite->retyped, aggregate);
    return n00bb_rewrite_members(rewrite, aggregate, n00bb_plan_operand);
}

static th_type *
n00bb_lifted_item(th_type *target, const ir_value_t *value)
{
    th_type *item = target ? th_option_item(th_resolve(target)) : nullptr;
    return item && value && !th_option_item(th_resolve(value->type))
        && !th_is_nil(th_resolve(value->type)) ? item : nullptr;
}

static ir_instr_t *
n00bb_pending_consume(const ir_instr_t *call, ir_value_t **address)
{
    ir_value_t *consumed = call && call->n_operands == 2
        ? ir_operand_value(&call->operands[1]) : nullptr;
    ir_instr_t *consume = consumed && consumed->def
        && consumed->def->op == IR_OP_CONSUME ? consumed->def : nullptr;
    *address = consume ? ir_operand_value(&consume->operands[0]) : nullptr;
    return consume;
}

static bool
n00bb_plan_operand(n00bb_rewrite_t *rewrite, ir_instr_t *in, uint32_t o,
                   th_type *target)
{
    ir_value_t *value = n00bb_operand_value(in, o);
    th_type *item = n00bb_lifted_item(target, value);
    if (item) target = item;
    bool variant = false;
    th_named_info(target ? th_resolve(target) : nullptr, nullptr, &variant,
                  nullptr);
    if (!value || !target
        || ir_type_eq(rewrite->context.module, value->type, target))
        return true;
    ir_instr_t *origin = n00bb_value_origin(rewrite->pdg, value);
    ir_func_t *callee = ir_call_function(origin);
    if (origin && origin->op == IR_OP_AGGREGATE && variant)
        core_dict_put(rewrite->sources, value, origin);
    else if (origin && origin->op == IR_OP_AGGREGATE)
        n00bb_retype_literal(rewrite, origin, target);
    else if (callee && !variant && callee->n_blocks
             && th_kind_of(th_resolve(value->type)) == TH_RECORD) {
        bool seen = false;
        th_type *wanted = core_dict_get(rewrite->requests, callee, &seen);
        if (!seen) core_list_push(*rewrite->request_calls, origin);
        core_dict_put(rewrite->requests, callee,
            !seen || (wanted && ir_type_eq(rewrite->context.module, wanted,
                                           target)) ? target : nullptr);
    }
    return true;
}

static ir_value_t *
n00bb_rewrite_variant_arm(n00bb_rewrite_t *rewrite, ir_instr_t *aggregate,
                          th_type *target, ir_span_t span)
{
    ir_lower_context_t *context = &rewrite->context;
    th_type *literal = th_resolve(aggregate->dest->type);
    th_type *arms = ir_type_definition(context->module, target);
    if (th_kind_of(literal) == TH_RECORD && th_record_len(literal) > 1) {
        core_buffer_t named = {0};
        for (size_t i = 0; i < th_record_len(literal); i++) {
            const th_member *member = th_record_member(literal, i);
            core_buffer_printf(&named, "%s`%.*s`",
                !i ? "" : i + 1 == th_record_len(literal) ? " and " : ", ",
                (int)member->name.u8_bytes, member->name.data);
        }
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(rewrite->unit);
        if (n00bb_report(diagnostics, rewrite->unit,
                N00B_DIAG_VARIANT_LITERAL_ARMS,
                n00bb_span_location(aggregate->span),
                N00B_TEXT_VARIANT_LITERAL_ARMS,
                N00BB_ARGS(ARG_TEXT(n00bb_name_leaf(th_print(
                               ir_module_types(context->module), target))),
                           ARG_TEXT(core_buffer_string(&named)))))
            n00bb_report_help(diagnostics, N00B_TEXT_KEEP_ONE_ARM,
                              N00BB_NO_ARGS);
        core_buffer_release(&named);
        return nullptr;
    }
    const th_member *chosen = th_kind_of(literal) == TH_RECORD
        && th_record_len(literal) == 1 && aggregate->n_operands == 1
        ? th_record_member(literal, 0) : nullptr;
    th_type *payload_type = chosen && arms
        ? th_record_lookup(th_resolve(arms), chosen->name) : nullptr;
    ir_value_t *payload = payload_type
        ? n00bb_materialize(context, nullptr,
                            ir_operand_value(&aggregate->operands[0]),
                            payload_type, span)
        : nullptr;
    ir_instr_t *make = payload
        ? ir_lower_emit_instruction(context, IR_OP_VARIANT_MAKE, span)
        : nullptr;
    return make && ir_lower_set_temp_dest(context, make,
            CORE_STRING_STATIC("__n00bb_variant_"), target)
        && ir_instr_operand_name(make, chosen->name)
        && ir_instr_operand_value(make, payload)
        ? make->dest : nullptr;
}

static bool
n00bb_rewrite_arm_store(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        bool *handled)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *address = in->n_operands == 2
        ? ir_operand_value(&in->operands[0]) : nullptr;
    ir_instr_t *field = address ? address->def : nullptr;
    ir_value_t *base = field && field->op == IR_OP_FIELD
        && field->n_operands == 2 && field->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&field->operands[0]) : nullptr;
    th_type *target = base && th_kind_of(th_resolve(base->type))
            == TH_INDIRECTION
        ? th_ind_referent(th_resolve(base->type)) : nullptr;
    bool variant = false;
    *handled = target && n00bb_case_member(rewrite->dispatch, target,
                                           field->operands[1].name, &variant)
        && variant;
    if (!*handled) return true;
    th_type *arms = ir_type_definition(context->module, target);
    th_type *payload_type = arms
        ? th_record_lookup(th_resolve(arms), field->operands[1].name)
        : nullptr;
    ir_value_t *payload = payload_type
        ? n00bb_materialize(context, nullptr,
                            ir_operand_value(&in->operands[1]), payload_type,
                            in->span)
        : nullptr;
    ir_instr_t *make = payload
        ? ir_lower_emit_instruction(context, IR_OP_VARIANT_MAKE, in->span)
        : nullptr;
    if (!make || !ir_lower_set_temp_dest(context, make,
            CORE_STRING_STATIC("__n00bb_variant_"), target)
        || !ir_instr_operand_name(make, field->operands[1].name)
        || !ir_instr_operand_value(make, payload)
        || !n00bb_rewrite_splice(rewrite, in))
        return false;
    in->operands[0] = (ir_operand_t){.kind = IR_OPD_VALUE, .value = base};
    in->operands[1] = (ir_operand_t){.kind = IR_OPD_VALUE,
                                     .value = make->dest};
    ir_instr_uses_changed(in);
    core_list_push(rewrite->abandoned, address);
    return true;
}

static ir_value_t *
n00bb_convert(n00bb_rewrite_t *rewrite, ir_instr_t *in, ir_value_t *value,
              th_type *target)
{
    if (ir_type_eq(rewrite->context.module, value->type, target))
        return value;
    if (th_is_nil(th_resolve(value->type))
        && n00bb_declared_flagset(tu_state(rewrite->unit)->link_analysis,
                                  target)) {
        ir_instr_t *empty = ir_lower_emit_instruction(&rewrite->context,
            IR_OP_BITSET_CONST, in->span);
        return empty && ir_lower_set_temp_dest(&rewrite->context, empty,
                CORE_STRING_STATIC("__n00bb_"), target)
            && ir_instr_operand_name(empty, CORE_STRING_STATIC("nil"))
            && ir_instr_operand_uint(empty, 0) ? empty->dest : nullptr;
    }
    th_type *atomic = n00bb_atomic_element_type(target);
    if (atomic && !n00bb_atomic_element_type(value->type)) {
        ir_value_t *inner = n00bb_convert(rewrite, in, value, atomic);
        return inner ? n00bb_emit_atomic_wrap(&rewrite->context, inner,
                                              target, in->span)
                     : nullptr;
    }
    th_type *item = n00bb_lifted_item(target, value);
    if (item) {
        if (ir_annot_get(&in->annots, N00BB_COMPARISON_KEY)
            && th_kind_of(th_resolve(item)) != TH_INDIRECTION) {
            th_ctx *types = ir_module_types(rewrite->context.module);
            core_diagnostics_t *diagnostics =
                core_translation_unit_diagnostics(rewrite->unit);
            n00bb_report(diagnostics, rewrite->unit,
                N00B_DIAG_OPTION_COMPARED_TO_VALUE,
                n00bb_span_location(in->span),
                N00B_TEXT_OPTION_COMPARED_TO_VALUE,
                N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, target)),
                           ARG_TEXT(n00bb_print_type(types, value->type))));
            n00bb_report_help(diagnostics,
                N00B_TEXT_TEST_THE_OPTION_FOR_NIL_FIRST, N00BB_NO_ARGS);
            return nullptr;
        }
        ir_value_t *inner = n00bb_convert(rewrite, in, value, item);
        return inner ? n00bb_materialize(&rewrite->context, nullptr, inner,
                                         target, in->span)
                     : nullptr;
    }
    bool variant = false;
    th_named_info(th_resolve(target), nullptr, &variant, nullptr);
    ir_instr_t *literal = variant
        ? core_dict_get(rewrite->sources, value, nullptr) : nullptr;
    th_type *shape = !variant && th_kind_of(th_resolve(value->type))
            == TH_RECORD
        ? n00bb_record_shape(tu_state(rewrite->unit), target) : nullptr;
    if (shape && !literal) {
        ir_lower_result_t cast = n00bb_emit_cast(&rewrite->context,
            rewrite->grammar, nullptr, &value, target, false, in->span);
        return cast.status == IR_LOWER_VALUE ? cast.value : nullptr;
    }
    return literal
        ? n00bb_rewrite_variant_arm(rewrite, literal, target, in->span)
        : n00bb_materialize(&rewrite->context, nullptr, value, target,
                            in->span);
}

static bool
n00bb_rewrite_operand(n00bb_rewrite_t *rewrite, ir_instr_t *in, uint32_t o,
                      th_type *target)
{
    ir_value_t *value = n00bb_operand_value(in, o);
    if (!value || !target
        || ir_type_eq(rewrite->context.module, value->type, target))
        return true;
    ir_value_t *converted = n00bb_convert(rewrite, in, value, target);
    if (!converted || !n00bb_rewrite_splice(rewrite, in)) return false;
    ir_operand_t *operand = &in->operands[o];
    if (operand->kind == IR_OPD_NAMED_ARG) operand->named.value = converted;
    else operand->value = converted;
    ir_instr_use_added(in, o);
    return true;
}

static bool
n00bb_rewrite_members(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                      n00bb_sink_fn visit)
{
    th_type *dest = th_resolve(in->dest->type);
    th_type *shape = n00bb_record_shape(tu_state(rewrite->unit), dest);
    bool record = shape && th_record_len(shape) == in->n_operands;
    bool tuple = th_builtin_kind_of(dest) == TH_BUILTIN_TUPLE
        && th_named_nargs(dest) == in->n_operands;
    ir_aggregate_form_t form = IR_AGGREGATE_PAIRS;
    bool formed = ir_instr_aggregate_form(in, &form);
    th_type *item = formed && th_kind_of(dest) == TH_NAMED
        ? n00bb_literal_element(rewrite, dest) : nullptr;
    th_type *element = !formed || form != IR_AGGREGATE_ELEMENTS ? nullptr
        : th_kind_of(dest) == TH_SEQUENCE ? th_seq_elem(dest) : item;
    th_type *pair = formed && form == IR_AGGREGATE_PAIRS && item
        && th_builtin_kind_of(item) == TH_BUILTIN_TUPLE
        && th_named_nargs(item) == 2 && !(in->n_operands & 1u)
        ? item : nullptr;
    bool ok = true;
    for (uint32_t i = 0; ok && pair && i < in->n_operands; i++)
        ok = visit(rewrite, in, i, th_named_arg(pair, i & 1u)->type);
    if (pair) return ok;
    for (uint32_t i = 0; ok && (record || tuple || element)
         && i < in->n_operands; i++)
        ok = visit(rewrite, in, i, element ? element
            : record ? th_record_member(shape, i)->type
                     : th_named_arg(dest, i)->type);
    return ok;
}

static bool
n00bb_rewrite_return(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                     n00bb_sink_fn visit)
{
    th_type *type = th_resolve(rewrite->context.function->type);
    uint32_t outputs = 0;
    for (size_t i = 0; i < th_func_nparams(type); i++)
        outputs += th_mode_produces_output(th_func_param(type, i)->mode);
    uint32_t at = in->n_operands > outputs ? 1 : 0;
    bool ok = !at || visit(rewrite, in, 0,
                                           th_func_ret(type));
    for (size_t i = 0; ok && i < th_func_nparams(type); i++) {
        const th_param *param = th_func_param(type, i);
        if (th_mode_produces_output(param->mode) && at < in->n_operands)
            ok = visit(rewrite, in, at++, param->type);
    }
    return ok;
}

static bool
n00bb_rewrite_arguments(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        n00bb_sink_fn visit)
{
    ir_value_t *callee = ir_operand_value(&in->operands[0]);
    ir_func_t *target = callee && callee->kind == IR_VAL_FUNC
        ? callee->owner : nullptr;
    th_type *signature = ir_instr_call_signature(in);
    if (!signature && target) signature = target->type;
    signature = signature ? th_resolve(signature) : nullptr;
    if (!target || !signature || th_kind_of(signature) != TH_FUNCTION)
        return true;
    uint32_t position = 0;
    bool ok = true;
    for (uint32_t o = 1; ok && o < in->n_operands; o++) {
        const ir_operand_t *operand = &in->operands[o];
        if (operand->kind == IR_OPD_NAMED_ARG) {
            const th_kwparam *keyword = th_func_kw_lookup(signature,
                                                          operand->named.name);
            ok = !keyword || visit(rewrite, in, o,
                                                   keyword->type);
            continue;
        }
        uint32_t index = UINT32_MAX;
        ir_value_t *param = ir_call_param(target, operand, &position, &index);
        const th_param *formal = index != UINT32_MAX
            ? th_func_param(signature, index)
            : param && param->is_rest ? th_func_rest(signature) : nullptr;
        if (formal && th_mode_is_in(formal->mode)
            && operand->kind != IR_OPD_FRESH)
            ok = visit(rewrite, in, o, formal->type);
    }
    return ok;
}

static bool
n00bb_rewrite_sinks(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                    n00bb_sink_fn visit)
{
    switch (in->op) {
    case IR_OP_STORE: {
        ir_value_t *pointer = ir_operand_value(&in->operands[0]);
        th_type *referent = pointer
            ? th_resolve(th_ind_referent(th_resolve(pointer->type))) : nullptr;
        th_type *atomic = referent ? n00bb_atomic_element_type(referent)
                                   : nullptr;
        return !referent || visit(rewrite, in, 1, atomic ? atomic : referent);
    }
    case IR_OP_GLOBAL_STORE: {
        ir_value_t *global = ir_operand_value(&in->operands[0]);
        return in->n_operands != 2 || !global || !global->global
            || visit(rewrite, in, 1, global->global->type);
    }
    case IR_OP_COPY:
        return in->dest_kind != IR_DEST_LOCAL || !in->dest_local
            || visit(rewrite, in, 0, in->dest_local->type);
    case IR_OP_AGGREGATE:
        return !in->dest || n00bb_rewrite_members(rewrite, in, visit);
    case IR_OP_RETURN:
        return n00bb_rewrite_return(rewrite, in, visit);
    case IR_OP_CALL:
        return !in->n_operands || n00bb_rewrite_arguments(rewrite, in, visit);
    case IR_OP_DISPATCH: {
        th_type *signature = ir_instr_call_signature(in);
        signature = signature ? th_resolve(signature) : nullptr;
        bool ok = true;
        for (uint32_t o = 1; ok && signature
             && th_kind_of(signature) == TH_FUNCTION && o < in->n_operands
             && o - 1 < th_func_nparams(signature); o++) {
            const th_param *formal = th_func_param(signature, o - 1);
            if (th_mode_is_in(formal->mode))
                ok = visit(rewrite, in, o, formal->type);
        }
        return ok;
    }
    case IR_OP_ADD: case IR_OP_SUB: case IR_OP_MUL: case IR_OP_DIV:
    case IR_OP_REM: case IR_OP_EQ: case IR_OP_NE: case IR_OP_LT:
    case IR_OP_LE: case IR_OP_GT: case IR_OP_GE: {
        ir_value_t *left = n00bb_operand_value(in, 0);
        return in->n_operands != 2 || !left || !is_integer_type(left->type)
            || visit(rewrite, in, 1, left->type);
    }
    default:
        return true;
    }
}

static ir_value_t *
n00bb_rewrite_object_field(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_lower_context_t *context = &rewrite->context;
    th_type *member = in->dest->type;
    ir_value_t *base = n00bb_reference_view(context,
        ir_operand_value(&in->operands[0]), in->span);
    ir_instr_t *address = base
        ? ir_lower_emit_instruction(context, IR_OP_FIELD, in->span) : nullptr;
    bool addressed = address && ir_lower_set_temp_dest(context, address,
            CORE_STRING_STATIC("__n00bb_field_address_"),
            th_ind(ir_module_types(context->module), 0, member))
        && ir_instr_operand_value(address, base)
        && ir_instr_operand_name(address, in->operands[1].name);
    ir_instr_t *load = addressed
        ? ir_lower_emit_instruction(context, IR_OP_LOAD, in->span) : nullptr;
    return load && ir_lower_set_temp_dest(context, load,
            CORE_STRING_STATIC("__n00bb_field_"), member)
        && ir_instr_operand_value(load, address->dest)
        && n00bb_set_reference_access(rewrite->ir_context, load->dest,
                                      member)
        ? load->dest : nullptr;
}

static bool
n00bb_rewrite_object_field_address(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *base = ir_operand_value(&in->operands[0]);
    th_type *object = th_resolve(th_ind_referent(th_resolve(base->type)));
    ir_value_t *reference = n00bb_emit_indirect_read(context, base, object,
                                                     in->span);
    ir_value_t *view = reference
        ? n00bb_reference_view(context, reference, in->span) : nullptr;
    if (!view || !n00bb_rewrite_splice(rewrite, in)) return false;
    in->operands[0].value = view;
    ir_instr_use_added(in, 0);
    return true;
}

static ir_value_t *
n00bb_rewrite_cast(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                   const ir_annot_t *open)
{
    ir_value_t *operand = ir_operand_value(&in->operands[0]);
    ir_lower_result_t cast = n00bb_emit_cast(&rewrite->context,
        rewrite->grammar, nullptr, &operand, in->dest->type, open->b,
        in->span);
    return cast.status == IR_LOWER_VALUE ? cast.value : nullptr;
}

static ir_block_t *
n00bb_rewrite_split(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    char buffer[64];
    for (;;) {
        core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
            "__n00bb_rewritten_%llu",
            (unsigned long long)rewrite->next_tail++);
        if (!ir_block_find(rewrite->context.function, name))
            return ir_block_split(in->block, in->index + 1, name);
    }
}

static bool
n00bb_rewrite_finalize(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_value_t *address = nullptr;
    ir_instr_t *consume = n00bb_pending_consume(in, &address);
    th_type *type = address
        ? th_ind_referent(th_resolve(address->type)) : nullptr;
    ir_block_t *block = in->block;
    ir_span_t span = in->span;
    ir_block_t *tail = type ? n00bb_rewrite_split(rewrite, in) : nullptr;
    if (!tail || !ir_instr_erase(in) || !ir_instr_erase(consume))
        return false;
    ir_lower_context_t *context = &rewrite->context;
    context->block = block;
    bool ok = !n00bb_type_may_finalize(rewrite->unit, type)
        || n00bb_emit_finalize_value(context, address, type, span, 0);
    ok = ok && n00bb_emit_jump(context, tail, span);
    context->block = rewrite->scratch;
    context->reachable = true;
    return ok;
}

static bool
n00bb_open_finalize_call(const ir_instr_t *in)
{
    ir_func_t *target = ir_call_function(in);
    return target && core_string_starts_with(target->name,
        CORE_STRING_STATIC("__n00bb_finalize_pending#"));
}

static bool
n00bb_rewrite_overflow(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                       ir_value_t *operand)
{
    static const core_string_t fits[] = {
        CORE_STRING_STATIC("wrap"), CORE_STRING_STATIC("trap"),
        CORE_STRING_STATIC("sat"), CORE_STRING_STATIC("undef"),
    };
    for (size_t i = 0; i < sizeof(fits) / sizeof(fits[0]); i++)
        if (ir_instr_has_mod(in, fits[i])) return true;
    ir_machine_t result = ir_machine_of(rewrite->context.module,
                                        in->dest->type);
    ir_machine_t source = ir_machine_of(rewrite->context.module,
                                        operand->type);
    bool integer = result.class == IR_CLASS_SINT
        || result.class == IR_CLASS_UINT || source.class == IR_CLASS_SINT
        || source.class == IR_CLASS_UINT;
    if (!integer) return true;
    if (in->op == IR_OP_CAST)
        return n00bb_machine_promotes(source, result)
            || ir_instr_add_mod(in, CORE_STRING_STATIC("trap"));
    return n00bb_trap_integer_arithmetic(in,
        result.class == IR_CLASS_SINT || source.class == IR_CLASS_SINT
            ? IR_CLASS_SINT : IR_CLASS_UINT);
}

static parse_sym_entry_t *const *
n00bb_rewrite_comparison(n00bb_rewrite_t *rewrite, th_type *type)
{
    return n00bb_comparison_members(tu_state(rewrite->unit)->link_analysis,
                                    rewrite->comparison, type);
}

static ir_value_t *
n00bb_rewrite_member(n00bb_rewrite_t *rewrite, th_type *type,
                     n00bb_order_t order, ir_span_t span)
{
    return n00bb_emit_enum_member(&rewrite->context,
        n00bb_rewrite_comparison(rewrite, type)[order], span);
}

static int
n00bb_order_tested(const ir_instr_t *test, const ir_value_t *order)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("less"), CORE_STRING_STATIC("equal"),
        CORE_STRING_STATIC("greater"),
    };
    if (test->op != IR_OP_EQ || test->n_operands != 2) return -1;
    ir_value_t *first = ir_operand_value(&test->operands[0]);
    ir_value_t *second = ir_operand_value(&test->operands[1]);
    ir_value_t *member = first == order ? second
        : second == order ? first : nullptr;
    const ir_instr_t *constant = member && member->kind == IR_VAL_INSTR
        ? member->def : nullptr;
    if (!constant || constant->op != IR_OP_ENUM_CONST
        || !constant->n_operands) return -1;
    for (int i = 0; i < 3; i++)
        if (core_string_eq(constant->operands[0].name, names[i])) return i;
    return -1;
}

static int
n00bb_value_order_tested(const ir_value_t *value, const ir_value_t *order)
{
    return value && value->kind == IR_VAL_INSTR && value->def
        ? n00bb_order_tested(value->def, order) : -1;
}

static bool
n00bb_true_constant(const ir_value_t *value)
{
    const ir_instr_t *constant = value && value->kind == IR_VAL_INSTR
        ? value->def : nullptr;
    return constant && constant->op == IR_OP_CONST && constant->n_operands
        && constant->operands[0].kind == IR_OPD_CONST
        && constant->operands[0].lit.i.mag == 1
        && !constant->operands[0].lit.i.neg;
}

static ir_op_t
n00bb_inclusive_order(const ir_instr_t *select, const ir_value_t *order)
{
    if (select->op != IR_OP_SELECT || select->n_operands != 3
        || !n00bb_true_constant(ir_operand_value(&select->operands[1]))
        || n00bb_value_order_tested(ir_operand_value(&select->operands[2]),
                                    order) != N00BB_ORDER_EQUAL)
        return IR_OP_COUNT;
    int strict = n00bb_value_order_tested(
        ir_operand_value(&select->operands[0]), order);
    return strict == N00BB_ORDER_LESS ? IR_OP_LE
        : strict == N00BB_ORDER_GREATER ? IR_OP_GE : IR_OP_COUNT;
}

static bool
n00bb_rewrite_order_tests(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                          ir_value_t *left, ir_value_t *right)
{
    static const ir_op_t direct[] = {IR_OP_LT, IR_OP_EQ, IR_OP_GT};
    ir_func_t *function = rewrite->context.function;
    core_list_t(n00bb_ir_instr_ptr_t) tests =
        core_list_new(n00bb_ir_instr_ptr_t);
    core_list_t(n00bb_ir_instr_ptr_t) inclusive =
        core_list_new(n00bb_ir_instr_ptr_t);
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *test = function->blocks[b]->instrs[i];
            if (test != in && n00bb_order_tested(test, in->dest) >= 0)
                core_list_push(tests, test);
            else if (n00bb_inclusive_order(test, in->dest) != IR_OP_COUNT)
                core_list_push(inclusive, test);
        }
    bool ok = true;
    for (size_t i = 0; ok && i < inclusive.len; i++) {
        ir_instr_t *select = inclusive.data[i];
        ir_value_t *value = n00bb_emit_binary_ir(&rewrite->context,
            n00bb_inclusive_order(select, in->dest), select->dest->type,
            left, right, select->span);
        ok = value != nullptr;
        if (ok) {
            ir_value_replace_uses(function, select->dest, value, nullptr,
                                  nullptr);
            ok = ir_instr_erase(select);
        }
    }
    for (size_t i = 0; ok && i < tests.len; i++) {
        ir_instr_t *test = tests.data[i];
        if (!ir_value_has_uses(function, test->dest)) {
            ok = ir_instr_erase(test);
            continue;
        }
        ir_value_t *value = n00bb_emit_binary_ir(&rewrite->context,
            direct[n00bb_order_tested(test, in->dest)], test->dest->type,
            left, right, test->span);
        ok = value != nullptr;
        if (ok) {
            ir_value_replace_uses(function, test->dest, value, nullptr,
                                  nullptr);
            ok = ir_instr_erase(test);
        }
    }
    core_list_free(inclusive);
    core_list_free(tests);
    return ok;
}

static ir_value_t *
n00bb_rewrite_three_way(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        ir_value_t *left, ir_value_t *right)
{
    ir_lower_context_t *context = &rewrite->context;
    if (in->dest_kind == IR_DEST_VALUE) {
        if (!n00bb_rewrite_order_tests(rewrite, in, left, right))
            return nullptr;
        if (!ir_value_has_uses(context->function, in->dest)) return in->dest;
    }
    return n00bb_emit_three_way(context, in->dest->type,
        n00bb_rewrite_comparison(rewrite, in->dest->type), left, right,
        in->span);
}

static ir_value_t *
n00bb_rewrite_shift_count(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                          th_type *receiver, ir_value_t *count)
{
    ir_lower_context_t *context = &rewrite->context;
    th_type *target = n00bb_shift_count_type(ir_module_types(context->module),
                                             receiver);
    ir_machine_t machine = ir_machine_of(context->module, receiver);
    ir_abcd_range_t range = {0};
    bool fits = target && !ir_type_eq(context->module, count->type, target)
        && !(count->def && count->def->op == IR_OP_CONST)
        && n00bb_integer_range(context->function, count, in, &range)
        && range.seen.lo >= 0 && range.seen.hi < (ir_int_t)machine.width;
    if (!fits)
        return n00bb_lower_shift_count(context, nullptr, count, receiver,
                                       in->span);
    ir_instr_t *cast = ir_lower_emit_instruction(context, IR_OP_CAST,
                                                 in->span);
    return cast && ir_lower_set_temp_dest(context, cast,
            CORE_STRING_STATIC("__n00bb_"), target)
        && ir_instr_operand_value(cast, count)
        && ir_instr_add_mod(cast, CORE_STRING_STATIC("wrap"))
        ? cast->dest : nullptr;
}

static ir_value_t *
n00bb_rewrite_primitive(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        core_string_t method, ir_value_t *left,
                        ir_value_t *right)
{
    const n00bb_binary_operator_t *binary = right
        ? n00bb_binary_operator_method(method) : nullptr;
    if (binary && (binary->op == IR_OP_SHL || binary->op == IR_OP_SHR)
        && is_integer_type(left->type) && is_integer_type(right->type)) {
        right = n00bb_rewrite_shift_count(rewrite, in, left->type, right);
        if (!right) return nullptr;
    }
    else if (right && is_integer_type(left->type)
             && is_integer_type(right->type))
        right = n00bb_widen_integer_operand(&rewrite->context, right,
                                            left->type, in->span);
    if (right && core_string_eq(method, CORE_STRING_STATIC("__cmp")))
        return n00bb_rewrite_three_way(rewrite, in, left, right);
    return n00bb_emit_primitive(&rewrite->context, method, in->dest->type,
                                left, right, in->span);
}

static bool
n00bb_rewrite_splice_after(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    for (uint32_t at = in->index + 1; rewrite->scratch->n_instrs; at++)
        if (!ir_instr_move(rewrite->scratch->instrs[0], in->block, at))
            return false;
    return true;
}

static ir_value_t *
n00bb_rewrite_nil(n00bb_rewrite_t *rewrite, ir_span_t span)
{
    ir_instr_t *nil = ir_lower_emit_instruction(&rewrite->context,
                                                IR_OP_NIL, span);
    return nil && ir_lower_set_temp_dest(&rewrite->context, nil,
            CORE_STRING_STATIC("__n00bb_"),
            th_nil(ir_module_types(rewrite->context.module)))
        ? nil->dest : nullptr;
}

static ir_value_t *
n00bb_element_address(ir_lower_context_t *context, ir_value_t *base,
                      ir_value_t *index, th_type *element, ir_span_t span)
{
    ir_instr_t *address = ir_lower_emit_instruction(context, IR_OP_INDEX,
                                                    span);
    if (!address || !ir_lower_set_temp_dest(context, address,
            CORE_STRING_STATIC("__n00bb_place_"),
            th_ind(ir_module_types(context->module), 0, element))
        || !ir_instr_operand_value(address, base)
        || !ir_instr_operand_value(address, index))
        return nullptr;
    ir_shape_t *shape = ir_value_shape(base);
    ir_shape_t *suffix = shape && th_kind_of(th_resolve(element)) == TH_SEQUENCE
        ? ir_shape_suffix(ir_module_ctx(context->module), shape, 1)
        : nullptr;
    return !suffix || ir_value_set_shape(address->dest, suffix)
        ? address->dest : nullptr;
}

static ir_value_t *
n00bb_rewrite_place(n00bb_rewrite_t *rewrite, ir_value_t *value,
                    ir_span_t span);

static ir_value_t *
n00bb_rewrite_payload_place(n00bb_rewrite_t *rewrite, ir_value_t *value,
                            ir_span_t span)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_instr_t *def = value->def;
    ir_value_t *holder = rewrite->writing && !rewrite->payload
        ? n00bb_rewrite_place(rewrite, ir_operand_value(&def->operands[0]),
                              span)
        : nullptr;
    char buffer[48];
    ir_local_t *local = holder ? ir_local_add(context->function,
        n00bb_format_name(buffer, sizeof(buffer), "__n00bb_payload_%llu",
                          (unsigned long long)context->next_value_id++),
        value->type) : nullptr;
    ir_instr_t *copy = local
        ? ir_lower_emit_instruction(context, IR_OP_COPY, span) : nullptr;
    ir_instr_t *take = copy && ir_instr_set_dest_local(copy, local)
            && ir_instr_operand_value(copy, value)
        ? ir_lower_emit_instruction(context, IR_OP_ADDRESS_OF, span)
        : nullptr;
    if (!take || !ir_lower_set_temp_dest(context, take,
            CORE_STRING_STATIC("__n00bb_place_"),
            th_ind(ir_module_types(context->module), 0, local->type))
        || !ir_instr_operand_local(take, local)) return nullptr;
    rewrite->payload_holder = holder;
    rewrite->payload = local;
    return take->dest;
}

static bool
n00bb_rewrite_payload_writeback(n00bb_rewrite_t *rewrite, ir_span_t span)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_local_t *local = rewrite->payload;
    ir_value_t *holder = rewrite->payload_holder;
    rewrite->payload = nullptr;
    rewrite->payload_holder = nullptr;
    if (!local) return true;
    ir_instr_t *read = ir_lower_emit_instruction(context, IR_OP_COPY, span);
    ir_instr_t *some = read && ir_lower_set_temp_dest(context, read,
            CORE_STRING_STATIC("__n00bb_payload_"), local->type)
        && ir_instr_operand_local(read, local)
        ? ir_lower_emit_instruction(context, IR_OP_OPTION_SOME, span)
        : nullptr;
    ir_instr_t *store = some && ir_lower_set_temp_dest(context, some,
            CORE_STRING_STATIC("__n00bb_payload_"),
            th_ind_referent(th_resolve(holder->type)))
        && ir_instr_operand_value(some, read->dest)
        ? ir_lower_emit_instruction(context, IR_OP_STORE, span) : nullptr;
    return store && ir_instr_operand_value(store, holder)
        && ir_instr_operand_value(store, some->dest);
}

static ir_value_t *
n00bb_rewrite_place(n00bb_rewrite_t *rewrite, ir_value_t *value,
                    ir_span_t span)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_instr_t *def = value ? value->def : nullptr;
    th_ctx *types = ir_module_types(context->module);
    if (def && def->op == IR_OP_LOAD && def->n_operands == 1)
        return ir_operand_value(&def->operands[0]);
    if (def && def->op == IR_OP_OPTION_GET && def->n_operands == 1)
        return n00bb_rewrite_payload_place(rewrite, value, span);
    ir_local_t *local = def && def->op == IR_OP_COPY && def->n_operands == 1
        ? ir_operand_local(&def->operands[0]) : nullptr;
    bool storage = value && (value->kind == IR_VAL_GLOBAL
                             || value->kind == IR_VAL_PARAM);
    if (local || storage) {
        ir_instr_t *take = ir_lower_emit_instruction(context,
            IR_OP_ADDRESS_OF, span);
        return take && ir_lower_set_temp_dest(context, take,
                CORE_STRING_STATIC("__n00bb_place_"),
                th_ind(types, 0, local ? local->type : value->type))
            && (local ? ir_instr_operand_local(take, local)
                      : ir_instr_operand_value(take, value))
            ? take->dest : nullptr;
    }
    if (def && def->op == IR_OP_COPY && def->n_operands == 1)
        return n00bb_rewrite_place(rewrite,
                                   ir_operand_value(&def->operands[0]), span);
    bool element = def && ((def->op == IR_OP_SEQUENCE_GET
                            && def->n_operands == 2)
        || (def->op == IR_OP_DISPATCH && def->n_operands == 3
            && def->operands[0].kind == IR_OPD_NAME
            && core_string_eq(def->operands[0].name,
                              CORE_STRING_STATIC("__read"))));
    if (element) {
        uint32_t at = def->op == IR_OP_DISPATCH ? 1 : 0;
        ir_value_t *base = n00bb_rewrite_place(rewrite,
            ir_operand_value(&def->operands[at]), span);
        return base ? n00bb_element_address(context, base,
                          ir_operand_value(&def->operands[at + 1]),
                          value->type, span)
                    : nullptr;
    }
    if (!def || def->op != IR_OP_EXTRACT || def->n_operands != 2
        || def->operands[1].kind != IR_OPD_NAME)
        return nullptr;
    ir_value_t *base = n00bb_rewrite_place(rewrite,
        ir_operand_value(&def->operands[0]), span);
    ir_instr_t *field = base
        ? ir_lower_emit_instruction(context, IR_OP_FIELD, span) : nullptr;
    return field && ir_lower_set_temp_dest(context, field,
            CORE_STRING_STATIC("__n00bb_place_"),
            th_ind(types, 0, value->type))
        && ir_instr_operand_value(field, base)
        && ir_instr_operand_name(field, def->operands[1].name)
        ? field->dest : nullptr;
}

static ir_value_t *
n00bb_rewrite_sequence(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                       core_string_t method)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *receiver = ir_operand_value(&in->operands[1]);
    ir_value_t *index = ir_operand_value(&in->operands[2]);
    th_type *sequence = th_resolve(receiver->type);
    if (th_kind_of(sequence) == TH_INDIRECTION)
        sequence = th_resolve(th_ind_referent(sequence));
    th_type *element = th_seq_elem(sequence);
    if (in->n_operands == 3
        && core_string_eq(method, CORE_STRING_STATIC("__read"))) {
        ir_instr_t *def = receiver->def;
        bool stored = def && def->n_operands
            && (def->op == IR_OP_LOAD || def->op == IR_OP_EXTRACT);
        ir_value_t *place = stored
            ? n00bb_rewrite_place(rewrite, receiver, in->span) : nullptr;
        if (place && th_kind_of(th_resolve(place->type)) == TH_INDIRECTION) {
            core_list_push(rewrite->abandoned, receiver);
            ir_value_t *address = n00bb_element_address(context, place,
                index, element, in->span);
            return address ? n00bb_emit_indirect_read(context, address,
                                                      element, in->span)
                           : nullptr;
        }
        ir_instr_t *read = ir_lower_emit_instruction(context,
            IR_OP_SEQUENCE_GET, in->span);
        return read && ir_lower_set_temp_dest(context, read,
                CORE_STRING_STATIC("__n00bb_array_element_"), element)
            && ir_instr_operand_value(read, receiver)
            && ir_instr_operand_value(read, index)
            ? read->dest : nullptr;
    }
    if (in->n_operands != 4
        || !core_string_eq(method, CORE_STRING_STATIC("__write")))
        return nullptr;
    rewrite->writing = true;
    ir_value_t *place = n00bb_rewrite_place(rewrite, receiver, in->span);
    rewrite->writing = false;
    if (place) core_list_push(rewrite->abandoned, receiver);
    ir_value_t *address = place ? n00bb_element_address(context, place,
        index, element, in->span) : nullptr;
    ir_value_t *value = address ? n00bb_materialize(context, nullptr,
        ir_operand_value(&in->operands[3]), element, in->span) : nullptr;
    ir_instr_t *write = value
        ? ir_lower_emit_instruction(context, IR_OP_STORE, in->span)
        : nullptr;
    return write && ir_instr_operand_value(write, address)
        && ir_instr_operand_value(write, value)
        && n00bb_rewrite_payload_writeback(rewrite, in->span)
        ? n00bb_rewrite_nil(rewrite, in->span) : nullptr;
}

static parse_sym_entry_t *
n00bb_rewrite_target_symbol(n00bb_rewrite_t *rewrite,
                            const n00bb_dispatch_target_t *target)
{
    n00bb_tu_state_t *state = tu_state(rewrite->unit);
    return target->kind == N00BB_TARGET_FUNCTION ? target->function
        : state->link ? initializer_declaration_symbol(
              linked_method_declaration(state->link, &target->method))
        : nullptr;
}

static core_string_t
n00bb_rewrite_option_builtin_name(const n00bb_dispatch_target_t *target,
                                  parse_sym_entry_t *symbol)
{
    core_string_t prefix = CORE_STRING_STATIC("builtins.");
    core_string_t name = symbol ? symbol->name
        : target->kind == N00BB_TARGET_METHOD
            && core_string_starts_with(target->method.qualified_name, prefix)
        ? core_string_slice(target->method.qualified_name, prefix.u8_bytes,
                            target->method.qualified_name.u8_bytes)
        : (core_string_t){0};
    return core_string_eq(name, CORE_STRING_STATIC("__option_is_nil"))
        || core_string_eq(name, CORE_STRING_STATIC("__option_get_or_else"))
        ? name : (core_string_t){0};
}

static ir_value_t *
n00bb_rewrite_option_builtin(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                             core_string_t name)
{
    ir_lower_context_t *context = &rewrite->context;
    bool is_nil = core_string_eq(name, CORE_STRING_STATIC("__option_is_nil"));
    ir_value_t *option = ir_operand_value(&in->operands[1]);
    th_type *item = th_option_item(th_resolve(option->type));
    ir_value_t *given = !is_nil && in->n_operands == 3
        ? in->operands[2].kind == IR_OPD_NAMED_ARG
            ? in->operands[2].named.value
            : ir_operand_value(&in->operands[2])
        : nullptr;
    ir_value_t *fallback = given && item
        ? n00bb_materialize(context, nullptr, given, item, in->span)
        : nullptr;    if (in->operands[1].kind == IR_OPD_FRESH
        || in->n_operands != (is_nil ? 2u : 3u) || (!is_nil && !fallback))
        return nullptr;
    ir_instr_t *instruction = ir_lower_emit_instruction(context,
        is_nil ? IR_OP_OPTION_IS_NIL : IR_OP_OPTION_GET_OR_ELSE, in->span);
    return instruction && ir_lower_set_temp_dest(context, instruction,
            CORE_STRING_STATIC("__n00bb_"), in->dest->type)
        && ir_instr_operand_value(instruction, option)
        && (is_nil || ir_instr_operand_value(instruction, fallback))
        && (is_nil || n00bb_set_reference_access(rewrite->ir_context,
                                                 instruction->dest,
                                                 in->dest->type))
        ? instruction->dest : nullptr;
}

bool
n00bb_place_named_arguments(ir_instr_t *in, ir_func_t *function)
{
    uint32_t count = in->n_operands;
    uint32_t positional = 1;
    while (positional < count
           && in->operands[positional].kind != IR_OPD_NAMED_ARG)
        positional++;
    if (positional == count
        && count - 1 >= th_func_nparams(th_resolve(function->type)))
        return true;
    ir_operand_t *named = core_alloc_array(ir_operand_t,
                                           count - positional + 1);
    for (uint32_t o = positional; named && o < count; o++)
        named[o - positional] = in->operands[o];
    bool *used = core_alloc_array(bool, count - positional + 1);
    bool ok = named && used;
    in->n_operands = positional;
    th_type *signature = th_resolve(function->type);
    ir_func_t *declared = function->n_params ? function
                                             : n00bb_cached_body(function);
    for (uint32_t p = positional - 1; ok && p < th_func_nparams(signature);
         p++) {
        ir_value_t *value = nullptr;
        core_string_t name = declared && p < declared->n_params
            ? declared->params[p]->name : (core_string_t){0};
        for (uint32_t k = 0; !value && name.data && k < count - positional;
             k++)
            if (!used[k] && core_string_eq(named[k].named.name, name)) {
                used[k] = true;
                value = named[k].named.value;
            }
        th_mode mode = th_func_param(signature, p)->mode;
        ok = value ? ir_instr_operand_value(in, value)
            : ir_func_parameter_is_optional(function, p)
                && th_mode_is_input_only(mode) ? ir_instr_operand_missing(in)
            : mode == TH_OUT && ir_instr_operand_out(in);
    }
    for (uint32_t k = 0; ok && k < count - positional; k++)
        ok = used[k] || ir_instr_operand_named(in, named[k].named.name,
                                               named[k].named.value);
    core_free(named);
    core_free(used);
    return ok;
}

static ir_value_t *
n00bb_rewrite_function_address(n00bb_rewrite_t *rewrite,
                               parse_sym_entry_t *symbol,
                               ir_func_t **function)
{
    n00bb_link_analysis_t *analysis =
        tu_state(rewrite->unit)->link_analysis;
    core_translation_unit_t *unit = symbol
        ? unit_for_symbol(analysis->grammar, &analysis->units, symbol)
        : nullptr;
    ir_module_t *owner = unit ? tu_state(unit)->ir_module : nullptr;
    *function = symbol ? link_ir_function_for_symbol(analysis, symbol)
                       : nullptr;
    return owner && *function ? ir_func_address(owner, *function) : nullptr;
}

static bool
n00bb_rewrite_pack_operands(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                            uint32_t fixed, th_type *collected)
{
    uint32_t end = 1;
    while (end < in->n_operands && in->operands[end].kind != IR_OPD_NAMED_ARG)
        end++;
    if (end < fixed + 1) return true;
    ir_instr_t *pack = ir_lower_emit_instruction(&rewrite->context,
                                                 IR_OP_AGGREGATE, in->span);
    bool ok = pack && ir_lower_set_temp_dest(&rewrite->context, pack,
            CORE_STRING_STATIC("__n00bb_rest_"), collected)
        && ir_instr_set_aggregate_form(pack, IR_AGGREGATE_ELEMENTS);
    for (uint32_t o = fixed + 1; ok && o < end; o++) {
        ir_value_t *value = ir_operand_value(&in->operands[o]);
        ok = value && ir_instr_operand_value(pack, value);
    }
    ir_value_t *array = ok ? pack->dest : nullptr;
    uint32_t count = in->n_operands;
    ir_operand_t *named = array
        ? core_alloc_array(ir_operand_t, count - end + 1) : nullptr;
    for (uint32_t o = end; named && o < count; o++)
        named[o - end] = in->operands[o];
    in->n_operands = named ? fixed + 1 : in->n_operands;
    ok = named && ir_instr_operand_value(in, array);
    for (uint32_t o = 0; ok && o < count - end; o++)
        ok = ir_instr_operand_named(in, named[o].named.name,
                                    named[o].named.value);
    core_free(named);
    if (ok) ir_instr_uses_changed(in);
    th_type *current = ir_instr_call_signature(in);
    return ok && (!current || ir_instr_set_call_signature(in,
        th_func_collect_rest(ir_module_types(rewrite->context.module),
                             current)));
}

static bool
n00bb_rewrite_pack_rest(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        parse_sym_entry_t *symbol, ir_func_t *function)
{
    th_type *declared = symbol ? th_resolve(n00bb_symbol_signature(symbol))
                               : nullptr;
    th_type *signature = th_resolve(function->type);
    if (!declared || th_kind_of(declared) != TH_FUNCTION
        || !th_func_rest(declared) || th_kind_of(signature) != TH_FUNCTION
        || th_func_rest(signature)
        || th_func_nparams(declared) >= th_func_nparams(signature))
        return true;
    uint32_t fixed = (uint32_t)th_func_nparams(declared);
    th_type *collected = th_resolve(th_func_param(signature, fixed)->type);
    return th_kind_of(collected) != TH_SEQUENCE
        || n00bb_rewrite_pack_operands(rewrite, in, fixed, collected);
}

static bool
n00bb_rewrite_callback_setup(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_annot_t *pending = ir_annot_get(&in->annots, N00BB_CALLBACK_SETUP_KEY);
    if (!pending || !pending->b) return true;
    ir_value_t *handler = in->n_operands
        ? ir_operand_value(&in->operands[0]) : nullptr;
    n00bb_dispatch_target_t target = {0};
    ir_func_t *function = nullptr;
    ir_value_t *setup = handler
        && n00bb_dispatch_target(rewrite->dispatch, handler->type,
                                 CORE_STRING_STATIC("setup"), &target)
        && target.kind == N00BB_TARGET_FUNCTION
        ? n00bb_rewrite_function_address(rewrite, target.function, &function)
        : nullptr;
    if (!setup || !ir_instr_operand_value(in, setup)) return false;
    ir_operand_t added = in->operands[in->n_operands - 1];
    for (uint32_t o = in->n_operands - 1; o > 0; o--)
        in->operands[o] = in->operands[o - 1];
    in->operands[0] = added;
    ir_instr_uses_changed(in);
    pending->b = false;
    th_type *signature = th_resolve(function->type);
    ir_func_t *declared = function->n_params ? function
                                             : n00bb_cached_body(function);
    for (uint32_t o = 3; o < in->n_operands; o++) {
        uint32_t p = o - 1;
        if (in->operands[o].kind == IR_OPD_NAMED_ARG)
            for (p = 0; declared && p < declared->n_params
                 && !core_string_eq(declared->params[p]->name,
                                    in->operands[o].named.name); p++) {}
        const th_param *formal = p < th_func_nparams(signature)
            ? th_func_param(signature, p) : nullptr;
        if (formal && !n00bb_rewrite_operand(rewrite, in, o, formal->type))
            return false;
    }
    return true;
}

static bool
n00bb_rewrite_method_call(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                          const n00bb_dispatch_target_t *target)
{
    parse_sym_entry_t *symbol = n00bb_rewrite_target_symbol(rewrite, target);
    core_string_t option_builtin = n00bb_rewrite_option_builtin_name(target,
                                                                     symbol);
    if (option_builtin.data)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_option_builtin(rewrite, in, option_builtin));
    ir_func_t *function = nullptr;
    ir_value_t *callee = n00bb_rewrite_function_address(rewrite, symbol,
                                                        &function);
    if (!callee) return false;
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *receiver = ir_operand_value(&in->operands[1]);
    th_type *signature = th_resolve(function->type);
    const th_param *self = th_kind_of(signature) == TH_FUNCTION
        && th_func_nparams(signature) ? th_func_param(signature, 0) : nullptr;
    th_type *constructed = in->operands[1].kind == IR_OPD_FRESH
        ? th_ind_referent(receiver->type) : nullptr;
    if (constructed
        && n00bb_object_type(ir_module_types(context->module), constructed)) {
        ir_value_t *object = n00bb_emit_fresh_object(context, receiver,
                                                     constructed, in->span);
        if (!object) return false;
        if (self && self->mode == TH_IN) {
            in->operands[1] = (ir_operand_t){.kind = IR_OPD_VALUE,
                                             .value = object};
            ir_instr_use_added(in, 1);
        }
    }
    bool write_back = ir_annot_get(&in->annots, N00BB_WRITE_BACK_KEY);
    ir_value_t *place = self && th_mode_produces_output(self->mode)
        && write_back
        ? n00bb_rewrite_place(rewrite, receiver, in->span) : nullptr;
    if (write_back && self && th_mode_produces_output(self->mode) && !place)
        return false;
    th_type *receiver_type = receiver->type;
    in->op = IR_OP_CALL;
    in->operands[0] = (ir_operand_t){.kind = IR_OPD_VALUE, .value = callee};
    ir_instr_use_added(in, 0);
    if (!n00bb_rewrite_pack_rest(rewrite, in, symbol, function)) return false;
    th_ctx *types = ir_module_types(rewrite->context.module);
    th_type *current = ir_instr_call_signature(in);
    bool closed = th_is_closed(types, function->type);
    if (closed && current && !th_is_closed(types, current))
        (void)th_match(current, function->type);
    if (closed ? current && !th_is_closed(types, current)
                   && !ir_instr_set_call_signature(in, function->type)
               : !current && !ir_instr_set_call_signature(in,
                                                          target->signature))
        return false;
    if (!n00bb_place_named_arguments(in, function)) return false;
    for (uint32_t p = in->n_operands - 1; th_kind_of(signature) == TH_FUNCTION
         && p < th_func_nparams(signature); p++) {
        th_mode mode = th_func_param(signature, p)->mode;
        bool padded = ir_func_parameter_is_optional(function, p)
                && th_mode_is_input_only(mode)
            ? ir_instr_operand_missing(in)
            : mode == TH_OUT && ir_instr_operand_out(in);
        if (!padded) return false;
    }
    core_list_t(n00bb_rewrite_value_ptr_t) places =
        core_list_new(n00bb_rewrite_value_ptr_t);
    core_list_t(n00bb_rewrite_value_ptr_t) values =
        core_list_new(n00bb_rewrite_value_ptr_t);
    uint32_t outputs = 0;
    bool written = true;
    for (uint32_t p = 0; written && th_kind_of(signature) == TH_FUNCTION
         && p < th_func_nparams(signature) && p + 1 < in->n_operands; p++) {
        th_mode mode = th_func_param(signature, p)->mode;
        if (!th_mode_produces_output(mode)) continue;
        ir_operand_t *operand = &in->operands[p + 1];
        ir_value_t *value = operand->kind == IR_OPD_VALUE
            ? operand->value : nullptr;
        ir_value_t *target = p == 0 ? place
            : value ? n00bb_rewrite_place(rewrite, value, in->span) : nullptr;
        if (value && !target && p > 0) {
            core_string_t name = describe_parameter_target(mode, symbol,
                function->params[p]->name);
            n00bb_report(core_translation_unit_diagnostics(rewrite->unit),
                rewrite->unit, N00B_DIAG_OUTPUT_ARGUMENT_NOT_WRITABLE,
                n00bb_span_location(value->def ? value->def->span : in->span),
                N00B_TEXT_ONLY_A_VARIABLE_FIELD_OR, N00BB_ARGS(ARG_TEXT(name)));
            core_free(name.data);
            written = false;
        }
        if (target) {
            core_list_push(places, target);
            core_list_push(values, value);
            core_list_push(values, (ir_value_t *)(uintptr_t)outputs);
        }
        outputs++;
    }
    if (!written) {
        core_list_free(places);
        core_list_free(values);
        return true;
    }
    bool ok = n00bb_rewrite_splice(rewrite, in)
        && n00bb_rewrite_arguments(rewrite, in, n00bb_rewrite_operand);
    for (size_t i = 0; ok && i < places.len; i++) {
        ir_value_t *value = values.data[2 * i];
        uint32_t index = (uint32_t)(uintptr_t)values.data[2 * i + 1];
        th_type *type = value ? value->type : receiver_type;
        ir_instr_t *output = ir_lower_emit_instruction(context,
            IR_OP_CALL_OUTPUT, in->span);
        ir_instr_t *store = output && ir_lower_set_temp_dest(context, output,
                CORE_STRING_STATIC("__n00bb_updated_"), type)
            && ir_instr_operand_value(output, in->dest)
            && ir_instr_operand_uint(output, index)
            ? ir_lower_emit_instruction(context, IR_OP_STORE, in->span)
            : nullptr;
        ok = store && ir_instr_operand_value(store, places.data[i])
            && ir_instr_operand_value(store, output->dest);
    }
    ok = ok && (!places.len || n00bb_rewrite_splice_after(rewrite, in));
    core_list_free(places);
    core_list_free(values);
    return ok;
}

static bool
n00bb_writes_back_unchanged(const ir_instr_t *write, const ir_value_t *updated,
                            const ir_instr_t *read)
{
    if (!read || read->op != IR_OP_COPY || !read->n_operands
        || write->n_operands < (write->op == IR_OP_GLOBAL_STORE ? 2u : 1u))
        return false;
    if (write->op == IR_OP_COPY && write->dest_kind == IR_DEST_LOCAL)
        return ir_operand_value(&write->operands[0]) == updated
            && ir_operand_local(&read->operands[0]) == write->dest_local;
    if (write->op != IR_OP_GLOBAL_STORE
        || ir_operand_value(&write->operands[1]) != updated)
        return false;
    const ir_value_t *global = ir_operand_value(&read->operands[0]);
    return global && global->kind == IR_VAL_GLOBAL
        && ir_operand_value(&write->operands[0]) == global;
}

static bool
n00bb_erase_unchanged_writebacks(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_func_t *function = rewrite->context.function;
    ir_value_t *receiver = ir_operand_value(&in->operands[1]);
    ir_instr_t *read = receiver ? receiver->def : nullptr;
    core_list_t(n00bb_ir_instr_ptr_t) writes =
        core_list_new(n00bb_ir_instr_ptr_t);
    for (uint32_t b = 0; read && b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *write = function->blocks[b]->instrs[i];
            if (n00bb_writes_back_unchanged(write, in->dest, read))
                core_list_push(writes, write);
        }
    bool ok = true;
    for (size_t w = 0; ok && w < writes.len; w++)
        ok = ir_instr_erase(writes.data[w]);
    core_list_free(writes);
    return ok;
}

static bool
n00bb_rewrite_assign_call(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                          const n00bb_dispatch_target_t *target)
{
    ir_func_t *function = nullptr;
    ir_value_t *callee = n00bb_rewrite_function_address(rewrite,
        target->function, &function);
    if (!callee || !function || in->n_operands < 2) return false;
    ir_lower_context_t *context = &rewrite->context;
    th_type *signature = th_resolve(function->type);
    uint32_t count = in->n_operands - 1;
    ir_value_t *arguments[count];
    bool ok = true;
    for (uint32_t o = 0; ok && o < count; o++) {
        ir_value_t *value = ir_operand_value(&in->operands[o + 1]);
        arguments[o] = value && o < th_func_nparams(signature)
            ? n00bb_convert(rewrite, in, value,
                            th_func_param(signature, o)->type)
            : value;
        ok = arguments[o] != nullptr;
    }
    ir_instr_t *call = ok
        ? ir_lower_emit_instruction(context, IR_OP_CALL, in->span) : nullptr;
    ok = call && ir_lower_set_temp_dest(context, call,
            CORE_STRING_STATIC("__n00bb_assigned_"), th_func_ret(signature))
        && ir_instr_operand_value(call, callee);
    for (uint32_t o = 0; ok && o < count; o++)
        ok = ir_instr_operand_value(call, arguments[o]);
    ok = ok && ir_instr_set_call_signature(call, function->type);
    if (ok && th_func_nparams(signature)
        && !th_mode_produces_output(th_func_param(signature, 0)->mode))
        return n00bb_erase_unchanged_writebacks(rewrite, in)
            && n00bb_rewrite_replace(rewrite, in,
                                     ir_operand_value(&in->operands[1]));
    ir_instr_t *output = ok
        ? ir_lower_emit_instruction(context, IR_OP_CALL_OUTPUT, in->span)
        : nullptr;
    return output && ir_lower_set_temp_dest(context, output,
            CORE_STRING_STATIC("__n00bb_updated_"), in->dest->type)
        && ir_instr_operand_value(output, call->dest)
        && ir_instr_operand_uint(output, 0)
        && n00bb_rewrite_replace(rewrite, in, output->dest);
}

static bool
n00bb_report_rival_overloads(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                             core_string_t method,
                             const n00bb_dispatch_target_t *target)
{
    core_list_t(parse_sym_entry_ptr_t) candidates =
        core_list_new(parse_sym_entry_ptr_t);
    core_list_push(candidates, target->function);
    core_list_push(candidates, target->rival);
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(rewrite->unit);
    core_string_t described = n00bb_describe_candidates(rewrite->grammar,
        tu_state(rewrite->unit), &candidates);
    n00bb_report(diagnostics, rewrite->unit, N00B_DIAG_AMBIGUOUS_NAME,
        n00bb_span_location(in->span),
        N00B_TEXT_NAMES_DECLARATIONS_FROM_DIFFERENT_MODULES,
        N00BB_ARGS(ARG_TEXT(method), ARG_TEXT(described)));
    core_free(described.data);
    for (size_t i = 0; i < candidates.len; i++)
        n00bb_diag_declared_here(diagnostics, candidates.data[i],
                                 N00B_TEXT_IS_DECLARED_HERE);
    n00bb_report_help(diagnostics, N00B_TEXT_REFER_TO_THE_ONE_YOU,
                      N00BB_NO_ARGS);
    core_list_free(candidates);
    return true;
}

static bool
n00bb_rewrite_trait_call(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                         core_string_t method,
                         const n00bb_dispatch_target_t *target)
{
    ir_lower_context_t *context = &rewrite->context;
    th_ctx *types = ir_module_types(context->module);
    th_type *contract = target->signature;
    th_type *packed = th_func_collect_rest(types, contract);
    if (packed != th_resolve(contract)) contract = packed;
    core_string_t trait = target->trait;
    char buffer[128];
    core_string_t name = n00bb_format_name(buffer, sizeof(buffer),
        "__trait_req_%.*s_%.*s", (int)trait.u8_bytes, trait.data,
        (int)method.u8_bytes, method.data);
    ir_func_t *requirement = ir_func_find(context->module, name);
    if (!requirement) {
        requirement = ir_func_add(context->module, name,
                                  th_type_clone(types, contract),
                                  IR_LINK_EXTERN);
        if (requirement && !th_is_closed(types, contract)
            && !ir_func_set_callable_contract(requirement, contract))
            requirement = nullptr;
    }
    th_type *signature = ir_instr_call_signature(in);
    th_type *spread = th_resolve(signature ? signature : target->signature);
    if (!signature) signature = contract;
    packed = th_func_collect_rest(types, signature);
    if (packed != th_resolve(signature)) signature = packed;
    ir_value_t *receiver = ir_operand_value(&in->operands[1]);
    th_type *resolved = th_resolve(signature);
    if (th_kind_of(spread) == TH_FUNCTION && th_func_rest(spread)
        && th_kind_of(resolved) == TH_FUNCTION
        && th_func_nparams(spread) < th_func_nparams(resolved)
        && !n00bb_rewrite_pack_operands(rewrite, in,
               (uint32_t)th_func_nparams(spread),
               th_resolve(th_func_param(resolved,
                                        th_func_nparams(spread))->type)))
        return false;
    const th_param *self = th_kind_of(resolved) == TH_FUNCTION
        && th_func_nparams(resolved) ? th_func_param(resolved, 0) : nullptr;
    ir_value_t *place = self && th_mode_produces_output(self->mode)
        && ir_annot_get(&in->annots, N00BB_WRITE_BACK_KEY)
        ? n00bb_rewrite_place(rewrite, receiver, in->span) : nullptr;
    ir_instr_t *selection = requirement
        ? ir_lower_emit_instruction(context, IR_OP_TRAIT_METHOD, in->span)
        : nullptr;
    if (!selection || !ir_lower_set_temp_dest(context, selection,
            CORE_STRING_STATIC("__n00bb_trait_method_"), signature)
        || !ir_instr_operand_name(selection, trait)
        || !ir_instr_operand_name(selection, method)
        || !ir_instr_operand_value(selection, receiver)
        || !ir_instr_operand_value(selection,
               ir_func_address(context->module, requirement))
        || !n00bb_rewrite_splice(rewrite, in))
        return false;
    in->op = IR_OP_CALL;
    in->operands[0] = (ir_operand_t){.kind = IR_OPD_VALUE,
                                     .value = selection->dest};
    ir_instr_use_added(in, 0);
    n00bb_link_analysis_t *analysis = tu_state(rewrite->unit)->link_analysis;
    parse_sym_entry_t *required = target->function ? target->function
        : trait_method_requirement_symbol(rewrite->grammar, analysis,
                                          rewrite->unit, trait, method);
    ir_func_t *declared = required
        ? link_ir_function_for_symbol(analysis, required) : nullptr;
    for (uint32_t p = 0; declared && p < th_func_nparams(declared->type);
         p++)
        if (ir_func_parameter_is_optional(declared, p)
            && !ir_func_set_parameter_optional(requirement, p, true))
            return false;
    if (declared && !n00bb_place_named_arguments(in, declared))
        return core_diagnostics_failed(
            core_translation_unit_diagnostics(rewrite->unit));
    if (!ir_instr_set_call_signature(in, signature)) return false;
    for (uint32_t o = 1; o < in->n_operands
         && o <= th_func_nparams(resolved); o++) {
        th_type *formal = th_func_param(resolved, o - 1)->type;
        if (in->operands[o].kind == IR_OPD_VALUE
            && th_is_closed(types, formal)
            && !n00bb_rewrite_operand(rewrite, in, o, formal)) return false;
    }
    if (!place) return true;
    ir_instr_t *output = ir_lower_emit_instruction(context, IR_OP_CALL_OUTPUT,
                                                   in->span);
    ir_instr_t *store = output && ir_lower_set_temp_dest(context, output,
            CORE_STRING_STATIC("__n00bb_updated_"), receiver->type)
        && ir_instr_operand_value(output, in->dest)
        && ir_instr_operand_uint(output, 0)
        ? ir_lower_emit_instruction(context, IR_OP_STORE, in->span) : nullptr;
    return store && ir_instr_operand_value(store, place)
        && ir_instr_operand_value(store, output->dest)
        && n00bb_rewrite_splice_after(rewrite, in);
}

static bool
n00bb_rewrite_callback(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                       th_type *receiver_type)
{
    th_ctx *types = ir_module_types(rewrite->context.module);
    th_type *instance = ir_instr_call_signature(in);
    instance = instance ? th_resolve(instance) : nullptr;
    if (!instance || th_kind_of(instance) != TH_FUNCTION
        || !th_func_nparams(instance))
        return false;
    size_t count = th_func_nparams(instance) - 1;
    th_param *params = core_alloc_array(th_param, count ? count : 1);
    for (size_t i = 0; i < count; i++)
        params[i] = *th_func_param(instance, i + 1);
    th_type *signature = th_func_with_error(types, th_func_ret(instance),
        th_func_error(instance), params, count, nullptr, nullptr, 0);
    core_free(params);
    for (uint32_t o = 1; o < in->n_operands; o++)
        in->operands[o - 1] = in->operands[o];
    in->n_operands--;
    ir_instr_uses_changed(in);
    in->op = th_kind_of(receiver_type) == TH_INDIRECTION
        ? IR_OP_CALL : IR_OP_CALLBACK_INVOKE;
    if (!signature || !ir_instr_set_call_signature(in, signature))
        return false;
    if (in->op != IR_OP_CALLBACK_INVOKE) return true;
    n00bb_link_analysis_t *analysis = tu_state(rewrite->unit)->link_analysis;
    n00bb_callback_contract_t *contract = nullptr;
    for (size_t u = 0; !contract && u < analysis->units.len; u++)
        contract = n00bb_callback_contract_for_type(
            tu_state(analysis->units.data[u]), receiver_type);
    return !contract || !contract->stateful
        || ir_instr_set_callback_invoke_handler(in,
               n00bb_specialize_callback_role(types, contract, receiver_type,
                                              contract->handler->type));
}

static ir_value_t *
n00bb_rewrite_flagset(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                      core_string_t method, ir_value_t *left,
                      ir_value_t *right)
{
    return n00bb_emit_flagset_operator(&rewrite->context, method,
                                       in->dest->type, left, right, in->span);
}

static ir_value_t *
n00bb_rewrite_flag_test(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        ir_value_t *flags, parse_sym_entry_t *flag)
{
    ir_lower_context_t *context = &rewrite->context;
    const n00bb_flagset_value_t *value = flag->has_comptime_value
        ? flag->comptime_value : nullptr;
    ir_instr_t *mask = value && flag->type
        ? ir_lower_emit_instruction(context, IR_OP_BITSET_CONST, in->span)
        : nullptr;
    ir_instr_t *test = mask && ir_lower_set_temp_dest(context, mask,
            CORE_STRING_STATIC("__n00bb_"), flag->type)
        && ir_instr_operand_name(mask, flag->name)
        && ir_instr_operand_uint(mask, value->mask)
        ? ir_lower_emit_instruction(context, IR_OP_BITSET_CONTAINS, in->span)
        : nullptr;
    return test && ir_lower_set_temp_dest(context, test,
            CORE_STRING_STATIC("__n00bb_"), in->dest->type)
        && ir_instr_operand_value(test, flags)
        && ir_instr_operand_value(test, mask->dest)
        ? test->dest : nullptr;
}

static bool n00bb_rewrite_dispatch(n00bb_rewrite_t *rewrite, ir_instr_t *in);

static ir_value_t *
n00bb_rewrite_case(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                   ir_value_t *subject)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_annot_t *name = ir_annot_get(&in->annots, N00BB_CASE_KEY);
    bool variant = false;
    parse_sym_entry_t *member = name && name->kind == IR_ANNOT_STR
        ? n00bb_case_member(rewrite->dispatch, subject->type, name->s,
                            &variant)
        : nullptr;
    th_type *tag_type = n00bb_ir_builtin_type(context->module,
                                              CORE_STRING_STATIC("uint"));
    if (!member) {
        ir_instr_t *no = ir_lower_emit_instruction(context, IR_OP_CONST,
                                                   in->span);
        return no && ir_lower_set_temp_dest(context, no,
                CORE_STRING_STATIC("__n00bb_"), in->dest->type)
            && ir_instr_operand_int(no, 0) ? no->dest : nullptr;
    }
    if (!variant) {
        ir_value_t *constant = n00bb_emit_enum_member(context, member,
                                                      in->span);
        return constant ? n00bb_emit_binary_ir(context, IR_OP_EQ,
                              in->dest->type, subject, constant, in->span)
                        : nullptr;
    }
    ir_instr_t *tag = tag_type
        ? ir_lower_emit_instruction(context, IR_OP_VARIANT_TAG, in->span)
        : nullptr;
    ir_instr_t *arm = tag && ir_lower_set_temp_dest(context, tag,
            CORE_STRING_STATIC("__n00bb_variant_tag_"), tag_type)
        && ir_instr_operand_value(tag, subject)
        ? ir_lower_emit_instruction(context, IR_OP_CONST, in->span)
        : nullptr;
    return arm && ir_lower_set_temp_dest(context, arm,
            CORE_STRING_STATIC("__n00bb_arm_tag_"), tag_type)
        && ir_instr_operand_int_lit(arm, ir_lit_hex(th_arm_tag(member->name)))
        ? n00bb_emit_binary_ir(context, IR_OP_EQ, in->dest->type, tag->dest,
                               arm->dest, in->span)
        : nullptr;
}

static const void *
shared_place(const ir_value_t *value)
{
    const ir_instr_t *read = value ? value->def : nullptr;
    ir_value_t *address = read && read->op == IR_OP_LOAD
            && read->n_operands == 1
        ? ir_operand_value(&read->operands[0]) : nullptr;
    if (value && value->kind == IR_VAL_GLOBAL)
        return value->global ? ir_global_identity(value->global) : nullptr;
    return address && !(address->def && address->def->op == IR_OP_ALLOC)
        ? address : nullptr;
}

static const void *
snapshot_place(const ir_instr_t *in)
{
    if (!ir_annot_get((ir_annots_t *)&in->annots, N00BB_SWITCH_COPY_KEY))
        return nullptr;
    return in->op == IR_OP_COPY && in->n_operands == 1
        ? shared_place(ir_operand_value(&in->operands[0]))
        : in->op == IR_OP_LOAD ? shared_place(in->dest) : nullptr;
}

static bool
precedes(ir_doms_t *doms, const ir_instr_t *a, const ir_instr_t *b)
{
    return a->block == b->block ? a->index < b->index
        : ir_dominates(doms, a->block, b->block);
}

static uint32_t
place_version(ir_pdg_memory_t *memory, const ir_instr_t *reader)
{
    return ir_pdg_memory_slot_def(memory,
        ir_pdg_memory_access_of(memory, reader, IR_ACCESS_LOAD));
}

static ir_value_t *
switch_copy(ir_func_t *function, ir_instr_t *in, ir_value_t *subject)
{
    const void *place = shared_place(subject);
    ir_value_numbers_t *numbers = place ? ir_func_value_numbers(function)
                                        : nullptr;
    ir_doms_t *doms = numbers ? ir_func_doms(function) : nullptr;
    ir_instr_t *copy = nullptr;
    for (uint32_t b = 0; doms && b < function->n_blocks; b++)
        for (uint32_t k = 0; k < function->blocks[b]->n_instrs; k++) {
            ir_instr_t *candidate = function->blocks[b]->instrs[k];
            if (snapshot_place(candidate) == place
                && precedes(doms, candidate, in)
                && (!copy || precedes(doms, copy, candidate)))
                copy = candidate;
        }
    if (!copy) return nullptr;
    if (subject->def)
        return ir_value_numbers_same(numbers, copy->dest, subject)
            ? copy->dest : nullptr;
    ir_pdg_memory_t *memory = ir_pdg_memory(ir_func_pdg(function));
    uint32_t version = place_version(memory, copy);
    return version != UINT32_MAX && version == place_version(memory, in)
        ? copy->dest : nullptr;
}

static bool
n00bb_rewrite_payload(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                      ir_value_t *subject)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *copy = in->block
        ? switch_copy(in->block->owner, in, subject) : nullptr;
    if (copy) subject = copy;
    if (th_is_nil(th_resolve(in->dest->type))) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(rewrite->unit);
        n00bb_report(diagnostics, rewrite->unit, N00B_DIAG_ARM_HAS_NO_PAYLOAD,
            n00bb_span_location(in->span), N00B_TEXT_ARM_CARRIES_NO_VALUE_SO,
            N00BB_ARGS(ARG_TEXT(in->operands[1].name)));
        return true;
    }
    ir_instr_t *payload = ir_lower_emit_instruction(context,
        IR_OP_VARIANT_PAYLOAD, in->span);
    return payload && ir_lower_set_temp_dest(context, payload,
            CORE_STRING_STATIC("__n00bb_payload_"), in->dest->type)
        && ir_instr_operand_value(payload, subject)
        && ir_instr_operand_name(payload, in->operands[1].name)
        && n00bb_rewrite_replace(rewrite, in, payload->dest);
}

static bool
n00bb_rewrite_equal_by_cmp(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                           ir_value_t *left, ir_value_t *right)
{
    ir_lower_context_t *context = &rewrite->context;
    th_type *comparison = n00bb_ir_builtin_type(context->module,
        CORE_STRING_STATIC("Comparison"));
    ir_instr_t *compare = comparison
        ? ir_lower_emit_instruction(context, IR_OP_DISPATCH, in->span)
        : nullptr;
    bool ok = compare && ir_lower_set_temp_dest(context, compare,
            CORE_STRING_STATIC("__n00bb_compare_"), comparison)
        && ir_instr_operand_name(compare, CORE_STRING_STATIC("__cmp"))
        && ir_instr_operand_value(compare, left)
        && ir_instr_operand_value(compare, right);
    ir_value_t *equal = ok ? n00bb_rewrite_member(rewrite, comparison,
        N00BB_ORDER_EQUAL, in->span) : nullptr;
    ir_value_t *same = equal ? n00bb_emit_binary_ir(context, IR_OP_EQ,
        in->dest->type, compare->dest, equal, in->span) : nullptr;
    return same && n00bb_rewrite_replace(rewrite, in, same)
        && n00bb_rewrite_dispatch(rewrite, compare);
}

static bool
n00bb_rewrite_dispatch(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    core_string_t method = in->operands[0].name;
    ir_value_t *receiver = ir_operand_value(&in->operands[1]);
    ir_value_t *other = in->n_operands == 3
        ? ir_operand_value(&in->operands[2]) : nullptr;
    if (!receiver) return true;
    th_type *receiver_type = th_resolve(receiver->type);
    if (in->operands[1].kind == IR_OPD_FRESH)
        receiver_type = th_resolve(th_ind_referent(receiver_type));
    n00bb_dispatch_target_t flagset = {0};
    if (other && th_kind_of(receiver_type) == TH_NAMED
        && n00bb_dispatch_target(rewrite->dispatch, receiver_type, method,
                                 &flagset)
        && flagset.kind == N00BB_TARGET_FLAGSET)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_flagset(rewrite, in, method, receiver, other));
    ir_instr_t *literal = other && other->def && other->def->op == IR_OP_CONST
        && other->def->n_operands
        && other->def->operands[0].kind == IR_OPD_CONST
        ? other->def : nullptr;
    ir_annot_t *known = ir_annot_get(&in->annots, N00BB_TUPLE_INDEX_KEY);
    if ((literal || (known && known->kind == IR_ANNOT_INT))
        && th_builtin_kind_of(receiver_type) == TH_BUILTIN_TUPLE
        && core_string_eq(method, CORE_STRING_STATIC("__read"))) {
        uint64_t position = known && known->kind == IR_ANNOT_INT
            ? (uint64_t)known->i.mag
            : (uint64_t)literal->operands[0].lit.i.mag;
        ir_instr_t *extract = ir_lower_emit_instruction(&rewrite->context,
            IR_OP_EXTRACT_INDEX, in->span);
        return extract && ir_lower_set_temp_dest(&rewrite->context, extract,
                CORE_STRING_STATIC("__n00bb_"), in->dest->type)
            && ir_instr_operand_value(extract, receiver)
            && ir_instr_operand_uint(extract, position)
            && n00bb_rewrite_replace(rewrite, in, extract->dest);
    }
    ir_class_t class = ir_machine_of(rewrite->context.module,
                                     receiver_type).class;
    if (n00bb_primitive_operator(class, method))
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_primitive(rewrite, in, method, receiver, other));
    bool writable = false;
    ir_annot_t *escapes = ir_annot_get(&in->annots,
                                       N00BB_REFERENCE_ESCAPES_KEY);
    if (escapes && th_kind_of(receiver_type) == TH_NAMED
        && n00bb_reference_dispatch(method, nullptr)) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(rewrite->unit);
        n00bb_report(diagnostics, rewrite->unit,
            N00B_DIAG_REFERENCE_ADDRESS_ESCAPES, n00bb_span_location(in->span),
            N00B_TEXT_REFERENCE_ADDRESS_ESCAPES,
            N00BB_ARGS(ARG_TEXT(escapes->s)));
        n00bb_report_help(diagnostics, N00B_TEXT_PASS_THE_REFERENCE_DIRECTLY,
                          N00BB_NO_ARGS);
        return true;
    }
    if (n00bb_reference_dispatch(method, &writable)) {
        ir_value_t *address = th_kind_of(receiver_type) == TH_INDIRECTION
            ? receiver
            : th_kind_of(receiver_type) == TH_NAMED
            ? n00bb_reference_hook_address(&rewrite->context, receiver,
                                           writable, in->span)
            : n00bb_reference_placeholder(&rewrite->context, receiver,
                                          in->dest->type, writable, in->span);
        return n00bb_rewrite_replace(rewrite, in, address);
    }
    bool element_access = core_string_eq(method, CORE_STRING_STATIC("__read"))
        || core_string_eq(method, CORE_STRING_STATIC("__write"));
    n00bb_dispatch_target_t target = {0};
    bool found = n00bb_dispatch_target_for(rewrite->dispatch, receiver_type,
                                           method, in->n_operands - 1,
                                           &target);
    if (th_kind_of(receiver_type) == TH_SEQUENCE && element_access
        && !(found && target.kind == N00BB_TARGET_FUNCTION))
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_sequence(rewrite, in, method));
    if (found && target.rival)
        return n00bb_report_rival_overloads(rewrite, in, method, &target);
    n00bb_dispatch_target_t assign = {0};
    if ((target.kind == N00BB_TARGET_ASSIGN
         || ir_annot_get(&in->annots, N00BB_COMPOUND_KEY))
        && n00bb_assign_target(rewrite->dispatch, receiver_type, method,
                               &assign))
        return n00bb_rewrite_assign_call(rewrite, in, &assign);
    if (found && (target.kind == N00BB_TARGET_METHOD
                  || target.kind == N00BB_TARGET_FUNCTION))
        return n00bb_rewrite_method_call(rewrite, in, &target);
    if (target.kind == N00BB_TARGET_REF_PLAIN)
        return n00bb_rewrite_replace(rewrite, in, receiver);
    if (target.kind == N00BB_TARGET_REF_CREATE) {
        ir_value_t *value = n00bb_emit_indirect_read(&rewrite->context,
            receiver, receiver_type, in->span);
        return n00bb_rewrite_replace(rewrite, in, value
            ? n00bb_emit_hook_call(&rewrite->context, &target.method, value,
                                   in->span)
            : nullptr);
    }
    if (target.kind == N00BB_TARGET_CASE)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_case(rewrite, in, receiver));
    if (target.kind == N00BB_TARGET_CALLBACK)
        return n00bb_rewrite_callback(rewrite, in, receiver_type);
    if (target.kind == N00BB_TARGET_TRAIT)
        return n00bb_rewrite_trait_call(rewrite, in, method, &target);
    if (target.kind == N00BB_TARGET_ENUM && other)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_primitive(rewrite, in, method, receiver, other));
    if (target.kind == N00BB_TARGET_FLAGSET && other)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_flagset(rewrite, in, method, receiver, other));
    if (target.kind == N00BB_TARGET_EQUAL_BY_CMP && other)
        return n00bb_rewrite_equal_by_cmp(rewrite, in, receiver, other);
    if (th_kind_of(receiver_type) != TH_VAR) return false;
    n00bb_report_unknown_method(rewrite, in, method, receiver,
                                receiver_type);
    return true;
}

static void
n00bb_report_unknown_method(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                            core_string_t method, ir_value_t *receiver,
                            th_type *receiver_type)
{
    th_ctx *types = ir_module_types(rewrite->context.module);
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(rewrite->unit);
    core_string_t value = n00bb_value_text(rewrite->unit, receiver, in->span);
    if (!value.u8_bytes) value = CORE_STRING_STATIC("the value");
    th_env_method_requirement_t found[16] = {0};
    size_t count = th_env_method_requirements(types, method, found,
                                              sizeof(found) / sizeof(*found));
    if (count > sizeof(found) / sizeof(*found))
        count = sizeof(found) / sizeof(*found);
    if (!count) {
        n00bb_report(diagnostics, rewrite->unit, N00B_DIAG_UNBOUNDED_METHOD,
            n00bb_span_location(in->span), N00B_TEXT_NO_TRAIT_HAS_METHOD,
            N00BB_ARGS(ARG_TEXT(method), ARG_TEXT(value)));
        n00bb_report_help(diagnostics, N00B_TEXT_GIVE_IT_A_TYPE_WITH,
            N00BB_ARGS(ARG_TEXT(value), ARG_TEXT(method)));
        return;
    }
    for (size_t i = 1; i < count; i++)
        for (size_t j = i; j > 0 && core_string_cmp(found[j - 1].trait_name,
                                                    found[j].trait_name) > 0;
             j--) {
            th_env_method_requirement_t swap = found[j];
            found[j] = found[j - 1];
            found[j - 1] = swap;
        }
    core_buffer_t traits = {0};
    for (size_t i = 0; i < count; i++) {
        core_buffer_printf(&traits, "%s`%.*s`",
            !i ? "" : i + 1 == count ? " and " : ", ",
            (int)n00bb_name_leaf(found[i].trait_name).u8_bytes,
            n00bb_name_leaf(found[i].trait_name).data);
    }
    n00bb_report(diagnostics, rewrite->unit, N00B_DIAG_UNBOUNDED_METHOD,
        n00bb_span_location(in->span), N00B_TEXT_AMBIGUOUS_METHOD,
        N00BB_ARGS(ARG_TEXT(method), ARG_TEXT(value)));
    n00bb_report_note(diagnostics, N00B_TEXT_METHOD_OF_TRAITS,
        N00BB_ARGS(ARG_TEXT(method), ARG_TEXT(core_buffer_string(&traits))));
    n00bb_report_help(diagnostics, N00B_TEXT_SAY_WHICH_TRAIT,
        N00BB_ARGS(ARG_TEXT(value),
                   ARG_TEXT(n00bb_print_type(types, receiver_type)),
                   ARG_TEXT(n00bb_name_leaf(found[0].trait_name))));
    core_buffer_release(&traits);
}

static th_type *
n00bb_literal_element(n00bb_rewrite_t *rewrite, th_type *container)
{
    th_ctx *types = ir_module_types(rewrite->context.module);
    n00bb_dispatch_target_t target = {0};
    if (!n00bb_dispatch_target(rewrite->dispatch, container,
                               CORE_STRING_STATIC("__init"), &target))
        return nullptr;
    th_type *contract = th_resolve(target.signature);
    th_type *signature = th_kind_of(contract) == TH_BINDER
        ? th_instantiate(types, contract) : th_type_clone(types, contract);
    th_type *seed = signature && th_kind_of(signature) == TH_FUNCTION
        && th_func_nparams(signature) == 2
        && th_match(th_func_param(signature, 0)->type, container)
        ? th_resolve(th_func_param(signature, 1)->type) : nullptr;
    return seed && th_kind_of(seed) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(seed)) : nullptr;
}

static ir_value_t *
n00bb_literal_payload(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                      th_type *element)
{
    ir_lower_context_t *context = &rewrite->context;
    th_ctx *types = ir_module_types(context->module);
    ir_aggregate_form_t form = IR_AGGREGATE_ELEMENTS;
    ir_instr_aggregate_form(in, &form);
    bool pairs = form == IR_AGGREGATE_PAIRS;
    bool tuple = th_builtin_kind_of(element) == TH_BUILTIN_TUPLE
        && th_named_nargs(element) == 2;
    core_list_t(n00bb_rewrite_value_ptr_t) items =
        core_list_new(n00bb_rewrite_value_ptr_t);
    bool ok = !pairs || !(in->n_operands & 1u);
    for (uint32_t o = 0; ok && o < in->n_operands; o += pairs ? 2 : 1) {
        ir_value_t *key = ir_operand_value(&in->operands[o]);
        ir_value_t *item = nullptr;
        if (pairs && tuple) {
            ir_value_t *value = ir_operand_value(&in->operands[o + 1]);
            ir_value_t *members[] = {
                n00bb_materialize(context, nullptr, key,
                                  th_named_arg(element, 0)->type, in->span),
                n00bb_materialize(context, nullptr, value,
                                  th_named_arg(element, 1)->type, in->span),
            };
            ir_instr_t *pair = members[0] && members[1]
                ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE,
                                            in->span) : nullptr;
            ok = pair && ir_lower_set_temp_dest(context, pair,
                    CORE_STRING_STATIC("__n00bb_pair_"), element)
                && ir_instr_operand_value(pair, members[0])
                && ir_instr_operand_value(pair, members[1]);
            item = ok ? pair->dest : nullptr;
        }
        else
            item = !pairs || !tuple
                ? n00bb_materialize(context, nullptr, key, element, in->span)
                : nullptr;
        ok = ok && item;
        if (ok) core_list_push(items, item);
    }
    ir_instr_t *array = ok
        ? ir_lower_emit_instruction(context, IR_OP_AGGREGATE, in->span)
        : nullptr;
    ok = array && ir_lower_set_temp_dest(context, array,
            CORE_STRING_STATIC("__n00bb_literal_payload_"),
            th_array(types, element))
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS);
    for (size_t i = 0; ok && i < items.len; i++)
        ok = ir_instr_operand_value(array, items.data[i]);
    core_list_free(items);
    return ok ? array->dest : nullptr;
}

static bool
n00bb_literal_undecided(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        th_type *container)
{
    th_ctx *types = ir_module_types(rewrite->context.module);
    th_type **variables = nullptr;
    size_t count = th_free_vars(types, container, &variables);
    bool undecided = false;
    for (size_t i = 0; !undecided && i < count; i++)
        undecided = !th_occurs(types, variables[i],
                               rewrite->context.function->type);
    if (!undecided) return false;
    core_diagnostics_t *reported =
        core_translation_unit_diagnostics(rewrite->unit);
    for (size_t i = 0; i < core_diagnostics_count(reported); i++) {
        core_diag_severity_t severity =
            core_diagnostics_at(reported, i)->severity;
        if ((severity == CORE_DIAG_ERROR || severity == CORE_DIAG_FATAL)
            && !core_string_eq(core_diagnostics_at(reported, i)->code,
                               CORE_STRING_STATIC("N00B-T1041")))
            return true;
    }
    core_string_t name = {0};
    if (th_kind_of(container) == TH_NAMED)
        th_named_info(container, &name, nullptr, nullptr);
    size_t start = name.u8_bytes;
    while (start && name.data[start - 1] != '.') start--;
    core_string_t shown = name.data
        ? core_string_slice(name, start, name.u8_bytes)
        : CORE_STRING_STATIC("array");
    char buffer[128];
    core_string_t example = n00bb_format_name(buffer, sizeof(buffer),
        "%.*s[%s]", (int)shown.u8_bytes, shown.data,
        th_kind_of(container) == TH_NAMED && th_named_nargs(container) == 2
            ? "string, int" : "int");
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(rewrite->unit);
    n00bb_report(diagnostics, rewrite->unit,
        N00B_DIAG_LITERAL_ELEMENTS_UNDECIDED, n00bb_span_location(in->span),
        N00B_TEXT_LITERAL_ELEMENTS_UNDECIDED,
        N00BB_ARGS(ARG_TEXT(shown), ARG_TEXT(example)));
    n00bb_report_help(diagnostics, N00B_TEXT_LITERAL_ELEMENTS_UNDECIDED_HELP,
        N00BB_ARGS(ARG_TEXT(shown), ARG_TEXT(example)));
    return true;
}

static bool
n00bb_rewrite_literal(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_lower_context_t *context = &rewrite->context;
    th_ctx *types = ir_module_types(context->module);
    th_type *container = th_resolve(in->dest->type);
    if (n00bb_literal_undecided(rewrite, in, container)) return true;
    if (th_kind_of(container) == TH_SEQUENCE) return true;
    th_type *element = n00bb_literal_element(rewrite, container);
    ir_value_t *payload = element
        ? n00bb_literal_payload(rewrite, in, element) : nullptr;
    th_type *address_type = payload
        ? th_ind(types, TH_NULLABLE | TH_RESEATABLE | TH_INDEXABLE, container)
        : nullptr;
    ir_instr_t *allocation = address_type
        ? ir_lower_emit_instruction(context, IR_OP_ALLOC, in->span) : nullptr;
    bool ok = allocation
        && ir_lower_set_temp_dest(context, allocation,
               CORE_STRING_STATIC("__n00bb_literal_storage_"), address_type)
        && ir_instr_operand_type(allocation, container);
    ir_instr_t *init = ok
        ? ir_lower_emit_instruction(context, IR_OP_DISPATCH, in->span)
        : nullptr;
    ok = init
        && ir_lower_set_temp_dest(context, init,
               CORE_STRING_STATIC("__n00bb_literal_init_"), th_nil(types))
        && ir_instr_operand_name(init, CORE_STRING_STATIC("__init"))
        && ir_instr_operand_fresh(init, allocation->dest)
        && ir_instr_operand_value(init, payload);
    ir_value_t *value = ok
        ? n00bb_emit_indirect_read(context, allocation->dest, container,
                                   in->span)
        : nullptr;
    return value && n00bb_rewrite_replace(rewrite, in, value)
        && n00bb_rewrite_dispatch(rewrite, init);
}

static bool
n00bb_outside_scratch(const ir_instr_t *in, void *state)
{
    return in->block == state;
}

static bool
n00bb_rewrite_object_literal(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_lower_context_t *context = &rewrite->context;
    th_ctx *types = ir_module_types(context->module);
    th_type *object = in->dest->type;
    th_type *reference = n00bb_reference_definition(types, object);
    th_type *fields = reference
        ? th_ind_referent(th_resolve(reference)) : nullptr;
    if (!fields) return false;
    in->dest->type = fields;
    ir_value_t *made = n00bb_object_from_fields(context, in->dest, object,
                                                in->span);
    if (!made) return false;
    ir_value_replace_uses(context->function, in->dest, made,
                          n00bb_outside_scratch, rewrite->scratch);
    return n00bb_rewrite_splice_after(rewrite, in);
}

static bool
n00bb_rewrite_return_layout(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    th_type *signature = th_resolve(rewrite->context.function->type);
    if (ir_callable_returns_value(rewrite->context.module, signature)
        || in->n_operands != ir_call_output_count(signature) + 1)
        return true;
    for (uint32_t o = 1; o < in->n_operands; o++)
        in->operands[o - 1] = in->operands[o];
    in->n_operands--;
    ir_instr_uses_changed(in);
    return true;
}

static bool
n00bb_record_literal_buildable(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    th_type *type = th_resolve(in->dest->type);
    bool variant = false;
    if (th_kind_of(type) != TH_NAMED
        || !th_named_info(type, nullptr, &variant, nullptr) || variant)
        return true;
    core_string_t text = n00bb_print_type(
        ir_module_types(rewrite->context.module), type);
    core_diagnostics_t *diagnostics =
        core_translation_unit_diagnostics(rewrite->unit);
    core_translation_unit_t *owner = nullptr;
    parse_scope_t *scope = record_scope_for_type(
        tu_state(rewrite->unit)->link_analysis, type, &owner);
    parse_sym_entry_t *hidden = nullptr;
    for (parse_sym_entry_t *field = scope ? scope->first_in_scope : nullptr;
         field; field = field->next_in_scope) {
        if (!field->is_field) continue;
        (void)parse_sym_set_binding(field, N00BB_FIELD_LITERAL_SET_KEY, field);
        if (!hidden && owner && owner != rewrite->unit
            && member_is_private(field, field->owner_type))
            hidden = field;
    }
    if (hidden) {
        n00bb_report(diagnostics, rewrite->unit, N00B_DIAG_FIELD_NOT_VISIBLE,
                     n00bb_span_location(in->span),
                     N00B_TEXT_RECORD_HAS_PRIVATE_FIELD,
                     N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(hidden->name),
                                ARG_TEXT(owner->module.identifier)));
        n00bb_report_help(diagnostics, N00B_TEXT_BUILD_IT_IN_MODULE,
                          N00BB_ARGS(ARG_TEXT(owner->module.identifier)));
        return false;
    }
    if (!n00bb_type_has_constructor(rewrite->unit, type)) return true;
    n00bb_report(diagnostics, rewrite->unit,
                 N00B_DIAG_RECORD_HAS_CONSTRUCTOR,
                 n00bb_span_location(in->span), N00B_TEXT_HAS_AN_INIT_SO_A,
                 N00BB_ARGS(ARG_TEXT(text)));
    n00bb_report_help(diagnostics, N00B_TEXT_TUPLE_CONSTRUCTOR_HELP,
                      N00BB_ARGS(ARG_TEXT(text)));
    return false;
}

static parse_sym_entry_t *
n00bb_rewrite_intrinsic_symbol(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_func_t *target = ir_call_function(in);
    return target
        ? core_dict_get(&tu_state(rewrite->unit)->intrinsics, target, nullptr)
        : nullptr;
}

static bool
n00bb_rewrite_intrinsic(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                        parse_sym_entry_t *symbol)
{
    ir_value_t *operands[8];
    size_t count = in->n_operands - 1;
    if (count > 8 || !n00bb_rewrite_sinks(rewrite, in, n00bb_rewrite_operand))
        return false;
    for (size_t i = 0; i < count; i++)
        if (!(operands[i] = ir_operand_value(&in->operands[i + 1])))
            return false;
    ir_lower_result_t result = n00bb_emit_intrinsic(&rewrite->context, symbol,
        operands, count, false, in->dest ? in->dest->type : nullptr,
        in->span);
    if (result.status == IR_LOWER_VALUE)
        return n00bb_rewrite_replace(rewrite, in, result.value);
    return result.status == IR_LOWER_NO_VALUE
        && n00bb_rewrite_splice(rewrite, in) && ir_instr_erase(in);
}

static bool
n00bb_rewrite_instruction(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    parse_sym_entry_t *intrinsic = n00bb_rewrite_intrinsic_symbol(rewrite, in);
    if (intrinsic)
        return n00bb_rewrite_intrinsic(rewrite, in, intrinsic);
    if (in->op == IR_OP_OPTION_GET && in->dest)
        return n00bb_set_reference_access(rewrite->ir_context, in->dest,
                                          in->dest->type);
    if (in->op == IR_OP_AGGREGATE && in->dest
        && ir_annot_get(&in->annots, N00BB_RECORD_LITERAL_KEY)
        && !n00bb_record_literal_buildable(rewrite, in))
        return false;
    ir_aggregate_form_t form = IR_AGGREGATE_ELEMENTS;
    if (in->op == IR_OP_AGGREGATE && in->dest
        && ir_instr_aggregate_form(in, &form))
        return n00bb_rewrite_literal(rewrite, in);
    size_t length = 0;
    const uint8_t *bytes = in->op == IR_OP_CONST && in->dest
        && th_kind_of(th_resolve(in->dest->type)) == TH_SEQUENCE
        ? ir_instr_const_bytes(in, &length) : nullptr;
    if (bytes) {
        ir_lower_result_t array = n00bb_emit_char_array(&rewrite->context,
            bytes, length, in->dest->type, in->span);
        return array.status == IR_LOWER_VALUE
            && n00bb_rewrite_replace(rewrite, in, array.value);
    }
    th_ctx *types = ir_module_types(rewrite->context.module);
    ir_value_t *operand = in->n_operands
        ? ir_operand_value(&in->operands[0]) : nullptr;
    ir_annot_t *open_cast = in->op == IR_OP_CAST
        ? ir_annot_get(&in->annots, N00BB_OPEN_CAST_KEY) : nullptr;
    parse_sym_entry_t *flag = in->op == IR_OP_EXTRACT && in->dest && operand
        && in->n_operands == 2 && in->operands[1].kind == IR_OPD_NAME
        ? n00bb_flagset_flag(rewrite->dispatch, operand->type,
                             in->operands[1].name) : nullptr;
    if (flag)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_flag_test(rewrite, in, operand, flag));
    bool variant = false;
    parse_sym_entry_t *arm = in->op == IR_OP_EXTRACT && in->dest && operand
        && in->n_operands == 2 && in->operands[1].kind == IR_OPD_NAME
        ? n00bb_case_member(rewrite->dispatch, operand->type,
                            in->operands[1].name, &variant) : nullptr;
    if (arm && variant)
        return n00bb_rewrite_payload(rewrite, in, operand);
    if (in->op == IR_OP_EXTRACT && in->dest && operand
        && n00bb_reference_definition(types, operand->type))
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_object_field(rewrite, in));
    if (in->op == IR_OP_FIELD && operand
        && th_kind_of(th_resolve(operand->type)) == TH_INDIRECTION
        && n00bb_reference_definition(types,
               th_ind_referent(th_resolve(operand->type))))
        return n00bb_rewrite_object_field_address(rewrite, in);
    if (open_cast && in->dest && operand)
        return n00bb_rewrite_replace(rewrite, in,
            n00bb_rewrite_cast(rewrite, in, open_cast));
    if (n00bb_open_finalize_call(in))
        return n00bb_rewrite_finalize(rewrite, in);
    if (in->op == IR_OP_CALLBACK_CREATE)
        return n00bb_rewrite_callback_setup(rewrite, in);
    bool arm_store = false;
    if (in->op == IR_OP_STORE) {
        if (!n00bb_rewrite_arm_store(rewrite, in, &arm_store)) return false;
        if (arm_store) return true;
    }
    if (in->op == IR_OP_RETURN)
        return n00bb_rewrite_return_layout(rewrite, in);
    if (in->op == IR_OP_DISPATCH && in->dest && in->n_operands >= 2
        && in->operands[0].kind == IR_OPD_NAME)
        return n00bb_rewrite_dispatch(rewrite, in);
    if ((in->op == IR_OP_CALL || in->op == IR_OP_LOAD
         || in->op == IR_OP_EXTRACT || in->op == IR_OP_EXTRACT_INDEX)
        && in->dest
        && !n00bb_set_reference_access(rewrite->ir_context, in->dest,
                                       in->dest->type))
        return false;
    bool arithmetic = in->op == IR_OP_CAST || in->op == IR_OP_ADD
        || in->op == IR_OP_SUB || in->op == IR_OP_MUL || in->op == IR_OP_DIV
        || in->op == IR_OP_REM || in->op == IR_OP_NEG;
    return !arithmetic || !in->dest || !operand
        || n00bb_rewrite_overflow(rewrite, in, operand);
}

typedef struct {
    n00bb_link_analysis_t *analysis;
    parse_grammar_t *grammar;
    n00bb_typing_t *typing;
    core_dict_t by_function;
    core_dict_t sources;
    n00bb_comparison_t comparison;
    core_dict_t modules;
    core_dict_t requests;
    core_list_t(n00bb_ir_instr_ptr_t) request_calls;
    core_dict_t doomed;
    core_dict_t failed;
    core_list_t(n00bb_ir_instr_ptr_t) pruned;
    core_list_t(n00bb_ir_instr_ptr_t) demotions;
    core_list_t(n00bb_rewrite_ptr_t) rewrites;
    core_list_t(n00bb_ir_instr_ptr_t) retyped;
} n00bb_rewriter_t;

static n00bb_rewrite_t *
n00bb_rewrite_for_function(n00bb_rewriter_t *rewriter, ir_func_t *function)
{
    if (!function || !function->n_blocks) return nullptr;
    bool found = false;
    n00bb_rewrite_t *rewrite = core_dict_get(&rewriter->by_function,
                                             function, &found);
    if (found) return rewrite;
    core_translation_unit_t *unit = core_dict_get(&rewriter->modules,
                                                  function->owner, nullptr);
    rewrite = unit ? core_alloc(n00bb_rewrite_t) : nullptr;
    if (!rewrite || !n00bb_rewrite_open(rewrite, rewriter->analysis,
                                        rewriter->grammar, unit, function)) {
        core_free(rewrite);
        return nullptr;
    }
    rewrite->pdg = ir_group_pdg_func(rewriter->typing->group, function);
    rewrite->retyped = &rewriter->retyped;
    rewrite->sources = &rewriter->sources;
    rewrite->comparison = &rewriter->comparison;
    rewrite->dispatch = rewriter->typing->dispatch;
    rewrite->requests = &rewriter->requests;
    rewrite->request_calls = &rewriter->request_calls;
    rewrite->pruned = &rewriter->pruned;
    rewrite->doomed = &rewriter->doomed;
    rewrite->demotions = &rewriter->demotions;
    core_dict_put(&rewriter->by_function, function, rewrite);
    core_list_push(rewriter->rewrites, rewrite);
    return rewrite;
}

static n00bb_rewrite_t *
n00bb_rewrite_for(n00bb_rewriter_t *rewriter, ir_instr_t *in)
{
    return in && in->block
        ? n00bb_rewrite_for_function(rewriter, in->block->owner) : nullptr;
}

static bool
n00bb_returned_literals(n00bb_rewrite_t *rewrite, ir_func_t *callee,
                        core_list_t(n00bb_ir_instr_ptr_t) *returns,
                        core_list_t(n00bb_ir_instr_ptr_t) *literals)
{
    for (uint32_t b = 0; b < callee->n_blocks; b++) {
        ir_block_t *block = callee->blocks[b];
        ir_instr_t *last = block->n_instrs
            ? block->instrs[block->n_instrs - 1] : nullptr;
        if (!last || last->op != IR_OP_RETURN) continue;
        ir_instr_t *origin = last->n_operands
            ? n00bb_value_origin(rewrite->pdg,
                                 ir_operand_value(&last->operands[0]))
            : nullptr;
        if (!origin || origin->op != IR_OP_AGGREGATE) return false;
        core_list_push(*returns, last);
        core_list_push(*literals, origin);
    }
    return returns->len > 0;
}

static void
n00bb_settle_returns(n00bb_rewriter_t *rewriter)
{
    for (size_t i = 0; i < rewriter->request_calls.len; i++) {
        ir_func_t *callee = ir_call_function(rewriter->request_calls.data[i]);
        th_type *target = core_dict_get(&rewriter->requests, callee, nullptr);
        th_type *signature = th_resolve(callee->type);
        th_type *result = th_kind_of(signature) == TH_FUNCTION
            ? th_func_ret(signature) : nullptr;
        n00bb_rewrite_t *rewrite = target && result
            && th_kind_of(result) == TH_VAR
            ? n00bb_rewrite_for_function(rewriter, callee) : nullptr;
        core_list_t(n00bb_ir_instr_ptr_t) returns =
            core_list_new(n00bb_ir_instr_ptr_t);
        core_list_t(n00bb_ir_instr_ptr_t) literals =
            core_list_new(n00bb_ir_instr_ptr_t);
        bool settled = rewrite && rewrite->pdg
            && n00bb_returned_literals(rewrite, callee, &returns, &literals);
        for (size_t k = 0; settled && k < literals.len; k++)
            settled = n00bb_retype_literal(rewrite, literals.data[k], target);
        for (size_t k = 0; settled && k < returns.len; k++)
            for (uint32_t e = 0; e < ir_group_pdg_out_count(
                     rewriter->typing->group, returns.data[k]); e++) {
                const ir_dep_edge_t *edge = ir_group_pdg_out_at(
                    rewriter->typing->group, returns.data[k], e);
                n00bb_rewrite_t *caller = edge->kind == IR_DEP_RETURN
                    ? n00bb_rewrite_for(rewriter, edge->to) : nullptr;
                if (caller && caller->pdg)
                    n00bb_retype_closure(caller, edge->to, target);
            }
        if (settled) th_var_rebind(result, target);
        core_list_free(returns);
        core_list_free(literals);
    }
}

static void
n00bb_doom(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    if (!core_dict_get(rewrite->doomed, in, nullptr)) {
        core_dict_put(rewrite->doomed, in, in);
        core_list_push(*rewrite->pruned, in);
    }
}

static bool
n00bb_doomed_uses(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    for (uint32_t i = 0; i < ir_pdg_out_count(rewrite->pdg, in); i++) {
        const ir_dep_edge_t *edge = ir_pdg_out_at(rewrite->pdg, in, i);
        if (edge->kind == IR_DEP_DATA
            && !core_dict_get(rewrite->doomed, edge->to, nullptr))
            return false;
    }
    return true;
}

static void
n00bb_doom_writes(n00bb_rewrite_t *rewrite, ir_instr_t *reader)
{
    for (uint32_t i = 0; i < ir_pdg_in_count(rewrite->pdg, reader); i++) {
        const ir_dep_edge_t *edge = ir_pdg_in_at(rewrite->pdg, reader, i);
        ir_instr_t *write = edge->from;
        if (edge->kind != IR_DEP_DATA || !edge->via
            || write->dest_kind != IR_DEST_LOCAL
            || !n00bb_doomed_uses(rewrite, write))
            continue;
        n00bb_doom(rewrite, write);
        ir_value_t *source = n00bb_operand_value(write, 0);
        if (source && source->def && source->def->op == IR_OP_CONST
            && n00bb_doomed_uses(rewrite, source->def))
            n00bb_doom(rewrite, source->def);
    }
}

static bool
n00bb_plan_scaffold(n00bb_rewrite_t *rewrite, ir_instr_t *branch)
{
    ir_block_t *finalize = branch->op == IR_OP_BRANCH
        && branch->n_operands == 3 ? branch->operands[1].block : nullptr;
    ir_instr_t *call = nullptr;
    for (uint32_t i = 0; finalize && !call && i < finalize->n_instrs; i++)
        if (n00bb_open_finalize_call(finalize->instrs[i]))
            call = finalize->instrs[i];
    ir_value_t *address = nullptr;
    n00bb_pending_consume(call, &address);
    th_type *type = address
        ? th_ind_referent(th_resolve(address->type)) : nullptr;
    if (!type || !rewrite->pdg
        || n00bb_type_may_finalize(rewrite->unit, type))
        return true;
    n00bb_doom(rewrite, branch);
    for (uint32_t i = 0; i < finalize->n_instrs; i++)
        n00bb_doom(rewrite, finalize->instrs[i]);
    n00bb_doom_writes(rewrite, branch);
    for (uint32_t i = 0; i < finalize->n_instrs; i++)
        n00bb_doom_writes(rewrite, finalize->instrs[i]);
    return true;
}

static bool
n00bb_apply_pruning(core_list_t(n00bb_ir_instr_ptr_t) *pruned)
{
    bool ok = true;
    for (size_t i = 0; i < pruned->len; i++) {
        ir_instr_t *in = pruned->data[i];
        if (in->op != IR_OP_BRANCH) continue;
        ir_block_t *finalize = in->operands[1].block;
        in->op = IR_OP_JUMP;
        in->operands[0] = in->operands[2];
        in->n_operands = 1;
        while (finalize->n_instrs)
            ok = ir_instr_erase(finalize->instrs[0]) && ok;
        ok = ir_block_erase(finalize) && ok;
    }
    for (size_t i = 0; i < pruned->len; i++)
        if (pruned->data[i]->block && pruned->data[i]->op != IR_OP_JUMP)
            ok = ir_instr_erase(pruned->data[i]) && ok;
    return ok;
}

static bool
n00bb_settle_outcome(n00bb_rewrite_t *rewrite, ir_instr_t *call)
{
    ir_func_t *target = ir_call_function(call);
    th_type *signature = ir_instr_call_signature(call);
    if (!signature && target) signature = target->type;
    th_type *outcome = signature && call->dest
        ? ir_call_outcome_type(rewrite->context.module, signature) : nullptr;
    if (outcome && rewrite->pdg)
        n00bb_retype_closure(rewrite, call, outcome);
    return true;
}

static bool
n00bb_rewrite_failed(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    core_string_t op = ir_op_spelling(in->op);
    ir_value_t *subject = in->n_operands > 1
        ? ir_operand_value(&in->operands[1])
        : in->n_operands ? ir_operand_value(&in->operands[0]) : nullptr;
    core_string_t method = in->op == IR_OP_DISPATCH && in->n_operands
        && in->operands[0].kind == IR_OPD_NAME ? in->operands[0].name
        : CORE_STRING_STATIC("");
    core_string_t type = subject && subject->type
        ? th_print(ir_module_types(rewrite->context.module), subject->type)
        : CORE_STRING_STATIC("?");
    n00bb_internal_error(core_translation_unit_diagnostics(rewrite->unit),
        rewrite->unit, n00bb_span_location(in->span), "rewriting typed IR",
        "could not rewrite `%.*s` %.*s on `%.*s`", (int)op.u8_bytes, op.data,
        (int)method.u8_bytes, method.data, (int)type.u8_bytes, type.data);
    return false;
}

static bool
n00bb_rewrite_sites(n00bb_rewriter_t *rewriter,
                    core_list_t(n00bb_ir_instr_ptr_t) *sites,
                    n00bb_site_fn act)
{
    bool ok = true;
    for (size_t i = 0; i < sites->len; i++) {
        ir_instr_t *in = sites->data[i];
        bool failed_before = false;
        core_dict_get(&rewriter->failed, in, &failed_before);
        if (failed_before) {
            ok = false;
            continue;
        }
        n00bb_rewrite_t *rewrite = n00bb_rewrite_for(rewriter, in);
        if (!rewrite) continue;
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(rewrite->unit);
        size_t failures = core_diagnostics_failures(diagnostics);
        bool acted = act(rewrite, in);
        if (!acted && core_diagnostics_failures(diagnostics) == failures)
            n00bb_rewrite_failed(rewrite, in);
        if (!acted) core_dict_put(&rewriter->failed, in, in);
        ok = acted && ok;
    }
    return ok;
}

static bool
n00bb_plan_sinks(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    return n00bb_rewrite_sinks(rewrite, in, n00bb_plan_operand);
}

static bool
n00bb_rewrite_atomic_field_read(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_value_t *base = in->n_operands == 2
            && in->operands[1].kind == IR_OPD_NAME
        ? ir_operand_value(&in->operands[0]) : nullptr;
    th_type *shape = base ? n00bb_record_shape(tu_state(rewrite->unit),
                                               th_resolve(base->type))
                          : nullptr;
    th_type *field = shape ? th_record_lookup(shape, in->operands[1].name)
                           : nullptr;
    if (!field || !n00bb_atomic_element_type(field)) return true;
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *place = n00bb_rewrite_place(rewrite, base, in->span);
    ir_instr_t *member = place
        ? ir_lower_emit_instruction(context, IR_OP_FIELD, in->span) : nullptr;
    if (!member || !ir_lower_set_temp_dest(context, member,
            CORE_STRING_STATIC("__n00bb_atomic_field_"),
            th_ind(ir_module_types(context->module), 0, field))
        || !ir_instr_operand_value(member, place)
        || !ir_instr_operand_name(member, in->operands[1].name))
        return false;
    ir_value_t *read = n00bb_emit_indirect_read(context, member->dest, field,
                                                in->span);
    return read && n00bb_rewrite_replace(rewrite, in, read);
}

static ir_value_t *
n00bb_atomic_assignment_value(ir_instr_t *in, th_type **slot)
{
    bool global = in->op == IR_OP_GLOBAL_STORE && in->n_operands == 2;
    bool local = in->op == IR_OP_COPY && in->dest_kind == IR_DEST_LOCAL
        && in->dest_local && in->n_operands == 1;
    ir_value_t *target = global ? ir_operand_value(&in->operands[0])
                                : nullptr;
    *slot = target && target->global ? target->global->type
        : local ? in->dest_local->type : nullptr;
    ir_value_t *value = *slot && n00bb_atomic_element_type(*slot)
        ? ir_operand_value(&in->operands[global ? 1 : 0]) : nullptr;
    return value && !n00bb_atomic_element_type(value->type) ? value : nullptr;
}

static bool
n00bb_rewrite_atomic_assignment(n00bb_rewrite_t *rewrite, ir_instr_t *in,
                                ir_value_t *value, th_type *slot)
{
    ir_lower_context_t *context = &rewrite->context;
    ir_instr_t *address = ir_lower_emit_instruction(context,
                                                    IR_OP_ADDRESS_OF, in->span);
    bool ok = address && ir_lower_set_temp_dest(context, address,
            CORE_STRING_STATIC("__n00bb_atomic_address_"),
            th_ind(ir_module_types(context->module), 0, slot))
        && (in->op == IR_OP_COPY
                ? ir_instr_operand_local(address, in->dest_local)
                : ir_instr_operand_value(address,
                                         ir_operand_value(&in->operands[0])));
    ir_value_t *converted = ok
        ? n00bb_convert(rewrite, in, value, n00bb_atomic_element_type(slot))
        : nullptr;
    ir_instr_t *store = converted
        ? ir_lower_emit_instruction(context, IR_OP_STORE, in->span) : nullptr;
    bool declaration = ir_annot_get(&in->annots, N00BB_DECLARATION_KEY);
    return store && ir_instr_operand_value(store, address->dest)
        && ir_instr_operand_value(store, converted)
        && ir_instr_add_mod(store, CORE_STRING_STATIC("atomic"))
        && (!declaration || ir_annot_set(ir_module_ctx(context->module),
                &store->annots, (ir_annot_t){.key = N00BB_DECLARATION_KEY,
                                             .kind = IR_ANNOT_BOOL,
                                             .b = true}))
        && n00bb_rewrite_splice(rewrite, in) && ir_instr_erase(in);
}

static bool
n00bb_rewrite_call_output(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    size_t ordinal = 0;
    const ir_instr_t *source = ir_call_output_source(in, &ordinal);
    th_type *output = source ? ir_call_output_type(
        ir_call_function_type(source), ordinal) : nullptr;
    th_type *wanted = in->dest ? in->dest->type : nullptr;
    ir_module_t *module = rewrite->context.module;
    if (!output || !wanted || !th_is_closed(ir_module_types(module), output)
        || ir_type_eq(module, output, wanted))
        return true;
    in->dest->type = output;
    ir_value_t *converted = n00bb_convert(rewrite, in, in->dest, wanted);
    if (!converted || rewrite->context.block != rewrite->scratch)
        return false;
    ir_value_replace_uses(rewrite->context.function, in->dest, converted,
                          n00bb_outside_scratch, rewrite->scratch);
    return n00bb_rewrite_splice_after(rewrite, in);
}

static bool
n00bb_emit_sinks(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    if (in->op == IR_OP_BRANCH) return true;
    if (in->op == IR_OP_CALL_OUTPUT)
        return n00bb_rewrite_call_output(rewrite, in);
    if (in->op == IR_OP_EXTRACT)
        return n00bb_rewrite_atomic_field_read(rewrite, in);
    th_type *slot = nullptr;
    ir_value_t *atomic = n00bb_atomic_assignment_value(in, &slot);
    if (atomic)
        return n00bb_rewrite_atomic_assignment(rewrite, in, atomic, slot);
    if (!n00bb_rewrite_sinks(rewrite, in, n00bb_rewrite_operand)) return false;
    if ((in->op == IR_OP_LOAD || in->op == IR_OP_STORE) && in->n_operands
        && n00bb_atomic_element_type(th_ind_referent(th_resolve(
               ir_operand_value(&in->operands[0])->type)))
        && !ir_instr_has_mod(in, CORE_STRING_STATIC("atomic"))
        && !ir_instr_add_mod(in, CORE_STRING_STATIC("atomic")))
        return false;
    return in->op != IR_OP_AGGREGATE || !in->dest
        || !n00bb_object_type(ir_module_types(rewrite->context.module),
                              in->dest->type)
        || n00bb_rewrite_object_literal(rewrite, in);
}

static bool
n00bb_plan_site(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    return in->op != IR_OP_BRANCH || n00bb_plan_scaffold(rewrite, in);
}

static ir_local_t *
n00bb_open_storage_local(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    return in->op == IR_OP_ALLOC
        ? core_dict_get(&tu_state(rewrite->unit)->open_storage, in, nullptr)
        : nullptr;
}

static bool
n00bb_placement_automatic(n00bb_rewrite_t *rewrite, th_type *type)
{
    ir_module_t *module = rewrite->context.module;
    const ir_final_layout_t *published = ir_layout_final_visible(module,
        type, ir_module_target(module));
    uint64_t size = 0;
    bool known = published && ir_layout_final_byte_extent(published, &size);
    if (!known && ir_layout_compute(module, type, nullptr)) {
        ir_descriptor_t *layout = ir_layout_of(module, type,
                                               ir_module_target(module));
        known = layout && ir_layout_is_settled(&layout->size);
        if (known) size = layout->size.value;
    }
    bool dynamic = false;
    n00bb_storage_decision_t decision = n00bb_storage_choose_placement(
        &(n00bb_storage_placement_facts_t){
            .has_lexical_lifetime = true,
            .size_known = known,
            .size_bytes = size,
            .stack_limit_bytes = N00BB_STACK_OBJECT_LIMIT_BYTES,
        }, &dynamic);
    return known && decision < N00BB_STORAGE_INVALID_TARGET && !dynamic;
}

static bool
n00bb_plan_demotion(n00bb_rewrite_t *rewrite, ir_instr_t *in)
{
    ir_local_t *local = in->dest ? n00bb_open_storage_local(rewrite, in)
                                 : nullptr;
    th_ctx *types = ir_module_types(rewrite->context.module);
    th_type *type = local ? th_resolve(local->type) : nullptr;
    if (!type || !rewrite->pdg || n00bb_atomic_element_type(type)
        || (th_is_closed(types, type)
            && !n00bb_placement_automatic(rewrite, type)))
        return true;
    size_t first = rewrite->demotions->len;
    core_list_push(*rewrite->demotions, in);
    for (uint32_t i = 0; i < ir_pdg_out_count(rewrite->pdg, in); i++) {
        const ir_dep_edge_t *edge = ir_pdg_out_at(rewrite->pdg, in, i);
        ir_instr_t *user = edge->kind == IR_DEP_DATA ? edge->to : nullptr;
        if (!user || core_dict_get(rewrite->doomed, user, nullptr)) continue;
        bool store = user->op == IR_OP_STORE && user->n_operands == 2
            && ir_operand_value(&user->operands[0]) == in->dest
            && ir_operand_value(&user->operands[1]) != in->dest;
        bool load = user->op == IR_OP_LOAD && user->n_operands == 1
            && ir_operand_value(&user->operands[0]) == in->dest;
        if (!store && !load) {
            rewrite->demotions->len = first;
            return true;
        }
        core_list_push(*rewrite->demotions, user);
    }
    core_list_push(*rewrite->demotions, (ir_instr_t *)nullptr);
    return true;
}

static bool
n00bb_demote(n00bb_rewrite_t *rewrite, ir_instr_t *storage,
             ir_instr_t *const *users, size_t count)
{
    ir_local_t *local = n00bb_open_storage_local(rewrite, storage);
    ir_lower_context_t *context = &rewrite->context;
    ir_value_t *value = local ? ir_local_value(context->function, local)
                              : nullptr;
    bool ok = value != nullptr;
    for (size_t i = 0; ok && i < count; i++) {
        ir_instr_t *user = users[i];
        if (!user->block) continue;
        if (user->op == IR_OP_STORE) {
            user->op = IR_OP_COPY;
            user->operands[0] = user->operands[1];
            user->n_operands = 1;
            ir_instr_use_added(user, 0);
            ok = ir_instr_set_dest_local(user, local);
        }
        else {
            user->op = IR_OP_COPY;
            user->operands[0].value = value;
            ir_instr_use_added(user, 0);
            ok = n00bb_set_reference_access(rewrite->ir_context, user->dest,
                                            user->dest->type);
        }
    }
    return ok && ir_instr_erase(storage);
}

static bool
n00bb_apply_demotions(n00bb_rewriter_t *rewriter)
{
    bool ok = true;
    core_list_t(n00bb_ir_instr_ptr_t) *plan = &rewriter->demotions;
    for (size_t i = 0; i < plan->len;) {
        size_t start = i;
        while (plan->data[i]) i++;
        n00bb_rewrite_t *rewrite = n00bb_rewrite_for(rewriter,
                                                     plan->data[start]);
        ok = rewrite && n00bb_demote(rewrite, plan->data[start],
                                     plan->data + start + 1, i - start - 1)
            && ok;
        i++;
    }
    return ok;
}

static bool
n00bb_erase_abandoned_reads(n00bb_rewrite_t *rewrite)
{
    bool erased = rewrite->abandoned.len > 0;
    while (erased) {
        erased = false;
        core_dict_t used;
        core_dict_init(&used, nullptr, nullptr);
        ir_func_used_values(rewrite->context.function, &used);
        for (size_t i = 0; i < rewrite->abandoned.len; i++) {
            ir_value_t *value = rewrite->abandoned.data[i];
            ir_instr_t *def = value ? value->def : nullptr;
            bool pure_copy = def && def->op == IR_OP_COPY
                && def->dest_kind != IR_DEST_LOCAL;
            if (!def || !def->n_operands || core_dict_contains(&used, value)
                || (def->op != IR_OP_LOAD && def->op != IR_OP_EXTRACT
                    && def->op != IR_OP_FIELD && !pure_copy))
                continue;
            ir_value_t *source = ir_operand_value(&def->operands[0]);
            if (!ir_instr_erase(def)) {
                core_dict_free(&used);
                return false;
            }
            rewrite->abandoned.data[i] = source;
            erased = true;
        }
        core_dict_free(&used);
    }
    return true;
}

bool
n00bb_rewrite_typed_ir(n00bb_link_analysis_t *analysis,
                       parse_grammar_t *grammar, n00bb_typing_t *typing)
{
    n00bb_rewriter_t rewriter = {
        .analysis = analysis,
        .grammar = grammar,
        .typing = typing,
        .rewrites = core_list_new(n00bb_rewrite_ptr_t),
        .retyped = core_list_new(n00bb_ir_instr_ptr_t),
        .pruned = core_list_new(n00bb_ir_instr_ptr_t),
        .demotions = core_list_new(n00bb_ir_instr_ptr_t),
    };
    core_dict_init(&rewriter.by_function, nullptr, nullptr);
    core_dict_init(&rewriter.sources, nullptr, nullptr);
    core_dict_init(&rewriter.requests, nullptr, nullptr);
    core_dict_init(&rewriter.modules, nullptr, nullptr);
    for (size_t u = 0; u < analysis->units.len; u++)
        core_dict_put(&rewriter.modules,
                      tu_state(analysis->units.data[u])->ir_module,
                      analysis->units.data[u]);
    core_dict_init(&rewriter.doomed, nullptr, nullptr);
    core_dict_init(&rewriter.failed, nullptr, nullptr);
    rewriter.request_calls = core_list_new(n00bb_ir_instr_ptr_t);
    bool ok = n00bb_rewrite_sites(&rewriter, &typing->outcome_calls,
                                  n00bb_settle_outcome)
        && n00bb_rewrite_sites(&rewriter, &typing->conversions,
                               n00bb_plan_sinks);
    n00bb_settle_returns(&rewriter);
    for (size_t u = 0; u < analysis->units.len; u++)
        ok = n00bb_rewrite_sites(&rewriter,
                &tu_state(analysis->units.data[u])->rewrite_sites,
                n00bb_plan_site) && ok;
    for (size_t u = 0; u < analysis->units.len; u++)
        ok = n00bb_rewrite_sites(&rewriter,
                &tu_state(analysis->units.data[u])->rewrite_sites,
                n00bb_plan_demotion) && ok;
    for (size_t i = 0; i < rewriter.rewrites.len; i++)
        rewriter.rewrites.data[i]->pdg = nullptr;
    ok = n00bb_apply_pruning(&rewriter.pruned) && ok;
    ok = n00bb_apply_demotions(&rewriter) && ok;
    for (size_t u = 0; u < analysis->units.len; u++)
        ok = n00bb_rewrite_sites(&rewriter,
                &tu_state(analysis->units.data[u])->rewrite_sites,
                n00bb_rewrite_instruction) && ok;
    ok = n00bb_rewrite_sites(&rewriter, &typing->conversions,
                             n00bb_emit_sinks)
        && n00bb_rewrite_sites(&rewriter, &rewriter.retyped, n00bb_emit_sinks)
        && ok;
    for (size_t i = 0; i < rewriter.rewrites.len; i++) {
        n00bb_rewrite_t *rewrite = rewriter.rewrites.data[i];
        ok = ir_block_erase(rewrite->scratch)
            && n00bb_erase_abandoned_reads(rewrite) && ok;
        core_list_free(rewrite->abandoned);
        core_free(rewrite->lower);
        core_free(rewrite);
    }
    for (size_t u = 0; ok && u < analysis->units.len; u++)
        core_dict_foreach(&tu_state(analysis->units.data[u])->intrinsics, item)
            ok = ir_func_erase(item->key) && ok;
    for (size_t u = 0; ok && u < analysis->units.len; u++) {
        ir_module_t *module = tu_state(analysis->units.data[u])->ir_module;
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);
            for (uint32_t l = 0; ok && l < function->n_locals; l++) {
                ir_value_t *local = ir_local_value(function,
                                                   function->locals[l]);
                ok = !local || ir_annot_get(ir_value_annots(local),
                        CORE_STRING_STATIC("ir.indirect_access"))
                    || n00bb_set_reference_access(analysis->ir_context, local,
                                                  function->locals[l]->type);
            }
        }
    }
    for (size_t u = 0; ok && u < analysis->units.len; u++) {
        core_translation_unit_t *unit = analysis->units.data[u];
        ir_module_t *module = tu_state(unit)->ir_module;
        for (size_t f = 0; module && !tu_state(unit)->interface_module
                 && f < ir_func_count(module); f++)
            ok = n00bb_finalize_temporaries(analysis, grammar, unit,
                                            ir_func_at(module, f)) && ok;
    }
    core_list_free(rewriter.rewrites);
    core_list_free(rewriter.retyped);
    core_dict_free(&rewriter.by_function);
    core_dict_free(&rewriter.sources);
    core_dict_free(&rewriter.requests);
    core_dict_free(&rewriter.modules);
    core_list_free(rewriter.request_calls);
    core_dict_free(&rewriter.doomed);
    core_dict_free(&rewriter.failed);
    core_list_free(rewriter.pruned);
    core_list_free(rewriter.demotions);
    return ok;
}
