#include "compiler_types.h"

#include "cached_body.h"
#include "call_plan.h"
#include "compiler_link.h"
#include "compiler_lower_emit.h"
#include "compiler_tu.h"
#include "compiler_visibility.h"
#include "diagnostics.h"
#include "field_extent.h"
#include "ir/operands.h"
#include "ir/pass.h"
#include "ir/pdg.h"
#include "lib/list.h"
#include "logic/datalog.h"
#include "lower/lower_internal.h"
#include "th/environment.h"
#include "types/constrain.h"
#include "types/refine.h"

static const core_string_t integer_names[] = {
    CORE_STRING_STATIC("int"),  CORE_STRING_STATIC("uint"),
    CORE_STRING_STATIC("i8"),   CORE_STRING_STATIC("i16"),
    CORE_STRING_STATIC("i32"),  CORE_STRING_STATIC("i64"),
    CORE_STRING_STATIC("i128"), CORE_STRING_STATIC("u8"),
    CORE_STRING_STATIC("u16"),  CORE_STRING_STATIC("u32"),
    CORE_STRING_STATIC("u64"),  CORE_STRING_STATIC("u128"),
    CORE_STRING_STATIC("byte"),
};

static const core_string_t float_names[] = {
    CORE_STRING_STATIC("float"),
    CORE_STRING_STATIC("f32"),
    CORE_STRING_STATIC("f64"),
};

static core_translation_unit_t *
unit_of_module(core_translation_unit_t *const *units, size_t count,
               const ir_module_t *module);

static core_translation_unit_t *unit_of(core_translation_unit_t *const *units,
                                        size_t count, const ir_instr_t *in)
{
    return unit_of_module(units, count,
                          in && in->block && in->block->owner
                              ? in->block->owner->owner : nullptr);
}

static bool poisoned(n00bb_link_analysis_t *analysis, th_type *type)
{
    th_type *resolved = type ? th_resolve(type) : nullptr;

    if (!resolved || th_mentions_poison(resolved)) {
        return resolved != nullptr;
    }
    if (th_kind_of(resolved) != TH_NAMED) {
        return false;
    }

    core_translation_unit_t *owner  = nullptr;
    parse_scope_t           *scope  = record_scope_for_type(analysis, resolved,
                                                            &owner);
    parse_sym_entry_t       *record = scope && scope->node
                                        ? parse_tree_sym(scope->node)
                                        : nullptr;

    return record && parse_sym_binding(record, N00BB_POISONED_KEY);
}

static bool literal_failure(const ty_failure_t *failure)
{
    return failure->why.at && failure->why.at->op == IR_OP_CONST
        && failure->kind == TY_FAIL_PREDICATE;
}

static bool reads_tainted(core_dict_t *tainted, const ir_instr_t *in)
{
    for (uint32_t o = 0; o < in->n_operands; o++) {
        const ir_value_t *value = ir_operand_referenced_value(&in->operands[o]);

        if (value && core_dict_contains(tainted, (void *)value)) {
            return true;
        }
    }

    return false;
}

static bool taint(core_dict_t *tainted, const ir_value_t *value)
{
    if (!value || core_dict_contains(tainted, (void *)value)) {
        return false;
    }
    core_dict_put(tainted, (void *)value, (void *)1);

    return true;
}

static bool spread_taint(core_dict_t *tainted, const ir_func_t *function)
{
    bool changed = false;

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];

        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *in = block->instrs[i];

            if (!reads_tainted(tainted, in)) {
                continue;
            }
            changed = taint(tainted, in->dest) || changed;
            if ((in->op == IR_OP_STORE || in->op == IR_OP_GLOBAL_STORE)
                && in->n_operands == 2) {
                changed = taint(tainted, ir_operand_value(&in->operands[0]))
                       || changed;
            }
        }
    }

    return changed;
}

static const ir_func_t *reach(const ir_instr_t *origin, core_dict_t *reached)
{
    const ir_func_t *function = origin && origin->block ? origin->block->owner
                                                        : nullptr;

    core_dict_init(reached, nullptr, nullptr);
    for (bool changed = function && taint(reached, origin->dest); changed;) {
        changed = spread_taint(reached, function);
    }

    return function;
}

static bool flows_from(const ir_instr_t *origin, const ir_value_t *value)
{
    core_dict_t reached;
    bool        found = reach(origin, &reached) && value
                     && core_dict_contains(&reached, (void *)value);

    core_dict_free(&reached);

    return found;
}

static bool meets_taint(core_dict_t *tainted, const ir_instr_t *origin)
{
    const ir_func_t *function = origin->block ? origin->block->owner : nullptr;
    core_dict_t      used;
    bool             meets    = false;

    core_dict_init(&used, nullptr, nullptr);
    (void)taint(&used, origin->dest);
    for (uint32_t b = 0; function && !meets && b < function->n_blocks; b++) {
        for (uint32_t i = 0; !meets && i < function->blocks[b]->n_instrs;
             i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            meets = reads_tainted(&used, in) && reads_tainted(tainted, in);
        }
    }
    core_dict_free(&used);

    return meets;
}

static core_string_t quoted_type(th_ctx *types, th_type *type)
{
    return n00bb_text(N00B_TEXT_QUOTED,
                      N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, type))));
}

static core_string_t shown(th_ctx *types, th_type *type)
{
    return n00bb_hide_reserved_segments(th_print(types, type));
}

static core_string_t span_text(core_translation_unit_t *unit, ir_span_t span)
{
    core_string_t contents = core_source_file_contents(&unit->source);

    if (!ir_span_known(span) || span.implied
        || span.end_byte <= span.start_byte
        || span.end_byte > contents.u8_bytes) {
        return (core_string_t){0};
    }

    return core_string_slice(contents, span.start_byte,
                             span.end_byte - span.start_byte);
}

typedef struct {
    core_string_t        text;
    core_diag_location_t at;
} source_t;

static source_t value_source(core_translation_unit_t *unit,
                             const ir_value_t *value, ir_span_t within)
{
    const ir_instr_t *def  = value ? value->def : nullptr;
    core_string_t     text = def ? span_text(unit, def->span)
                                 : (core_string_t){0};

    if (text.u8_bytes) {
        return (source_t){text, n00bb_span_location(def->span)};
    }
    core_string_t name = value && value->kind == IR_VAL_PARAM ? value->name
                       : value && value->kind == IR_VAL_GLOBAL && value->global
                           ? value->global->name
                           : (core_string_t){0};

    name = name.data ? n00bb_name_leaf(name) : name;

    core_diag_location_t at = name.u8_bytes
                                ? n00bb_name_location(unit, within, name)
                                : (core_diag_location_t){0};

    return at.has_location ? (source_t){name, at}
                           : (source_t){{0}, n00bb_span_location(within)};
}

core_string_t n00bb_value_text(core_translation_unit_t *unit,
                               const ir_value_t *value, ir_span_t within)
{
    return value_source(unit, value, within).text;
}

static bool report_condition(th_ctx *types, core_translation_unit_t *unit,
                             const ty_failure_t *failure)
{
    const ir_instr_t *at   = failure->why.at;
    ir_annot_t       *mark = at ? ir_annot_get((ir_annots_t *)&at->annots,
                                               N00BB_CONDITION_KEY)
                                : nullptr;
    const ir_value_t *condition = mark && at->n_operands
                                    ? ir_operand_value(&at->operands[0])
                                    : nullptr;
    th_type *found = condition && condition->type
                       ? th_resolve(condition->type)
                       : nullptr;

    if (!found || th_kind_of(found) == TH_VAR) {
        return false;
    }

    core_diagnostics_t  *diagnostics = core_translation_unit_diagnostics(unit);
    n00bb_text_t         construct   = (n00bb_text_t)mark->i.mag;
    core_string_t        text        = span_text(unit, at->span);
    core_string_t        type        = n00bb_print_type(types, found);
    core_diag_location_t where       = n00bb_span_location(at->span);

    if (text.u8_bytes) {
        n00bb_report(diagnostics, unit, N00B_DIAG_CONDITION_NOT_BOOL, where,
                     N00B_TEXT_IS_BUT_MUST_BE_BOOL,
                     N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(type),
                                ARG_PHRASE_ID(construct)));
    }
    else {
        n00bb_report(diagnostics, unit, N00B_DIAG_CONDITION_NOT_BOOL, where,
                     N00B_TEXT_THIS_EXPRESSION_IS_BUT_MUST,
                     N00BB_ARGS(ARG_TEXT(type), ARG_PHRASE_ID(construct)));
    }
    if (th_is_nil(found) || th_option_item(found)) {
        n00bb_report_help(diagnostics, N00B_TEXT_TEST_AN_OPTION_WITH_NIL,
                          N00BB_NO_ARGS);
    }
    else if (text.u8_bytes && is_integer_type(found)) {
        n00bb_report_help(diagnostics, N00B_TEXT_WRITE_THE_COMPARISON_YOU_MEAN,
                          N00BB_ARGS(ARG_TEXT(text)));
    }

    return true;
}

static core_translation_unit_t *
unit_of_module(core_translation_unit_t *const *units, size_t count,
               const ir_module_t *module)
{
    for (size_t i = 0; module && i < count; i++) {
        if (tu_state(units[i])->ir_module == module) {
            return units[i];
        }
    }

    return nullptr;
}

static core_string_t function_name(const ir_func_t *function)
{
    int64_t cut = core_string_find_byte(function->name, '#');

    return cut >= 0 ? core_string_slice(function->name, 0, (size_t)cut)
                    : function->name;
}

static void note_declared(core_translation_unit_t *const *units, size_t count,
                          core_translation_unit_t *unit,
                          const ir_func_t         *function)
{
    core_translation_unit_t *home = unit_of_module(units, count,
                                                   function->owner);
    n00bb_link_analysis_t   *analysis = tu_state(unit)->link_analysis;
    for (size_t u = 0; !home && analysis && function->owner
         && u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        if (state && state->ir_module
            && core_string_eq(ir_module_name(state->ir_module),
                              ir_module_name(function->owner)))
            home = analysis->units.data[u];
    }
    core_string_t            name = function_name(function);

    if (!home || !ir_span_known(function->span)) {
        return;
    }
    n00bb_report_secondary(core_translation_unit_diagnostics(unit), home,
                           n00bb_name_location(home, function->span, name),
                           N00B_TEXT_IS_DECLARED_HERE,
                           N00BB_ARGS(ARG_TEXT(name)));
}

static bool is_reference_to(th_type *reference, th_type *value)
{
    reference = reference ? th_resolve(reference) : nullptr;

    return reference && th_kind_of(reference) == TH_INDIRECTION
        && th_same_type(th_resolve(th_ind_referent(reference)),
                        th_resolve(value));
}

static const ir_instr_t *item_pack(const ir_instr_t *array)
{
    const ir_block_t *block = array->block;

    for (uint32_t i = array->index + 1; block && i < block->n_instrs; i++) {
        const ir_instr_t *pack = block->instrs[i];

        for (uint32_t o = 0; pack->op == IR_OP_AGGREGATE
             && o < pack->n_operands; o++) {
            if (ir_operand_value(&pack->operands[o]) == array->dest) {
                return pack;
            }
        }
    }

    return nullptr;
}

static const ir_instr_t *rest_call(const ir_instr_t *pack,
                                   const ir_value_t **parameter)
{
    const ir_block_t *block = pack->block;

    for (uint32_t i = pack->index + 1; block && i < block->n_instrs; i++) {
        const ir_instr_t *call   = block->instrs[i];
        const ir_func_t  *callee = n00bb_direct_call_target(call);

        uint32_t position = 0;

        for (uint32_t o = 1; callee && o < call->n_operands; o++) {
            uint32_t index = UINT32_MAX;

            const ir_value_t *bound = ir_call_param(
                (ir_func_t *)callee, &call->operands[o], &position, &index);

            if (bound && ir_operand_value(&call->operands[o]) == pack->dest
                && core_string_starts_with(pack->dest->name,
                       CORE_STRING_STATIC("__n00bb_rest_"))) {
                *parameter = bound;
                return call;
            }
        }
    }

    return nullptr;
}

// A method a `where` constraint requires, such as the `get` of
// `Getter[string]`, that the argument's type has, but with another type.
static bool bound_method_failure(const ty_failure_t *failure)
{
    return failure->kind == TY_FAIL_PREDICATE
        && failure->pred.kind == TY_PRED_DISPATCH && failure->pred.bound
        && failure->pred.subject && failure->pred.name.u8_bytes
        && failure->why.at;
}

static bool report_bound_method(th_ctx *types,
                                core_translation_unit_t *const *units,
                                size_t count, core_translation_unit_t *unit,
                                const ty_failure_t *failure)
{
    const ir_instr_t *call   = failure->why.at;
    const ir_func_t  *callee = n00bb_direct_call_target(call);

    if (!bound_method_failure(failure) || !callee) {
        return false;
    }
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t subject = n00bb_print_type(types,
        th_resolve(failure->pred.subject));
    core_string_t wanted = failure->pred.other
        ? n00bb_print_type(types, failure->pred.other)
        : (core_string_t){0};

    n00bb_report(diagnostics, unit, N00B_DIAG_GENERIC_MISSING_IMPLEMENTATION,
                 n00bb_span_location(call->span),
                 N00B_TEXT_REQUIRED_METHOD_DOES_NOT_FIT,
                 N00BB_ARGS(ARG_TEXT(subject),
                            ARG_TEXT(function_name(callee)),
                            ARG_TEXT(failure->pred.name),
                            ARG_TEXT(wanted)));
    note_declared(units, count, unit, callee);

    return true;
}

static bool report_required_trait(th_ctx *types,
                                  core_translation_unit_t *const *units,
                                  size_t count, core_translation_unit_t *unit,
                                  const ty_failure_t *failure)
{
    const ir_instr_t *call   = failure->why.at;
    const ir_func_t  *callee = n00bb_direct_call_target(call);
    bool              dispatched = call && call->op == IR_OP_DISPATCH
        && call->n_operands && call->operands[0].kind == IR_OPD_NAME;

    if (failure->kind != TY_FAIL_PREDICATE
        || failure->pred.kind != TY_PRED_IMPLEMENTS || !failure->pred.subject
        || !failure->pred.name.u8_bytes || (!callee && !dispatched)) {
        return false;
    }
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       subject     = n00bb_print_type(types,
        th_resolve(failure->pred.subject));
    core_string_t       trait       = n00bb_name_leaf(failure->pred.name);
    bool                comparison  = dispatched
        && ir_annot_get((ir_annots_t *)&call->annots, N00BB_COMPARISON_KEY);

    if (comparison) {
        n00bb_report(diagnostics, unit,
                     N00B_DIAG_GENERIC_MISSING_IMPLEMENTATION,
                     n00bb_span_location(call->span),
                     N00B_TEXT_COMPARED_TYPE_MISSING_TRAIT,
                     N00BB_ARGS(ARG_TEXT(subject), ARG_TEXT(trait)));
    }
    else {
        n00bb_report(diagnostics, unit,
                     N00B_DIAG_GENERIC_MISSING_IMPLEMENTATION,
                     n00bb_span_location(call->span),
                     N00B_TEXT_REQUIRED_TRAIT_MISSING,
                     N00BB_ARGS(ARG_TEXT(subject), ARG_TEXT(trait),
                                ARG_TEXT(callee ? function_name(callee)
                                                : call->operands[0].name)));
    }
    if (callee) {
        note_declared(units, count, unit, callee);
    }
    n00bb_report_help(diagnostics, N00B_TEXT_IMPLEMENT_THE_TRAIT_FOR,
                      N00BB_ARGS(ARG_TEXT(trait), ARG_TEXT(subject)));

    return true;
}

static bool report_varargs_array(th_ctx *types,
                                 core_translation_unit_t *const *units,
                                 size_t count, core_translation_unit_t *unit,
                                 const ty_failure_t *failure)
{
    const ir_instr_t *array = failure->why.at;
    const ir_value_t *item  = array ? array->dest : nullptr;
    const ir_instr_t *pack  = item ? item_pack(array) : nullptr;
    const ir_value_t *rest  = nullptr;
    const ir_instr_t *call  = pack && pack->dest ? rest_call(pack, &rest)
                                                 : nullptr;

    if (!call && array && array->op == IR_OP_AGGREGATE && array->dest
        && rest_call(array, &rest)) {
        pack = array;
        item = nullptr;
        for (uint32_t o = 0; !item && o < pack->n_operands; o++) {
            const ir_value_t *value = ir_operand_value(&pack->operands[o]);
            th_type *type = value ? th_resolve(value->type) : nullptr;
            if (type && (th_kind_of(type) == TH_SEQUENCE
                         || th_builtin_kind_of(type) == TH_BUILTIN_ARRAY))
                item = value;
        }
        call = item ? rest_call(pack, &rest) : nullptr;
    }

    if (!call) {
        return false;
    }
    const ir_func_t    *callee      = n00bb_direct_call_target(call);
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    th_type            *element     = rest->type
                                          && th_kind_of(th_resolve(rest->type))
                                                 == TH_SEQUENCE
                                          ? th_seq_elem(th_resolve(rest->type))
                                          : rest->type;

    n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_TYPE,
                 value_source(unit, item, pack->span).at,
                 N00B_TEXT_VARARGS_GIVEN_ARRAY,
                 N00BB_ARGS(ARG_TEXT(function_name(callee)),
                            ARG_TEXT(rest->name)));
    n00bb_report_label(diagnostics, N00B_TEXT_EXPECTED,
                       N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, element))));
    n00bb_report_help(diagnostics, N00B_TEXT_VARARGS_GIVEN_ARRAY_HELP,
                      N00BB_NO_ARGS);
    note_declared(units, count, unit, callee);

    return true;
}

static bool report_argument(th_ctx *types, core_translation_unit_t *const *units,
                            size_t count, core_translation_unit_t *unit,
                            const ty_failure_t *failure)
{
    const ir_instr_t *call   = failure->why.at;
    const ir_func_t  *callee = n00bb_direct_call_target(call);
    th_type          *actual = failure->actual ? th_resolve(failure->actual)
                                               : nullptr;

    if (!callee || !actual || !failure->expected) {
        return false;
    }
    for (uint32_t o = 1; o < call->n_operands; o++) {
        const ir_operand_t *operand = &call->operands[o];
        const ir_value_t   *value   = operand->kind == IR_OPD_NAMED_ARG
                                        ? operand->named.value
                                        : ir_operand_value(operand);
        uint32_t            index   = o - 1;

        if (!value || !th_same_type(th_resolve(value->type), actual)) {
            continue;
        }
        for (uint32_t p = 0; operand->kind == IR_OPD_NAMED_ARG
             && p < callee->n_params; p++) {
            if (core_string_eq(callee->params[p]->name, operand->named.name)) {
                index = p;
            }
        }

        core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(
            unit);
        core_string_t       name        = function_name(callee);
        core_string_t       parameter   = index < callee->n_params
                                            ? callee->params[index]->name
                                            : (core_string_t){0};
        core_string_t       target      = parameter.u8_bytes
            ? n00bb_text(N00B_TEXT_PARAMETER_NAMED_OF,
                         N00BB_ARGS(ARG_NUMBER(index + 1), ARG_TEXT(parameter),
                                    ARG_TEXT(name)))
            : n00bb_text(N00B_TEXT_PARAMETER_NUMBER_OF,
                         N00BB_ARGS(ARG_NUMBER(index + 1), ARG_TEXT(name)));

        th_type      *declared = index < callee->n_params
                                   ? callee->params[index]->type
                                   : failure->expected;
        core_string_t argument = quoted_type(types, actual);

        n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_TYPE,
                     value_source(unit, value, call->span).at,
                     N00B_TEXT_IS_BUT_THIS_ARGUMENT_IS,
                     N00BB_ARGS(ARG_TEXT(target),
                                ARG_TEXT(n00bb_print_type(types, declared)),
                                ARG_TEXT(argument)));
        n00bb_report_label(diagnostics, N00B_TEXT_EXPECTED,
            N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, declared))));
        core_free(argument.data);
        core_free(target.data);
        note_declared(units, count, unit, callee);
        if (is_reference_to(actual, failure->expected)) {
            n00bb_report_help(diagnostics,
                              N00B_TEXT_READ_THE_REFERENCED_VALUE_WITH,
                              N00BB_NO_ARGS);
        }

        return true;
    }

    return false;
}

static bool returned_storage(const ir_instr_t *store)
{
    const ir_value_t *address  = store->op == IR_OP_STORE
                                     && store->n_operands == 2
                                   ? ir_operand_value(&store->operands[0])
                                   : nullptr;
    const ir_func_t  *function = store->block ? store->block->owner : nullptr;

    if (!address || !address->def || address->def->op != IR_OP_ALLOC
        || !function) {
        return false;
    }
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in    = function->blocks[b]->instrs[i];
            const ir_value_t *value = in->op == IR_OP_RETURN && in->n_operands
                                        ? ir_operand_value(&in->operands[0])
                                        : nullptr;
            const ir_instr_t *load  = value ? value->def : nullptr;

            if (load && load->op == IR_OP_LOAD && load->n_operands
                && ir_operand_value(&load->operands[0]) == address) {
                return true;
            }
        }
    }

    return false;
}

static core_diag_location_t return_type_location(core_translation_unit_t *unit,
                                                 const ir_func_t *function)
{
    core_string_t source = core_source_file_contents(&unit->source);
    ir_span_t     span   = function->span;

    if (!ir_span_known(span) || span.end_byte > source.u8_bytes) {
        return (core_diag_location_t){0};
    }
    for (size_t at = span.start_byte; at + 2 < span.end_byte; at++) {
        if (source.data[at] != '-' || source.data[at + 1] != '>') {
            continue;
        }

        size_t start = at + 2;

        while (start < span.end_byte && source.data[start] == ' ') {
            start++;
        }

        size_t end = start;

        while (end < span.end_byte && source.data[end] != ' '
               && source.data[end] != '{' && source.data[end] != '\n') {
            end++;
        }

        return end > start ? core_diag_location_span(source, start, end)
                           : (core_diag_location_t){0};
    }

    return (core_diag_location_t){0};
}

static bool literal_is_initializer(const ir_instr_t *literal)
{
    const ir_func_t *function = literal->block ? literal->block->owner
                                               : nullptr;

    for (uint32_t b = 0; function && b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                if (ir_operand_value(&in->operands[o]) != literal->dest) {
                    continue;
                }

                return in->op == IR_OP_AGGREGATE || returned_storage(in);
            }
        }
    }

    return false;
}

static bool report_literal(th_ctx *types, core_translation_unit_t *unit,
                           const ty_failure_t *failure)
{
    const ir_instr_t *literal = failure->why.at;
    th_type          *string  = th_name(types, CORE_STRING_STATIC("string"));

    if (!literal || literal->op != IR_OP_CONST || !literal->dest
        || failure->kind != TY_FAIL_PREDICATE
        || !core_string_eq(failure->pred.name,
                           CORE_STRING_STATIC("String_Syntax"))
        || !n00bb_string_where_buffer_expected(types, failure->pred.subject,
                                               string)) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       text        = span_text(unit, literal->span);
    bool                initializer = literal_is_initializer(literal);

    n00bb_report(diagnostics, unit, N00B_DIAG_STRING_NOT_BUFFER,
                 n00bb_span_location(literal->span),
                 initializer ? N00B_TEXT_IS_A_STRING_WHICH_IS
                             : N00B_TEXT_IS_A_STRING_BUT_A,
                 N00BB_ARGS(ARG_TEXT(text)));
    n00bb_report_help(diagnostics,
                      initializer ? N00B_TEXT_COPY_IT_INTO_A_BUFFER
                                  : N00B_TEXT_WRITE_BUFFER_FROM_STRING,
                      N00BB_ARGS(ARG_TEXT(text)));

    return true;
}

static bool report_unpropagated(th_ctx *types,
                                core_translation_unit_t *const *units,
                                size_t count, core_translation_unit_t *unit,
                                const ir_value_t *value, th_type *outcome)
{
    const ir_instr_t *call   = value && value->def
                                     && value->def->op == IR_OP_CALL
                                 ? value->def
                                 : nullptr;
    const ir_func_t  *callee = call ? n00bb_direct_call_target(call)
                                    : nullptr;
    bool              variant    = false;
    th_type          *definition = callee
        ? ir_type_definition(call->block->owner->owner, outcome)
        : nullptr;
    th_type          *error      = definition
                                       && th_named_info(th_resolve(outcome),
                                                        nullptr, &variant,
                                                        nullptr)
                                       && variant
                                       && th_kind_of(definition) == TH_RECORD
                                 ? th_record_lookup(definition,
                                                    CORE_STRING_STATIC("error"))
                                 : nullptr;

    if (!error) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       name        = function_name(callee);

    n00bb_report(diagnostics, unit, N00B_DIAG_FAILURE_NOT_PROPAGATED,
                 n00bb_span_location(call->span),
                 N00B_TEXT_CAN_FAIL_WITH_SO_THIS,
                 N00BB_ARGS(ARG_TEXT(name),
                            ARG_TEXT(n00bb_print_type(types, error))));
    note_declared(units, count, unit, callee);
    n00bb_report_help(diagnostics, N00B_TEXT_WRITE_TO_PASS_THE_FAILURE,
                      N00BB_ARGS(ARG_TEXT(name)));

    return true;
}

static bool report_store(th_ctx *types, core_translation_unit_t *const *units,
                         size_t count, core_translation_unit_t *unit,
                         const ty_failure_t *failure)
{
    const ir_instr_t *at    = failure->why.at;
    const ir_value_t *value = nullptr;

    if (!at || failure->kind != TY_FAIL_MISMATCH || !failure->expected
        || ir_annot_get((ir_annots_t *)&at->annots,
                        CORE_STRING_STATIC(N00BB_TRY_ERROR_ANNOTATION))
        || !failure->actual) {
        return false;
    }
    if ((at->op == IR_OP_STORE || at->op == IR_OP_GLOBAL_STORE)
        && at->n_operands == 2) {
        value = ir_operand_value(&at->operands[1]);
    }
    else if (at->op == IR_OP_COPY && at->n_operands == 1) {
        value = ir_operand_value(&at->operands[0]);
    }

    if (report_unpropagated(types, units, count, unit, value,
                            failure->actual)) {
        return true;
    }

    source_t source = value_source(unit, value, at->span);

    if (!source.text.u8_bytes) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       actual = quoted_type(types, failure->actual);
    core_string_t       expected = n00bb_print_type(types, failure->expected);

    if (returned_storage(at)) {
        core_string_t name = function_name(at->block->owner);

        n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
                     source.at, N00B_TEXT_IS_BUT_RETURNS,
                     N00BB_ARGS(ARG_TEXT(source.text), ARG_TEXT(actual),
                                ARG_TEXT(name), ARG_TEXT(expected)));

        core_diag_location_t declared = return_type_location(
            unit, at->block->owner);

        if (declared.has_location) {
            n00bb_report_secondary(diagnostics, unit, declared,
                                   N00B_TEXT_THE_RETURN_TYPE_OF_IS,
                                   N00BB_ARGS(ARG_TEXT(name)));
        }
    }
    else {
        n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
                     source.at, N00B_TEXT_IS_WHICH_DOES_NOT_CONVERT,
                     N00BB_ARGS(ARG_TEXT(source.text), ARG_TEXT(actual),
                                ARG_TEXT(expected)));
    }
    core_free(actual.data);
    if (is_reference_to(failure->actual, failure->expected)) {
        n00bb_report_help(diagnostics, N00B_TEXT_READ_THE_REFERENCED_VALUE_WITH,
                          N00BB_NO_ARGS);
    }

    return true;
}

static void report_case_mismatch(th_ctx *types, core_translation_unit_t *unit,
                                 const ir_instr_t *test)
{
    ir_value_t *subject = test->n_operands > 1
                            ? ir_operand_value(&test->operands[1])
                            : nullptr;
    ir_value_t *value   = test->n_operands > 2
                            ? ir_operand_value(&test->operands[2])
                            : nullptr;
    ir_instr_t *def     = value ? value->def : nullptr;
    th_type    *type    = value ? th_resolve(value->type) : nullptr;
    bool        literal = def && def->op == IR_OP_CONST && def->n_operands
                       && def->operands[0].kind == IR_OPD_CONST
                       && !def->operands[0].lit.text.data;
    core_buffer_t text  = {0};
    ir_annot_t   *kind  = ir_annot_get((ir_annots_t *)&test->annots,
                                       N00BB_CASE_KIND_KEY);
    ir_annot_t   *named = ir_annot_get((ir_annots_t *)&test->annots,
                                       N00BB_CASE_KEY);

    // A bare name that is not a value can only name an arm or an enum
    // member, and this subject has neither.
    if (named && named->kind == IR_ANNOT_STR) {
        n00bb_report(core_translation_unit_diagnostics(unit), unit,
                     N00B_DIAG_UNRESOLVED_NAME, n00bb_span_location(test->span),
                     N00B_TEXT_CASE_NAMES_NO_VALUE,
                     N00BB_ARGS(ARG_TEXT(named->s),
                                ARG_TEXT(subject ? shown(types, subject->type)
                                                 : (core_string_t){0})));
        return;
    }
    if (kind && kind->kind == IR_ANNOT_STR) {
        core_buffer_append_string(&text, kind->s);
    }
    else if (type && n00bb_scalar_role_is(type, IR_ROLE_CHAR)) {
        core_buffer_puts(&text, "a character");
    }
    else if (type
             && th_hash(types, type)
                    == th_hash(types, th_name(types, CORE_STRING_STATIC("bool")))) {
        core_buffer_puts(&text, "a boolean");
    }
    else if (literal && !def->operands[0].lit.is_float
             && (!type || th_kind_of(type) == TH_VAR)) {
        core_buffer_puts(&text, "an integer");
    }
    else {
        core_string_t name = shown(types, type);

        core_buffer_printf(&text, "`%.*s`", (int)name.u8_bytes, name.data);
    }
    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                 N00B_DIAG_VALUE_TYPE_MISMATCH,
                 n00bb_span_location(def ? def->span : test->span),
                 N00B_TEXT_THIS_CASE_IS_BUT_THE_SWITCH,
                 N00BB_ARGS(ARG_TEXT(core_buffer_string(&text)),
                            ARG_TEXT(subject ? shown(types, subject->type)
                                             : (core_string_t){0})));
    core_buffer_release(&text);
}

static const ir_instr_t *case_test_of(const ir_instr_t *at)
{
    if (!at || ir_annot_get((ir_annots_t *)&at->annots, N00BB_SWITCH_KEY)) {
        return at;
    }

    ir_func_t *f = at->dest && at->block ? at->block->owner : nullptr;

    for (uint32_t b = 0; f && b < f->n_blocks; b++) {
        for (uint32_t i = 0; i < f->blocks[b]->n_instrs; i++) {
            ir_instr_t *in = f->blocks[b]->instrs[i];

            if (in->op == IR_OP_DISPATCH && in->n_operands == 3
                && ir_operand_value(&in->operands[2]) == at->dest
                && ir_annot_get(&in->annots, N00BB_SWITCH_KEY)) {
                return in;
            }
        }
    }

    return nullptr;
}

static bool source_parameter(th_type *type)
{
    type               = type ? th_resolve(type) : nullptr;
    core_string_t name = type && th_kind_of(type) == TH_VAR
                           ? th_var_name(type)
                           : (core_string_t){0};

    return name.u8_bytes && core_string_find_byte(name, ' ') < 0
        && core_string_find_byte(name, '@') < 0;
}

static bool report_fixed_parameter(th_ctx *types, core_translation_unit_t *unit,
                                   const ty_failure_t *failure)
{
    th_type *parameter = source_parameter(failure->expected) ? failure->expected
                       : source_parameter(failure->actual)   ? failure->actual
                                                             : nullptr;
    th_type *fixed     = parameter == failure->expected ? failure->actual
                                                        : failure->expected;
    ir_func_t *owner   = failure->why.at && failure->why.at->block
                           ? failure->why.at->block->owner
                           : nullptr;

    if (!parameter || !fixed || !owner || !th_is_closed(types, fixed)) {
        return false;
    }
    n00bb_report(core_translation_unit_diagnostics(unit), unit,
                 N00B_DIAG_TYPE_PARAMETER_FIXED,
                 n00bb_span_location(owner->span),
                 N00B_TEXT_TYPE_PARAMETER_FIXED,
                 N00BB_ARGS(ARG_TEXT(th_var_name(th_resolve(parameter))),
                            ARG_TEXT(owner->name),
                            ARG_TEXT(n00bb_print_type(types, fixed))));
    n00bb_report_note(core_translation_unit_diagnostics(unit),
                      N00B_TEXT_A_DECLARED_TYPE_PARAMETER_MUST, N00BB_NO_ARGS);
    n00bb_report_help(core_translation_unit_diagnostics(unit),
                      N00B_TEXT_USE_INSTEAD_OF_THE_TYPE,
                      N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, fixed))));

    return true;
}

static bool report_dispatch(th_ctx *types, const n00bb_dispatch_index_t *index,
                            core_translation_unit_t *unit,
                            const ty_failure_t      *failure);
static bool report_literal_argument(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *const *units,
                                    size_t count, core_translation_unit_t *unit,
                                    const ty_failure_t *failure);
static bool report_nil(th_ctx *types, core_translation_unit_t *unit,
                       const ty_failure_t *failure);
static bool report_literal_store(th_ctx *types, core_translation_unit_t *unit,
                                 const ty_failure_t *failure);
static bool report_pattern(th_ctx *types, core_translation_unit_t *unit,
                           const ty_failure_t *failure);
static bool report_literal_operator(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *unit,
                                    const ty_failure_t *failure);
static bool report_body_failure(th_ctx *types, core_translation_unit_t *unit,
                                const ty_failure_t *failure);
static bool report_container_item(core_translation_unit_t *unit,
                                  const ty_failure_t      *failure);
static bool report_tuple_store(th_ctx *types, core_translation_unit_t *unit,
                               const ty_failure_t *failure);

static size_t declared_at(parse_scope_t *scope, core_string_t name)
{
    parse_sym_entry_t  *field = parse_symtab_lookup_in(scope, name);
    parse_token_info_t *first = field && field->decl_node
                                    ? parse_tree_find_first_terminal(
                                          field->decl_node)
                                    : nullptr;

    return first ? parse_token_start_byte(first) : SIZE_MAX;
}

static bool report_field(th_ctx *types, core_translation_unit_t *unit,
                         const ty_failure_t *failure)
{
    n00bb_link_analysis_t *analysis = tu_state(unit)->link_analysis;
    const ir_instr_t *at      = failure->why.at;
    th_type          *subject = failure->kind == TY_FAIL_PREDICATE
                                    && failure->pred.kind == TY_PRED_FIELD
                                    && failure->pred.subject
                                  ? n00bb_shape_object(types,
                                                       failure->pred.subject)
                                  : nullptr;

    if (!at || !subject || th_kind_of(subject) == TH_VAR) {
        return false;
    }

    core_diagnostics_t      *diagnostics = core_translation_unit_diagnostics(
        unit);
    core_string_t            type        = n00bb_print_type(types, subject);
    core_translation_unit_t *owner       = nullptr;
    parse_scope_t           *scope       = analysis
        ? record_scope_for_type(analysis, subject, &owner)
        : nullptr;
    parse_sym_entry_t       *record      = scope && scope->node
                                             ? parse_tree_sym(scope->node)
                                             : nullptr;
    core_list_t(core_string_t) fields    = core_list_new(core_string_t);

    for (parse_sym_entry_t *field = scope ? scope->first_in_scope : nullptr;
         field; field = field->next_in_scope) {
        if (field->is_field && !record) {
            record = field->owner_type;
        }
        if (field->is_field && !field->is_synthetic) {
            core_list_push(fields, field->name);
            for (size_t at = fields.len - 1;
                 at > 0
                 && declared_at(scope, fields.data[at - 1])
                        > declared_at(scope, fields.data[at]);
                 at--) {
                core_string_t swap   = fields.data[at];
                fields.data[at]      = fields.data[at - 1];
                fields.data[at - 1]  = swap;
            }
        }
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_FIELD_MISSING,
                 n00bb_name_location(unit, at->span, failure->pred.name),
                 N00B_TEXT_HAS_NO_FIELD,
                 N00BB_ARGS(ARG_TEXT(type), ARG_TEXT(failure->pred.name)));
    if (fields.len) {
        n00bb_report_note(diagnostics, N00B_TEXT_TYPE_HAS_FIELDS,
                          N00BB_ARGS(ARG_NAMES(fields.data, fields.len),
                                     ARG_TEXT(type)));
    }
    if (record) {
        n00bb_diag_declared_here(diagnostics, record,
                                 N00B_TEXT_TYPE_IS_DECLARED_HERE);
    }
    core_list_free(fields);

    return true;
}

static bool report_unchecked_call(th_ctx *types,
                                  core_translation_unit_t *const *units,
                                  size_t count, core_translation_unit_t *unit,
                                  const ty_failure_t *failure)
{
    const ir_instr_t *at = failure->why.at;
    for (uint32_t i = 0; at && i < at->n_operands; i++) {
        const ir_value_t *value = ir_operand_value(&at->operands[i]);
        if (!value || !value->def || value->def->op != IR_OP_CALL) continue;
        if (report_unpropagated(types, units, count, unit, value, value->type)
            || (failure->actual
                && report_unpropagated(types, units, count, unit, value,
                                       failure->actual)))
            return true;
    }
    return false;
}

static void report_failure(th_ctx *types, const n00bb_dispatch_index_t *index,
                           core_translation_unit_t *const *units,
                           size_t count, core_translation_unit_t *unit,
                           const ty_failure_t *failure)
{
    core_diagnostics_t  *diagnostics = core_translation_unit_diagnostics(unit);
    core_diag_location_t at          = failure->why.at
                                         ? n00bb_span_location(
                                               failure->why.at->span)
                                         : (core_diag_location_t){0};

    const ir_instr_t *test = case_test_of(failure->why.at);

    if (test) {
        report_case_mismatch(types, unit, test);
        return;
    }
    if (report_unchecked_call(types, units, count, unit, failure)
        || report_fixed_parameter(types, unit, failure)
        || report_condition(types, unit, failure)
        || report_nil(types, unit, failure)
        || report_bound_method(types, units, count, unit, failure)
        || report_required_trait(types, units, count, unit, failure)
        || report_argument(types, units, count, unit, failure)
        || report_varargs_array(types, units, count, unit, failure)
        || report_body_failure(types, unit, failure)
        || report_literal(types, unit, failure)
        || report_tuple_store(types, unit, failure)
        || report_store(types, units, count, unit, failure)
        || report_dispatch(types, index, unit, failure)
        || report_literal_operator(types, index, unit, failure)
        || report_container_item(unit, failure)
        || report_literal_argument(types, index, units, count, unit,
                                   failure)
        || report_literal_store(types, unit, failure)
        || report_pattern(types, unit, failure)
        || report_field(types, unit, failure)) {
        return;
    }
    if (failure->why.at && failure->why.at->op == IR_OP_BRANCH) {
        const ir_value_t *condition = failure->why.at->n_operands
            ? ir_operand_value(&failure->why.at->operands[0]) : nullptr;
        th_type *found = failure->actual ? th_resolve(failure->actual)
                                         : nullptr;

        if (!failure->expected || !found) {
            return;
        }
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_MISMATCH,
                     condition && condition->def
                         ? n00bb_span_location(condition->def->span)
                         : at,
                     N00B_TEXT_THIS_CONDITION_IS_NOT_BOOL,
                     N00BB_ARGS(ARG_TEXT(shown(types, found))));
        if (th_is_nil(found) || th_option_item(found)) {
            n00bb_report_help(diagnostics, N00B_TEXT_TEST_AN_OPTION_WITH_NIL,
                              N00BB_NO_ARGS);
        }
        return;
    }
    if (failure->expected && failure->actual && failure->why.at
        && ir_annot_get((ir_annots_t *)&failure->why.at->annots,
                        CORE_STRING_STATIC(N00BB_TRY_ERROR_ANNOTATION))) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TRY_FAILURE_TYPES_DIFFER, at,
                     N00B_TEXT_THIS_CAN_FAIL_WITH_BUT,
                     N00BB_ARGS(ARG_TEXT(shown(types, failure->actual)),
                                ARG_TEXT(shown(types, failure->expected))));
        n00bb_report_help(diagnostics, N00B_TEXT_CONVERT_THE_FAILURES_TO_ONE,
                          N00BB_NO_ARGS);
    }
    else if (failure->expected && failure->actual) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_MISMATCH, at,
                     N00B_TEXT_EXPECTED_FOUND,
                     N00BB_ARGS(ARG_TEXT(shown(types, failure->expected)),
                                ARG_TEXT(shown(types, failure->actual))));
    }
    else if (failure->actual && failure->kind == TY_FAIL_PREDICATE
             && failure->pred.kind == TY_PRED_ONE_OF && failure->pred.other) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_REQUIREMENT, at,
                     N00B_TEXT_NOT_ONE_OF_ALLOWED,
                     N00BB_ARGS(ARG_TEXT(shown(types, failure->actual)),
                                ARG_TEXT(shown(types, failure->pred.other))));
        n00bb_report_help(diagnostics, N00B_TEXT_NOT_ONE_OF_ALLOWED_HELP,
                          N00BB_ARGS(ARG_TEXT(
                              shown(types, failure->pred.other))));
    }
    else if (failure->actual) {
        th_type *underlying = n00bb_distinct_underlying(types,
                                                        failure->actual);

        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_REQUIREMENT, at,
                     N00B_TEXT_CANNOT_BE_USED_HERE,
                     N00BB_ARGS(ARG_TEXT(shown(types, failure->actual))));
        // A distinct type keeps only equality from the type it was made
        // from; say so, rather than leave the error opaque.
        if (underlying) {
            n00bb_report_note(diagnostics,
                N00B_TEXT_DISTINCT_TYPE_HAS_ONLY_EQUALITY,
                N00BB_ARGS(ARG_TEXT(shown(types, failure->actual)),
                           ARG_TEXT(shown(types, underlying))));
            n00bb_report_help(diagnostics,
                N00B_TEXT_CAST_TO_USE_ITS_OPERATORS,
                N00BB_ARGS(ARG_TEXT(shown(types, underlying))));
        }
    }
    else {
        n00bb_report(diagnostics, unit, N00B_DIAG_TYPE_MISMATCH, at,
                     N00B_TEXT_THE_TYPES_IN_THIS_EXPRESSION, N00BB_NO_ARGS);
    }
}

static th_type *contract(th_ctx *types, th_type *result, th_type *self,
                         th_type *other)
{
    th_param params[] = {
        {.mode = TH_IN, .type = self},
        {.mode = TH_IN, .type = other},
    };

    return th_func(types, result, params, other ? 2 : 1, nullptr, nullptr, 0);
}

static void declare_number_contracts(ty_solver_t *solver, th_ctx *types,
                                     core_string_t trait, bool integer)
{
    th_type *self = th_var(types);

    for (size_t i = 0; i < n00bb_binary_operator_count; i++) {
        const n00bb_binary_operator_t *op    = &n00bb_binary_operators[i];
        bool                           shift = op->op == IR_OP_SHL
                                            || op->op == IR_OP_SHR;
        bool bitwise = shift || op->op == IR_OP_AND || op->op == IR_OP_OR
                    || op->op == IR_OP_XOR;

        if (bitwise && !integer) {
            continue;
        }
        ty_declare_trait_method(solver, trait, op->method,
                                contract(types, self, self,
                                         shift ? th_name(types,
                                                         CORE_STRING_STATIC(
                                                             "uint"))
                                               : self));
    }
    ty_declare_trait_method(solver, trait, CORE_STRING_STATIC("__equal"),
                            contract(types,
                                     th_name(types, CORE_STRING_STATIC("bool")),
                                     self, self));
    ty_declare_trait_method(solver, trait, CORE_STRING_STATIC("__cmp"),
                            contract(types,
                                     th_name(types,
                                             CORE_STRING_STATIC("Comparison")),
                                     self, self));
    ty_declare_trait_method(solver, trait, CORE_STRING_STATIC("__negate"),
                            contract(types, self, self, nullptr));
    ty_declare_trait_method(solver, trait, N00BB_REF_METHOD,
                            contract(types, th_ind(types, 0, self), self,
                                     nullptr));
}

static parse_sym_entry_t *nominal_symbol(n00bb_link_analysis_t *analysis,
                                         th_type *receiver);

static bool declared_enum(n00bb_link_analysis_t *analysis, th_ctx *types,
                          th_type *receiver)
{
    core_string_t            name = {0};
    th_env_type_shape_kind_t kind = {0};

    if (th_kind_of(receiver) != TH_NAMED
        || !th_named_info(receiver, &name, nullptr, nullptr)) {
        return false;
    }

    parse_sym_entry_t *symbol = nominal_symbol(analysis, receiver);

    if (symbol) {
        return symbol->member_scope
            && core_string_eq(symbol->member_scope->scope_tag,
                              CORE_STRING_STATIC("enum"));
    }

    return th_env_type_shape(types, name, &kind)
        && kind == TH_ENV_TYPE_SHAPE_ENUM;
}

typedef core_list_t(parse_sym_entry_ptr_t) function_list_t;
typedef function_list_t *function_list_ptr_t;

core_list_decl(function_list_ptr_t);

struct n00bb_dispatch_index {
    n00bb_link_analysis_t *analysis;
    core_dict_t            functions;
    core_list_t(function_list_ptr_t) lists;
    core_list_t(th_trait_requirement) bounds;
    // While typing, before literals are sized: an operator meeting an
    // unbound operand waits for sizing (integer_promotion).
    bool                   sizing_pending;
};

// The trait whose bound a method call at `at` goes through, when its
// receiver's type is a bound type variable.
static core_string_t bound_trait_of_call(const n00bb_dispatch_index_t *index,
                                         const ir_instr_t *at,
                                         th_type **receiver)
{
    const ir_value_t *self = at && at->op == IR_OP_DISPATCH
            && at->n_operands >= 2
        ? ir_operand_value(&at->operands[1]) : nullptr;
    th_type *type = self ? th_resolve(self->type) : nullptr;

    for (size_t i = 0; type && th_kind_of(type) == TH_VAR
         && i < index->bounds.len; i++) {
        if (index->bounds.data[i].kind == TH_CON_IMPLEMENTS
            && th_resolve(index->bounds.data[i].subject) == type) {
            *receiver = type;
            return index->bounds.data[i].iface;
        }
    }

    return (core_string_t){0};
}

// A call through a bound that gives a trait parameter another type than an
// earlier call through it did.
static bool report_trait_parameter(th_ctx *types,
                                   const n00bb_dispatch_index_t *index,
                                   core_translation_unit_t *unit,
                                   const ty_failure_t *failure)
{
    th_type      *receiver = nullptr;
    core_string_t trait    = bound_trait_of_call(index, failure->why.at,
                                                 &receiver);

    if (!trait.data || !failure->expected || !failure->actual
        || failure->why.at->operands[0].kind != IR_OPD_NAME) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    // The receiver as written: the solver's copy of its type variable has
    // no name to show.
    core_string_t       variable    = n00bb_value_text(unit,
        ir_operand_value(&failure->why.at->operands[1]),
        failure->why.at->span);

    if (!variable.u8_bytes) {
        variable = shown(types, receiver);
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_TRAIT_PARAMETER_FIXED,
                 n00bb_span_location(failure->why.at->span),
                 N00B_TEXT_TRAIT_PARAMETER_FIXED,
                 N00BB_ARGS(ARG_TEXT(failure->why.at->operands[0].name),
                            ARG_TEXT(variable),
                            ARG_TEXT(shown(types, failure->actual)),
                            ARG_TEXT(shown(types, failure->expected))));
    n00bb_report_help(diagnostics, N00B_TEXT_TRAIT_PARAMETERS_ARE_PER_BOUND,
                      N00BB_ARGS(ARG_TEXT(n00bb_name_leaf(trait)),
                                 ARG_TEXT(variable)));

    return true;
}

static parse_sym_entry_t *nominal_symbol(n00bb_link_analysis_t *analysis,
                                         th_type *receiver)
{
    return th_kind_of(receiver) == TH_NAMED
             ? n00bb_nominal_symbol(analysis, receiver, nullptr)
             : nullptr;
}

parse_sym_entry_t *n00bb_flagset_flag(const n00bb_dispatch_index_t *index,
                                      th_type *receiver, core_string_t name)
{
    parse_sym_entry_t *symbol = nominal_symbol(index->analysis,
                                               th_resolve(receiver));
    parse_sym_entry_t *member = symbol && symbol->member_scope
                                  ? parse_symtab_lookup_in(symbol->member_scope,
                                                           name)
                                  : nullptr;

    return member && n00bb_flagset_member(member) ? member : nullptr;
}

static parse_sym_entry_t *n00bb_case_owner(const n00bb_dispatch_index_t *index,
                                    th_type *receiver, bool *variant)
{
    receiver = receiver ? th_resolve(receiver) : nullptr;

    parse_sym_entry_t *symbol = receiver ? nominal_symbol(index->analysis,
                                                          receiver)
                                         : nullptr;
    core_string_t      tag    = symbol && symbol->member_scope
                                  ? symbol->member_scope->scope_tag
                                  : (core_string_t){0};
    bool               arms   = core_string_eq(tag,
                                               CORE_STRING_STATIC("variant"));

    if (!arms && !core_string_eq(tag, CORE_STRING_STATIC("enum"))) {
        return nullptr;
    }
    if (variant) {
        *variant = arms;
    }

    return symbol;
}

static bool case_member_of(parse_sym_entry_t *member, bool variant)
{
    return variant ? n00bb_variant_arm_symbol(member)
                   : member->kind == PARSE_SYM_ENUM_CONST
                         && !n00bb_flagset_member(member);
}

parse_sym_entry_t *n00bb_case_member(const n00bb_dispatch_index_t *index,
                                     th_type *receiver, core_string_t name,
                                     bool *variant)
{
    bool               arms   = false;
    parse_sym_entry_t *owner  = n00bb_case_owner(index, receiver, &arms);
    parse_sym_entry_t *member = owner ? parse_symtab_lookup_in(
                                            owner->member_scope, name)
                                      : nullptr;

    if (variant) {
        *variant = arms;
    }

    return member && case_member_of(member, arms) ? member
                                                                   : nullptr;
}

bool n00bb_declared_flagset(n00bb_link_analysis_t *analysis,
                            th_type *receiver)
{
    parse_sym_entry_t *symbol = nominal_symbol(analysis, receiver);

    for (parse_sym_entry_t *member = symbol && symbol->member_scope
                                       ? symbol->member_scope->first_in_scope
                                       : nullptr;
         member; member = member->next_in_scope) {
        if (n00bb_flagset_member(member)) {
            return true;
        }
    }

    return false;
}

static th_type *field_signature(void *context, th_type *receiver,
                                core_string_t name)
{
    n00bb_dispatch_index_t *index = context;
    th_ctx                 *types = ir_ctx_types(index->analysis->ir_context);
    bool                    variant = false;
    parse_sym_entry_t      *arm     = n00bb_case_member(index, receiver, name,
                                                        &variant);

    if (arm && variant) {
        return n00bb_variant_arm_payload_type(types, receiver, arm);
    }

    return n00bb_flagset_flag(index, receiver, name) ? receiver : nullptr;
}

static th_type *flagset_operator(n00bb_link_analysis_t *analysis,
                                 th_ctx *types, th_type *receiver,
                                 core_string_t method)
{
    static const core_string_t combining[] = {
        CORE_STRING_STATIC("__add"),     CORE_STRING_STATIC("__subtract"),
        CORE_STRING_STATIC("__bit_or"),  CORE_STRING_STATIC("__bit_and"),
        CORE_STRING_STATIC("__bit_xor"),
    };

    if (!n00bb_declared_flagset(analysis, receiver)) {
        return nullptr;
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__equal"))) {
        return contract(types, th_name(types, CORE_STRING_STATIC("bool")),
                        receiver, receiver);
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__truth"))
        || core_string_eq(method, CORE_STRING_STATIC("__not"))) {
        return contract(types, th_name(types, CORE_STRING_STATIC("bool")),
                        receiver, nullptr);
    }
    for (size_t i = 0; i < sizeof(combining) / sizeof(*combining); i++) {
        if (core_string_eq(method, combining[i])) {
            return contract(types, receiver, receiver, receiver);
        }
    }

    return nullptr;
}

static th_type *bitint_operator(th_ctx *types, th_type *receiver,
                                core_string_t method)
{
    const n00bb_binary_operator_t *binary = n00bb_binary_operator_method(
        method);
    bool shift = binary && (binary->op == IR_OP_SHL || binary->op == IR_OP_SHR);

    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;

    if (!th_bitint_info(receiver, nullptr, &flavor)) {
        return nullptr;
    }
    if (binary) {
        return contract(types, receiver, receiver,
                        shift ? th_name(types, CORE_STRING_STATIC("uint"))
                              : receiver);
    }
    // `i:N` and `u:N` are integers (the `Integer` trait); `bits:N` is not.
    if (core_string_eq(method, CORE_STRING_STATIC("from_bits"))) {
        return flavor == TH_BITINT_BITS
                 ? nullptr
                 : contract(types, receiver, receiver,
                            th_name(types, CORE_STRING_STATIC("u128")));
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__equal"))) {
        return contract(types, th_name(types, CORE_STRING_STATIC("bool")),
                        receiver, receiver);
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__cmp"))) {
        return contract(types,
                        th_name(types, CORE_STRING_STATIC("Comparison")),
                        receiver, receiver);
    }

    return core_string_eq(method, CORE_STRING_STATIC("__negate"))
             ? contract(types, receiver, receiver, nullptr)
             : nullptr;
}

static th_type *enum_operator(n00bb_link_analysis_t *analysis, th_ctx *types,
                              th_type *receiver, core_string_t method)
{
    if (!declared_enum(analysis, types, receiver)
        && !n00bb_scalar_role_is(receiver, IR_ROLE_CHAR)
        && th_kind_of(receiver) != TH_BITINT
        && th_hash(types, receiver)
               != th_hash(types, th_name(types, CORE_STRING_STATIC("bool")))) {
        return nullptr;
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__equal"))) {
        return contract(types, th_name(types, CORE_STRING_STATIC("bool")),
                        receiver, receiver);
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__cmp"))) {
        return contract(types, th_name(types, CORE_STRING_STATIC("Comparison")),
                        receiver, receiver);
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__not"))
        && th_hash(types, receiver)
               == th_hash(types, th_name(types, CORE_STRING_STATIC("bool")))) {
        return contract(types, receiver, receiver, nullptr);
    }

    return nullptr;
}

th_type *n00bb_distinct_underlying(th_ctx *types, th_type *type)
{
    core_string_t            name = {0};
    th_env_type_shape_kind_t kind = {0};

    type = th_resolve(type);
    if (th_kind_of(type) != TH_NAMED
        || !th_named_info(type, &name, nullptr, nullptr)) {
        return nullptr;
    }

    th_type *shape = th_env_type_shape(types, name, &kind);

    return shape && kind == TH_ENV_TYPE_SHAPE_NOMINAL ? th_resolve(shape)
                                                      : nullptr;
}

// A distinct type gets the default equality of the type it was made from,
// when that type has one, but none of its other operators.
static th_type *distinct_equality(const n00bb_dispatch_index_t *index,
                                  th_ctx *types, th_type *receiver,
                                  core_string_t method)
{
    if (!core_string_eq(method, CORE_STRING_STATIC("__equal"))) {
        return nullptr;
    }

    th_type                *shape = n00bb_distinct_underlying(types,
                                                              receiver);
    n00bb_dispatch_target_t inner = {0};

    if (!shape || !n00bb_dispatch_target_for(index, shape, method, 2, &inner)
        || !inner.signature) {
        return nullptr;
    }

    return contract(types, th_name(types, CORE_STRING_STATIC("bool")),
                    receiver, receiver);
}

static th_type *trait_requirement(const n00bb_dispatch_index_t *index,
                                  th_ctx *types, th_type *receiver,
                                  core_string_t method, core_string_t *trait)
{
    n00bb_link_analysis_t *analysis = index->analysis;

    for (size_t i = 0; th_kind_of(receiver) == TH_VAR
         && i < index->bounds.len; i++) {
        th_trait_requirement *bound    = &index->bounds.data[i];
        th_env_method_t       selected = {0};

        if (bound->subject != receiver || bound->kind != TH_CON_IMPLEMENTS) {
            continue;
        }
        *trait = bound->iface;
        if (th_env_method_resolve(types, receiver, bound->iface, method,
                                  &selected)
            == TH_ENV_METHOD_FOUND) {
            return selected.signature;
        }

        parse_sym_entry_t *requirement = analysis->units.len
            ? trait_method_requirement_symbol(analysis->grammar, analysis,
                                              analysis->units.data[0],
                                              bound->iface, method)
            : nullptr;

        if (requirement) {
            return n00bb_symbol_signature(requirement);
        }

        th_env_method_requirement_t found[8] = {0};
        size_t                      count    = th_env_method_requirements(
            types, method, found, sizeof(found) / sizeof(*found));

        for (size_t r = 0; r < count && r < sizeof(found) / sizeof(*found);
             r++) {
            if (core_string_eq(found[r].trait_name, bound->iface)) {
                return found[r].contract ? found[r].contract
                                         : found[r].signature;
            }
        }
    }
    *trait = (core_string_t){0};

    th_env_method_requirement_t found[2] = {0};

    if (th_kind_of(receiver) != TH_VAR
        || th_env_method_requirements(types, method, found, 2) != 1) {
        return nullptr;
    }
    *trait = found[0].trait_name;

    parse_sym_entry_t *requirement = analysis->units.len
        ? trait_method_requirement_symbol(analysis->grammar, analysis,
                                          analysis->units.data[0], *trait,
                                          method)
        : nullptr;

    return requirement ? n00bb_symbol_signature(requirement)
         : found[0].contract ? found[0].contract
                             : found[0].signature;
}

static n00bb_dispatch_index_t *dispatch_index_new(
    n00bb_link_analysis_t *analysis)
{
    n00bb_dispatch_index_t *index = core_alloc(n00bb_dispatch_index_t);

    index->analysis = analysis;
    core_dict_init_string(&index->functions);
    index->lists  = core_list_new(function_list_ptr_t);
    index->bounds = core_list_new(th_trait_requirement);
    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);

        if (!state || !state->annotations) {
            continue;
        }

        core_list_t(parse_scope_ptr_t) scopes = module_scopes(state);

        for (size_t s = 0; s < scopes.len; s++) {
            for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
                 symbol; symbol = symbol->next_in_scope) {
                if (symbol->kind != PARSE_SYM_FUNCTION || !symbol->type) {
                    continue;
                }

                bool             found = false;
                function_list_t *list  = core_dict_get(&index->functions,
                                                       &symbol->name, &found);

                if (!found) {
                    list  = core_alloc(function_list_t);
                    *list = core_list_new(parse_sym_entry_ptr_t);
                    core_dict_put(&index->functions, &symbol->name, list);
                    core_list_push(index->lists, list);
                }
                core_list_push(*list, symbol);
            }
        }
        core_list_free(scopes);
    }

    return index;
}

static th_type *var_named_in(th_ctx *types, th_type *type, core_string_t name)
{
    th_type **vars  = nullptr;
    size_t    count = type ? th_free_vars(types, type, &vars) : 0;
    th_type  *found = nullptr;

    for (size_t v = 0; !found && v < count; v++) {
        found = core_string_eq(th_var_name(vars[v]), name) ? vars[v] : nullptr;
    }

    return found;
}

static th_type *function_var(th_ctx *types, ir_func_t *function,
                             core_string_t name)
{
    th_type *found = var_named_in(types, function->type, name);

    for (uint32_t l = 0; !found && l < function->n_locals; l++) {
        found = var_named_in(types, function->locals[l]->type, name);
    }
    for (uint32_t b = 0; !found && b < function->n_blocks; b++) {
        for (uint32_t k = 0; !found && k < function->blocks[b]->n_instrs;
             k++) {
            ir_instr_t *in = function->blocks[b]->instrs[k];

            found = in->dest ? var_named_in(types, in->dest->type, name)
                             : nullptr;
        }
    }

    return found;
}

static th_type *in_function(th_ctx *types, ir_func_t *function, th_type *type)
{
    th_type **vars  = nullptr;
    size_t    count = th_free_vars(types, type, &vars);
    th_type **to    = core_alloc_array(th_type *, count ? count : 1);

    for (size_t v = 0; v < count; v++) {
        core_string_t name = th_var_name(vars[v]);
        th_type      *mine = name.data ? function_var(types, function, name)
                                       : nullptr;

        to[v] = mine ? mine : vars[v];
    }

    th_type *mapped = count ? th_type_clone_replacing(
                                  types, type, (const th_type *const *)vars,
                                  to, count)
                            : type;

    core_free(to);

    return mapped;
}

// A bound method's call shape with its receiver put back, passed in `mode`.
static th_type *method_shape(th_ctx *types, th_type *receiver, th_mode mode,
                             th_type *call)
{
    call             = th_resolve(call);
    size_t      n    = th_func_nparams(call);
    size_t      nkw  = th_func_nkw(call);
    th_param   *ps   = core_alloc_array(th_param, n + 1);
    th_kwparam *kws  = core_alloc_array(th_kwparam, nkw + 1);

    ps[0] = (th_param){.mode = mode, .type = receiver};
    for (size_t i = 0; i < n; i++) {
        ps[i + 1] = *th_func_param(call, i);
    }
    for (size_t k = 0; k < nkw; k++) {
        kws[k] = *th_func_kw(call, k);
    }

    th_type *shape = th_func_with_error(types, th_func_ret(call),
                                        th_func_error(call), ps, n + 1,
                                        th_func_rest(call), kws, nkw);

    core_free(ps);
    core_free(kws);

    return shape;
}

// The mode the receiver of trait `trait`'s method `method` is passed in.
static th_mode bound_receiver_mode(n00bb_link_analysis_t *analysis,
                                   core_string_t trait, core_string_t method)
{
    parse_sym_entry_t *requirement = trait.data && analysis->units.len
        ? trait_method_requirement_symbol(analysis->grammar, analysis,
                                          analysis->units.data[0], trait,
                                          method)
        : nullptr;
    th_type *signature = requirement
        ? th_resolve(n00bb_symbol_signature(requirement)) : nullptr;

    while (signature && th_kind_of(signature) == TH_BINDER) {
        signature = th_resolve(th_binder_body(signature));
    }

    return signature && th_kind_of(signature) == TH_FUNCTION
            && th_func_nparams(signature)
        ? th_func_param(signature, 0)->mode
        : TH_IN;
}

static void declare_bounds(ty_solver_t *solver, n00bb_dispatch_index_t *index)
{
    n00bb_link_analysis_t *analysis = index->analysis;
    th_ctx                *types    = ir_ctx_types(analysis->ir_context);

    for (size_t l = 0; l < index->lists.len; l++) {
        function_list_t *list = index->lists.data[l];

        for (size_t i = 0; i < list->len; i++) {
            parse_sym_entry_t *symbol = list->data[i];
            core_list_t(th_trait_requirement) declared =
                n00bb_declared_bounds(symbol);
            ir_func_t *function = declared.len
                ? link_ir_function_for_symbol(analysis, symbol)
                : nullptr;

            // A trait's method constraints follow its `implements` one.
            core_string_t trait = {0};

            for (size_t r = 0; function && r < declared.len; r++) {
                th_trait_requirement bound = declared.data[r];
                th_type *var = function_var(types, function,
                                            th_var_name(bound.subject));

                if (bound.kind == TH_CON_IMPLEMENTS) {
                    trait = bound.iface;
                }
                if (var && bound.kind == TH_CON_METHOD) {
                    ty_declare_bound(solver, var,
                                     (ty_pred_t){.kind  = TY_PRED_DISPATCH,
                                                 .name  = bound.iface,
                                                 .other = method_shape(
                                                     types, var,
                                                     bound_receiver_mode(
                                                         analysis, trait,
                                                         bound.iface),
                                                     in_function(
                                                         types, function,
                                                         bound.target))},
                                     (ty_why_t){0});
                }
                if (var && bound.kind == TH_CON_ONE_OF) {
                    ty_declare_bound(solver, var,
                                     (ty_pred_t){.kind  = TY_PRED_ONE_OF,
                                                 .other = bound.target},
                                     (ty_why_t){0});
                }
                if (!var || bound.kind != TH_CON_IMPLEMENTS) {
                    continue;
                }
                bound.subject = var;
                core_list_push(index->bounds, bound);
                ty_declare_bound(solver, var,
                                 (ty_pred_t){.kind = TY_PRED_IMPLEMENTS,
                                             .name = bound.iface},
                                 (ty_why_t){0});
                // A bound that names no trait arguments still fixes the
                // trait's parameters for every call through it.
                bool given = r + 1 < declared.len
                    && declared.data[r + 1].kind == TH_CON_METHOD;
                parse_sym_entry_t *trait_symbol = given ? nullptr
                    : n00bb_link_trait_symbol(analysis->grammar, analysis,
                                              symbol->owner_unit,
                                              bound.iface);
                core_list_t(th_trait_requirement) methods =
                    core_list_new(th_trait_requirement);
                size_t parameters = 0;
                // A trait with no parameters has nothing for a bound to fix.
                if (trait_symbol
                    && n00bb_trait_method_instances(types, trait_symbol, var,
                                                    nullptr, &methods,
                                                    &parameters)
                    && parameters) {
                    for (size_t m = 0; m < methods.len; m++) {
                        ty_declare_bound(solver, var,
                            (ty_pred_t){.kind  = TY_PRED_DISPATCH,
                                        .name  = methods.data[m].iface,
                                        .other = methods.data[m].target},
                            (ty_why_t){0});
                    }
                }
                core_list_free(methods);
            }
            core_list_free(declared);
        }
    }
}

static void dispatch_index_free(n00bb_dispatch_index_t *index)
{
    if (index) {
        core_list_free(index->bounds);
    }
    for (size_t i = 0; index && i < index->lists.len; i++) {
        function_list_t *list = index->lists.data[i];

        core_list_free(*list);
        core_free(list);
    }
    if (index) {
        core_list_free(index->lists);
        core_dict_free(&index->functions);
        core_free(index);
    }
}

static bool same_constructor(th_type *a, th_type *b)
{
    core_string_t left  = {0};
    core_string_t right = {0};

    if (th_kind_of(a) != th_kind_of(b) || th_kind_of(a) == TH_VAR) {
        return false;
    }
    if (th_kind_of(a) != TH_NAMED) {
        return true;
    }
    th_named_info(a, &left, nullptr, nullptr);
    th_named_info(b, &right, nullptr, nullptr);

    return core_string_eq(left, right);
}

static th_type *function_signature(const n00bb_dispatch_index_t *index,
                                   parse_sym_entry_t *symbol)
{
    th_ctx    *types    = ir_ctx_types(index->analysis->ir_context);
    ir_func_t *function = link_ir_function_for_symbol(index->analysis,
                                                      symbol);
    th_type   *declared = th_resolve(n00bb_symbol_signature(symbol));

    if (!function || !th_is_closed(types, function->type)) {
        return declared;
    }

    th_type        *found = th_resolve(function->type);
    const th_param *rest  = th_kind_of(declared) == TH_FUNCTION
                              ? th_func_rest(declared)
                              : nullptr;
    size_t          fixed = rest ? th_func_nparams(declared) : 0;

    if (!rest || th_kind_of(found) != TH_FUNCTION || th_func_rest(found)
        || th_func_nparams(found) != fixed + 1
        || th_kind_of(th_resolve(th_func_param(found, fixed)->type))
               != TH_SEQUENCE) {
        return found;
    }

    th_param   *params   = core_alloc_array(th_param, fixed + 1);
    th_kwparam *keywords = core_alloc_array(th_kwparam,
                                            th_func_nkw(found) + 1);

    for (size_t i = 0; i < fixed; i++) {
        params[i] = *th_func_param(found, i);
    }
    for (size_t i = 0; i < th_func_nkw(found); i++) {
        keywords[i] = *th_func_kw(found, i);
    }

    th_param element  = {
        .mode = rest->mode,
        .type = th_seq_elem(th_resolve(th_func_param(found, fixed)->type)),
    };
    th_type *expanded = th_func_with_error(types, th_func_ret(found),
                                           th_func_error(found), params, fixed,
                                           &element, keywords,
                                           th_func_nkw(found));

    core_free(params);
    core_free(keywords);

    return expanded;
}

static int receiver_bounds(th_ctx *types, parse_sym_entry_t *symbol,
                           th_type *self, th_type *receiver)
{
    core_list_t(th_trait_requirement) bounds = n00bb_declared_bounds(symbol);
    th_type *subject = th_resolve(self);
    int      found   = 0;

    for (size_t i = 0; found >= 0 && i < bounds.len; i++) {
        th_trait_requirement *bound = &bounds.data[i];

        if (th_resolve(bound->subject) != subject) {
            continue;
        }
        if (bound->kind == TH_CON_EXCLUDES) {
            found = bound->target
                     && th_hash(types, th_resolve(bound->target))
                            == th_hash(types, receiver)
                  ? -1 : 1;
        }
        else if (bound->kind == TH_CON_IMPLEMENTS) {
            found = th_env_implements(types, receiver, bound->iface) ? 1 : -1;
        }
    }
    core_list_free(bounds);

    return found;
}

static bool takes_arity(const n00bb_dispatch_index_t *index,
                        parse_sym_entry_t *symbol, th_type *signature,
                        size_t arity)
{
    ir_func_t *function = arity == SIZE_MAX
        ? nullptr : link_ir_function_for_symbol(index->analysis, symbol);

    if (!function) {
        return true;
    }

    size_t params   = th_func_nparams(signature);
    size_t required = 0;

    for (size_t i = 0; i < params; i++) {
        required += !ir_func_parameter_is_optional(function, i);
    }

    return arity >= required
        && (th_func_rest(signature)
            || arity <= params + th_func_nkw(signature));
}

static parse_sym_entry_t *module_function(const n00bb_dispatch_index_t *index,
                                          th_type *receiver,
                                          core_string_t method, bool exact,
                                          size_t arity,
                                          parse_sym_entry_t **rival)
{
    bool               found    = false;
    function_list_t   *list     = core_dict_get(
        (core_dict_t *)&index->functions, &method, &found);
    parse_sym_entry_t *fallback = nullptr;
    int                fallback_rank = 0;
    parse_sym_entry_t *matched  = nullptr;
    th_type           *matched_self = nullptr;

    receiver = th_resolve(receiver);
    for (size_t i = 0; found && i < list->len; i++) {
        parse_sym_entry_t *symbol    = list->data[i];
        th_type           *signature = th_resolve(
            n00bb_symbol_signature(symbol));
        const th_param    *self      = th_kind_of(signature) == TH_FUNCTION
                                          && th_func_nparams(signature)
                                      ? th_func_param(signature, 0)
                                      : nullptr;

        if (!self || !takes_arity(index, symbol, signature, arity)) {
            continue;
        }

        th_ctx       *types  = ir_ctx_types(index->analysis->ir_context);
        core_string_t holder = {0};
        bool          buffer = th_named_info(receiver, &holder, nullptr,
                                             nullptr)
            && core_string_eq(holder,
                   core_string_borrow(n00bb_buffer_type_name,
                                      sizeof(n00bb_buffer_type_name) - 1));
        bool coerced = buffer && self && th_mode_is_input_only(self->mode)
            && th_hash(types, th_resolve(self->type))
                   == th_hash(types,
                              th_name(types, CORE_STRING_STATIC("string")));

        bool accepts = self
            && th_env_type_pattern_subsumes(self->type, receiver);
        int  bounds  = accepts ? receiver_bounds(types, symbol, self->type,
                                                 receiver)
                               : 0;
        // An optional parameter takes its item type, as a call with one
        // candidate does. Such a candidate ranks below any that take the
        // receiver as it is.
        th_type *item    = accepts || coerced
            ? nullptr : th_option_item(th_resolve(self->type));
        bool     wrapped = item && th_env_type_pattern_subsumes(item, receiver)
            && receiver_bounds(types, symbol, item, receiver) >= 0;

        if (wrapped) {
            if (!fallback) {
                fallback      = symbol;
                fallback_rank = 0;
            }
            continue;
        }
        if ((!accepts && !coerced) || bounds < 0) {
            continue;
        }
        if (same_constructor(th_resolve(self->type), receiver)) {
            if (!matched) {
                matched      = symbol;
                matched_self = self->type;
            }
            else if (rival && !*rival
                     && canonical_symbol(symbol)->owner_unit
                            != canonical_symbol(matched)->owner_unit
                     && th_hash(types, th_resolve(self->type))
                            == th_hash(types, th_resolve(matched_self))) {
                *rival = symbol;
            }
            continue;
        }

        int rank = accepts ? 1 + bounds : 0;

        if (!fallback || rank > fallback_rank) {
            fallback      = symbol;
            fallback_rank = rank;
        }
    }

    return matched ? matched : exact ? nullptr : fallback;
}

bool n00bb_reference_dispatch(core_string_t method, bool *writable)
{
    bool mutable = core_string_eq(method,
                                  CORE_STRING_STATIC("__ref_address_mut"));

    if (writable) {
        *writable = mutable;
    }

    return mutable
        || core_string_eq(method, CORE_STRING_STATIC("__ref_address"));
}

static size_t visible_initializers(const n00bb_dispatch_index_t *index,
                                   th_type                      *receiver);

bool n00bb_dispatch_target(const n00bb_dispatch_index_t *index,
                           th_type *receiver, core_string_t method,
                           n00bb_dispatch_target_t *target)
{
    return n00bb_dispatch_target_for(index, receiver, method, SIZE_MAX,
                                     target);
}

bool n00bb_dispatch_target_for(const n00bb_dispatch_index_t *index,
                               th_type *receiver, core_string_t method,
                               size_t arity, n00bb_dispatch_target_t *target)
{
    n00bb_link_analysis_t *analysis = index->analysis;
    th_ctx                *types    = ir_ctx_types(analysis->ir_context);

    *target  = (n00bb_dispatch_target_t){0};
    receiver = th_resolve(receiver);
    if (th_kind_of(receiver) == TH_INDIRECTION
        && n00bb_reference_dispatch(method, nullptr)) {
        target->kind      = N00BB_TARGET_REFERENCE;
        target->signature = contract(types, receiver, receiver, nullptr);
        return true;
    }
    if (th_kind_of(receiver) == TH_INDIRECTION
        && core_string_eq(method, CORE_STRING_STATIC("__equal"))) {
        target->kind      = N00BB_TARGET_ENUM;
        target->signature = contract(
            types, th_name(types, CORE_STRING_STATIC("bool")), receiver,
            receiver);
        return true;
    }
    if (core_string_eq(method, N00BB_REF_METHOD)) {
        if (th_env_method_resolve_by_name(types, receiver,
                                          CORE_STRING_STATIC("__ref_create"),
                                          &target->method, nullptr)
            == TH_ENV_METHOD_FOUND) {
            target->kind      = N00BB_TARGET_REF_CREATE;
            target->signature = target->method.signature;
            return true;
        }
        target->kind      = N00BB_TARGET_REF_PLAIN;
        target->signature = contract(types, th_ind(types, 0, receiver),
                                     receiver, nullptr);
        return true;
    }
    if (core_string_eq(method, N00BB_CASE_METHOD)) {
        if (!n00bb_case_owner(index, receiver, nullptr)) {
            return false;
        }
        target->kind      = N00BB_TARGET_CASE;
        target->signature = contract(
            types, th_name(types, CORE_STRING_STATIC("bool")), receiver,
            nullptr);
        return true;
    }
    target->signature = flagset_operator(analysis, types, receiver, method);
    if (target->signature) {
        target->kind = N00BB_TARGET_FLAGSET;
        return true;
    }
    // A bit integer's `from_bits` is registered so it implements `Integer`,
    // but has no body: it is a wrapping cast, chosen before that function.
    if (th_kind_of(receiver) == TH_BITINT
        && core_string_eq(method, CORE_STRING_STATIC("from_bits"))) {
        target->signature = bitint_operator(types, receiver, method);
        if (target->signature) {
            target->kind = N00BB_TARGET_PRIMITIVE;
            return true;
        }
    }
    target->function = module_function(index, receiver, method, true, arity,
                                       &target->rival);
    if (target->function) {
        target->kind      = N00BB_TARGET_FUNCTION;
        target->signature = function_signature(index, target->function);
        return true;
    }
    if (th_env_method_resolve_by_name(types, receiver, method,
                                      &target->method, nullptr)
        == TH_ENV_METHOD_FOUND) {
        target->kind      = N00BB_TARGET_METHOD;
        target->signature = target->method.signature;
        return true;
    }
    target->signature = enum_operator(analysis, types, receiver, method);
    if (target->signature) {
        target->kind = N00BB_TARGET_ENUM;
        return true;
    }
    target->signature = bitint_operator(types, receiver, method);
    if (target->signature) {
        target->kind = N00BB_TARGET_PRIMITIVE;
        return true;
    }
    target->signature = distinct_equality(index, types, receiver, method);
    if (target->signature) {
        target->kind = N00BB_TARGET_DISTINCT_EQUAL;
        return true;
    }
    target->signature = trait_requirement(index, types, receiver, method,
                                          &target->trait);
    if (target->signature) {
        target->kind = N00BB_TARGET_TRAIT;
        return true;
    }
    for (size_t u = 0; !target->signature && u < analysis->units.len; u++) {
        target->signature = n00bb_callback_method_signature(
            types, receiver, method, tu_state(analysis->units.data[u]));
    }
    if (target->signature) {
        target->kind = N00BB_TARGET_CALLBACK;
        return true;
    }
    target->function = module_function(index, receiver, method, false, arity,
                                       nullptr);
    // A type with no `__init` of any arity is built from zeroed storage, so
    // `new` on it calls nothing, and takes no arguments.
    if (!target->function && th_kind_of(receiver) == TH_NAMED
        && core_string_eq(method, CORE_STRING_STATIC("__init"))
        && !visible_initializers(index, receiver)) {
        th_param self = {.mode = TH_INOUT, .type = receiver};

        target->kind      = N00BB_TARGET_ZEROED;
        target->signature = th_func(types,
                                    th_name(types, CORE_STRING_STATIC("nil")),
                                    &self, 1, nullptr, nullptr, 0);
        return true;
    }
    if (!target->function) {
        n00bb_dispatch_target_t compare = {0};

        if (n00bb_assign_target(index, receiver, method, target)) {
            return true;
        }
        if (!core_string_eq(method, CORE_STRING_STATIC("__equal"))
            || !n00bb_dispatch_target(index, receiver,
                                      CORE_STRING_STATIC("__cmp"), &compare)) {
            return false;
        }
        target->kind      = N00BB_TARGET_EQUAL_BY_CMP;
        target->signature = contract(
            types, th_name(types, CORE_STRING_STATIC("bool")), receiver,
            receiver);
        return true;
    }
    target->kind      = N00BB_TARGET_FUNCTION;
    target->signature = function_signature(index, target->function);
    return true;
}

// Overloaded calls. A call to an overloaded function goes to the candidate
// whose parameters fit its arguments most closely. Each argument fits its
// parameter exactly, by a promotion, by a generic parameter (one with a
// trait or `where` bound before one without), or by a conversion. One
// candidate is chosen when it fits each argument at least as well as each
// other candidate, and one argument better (or needs no defaults where the
// other does).

static bool coerces(void *context, th_type *to, th_type *from);
static core_string_t function_parameter_name(
    const n00bb_dispatch_index_t *dispatch, parse_sym_entry_t *function,
    size_t index);

typedef enum : uint8_t {
    FIT_EXACT,
    FIT_PROMOTED,
    FIT_BOUNDED,
    FIT_GENERIC,
    FIT_CONVERTED,
    FIT_CHECKED,
    FIT_NONE,
} overload_fit_t;

typedef struct {
    parse_sym_entry_t *symbol;
    th_type           *signature;
    overload_fit_t    *fits;
    th_type          **params;
    bool               defaulted;
} overload_option_t;
core_list_decl(overload_option_t);

typedef enum : uint8_t {
    OVERLOAD_CHOSEN,
    OVERLOAD_WAIT,
    OVERLOAD_NO_MATCH,
    OVERLOAD_AMBIGUOUS,
} overload_outcome_t;

static n00bb_overload_site_t *overload_site(
    const n00bb_dispatch_index_t *index, const ir_instr_t *at)
{
    n00bb_link_analysis_t *analysis = index ? index->analysis : nullptr;

    for (size_t u = 0; at && analysis && u < analysis->units.len; u++) {
        n00bb_tu_state_t *state = tu_state(analysis->units.data[u]);
        bool              found = false;
        void             *site  = state
            ? core_dict_get(&state->overload_sites, (void *)at, &found)
            : nullptr;

        if (found) {
            return site;
        }
    }

    return nullptr;
}

bool n00bb_overload_target(const n00bb_dispatch_index_t *index,
                           const ir_instr_t *at,
                           n00bb_dispatch_target_t *target)
{
    n00bb_overload_site_t *site = overload_site(index, at);

    if (!site || !site->chosen) {
        return false;
    }
    *target           = (n00bb_dispatch_target_t){0};
    target->kind      = N00BB_TARGET_FUNCTION;
    target->function  = site->chosen;
    target->signature = function_signature(index, site->chosen);

    return true;
}

static bool named_in(th_type *type, const core_string_t *names, size_t count)
{
    core_string_t name = {0};

    if (th_kind_of(type) != TH_NAMED
        || !th_named_info(type, &name, nullptr, nullptr)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (core_string_eq(name, names[i])) {
            return true;
        }
    }

    return false;
}

static bool integer_type(th_type *type)
{
    return th_kind_of(type) == TH_BITINT
        || named_in(type, integer_names,
                    sizeof(integer_names) / sizeof(*integer_names));
}

static bool float_type(th_type *type)
{
    return named_in(type, float_names,
                    sizeof(float_names) / sizeof(*float_names));
}

// Whether a `from` value widens to `to` without loss: an integer to a wider
// one of its signedness, or an unsigned one to a strictly wider signed one;
// a float to a wider float. Integers never become floats.
static bool numeric_promotes(ir_module_t *module, th_type *from, th_type *to)
{
    from = th_resolve(from);
    to   = th_resolve(to);
    if (!module || !from || !to) {
        return false;
    }

    ir_machine_t source      = ir_machine_of(module, from);
    ir_machine_t destination = ir_machine_of(module, to);

    if (integer_type(from) && integer_type(to)) {
        return n00bb_machine_promotes(source, destination);
    }

    return float_type(from) && float_type(to)
        && destination.width >= source.width;
}

static overload_fit_t overload_fit(const n00bb_dispatch_index_t *index,
                                   ir_module_t *module,
                                   parse_sym_entry_t *symbol, th_type *param,
                                   th_type *arg)
{
    th_ctx *types = ir_ctx_types(index->analysis->ir_context);

    param = th_resolve(param);
    arg   = th_resolve(arg);
    if (!param || !arg) {
        return FIT_NONE;
    }
    if (!th_is_closed(types, param)) {
        if (!th_env_type_pattern_subsumes(param, arg)) {
            th_type *item = th_option_item(param);

            return item && overload_fit(index, module, symbol, item, arg)
                        != FIT_NONE
                     ? FIT_CONVERTED
                     : FIT_NONE;
        }

        int bounds = receiver_bounds(types, symbol, param, arg);

        return bounds < 0                                     ? FIT_NONE
             : th_kind_of(param) == TH_VAR && bounds == 0     ? FIT_GENERIC
                                                              : FIT_BOUNDED;
    }
    if (th_same_type(param, arg) || th_hash(types, param) == th_hash(types, arg)
        || (!th_is_closed(types, arg)
            && th_env_type_pattern_subsumes(arg, param))) {
        return FIT_EXACT;
    }
    if (numeric_promotes(module, arg, param)) {
        return FIT_PROMOTED;
    }

    th_type *item = th_option_item(param);

    if ((item && (th_is_nil(arg)
                  || overload_fit(index, module, symbol, item, arg)
                         != FIT_NONE))
        || coerces((void *)index, param, arg)) {
        return FIT_CONVERTED;
    }

    // Any integer passes to an integer parameter through a checked
    // conversion; that fits worst of all.
    return integer_type(arg) && integer_type(param) ? FIT_CHECKED
                                                    : FIT_NONE;
}

// How two candidates' parameters compare for one argument: -1 when the
// first fits better, 1 when the second does, 0 when they fit alike, and 2
// when neither is more specific than the other.
static int compare_fit(ir_module_t *module, overload_fit_t first,
                       th_type *first_param, overload_fit_t second,
                       th_type *second_param)
{
    if (first != second) {
        return first < second ? -1 : 1;
    }
    first_param  = th_resolve(first_param);
    second_param = th_resolve(second_param);
    if (th_same_type(first_param, second_param)) {
        return 0;
    }
    if (first == FIT_PROMOTED || first == FIT_CHECKED) {
        bool forward  = numeric_promotes(module, first_param, second_param);
        bool backward = numeric_promotes(module, second_param, first_param);

        return forward && backward ? 0
             : forward             ? -1
             : backward            ? 1
                                   : 2;
    }
    if (first == FIT_BOUNDED || first == FIT_GENERIC) {
        bool forward  = th_env_type_pattern_subsumes(second_param,
                                                     first_param);
        bool backward = th_env_type_pattern_subsumes(first_param,
                                                     second_param);

        return forward == backward ? 0 : forward ? -1 : 1;
    }

    return 0;
}

// Whether `first` is at least as specific as `second` for every argument
// and more specific for one.
static bool overload_beats(ir_module_t *module, const overload_option_t *first,
                           const overload_option_t *second, size_t count)
{
    bool better = false;

    for (size_t i = 0; i < count; i++) {
        int order = compare_fit(module, first->fits[i], first->params[i],
                                second->fits[i], second->params[i]);

        if (order > 0) {
            return false;
        }
        better = better || order < 0;
    }

    return better || (!first->defaulted && second->defaulted);
}

// Whether a candidate takes `count` positional arguments and the call's
// keywords, and whether it needs a default for a parameter the call leaves
// out.
static bool overload_takes(const n00bb_dispatch_index_t *index,
                           parse_sym_entry_t *symbol, th_type *signature,
                           th_type *call, size_t count, bool *defaulted)
{
    ir_func_t *function = link_ir_function_for_symbol(index->analysis,
                                                      symbol);
    size_t     params   = th_func_nparams(signature);
    bool       rest     = th_func_rest(signature) != nullptr;

    *defaulted = rest && count <= params;
    if (count > params && !rest) {
        return false;
    }
    // A parameter with a default is a keyword parameter: no positional
    // argument fills it.
    for (size_t p = 0; function && p < count && p < params; p++) {
        if (ir_func_parameter_is_optional(function, p)) {
            return false;
        }
    }

    size_t keywords = th_func_nkw(call);
    bool  *named    = core_alloc_array(bool, params + 1);
    bool   ok       = true;

    for (size_t k = 0; ok && k < keywords; k++) {
        core_string_t name = th_func_kw(call, k)->name;
        bool          found = th_func_kw_lookup(signature, name) != nullptr;

        for (size_t p = count; !found && p < params; p++) {
            found    = core_string_eq(function_parameter_name(index, symbol,
                                                              p),
                                      name);
            named[p] = named[p] || found;
        }
        ok = found;
    }
    for (size_t p = count; ok && p < params; p++) {
        bool optional = !function || ir_func_parameter_is_optional(function, p);

        ok         = named[p] || optional;
        *defaulted = *defaulted || !named[p];
    }
    core_free(named);

    return ok;
}

static th_type *overload_param(th_type *signature, size_t i)
{
    const th_param *rest = th_func_rest(signature);

    return i < th_func_nparams(signature) ? th_func_param(signature, i)->type
         : rest                           ? rest->type
                                          : nullptr;
}

static void overload_options_free(core_list_t(overload_option_t) *options)
{
    for (size_t i = 0; i < options->len; i++) {
        core_free(options->data[i].fits);
        core_free(options->data[i].params);
    }
    core_list_free(*options);
}

// Chooses among an overloaded call's candidates. `call` is the shape of its
// arguments. On an ambiguity, `tied` gets the candidates none beats.
static overload_outcome_t choose_overload(
    const n00bb_dispatch_index_t *index, const n00bb_overload_site_t *site,
    const ir_instr_t *at, th_type *call, parse_sym_entry_t **chosen,
    core_list_t(parse_sym_entry_ptr_t) *tied)
{
    ir_module_t *module = at && at->block && at->block->owner
        ? at->block->owner->owner : nullptr;
    size_t       count  = th_func_nparams(call);
    core_list_t(overload_option_t) options = core_list_new(overload_option_t);

    *chosen = nullptr;
    for (size_t c = 0; c < site->candidates.len; c++) {
        parse_sym_entry_t *symbol    = canonical_symbol(
            site->candidates.data[c]);
        th_type           *signature = th_resolve(
            function_signature(index, symbol));
        overload_option_t  option    = {.symbol = symbol,
                                        .signature = signature};

        if (th_kind_of(signature) != TH_FUNCTION
            || !overload_takes(index, symbol, signature, call, count,
                               &option.defaulted)) {
            continue;
        }
        option.fits   = core_alloc_array(overload_fit_t, count + 1);
        option.params = core_alloc_array(th_type *, count + 1);
        for (size_t i = 0; i < count; i++) {
            option.params[i] = overload_param(signature, i);
        }
        core_list_push(options, option);
    }

    // An argument whose type is not yet known can only be passed over when
    // each candidate takes the same type for it.
    for (size_t i = 0; i < count; i++) {
        th_type *arg = th_resolve(th_func_param(call, i)->type);

        if (th_kind_of(arg) != TH_VAR) {
            continue;
        }
        for (size_t o = 1; o < options.len; o++) {
            if (!th_same_type(th_resolve(options.data[o].params[i]),
                              th_resolve(options.data[0].params[i]))) {
                overload_options_free(&options);
                return OVERLOAD_WAIT;
            }
        }
    }

    size_t kept = 0;

    for (size_t o = 0; o < options.len; o++) {
        overload_option_t *option = &options.data[o];
        bool               fits   = true;

        for (size_t i = 0; fits && i < count; i++) {
            th_type *arg = th_resolve(th_func_param(call, i)->type);

            option->fits[i] = th_kind_of(arg) == TH_VAR
                ? FIT_EXACT
                : overload_fit(index, module, option->symbol,
                               option->params[i], arg);
            fits = option->fits[i] != FIT_NONE;
        }
        for (size_t k = 0; fits && k < th_func_nkw(call); k++) {
            const th_kwparam *given = th_func_kw(call, k);
            const th_kwparam *taken = th_func_kw_lookup(option->signature,
                                                        given->name);
            th_type          *want  = taken ? taken->type : nullptr;

            for (size_t p = count;
                 !want && p < th_func_nparams(option->signature); p++) {
                if (core_string_eq(function_parameter_name(index,
                                                           option->symbol, p),
                                   given->name)) {
                    want = th_func_param(option->signature, p)->type;
                }
            }
            fits = !want || th_kind_of(th_resolve(given->type)) == TH_VAR
                || overload_fit(index, module, option->symbol, want,
                                given->type) != FIT_NONE;
        }
        if (fits) {
            options.data[kept++] = *option;
        }
        else {
            core_free(option->fits);
            core_free(option->params);
        }
    }
    options.len = kept;

    overload_outcome_t outcome = options.len ? OVERLOAD_AMBIGUOUS
                                             : OVERLOAD_NO_MATCH;

    for (size_t o = 0; o < options.len && !*chosen; o++) {
        bool best = true;

        for (size_t other = 0; best && other < options.len; other++) {
            best = other == o
                || overload_beats(module, &options.data[o],
                                  &options.data[other], count);
        }
        if (best) {
            *chosen = options.data[o].symbol;
            outcome = OVERLOAD_CHOSEN;
        }
    }
    for (size_t o = 0; tied && outcome == OVERLOAD_AMBIGUOUS
         && o < options.len; o++) {
        bool beaten = false;

        for (size_t other = 0; !beaten && other < options.len; other++) {
            beaten = other != o
                && overload_beats(module, &options.data[other],
                                  &options.data[o], count);
        }
        if (!beaten) {
            core_list_push(*tied, options.data[o].symbol);
        }
    }
    overload_options_free(&options);

    return outcome;
}

// A method call, an operator or a trait method that goes to a module
// function chooses among the functions of its name that the calling module
// can see, anywhere in the program, as a call by name does. Those found
// first (a type's own methods, an enum's or flagset's operators) keep
// their place.
static n00bb_overload_site_t *method_site(const n00bb_dispatch_index_t *index,
                                          const ir_instr_t *at,
                                          th_type *shape)
{
    n00bb_link_analysis_t *analysis = index->analysis;
    th_type *receiver = shape && th_kind_of(shape) == TH_FUNCTION
            && th_func_nparams(shape)
        ? th_resolve(th_func_param(shape, 0)->type)
        : nullptr;

    th_ctx    *types    = ir_ctx_types(analysis->ir_context);
    ir_func_t *function = at && at->block ? at->block->owner : nullptr;

    // A generic function's body leaves its calls to trait requirements; a
    // compound assignment goes to its type's assigning operator.
    if (!at || at->op != IR_OP_DISPATCH || !at->n_operands
        || ir_annot_get((ir_annots_t *)&at->annots, N00BB_COMPOUND_KEY)
        || at->operands[0].kind != IR_OPD_NAME || !receiver
        || !th_is_closed(types, receiver) || !function
        || !th_is_closed(types, function->type)) {
        return nullptr;
    }

    core_string_t           method = at->operands[0].name;
    n00bb_dispatch_target_t target = {0};

    if (!n00bb_dispatch_target_for(index, receiver, method,
                                   th_func_nparams(shape)
                                       + th_func_nkw(shape),
                                   &target)
        || target.kind != N00BB_TARGET_FUNCTION) {
        return nullptr;
    }

    core_translation_unit_t *consumer = unit_of(analysis->units.data,
                                                analysis->units.len, at);
    bool                     found    = false;
    function_list_t         *list     = core_dict_get(
        (core_dict_t *)&index->functions, &method, &found);

    if (!consumer || !found) {
        return nullptr;
    }

    // The candidates are the functions whose first parameter takes the
    // receiver. With one at most, and none hidden, the call needs no choice.
    ir_module_t    *module  = function->owner;
    function_list_t fitting = core_list_new(parse_sym_entry_ptr_t);
    function_list_t visible = core_list_new(parse_sym_entry_ptr_t);

    for (size_t i = 0; i < list->len; i++) {
        parse_sym_entry_t *origin    = canonical_symbol(list->data[i]);
        th_type           *signature = th_resolve(
            function_signature(index, origin));
        bool               listed    = false;

        for (size_t j = 0; !listed && j < fitting.len; j++) {
            listed = fitting.data[j] == origin;
        }
        if (listed || th_kind_of(signature) != TH_FUNCTION
            || !th_func_nparams(signature)
            || overload_fit(index, module, origin,
                            th_func_param(signature, 0)->type, receiver)
                   == FIT_NONE) {
            continue;
        }
        core_list_push(fitting, origin);
        if (n00bb_member_visible_to(consumer, origin->owner_unit, origin)) {
            core_list_push(visible, origin);
        }
    }

    size_t all = fitting.len;

    core_list_free(fitting);
    if (visible.len < 2 && visible.len == all) {
        core_list_free(visible);
        return nullptr;
    }

    n00bb_overload_site_t *site = core_alloc(n00bb_overload_site_t);

    site->candidates = visible;
    core_dict_put(&tu_state(consumer)->overload_sites, (void *)at, site);

    return site;
}

static th_type *best_integer_fit(th_ctx *types, ir_abcd_range_t found);

// A built-in integer type an operation can promote: a named integer type, or
// an `i:N` / `u:N` (not `bits:N`, and not a distinct type or enum).
static bool promotable_integer(th_ctx *types, th_type *type,
                               ir_interval_t *range)
{
    uint64_t         width  = 0;
    th_bitint_flavor flavor = TH_BITINT_BITS;

    type = type ? th_resolve(type) : nullptr;
    if (!type) {
        return false;
    }
    if (th_bitint_info(type, &width, &flavor)) {
        if (flavor == TH_BITINT_BITS) {
            return false;
        }
        *range = ir_interval_of_width(width, flavor == TH_BITINT_SIGNED);
        return true;
    }
    // Compare names, not hashes: this runs each time the solver judges an
    // operator, and building and hashing the twelve names there is costly.
    core_string_t name      = {0};
    size_t        arguments = 0;

    (void)types;
    if (th_kind_of(type) != TH_NAMED
        || !th_named_info(type, &name, nullptr, &arguments) || arguments) {
        return false;
    }
    for (size_t i = 0; i < sizeof(integer_names) / sizeof(*integer_names);
         i++) {
        if (core_string_eq(name, integer_names[i])) {
            return n00bb_type_interval(type, range);
        }
    }
    return false;
}

// The smallest integer type holding every value of both operand types. It
// stops at 64 bits unless an operand is 128-bit: `uint` with `int` is
// `int`, and converting the `uint` operand is checked.
static th_type *promoted_integer(th_ctx *types, ir_interval_t left,
                                 ir_interval_t right)
{
    // An operand is 128-bit: that is the explicit evidence to go past 64.
    bool wide = left.lo < (ir_int_t)INT64_MIN
             || left.hi > (ir_int_t)UINT64_MAX
             || right.lo < (ir_int_t)INT64_MIN
             || right.hi > (ir_int_t)UINT64_MAX;
    ir_interval_t both = ir_interval_join(left, right);

    if (!wide && (both.lo < (ir_int_t)INT64_MIN
                  || both.hi > (ir_int_t)INT64_MAX)
        && both.lo < 0) {
        return th_name(types, CORE_STRING_STATIC("int"));
    }
    return best_integer_fit(types, (ir_abcd_range_t){.seen = both});
}

th_type *n00bb_integer_promotion(th_ctx *types, th_type *left,
                                 th_type *right)
{
    ir_interval_t lr = {0}, rr = {0};

    if (!left || !right || !promotable_integer(types, left, &lr)
        || !promotable_integer(types, right, &rr)
        || th_hash(types, th_resolve(left)) == th_hash(types,
                                                       th_resolve(right))) {
        return nullptr;
    }
    return promoted_integer(types, lr, rr);
}

typedef enum {
    PROMOTION_NONE,
    PROMOTION_WAIT,
    PROMOTION_CHOSEN,
    PROMOTION_FAILS,
} promotion_t;

// A built-in integer operator whose operands have different types runs at
// a type holding both, and never narrows an operand: `u8 + u16` is a `u16`
// addition and `u8 + i8` an `i16` one. An operand that is still an open
// literal is sized from its own values first (`a + 1000` with `a: u8`
// sizes `1000` as `u16`), so the call waits until it is.
// A float literal, whose type is still open while typing starts.
static bool float_constant(const ir_value_t *value)
{
    const ir_instr_t *def = value ? value->def : nullptr;

    return def && def->op == IR_OP_CONST && def->n_operands == 1
        && def->operands[0].kind == IR_OPD_CONST
        && def->operands[0].lit.is_float;
}

// An integer literal (not a string or float one).
static bool integer_constant(const ir_value_t *value)
{
    const ir_instr_t *def = value ? value->def : nullptr;

    return def && def->op == IR_OP_CONST && def->n_operands == 1
        && def->operands[0].kind == IR_OPD_CONST
        && !def->operands[0].lit.is_float
        && !def->operands[0].lit.text.data;
}

// The smallest integer type holding an integer literal's value, and that
// value as a range; null past 128 bits.
static th_type *literal_integer_type(th_ctx *types, const ir_value_t *value,
                                     ir_interval_t *range)
{
    ir_lit_t literal = value->def->operands[0].lit.i;
    ir_int_t point   = 0;

    if (!literal.neg && literal.mag > (ir_uint_t)ir_int_max) {
        point = ir_int_max;
    }
    else if (literal.neg && literal.mag > (ir_uint_t)ir_int_max + 1) {
        return nullptr;
    }
    else {
        point = literal.neg
            ? (literal.mag == (ir_uint_t)ir_int_max + 1
                   ? ir_int_min : -(ir_int_t)literal.mag)
            : (ir_int_t)literal.mag;
    }
    *range = ir_interval_point(point);
    return best_integer_fit(types, (ir_abcd_range_t){.seen = *range});
}

// Whether an open value comes from a float literal (or, with `!real`, an
// integer one): directly, or through copies, a local's slot or a module
// variable's stores (`var x = 0.5` then `x + 1`; `var n = 2` then
// `f + n`). Only asked where an open value meets a literal or a float, so
// the few steps of search stay off typing's hot path.
static bool literal_origin(const ir_func_t *function, const ir_value_t *value,
                           bool real, int depth)
{
    if (!value || depth > 4) {
        return false;
    }
    if (real ? float_constant(value) : integer_constant(value)) {
        return true;
    }

    const ir_instr_t *def = value->def;
    const ir_value_t *slot = def && def->op == IR_OP_LOAD
                               ? ir_value_operand(def, 0)
                               : nullptr;
    const ir_global_t *global = value->kind == IR_VAL_GLOBAL
                                  ? value->global
                                  : nullptr;
    const ir_local_t  *local  = value->kind == IR_VAL_LOCAL ? value->local
                                                            : nullptr;

    if (!slot && !global && !local) {
        return def && def->op == IR_OP_COPY
            && literal_origin(function, ir_value_operand(def, 0), real,
                              depth + 1);
    }
    // The values written to the local, slot or module variable.
    for (uint32_t b = 0; function && b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in      = function->blocks[b]->instrs[i];
            const ir_value_t *written = nullptr;

            if (local && in->op == IR_OP_COPY && in->dest_local == local) {
                written = ir_value_operand(in, 0);
            }
            else if (slot && in->op == IR_OP_STORE
                     && ir_value_operand(in, 0) == slot) {
                written = ir_value_operand(in, 1);
            }
            else if (global && in->op == IR_OP_GLOBAL_STORE) {
                const ir_value_t *target = ir_value_operand(in, 0);

                if (target && target->kind == IR_VAL_GLOBAL
                    && target->global == global) {
                    written = ir_value_operand(in, 1);
                }
            }
            if (written && literal_origin(function, written, real,
                                          depth + 1)) {
                return true;
            }
        }
    }
    // A module variable is set in its module's initializer, which is
    // usually not the function reading it.
    ir_module_t *owner = global ? global->owner : nullptr;

    for (size_t f = 0; owner && f < ir_func_count(owner); f++) {
        const ir_func_t *other = ir_func_at(owner, f);

        for (uint32_t b = 0; other != function && b < other->n_blocks; b++) {
            for (uint32_t i = 0; i < other->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in     = other->blocks[b]->instrs[i];
                const ir_value_t *target = in->op == IR_OP_GLOBAL_STORE
                                             ? ir_value_operand(in, 0)
                                             : nullptr;

                if (target && target->kind == IR_VAL_GLOBAL
                    && target->global == global
                    && literal_origin(other, ir_value_operand(in, 1), real,
                                      depth + 1)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool float_origin(const ir_func_t *function, const ir_value_t *value,
                         int depth)
{
    return literal_origin(function, value, true, depth);
}

static promotion_t integer_promotion(const n00bb_dispatch_index_t *index,
                                     const ir_instr_t *at, th_type **out)
{
    th_ctx *types = ir_ctx_types(index->analysis->ir_context);

    // A `switch` case test is not a mixed operation: a case must have the
    // switch value's type (`case 1.5` on a `uint` is an error).
    if (!at || at->op != IR_OP_DISPATCH || at->n_operands != 3
        || at->operands[0].kind != IR_OPD_NAME
        || ir_annot_get((ir_annots_t *)&at->annots, N00BB_SWITCH_KEY)) {
        return PROMOTION_NONE;
    }

    core_string_t                  method = at->operands[0].name;
    const n00bb_binary_operator_t *binary = n00bb_binary_operator_method(
        method);
    bool equal   = core_string_eq(method, CORE_STRING_STATIC("__equal"));
    bool compare = core_string_eq(method, CORE_STRING_STATIC("__cmp"));

    if ((!binary && !equal && !compare)
        || (binary && (binary->op == IR_OP_SHL || binary->op == IR_OP_SHR))) {
        return PROMOTION_NONE;
    }

    const ir_value_t *left  = ir_value_operand(at, 1);
    const ir_value_t *right = ir_value_operand(at, 2);
    th_type          *lt    = left && left->type ? th_resolve(left->type)
                                                 : nullptr;
    th_type          *rt    = right && right->type ? th_resolve(right->type)
                                                   : nullptr;
    ir_interval_t     lr = {0}, rr = {0};
    bool              lp = lt && promotable_integer(types, lt, &lr);
    bool              rp = rt && promotable_integer(types, rt, &rr);
    bool              lf = lt && float_type(lt);
    bool              rf = rt && float_type(rt);
    bool              l_open = lt && th_kind_of(lt) == TH_VAR
                            && !th_var_ref(lt);
    bool              r_open = rt && th_kind_of(rt) == TH_VAR
                            && !th_var_ref(rt);

    // An integer literal beside a declared operand needs no waiting: its
    // value is in the IR. If it fits the declared integer type it simply
    // takes that type (the usual lookup); otherwise it is the smallest type
    // holding its value, and the operation promotes now. Waiting would
    // leave everything downstream unresolved through sizing.
    if ((lp || lf) && r_open && integer_constant(right)) {
        rt = literal_integer_type(types, right, &rr);
        if (!rt || (lp && rr.lo >= lr.lo && rr.hi <= lr.hi)) {
            return PROMOTION_NONE;
        }
        rp     = true;
        r_open = false;
    }
    else if ((rp || rf) && l_open && integer_constant(left)) {
        lt = literal_integer_type(types, left, &lr);
        if (!lt || (rp && lr.lo >= rr.lo && lr.hi <= rr.hi)) {
            return PROMOTION_NONE;
        }
        lp     = true;
        l_open = false;
    }

    // Before literals are sized, an unbound operand may be one: wait. After
    // sizing anything still unbound (a generic's type) resolves as usual.
    // Two unbound operands where one is a float literal wait too, so the
    // float settles before the integer is unified with it (`1.5 + 2`).
    // A float operand only waits for an open integer, a literal or a value
    // assigned one (`var n = 2` then `f + n`); an open float (`0.0` beside
    // an `f32`) unifies with it as usual.
    const ir_func_t *function = at->block ? at->block->owner : nullptr;

    if (index->sizing_pending
        && ((lp && r_open) || (rp && l_open)
            || (lf && r_open && literal_origin(function, right, false, 0))
            || (rf && l_open && literal_origin(function, left, false, 0))
            || (l_open && r_open
                && (float_constant(left) || float_constant(right)))
            || (l_open && r_open && integer_constant(right)
                && float_origin(at->block ? at->block->owner : nullptr,
                                left, 0))
            || (l_open && r_open && integer_constant(left)
                && float_origin(at->block ? at->block->owner : nullptr,
                                right, 0)))) {
        return PROMOTION_WAIT;
    }

    // An integer meeting a float in an arithmetic or comparison operator
    // converts to the float's type: mixing ints and floats gives a float.
    // Only here; an int is not otherwise a float.
    if ((lf && rp) || (rf && lp)) {
        if (binary && (binary->unsigned_only || binary->op == IR_OP_REM)) {
            return PROMOTION_NONE;
        }

        th_type *real = lf ? lt : rt;

        // Each operand keeps its own type here: the solver never turns an
        // int into a float, and the rewrite converts the integer operand.
        *out = contract(types,
                        equal ? th_name(types, CORE_STRING_STATIC("bool"))
                        : compare
                            ? th_name(types, CORE_STRING_STATIC("Comparison"))
                            : real,
                        lt, rt);
        return *out ? PROMOTION_CHOSEN : PROMOTION_NONE;
    }
    if (!lp || !rp || th_hash(types, lt) == th_hash(types, rt)) {
        return PROMOTION_NONE;
    }

    th_type      *promoted = promoted_integer(types, lr, rr);
    ir_interval_t bounds   = {0};

    // Bitwise operators exist only on unsigned integers. With a signed left
    // operand the usual lookup reports that; with only a signed right one
    // it would narrow that operand to the left's type, so fail here.
    if (binary && binary->unsigned_only
        && (!n00bb_type_interval(promoted, &bounds) || bounds.lo < 0)) {
        return lr.lo < 0 ? PROMOTION_NONE : PROMOTION_FAILS;
    }

    th_type *result   = equal ? th_name(types, CORE_STRING_STATIC("bool"))
                      : compare
                          ? th_name(types, CORE_STRING_STATIC("Comparison"))
                          : promoted;

    *out = contract(types, result, promoted, promoted);
    return *out ? PROMOTION_CHOSEN : PROMOTION_NONE;
}

static ty_overload_t overload_choice(void *context, const ir_instr_t *at,
                                     th_type *call, th_type **signature)
{
    n00bb_dispatch_index_t *index = context;
    th_type                *promoted = nullptr;

    switch (integer_promotion(index, at, &promoted)) {
    case PROMOTION_WAIT:
        return TY_OVERLOAD_WAIT;
    case PROMOTION_CHOSEN:
        *signature = promoted;
        return TY_OVERLOAD_CHOSEN;
    case PROMOTION_FAILS:
        return TY_OVERLOAD_FAILS;
    case PROMOTION_NONE:
        break;
    }

    n00bb_overload_site_t  *site  = overload_site(index, at);
    th_type                *shape = call ? th_resolve(call) : nullptr;

    if (!site) {
        site = method_site(index, at, shape);
    }
    if (!site) {
        return TY_OVERLOAD_NONE;
    }
    if (!site->chosen && shape && th_kind_of(shape) == TH_FUNCTION) {
        switch (choose_overload(index, site, at, shape, &site->chosen,
                                nullptr)) {
        case OVERLOAD_WAIT:
            return TY_OVERLOAD_WAIT;
        case OVERLOAD_NO_MATCH:
        case OVERLOAD_AMBIGUOUS:
            return TY_OVERLOAD_FAILS;
        case OVERLOAD_CHOSEN:
            break;
        }
    }
    *signature = site->chosen ? function_signature(index, site->chosen)
                              : nullptr;

    return *signature ? TY_OVERLOAD_CHOSEN : TY_OVERLOAD_FAILS;
}

bool n00bb_assign_target(const n00bb_dispatch_index_t *index,
                         th_type *receiver, core_string_t method,
                         n00bb_dispatch_target_t *target)
{
    if (!n00bb_binary_operator_method(method)) {
        return false;
    }

    char              buffer[64];
    core_string_t     assign   = n00bb_format_name(buffer, sizeof(buffer),
                                                   "%.*s_assign",
                                                   (int)method.u8_bytes,
                                                   method.data);
    parse_sym_entry_t *symbol  = module_function(index, receiver, assign,
                                                 false, SIZE_MAX, nullptr);
    th_type           *updated = symbol
                                     ? function_signature(index, symbol)
                                     : nullptr;

    if (!updated || th_kind_of(updated) != TH_FUNCTION
        || !th_func_nparams(updated)) {
        return false;
    }

    th_ctx  *types  = ir_ctx_types(index->analysis->ir_context);
    size_t   count  = th_func_nparams(updated);
    th_param params[count];

    for (size_t i = 0; i < count; i++) {
        params[i] = (th_param){.mode = TH_IN,
                               .type = th_func_param(updated, i)->type};
    }
    *target = (n00bb_dispatch_target_t){
        .kind      = N00BB_TARGET_ASSIGN,
        .function  = symbol,
        .signature = th_func_with_error(types, params[0].type,
                                        th_func_error(updated), params, count,
                                        nullptr, nullptr, 0),
    };
    return true;
}

static const ir_value_t *dispatch_operand(const ir_instr_t *in, uint32_t i)
{
    return i < in->n_operands && in->operands[i].kind != IR_OPD_NAME
                && in->operands[i].kind != IR_OPD_NAMED_ARG
             ? ir_operand_value(&in->operands[i])
             : nullptr;
}

static const void *read_variable(const ir_value_t *value)
{
    const ir_instr_t *read    = value ? value->def : nullptr;
    const ir_value_t *address = read && read->op == IR_OP_LOAD
                                        && read->n_operands
                                  ? ir_operand_value(&read->operands[0])
                                  : nullptr;

    if (read && read->op == IR_OP_COPY && read->n_operands) {
        return ir_operand_local(&read->operands[0]);
    }
    if (value && value->kind == IR_VAL_GLOBAL) {
        return value;
    }

    return address
                && (address->kind == IR_VAL_GLOBAL
                    || (address->def && address->def->op == IR_OP_ALLOC))
             ? address
             : nullptr;
}

static bool assigns_variable(const ir_instr_t *in, const void *variable)
{
    return (in->dest_local && (const void *)in->dest_local == variable)
        || ((in->op == IR_OP_STORE || in->op == IR_OP_GLOBAL_STORE)
            && in->n_operands == 2
            && (const void *)ir_operand_value(&in->operands[0]) == variable);
}

static const ir_instr_t *assignment_after_test(const ir_instr_t *use,
                                               const ir_value_t *operand)
{
    const void      *variable = read_variable(operand);
    const ir_func_t *function = use->block ? use->block->owner : nullptr;
    bool             tested   = false;
    uint32_t         test_at  = 0;

    for (uint32_t b = 0; variable && function && b < function->n_blocks;
         b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            if (in->op == IR_OP_OPTION_IS_NIL && in->n_operands
                && read_variable(ir_operand_value(&in->operands[0]))
                       == variable
                && in->span.start_byte < use->span.start_byte
                && (!tested || in->span.start_byte > test_at)) {
                tested  = true;
                test_at = in->span.start_byte;
            }
        }
    }

    const ir_instr_t *assigned = nullptr;

    for (uint32_t b = 0; tested && b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            if (assigns_variable(in, variable)
                && in->span.start_byte > test_at
                && in->span.start_byte < use->span.start_byte
                && (!assigned
                    || in->span.start_byte > assigned->span.start_byte)) {
                assigned = in;
            }
        }
    }

    return assigned;
}

typedef struct {
    const void   *base;
    core_string_t field;
    bool          global;
} nil_place_t;

static nil_place_t place_of(const ir_value_t *value)
{
    const ir_instr_t *def   = value ? value->def : nullptr;
    nil_place_t       place = {0};

    if (value && value->kind == IR_VAL_GLOBAL) {
        return (nil_place_t){.base = value, .global = true};
    }
    if (value && value->kind == IR_VAL_PARAM) {
        return (nil_place_t){.base = value};
    }
    if (def && (def->op == IR_OP_LOAD || def->op == IR_OP_COPY)
        && def->n_operands) {
        const ir_value_t *source = ir_operand_value(&def->operands[0]);

        if (source && source->kind == IR_VAL_GLOBAL) {
            return (nil_place_t){.base = source, .global = true};
        }
    }
    if (def && def->op == IR_OP_EXTRACT && def->n_operands == 2
        && def->operands[1].kind == IR_OPD_NAME) {
        place       = place_of(ir_operand_value(&def->operands[0]));
        place.field = place.base && !place.field.u8_bytes
                        ? def->operands[1].name
                        : (core_string_t){0};
        place.base  = place.field.u8_bytes ? place.base : nullptr;
        return place;
    }
    place.base = read_variable(value);

    return place;
}

static bool same_place(nil_place_t a, nil_place_t b)
{
    return a.base && a.base == b.base && core_string_eq(a.field, b.field);
}

static const ir_value_t *nil_value(const ty_failure_t *failure)
{
    const ir_instr_t *at = failure->why.at;

    if (!at) {
        return nullptr;
    }
    if ((at->op == IR_OP_STORE || at->op == IR_OP_GLOBAL_STORE)
        && at->n_operands == 2) {
        return ir_operand_value(&at->operands[1]);
    }
    if (at->op == IR_OP_COPY && at->n_operands == 1) {
        return ir_operand_value(&at->operands[0]);
    }
    for (uint32_t o = 1; at->op == IR_OP_CALL && o < at->n_operands; o++) {
        const ir_value_t *value = ir_operand_value(&at->operands[o]);

        if (value && th_option_item(th_resolve(value->type))) {
            return value;
        }
    }

    return nullptr;
}

static bool kills_place(const ir_instr_t *in, nil_place_t place)
{
    if (assigns_variable(in, place.base)) {
        return true;
    }
    if (place.global && in->op == IR_OP_GLOBAL_STORE && in->n_operands
        && (const void *)ir_operand_value(&in->operands[0]) == place.base) {
        return true;
    }
    if (in->op == IR_OP_ADDRESS_OF && in->n_operands
        && (const void *)ir_operand_local(&in->operands[0]) == place.base) {
        return true;
    }

    return false;
}

static const ir_instr_t *nil_test_before(const ir_instr_t *use,
                                         nil_place_t place)
{
    const ir_func_t  *function = use->block ? use->block->owner : nullptr;
    const ir_instr_t *test     = nullptr;

    for (uint32_t b = 0; function && b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            if (in->op == IR_OP_OPTION_IS_NIL && in->n_operands
                && same_place(place_of(ir_operand_value(&in->operands[0])),
                              place)
                && in->span.start_byte < use->span.start_byte
                && (!test || in->span.start_byte > test->span.start_byte)) {
                test = in;
            }
        }
    }

    return test;
}

static const ir_instr_t *kill_after_test(const ir_instr_t *use,
                                         nil_place_t place)
{
    const ir_func_t  *function = use->block ? use->block->owner : nullptr;
    const ir_instr_t *test     = nil_test_before(use, place);
    const ir_instr_t *kill     = nullptr;

    for (uint32_t b = 0; test && b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            if (kills_place(in, place)
                && in->span.start_byte > test->span.start_byte
                && in->span.start_byte < use->span.start_byte
                && (!kill || in->span.start_byte > kill->span.start_byte)) {
                kill = in;
            }
        }
    }

    return kill;
}

static const ir_instr_t *kill_site(const ir_instr_t *kill)
{
    const ir_func_t *function = kill->block ? kill->block->owner : nullptr;

    for (uint32_t b = 0; kill->dest && function && b < function->n_blocks;
         b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            for (uint32_t o = 1; in->op == IR_OP_CALL && o < in->n_operands;
                 o++) {
                if (ir_operand_value(&in->operands[o]) == kill->dest) {
                    return in;
                }
            }
        }
    }

    return kill;
}

static bool report_nil(th_ctx *types, core_translation_unit_t *unit,
                       const ty_failure_t *failure)
{
    const ir_value_t *value = failure->expected && failure->actual
                                ? nil_value(failure)
                                : nullptr;
    th_type          *item  = value ? th_option_item(th_resolve(value->type))
                                    : nullptr;

    if (!item || !th_same_type(th_resolve(item),
                               th_resolve(failure->expected))) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    const ir_instr_t   *at          = failure->why.at;
    source_t            source      = value_source(unit, value, at->span);
    nil_place_t         place       = place_of(value);

    (void)types;
    if (!source.text.u8_bytes || !place.base) {
        n00bb_report(diagnostics, unit, N00B_DIAG_OPTION_NOT_PROVEN,
                     source.at, N00B_TEXT_OPTION_MAY_BE_NIL, N00BB_NO_ARGS);
        n00bb_report_help(diagnostics, N00B_TEXT_IR_STORE_AND_TEST,
                          N00BB_NO_ARGS);
        return true;
    }
    const ir_instr_t *kill = kill_after_test(at, place);

    n00bb_report(diagnostics, unit, N00B_DIAG_OPTION_NOT_PROVEN, source.at,
                 N00B_TEXT_NAMED_MAY_BE_NIL, N00BB_ARGS(ARG_TEXT(source.text)));
    if (kill) {
        n00bb_report_secondary(diagnostics, unit,
                               n00bb_span_location(kill_site(kill)->span),
                               N00B_TEXT_IR_ASSIGNED_AFTER_TEST,
                               N00BB_ARGS(ARG_TEXT(source.text)));
    }
    n00bb_report_help(diagnostics, N00B_TEXT_IR_TEST_BEFORE_USE,
                      N00BB_ARGS(ARG_TEXT(source.text)));

    return true;
}

static core_string_t item_kind(const ir_value_t *value);
static bool report_operator_pairing(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *unit,
                                    const ir_instr_t *at, core_string_t method,
                                    th_type *receiver);

static core_string_t operand_phrase(th_ctx *types,
                                    core_translation_unit_t *unit,
                                    const ir_value_t *value, ir_span_t within,
                                    const ir_instr_t *origin)
{
    source_t      source  = value_source(unit, value, within);
    core_string_t literal = item_kind(origin ? origin->dest : value);
    bool          written = core_string_eq(literal,
                                           CORE_STRING_STATIC("string"));
    bool          decimal = core_string_eq(literal,
                                           CORE_STRING_STATIC("float"));
    bool          integer = core_string_eq(literal,
                                           CORE_STRING_STATIC("integer"))
                        && th_hash(types, th_resolve(value->type))
                               != th_hash(types,
                                          th_name(types,
                                                  CORE_STRING_STATIC("bool")));
    core_buffer_t kind    = {0};

    if (!source.text.u8_bytes) {
        return (core_string_t){0};
    }
    if (integer) {
        core_string_t phrase = n00bb_text(N00B_TEXT_AN_INTEGER, N00BB_NO_ARGS);

        core_buffer_append_string(&kind, phrase);
        core_free(phrase.data);
    }
    else {
        core_buffer_puts(&kind, "`");
        core_buffer_append_string(&kind,
                                  decimal ? CORE_STRING_STATIC("f64")
                                  : written
                                      ? CORE_STRING_STATIC("string")
                                      : n00bb_print_type(types, value->type));
        core_buffer_puts(&kind, "`");
    }

    core_string_t text = n00bb_text(
        N00B_TEXT_OPERAND_IS,
        N00BB_ARGS(ARG_TEXT(source.text), ARG_TEXT(core_buffer_string(&kind))));

    core_buffer_release(&kind);

    return text;
}

static bool report_option_operand(th_ctx *types, core_translation_unit_t *unit,
                                  const ir_instr_t *at, core_string_t spelling,
                                  th_type *receiver)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    const ir_value_t   *left        = dispatch_operand(at, 1);
    source_t            source      = value_source(unit, left, at->span);
    core_string_t       type        = n00bb_print_type(types, receiver);

    if (!spelling.u8_bytes) {
        n00bb_report(diagnostics, unit, N00B_DIAG_OPTION_USED_AS_VALUE,
                     n00bb_span_location(at->span),
                     N00B_TEXT_THIS_VALUE_IS_WHICH_MAY,
                     N00BB_ARGS(ARG_TEXT(type),
                                ARG_TEXT(CORE_STRING_STATIC("Not"))));
        n00bb_report_help(diagnostics, N00B_TEXT_STORE_IT_IN_A_VARIABLE,
                          N00BB_NO_ARGS);
        return true;
    }
    if (!source.text.u8_bytes || !read_variable(left)) {
        n00bb_report(diagnostics, unit, N00B_DIAG_OPTION_USED_AS_VALUE,
                     source.at, N00B_TEXT_THIS_OPERAND_IS_AND_MAY,
                     N00BB_ARGS(ARG_TEXT(type), ARG_TEXT(spelling)));
        n00bb_report_help(diagnostics, N00B_TEXT_STORE_IT_IN_A_VARIABLE,
                          N00BB_NO_ARGS);
        return true;
    }

    const ir_instr_t *assigned = assignment_after_test(at, left);

    n00bb_report(diagnostics, unit, N00B_DIAG_OPTION_USED_AS_VALUE, source.at,
                 N00B_TEXT_IS_AND_MAY_BE_NIL,
                 N00BB_ARGS(ARG_TEXT(source.text), ARG_TEXT(type),
                            ARG_TEXT(spelling)));
    if (assigned) {
        n00bb_report_secondary(diagnostics, unit,
                               n00bb_span_location(assigned->span),
                               N00B_TEXT_IS_ASSIGNED_HERE_AFTER_THE,
                               N00BB_ARGS(ARG_TEXT(source.text)));
        n00bb_report_help(diagnostics, N00B_TEXT_USE_BEFORE_ASSIGNING_IT_OR,
                          N00BB_ARGS(ARG_TEXT(source.text)));
    }
    else {
        n00bb_report_help(
            diagnostics, N00B_TEXT_TEST_IT_AGAINST_NIL_FIRST,
            N00BB_ARGS(ARG_TEXT(source.text), ARG_TEXT(source.text),
                       ARG_TEXT(n00bb_print_type(types,
                                                 th_option_item(receiver)))));
    }

    return true;
}

static bool report_operator(th_ctx *types,
                            const n00bb_dispatch_index_t *index,
                            core_translation_unit_t *unit,
                            const ir_instr_t *at, core_string_t method,
                            th_type *receiver, const ir_instr_t *literal)
{
    const n00bb_binary_operator_t *binary = n00bb_binary_operator_method(
        method);

    if (!binary && !core_string_eq(method, CORE_STRING_STATIC("__not"))) {
        return false;
    }
    if (report_operator_pairing(types, index, unit, at, method, receiver)) {
        return true;
    }
    if (th_option_item(receiver)) {
        return report_option_operand(types, unit, at,
                                     binary ? binary->spelling
                                            : (core_string_t){0},
                                     receiver);
    }

    const ir_value_t *left  = dispatch_operand(at, 1);
    const ir_value_t *right = binary ? dispatch_operand(at, 2) : nullptr;
    ir_interval_t     right_bounds = {0};

    // A bitwise operator with a signed right operand (integer_promotion
    // refuses it): the operand types may differ, but neither may be signed.
    if (binary && binary->unsigned_only && right && right->type
        && n00bb_integer_promotion(types, receiver, right->type)
        && n00bb_type_interval(th_resolve(right->type), &right_bounds)
        && right_bounds.lo < 0) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        source_t            source = value_source(unit, right, at->span);
        core_string_t       signed_name = n00bb_print_type(types,
                                                           right->type);
        core_buffer_t       unsigned_name = {0};
        size_t              skip = signed_name.u8_bytes > 1
            && signed_name.data[0] == 'i' && signed_name.data[1] >= '0'
            && signed_name.data[1] <= '9';

        n00bb_report(diagnostics, unit, N00B_DIAG_BITWISE_NEGATIVE,
                     source.at, N00B_TEXT_BITWISE_SIGNED_OPERAND,
                     N00BB_ARGS(ARG_TEXT(source.text),
                                ARG_TEXT(signed_name)));
        core_buffer_puts(&unsigned_name, "u");
        core_buffer_append_string(&unsigned_name,
            skip ? core_string_slice(signed_name, 1,
                                     signed_name.u8_bytes - 1)
                 : CORE_STRING_STATIC("int"));
        n00bb_report_help(diagnostics, N00B_TEXT_CONVERT_IT_WITH_CAST,
                          N00BB_ARGS(ARG_TEXT(core_buffer_string(
                              &unsigned_name))));
        core_buffer_release(&unsigned_name);
        return true;
    }

    core_string_t     first = right
        ? operand_phrase(types, unit, left, at->span,
                         flows_from(literal, left) ? literal : nullptr)
        : (core_string_t){0};
    core_string_t     other = first.u8_bytes
        ? operand_phrase(types, unit, right, at->span,
                         flows_from(literal, right) ? literal : nullptr)
        : (core_string_t){0};

    if (!other.u8_bytes) {
        core_free(first.data);
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);

    n00bb_report(diagnostics, unit, N00B_DIAG_OPERATOR_OPERANDS_MISMATCH,
                 value_source(unit, left, at->span).at,
                 N00B_TEXT_OPERANDS_OF_DIFFERENT_TYPES,
                 N00BB_ARGS(ARG_TEXT(binary->spelling), ARG_TEXT(first),
                            ARG_TEXT(other)));
    if (th_kind_of(th_resolve(left->type)) == TH_INDIRECTION
        || th_kind_of(th_resolve(right->type)) == TH_INDIRECTION) {
        n00bb_report_help(diagnostics, N00B_TEXT_READ_THE_REFERENCED_VALUE_WITH,
                          N00BB_NO_ARGS);
    }
    else {
        n00bb_report_help(diagnostics, N00B_TEXT_CONVERT_ONE_OPERAND_SO_BOTH,
                          N00BB_NO_ARGS);
    }
    core_free(first.data);
    core_free(other.data);

    return true;
}

static bool binary_dispatch(const ir_instr_t *in)
{
    return in && in->op == IR_OP_DISPATCH && in->n_operands == 3
        && in->operands[0].kind == IR_OPD_NAME
        && n00bb_binary_operator_method(in->operands[0].name);
}

static bool joins(const ir_instr_t *in, const ir_instr_t *literal,
                  const ir_instr_t *other)
{
    const ir_value_t *left  = dispatch_operand(in, 1);
    const ir_value_t *right = dispatch_operand(in, 2);

    return (flows_from(literal, left) && (!other || flows_from(other, right)))
        || (flows_from(literal, right) && (!other || flows_from(other, left)));
}

static const ir_instr_t *merge_site(const ty_failure_t *failure)
{
    const ir_instr_t *literal = failure->why.at;
    const ir_instr_t *cause   = failure->cause.at;

    if (!literal_failure(failure) || !cause || cause == literal) {
        return nullptr;
    }
    if (binary_dispatch(cause)) {
        return joins(cause, literal, nullptr) ? cause : nullptr;
    }
    if (cause->op != IR_OP_CONST || !literal->block
        || !cause->block || cause->block->owner != literal->block->owner) {
        return nullptr;
    }

    const ir_func_t *function = literal->block->owner;

    for (uint32_t b = 0; b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            if (binary_dispatch(in) && joins(in, literal, cause)) {
                return in;
            }
        }
    }

    return nullptr;
}

static bool report_literal_operator(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *unit,
                                    const ty_failure_t *failure)
{
    const ir_instr_t *site = merge_site(failure);
    const ir_value_t *left = site ? dispatch_operand(site, 1) : nullptr;

    return left
        && report_operator(types, index, unit, site, site->operands[0].name,
                           th_resolve(left->type), failure->why.at);
}

static void describe_arguments(th_ctx *types, const ir_instr_t *at,
                               core_buffer_t *arguments)
{
    core_buffer_puts(arguments, "(");
    for (uint32_t i = 1; i < at->n_operands; i++) {
        const ir_value_t *value = dispatch_operand(at, i);

        if (!value) {
            continue;
        }
        if (arguments->byte_len > 1) {
            core_buffer_puts(arguments, ", ");
        }
        core_buffer_append_string(arguments,
                                  n00bb_print_type(types, value->type));
    }
    core_buffer_puts(arguments, ")");
}

static void note_candidates(th_ctx *types, core_diagnostics_t *diagnostics,
                            const function_list_t *list);

static void report_overload(th_ctx *types, const n00bb_dispatch_index_t *index,
                            core_translation_unit_t *unit,
                            const ir_instr_t *at, core_string_t method)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_buffer_t       arguments   = {0};

    describe_arguments(types, at, &arguments);
    n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_NO_MATCH,
                 n00bb_span_location(at->span),
                 N00B_TEXT_NO_OVERLOAD_OF_ACCEPTS_ARGUMENTS,
                 N00BB_ARGS(ARG_TEXT(method),
                            ARG_TEXT(core_buffer_string(&arguments))));
    core_buffer_release(&arguments);

    bool               found = false;
    function_list_t   *list  = core_dict_get((core_dict_t *)&index->functions,
                                             &method, &found);

    if (found) {
        note_candidates(types, diagnostics, list);
    }
}

// The shape of a dispatch's arguments, as the call's type constraint has it.
static th_type *dispatch_shape(th_ctx *types, const ir_instr_t *at)
{
    th_param   *params = core_alloc_array(th_param, at->n_operands + 1);
    th_kwparam *kws    = core_alloc_array(th_kwparam, at->n_operands + 1);
    size_t      n      = 0;
    size_t      nkw    = 0;

    for (uint32_t o = 1; o < at->n_operands; o++) {
        const ir_operand_t *operand = &at->operands[o];

        if (operand->kind == IR_OPD_NAMED_ARG) {
            kws[nkw++] = (th_kwparam){
                .name = operand->named.name,
                .type = operand->named.value ? operand->named.value->type
                                             : th_var(types)};
            continue;
        }

        const ir_value_t *value = dispatch_operand(at, o);
        th_type          *type  = value ? th_resolve(value->type) : nullptr;

        if (operand->kind == IR_OPD_FRESH && type
            && th_kind_of(type) == TH_INDIRECTION) {
            type = th_ind_referent(type);
        }
        params[n++] = (th_param){.type = type ? type : th_var(types)};
    }

    th_type *shape = th_func(types, at->dest ? at->dest->type : th_var(types),
                             params, n, nullptr, kws, nkw);

    core_free(params);
    core_free(kws);

    return shape;
}

// Reports an overloaded call typing could not send to one function.
static bool report_overload_site(th_ctx *types,
                                 const n00bb_dispatch_index_t *index,
                                 core_translation_unit_t *unit,
                                 const ir_instr_t *at, core_string_t method)
{
    n00bb_overload_site_t *site = overload_site(index, at);

    if (!site || site->chosen) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_buffer_t       arguments   = {0};
    parse_sym_entry_t  *chosen      = nullptr;
    function_list_t     tied        = core_list_new(parse_sym_entry_ptr_t);
    overload_outcome_t  outcome     = choose_overload(
        index, site, at, dispatch_shape(types, at), &chosen, &tied);

    describe_arguments(types, at, &arguments);
    if (outcome == OVERLOAD_AMBIGUOUS) {
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_AMBIGUOUS,
                     n00bb_span_location(at->span),
                     N00B_TEXT_OVERLOADS_OF_FIT_EQUALLY,
                     N00BB_ARGS(ARG_NUMBER(tied.len), ARG_TEXT(method),
                                ARG_TEXT(core_buffer_string(&arguments))));
        note_candidates(types, diagnostics, &tied);
    }
    else if (outcome == OVERLOAD_WAIT) {
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_AMBIGUOUS,
                     n00bb_span_location(at->span),
                     N00B_TEXT_OVERLOAD_ARGUMENTS_UNKNOWN,
                     N00BB_ARGS(ARG_TEXT(method)));
        note_candidates(types, diagnostics, &site->candidates);
    }
    else {
        n00bb_report(diagnostics, unit, N00B_DIAG_OVERLOAD_NO_MATCH,
                     n00bb_span_location(at->span),
                     N00B_TEXT_NO_OVERLOAD_OF_ACCEPTS_ARGUMENTS,
                     N00BB_ARGS(ARG_TEXT(method),
                                ARG_TEXT(core_buffer_string(&arguments))));
        note_candidates(types, diagnostics, &site->candidates);
    }
    core_buffer_release(&arguments);
    core_list_free(tied);

    return true;
}

// Reports each overloaded call typing left waiting on its arguments' types.
static bool check_overload_sites(th_ctx *types,
                                 const n00bb_dispatch_index_t *index,
                                 core_translation_unit_t *const *units,
                                 size_t count, ty_solver_t *solver)
{
    core_dict_t failed;
    bool        ok = true;

    core_dict_init(&failed, nullptr, nullptr);
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        core_dict_put(&failed, (void *)ty_failure_at(solver, i)->why.at,
                      (void *)1);
    }
    for (size_t u = 0; u < count; u++) {
        core_dict_foreach(&tu_state(units[u])->overload_sites, item) {
            n00bb_overload_site_t *site = item->value;

            if (!site->chosen && !core_dict_contains(&failed, item->key)) {
                ok = !report_overload_site(types, index, units[u], item->key,
                                           ((const ir_instr_t *)item->key)
                                               ->operands[0].name)
                  && ok;
            }
        }
    }
    core_dict_free(&failed);

    return ok;
}

static void note_candidates(th_ctx *types, core_diagnostics_t *diagnostics,
                            const function_list_t *list)
{
    for (size_t i = 0; i < list->len; i++) {
        parse_sym_entry_t   *origin = canonical_symbol(list->data[i]);
        core_diag_location_t where  = n00bb_symbol_location(origin);
        core_buffer_t        shown  = {0};

        if (!where.has_location || !origin->owner_unit) {
            continue;
        }
        core_buffer_puts(&shown, "`");
        core_buffer_append_string(&shown,
                                  origin->owner_unit->module.identifier);
        core_buffer_puts(&shown, ".");
        core_buffer_append_string(&shown, origin->name);
        core_buffer_append_string(
            &shown, n00bb_print_type_qualified(types,
                                               n00bb_symbol_signature(origin)));
        core_buffer_puts(&shown, "`");
        n00bb_report_secondary(diagnostics, origin->owner_unit, where,
                               N00B_TEXT_CANDIDATE_IS_DECLARED_HERE,
                               N00BB_ARGS(ARG_TEXT(core_buffer_string(&shown))));
        core_buffer_release(&shown);
    }
}

static bool report_method_argument(th_ctx *types, parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   const ir_instr_t        *at,
                                   const n00bb_dispatch_target_t *target)
{
    th_type          *signature = th_resolve(target->signature);
    n00bb_call_plan_t names     = {0};

    if (!n00bb_call_plan_init(&names, grammar, target->function, signature,
                              0)) {
        n00bb_call_plan_free(&names);
        return false;
    }

    bool reported = false;

    for (uint32_t o = 2; !reported && o < at->n_operands; o++) {
        const ir_operand_t *operand = &at->operands[o];
        const ir_value_t   *value   = operand->kind == IR_OPD_NAMED_ARG
                                        ? operand->named.value
                                        : ir_operand_value(operand);
        size_t              index   = o - 1;

        for (size_t p = 0; operand->kind == IR_OPD_NAMED_ARG
             && p < th_func_nparams(signature); p++) {
            if (core_string_eq(n00bb_call_plan_parameter_name(&names, p),
                               operand->named.name)) {
                index = p;
            }
        }
        if (!value || index >= th_func_nparams(signature)) {
            continue;
        }

        th_type *formal = th_resolve(th_func_param(signature, index)->type);

        th_type *actual = th_resolve(value->type);

        // A formal with the trait's variables in it (`'E?`) is what typing
        // made of it for this call, not something to judge here.
        if (th_same_type(actual, formal) || !th_is_closed(types, formal)
            || is_integer_type(actual) == is_integer_type(formal)) {
            continue;
        }

        core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(
            unit);
        core_string_t       name        = n00bb_call_plan_parameter_name(
            &names, index);
        core_string_t       callee      = target->function->name;
        core_string_t       subject     = name.u8_bytes
            ? n00bb_text(N00B_TEXT_PARAMETER_NAMED_OF,
                         N00BB_ARGS(ARG_NUMBER(index), ARG_TEXT(name),
                                    ARG_TEXT(callee)))
            : n00bb_text(N00B_TEXT_PARAMETER_NUMBER_OF,
                         N00BB_ARGS(ARG_NUMBER(index), ARG_TEXT(callee)));

        core_string_t argument = quoted_type(types, value->type);

        n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_TYPE,
                     value_source(unit, value, at->span).at,
                     N00B_TEXT_IS_BUT_THIS_ARGUMENT_IS,
                     N00BB_ARGS(ARG_TEXT(subject),
                                ARG_TEXT(n00bb_print_type(types, formal)),
                                ARG_TEXT(argument)));
        core_free(argument.data);
        core_free(subject.data);
        n00bb_diag_declared_here(diagnostics, target->function,
                                 N00B_TEXT_IS_DECLARED_HERE);
        reported = true;
    }
    n00bb_call_plan_free(&names);

    return reported;
}

static bool dispatch_callee(const n00bb_dispatch_index_t *index,
                            core_translation_unit_t *unit,
                            const ir_instr_t *at, th_type *receiver,
                            core_string_t method, size_t arity,
                            n00bb_dispatch_target_t *target)
{
    n00bb_link_analysis_t *analysis = index->analysis;

    if (n00bb_overload_target(index, at, target)) {
        return true;
    }
    if (!receiver) {
        return false;
    }
    receiver = th_resolve(receiver);
    if ((n00bb_dispatch_target_for(index, receiver, method, arity, target)
         && target->function)
        || (n00bb_dispatch_target(index, receiver, method, target)
            && target->function)) {
        return true;
    }
    *target = (n00bb_dispatch_target_t){.kind = N00BB_TARGET_TRAIT};
    for (size_t i = 0; !target->function && th_kind_of(receiver) == TH_VAR
         && i < index->bounds.len; i++) {
        const th_trait_requirement *bound = &index->bounds.data[i];

        if (bound->subject == receiver && bound->kind == TH_CON_IMPLEMENTS) {
            target->function = trait_method_requirement_symbol(
                analysis->grammar, analysis, unit, bound->iface, method);
        }
    }

    th_env_method_requirement_t found[2] = {0};

    if (!target->function && th_kind_of(receiver) == TH_VAR
        && th_env_method_requirements(ir_ctx_types(analysis->ir_context),
                                      method, found, 2)
               == 1) {
        target->function = trait_method_requirement_symbol(
            analysis->grammar, analysis, unit, found[0].trait_name, method);
    }
    if (target->function && target->kind == N00BB_TARGET_TRAIT) {
        target->signature = n00bb_symbol_signature(target->function);
    }

    return target->function != nullptr;
}

static bool report_iteration(th_ctx *types, core_translation_unit_t *unit,
                             const ir_instr_t *at, core_string_t method,
                             th_type *receiver)
{
    if (!core_string_eq(method, CORE_STRING_STATIC("__iterator"))) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    source_t            source      = value_source(unit, dispatch_operand(at, 1),
                                                   at->span);
    size_t              count       = 0;

    if (!source.text.u8_bytes) {
        return false;
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_NOT_ITERABLE, source.at,
                 N00B_TEXT_IS_WHICH_CANNOT_BE_ITERATED,
                 N00BB_ARGS(ARG_TEXT(source.text),
                            ARG_TEXT(n00bb_print_type(types, receiver))));
    n00bb_report_help(diagnostics,
                      th_tuple_element(receiver, 0, &count)
                          ? N00B_TEXT_TUPLE_NOT_ITERABLE_HELP
                          : N00B_TEXT_A_FOR_LOOP_NEEDS_A,
                      N00BB_NO_ARGS);

    return true;
}

// A `with` manager's hook calls are the compiler's; one with no method to
// reach means the value is not a manager.
static bool report_with_manager(th_ctx *types, core_translation_unit_t *unit,
                                const ir_instr_t *at, core_string_t method,
                                th_type *receiver)
{
    if (!ir_annot_get((ir_annots_t *)&at->annots, N00BB_MAY_FAIL_KEY)
        || (!core_string_eq(method, CORE_STRING_STATIC("__enter"))
            && !core_string_eq(method, CORE_STRING_STATIC("__exit")))) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    source_t            source      = value_source(unit, dispatch_operand(at, 1),
                                                   at->span);
    core_string_t       text        = source.text.u8_bytes
                                        ? source.text
                                        : CORE_STRING_STATIC("the manager");

    n00bb_report(diagnostics, unit, N00B_DIAG_NOT_A_CONTEXT,
                 source.text.u8_bytes ? source.at
                                      : n00bb_span_location(at->span),
                 N00B_TEXT_IS_WHICH_IS_NOT_A_CONTEXT,
                 N00BB_ARGS(ARG_TEXT(text),
                            ARG_TEXT(n00bb_print_type(types, receiver)),
                            ARG_TEXT(method)));
    n00bb_report_help(diagnostics, N00B_TEXT_A_WITH_NEEDS_A_CONTEXT,
                      N00BB_NO_ARGS);

    return true;
}

static bool report_tuple_index(core_translation_unit_t *unit,
                               const ir_instr_t *at, core_string_t method,
                               th_type *receiver)
{
    size_t count = 0;
    bool   write = core_string_eq(method, CORE_STRING_STATIC("__write"));

    if ((!write && !core_string_eq(method, CORE_STRING_STATIC("__read")))
        || !th_tuple_element(receiver, 0, &count)) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);

    if (write) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TUPLE_IMMUTABLE,
                     n00bb_span_location(at->span), N00B_TEXT_TUPLE_IMMUTABLE,
                     N00BB_NO_ARGS);
        n00bb_report_help(diagnostics, N00B_TEXT_TUPLE_IMMUTABLE_HELP,
                          N00BB_NO_ARGS);
        return true;
    }

    const ir_value_t *index = dispatch_operand(at, 2);
    const ir_instr_t *def   = index ? index->def : nullptr;
    source_t          where = value_source(unit, index, at->span);

    if (def && def->op == IR_OP_CONST && def->n_operands
        && def->operands[0].kind == IR_OPD_CONST
        && !def->operands[0].lit.is_float && !def->operands[0].lit.i.neg) {
        n00bb_report(diagnostics, unit, N00B_DIAG_TUPLE_INDEX_RANGE, where.at,
                     N00B_TEXT_TUPLE_INDEX_RANGE,
                     N00BB_ARGS(ARG_NUMBER(count),
                                ARG_NUMBER((uint64_t)def->operands[0]
                                               .lit.i.mag)));
        return true;
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_TUPLE_INDEX_NOT_CONSTANT,
                 where.at, N00B_TEXT_TUPLE_INDEX_NOT_CONSTANT, N00BB_NO_ARGS);
    n00bb_report_help(diagnostics, N00B_TEXT_TUPLE_INDEX_NOT_CONSTANT_HELP,
                      N00BB_NO_ARGS);

    return true;
}

static bool dispatch_plan_reports(parse_grammar_t         *grammar,
                                  core_translation_unit_t *unit,
                                  const n00bb_written_call_t *site,
                                  const n00bb_dispatch_target_t *target,
                                  bool                     method_style)
{
    n00bb_call_plan_t plan  = {0};
    bool              bound = n00bb_call_plan_init(
        &plan, grammar, target->function, target->signature,
        method_style ? 1 : 0);

    if (bound) {
        n00bb_call_plan_use(&plan, site);
        bound = n00bb_call_plan_bind(&plan);
    }

    if (!bound) {
        n00bb_call_plan_report(&plan, unit,
                               core_translation_unit_diagnostics(unit),
                               site->span);
    }
    n00bb_call_plan_free(&plan);

    return !bound;
}

static bool report_missing_operator(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *unit,
                                    const ir_instr_t *at, core_string_t method,
                                    th_type *receiver)
{
    const n00bb_binary_operator_t *binary = n00bb_binary_operator_method(
        method);
    const ir_value_t              *left   = binary ? dispatch_operand(at, 1)
                                                   : nullptr;
    n00bb_dispatch_target_t        target = {0};

    if (!left || th_kind_of(receiver) == TH_VAR
        || th_kind_of(receiver) == TH_INDIRECTION || th_option_item(receiver)
        || item_kind(left).u8_bytes
        || n00bb_dispatch_target(index, receiver, method, &target)) {
        return false;
    }
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);

    n00bb_report(diagnostics, unit, N00B_DIAG_TRAIT_NOT_IMPLEMENTED,
                 value_source(unit, left, at->span).at,
                 N00B_TEXT_DOES_NOT_IMPLEMENT_FOR_THESE,
                 N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, receiver)),
                            ARG_TEXT(binary->trait)));
    n00bb_report_note(diagnostics, N00B_TEXT_IMPLEMENTATIONS_CONSIDERED,
                      N00BB_ARGS(ARG_NUMBER(th_env_method_candidates(
                                     tu_state(unit)->types, binary->trait,
                                     method, nullptr, 0)),
                                 ARG_TEXT(binary->trait)));

    ir_interval_t bounds = {0};
    core_string_t signed_name = n00bb_print_type(types, receiver);

    if (binary->unsigned_only && n00bb_type_interval(receiver, &bounds)
        && bounds.lo < 0 && signed_name.u8_bytes
        && signed_name.data[0] == 'i') {
        core_buffer_t unsigned_name = {0};

        size_t skip = signed_name.u8_bytes > 1 && signed_name.data[1] >= '0'
                   && signed_name.data[1] <= '9';

        core_buffer_puts(&unsigned_name, "u");
        core_buffer_append_string(&unsigned_name,
                                  core_string_slice(signed_name, skip,
                                                    signed_name.u8_bytes
                                                        - skip));
        n00bb_report_help(diagnostics, N00B_TEXT_BITWISE_NEEDS_UNSIGNED,
                          N00BB_ARGS(ARG_TEXT(core_buffer_string(
                              &unsigned_name))));
        core_buffer_release(&unsigned_name);
    }

    return true;
}

static core_string_t literal_phrase(core_string_t kind);

static core_string_t operand_kind(th_ctx *types, const ir_value_t *value)
{
    core_string_t kind = item_kind(value);

    return kind.u8_bytes ? literal_phrase(kind)
                         : quoted_type(types, value->type);
}

static bool report_operator_pairing(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *unit,
                                    const ir_instr_t *at, core_string_t method,
                                    th_type *receiver)
{
    const n00bb_binary_operator_t *binary = n00bb_binary_operator_method(
        method);
    const ir_value_t              *left   = binary ? dispatch_operand(at, 1)
                                                   : nullptr;
    const ir_value_t              *right  = binary ? dispatch_operand(at, 2)
                                                   : nullptr;
    n00bb_dispatch_target_t        target = {0};
    th_type                       *shape  = left && right
                                     && n00bb_dispatch_target(index, receiver,
                                                              method, &target)
                                     && target.signature
                                       ? th_resolve(target.signature)
                                       : nullptr;
    th_type *self  = shape && th_kind_of(shape) == TH_FUNCTION
                         && th_func_nparams(shape) == 2
                       ? th_resolve(th_func_param(shape, 0)->type)
                       : nullptr;
    th_type *other = self ? th_resolve(th_func_param(shape, 1)->type)
                          : nullptr;

    if (!other || th_kind_of(other) == TH_VAR || th_same_type(self, other)) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       first       = quoted_type(types, receiver);
    core_string_t       second      = operand_kind(types, right);

    n00bb_report(diagnostics, unit, N00B_DIAG_OPERATOR_OPERANDS_MISMATCH,
                 value_source(unit, right, at->span).at,
                 N00B_TEXT_NO_OPERATOR_FOR,
                 N00BB_ARGS(ARG_TEXT(binary->spelling), ARG_TEXT(first),
                            ARG_TEXT(second)));
    n00bb_report_note(diagnostics, N00B_TEXT_OPERATOR_TAKES_ON_RIGHT,
                      N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, receiver)),
                                 ARG_TEXT(binary->spelling),
                                 ARG_TEXT(n00bb_print_type(types, other))));
    core_free(first.data);
    core_free(second.data);

    return true;
}

static bool report_dispatch(th_ctx *types, const n00bb_dispatch_index_t *index,
                            core_translation_unit_t *unit,
                            const ty_failure_t      *failure)
{
    const ir_instr_t *at = failure->why.at;

    if (!at || at->op != IR_OP_DISPATCH || failure->kind != TY_FAIL_PREDICATE
        || failure->pred.kind != TY_PRED_DISPATCH || !failure->pred.subject) {
        return false;
    }

    core_string_t method   = failure->pred.name;
    th_type      *receiver = th_resolve(failure->pred.subject);

    if (report_overload_site(types, index, unit, at, method)) {
        return true;
    }
    if (report_iteration(types, unit, at, method, receiver)
        || report_with_manager(types, unit, at, method, receiver)
        || report_tuple_index(unit, at, method, receiver)
        || report_missing_operator(types, index, unit, at, method, receiver)
        || report_operator(types, index, unit, at, method, receiver,
                           nullptr)) {
        return true;
    }

    const n00bb_written_call_t *call = core_dict_get(
        &tu_state(unit)->dispatch_calls, (void *)at, nullptr);

    if (!call) {
        return false;
    }

    parse_grammar_t  *grammar   = index->analysis->grammar;
    bool method_style = ir_annot_get((ir_annots_t *)&at->annots,
                                     N00BB_DISPATCH_IMPLICIT_SELF_KEY)
        != nullptr;

    n00bb_dispatch_target_t target = {0};
    bool found = dispatch_callee(index, unit, at, receiver, method,
                                 at->n_operands - 1, &target);
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);

    if (!method_style && !found) {
        report_overload(types, index, unit, at, method);
        return true;
    }
    if (!found && n00bb_dispatch_target(index, receiver, method, &target)
        && target.kind == N00BB_TARGET_ZEROED) {
        core_string_t type = n00bb_print_type(types, receiver);

        if (n00bb_report(diagnostics, unit, N00B_DIAG_NEW_WITHOUT_INIT_ARGUMENTS,
                         n00bb_span_location(at->span),
                         N00B_TEXT_NEW_WITHOUT_INIT_TAKES_NO,
                         N00BB_ARGS(ARG_TEXT(type))))
            n00bb_report_help(diagnostics, N00B_TEXT_CREATE_IT_WITH_NEW_THEN,
                              N00BB_ARGS(ARG_TEXT(type)));
        return true;
    }
    if (!found) {
        core_string_t type = n00bb_print_type(types, receiver);

        n00bb_report(diagnostics, unit, N00B_DIAG_NO_TRAIT_PROVIDES,
                     n00bb_span_location(at->span),
                     N00B_TEXT_NO_TRAIT_PROVIDES,
                     N00BB_ARGS(ARG_TEXT(type), ARG_TEXT(method)));
        return true;
    }

    return dispatch_plan_reports(grammar, unit, call, &target, method_style)
        || report_method_argument(types, grammar, unit, at, &target);
}

static core_string_t literal_kind(core_string_t trait)
{
    if (core_string_eq(trait, CORE_STRING_STATIC("String_Syntax"))) {
        return CORE_STRING_STATIC("string");
    }
    if (core_string_eq(trait, CORE_STRING_STATIC("Some_Integer"))) {
        return CORE_STRING_STATIC("integer");
    }
    if (core_string_eq(trait, CORE_STRING_STATIC("Some_Float"))) {
        return CORE_STRING_STATIC("float");
    }

    return (core_string_t){0};
}

static core_string_t literal_phrase(core_string_t kind)
{
    if (core_string_eq(kind, CORE_STRING_STATIC("integer"))) {
        return n00bb_text(N00B_TEXT_AN_INTEGER, N00BB_NO_ARGS);
    }
    if (core_string_eq(kind, CORE_STRING_STATIC("float"))) {
        return n00bb_text(N00B_TEXT_A_FLOAT, N00BB_NO_ARGS);
    }

    return n00bb_text(N00B_TEXT_QUOTED, N00BB_ARGS(ARG_TEXT(kind)));
}

static const ir_instr_t *literal_user(const ir_instr_t *literal,
                                      uint32_t         *operand)
{
    const ir_func_t *function = literal->block ? literal->block->owner
                                               : nullptr;

    for (uint32_t b = 0; function && b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            for (uint32_t o = 0; o < in->n_operands; o++) {
                const ir_operand_t *slot  = &in->operands[o];
                const ir_value_t   *value = slot->kind == IR_OPD_NAMED_ARG
                                              ? slot->named.value
                                              : ir_operand_value(slot);

                if (value == literal->dest) {
                    *operand = o;
                    return in;
                }
            }
        }
    }

    return nullptr;
}

static size_t visible_initializers(const n00bb_dispatch_index_t *index,
                                   th_type                      *receiver)
{
    core_string_t    method = CORE_STRING_STATIC("__init");
    bool             found  = false;
    function_list_t *list   = core_dict_get((core_dict_t *)&index->functions,
                                            &method, &found);
    size_t           count  = 0;

    for (size_t i = 0; found && i < list->len; i++) {
        th_type *signature = th_resolve(
            n00bb_symbol_signature(list->data[i]));

        count += th_kind_of(signature) == TH_FUNCTION
              && th_func_nparams(signature)
              && same_constructor(th_resolve(th_func_param(signature, 0)->type),
                                  th_resolve(receiver));
    }

    return count;
}

static core_string_t index_trait(core_string_t method)
{
    static const struct {
        core_string_t method;
        core_string_t trait;
    } traits[] = {
        {CORE_STRING_STATIC("__read"), CORE_STRING_STATIC("Index_Read")},
        {CORE_STRING_STATIC("__write"), CORE_STRING_STATIC("Index_Write")},
        {CORE_STRING_STATIC("__slice"), CORE_STRING_STATIC("Index_Slice")},
        {CORE_STRING_STATIC("__write_slice"),
         CORE_STRING_STATIC("Index_Slice_Write")},
    };

    for (size_t i = 0; i < sizeof(traits) / sizeof(*traits); i++) {
        if (core_string_eq(method, traits[i].method)) {
            return traits[i].trait;
        }
    }

    return (core_string_t){0};
}

static bool report_literal_argument(th_ctx *types,
                                    const n00bb_dispatch_index_t *index,
                                    core_translation_unit_t *const *units,
                                    size_t count, core_translation_unit_t *unit,
                                    const ty_failure_t *failure)
{
    const ir_instr_t *literal = failure->why.at;
    core_string_t     kind    = failure->kind == TY_FAIL_PREDICATE
                                     && literal && literal->op == IR_OP_CONST
                                     && literal->dest
                                  ? literal_kind(failure->pred.name)
                                  : (core_string_t){0};
    uint32_t          operand = 0;
    const ir_instr_t *user    = kind.u8_bytes ? literal_user(literal, &operand)
                                              : nullptr;

    if (!user || (user->op != IR_OP_CALL && user->op != IR_OP_DISPATCH)
        || operand == 0) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    parse_grammar_t    *grammar     = index->analysis->grammar;
    const ir_value_t   *self        = user->op == IR_OP_DISPATCH
                                        ? dispatch_operand(user, 1)
                                        : nullptr;
    core_string_t       method      = user->op == IR_OP_DISPATCH
                                        ? user->operands[0].name
                                        : (core_string_t){0};

    if (self && core_string_eq(method, CORE_STRING_STATIC("__init"))) {
        const n00bb_written_call_t *call = core_dict_get(
            &tu_state(unit)->dispatch_calls, (void *)user, nullptr);
        th_type      *built = th_resolve(self->type);

        if (th_kind_of(built) == TH_INDIRECTION) {
            built = th_resolve(th_ind_referent(built));
        }
        n00bb_report(diagnostics, unit, N00B_DIAG_CONSTRUCTOR_NO_MATCH,
                     call ? call->span : n00bb_span_location(user->span),
                     N00B_TEXT_NONE_OF_THE_VISIBLE_INIT,
                     N00BB_ARGS(ARG_NUMBER(visible_initializers(index, built)),
                                ARG_TEXT(n00bb_print_type(types, built))));
        return true;
    }

    n00bb_dispatch_target_t target = {0};
    const ir_func_t        *callee = n00bb_direct_call_target(user);

    if (!callee && (!self
                    || !dispatch_callee(index, unit, user, self->type, method,
                                        user->n_operands - 1, &target))) {
        core_string_t trait = self ? index_trait(method)
                                   : (core_string_t){0};

        if (!trait.data) {
            return false;
        }
        th_type *container = th_resolve(self->type);

        if (th_kind_of(container) == TH_INDIRECTION) {
            container = th_resolve(th_ind_referent(container));
        }
        n00bb_report(diagnostics, unit, N00B_DIAG_TRAIT_NOT_IMPLEMENTED,
                     n00bb_span_location(literal->span),
                     N00B_TEXT_DOES_NOT_IMPLEMENT_FOR_THESE,
                     N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, container)),
                                ARG_TEXT(trait)));
        return true;
    }

    th_type          *signature = callee ? callee->type : target.signature;
    n00bb_call_plan_t names     = {0};
    bool              planned   = n00bb_call_plan_init(
        &names, grammar, callee ? nullptr : target.function, signature, 0);
    size_t            index_of  = operand - 1;
    const ir_operand_t *slot    = &user->operands[operand];

    for (size_t p = 0; planned && slot->kind == IR_OPD_NAMED_ARG
         && p < th_func_nparams(th_resolve(signature)); p++) {
        core_string_t name = callee && p < callee->n_params
                               ? callee->params[p]->name
                               : n00bb_call_plan_parameter_name(&names, p);

        if (core_string_eq(name, slot->named.name)) {
            index_of = p;
        }
    }

    th_type *resolved = th_resolve(signature);

    if (!planned || index_of >= th_func_nparams(resolved)) {
        n00bb_call_plan_free(&names);
        return false;
    }

    size_t        shown     = callee ? index_of + 1 : index_of;
    core_string_t parameter = callee && index_of < callee->n_params
                                ? callee->params[index_of]->name
                                : n00bb_call_plan_parameter_name(&names,
                                                                 index_of);
    core_string_t name      = callee ? function_name(callee)
                                     : target.function->name;
    core_string_t subject   = parameter.u8_bytes
        ? n00bb_text(N00B_TEXT_PARAMETER_NAMED_OF,
                     N00BB_ARGS(ARG_NUMBER(shown), ARG_TEXT(parameter),
                                ARG_TEXT(name)))
        : n00bb_text(N00B_TEXT_PARAMETER_NUMBER_OF,
                     N00BB_ARGS(ARG_NUMBER(shown), ARG_TEXT(name)));

    core_string_t phrase = literal_phrase(kind);

    n00bb_report(diagnostics, unit, N00B_DIAG_CALL_ARGUMENT_TYPE,
                 n00bb_span_location(literal->span),
                 N00B_TEXT_IS_BUT_THIS_ARGUMENT_IS,
                 N00BB_ARGS(ARG_TEXT(subject),
                            ARG_TEXT(n00bb_print_type(
                                types, th_func_param(resolved, index_of)
                                           ->type)),
                            ARG_TEXT(phrase)));
    core_free(phrase.data);
    core_free(subject.data);
    n00bb_call_plan_free(&names);
    if (callee) {
        note_declared(units, count, unit, callee);
    }
    else {
        n00bb_diag_declared_here(diagnostics, target.function,
                                 N00B_TEXT_IS_DECLARED_HERE);
    }

    return true;
}

static bool report_pattern(th_ctx *types, core_translation_unit_t *unit,
                           const ty_failure_t *failure)
{
    const ir_instr_t *at = failure->why.at;

    if (!at || at->op != IR_OP_EXTRACT_INDEX || !at->n_operands
        || failure->kind != TY_FAIL_PREDICATE
        || failure->pred.kind != TY_PRED_FIELD || !failure->pred.positional
        || !failure->pred.subject) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    th_type            *subject     = th_resolve(failure->pred.subject);
    const ir_value_t   *unpacked    = ir_operand_value(&at->operands[0]);
    const ir_func_t    *function    = at->block->owner;
    size_t              count       = 0;
    size_t              names       = 0;

    if (!th_tuple_element(subject, 0, &count)) {
        n00bb_report(diagnostics, unit, N00B_DIAG_LOOP_BINDINGS_MISMATCH,
                     n00bb_span_location(at->span),
                     N00B_TEXT_PATTERN_NEEDS_NAME,
                     N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, subject))));
        return true;
    }
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];

            names += in->op == IR_OP_EXTRACT_INDEX && in->n_operands
                  && ir_operand_value(&in->operands[0]) == unpacked;
        }
    }
    n00bb_report(diagnostics, unit, N00B_DIAG_LOOP_BINDINGS_MISMATCH,
                 n00bb_span_location(at->span), N00B_TEXT_PATTERN_ARITY,
                 N00BB_ARGS(ARG_NUMBER(names),
                            ARG_TEXT(n00bb_print_type(types, subject)),
                            ARG_NUMBER(count)));

    return true;
}

static core_string_t stored_name(core_translation_unit_t *unit,
                                 const ir_instr_t        *store,
                                 const ir_instr_t        *literal)
{
    if (store->dest_local && store->dest_local->source_name.u8_bytes) {
        return store->dest_local->source_name;
    }
    if (!ir_span_known(store->span) || !ir_span_known(literal->span)
        || store->span.start_byte >= literal->span.start_byte) {
        return (core_string_t){0};
    }

    ir_span_t lhs = store->span;

    lhs.end_byte = literal->span.start_byte;

    core_string_t text = span_text(unit, lhs);

    while (text.u8_bytes
           && (text.data[text.u8_bytes - 1] == ' '
               || text.data[text.u8_bytes - 1] == '=')) {
        text.u8_bytes--;
    }

    return text;
}

// True when `store` initializes a `var` or `let` that has no declared type,
// so the literal is what gave the variable its type.
static bool untyped_declaration(core_translation_unit_t *unit,
                                const ir_instr_t        *store,
                                const ir_instr_t        *literal)
{
    if (!ir_span_known(store->span) || !ir_span_known(literal->span)
        || store->span.start_byte >= literal->span.start_byte) {
        return false;
    }

    ir_span_t lhs = store->span;

    lhs.end_byte = literal->span.start_byte;

    core_string_t text = span_text(unit, lhs);

    for (size_t i = 0; i < text.u8_bytes; i++) {
        if (text.data[i] == ':') {
            return false;
        }
    }

    // The store's span starts at the name; the keyword comes before it.
    ir_span_t before = store->span;

    before.start_byte = store->span.start_byte > 16
                          ? store->span.start_byte - 16 : 0;
    before.end_byte   = store->span.start_byte;
    text              = span_text(unit, before);
    while (text.u8_bytes
           && (text.data[text.u8_bytes - 1] == ' '
               || text.data[text.u8_bytes - 1] == '\t')) {
        text.u8_bytes--;
    }
    if (text.u8_bytes < 3) {
        return false;
    }

    const char *keyword = text.data + text.u8_bytes - 3;

    return (!memcmp(keyword, "var", 3) || !memcmp(keyword, "let", 3))
        && (text.u8_bytes == 3 || keyword[-1] == ' ' || keyword[-1] == '\t'
            || keyword[-1] == '\n' || keyword[-1] == '{'
            || keyword[-1] == ';');
}

static bool report_literal_store(th_ctx *types, core_translation_unit_t *unit,
                                 const ty_failure_t *failure)
{
    const ir_instr_t *literal = failure->why.at;
    core_string_t     kind    = failure->kind == TY_FAIL_PREDICATE
                                     && literal && literal->op == IR_OP_CONST
                                     && literal->dest && failure->pred.subject
                                  ? literal_kind(failure->pred.name)
                                  : (core_string_t){0};
    uint32_t          operand = 0;
    const ir_instr_t *user    = kind.u8_bytes ? literal_user(literal, &operand)
                                              : nullptr;
    bool              stored  = user
        && (((user->op == IR_OP_STORE || user->op == IR_OP_GLOBAL_STORE)
             && operand == 1)
            || (user->op == IR_OP_COPY && operand == 0)
            || user->op == IR_OP_AGGREGATE);

    if (!stored) {
        return false;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       text        = span_text(unit, literal->span);
    core_string_t       expected    = n00bb_print_type(types,
                                                       failure->pred.subject);
    core_string_t       target      = stored_name(unit, user, literal);

    th_kind  place_kind = th_kind_of(th_resolve(failure->pred.subject));

    if (target.u8_bytes && (place_kind == TH_VAR || place_kind == TH_BOUND)) {
        n00bb_report(diagnostics, unit, N00B_DIAG_ASSIGN_TO_TYPE_PARAMETER,
                     n00bb_span_location(literal->span),
                     N00B_TEXT_CANNOT_ASSIGN_TO_ITS_TYPE,
                     N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(target),
                                ARG_TEXT(expected)));
        n00bb_report_help(diagnostics, N00B_TEXT_ONLY_A_VALUE_OF_TYPE,
                          N00BB_ARGS(ARG_TEXT(expected), ARG_TEXT(target)));
        return true;
    }

    core_string_t phrase = literal_phrase(kind);

    if (returned_storage(user)) {
        core_string_t        name     = function_name(user->block->owner);
        core_diag_location_t declared = return_type_location(
            unit, user->block->owner);

        n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
                     n00bb_span_location(literal->span),
                     N00B_TEXT_IS_BUT_RETURNS,
                     N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(phrase),
                                ARG_TEXT(name), ARG_TEXT(expected)));
        if (declared.has_location) {
            n00bb_report_secondary(diagnostics, unit, declared,
                                   N00B_TEXT_THE_RETURN_TYPE_OF_IS,
                                   N00BB_ARGS(ARG_TEXT(name)));
        }
    }
    else if (target.u8_bytes
             && core_string_eq(kind, CORE_STRING_STATIC("integer"))
             && untyped_declaration(unit, user, literal)) {
        n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
                     n00bb_span_location(literal->span),
                     N00B_TEXT_INFERRED_INTEGER_USED_AS,
                     N00BB_ARGS(ARG_TEXT(target), ARG_TEXT(text),
                                ARG_TEXT(expected)));
        if (float_type(th_resolve(failure->pred.subject))) {
            n00bb_report_help(diagnostics, N00B_TEXT_WRITE_A_FLOAT_LITERAL,
                              N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(target),
                                         ARG_TEXT(expected)));
        }
    }
    else {
        n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
                     n00bb_span_location(literal->span),
                     N00B_TEXT_IS_WHICH_DOES_NOT_CONVERT,
                     N00BB_ARGS(ARG_TEXT(text), ARG_TEXT(phrase),
                                ARG_TEXT(expected)));
    }
    core_free(phrase.data);

    return true;
}

static bool report_body_failure(th_ctx *types, core_translation_unit_t *unit,
                                const ty_failure_t *failure)
{
    const ir_instr_t *at      = failure->why.at;
    core_string_t     kind    = literal_failure(failure)
                                  ? literal_kind(failure->pred.name)
                                  : (core_string_t){0};
    uint32_t          operand = 0;
    const ir_instr_t *raise   = kind.u8_bytes ? literal_user(at, &operand)
                                              : at;
    const ir_func_t  *function = raise && raise->op == IR_OP_FAIL
                                     && raise->block
                                   ? raise->block->owner
                                   : nullptr;
    th_type          *declared = function && function->type
                                   ? th_func_error(th_resolve(function->type))
                                   : nullptr;

    if (!declared || (!kind.u8_bytes && !failure->actual)) {
        return false;
    }

    core_string_t       name        = function_name(function);
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    const ir_value_t   *raised      = !kind.u8_bytes && raise->n_operands
                                          ? ir_operand_value(&raise->operands[0])
                                          : nullptr;

    if (raised && raised->def && raised->def->op == IR_OP_VARIANT_PAYLOAD) {
        core_string_t actual = n00bb_print_type(types, failure->actual);

        if (core_string_eq(function->name,
                           CORE_STRING_STATIC(N00BB_MODULE_INIT_NAME))) {
            n00bb_report(diagnostics, unit, N00B_DIAG_TOP_LEVEL_FAILURE,
                         n00bb_span_location(raise->span),
                         N00B_TEXT_CAN_FAIL_WITH_BUT_IT,
                         N00BB_ARGS(ARG_PHRASE(PHRASE_THIS_CALL),
                                    ARG_TEXT(actual)));
            n00bb_report_help(diagnostics,
                              N00B_TEXT_HANDLE_THE_FAILURE_WITH_TRY,
                              N00BB_NO_ARGS);

            return true;
        }
        if (th_is_nil(declared)) {
            n00bb_report(diagnostics, unit, N00B_DIAG_FAILURE_TYPE_MISMATCH,
                         n00bb_span_location(raise->span),
                         N00B_TEXT_CAN_FAIL_WITH_BUT_DECLARES,
                         N00BB_ARGS(ARG_PHRASE(PHRASE_THIS_CALL),
                                    ARG_TEXT(actual), ARG_TEXT(name)));
        }
        else {
            n00bb_report(diagnostics, unit, N00B_DIAG_FAILURE_TYPE_MISMATCH,
                         n00bb_span_location(raise->span),
                         N00B_TEXT_CAN_FAIL_WITH_BUT_FAILS,
                         N00BB_ARGS(ARG_PHRASE(PHRASE_THIS_CALL),
                                    ARG_TEXT(actual), ARG_TEXT(name),
                                    ARG_TEXT(n00bb_print_type(types,
                                                              declared))));
        }
        n00bb_report_help(diagnostics, N00B_TEXT_HANDLE_THE_FAILURE_WITH_TRY_2,
                          N00BB_NO_ARGS);

        return true;
    }

    core_string_t found = kind.u8_bytes ? literal_phrase(kind)
                                        : quoted_type(types, failure->actual);

    n00bb_report(diagnostics, unit,
                 N00B_DIAG_FUNCTION_BODY_SIGNATURE_MISMATCH,
                 n00bb_name_location(unit, function->span, name),
                 N00B_TEXT_BODY_FAILS_WITH,
                 N00BB_ARGS(ARG_TEXT(name), ARG_TEXT(found),
                            ARG_TEXT(n00bb_print_type(types, declared))));
    core_free(found.data);

    return true;
}

static core_string_t item_kind(const ir_value_t *value)
{
    const ir_instr_t *def   = value ? value->def : nullptr;
    size_t            bytes = 0;

    if (!def || def->op != IR_OP_CONST) {
        return (core_string_t){0};
    }
    if (!def->n_operands || def->operands[0].kind != IR_OPD_CONST) {
        return ir_instr_const_bytes(def, &bytes) ? CORE_STRING_STATIC("string")
                                                 : (core_string_t){0};
    }

    return def->operands[0].lit.text.data ? CORE_STRING_STATIC("string")
         : def->operands[0].lit.is_float  ? CORE_STRING_STATIC("float")
                                          : CORE_STRING_STATIC("integer");
}

static const ir_instr_t *failed_container(const ty_failure_t *failure)
{
    uint32_t            operand = 0;
    const ir_instr_t   *user    = literal_failure(failure)
                                    ? literal_user(failure->why.at, &operand)
                                    : nullptr;
    ir_aggregate_form_t form    = IR_AGGREGATE_ELEMENTS;

    return user && user->op == IR_OP_AGGREGATE
                && !ir_annot_get((ir_annots_t *)&user->annots,
                                 N00BB_RECORD_LITERAL_KEY)
                && (!ir_instr_aggregate_form(user, &form)
                    || form == IR_AGGREGATE_ELEMENTS)
             ? user
             : nullptr;
}

static bool report_container_item(core_translation_unit_t *unit,
                                  const ty_failure_t      *failure)
{
    const ir_instr_t *container = failed_container(failure);
    core_string_t     first     = container && container->n_operands > 1
                                    ? item_kind(ir_operand_value(
                                          &container->operands[0]))
                                    : (core_string_t){0};
    const ir_value_t *odd       = nullptr;

    for (uint32_t o = 1; first.u8_bytes && !odd && o < container->n_operands;
         o++) {
        const ir_value_t *item = ir_operand_value(&container->operands[o]);
        core_string_t     kind = item_kind(item);

        if (kind.u8_bytes && !core_string_eq(kind, first)) {
            odd = item;
        }
    }
    if (!odd) {
        return false;
    }

    source_t      source = value_source(unit, odd, container->span);
    core_string_t phrase = literal_phrase(item_kind(odd));
    bool          whole  = core_string_eq(first, CORE_STRING_STATIC("integer"));

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);

    n00bb_report(diagnostics, unit, N00B_DIAG_LITERAL_ITEM_TYPE_MISMATCH,
                 source.at,
                 whole ? N00B_TEXT_ITEM_IS_BUT_OTHERS_INTEGERS
                       : N00B_TEXT_ITEM_IS_BUT_OTHERS_ARE,
                 N00BB_ARGS(ARG_TEXT(source.text), ARG_TEXT(phrase),
                            ARG_TEXT(first)));
    n00bb_report_secondary(diagnostics, unit,
                           value_source(unit,
                                        ir_operand_value(
                                            &container->operands[0]),
                                        container->span)
                               .at,
                           N00B_TEXT_THE_FIRST_ITEM_IS_HERE, N00BB_NO_ARGS);
    core_free(phrase.data);

    return true;
}

static bool tuple_literal(const ir_instr_t *in)
{
    return in && in->op == IR_OP_AGGREGATE
        && ir_annot_get((ir_annots_t *)&in->annots, N00BB_RECORD_LITERAL_KEY);
}

static const ir_instr_t *failed_tuple(const ty_failure_t *failure)
{
    uint32_t          operand = 0;
    const ir_instr_t *at      = failure->why.at;
    uint32_t          slot    = !at                       ? 0
                              : at->op == IR_OP_COPY      ? 1
                              : at->op == IR_OP_STORE
                                    || at->op == IR_OP_GLOBAL_STORE
                                  ? 2
                                  : 0;
    const ir_value_t *stored  = slot && slot <= at->n_operands
                                  ? ir_operand_value(&at->operands[slot - 1])
                                  : nullptr;
    const ir_instr_t *tuple   = tuple_literal(at)          ? at
                              : stored                    ? stored->def
                              : literal_failure(failure)
                                  ? literal_user(at, &operand)
                                  : nullptr;

    return tuple_literal(tuple) ? tuple : nullptr;
}

static ir_span_t annotation_span(core_translation_unit_t *unit, ir_span_t span)
{
    core_string_t text  = span_text(unit, span);
    size_t        start = 0;
    size_t        end   = 0;

    while (start < text.u8_bytes && text.data[start] != ':') {
        start++;
    }
    start++;
    while (start < text.u8_bytes && text.data[start] == ' ') {
        start++;
    }
    end = start;
    while (end < text.u8_bytes && text.data[end] != ' '
           && text.data[end] != '=') {
        end++;
    }
    if (end <= start || span.start_line != span.end_line) {
        return IR_SPAN_UNKNOWN;
    }
    span.start_byte += (uint32_t)start;
    span.start_col  += (uint32_t)start;
    span.end_byte    = span.start_byte + (uint32_t)(end - start);
    span.end_col     = span.start_col + (uint32_t)(end - start);

    return span;
}

static core_string_t declared_name(core_string_t text)
{
    for (size_t i = 0; i < text.u8_bytes; i++) {
        if (text.data[i] == ':' || text.data[i] == ' '
            || text.data[i] == '=') {
            text.u8_bytes = i;
        }
    }

    return text;
}

static void append_literal_type(th_ctx *types, core_buffer_t *out,
                                const ir_instr_t *tuple)
{
    th_type *record = tuple->dest ? th_resolve(tuple->dest->type) : nullptr;

    core_buffer_puts(out, "`Tuple[");
    for (uint32_t o = 0; o < tuple->n_operands; o++) {
        const ir_value_t *item   = ir_operand_value(&tuple->operands[o]);
        const th_member  *member = record && th_kind_of(record) == TH_RECORD
                                       && o < th_record_len(record)
                                     ? th_record_member(record, o)
                                     : nullptr;
        core_string_t     kind   = item_kind(item);

        if (o) {
            core_buffer_puts(out, ", ");
        }
        if (member) {
            core_buffer_append_string(out, member->name);
            core_buffer_puts(out, ": ");
        }
        if (core_string_eq(kind, CORE_STRING_STATIC("integer"))
            || core_string_eq(kind, CORE_STRING_STATIC("float"))) {
            core_buffer_puts(out, "{");
            core_buffer_append_string(out, kind);
            core_buffer_puts(out, "}");
        }
        else {
            core_buffer_append_string(out, kind.u8_bytes
                                               ? kind
                                               : n00bb_print_type(types,
                                                                  item->type));
        }
    }
    core_buffer_puts(out, "]`");
}

static bool report_tuple_store(th_ctx *types, core_translation_unit_t *unit,
                               const ty_failure_t *failure)
{
    const ir_instr_t *tuple   = failed_tuple(failure);
    uint32_t          operand = 0;
    const ir_instr_t *store   = tuple ? literal_user(tuple, &operand)
                                      : nullptr;
    const ir_value_t *global  = store && store->op == IR_OP_GLOBAL_STORE
                                    && store->n_operands == 2
                                  ? ir_operand_value(&store->operands[0])
                                  : nullptr;
    const ir_global_t *slot   = global ? global->global : nullptr;
    core_string_t     name    = slot ? declared_name(span_text(unit, slot->span))
                              : store && store->dest_local
                                  ? store->dest_local->source_name
                                  : (core_string_t){0};
    th_type          *declared = slot ? slot->type
                               : store && store->dest_local
                                   ? store->dest_local->type
                                   : nullptr;

    if (!name.u8_bytes || !declared) {
        return false;
    }

    core_buffer_t found = {0};

    append_literal_type(types, &found, tuple);

    core_string_t destination = n00bb_text(
        N00B_TEXT_DESTINATION_VARIABLE,
        N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, declared)),
                   ARG_TEXT(name)));

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       type_text   = n00bb_print_type(types, declared);

    n00bb_report(diagnostics, unit, N00B_DIAG_VALUE_TYPE_MISMATCH,
                 n00bb_span_location(tuple->span), N00B_TEXT_VALUE_IS_BUT,
                 N00BB_ARGS(ARG_TEXT(span_text(unit, tuple->span)),
                            ARG_TEXT(core_buffer_string(&found)),
                            ARG_TEXT(destination)));
    if (slot) {
        ir_span_t annotation = annotation_span(unit, slot->span);

        if (ir_span_known(annotation)) {
            n00bb_report_secondary(diagnostics, unit,
                                   n00bb_span_location(annotation),
                                   N00B_TEXT_IS_DECLARED_HERE_2,
                                   N00BB_ARGS(ARG_TEXT(name),
                                              ARG_TEXT(type_text)));
        }
    }
    core_free(destination.data);
    core_buffer_release(&found);

    return true;
}

static core_string_t assigned_name(core_translation_unit_t *unit,
                                   const ir_instr_t        *store,
                                   const ir_instr_t        *value)
{
    const ir_value_t *global = store->op == IR_OP_GLOBAL_STORE
                                    && store->n_operands == 2
                                  ? ir_operand_value(&store->operands[0])
                                  : nullptr;

    return global && global->global
             ? declared_name(span_text(unit, global->global->span))
             : stored_name(unit, store, value);
}

static bool check_empty_lists(th_ctx *types,
                              core_translation_unit_t *const *units,
                              size_t count,
                              const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    bool ok = true;

    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];

        for (uint32_t b = 0;
             th_is_closed(types, function->type) && b < function->n_blocks;
             b++) {
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t         *in      = function->blocks[b]->instrs[i];
                ir_aggregate_form_t form    = IR_AGGREGATE_ELEMENTS;
                uint32_t            operand = 0;

                if (in->op != IR_OP_AGGREGATE || in->n_operands || !in->dest
                    || ir_annot_get(&in->annots, N00BB_RECORD_LITERAL_KEY)
                    || (ir_instr_aggregate_form(in, &form)
                        && form != IR_AGGREGATE_ELEMENTS)
                    || th_is_closed(types, th_resolve(in->dest->type))) {
                    continue;
                }

                core_translation_unit_t *unit = unit_of(units, count, in);
                const ir_instr_t        *user = literal_user(in, &operand);
                core_string_t            name = unit && user
                                                  ? assigned_name(unit, user,
                                                                  in)
                                                  : (core_string_t){0};

                if (!name.u8_bytes) {
                    continue;
                }

                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);

                n00bb_report(diagnostics, unit,
                             N00B_DIAG_CONTAINER_LITERAL_UNTYPED,
                             n00bb_span_location(in->span),
                             N00B_TEXT_EMPTY_LIST_UNTYPED,
                             N00BB_ARGS(ARG_TEXT(name)));
                n00bb_report_help(diagnostics, N00B_TEXT_ANNOTATE_EMPTY_LIST,
                                  N00BB_ARGS(ARG_TEXT(name)));
                ok = false;
            }
        }
    }

    return ok;
}

// Whether a store to `address` writes the variable it starts from. A field
// of an object is in the storage its handle refers to, not in the variable
// holding the handle, so a field taken where an object is kept leaves the
// variable unchanged.
static bool reaches_root(th_ctx *types, const ir_value_t *address)
{
    while (address && address->kind != IR_VAL_GLOBAL) {
        const ir_instr_t *def = address->def;

        if (!def) {
            return false;
        }
        if (def->op == IR_OP_ADDRESS_OF) {
            return true;
        }
        if (def->op != IR_OP_FIELD || !def->n_operands) {
            return false;
        }
        address = ir_operand_value(&def->operands[0]);

        th_type *held = address ? th_resolve(address->type) : nullptr;

        if (held && th_kind_of(held) == TH_INDIRECTION
            && n00bb_reference_definition(
                types, th_resolve(th_ind_referent(held)))) {
            return false;
        }
    }

    return address != nullptr;
}

static n00bb_text_t mode_parameter(th_mode mode)
{
    switch (mode) {
    case TH_OUT:
        return N00B_TEXT_OUT_PARAMETER;
    case TH_INOUT:
        return N00B_TEXT_MODIFY_PARAMETER;
    case TH_CONSUME:
        return N00B_TEXT_CONSUME_PARAMETER;
    case TH_VIEW:
        return N00B_TEXT_VIEW_PARAMETER;
    default:
        return N00B_TEXT_IN_PARAMETER;
    }
}

static bool writes_tuple_element(const ir_instr_t *store)
{
    const ir_value_t *address = store->n_operands == 2
                                  ? ir_operand_value(&store->operands[0])
                                  : nullptr;
    const ir_instr_t *field   = address ? address->def : nullptr;
    const ir_value_t *base    = field && field->op == IR_OP_FIELD
                                    && field->n_operands
                                  ? ir_operand_value(&field->operands[0])
                                  : nullptr;
    th_type          *holder  = base ? th_resolve(base->type) : nullptr;

    if (holder && th_kind_of(holder) == TH_INDIRECTION) {
        holder = th_resolve(th_ind_referent(holder));
    }

    return holder
        && (th_builtin_kind_of(holder) == TH_BUILTIN_TUPLE
            || th_kind_of(holder) == TH_RECORD);
}

static bool check_write_site(core_translation_unit_t  *unit,
                             const n00bb_write_site_t *site)
{
    if (writes_tuple_element(site->store)) {
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);

        n00bb_report(diagnostics, unit, N00B_DIAG_TUPLE_IMMUTABLE,
                     n00bb_node_span(site->target), N00B_TEXT_TUPLE_IMMUTABLE,
                     N00BB_NO_ARGS);
        n00bb_report_help(diagnostics, N00B_TEXT_TUPLE_IMMUTABLE_HELP,
                          N00BB_NO_ARGS);
        return false;
    }

    parse_sym_entry_t *root   = canonical_symbol(site->root);
    bool               param  = root->kind == PARSE_SYM_PARAM;
    bool               frozen = param
        ? !(root->has_mode && th_mode_by_reference(root->mode))
        : root->mutability == PARSE_SYM_IMMUTABLE
              || root->mutability == PARSE_SYM_CONST;

    if (!frozen || site->store->n_operands != 2
        || !reaches_root(tu_state(unit)->types,
                         ir_operand_value(&site->store->operands[0]))) {
        return true;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       subject     = n00bb_text(
        N00B_TEXT_ITS_FIELD, N00BB_ARGS(ARG_TEXT(site->field)));
    core_diag_location_t at         = n00bb_node_span(site->target);

    if (param) {
        n00bb_report(diagnostics, unit, N00B_DIAG_NOT_WRITABLE, at,
                     N00B_TEXT_PARAMETER_CANNOT_BE_ASSIGNED,
                     N00BB_ARGS(ARG_TEXT(root->name),
                                ARG_PHRASE_ID(mode_parameter(
                                    root->has_mode ? root->mode : TH_IN)),
                                ARG_TEXT(subject)));
        n00bb_diag_declared_here(diagnostics, root, N00B_TEXT_IS_DECLARED_HERE);
        n00bb_report_help(diagnostics, N00B_TEXT_DECLARE_IT_AS_MODIFY_TO,
                          N00BB_ARGS(ARG_TEXT(root->name)));
    }
    else {
        n00bb_report(diagnostics, unit, N00B_DIAG_NOT_WRITABLE, at,
                     N00B_TEXT_IS_A_SO_CANNOT_BE,
                     N00BB_ARGS(ARG_TEXT(root->name),
                                ARG_TEXT(root->mutability == PARSE_SYM_CONST
                                             ? CORE_STRING_STATIC("const")
                                             : CORE_STRING_STATIC("let")),
                                ARG_TEXT(subject)));
        n00bb_diag_declared_here(diagnostics, root, N00B_TEXT_IS_DECLARED_HERE);
        if (root->mutability == PARSE_SYM_IMMUTABLE) {
            n00bb_report_help(diagnostics, N00B_TEXT_DECLARE_WITH_VAR_TO_ALLOW,
                              N00BB_ARGS(ARG_TEXT(root->name)));
        }
    }
    core_free(subject.data);

    return false;
}

static bool check_write_sites(core_translation_unit_t *const *units,
                              size_t count)
{
    bool ok = true;

    for (size_t u = 0; u < count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);

        for (size_t i = 0; i < state->write_sites.len; i++) {
            ok = check_write_site(units[u], &state->write_sites.data[i]) && ok;
        }
        state->write_sites.len = 0;
    }

    return ok;
}

static bool check_field_access(th_ctx *types, n00bb_link_analysis_t *analysis,
                               core_translation_unit_t *unit,
                               const ir_instr_t        *in)
{
    if ((in->op != IR_OP_FIELD && in->op != IR_OP_EXTRACT)
        || in->n_operands != 2 || in->operands[1].kind != IR_OPD_NAME
        || !ir_span_known(in->span) || in->span.implied) {
        return true;
    }

    const ir_value_t *base     = ir_operand_value(&in->operands[0]);

    if (base && base->def && base->def->op == IR_OP_NOMINAL_CAST
        && base->def->n_operands) {
        base = ir_operand_value(&base->def->operands[0]);
    }

    th_type          *receiver = base ? th_resolve(base->type) : nullptr;

    if (receiver && in->op == IR_OP_FIELD
        && th_kind_of(receiver) == TH_INDIRECTION) {
        receiver = th_resolve(th_ind_referent(receiver));
    }
    if (!receiver || th_kind_of(receiver) != TH_NAMED) {
        return true;
    }

    core_translation_unit_t *owner = nullptr;
    parse_scope_t           *scope = record_scope_for_type(analysis, receiver,
                                                           &owner);
    parse_sym_entry_t       *field = scope
        ? parse_symtab_lookup_in(scope, in->operands[1].name)
        : nullptr;

    if (!field || !owner || owner == unit || !field->is_field
        || !member_is_private(field, field->owner_type)) {
        return true;
    }

    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t       module      = owner->module.identifier;

    n00bb_report(diagnostics, unit, N00B_DIAG_FIELD_NOT_VISIBLE,
                 n00bb_name_location(unit, in->span, field->name),
                 N00B_TEXT_FIELD_OF_IS_PRIVATE_TO,
                 N00BB_ARGS(ARG_TEXT(field->name),
                            ARG_TEXT(n00bb_print_type(types, receiver)),
                            ARG_TEXT(module)));
    n00bb_report_label(diagnostics, N00B_TEXT_PRIVATE_FIELD, N00BB_NO_ARGS);
    n00bb_diag_declared_here(diagnostics, field,
                             N00B_TEXT_FIELD_IS_DECLARED_HERE);
    n00bb_report_help(diagnostics, N00B_TEXT_USE_A_FUNCTION_OF_MODULE,
                      N00BB_ARGS(ARG_TEXT(module)));

    return false;
}

static bool report_untyped(core_translation_unit_t *unit, core_string_t name,
                           bool mutable, ir_span_t span)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);

    name = n00bb_name_leaf(name);
    n00bb_report(diagnostics, unit, N00B_DIAG_VARIABLE_UNTYPED,
                 n00bb_name_location(unit, span, name),
                 N00B_TEXT_VARIABLE_UNTYPED, N00BB_ARGS(ARG_TEXT(name)));
    n00bb_report_help(diagnostics, N00B_TEXT_VARIABLE_UNTYPED_HELP,
                      N00BB_ARGS(ARG_TEXT(name),
                                 ARG_TEXT(mutable ? CORE_STRING_STATIC("var")
                                                  : CORE_STRING_STATIC("let"))));

    return false;
}

static bool check_untyped(th_ctx *types, core_translation_unit_t *const *units,
                          size_t count)
{
    bool ok = true;

    for (size_t u = 0; u < count; u++) {
        n00bb_tu_state_t *state  = tu_state(units[u]);
        ir_module_t      *module = state->interface_module
                                       || core_diagnostics_failed(
                                           core_translation_unit_diagnostics(
                                               units[u]))
                                     ? nullptr
                                     : state->ir_module;

        for (size_t g = 0; module && g < ir_global_count(module); g++) {
            ir_global_t *global = ir_global_at(module, g);
            th_type     *type   = global->type ? th_resolve(global->type)
                                               : nullptr;

            if (type && th_kind_of(type) == TH_VAR && !th_var_ref(type)
                && ir_span_known(global->span) && !global->span.implied) {
                ok = report_untyped(units[u], global->name,
                                    global->mutability == IR_MUT_MUTABLE,
                                    global->span)
                  && ok;
            }
        }
        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);

            if (!th_is_closed(types, function->type)) {
                continue;
            }
            for (uint32_t l = 0; l < function->n_locals; l++) {
                ir_local_t *local = function->locals[l];
                th_type    *type  = local->type ? th_resolve(local->type)
                                                : nullptr;

                if (type && th_kind_of(type) == TH_VAR && !th_var_ref(type)
                    && !local->synthetic && local->source_name.u8_bytes
                    && ir_span_known(local->span) && !local->span.implied) {
                    ok = report_untyped(units[u], local->source_name,
                                        local->mutability == IR_MUT_MUTABLE,
                                        local->span)
                      && ok;
                }
            }
        }
    }

    return ok;
}

static bool check_field_privacy(th_ctx *types, n00bb_link_analysis_t *analysis,
                                core_translation_unit_t *const *units,
                                size_t count)
{
    bool ok = true;

    for (size_t u = 0; u < count; u++) {
        n00bb_tu_state_t *state  = tu_state(units[u]);
        ir_module_t      *module = state->ir_module;

        for (size_t f = 0; module && !state->interface_module
             && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);

            for (uint32_t b = 0; b < function->n_blocks; b++) {
                for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                    ok = check_field_access(types, analysis, units[u],
                                            function->blocks[b]->instrs[i])
                      && ok;
                }
            }
        }
    }

    return ok;
}

static bool check_dispatch_call(th_ctx *types,
                                const n00bb_dispatch_index_t *index,
                                core_translation_unit_t *unit,
                                const ir_instr_t *at, const n00bb_written_call_t *call)
{
    const ir_value_t *self = dispatch_operand(at, 1);

    if ((!self && !overload_site(index, at))
        || at->operands[0].kind != IR_OPD_NAME) {
        return true;
    }

    parse_grammar_t        *grammar   = index->analysis->grammar;
    n00bb_dispatch_target_t target    = {0};

    bool method_style = ir_annot_get((ir_annots_t *)&at->annots,
                                     N00BB_DISPATCH_IMPLICIT_SELF_KEY)
        != nullptr;
    if (!dispatch_callee(index, unit, at, self ? self->type : nullptr,
                         at->operands[0].name, at->n_operands - 1,
                         &target)) {
        return true;
    }

    return !dispatch_plan_reports(grammar, unit, call, &target, method_style)
        && (target.kind != N00BB_TARGET_TRAIT
            || !report_method_argument(types, grammar, unit, at, &target));
}

static bool check_dispatch_arities(th_ctx *types,
                                   const n00bb_dispatch_index_t *index,
                                   core_translation_unit_t *const *units,
                                   size_t count, ty_solver_t *solver)
{
    core_dict_t failed;
    bool        ok = true;

    core_dict_init(&failed, nullptr, nullptr);
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        core_dict_put(&failed, (void *)ty_failure_at(solver, i)->why.at,
                      (void *)1);
    }
    for (size_t u = 0; u < count; u++) {
        core_dict_foreach(&tu_state(units[u])->dispatch_calls, item) {
            const ir_instr_t *at = item->key;

            if (!core_dict_contains(&failed, (void *)at)) {
                ok = check_dispatch_call(types, index, units[u], at,
                                         item->value)
                  && ok;
            }
        }
    }
    core_dict_free(&failed);

    return ok;
}

static th_type *method_signature(void *context, th_type *receiver,
                                 core_string_t method, th_type *call)
{
    n00bb_dispatch_target_t target = {0};
    th_type                *shape  = call ? th_resolve(call) : nullptr;
    size_t                  arity  = shape && th_kind_of(shape) == TH_FUNCTION
                                       ? th_func_nparams(shape)
                                             + th_func_nkw(shape)
                                       : SIZE_MAX;

    return n00bb_dispatch_target_for(context, receiver, method, arity,
                                     &target)
             ? target.signature
             : nullptr;
}

static size_t dispatch_requirements(void *context, const ir_instr_t *at,
                                    th_type *receiver, core_string_t method,
                                    th_type *call, th_type *signature,
                                    th_trait_requirement *requirements,
                                    size_t max_requirements)
{
    n00bb_dispatch_index_t *index  = context;
    th_ctx                 *types  = ir_ctx_types(index->analysis->ir_context);
    n00bb_dispatch_target_t target = {0};
    th_type                *shape  = call ? th_resolve(call) : nullptr;
    size_t                  arity  = shape && th_kind_of(shape) == TH_FUNCTION
                                       ? th_func_nparams(shape)
                                             + th_func_nkw(shape)
                                       : SIZE_MAX;

    if (!n00bb_overload_target(index, at, &target)
        && !n00bb_dispatch_target_for(context, receiver, method, arity,
                                      &target)) {
        return 0;
    }

    core_list_t(th_trait_requirement) declared =
        core_list_new(th_trait_requirement);

    if (target.kind == N00BB_TARGET_FUNCTION && target.function) {
        core_list_free(declared);
        declared = n00bb_declared_bounds(target.function);
    }
    else if (target.kind == N00BB_TARGET_METHOD
             && target.method.qualified_name.data
             && th_ctx_environment(types)) {
        size_t count = th_env_function_requirements(
            th_ctx_environment(types), target.method.qualified_name, nullptr,
            0);

        for (size_t i = 0; i < count; i++) {
            core_list_push(declared, ((th_trait_requirement){0}));
        }
        th_env_function_requirements(th_ctx_environment(types),
                                     target.method.qualified_name,
                                     declared.data, declared.len);
    }
    for (size_t i = 0; requirements && i < declared.len
         && i < max_requirements; i++) {
        th_trait_requirement requirement = declared.data[i];
        th_type *subject = var_named_in(types, signature,
            th_var_name(th_resolve(requirement.subject)));

        requirement.subject = subject ? subject : requirement.subject;
        requirements[i] = requirement;
    }

    size_t count = declared.len;

    core_list_free(declared);

    return count;
}

static core_string_t function_parameter_name(
    const n00bb_dispatch_index_t *dispatch, parse_sym_entry_t *function,
    size_t index)
{
    n00bb_function_parameter_facts_t *facts  = function
        ? parse_sym_binding(function, N00BB_PARAMETER_FACTS_KEY)
        : nullptr;
    parse_sym_entry_t                 *formal =
        facts && index < facts->param_count && facts->params[index].node
            ? parse_tree_sym(facts->params[index].node)
            : nullptr;

    if (formal) {
        return formal->name;
    }

    // A function from an interface (an imported module's cached build) has
    // no parse tree; its parameter names come from the interface.
    core_string_t name = {0};

    if (function && dispatch && dispatch->analysis) {
        parse_grammar_t *grammar = dispatch->analysis->grammar;

        (void)function_parameter_info(grammar, function,
            callable_formal_parameters(grammar, function), index,
            nullptr, &name, nullptr, nullptr);
    }

    return name;
}

static core_string_t parameter_name(void *context, const ir_instr_t *at,
                                    th_type *receiver, core_string_t method,
                                    size_t index)
{
    n00bb_dispatch_target_t target = {0};

    if (!n00bb_overload_target(context, at, &target)
        && !n00bb_dispatch_target(context, receiver, method, &target)) {
        return (core_string_t){0};
    }

    return function_parameter_name(context, target.function, index);
}

static bool coerces(void *context, th_type *to, th_type *from)
{
    n00bb_dispatch_index_t *index = context;
    th_ctx                 *types = ir_ctx_types(index->analysis->ir_context);
    core_string_t           name  = {0};

    if (th_is_nil(th_resolve(from))) {
        return n00bb_declared_flagset(index->analysis, to);
    }
    if (n00bb_reference_views(from, to) || n00bb_value_views(from, to)) {
        return true;
    }
    // A float widens to a wider float; f32 is the only narrower one.
    if (float_type(th_resolve(from)) && float_type(th_resolve(to))
        && th_named_info(th_resolve(from), &name, nullptr, nullptr)
        && core_string_eq(name, CORE_STRING_STATIC("f32"))) {
        return true;
    }

    return th_hash(types, to)
               == th_hash(types, th_name(types, CORE_STRING_STATIC("string")))
        && th_named_info(from, &name, nullptr, nullptr)
        && core_string_eq(name,
               core_string_borrow(n00bb_buffer_type_name,
                                  sizeof(n00bb_buffer_type_name) - 1));
}

static bool union_arm(void *context, th_type *record, core_string_t member)
{
    n00bb_dispatch_index_t *index = context;

    return n00bb_union_arm_group(
               th_hash(ir_ctx_types(index->analysis->ir_context), record),
               member)
        != 0;
}

static bool read_anywhere(const ir_pdg_t *g, ir_instr_t *in)
{
    for (uint32_t i = 0; i < ir_pdg_out_count(g, in); i++) {
        ir_dep_kind_t kind = ir_pdg_out_at(g, in, i)->kind;

        if (kind == IR_DEP_DATA || kind == IR_DEP_MEMORY) {
            return true;
        }
    }

    return false;
}

static void register_unwrap(void *state, ir_instr_t *unwrap)
{
    n00bb_tu_state_t *unit_state = state;

    core_list_push(unit_state->rewrite_sites, unwrap);
}

static bool settle_inferred_slot(th_type *declared, th_type *inferred)
{
    th_type *slot = declared ? th_resolve(declared) : nullptr;

    if (slot && inferred && th_kind_of(slot) == TH_VAR && !th_var_ref(slot)) {
        th_var_bind(slot, th_resolve(inferred));
        return true;
    }

    return false;
}

static void refresh_interface_contract(n00bb_link_analysis_t *analysis,
                                       parse_sym_entry_t     *symbol,
                                       const ir_func_t       *function)
{
    core_translation_unit_t *unit = unit_for_ir_module(analysis,
                                                       function->owner);
    th_env *environment = analysis->interface_environment;

    if (!unit || !environment || th_env_is_frozen(environment)) {
        return;
    }

    th_ctx  *types    = tu_state(unit)->types;
    th_type *contract = n00bb_symbol_contract(types, symbol);
    core_buffer_t qualified = {0};

    if (contract
        && append_environment_symbol_name(analysis->grammar, unit, symbol,
                                          &qualified)) {
        (void)(th_env_replace_function_contract(
                   environment, core_buffer_string(&qualified), contract)
               && n00bb_replace_function_typevars(
                   types, environment, symbol, contract,
                   core_buffer_string(&qualified)));
    }
    core_buffer_release(&qualified);
}

static bool settle_parameters(th_ctx *types, th_type *declared,
                              th_type *solved)
{
    bool settled = false;

    for (size_t i = 0; i < th_func_nparams(declared)
                       && i < th_func_nparams(solved); i++) {
        th_type *slot = th_func_param(declared, i)->type;
        th_type **open = nullptr;

        if (slot && th_free_vars(types, slot, &open)
            && th_match(slot, th_func_param(solved, i)->type)) {
            settled = true;
        }
    }

    return settled;
}

static void settle_symbol_signatures(n00bb_dispatch_index_t *index)
{
    th_ctx *types = ir_ctx_types(index->analysis->ir_context);

    for (size_t l = 0; l < index->lists.len; l++) {
        function_list_t *list = index->lists.data[l];

        for (size_t i = 0; i < list->len; i++) {
            parse_sym_entry_t *symbol   = list->data[i];
            ir_func_t         *function = link_ir_function_for_symbol(
                index->analysis, symbol);
            th_type *source = symbol->type ? th_resolve(symbol->type)
                                           : nullptr;
            th_type *solved = function && function->type
                                ? th_resolve(function->type)
                                : nullptr;

            if (!source || !solved || th_kind_of(source) != TH_FUNCTION
                || th_kind_of(solved) != TH_FUNCTION) {
                continue;
            }
            bool settled = settle_parameters(types, source, solved);

            settled = settle_inferred_slot(th_func_ret(source),
                                           th_func_ret(solved))
                   || settled;

            settled = settle_inferred_slot(th_func_error(source),
                                           th_func_error(solved))
                   || settled;

            th_type *contract = symbol->callable_contract
                                  ? th_resolve(symbol->callable_contract)
                                  : nullptr;

            if (contract && th_kind_of(contract) == TH_FUNCTION) {
                settled = settle_parameters(types, contract, solved)
                       || settled;
                settled = settle_inferred_slot(th_func_ret(contract),
                                               th_func_ret(solved))
                       || settled;
                settled = settle_inferred_slot(th_func_error(contract),
                                               th_func_error(solved))
                       || settled;
            }
            if (settled) {
                refresh_interface_contract(index->analysis, symbol, function);
            }
        }
    }
}

static void prepare_function(n00bb_tu_state_t *state, ir_func_t *f,
                             ir_instr_t *result_default)
{
    ir_pdg_t *g      = ir_func_pdg(f);
    bool      unread = g && result_default && !read_anywhere(g, result_default);
    th_type  *ret    = th_kind_of(th_resolve(f->type)) == TH_FUNCTION
                           ? th_resolve(th_func_ret(th_resolve(f->type)))
                           : nullptr;

    ty_refine_options(f, g, register_unwrap, state);

    ir_value_t *nil = result_default
        ? ir_operand_value(
              &result_default->operands[result_default->n_operands - 1])
        : nullptr;
    bool declared = ret && !th_is_nil(ret)
                 && (th_kind_of(ret) != TH_VAR || th_var_name(ret).u8_bytes)
                 && !th_option_item(ret);

    if (unread) {
        ir_instr_erase(result_default);
        if (nil && nil->def) {
            ir_instr_erase(nil->def);
        }
    }
    else if (declared && nil && nil->def && nil->def->op == IR_OP_NIL) {
        nil->def->op = IR_OP_UNDEF;
        nil->type    = ret;
    }
}

static th_type *best_integer_fit(th_ctx *types, ir_abcd_range_t found)
{
    static const struct {
        core_string_t name;
        ir_int_t      lo;
        ir_int_t      hi;
    } fits[] = {
        {CORE_STRING_STATIC("u8"), 0, 0xff},
        {CORE_STRING_STATIC("u16"), 0, 0xffff},
        {CORE_STRING_STATIC("u32"), 0, 0xffffffff},
        {CORE_STRING_STATIC("uint"), 0, (ir_int_t)UINT64_MAX},
        {CORE_STRING_STATIC("i8"), INT8_MIN, INT8_MAX},
        {CORE_STRING_STATIC("i16"), INT16_MIN, INT16_MAX},
        {CORE_STRING_STATIC("i32"), INT32_MIN, INT32_MAX},
        {CORE_STRING_STATIC("int"), INT64_MIN, INT64_MAX},
    };
    ir_interval_t range         = found.seen;
    bool          unbounded_min = found.unbounded_min;
    bool          unbounded_max = found.unbounded_max;
    core_string_t chosen        = {0};

    if (!unbounded_min && !unbounded_max) {
        for (size_t i = 0; !chosen.data && i < sizeof(fits) / sizeof(*fits);
             i++) {
            if ((range.lo >= 0) == (fits[i].lo == 0) && range.lo >= fits[i].lo
                && range.hi <= fits[i].hi) {
                chosen = fits[i].name;
            }
        }
        if (!chosen.data) {
            chosen = range.lo >= 0 ? CORE_STRING_STATIC("u128")
                                   : CORE_STRING_STATIC("i128");
        }
    }
    else if (unbounded_min) {
        chosen = !unbounded_max && range.hi > INT64_MAX
                   ? CORE_STRING_STATIC("i128")
                                      : CORE_STRING_STATIC("int");
    }
    else {
        chosen = range.lo >= 0 ? CORE_STRING_STATIC("uint")
                               : CORE_STRING_STATIC("int");
    }

    return th_name(types, chosen);
}

th_type *n00bb_integer_fit(th_ctx *types, __int128 value)
{
    return best_integer_fit(types, (ir_abcd_range_t){
        .seen = {.lo = value < 0 ? (ir_int_t)value : 0,
                 .hi = (ir_int_t)value}});
}

typedef struct {
    th_type        *var;
    ir_abcd_range_t range;
    bool            seen;
    th_type        *overload;
    ir_interval_t   overload_bounds;
    bool            overloads_differ;
    bool            bitwise;
    const ir_instr_t *negative_at;
    ir_int_t        negative;
    // The integer types the overloaded calls it is passed to take for it.
    ir_module_t    *module;
    th_type        *choices[8];
    size_t          choice_count;
    bool            choices_overflow;
} integer_evidence_t;

core_list_decl(integer_evidence_t);

static ir_abcd_range_t unknown_integer(void)
{
    return (ir_abcd_range_t){
        .seen          = {ir_int_min, ir_int_max},
        .unbounded_min = true,
        .unbounded_max = true,
    };
}

static void note_integer_value(core_list_t(integer_evidence_t) *evidence,
                               ir_abcd_t *abcd, const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;

    for (size_t i = 0; type && i < evidence->len; i++) {
        integer_evidence_t *entry = &evidence->data[i];

        if (entry->var != type) {
            continue;
        }

        ir_abcd_range_t range = unknown_integer();

        (void)ir_abcd_range_of(abcd, value, nullptr, &range);
        // Without explicit evidence, the range analysis caps at 64 bits: a
        // range past 64 bits that is not an exact constant (say, a product
        // of loop-bounded values) counts as unbounded there, which sizes as
        // a 64-bit type with overflow checks.
        if (range.seen.lo != range.seen.hi) {
            if (range.seen.hi > (ir_int_t)UINT64_MAX) {
                range.unbounded_max = true;
            }
            if (range.seen.lo < (ir_int_t)INT64_MIN) {
                range.unbounded_min = true;
            }
        }
        if (entry->seen) {
            ir_abcd_range_t prior = entry->range;

            range.unbounded_min = range.seen.lo == prior.seen.lo
                                    ? range.unbounded_min
                                          || prior.unbounded_min
                                  : range.seen.lo < prior.seen.lo
                                    ? range.unbounded_min
                                    : prior.unbounded_min;
            range.unbounded_max = range.seen.hi == prior.seen.hi
                                    ? range.unbounded_max
                                          || prior.unbounded_max
                                  : range.seen.hi > prior.seen.hi
                                    ? range.unbounded_max
                                    : prior.unbounded_max;
            range.seen = ir_interval_join(range.seen, prior.seen);
        }
        entry->range = range;
        entry->seen  = true;
        return;
    }
}

typedef struct {
    ir_module_t *module;
    core_dict_t  open;
    bool         any_var;
} integer_view_t;

static bool integer_like(integer_view_t *view, const ir_value_t *value)
{
    th_type *type = value && value->type ? th_resolve(value->type) : nullptr;

    return type
        && (core_dict_contains(&view->open, type)
            || (view->any_var && th_kind_of(type) == TH_VAR)
            || ir_interval_of_integer_type(view->module, type, nullptr));
}

static const ir_instr_t *defining(const ir_value_t *value)
{
    value = ir_value_uncopied(value);

    return value && value->kind == IR_VAL_INSTR ? value->def : nullptr;
}

static bool order_view(integer_view_t *view, const ir_instr_t *in,
                       ir_abcd_view_t *out)
{
    static const struct {
        core_string_t name;
        ir_op_t       op;
    } outcomes[] = {
        {CORE_STRING_STATIC("less"), IR_OP_LT},
        {CORE_STRING_STATIC("greater"), IR_OP_GT},
        {CORE_STRING_STATIC("equal"), IR_OP_EQ},
    };

    for (uint32_t side = 0; side < 2; side++) {
        const ir_instr_t *order    = defining(ir_value_operand(in, side));
        const ir_instr_t *constant = defining(ir_value_operand(in, 1 - side));

        if (!order || order->op != IR_OP_DISPATCH || order->n_operands != 3
            || order->operands[0].kind != IR_OPD_NAME
            || !core_string_eq(order->operands[0].name,
                               CORE_STRING_STATIC("__cmp"))
            || !constant || constant->op != IR_OP_ENUM_CONST
            || !constant->n_operands
            || !integer_like(view, ir_value_operand(order, 1))
            || !integer_like(view, ir_value_operand(order, 2))) {
            continue;
        }
        for (size_t i = 0; i < sizeof(outcomes) / sizeof(*outcomes); i++) {
            if (core_string_eq(constant->operands[0].name, outcomes[i].name)) {
                *out = (ir_abcd_view_t){
                    .op    = in->op == IR_OP_EQ
                               ? outcomes[i].op
                               : ir_comparison_negated(outcomes[i].op),
                    .left  = ir_value_operand(order, 1),
                    .right = ir_value_operand(order, 2)};
                return true;
            }
        }
    }

    return false;
}

static bool integer_view(void *context, const ir_instr_t *in,
                         ir_abcd_view_t *out)
{
    integer_view_t *view = context;

    if ((in->op == IR_OP_EQ || in->op == IR_OP_NE) && in->n_operands == 2) {
        return order_view(view, in, out);
    }
    if (in->op != IR_OP_DISPATCH || !in->dest || in->n_operands < 2
        || in->operands[0].kind != IR_OPD_NAME || !integer_like(view, in->dest)) {
        return false;
    }

    core_string_t                  method   = in->operands[0].name;
    const n00bb_binary_operator_t *operator =
        n00bb_binary_operator_method(method);
    const ir_value_t *left  = ir_value_operand(in, 1);
    const ir_value_t *right = in->n_operands == 3 ? ir_value_operand(in, 2)
                                                  : nullptr;

    if (operator && right && integer_like(view, left)
        && integer_like(view, right)) {
        *out = (ir_abcd_view_t){.op = operator->op, .left = left,
                                .right = right, .trapping = true};
        return true;
    }
    if (core_string_eq(method, CORE_STRING_STATIC("__negate"))
        && in->n_operands == 2 && integer_like(view, left)) {
        *out = (ir_abcd_view_t){.op = IR_OP_NEG, .left = left,
                                .trapping = true};
        return true;
    }

    return false;
}

bool n00bb_integer_range(ir_func_t *function, const ir_value_t *value,
                         const ir_instr_t *at, ir_abcd_range_t *range)
{
    integer_view_t view = {.module = function->owner, .any_var = true};

    core_dict_init(&view.open, nullptr, nullptr);

    ir_abcd_hooks_t hooks = {.context = &view, .view = integer_view};
    ir_abcd_t      *abcd  = ir_abcd_build_with(function, &hooks);
    bool            known = abcd && ir_abcd_range_of(abcd, value, at, range)
                 && !range->unbounded_min && !range->unbounded_max;

    ir_abcd_free(abcd);
    core_dict_free(&view.open);

    return known;
}

static bool constant_index(void *context, const ir_instr_t *at,
                           uint64_t *index)
{
    (void)context;
    const ir_value_t *value    = at->n_operands == 3
                                   ? ir_operand_value(&at->operands[2])
                                   : nullptr;
    ir_func_t        *function = at->block ? at->block->owner : nullptr;

    ir_abcd_range_t range = {0};

    if (!value || !function
        || !n00bb_integer_range(function, value, at, &range)
        || range.seen.lo != range.seen.hi || range.seen.lo < 0) {
        return false;
    }
    *index = (uint64_t)range.seen.lo;

    return ir_annot_set(ir_module_ctx(function->owner),
                        &((ir_instr_t *)at)->annots,
                        (ir_annot_t){.key  = N00BB_TUPLE_INDEX_KEY,
                                     .kind = IR_ANNOT_INT,
                                     .i    = {.mag = *index}});
}

static bool integer_bounds(ir_module_t *module, th_type *type,
                           ir_interval_t *bounds)
{
    ir_machine_t machine = th_is_closed(ir_module_types(module), type)
                             ? ir_machine_of(module, th_resolve(type))
                             : (ir_machine_t){0};

    if ((machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
        || machine.role != IR_ROLE_NONE || !machine.width
        || machine.width > 64) {
        return false;
    }

    ir_int_t span = (ir_int_t)1 << (machine.width - 1);

    *bounds = machine.class == IR_CLASS_SINT
                ? (ir_interval_t){-span, span - 1}
                : (ir_interval_t){0, span * 2 - 1};
    return true;
}

static void note_choice(integer_evidence_t *entry, ir_module_t *module,
                        th_type *type)
{
    for (size_t i = 0; i < entry->choice_count; i++) {
        if (ir_type_eq(module, entry->choices[i], type)) {
            return;
        }
    }
    if (entry->choice_count == sizeof(entry->choices) / sizeof(*entry->choices)) {
        entry->choices_overflow = true;
        return;
    }
    entry->module                          = module;
    entry->choices[entry->choice_count++] = type;
}

// An integer literal passed to an overloaded call notes the integer type
// each candidate takes in its place.
static bool note_overload_site(core_list_t(integer_evidence_t) *evidence,
                               const n00bb_dispatch_index_t *index,
                               ir_module_t *module, const ir_instr_t *in)
{
    n00bb_overload_site_t *site = overload_site(index, in);

    if (!site) {
        return false;
    }

    uint32_t position = 0;

    for (uint32_t o = 1; o < in->n_operands; o++) {
        const ir_value_t *value = dispatch_operand(in, o);

        if (!value) {
            continue;
        }

        th_type *type = value->type ? th_resolve(value->type) : nullptr;

        for (size_t e = 0; type && e < evidence->len; e++) {
            integer_evidence_t *entry = &evidence->data[e];

            if (entry->var != type) {
                continue;
            }
            for (size_t c = 0; c < site->candidates.len; c++) {
                th_type *signature = th_resolve(function_signature(
                    index, canonical_symbol(site->candidates.data[c])));
                th_type *param = th_kind_of(signature) == TH_FUNCTION
                    ? overload_param(signature, position) : nullptr;

                param = param ? th_resolve(param) : nullptr;
                if (param && integer_type(param)) {
                    note_choice(entry, module, param);
                }
            }
        }
        position++;
    }

    return true;
}

// Of the noted integer types that hold the literal's range, the one that
// promotes to each other one, if there is one.
static th_type *chosen_integer(const integer_evidence_t *entry,
                               ir_abcd_range_t range)
{
    th_type *fitting[8];
    size_t   count = 0;

    for (size_t i = 0; !entry->choices_overflow && i < entry->choice_count;
         i++) {
        ir_interval_t bounds = {0};

        if (integer_bounds(entry->module, entry->choices[i], &bounds)
            && !range.unbounded_min && !range.unbounded_max
            && range.seen.lo >= bounds.lo && range.seen.hi <= bounds.hi) {
            fitting[count++] = entry->choices[i];
        }
    }
    for (size_t i = 0; i < count; i++) {
        bool narrowest = true;

        for (size_t j = 0; narrowest && j < count; j++) {
            narrowest = i == j
                || numeric_promotes(entry->module, fitting[i], fitting[j]);
        }
        if (narrowest) {
            return fitting[i];
        }
    }

    return nullptr;
}

static void note_overload_evidence(core_list_t(integer_evidence_t) *evidence,
                                   const n00bb_dispatch_index_t *index,
                                   ir_module_t *module, const ir_instr_t *in)
{
    if (in->op == IR_OP_DISPATCH
        && note_overload_site(evidence, index, module, in)) {
        return;
    }

    const ir_value_t *receiver = in->op == IR_OP_DISPATCH
            && in->n_operands >= 2 && in->operands[0].kind == IR_OPD_NAME
        ? ir_value_operand(in, 1) : nullptr;
    th_type *type = receiver && receiver->type ? th_resolve(receiver->type)
                                               : nullptr;
    integer_evidence_t *entry = nullptr;

    for (size_t i = 0; type && !entry && i < evidence->len; i++) {
        if (evidence->data[i].var == type) {
            entry = &evidence->data[i];
        }
    }

    bool             found = false;
    function_list_t *list  = entry ? core_dict_get(
        (core_dict_t *)&index->functions, &in->operands[0].name, &found)
                                   : nullptr;

    for (size_t i = 0; found && i < list->len; i++) {
        th_type *signature = th_resolve(n00bb_symbol_signature(list->data[i]));
        th_type *first = th_kind_of(signature) == TH_FUNCTION
                && th_func_nparams(signature)
            ? th_func_param(signature, 0)->type : nullptr;
        ir_interval_t bounds = {0};

        if (!first || !integer_bounds(module, first, &bounds)) {
            continue;
        }
        if (entry->overload
            && !ir_type_eq(module, entry->overload, first)) {
            entry->overloads_differ = true;
        }
        entry->overload        = first;
        entry->overload_bounds = bounds;
    }
}

static void note_bitwise_operand(core_list_t(integer_evidence_t) *evidence,
                                 ir_abcd_t *abcd, const ir_instr_t *in,
                                 const ir_value_t *operand)
{
    th_type *type = operand && operand->type ? th_resolve(operand->type)
                                             : nullptr;

    for (size_t i = 0; type && i < evidence->len; i++) {
        integer_evidence_t *entry = &evidence->data[i];

        if (entry->var != type) {
            continue;
        }

        ir_abcd_range_t range = unknown_integer();

        entry->bitwise = true;
        if (!entry->negative_at && ir_abcd_range_of(abcd, operand, in, &range)
            && !range.unbounded_min && range.seen.lo < 0) {
            const ir_instr_t *def    = defining(operand);
            bool              inside = def && !def->span.implied
                && def->span.start_byte >= in->span.start_byte
                && def->span.end_byte <= in->span.end_byte
                && def->span.end_byte > def->span.start_byte
                && (def->span.start_byte != in->span.start_byte
                    || def->span.end_byte != in->span.end_byte);

            entry->negative_at = inside ? def : in;
            entry->negative    = range.seen.lo;
        }
        return;
    }
}

static void note_bitwise_operands(core_list_t(integer_evidence_t) *evidence,
                                  ir_abcd_t *abcd, const ir_instr_t *in)
{
    const n00bb_binary_operator_t *operator =
        in->op == IR_OP_DISPATCH && in->n_operands == 3
                && in->operands[0].kind == IR_OPD_NAME
            ? n00bb_binary_operator_method(in->operands[0].name)
            : nullptr;

    if (in->op == IR_OP_NOT && in->n_operands == 1) {
        note_bitwise_operand(evidence, abcd, in, ir_value_operand(in, 0));
    }
    if (operator && operator->unsigned_only) {
        note_bitwise_operand(evidence, abcd, in, ir_value_operand(in, 1));
        if (!operator->shift) {
            note_bitwise_operand(evidence, abcd, in, ir_value_operand(in, 2));
        }
    }
}

static bool report_bitwise_negative(core_translation_unit_t *const *units,
                                    size_t count,
                                    const integer_evidence_t *entry)
{
    core_translation_unit_t *unit = unit_of(units, count, entry->negative_at);

    if (!unit) {
        return false;
    }

    core_buffer_t shown = {0};

    if (entry->negative < 0) {
        core_buffer_puts(&shown, "-");
    }
    ir_uint_append(&shown, entry->negative < 0 ? (ir_uint_t)-entry->negative
                                               : (ir_uint_t)entry->negative);
    if (n00bb_report(core_translation_unit_diagnostics(unit), unit,
                     N00B_DIAG_BITWISE_NEGATIVE,
                     n00bb_span_location(entry->negative_at->span),
                     N00B_TEXT_BITWISE_VALUE_CAN_BE_NEGATIVE,
                     N00BB_ARGS(ARG_TEXT(core_buffer_string(&shown))))) {
        n00bb_report_help(core_translation_unit_diagnostics(unit),
                          N00B_TEXT_KEEP_BITWISE_VALUES_UNSIGNED,
                          N00BB_NO_ARGS);
    }
    core_buffer_release(&shown);

    return true;
}

static size_t met_root(size_t *meets, size_t at)
{
    while (meets[at] != at) {
        meets[at] = meets[meets[at]];
        at        = meets[at];
    }
    return at;
}

static size_t evidence_index(core_list_t(integer_evidence_t) *evidence,
                             th_type *type)
{
    type = type ? th_resolve(type) : nullptr;
    for (size_t i = 0; type && i < evidence->len; i++) {
        if (evidence->data[i].var == type) {
            return i;
        }
    }
    return SIZE_MAX;
}

// Two open integer types in the same position of operands that must have
// one type, as `Set[A]` and `Set[B]` in `s + t`, meet: sized apart they
// could differ (u8 and u16), and the call would not type. Typing resolves
// such a call after sizing, so sizing joins them here.
static void note_meeting_types(core_list_t(integer_evidence_t) *evidence,
                               size_t *meets, const ir_instr_t *in)
{
    const ir_value_t *left  = in->op == IR_OP_DISPATCH && in->n_operands == 3
                                  ? ir_value_operand(in, 1)
                                  : nullptr;
    const ir_value_t *right = left ? ir_value_operand(in, 2) : nullptr;
    th_type *a = left && left->type ? th_resolve(left->type) : nullptr;
    th_type *b = right && right->type ? th_resolve(right->type) : nullptr;
    core_string_t a_name = {0}, b_name = {0};
    size_t        a_args = 0, b_args = 0;

    if (!a || !b || !th_named_info(a, &a_name, nullptr, &a_args)
        || !th_named_info(b, &b_name, nullptr, &b_args)
        || a_args != b_args || !core_string_eq(a_name, b_name)) {
        return;
    }
    for (size_t i = 0; i < a_args; i++) {
        const th_arg *x = th_named_arg(a, i);
        const th_arg *y = th_named_arg(b, i);
        size_t        p = x && x->kind == TH_ARG_TYPE
                              ? evidence_index(evidence, x->type)
                              : SIZE_MAX;
        size_t        q = y && y->kind == TH_ARG_TYPE
                              ? evidence_index(evidence, y->type)
                              : SIZE_MAX;
        if (p != SIZE_MAX && q != SIZE_MAX) {
            meets[met_root(meets, p)] = met_root(meets, q);
        }
    }
}

// An inferred integer that is an operand of a built-in integer operator
// alongside a declared integer type is widened to hold that type too, and
// again for each such type it meets. A literal constant is not: it stays
// the smallest type holding its value, and the operation promotes.
static void note_declared_meeting(core_list_t(integer_evidence_t) *evidence,
                                  th_ctx *types, const ir_instr_t *in)
{
    if (in->op != IR_OP_DISPATCH || in->n_operands != 3
        || in->operands[0].kind != IR_OPD_NAME) {
        return;
    }

    core_string_t                  method = in->operands[0].name;
    const n00bb_binary_operator_t *binary = n00bb_binary_operator_method(
        method);

    if ((!binary || binary->op == IR_OP_SHL || binary->op == IR_OP_SHR)
        && !core_string_eq(method, CORE_STRING_STATIC("__equal"))
        && !core_string_eq(method, CORE_STRING_STATIC("__cmp"))) {
        return;
    }
    for (int side = 1; side <= 2; side++) {
        const ir_value_t *open   = ir_value_operand(in, side);
        const ir_value_t *other  = ir_value_operand(in, 3 - side);
        size_t            at     = open ? evidence_index(evidence, open->type)
                                        : SIZE_MAX;
        ir_interval_t     bounds = {0};

        if (at == SIZE_MAX || !other || !other->type
            || (open->def && open->def->op == IR_OP_CONST)
            || !promotable_integer(types, other->type, &bounds)) {
            continue;
        }

        integer_evidence_t *entry = &evidence->data[at];

        if (!entry->seen) {
            entry->range = (ir_abcd_range_t){.seen = bounds};
            entry->seen  = true;
            continue;
        }
        // A side with no fixpoint stays unbounded (and so 64-bit); only a
        // bounded side widens to hold the declared type.
        if (!entry->range.unbounded_min) {
            entry->range.seen.lo = ir_int_lesser(entry->range.seen.lo,
                                                 bounds.lo);
        }
        if (!entry->range.unbounded_max) {
            entry->range.seen.hi = ir_int_greater(entry->range.seen.hi,
                                                  bounds.hi);
        }
    }
}

// Every open type gets the joined range of the types it meets.
static void join_met_evidence(core_list_t(integer_evidence_t) *evidence,
                              size_t *meets)
{
    for (size_t i = 0; i < evidence->len; i++) {
        size_t root = met_root(meets, i);
        if (root == i || !evidence->data[i].seen) {
            continue;
        }
        integer_evidence_t *into = &evidence->data[root];
        ir_abcd_range_t     from = evidence->data[i].range;
        if (!into->seen) {
            into->range = from;
            into->seen  = true;
            continue;
        }
        ir_abcd_range_t *range = &into->range;
        range->unbounded_min = from.seen.lo < range->seen.lo
                                 ? from.unbounded_min
                             : from.seen.lo == range->seen.lo
                                 ? range->unbounded_min || from.unbounded_min
                                 : range->unbounded_min;
        range->unbounded_max = from.seen.hi > range->seen.hi
                                 ? from.unbounded_max
                             : from.seen.hi == range->seen.hi
                                 ? range->unbounded_max || from.unbounded_max
                                 : range->unbounded_max;
        range->seen = ir_interval_join(range->seen, from.seen);
    }
    for (size_t i = 0; i < evidence->len; i++) {
        size_t root = met_root(meets, i);
        if (root != i && evidence->data[root].seen) {
            evidence->data[i].range = evidence->data[root].range;
            evidence->data[i].seen  = true;
        }
    }
}

static bool materialize_integers(ty_solver_t *solver, th_ctx *types,
                                 const n00bb_dispatch_index_t *index,
                                 core_list_t(n00bb_ir_func_ptr_t) *functions,
                                 core_translation_unit_t *const *units,
                                 size_t unit_count, bool *reported)
{
    size_t count = ty_open_count(solver, CORE_STRING_STATIC("Some_Integer"));

    if (!count) {
        return false;
    }

    core_list_t(integer_evidence_t) evidence =
        core_list_new(integer_evidence_t);

    integer_view_t view = {0};

    core_dict_init(&view.open, nullptr, nullptr);
    for (size_t i = 0; i < count; i++) {
        core_list_push(evidence, ((integer_evidence_t){
                                     .var = ty_open_at(solver, i)}));
        core_dict_put(&view.open, ty_open_at(solver, i),
                      ty_open_at(solver, i));
    }
    // Union-find over the entries: open types that meet are sized together.
    size_t *meets = core_alloc_array(size_t, count);
    for (size_t i = 0; i < count; i++) meets[i] = i;

    // Sizing literals, not proving checks (ir_abcd_hooks_t.literal_sizing).
    ir_abcd_hooks_t hooks = {.context = &view, .view = integer_view,
                             .literal_sizing = true};

    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];

        view.module = function->owner;

        ir_abcd_t *abcd = ir_abcd_build_with(function, &hooks);

        for (uint32_t p = 0; p < function->n_params; p++) {
            note_integer_value(&evidence, abcd, function->params[p]);
        }
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                note_integer_value(&evidence, abcd,
                                   function->blocks[b]->instrs[i]->dest);
                note_bitwise_operands(&evidence, abcd,
                                      function->blocks[b]->instrs[i]);
            }
        }
        ir_abcd_free(abcd);
        for (uint32_t b = 0; b < function->n_blocks; b++) {
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                note_overload_evidence(&evidence, index, function->owner,
                                       function->blocks[b]->instrs[i]);
                note_meeting_types(&evidence, meets,
                                   function->blocks[b]->instrs[i]);
                note_declared_meeting(&evidence, types,
                                      function->blocks[b]->instrs[i]);
            }
        }
    }
    core_dict_free(&view.open);
    join_met_evidence(&evidence, meets);
    core_free(meets);
    for (size_t i = 0; i < evidence.len; i++) {
        integer_evidence_t *entry = &evidence.data[i];
        ir_abcd_range_t     range = entry->seen ? entry->range
                                                : unknown_integer();

        if (entry->negative_at) {
            *reported = report_bitwise_negative(units, unit_count, entry)
                     || *reported;
        }
        else if (entry->bitwise
                 && (range.unbounded_min || range.seen.lo < 0)) {
            range.seen.lo       = 0;
            range.unbounded_min = false;
            if (range.seen.hi < 0) {
                range.seen.hi = 0;
            }
        }
        bool fits = entry->overload && !entry->overloads_differ
            && range.seen.lo >= entry->overload_bounds.lo
            && range.seen.hi <= entry->overload_bounds.hi;
        th_type *chosen = chosen_integer(entry, range);

        ty_settle_open(solver, CORE_STRING_STATIC("Some_Integer"), entry->var,
                       chosen ? chosen
                       : fits ? entry->overload
                              : best_integer_fit(types, range));
    }
    core_list_free(evidence);

    return true;
}

void n00bb_typing_free(n00bb_typing_t *typing)
{
    dispatch_index_free(typing->dispatch);
    ir_group_pdg_free(typing->group);
    core_list_free(typing->conversions);
    core_list_free(typing->outcome_calls);
    *typing = (n00bb_typing_t){0};
}

static bool constant_fits(th_type *type, ir_lit_t literal)
{
    ir_interval_t range;

    if (!n00bb_type_interval(type, &range)) {
        return true;
    }
    if (!literal.neg && literal.mag > (ir_uint_t)ir_int_max) {
        return range.lo == 0 && range.hi == ir_int_max;
    }

    return ir_interval_contains(range,
                                ir_interval_point(ir_lit_signed(literal)));
}

typedef struct {
    ir_instr_t   *test;
    core_string_t name;
    uint64_t      switch_id;
    uint64_t      case_index;
    uint64_t      patterns;
} case_test_t;
core_list_decl(case_test_t);

static bool annot_uint(ir_instr_t *in, core_string_t key, uint64_t *out)
{
    ir_annot_t *annot = ir_annot_get(&in->annots, key);

    if (!annot || annot->kind != IR_ANNOT_INT) {
        return false;
    }
    *out = (uint64_t)annot->i.mag;
    return true;
}

static core_string_t case_name(ir_instr_t *in)
{
    ir_annot_t *annot = ir_annot_get(&in->annots, N00BB_CASE_KEY);
    ir_value_t *value = in->n_operands == 3
                          ? ir_operand_value(&in->operands[2])
                          : nullptr;
    ir_instr_t *def   = value ? value->def : nullptr;

    if (annot && annot->kind == IR_ANNOT_STR) {
        return annot->s;
    }

    return def && def->op == IR_OP_ENUM_CONST && def->n_operands
                && def->operands[0].kind == IR_OPD_NAME
             ? def->operands[0].name
             : (core_string_t){0};
}

static ir_instr_t *unmatched_jump(ir_func_t *f, uint64_t id)
{
    for (uint32_t b = 0; b < f->n_blocks; b++) {
        ir_instr_t *last = ir_block_terminator(f->blocks[b]);
        uint64_t    at   = 0;

        if (last && last->op == IR_OP_JUMP
            && annot_uint(last, N00BB_SWITCH_KEY, &at) && at == id) {
            return last;
        }
    }

    return nullptr;
}

// The constant a scalar `case` compares against, when it is one.
static const ir_value_t *case_constant(const case_test_t *test, ir_lit_t *out)
{
    const ir_instr_t *in    = test->test;
    const ir_value_t *value = in->n_operands == 3
                                ? ir_operand_value(&in->operands[2])
                                : nullptr;
    const ir_instr_t *def   = value ? value->def : nullptr;

    if (!def || def->op != IR_OP_CONST || !def->n_operands
        || def->operands[0].kind != IR_OPD_CONST
        || def->operands[0].lit.is_float || def->operands[0].lit.text.data
        || !core_string_eq(in->operands[0].name,
                           CORE_STRING_STATIC("__equal"))) {
        return nullptr;
    }
    *out = def->operands[0].lit.i;
    return value;
}

// Two `case`s of a scalar `switch` that match the same value: the first
// always wins, so the second is dead and almost certainly a mistake.
static bool switch_constants_distinct(core_translation_unit_t *unit,
                                      const case_test_t       *tests,
                                      size_t                   count)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    bool                ok          = true;

    for (size_t i = 0; i < count; i++) {
        ir_lit_t          value = {0};
        const ir_value_t *found = case_constant(&tests[i], &value);

        for (size_t j = 0; found && j < i; j++) {
            ir_lit_t earlier = {0};

            if (tests[j].case_index == tests[i].case_index
                || !case_constant(&tests[j], &earlier)
                || earlier.mag != value.mag
                || (earlier.neg != value.neg && value.mag)) {
                continue;
            }

            ir_span_t span = tests[i].test->span;

            n00bb_report(diagnostics, unit, N00B_DIAG_DUPLICATE_SWITCH_CASE,
                         n00bb_span_location(span),
                         N00B_TEXT_VALUE_ALREADY_HAS_A_CASE,
                         N00BB_ARGS(ARG_TEXT(n00bb_value_text(unit, found,
                                                              span))));
            n00bb_report_secondary(diagnostics, unit,
                                   n00bb_span_location(tests[j].test->span),
                                   N00B_TEXT_FIRST_HANDLED_HERE,
                                   N00BB_NO_ARGS);
            ok = false;
            break;
        }
    }

    return ok;
}

static bool switch_fits(const n00bb_dispatch_index_t *index,
                        core_translation_unit_t *unit, ir_func_t *f,
                        const case_test_t *tests, size_t count)
{
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    ir_value_t        *subject = ir_operand_value(&tests[0].test->operands[1]);
    bool               variant = false;
    parse_sym_entry_t *owner   = subject ? n00bb_case_owner(index,
                                                            subject->type,
                                                            &variant)
                                         : nullptr;
    ir_annot_t        *named   = ir_annot_get(&tests[0].test->annots,
                                              N00BB_SWITCH_VARIABLE_KEY);
    bool               ok      = true;
    th_ctx            *types   = ir_module_types(f->owner);
    th_type           *kind    = subject ? th_resolve(subject->type) : nullptr;

    if (!owner) {
        if (!kind || th_kind_of(kind) == TH_VAR || is_integer_type(kind)
            || n00bb_scalar_role_is(kind, IR_ROLE_CHAR)
            || th_hash(types, kind)
                   == th_hash(types, th_name(types, CORE_STRING_STATIC("bool")))) {
            return switch_constants_distinct(unit, tests, count);
        }
        n00bb_report(diagnostics, unit, N00B_DIAG_SWITCH_SUBJECT_UNSUPPORTED,
                     n00bb_span_location(subject->def ? subject->def->span
                                                      : tests[0].test->span),
                     N00B_TEXT_A_SWITCH_CANNOT_MATCH,
                     N00BB_ARGS(ARG_TEXT(n00bb_print_type(types, kind))));
        n00bb_report_help(diagnostics, N00B_TEXT_COMPARE_THEM_WITH_IF,
                          N00BB_NO_ARGS);
        return false;
    }
    if (variant && named && named->kind == IR_ANNOT_BOOL && !named->b) {
        n00bb_report(diagnostics, unit, N00B_DIAG_SUBJECT_NOT_VARIABLE,
                     n00bb_span_location(tests[0].test->span),
                     N00B_TEXT_A_SWITCH_ON_A_VARIANT, N00BB_NO_ARGS);
        n00bb_report_help(diagnostics, N00B_TEXT_STORE_THE_VALUE_IN_A_2,
                          N00BB_NO_ARGS);
        ok = false;
    }
    for (size_t i = 0; i < count; i++) {
        const case_test_t *test = &tests[i];
        ir_span_t          span = test->test->span;

        if (variant && test->patterns != 1
            && (!i || tests[i - 1].case_index != test->case_index)) {
            n00bb_report(diagnostics, unit, N00B_DIAG_VARIANT_CASE_NOT_ARM,
                         n00bb_span_location(span),
                         N00B_TEXT_EACH_CASE_OF_A_SWITCH,
                         N00BB_ARGS(ARG_NUMBER(test->patterns)));
            n00bb_report_help(diagnostics, N00B_TEXT_GIVE_EACH_ARM_ITS_OWN,
                              N00BB_NO_ARGS);
            ok = false;
        }
        if (!test->name.data) {
            if (variant) {
                n00bb_report(diagnostics, unit, N00B_DIAG_VARIANT_CASE_NOT_ARM,
                             n00bb_span_location(span),
                             N00B_TEXT_EACH_CASE_OF_A_SWITCH_2, N00BB_NO_ARGS);
                ok = false;
            }
            continue;
        }
        if (!n00bb_case_member(index, subject->type, test->name, nullptr)) {
            if (variant) {
                n00bb_report(diagnostics, unit, N00B_DIAG_VARIANT_CASE_NOT_ARM,
                             n00bb_span_location(span),
                             N00B_TEXT_EACH_CASE_OF_A_SWITCH_2, N00BB_NO_ARGS);
            }
            else {
                n00bb_report(diagnostics, unit, N00B_DIAG_UNRESOLVED_NAME,
                             n00bb_span_location(span), N00B_TEXT_CANNOT_FIND,
                             N00BB_ARGS(ARG_TEXT(test->name)));
                n00bb_report_label(diagnostics, N00B_TEXT_NOT_FOUND_IN,
                                   N00BB_ARGS(ARG_TEXT(owner->name)));
            }
            ok = false;
            continue;
        }
        for (size_t j = 0; j < i; j++) {
            if (core_string_eq(tests[j].name, test->name)) {
                n00bb_report(diagnostics, unit, N00B_DIAG_DUPLICATE_SWITCH_CASE,
                             n00bb_span_location(span),
                             N00B_TEXT_ARM_ALREADY_HAS_A_CASE,
                             N00BB_ARGS(ARG_TEXT(test->name)));
                n00bb_report_secondary(diagnostics, unit,
                                       n00bb_span_location(tests[j].test->span),
                                       N00B_TEXT_FIRST_HANDLED_HERE,
                                       N00BB_NO_ARGS);
                ok = false;
                break;
            }
        }
    }

    ir_instr_t *jump = unmatched_jump(f, tests[0].switch_id);

    if (!ok || !jump) {
        return ok;
    }

    core_list_t(core_string_t) missing = core_list_new(core_string_t);
    core_buffer_t              listed  = {0};

    for (parse_sym_entry_t *member = owner->member_scope->first_in_scope;
         member; member = member->next_in_scope) {
        bool covered = !n00bb_case_member(index, subject->type, member->name,
                                          nullptr);

        for (size_t i = 0; !covered && i < count; i++) {
            covered = core_string_eq(tests[i].name, member->name);
        }
        if (covered) {
            continue;
        }
        core_list_push(missing, member->name);
        core_buffer_printf(&listed, "%s`%.*s.%.*s`", listed.byte_len ? ", " : "",
                           (int)owner->name.u8_bytes, owner->name.data,
                           (int)member->name.u8_bytes, member->name.data);
    }
    // An open enum holds values none of its items name, so naming every
    // item never covers the subject.
    const n00bb_enum_range_t *enum_range = variant ? nullptr
        : parse_sym_binding(owner, CORE_STRING_STATIC("n00bb.enum-range"));
    bool open_enum = enum_range && enum_range->is_open;
    if (open_enum)
        core_buffer_printf(&listed, "%sother values of `%.*s`",
                           listed.byte_len ? ", " : "",
                           (int)owner->name.u8_bytes, owner->name.data);

    ir_annot_t *expression = ir_annot_get(&jump->annots,
                                          N00BB_SWITCH_EXPRESSION_KEY);
    ir_annot_t *subject_text = ir_annot_get(&jump->annots,
                                            N00BB_SWITCH_SUBJECT_KEY);

    if (!missing.len && !open_enum) {
        ir_block_t *block = jump->block;
        ir_span_t   span  = jump->span;
        ir_instr_t *never = ir_instr_erase(jump)
                              ? ir_instr_add(block, IR_OP_UNREACHABLE)
                              : nullptr;

        ok = never != nullptr;
        if (never) {
            never->span = span;
        }
    }
    else if (variant || (missing.len && expression && expression->b)) {
        core_string_t text = subject_text && subject_text->kind == IR_ANNOT_STR
                               ? subject_text->s
                               : (core_string_t){0};

        n00bb_report(diagnostics, unit, N00B_DIAG_SWITCH_NOT_EXHAUSTIVE,
                     n00bb_span_location(jump->span),
                     text.u8_bytes ? N00B_TEXT_SWITCH_HAS_NO_CASE_FOR
                                   : N00B_TEXT_SWITCH_SUBJECT_HAS_NO_CASE_FOR,
                     N00BB_ARGS(ARG_NAMES(missing.data, missing.len),
                                ARG_TEXT(text)));
        n00bb_report_help(diagnostics, N00B_TEXT_ADD_A_CASE_FOR_EACH_2,
                          N00BB_NO_ARGS);
        ok = false;
    }
    else {
        ok = ir_annot_set(ir_module_ctx(f->owner), &jump->annots,
                          (ir_annot_t){
                              .key  = N00BB_SWITCH_MISSING_KEY,
                              .kind = IR_ANNOT_STR,
                              .s    = ir_strdup(ir_module_ctx(f->owner),
                                                core_buffer_string(&listed)),
                          });
    }
    core_buffer_release(&listed);
    core_list_free(missing);

    return ok;
}

static bool switches_fit(const n00bb_dispatch_index_t *index,
                         core_translation_unit_t *const *units, size_t count,
                         const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    bool ok = true;

    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t                 *function = functions->data[f];
        core_translation_unit_t   *unit     = nullptr;
        core_list_t(case_test_t)   tests    = core_list_new(case_test_t);

        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];

            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *in   = block->instrs[i];
                case_test_t test = {.test = in};

                if (in->op == IR_OP_DISPATCH && in->n_operands >= 2
                    && annot_uint(in, N00BB_SWITCH_KEY, &test.switch_id)
                    && annot_uint(in, N00BB_CASE_INDEX_KEY, &test.case_index)
                    && annot_uint(in, N00BB_CASE_PATTERNS_KEY,
                                  &test.patterns)) {
                    test.name = case_name(in);
                    unit      = unit ? unit : unit_of(units, count, in);
                    core_list_push(tests, test);
                }
            }
        }
        core_list_t(case_test_t) group = core_list_new(case_test_t);

        for (size_t start = 0; unit && start < tests.len; start++) {
            uint64_t id    = tests.data[start].switch_id;
            bool     first = true;

            for (size_t j = 0; first && j < start; j++) {
                first = tests.data[j].switch_id != id;
            }
            if (!first) {
                continue;
            }
            group.len = 0;
            for (size_t j = start; j < tests.len; j++) {
                if (tests.data[j].switch_id == id) {
                    core_list_push(group, tests.data[j]);
                }
            }
            ok = switch_fits(index, unit, function, group.data, group.len)
              && ok;
        }
        core_list_free(group);
        core_list_free(tests);
    }

    return ok;
}

static bool finalizers_fit(th_ctx *types, core_translation_unit_t *const *units,
                           size_t count,
                           const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    bool ok = true;

    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function  = functions->data[f];
        th_type   *signature = th_resolve(function->type);

        if (!core_string_eq(function->name, CORE_STRING_STATIC("__finalize"))
            || th_kind_of(signature) != TH_FUNCTION
            || !function->n_blocks) {
            continue;
        }

        core_translation_unit_t *unit = unit_of(units, count,
                                                function->blocks[0]->n_instrs
                                                    ? function->blocks[0]
                                                          ->instrs[0]
                                                    : nullptr);
        core_diagnostics_t *diagnostics =
            unit ? core_translation_unit_diagnostics(unit) : nullptr;
        core_diag_location_t at = unit ? n00bb_name_location(unit,
                                                             function->span,
                                                             function->name)
                                       : (core_diag_location_t){0};
        th_type *failure = th_func_error(signature)
                             ? th_resolve(th_func_error(signature))
                             : nullptr;
        th_type *result  = th_resolve(th_func_ret(signature));

        if (failure && !th_is_nil(failure)) {
            ok = false;
            if (unit) {
                n00bb_report(diagnostics, unit, N00B_DIAG_FINALIZE_CANNOT_FAIL,
                             at, N00B_TEXT_FINALIZE_CANNOT_FAIL,
                             N00BB_NO_ARGS);
            }
        }
        if (result && !th_is_nil(result)) {
            ok = false;
            if (unit) {
                n00bb_report(diagnostics, unit,
                             N00B_DIAG_FINALIZE_RETURNS_VALUE, at,
                             N00B_TEXT_FINALIZE_RETURNS_BUT_IT_MUST,
                             N00BB_ARGS(ARG_TEXT(
                                 n00bb_print_type(types, result))));
            }
        }
    }

    return ok;
}

static bool constants_fit(th_ctx *types, core_translation_unit_t *const *units,
                          size_t count,
                          const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    bool ok = true;

    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];

        for (uint32_t b = 0; b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];

            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *in = block->instrs[i];

                if (in->op != IR_OP_CONST || !in->dest || !in->n_operands
                    || in->operands[0].kind != IR_OPD_CONST
                    || in->operands[0].lit.is_float
                    || in->operands[0].lit.text.data
                    || constant_fits(th_resolve(in->dest->type),
                                     in->operands[0].lit.i)) {
                    continue;
                }

                core_translation_unit_t *unit = unit_of(units, count, in);
                core_diagnostics_t *diagnostics =
                    unit ? core_translation_unit_diagnostics(unit) : nullptr;
                core_buffer_t shown = {0};
                core_string_t name  = th_print(types, in->dest->type);

                ok = false;
                if (!unit) {
                    continue;
                }
                if (in->operands[0].lit.i.neg) {
                    core_buffer_puts(&shown, "-");
                }
                ir_uint_append(&shown, in->operands[0].lit.i.mag);
                core_string_t written = span_text(unit, in->span);
                char          first   = written.u8_bytes ? written.data[0]
                                                         : '0';
                bool          folded  = !(first == '_'
                                          || (first >= 'a' && first <= 'z')
                                          || (first >= 'A' && first <= 'Z'));
                if (n00bb_report(diagnostics, unit,
                                 N00B_DIAG_INTEGER_CONSTANT_RANGE,
                                 n00bb_span_location(in->span),
                                 folded ? N00B_TEXT_CONSTANT_DOES_NOT_FIT
                                        : N00B_TEXT_VALUE_DOES_NOT_FIT,
                                 N00BB_ARGS(ARG_TEXT(core_buffer_string(&shown)),
                                            ARG_TEXT(name)))) {
                    n00bb_report_help(diagnostics,
                                      N00B_TEXT_USE_A_WIDER_INTEGER_TYPE,
                                      N00BB_ARGS(ARG_TEXT(name)));
                }
                core_buffer_release(&shown);
            }
        }
    }

    return ok;
}

static const ir_value_t *arm_test(const ir_block_t *block, uint64_t *tag,
                                  uint32_t *slot)
{
    const ir_instr_t *branch = ir_block_terminator((ir_block_t *)block);
    const ir_value_t *condition = branch && branch->op == IR_OP_BRANCH
            && branch->n_operands == 3
        ? ir_operand_value(&branch->operands[0]) : nullptr;
    const ir_instr_t *compare = condition ? condition->def : nullptr;

    if (!compare || (compare->op != IR_OP_EQ && compare->op != IR_OP_NE)
        || compare->n_operands != 2) {
        return nullptr;
    }
    for (uint32_t side = 0; side < 2; side++) {
        const ir_value_t *tested = ir_operand_value(&compare->operands[side]);
        const ir_value_t *known  =
            ir_operand_value(&compare->operands[1 - side]);
        const ir_instr_t *read   = tested ? tested->def : nullptr;
        const ir_instr_t *value  = known ? known->def : nullptr;

        if (!read || read->op != IR_OP_VARIANT_TAG || read->n_operands != 1
            || !value || value->op != IR_OP_CONST || value->n_operands != 1
            || value->operands[0].kind != IR_OPD_CONST
            || value->operands[0].lit.is_float) {
            continue;
        }
        *tag  = (uint64_t)value->operands[0].lit.i.mag;
        *slot = compare->op == IR_OP_EQ ? 1 : 2;
        return ir_operand_value(&read->operands[0]);
    }

    return nullptr;
}

static uint64_t other_arm(th_ctx *types, th_type *variant, uint64_t tag);

static bool arm_guarded(ir_func_t *function, ir_doms_t *doms,
                        ir_value_numbers_t *numbers, const ir_instr_t *read,
                        const ir_value_t *subject, uint64_t tag,
                        uint64_t *other)
{
    for (ir_block_t *at = read->block; at;) {
        ir_block_t *parent = ir_idom_of(doms, at);

        if (!parent || parent == at) {
            break;
        }

        uint64_t          tested = 0;
        uint32_t          slot   = 0;
        const ir_value_t *value  = arm_test(parent, &tested, &slot);
        ir_instr_t       *branch = value ? ir_block_terminator(parent)
                                         : nullptr;

        for (uint32_t side_slot = 1; branch && side_slot <= 2; side_slot++) {
            ir_block_t *side = ir_succ_of(&branch->operands[side_slot]);
            uint64_t    held = side_slot == slot
                ? tested
                : other_arm(ir_module_types(function->owner), subject->type,
                            tested);

            if (!held || !side || ir_pred_count(doms, side) != 1
                || !ir_dominates(doms, side, read->block)
                || !ir_value_numbers_same(numbers, value, subject)) {
                continue;
            }
            if (held == tag) {
                return true;
            }
            if (!*other) {
                *other = held;
            }
        }
        at = parent;
    }

    return false;
}

static th_type *variant_arms(th_ctx *types, th_type *variant)
{
    variant = variant ? th_resolve(variant) : nullptr;
    while (variant && th_kind_of(variant) == TH_INDIRECTION) {
        variant = th_resolve(th_ind_referent(variant));
    }
    if (variant && th_kind_of(variant) == TH_NAMED) {
        variant = th_type_definition(types, variant);
        variant = variant ? th_resolve(variant) : nullptr;
    }

    return variant && th_kind_of(variant) == TH_RECORD ? variant : nullptr;
}

static core_string_t arm_named(th_ctx *types, th_type *variant, uint64_t tag)
{
    th_type *arms = variant_arms(types, variant);

    for (size_t i = 0; arms && i < th_record_len(arms); i++) {
        const th_member *member = th_record_member(arms, i);

        if (member && th_arm_tag(member->name) == tag) {
            return member->name;
        }
    }

    return (core_string_t){0};
}

static uint64_t other_arm(th_ctx *types, th_type *variant, uint64_t tag)
{
    th_type *arms = variant_arms(types, variant);

    if (!arms || th_record_len(arms) != 2) {
        return 0;
    }
    for (size_t i = 0; i < 2; i++) {
        const th_member *member = th_record_member(arms, i);
        uint64_t         arm    = member ? th_arm_tag(member->name) : 0;

        if (arm && arm != tag) {
            return arm;
        }
    }

    return 0;
}

static bool user_variant(th_type *type)
{
    core_string_t name = {0};

    type = type ? th_resolve(type) : nullptr;
    while (type && th_kind_of(type) == TH_INDIRECTION) {
        type = th_resolve(th_ind_referent(type));
    }

    bool variant = false;

    return type && th_named_info(type, &name, &variant, nullptr) && variant
        && !core_string_starts_with(n00bb_name_leaf(name),
                                    CORE_STRING_STATIC("__"));
}

static bool variant_arms_checked(core_translation_unit_t *const *units,
                                 size_t count,
                                 const core_list_t(n00bb_ir_func_ptr_t)
                                     *functions)
{
    bool ok = true;

    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];
        ir_doms_t          *doms    = nullptr;
        ir_value_numbers_t *numbers = nullptr;

        for (uint32_t b = 0; b < function->n_blocks; b++) {
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in = function->blocks[b]->instrs[i];

                const ir_value_t *checked = in->op == IR_OP_VARIANT_PAYLOAD
                        && in->n_operands == 2
                        && in->operands[1].kind == IR_OPD_NAME
                        && ir_span_known(in->span) && !in->span.implied
                    ? ir_operand_value(&in->operands[0]) : nullptr;

                if (!checked || !user_variant(checked->type)) {
                    continue;
                }
                if (!numbers) {
                    numbers = ir_func_value_numbers(function);
                    doms    = ir_func_doms(function);
                }

                const ir_value_t *subject = ir_operand_value(&in->operands[0]);
                core_string_t     arm     = in->operands[1].name;
                uint64_t          other   = 0;
                bool              guarded = !doms || !numbers || !subject
                    || !ir_block_reachable(doms, in->block)
                    || arm_guarded(function, doms, numbers, in, subject,
                                   th_arm_tag(arm), &other);

                if (guarded) {
                    continue;
                }
                ok = false;

                core_translation_unit_t *unit = unit_of(units, count, in);

                if (!unit) {
                    continue;
                }

                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);
                core_string_t value = n00bb_value_text(unit, subject,
                                                       in->span);
                core_string_t held  = other
                    ? arm_named(ir_module_types(function->owner),
                                subject->type, other)
                    : (core_string_t){0};

                if (!value.u8_bytes) {
                    value = CORE_STRING_STATIC("the variant");
                }
                bool reported = held.u8_bytes
                    ? n00bb_report(diagnostics, unit,
                          N00B_DIAG_VARIANT_ARM_UNCHECKED,
                          n00bb_span_location(in->span),
                          N00B_TEXT_VARIANT_ARM_OTHER,
                          N00BB_ARGS(ARG_TEXT(value), ARG_TEXT(arm),
                                     ARG_TEXT(held)))
                    : n00bb_report(diagnostics, unit,
                          N00B_DIAG_VARIANT_ARM_UNCHECKED,
                          n00bb_span_location(in->span),
                          N00B_TEXT_VARIANT_ARM_UNCHECKED,
                          N00BB_ARGS(ARG_TEXT(value), ARG_TEXT(arm)));

                if (reported) {
                    n00bb_report_help(diagnostics,
                                      N00B_TEXT_READ_THE_ARM_IN_A_CASE,
                                      N00BB_ARGS(ARG_TEXT(value),
                                                 ARG_TEXT(arm)));
                }
            }
        }
    }

    return ok;
}

bool n00bb_check_variant_arms(n00bb_link_analysis_t *analysis)
{
    core_list_t(n00bb_ir_func_ptr_t) functions =
        core_list_new(n00bb_ir_func_ptr_t);

    for (size_t u = 0; u < analysis->units.len; u++) {
        n00bb_tu_state_t *state  = tu_state(analysis->units.data[u]);
        ir_module_t      *module = state && !state->interface_module
                                     ? state->ir_module
                                     : nullptr;

        for (size_t f = 0; module && f < ir_func_count(module); f++) {
            ir_func_t *function = ir_func_at(module, f);

            if (function->n_blocks) {
                core_list_push(functions, function);
            }
        }
    }

    bool ok = variant_arms_checked(analysis->units.data, analysis->units.len,
                                   &functions);

    core_list_free(functions);

    return ok;
}

static bool patterns_fit(th_ctx *types, core_translation_unit_t *const *units,
                         size_t count,
                         const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    bool        ok = true;
    core_dict_t reported;

    core_dict_init(&reported, nullptr, nullptr);
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];

        for (uint32_t b = 0; b < function->n_blocks; b++) {
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                const ir_instr_t *in    = function->blocks[b]->instrs[i];
                const ir_annot_t *arity = in->op == IR_OP_EXTRACT_INDEX
                        && in->n_operands
                    ? ir_annot_get((ir_annots_t *)&in->annots,
                                   N00BB_PATTERN_ARITY_KEY)
                    : nullptr;
                const ir_value_t *unpacked = arity
                    ? ir_operand_value(&in->operands[0]) : nullptr;
                th_type *subject  = unpacked ? th_resolve(unpacked->type)
                                             : nullptr;
                size_t   elements = 0;

                if (!subject || arity->kind != IR_ANNOT_INT
                    || !th_tuple_element(subject, 0, &elements)
                    || elements <= arity->i.mag
                    || !core_dict_add(&reported, (void *)unpacked, 1)) {
                    continue;
                }
                ok = false;

                core_translation_unit_t *unit = unit_of(units, count, in);
                core_diagnostics_t *diagnostics =
                    unit ? core_translation_unit_diagnostics(unit) : nullptr;

                if (diagnostics
                    && n00bb_report(diagnostics, unit,
                           N00B_DIAG_LOOP_BINDINGS_MISMATCH,
                           n00bb_span_location(in->span),
                           N00B_TEXT_PATTERN_ARITY,
                           N00BB_ARGS(ARG_NUMBER((uint64_t)arity->i.mag),
                                      ARG_TEXT(n00bb_print_type(types,
                                                                subject)),
                                      ARG_NUMBER(elements)))) {
                    n00bb_report_help(diagnostics,
                                      N00B_TEXT_NAME_EVERY_ELEMENT,
                                      N00BB_NO_ARGS);
                }
            }
        }
    }
    core_dict_free(&reported);

    return ok;
}

static void taint_failures(ty_solver_t *solver,
                           core_list_t(n00bb_ir_func_ptr_t) *functions,
                           core_dict_t *tainted)
{
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        const ty_failure_t *failure = ty_failure_at(solver, i);

        if (failure->why.at && !literal_failure(failure)) {
            (void)taint(tainted, failure->why.at->dest);
        }
    }
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];

        for (uint32_t b = 0;
             ir_annot_get(&function->annots, N00BB_FAILED_KEY)
             && b < function->n_blocks;
             b++) {
            for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
                ir_instr_t *in = function->blocks[b]->instrs[i];

                if (in->op == IR_OP_UNDEF) {
                    (void)taint(tainted, in->dest);
                }
            }
        }
    }
    for (bool changed = core_dict_len(tainted) > 0; changed;) {
        changed = false;
        for (size_t f = 0; f < functions->len; f++) {
            changed = spread_taint(tainted, functions->data[f]) || changed;
        }
    }
}

static bool downstream_failure(ty_solver_t *solver, core_dict_t *tainted,
                               const ty_failure_t *failure)
{
    const ir_instr_t *at = failure->why.at;

    if (!at) {
        return false;
    }
    if (!literal_failure(failure)) {
        return reads_tainted(tainted, at);
    }
    if (meets_taint(tainted, at)) {
        return true;
    }
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        const ty_failure_t *other = ty_failure_at(solver, i);

        if (other != failure && other->why.at && !literal_failure(other)) {
            for (uint32_t o = 0; o < other->why.at->n_operands; o++) {
                if (ir_operand_referenced_value(&other->why.at->operands[o])
                    == at->dest) {
                    return true;
                }
            }
        }
    }

    return false;
}

typedef struct {
    bool inspected;
    bool escapes;
} outcome_uses_t;

static bool outcome_inspection(ir_instr_t *in, uint32_t operand, void *state)
{
    (void)operand;
    outcome_uses_t *uses = state;
    uses->inspected = true;
    uses->escapes = in->op != IR_OP_VARIANT_TAG
        && in->op != IR_OP_VARIANT_PAYLOAD;
    return !uses->escapes;
}

// The failure type a direct call's callee settled on.
static th_type *callee_failure(const ir_instr_t *call)
{
    const ir_func_t *callee = n00bb_direct_call_target(call);
    th_type *signature = callee && callee->type ? th_resolve(callee->type)
                                                : nullptr;
    return signature && th_kind_of(signature) == TH_FUNCTION
            && th_func_error(signature)
        ? th_resolve(th_func_error(signature))
        : nullptr;
}

// A call site's copy of its callee's signature whose failure type nothing
// bound: a call that can fail is written with `!`, which ties its outcome
// to the callee's failure, so this one can't fail. A failure type a
// parameter's type also names stays open.
static void settle_site_failures(th_ctx *types,
    const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    th_type *nil = th_name(types, CORE_STRING_STATIC("nil"));
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];
        for (uint32_t b = 0; function && b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                th_type *site = ir_instr_call_signature(block->instrs[i]);
                site = site ? th_resolve(site) : nullptr;
                th_type *failure = site && th_kind_of(site) == TH_FUNCTION
                        && th_func_error(site)
                    ? th_resolve(th_func_error(site)) : nullptr;
                if (!failure || th_kind_of(failure) != TH_VAR
                    || th_var_ref(failure) || th_var_name(failure).u8_bytes)
                    continue;
                bool shared = false;
                for (size_t p = 0; !shared && p < th_func_nparams(site);
                     p++) {
                    th_type **open = nullptr;
                    size_t count = th_free_vars(types,
                        th_func_param(site, p)->type, &open);
                    for (size_t v = 0; !shared && v < count; v++)
                        shared = th_resolve(open[v]) == failure;
                }
                if (!shared) th_var_bind(failure, nil);
            }
        }
    }
}

// Calls lowered without `!` while their callee's failure type was still
// being inferred: the callee must have turned out not to fail.
static bool check_pending_failures(
    th_ctx *types, core_translation_unit_t *const *units, size_t count,
    const core_list_t(n00bb_ir_func_ptr_t) *functions)
{
    bool ok = true;
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t *function = functions->data[f];
        for (uint32_t b = 0; function && b < function->n_blocks; b++) {
            ir_block_t *block = function->blocks[b];
            for (uint32_t i = 0; i < block->n_instrs; i++) {
                ir_instr_t *call = block->instrs[i];
                if (!ir_annot_get(&call->annots, N00BB_FAILURE_PENDING_KEY))
                    continue;
                th_type *failure = callee_failure(call);
                core_translation_unit_t *unit = unit_of(units, count, call);
                if (!unit || !failure || th_kind_of(failure) == TH_VAR
                    || th_is_nil(failure))
                    continue;
                const ir_func_t *callee = n00bb_direct_call_target(call);
                core_diagnostics_t *diagnostics =
                    core_translation_unit_diagnostics(unit);
                core_string_t name = function_name(callee);
                n00bb_report(diagnostics, unit,
                             N00B_DIAG_FAILURE_NOT_PROPAGATED,
                             n00bb_span_location(call->span),
                             N00B_TEXT_CAN_FAIL_WITH_SO_THIS,
                             N00BB_ARGS(ARG_TEXT(name),
                                        ARG_TEXT(n00bb_print_type(types,
                                                                  failure))));
                note_declared(units, count, unit, callee);
                n00bb_report_help(diagnostics,
                                  N00B_TEXT_WRITE_TO_PASS_THE_FAILURE,
                                  N00BB_ARGS(ARG_TEXT(name)));
                ok = false;
            }
        }
    }
    return ok;
}

// A method call is dispatched by its receiver's type, so whether it can fail
// is known only once typing chose the method. One written without `!` (or
// a `try`) that reaches a method that can fail does not pass the failure on.
static bool check_unpropagated_dispatches(
    th_ctx *types, core_translation_unit_t *const *units, size_t count,
    ty_solver_t *solver)
{
    bool ok = true;
    for (size_t i = 0; i < ty_dispatch_count(solver); i++) {
        const ir_instr_t *in = ty_dispatch_at(solver, i);
        th_type *instance = ty_dispatch_instance(solver, in);
        th_type *signature = instance ? th_resolve(instance) : nullptr;
        th_type *failure = signature && th_kind_of(signature) == TH_FUNCTION
                && th_func_error(signature)
            ? th_resolve(th_func_error(signature)) : nullptr;
        core_translation_unit_t *unit = unit_of(units, count, in);
        if (!unit || !in->dest || !failure || th_kind_of(failure) == TH_VAR
            || !in->n_operands || in->operands[0].kind != IR_OPD_NAME)
            continue;
        core_diagnostics_t *diagnostics =
            core_translation_unit_diagnostics(unit);
        core_string_t name = in->operands[0].name;
        bool shaped = ir_type_is_call_outcome(in->dest->type);
        // A `!` on a method that cannot fail has nothing to pass on. The
        // compiler's own calls (a `with` hook's) may reach such a method.
        if (shaped && th_is_nil(failure)
            && !ir_annot_get((ir_annots_t *)&in->annots,
                             N00BB_MAY_FAIL_KEY)) {
            n00bb_report(diagnostics, unit, N00B_DIAG_PROPAGATE_INFALLIBLE,
                         n00bb_span_location(in->span),
                         N00B_TEXT_CANNOT_FAIL_SO_THIS_HAS,
                         N00BB_ARGS(ARG_TEXT(name)));
            n00bb_report_help(diagnostics, N00B_TEXT_REMOVE_THE,
                              N00BB_NO_ARGS);
            ok = false;
            continue;
        }
        if (shaped || th_is_nil(failure)) continue;
        n00bb_report(diagnostics, unit, N00B_DIAG_FAILURE_NOT_PROPAGATED,
                     n00bb_span_location(in->span),
                     N00B_TEXT_CAN_FAIL_WITH_SO_THIS,
                     N00BB_ARGS(ARG_TEXT(name),
                                ARG_TEXT(n00bb_print_type(types, failure))));
        n00bb_report_help(diagnostics, N00B_TEXT_WRITE_TO_PASS_THE_FAILURE,
                          N00BB_ARGS(ARG_TEXT(name)));
        ok = false;
    }
    return ok;
}

// A call written with `!` whose callee's inferred failure type turned out
// to be `nil`: there is nothing for the `!` to pass along.
static bool report_infallible_bang(core_translation_unit_t *const *units,
                                   size_t count,
                                   core_translation_unit_t *unit,
                                   const ir_instr_t *call)
{
    th_type *failure = callee_failure(call);
    const ir_func_t *callee = n00bb_direct_call_target(call);
    if (!failure || !th_is_nil(failure) || !callee) return false;
    core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(unit);
    core_string_t name = function_name(callee);
    n00bb_report(diagnostics, unit, N00B_DIAG_PROPAGATE_INFALLIBLE,
                 n00bb_span_location(call->span),
                 N00B_TEXT_CANNOT_FAIL_SO_THIS_HAS,
                 N00BB_ARGS(ARG_TEXT(name)));
    note_declared(units, count, unit, callee);
    n00bb_report_help(diagnostics, N00B_TEXT_REMOVE_THE, N00BB_NO_ARGS);
    return true;
}

static bool check_unpropagated_outcomes(
    th_ctx *types, core_translation_unit_t *const *units, size_t count,
    const core_list_t(ty_outcome_call_t) *calls, core_dict_t *tainted)
{
    bool ok = true;
    for (size_t i = 0; i < calls->len; i++) {
        ir_instr_t              *call    = calls->data[i];
        core_translation_unit_t *unit    = unit_of(units, count, call);
        outcome_uses_t           uses    = {0};
        if (!unit || !call->dest || !call->block || !call->block->owner)
            continue;
        if (report_infallible_bang(units, count, unit, call)) {
            ok = false;
            continue;
        }
        ir_value_visit_uses(call->block->owner, call->dest,
                            outcome_inspection, &uses);
        if ((uses.inspected && !uses.escapes)
            || !report_unpropagated(types, units, count, unit, call->dest,
                                    call->dest->type))
            continue;
        ok = false;
        for (bool changed = taint(tainted, call->dest); changed;)
            changed = spread_taint(tainted, call->block->owner);
    }
    return ok;
}

typedef struct {
    ir_func_t *function;
    uint64_t   statement;
} statement_cut_t;
core_list_decl(statement_cut_t);

static const ir_annot_t *statement_tag(const ir_instr_t *at)
{
    const ir_annot_t *tag = ir_func_is_initializer(at->block->owner)
        ? ir_annot_get((ir_annots_t *)&at->block->annots, N00BB_STATEMENT_KEY)
        : nullptr;

    return tag && tag->kind == IR_ANNOT_INT ? tag : nullptr;
}

static void add_cut(core_list_t(statement_cut_t) *cuts, ir_func_t *function,
                    uint64_t statement)
{
    for (size_t i = 0; i < cuts->len; i++) {
        if (cuts->data[i].function == function
            && cuts->data[i].statement == statement) {
            return;
        }
    }
    core_list_push(*cuts, ((statement_cut_t){function, statement}));
}

static core_translation_unit_t *function_unit(core_translation_unit_t *const *units,
                                              size_t count,
                                              const ir_func_t *function)
{
    return unit_of_module(units, count, function->owner);
}

static bool block_has_error(core_diagnostics_t *diagnostics,
                            const ir_block_t *block)
{
    for (size_t d = 0; d < core_diagnostics_count(diagnostics); d++) {
        const core_diagnostic_t *item = core_diagnostics_at(diagnostics, d);

        if ((item->severity != CORE_DIAG_ERROR
             && item->severity != CORE_DIAG_FATAL)
            || !item->location.has_location) {
            continue;
        }
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            if (ir_span_known(block->instrs[i]->span)
                && span_contains(block->instrs[i]->span, item->location)) {
                return true;
            }
        }
    }

    return false;
}

static void cut_erroneous_statements(core_translation_unit_t *const *units,
                                     size_t count,
                                     core_list_t(n00bb_ir_func_ptr_t) *functions,
                                     core_list_t(statement_cut_t) *cuts)
{
    for (size_t f = 0; f < functions->len; f++) {
        ir_func_t               *function = functions->data[f];
        core_translation_unit_t *unit     = ir_func_is_initializer(function)
                                                ? function_unit(units, count,
                                                                function)
                                                : nullptr;
        core_diagnostics_t      *diagnostics =
            unit ? core_translation_unit_diagnostics(unit) : nullptr;

        for (uint32_t b = 0; diagnostics && b < function->n_blocks; b++) {
            const ir_annot_t *tag = ir_annot_get(
                &function->blocks[b]->annots, N00BB_STATEMENT_KEY);

            if (tag && tag->kind == IR_ANNOT_INT
                && block_has_error(diagnostics, function->blocks[b])) {
                add_cut(cuts, function, tag->i.mag);
            }
        }
    }
}

static bool cut_placeholders(ir_func_t *function,
                             core_list_t(statement_cut_t) *cuts)
{
    if (!ir_func_is_initializer(function)) {
        return false;
    }
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in  = function->blocks[b]->instrs[i];
            const ir_annot_t *tag = in->op == IR_OP_UNDEF ? statement_tag(in)
                                                          : nullptr;

            if (in->op == IR_OP_UNDEF && !tag) {
                return false;
            }
            if (tag) {
                add_cut(cuts, function, tag->i.mag);
            }
        }
    }

    return true;
}

static bool collect_failed_functions(ty_solver_t *solver,
                                     core_translation_unit_t *const *units,
                                     size_t count,
                                     core_list_t(n00bb_ir_func_ptr_t) *functions,
                                     core_dict_t *failed, bool *cut)
{
    bool contained = true;
    core_list_t(statement_cut_t) cuts = core_list_new(statement_cut_t);

    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        const ir_instr_t *at = ty_failure_at(solver, i)->why.at;

        if (!at || !at->block) {
            contained = false;
            continue;
        }
        const ir_annot_t *tag = statement_tag(at);

        if (tag) {
            add_cut(&cuts, at->block->owner, tag->i.mag);
            continue;
        }
        core_dict_put(failed, at->block->owner, (void *)1);
    }
    for (size_t i = 0; i < functions->len; i++) {
        if (ir_annot_get(&functions->data[i]->annots, N00BB_FAILED_KEY)
            && !cut_placeholders(functions->data[i], &cuts)) {
            core_dict_put(failed, functions->data[i], (void *)1);
        }
    }
    cut_erroneous_statements(units, count, functions, &cuts);
    for (size_t i = 0; i < cuts.len; i++) {
        ir_func_t               *function = cuts.data[i].function;
        core_translation_unit_t *unit     = function_unit(units, count,
                                                          function);

        if (core_dict_contains(failed, function)) {
            continue;
        }
        if (unit && n00bb_cut_tagged_statement(tu_state(unit), function,
                                               cuts.data[i].statement)) {
            *cut = true;
            continue;
        }
        core_dict_put(failed, function, (void *)1);
    }
    core_list_free(cuts);

    return contained;
}

static bool in_failed_function(core_dict_t *failed, const ir_instr_t *in)
{
    return !in || !in->block || core_dict_contains(failed, in->block->owner);
}

static void discard_failed_functions(n00bb_link_analysis_t *analysis,
                                     core_translation_unit_t *const *units,
                                     size_t count,
                                     core_list_t(n00bb_ir_func_ptr_t) *functions,
                                     core_dict_t *failed)
{
    size_t kept = 0;

    for (size_t i = 0; i < functions->len; i++) {
        ir_func_t *function = functions->data[i];

        if (!core_dict_contains(failed, function)) {
            functions->data[kept++] = function;
            continue;
        }
        for (size_t u = 0; u < count; u++) {
            n00bb_tu_state_t *state = tu_state(units[u]);

            if (state->ir_module != function->owner) {
                continue;
            }
            if (ir_func_is_initializer(function)) {
                state->initializer_excluded = true;
            }
            (void)exclude_function(analysis, units[u], function);
        }
    }
    functions->len = kept;
}

bool n00bb_infer_types(n00bb_link_analysis_t *analysis,
                       core_translation_unit_t *const *units, size_t count,
                       n00bb_typing_t *typing)
{
    th_ctx *types = ir_ctx_types(analysis->ir_context);

    core_list_t(n00bb_ir_func_ptr_t) functions =
        core_list_new(n00bb_ir_func_ptr_t);
    core_dict_t defaults;

    core_dict_init(&defaults, nullptr, nullptr);
    for (size_t u = 0; u < count; u++) {
        n00bb_tu_state_t *state = tu_state(units[u]);

        for (size_t i = 0; i < state->result_defaults.len; i++) {
            ir_instr_t *init = state->result_defaults.data[i];

            if (init->block) {
                core_dict_put(&defaults, init->block->owner, init);
            }
        }
    }
    for (size_t u = 0; u < count; u++) {
        ir_module_t *module = tu_state(units[u])->ir_module;

        for (size_t i = 0; module && i < ir_func_count(module); i++) {
            ir_func_t *f = ir_func_at(module, i);

            if (f->n_blocks) {
                prepare_function(tu_state(units[u]), f,
                                 core_dict_get(&defaults, f, nullptr));
                core_list_push(functions, f);
            }
        }
    }
    core_dict_free(&defaults);

    core_datalog_engine_t *logic    = core_datalog_engine_new();
    ty_solver_t          *solver   = ty_solver_new(types, logic);
    ir_group_pdg_t       *group    = ir_group_pdg_of(functions.data,
                                                     functions.len, nullptr);
    ty_coverage_t         coverage = {
        .outcome_calls = core_list_new(ty_outcome_call_t)};

    typing->dispatch = dispatch_index_new(analysis);
    ty_solver_set_methods(solver, method_signature, typing->dispatch);
    ty_solver_set_overloads(solver, overload_choice, typing->dispatch);
    ty_solver_set_requirements(solver, dispatch_requirements,
                               typing->dispatch);
    ty_solver_set_fields(solver, field_signature, typing->dispatch);
    ty_solver_set_param_names(solver, parameter_name, typing->dispatch);
    ty_solver_set_constant_index(solver, constant_index, typing->dispatch);
    ty_solver_set_coercion(solver, coerces, typing->dispatch);
    ty_solver_set_omittable(solver, union_arm, typing->dispatch);
    declare_bounds(solver, typing->dispatch);

    ty_declare_trait(solver, CORE_STRING_STATIC("Some_Integer"), nullptr, 0);
    for (size_t i = 0; i < sizeof(integer_names) / sizeof(*integer_names);
         i++) {
        ty_declare_impl(solver, th_name(types, integer_names[i]),
                        CORE_STRING_STATIC("Some_Integer"));
    }
    for (uint64_t width = 1; width <= 128; width++) {
        ty_declare_impl(solver, th_bitint(types, width, false),
                        CORE_STRING_STATIC("Some_Integer"));
        ty_declare_impl(solver, th_bitint(types, width, true),
                        CORE_STRING_STATIC("Some_Integer"));
    }
    ty_declare_trait(solver, CORE_STRING_STATIC("Some_Float"), nullptr, 0);
    for (size_t i = 0; i < sizeof(float_names) / sizeof(*float_names); i++) {
        ty_declare_impl(solver, th_name(types, float_names[i]),
                        CORE_STRING_STATIC("Some_Float"));
    }
    declare_number_contracts(solver, types, CORE_STRING_STATIC("Some_Integer"),
                             true);
    declare_number_contracts(solver, types, CORE_STRING_STATIC("Some_Float"),
                             false);
    ty_declare_default(solver, CORE_STRING_STATIC("Some_Float"),
                       th_name(types, CORE_STRING_STATIC("f64")));
    ty_declare_trait(solver, CORE_STRING_STATIC("String_Syntax"), nullptr, 0);
    ty_declare_impl(solver, th_name(types, CORE_STRING_STATIC("string")),
                    CORE_STRING_STATIC("String_Syntax"));
    ty_declare_impl(solver,
                    th_array(types, th_name(types, CORE_STRING_STATIC("char"))),
                    CORE_STRING_STATIC("String_Syntax"));
    ty_declare_impl(solver,
                    th_array(types, th_name(types, CORE_STRING_STATIC("byte"))),
                    CORE_STRING_STATIC("String_Syntax"));
    ty_declare_default(solver, CORE_STRING_STATIC("String_Syntax"),
                       th_name(types, CORE_STRING_STATIC("string")));
    th_arg item = {.kind = TH_ARG_TYPE, .type = th_var(types)};

    ty_declare_default(solver, CORE_STRING_STATIC("Sequence_Syntax"),
                       th_apply(types,
                                CORE_STRING_STATIC("builtin.containers.list.List"),
                                &item, 1));
    th_arg entry[] = {
        {.kind = TH_ARG_TYPE, .type = th_var(types)},
        {.kind = TH_ARG_TYPE, .type = th_var(types)},
    };

    ty_declare_default(solver, CORE_STRING_STATIC("Dict_Syntax"),
                       th_apply(types,
                                CORE_STRING_STATIC("builtin.containers.dict.Dict"),
                                entry, 2));
    // Until literals are sized, an operator meeting an unbound operand
    // waits (integer_promotion). Constraining already judges some calls.
    typing->dispatch->sizing_pending = true;
    ty_constrain_group(solver, group, &coverage);
    ty_solve(solver);
    bool bitwise_negative = false;

    if (materialize_integers(solver, types, typing->dispatch, &functions,
                             units, count, &bitwise_negative)) {
        ir_group_pdg_free(group);
        group = ir_group_pdg_of(functions.data, functions.len, nullptr);
    }
    typing->dispatch->sizing_pending = false;
    ty_solve(solver);

    bool switched = switches_fit(typing->dispatch, units, count, &functions);
    bool arities  = check_dispatch_arities(types, typing->dispatch, units,
                                           count, solver);

    arities = check_overload_sites(types, typing->dispatch, units, count,
                                   solver)
           && arities;
    bool writable = check_write_sites(units, count);
    bool visible  = check_field_privacy(types, typing->dispatch->analysis,
                                        units, count);

    bool fits     = constants_fit(types, units, count, &functions);
    bool finalize = finalizers_fit(types, units, count, &functions);
    bool patterns = patterns_fit(types, units, count, &functions);
    bool ok       = ty_failure_count(solver) == 0 && arities && writable
           && visible && fits && finalize && switched && !bitwise_negative
           && patterns;

    for (size_t i = 0; i < ty_dispatch_count(solver); i++) {
        ir_instr_t *in       = (ir_instr_t *)ty_dispatch_at(solver, i);
        th_type    *instance = ty_dispatch_instance(solver, in);

        if (ok || instance) {
            ir_instr_set_call_signature(in, instance);
        }
    }
    settle_site_failures(types, &functions);

    core_dict_t reported;
    core_dict_t tainted;

    core_dict_init(&reported, nullptr, nullptr);
    core_dict_init(&tainted, nullptr, nullptr);
    taint_failures(solver, &functions, &tainted);
    ok = check_unpropagated_outcomes(types, units, count,
                                     &coverage.outcome_calls, &tainted)
        && ok;
    ok = check_pending_failures(types, units, count, &functions) && ok;
    ok = check_unpropagated_dispatches(types, units, count, solver) && ok;
    // A call whose argument's method doesn't fit a `where` constraint gets
    // that one error; the mismatches it causes there are not reported.
    core_dict_t bound_sites;
    core_dict_init(&bound_sites, nullptr, nullptr);
    // A call through a bound whose types conflict with an earlier call's
    // gets one error, from its first mismatch.
    core_dict_t parameter_sites;
    core_dict_init(&parameter_sites, nullptr, nullptr);
    // A type that doesn't implement a bound's trait at all is told that,
    // not how each of the trait's methods misses.
    core_dict_t implements_sites;
    core_dict_init(&implements_sites, nullptr, nullptr);
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        const ty_failure_t *failure = ty_failure_at(solver, i);
        if (failure->kind == TY_FAIL_PREDICATE && failure->why.at
            && failure->pred.kind == TY_PRED_IMPLEMENTS) {
            core_dict_put(&implements_sites, (void *)failure->why.at,
                          (void *)1);
        }
    }
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        const ty_failure_t *failure = ty_failure_at(solver, i);
        th_type            *receiver = nullptr;
        if (bound_method_failure(failure)
            && !core_dict_contains(&implements_sites,
                                   (void *)failure->why.at)) {
            core_dict_put(&bound_sites, (void *)failure->why.at, (void *)1);
        }
        else if (failure->expected && failure->actual
                 && bound_trait_of_call(typing->dispatch, failure->why.at,
                                        &receiver).data
                 && !core_dict_contains(&parameter_sites,
                                        (void *)failure->why.at)) {
            core_dict_put(&parameter_sites, (void *)failure->why.at,
                          (void *)failure);
        }
    }
    for (size_t i = 0; i < ty_failure_count(solver); i++) {
        const ty_failure_t      *failure = ty_failure_at(solver, i);
        core_translation_unit_t *unit    = unit_of(units, count,
                                                   failure->why.at);

        if (!bound_method_failure(failure) && failure->why.at
            && core_dict_contains(&bound_sites, (void *)failure->why.at)) {
            continue;
        }
        if (bound_method_failure(failure)
            && core_dict_contains(&implements_sites,
                                  (void *)failure->why.at)) {
            continue;
        }
        bool parameter = false;
        const ty_failure_t *first = failure->why.at
            ? core_dict_get(&parameter_sites, (void *)failure->why.at,
                            &parameter)
            : nullptr;
        if (parameter) {
            if (first == failure && unit) {
                report_trait_parameter(types, typing->dispatch, unit,
                                       failure);
            }
            continue;
        }

        const ir_instr_t        *test    = case_test_of(failure->why.at);

        if (!test && failure->why.at
            && ir_annot_get((ir_annots_t *)&failure->why.at->annots,
                            N00BB_CONDITION_KEY)) {
            test = failure->why.at;
        }
        if (!test && failure->why.at
            && failure->why.at->op == IR_OP_EXTRACT_INDEX
            && failure->why.at->n_operands) {
            test = (const ir_instr_t *)ir_operand_value(
                &failure->why.at->operands[0]);
        }
        if (!test) {
            test = failed_container(failure);
        }
        if (!test) {
            test = failed_tuple(failure);
        }
        if (!unit || (test && core_dict_contains(&reported, (void *)test))
            || poisoned(analysis, failure->expected)
            || poisoned(analysis, failure->actual)
            || downstream_failure(solver, &tainted, failure)) {
            continue;
        }
        if (test) {
            core_dict_put(&reported, (void *)test, (void *)1);
        }
        report_failure(types, typing->dispatch, units, count, unit, failure);

        const ir_instr_t *site = merge_site(failure);

        for (bool changed = site && taint(&tainted, site->dest); changed;) {
            changed = spread_taint(&tainted, site->block->owner);
        }
    }
    core_dict_free(&tainted);
    core_dict_free(&reported);
    core_dict_free(&bound_sites);
    core_dict_free(&parameter_sites);
    core_dict_free(&implements_sites);
    ok = check_empty_lists(types, units, count, &functions) && ok;
    ok = check_untyped(types, units, count) && ok;
    for (size_t u = 0; u < count; u++) {
        core_dict_foreach(&tu_state(units[u])->dispatch_calls, item)
            n00bb_written_call_free(item->value);
        core_dict_free(&tu_state(units[u])->dispatch_calls);
        core_dict_init(&tu_state(units[u])->dispatch_calls, nullptr, nullptr);
    }

    core_dict_t failed;

    core_dict_init(&failed, nullptr, nullptr);
    bool cut       = false;
    bool contained = collect_failed_functions(solver, units, count,
                                              &functions, &failed, &cut);

    typing->recovered = !ok && contained;
    typing->conversions   = core_list_new(n00bb_ir_instr_ptr_t);
    typing->outcome_calls = core_list_new(n00bb_ir_instr_ptr_t);
    for (size_t i = 0; i < coverage.outcome_calls.len; i++) {
        if (!in_failed_function(&failed, coverage.outcome_calls.data[i])) {
            core_list_push(typing->outcome_calls,
                           coverage.outcome_calls.data[i]);
        }
    }
    core_list_free(coverage.outcome_calls);
    for (size_t i = 0; i < ty_conversion_count(solver); i++) {
        ir_instr_t *in = (ir_instr_t *)ty_conversion_at(solver, i);

        if (!in_failed_function(&failed, in)) {
            core_list_push(typing->conversions, in);
        }
    }
    if (contained && core_dict_len(&failed)) {
        discard_failed_functions(analysis, units, count, &functions, &failed);
    }
    if (contained && (cut || core_dict_len(&failed))) {
        ir_group_pdg_free(group);
        group = ir_group_pdg_of(functions.data, functions.len, nullptr);
    }
    core_dict_free(&failed);
    typing->group = group;
    if (ok || typing->recovered) {
        settle_symbol_signatures(typing->dispatch);
    }
    ty_solver_free(solver);
    core_datalog_engine_free(logic);
    core_list_free(functions);

    return ok;
}
